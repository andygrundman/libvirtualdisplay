#ifndef NOMINMAX
#  define NOMINMAX
#endif

#include "virtual_display/driver/ioctl_dispatcher.h"
#include "virtual_display/driver/hdr_capabilities.h"
#include "virtual_display/driver/windows_control_protocol.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <wdf.h>
#include <IddCx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::uint32_t kMaxPermanentDisplays = 4;
  constexpr std::uint32_t kMaxTemporaryDisplays = 8;
  constexpr std::uint64_t kPermanentDisplayIdBase = 0x7000000000000000ull;
  constexpr wchar_t kTemporaryDisplayProfilesKey[] = L"SOFTWARE\\Sunshine\\VirtualDisplayDriver\\TemporaryDisplays";
  const GUID kControlInterfaceGuid = vdd::to_windows_guid(vdd::kDeviceInterfaceGuid);

  class IddCxBackend;
  class SwapChainProcessor;

  struct AdapterContext {
    IddCxBackend *backend {};
  };

  struct MonitorContext {
    IddCxBackend *backend {};
    std::uint64_t display_id {};
  };

  struct DeviceContext {
    class DeviceState *state {};
  };

  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(AdapterContext, GetAdapterContext);
  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(MonitorContext, GetMonitorContext);
  WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, GetDeviceContext);

  struct MonitorRecord {
    vdd::DisplayDescriptor descriptor {};
    IDDCX_MONITOR monitor {};
    std::unique_ptr<SwapChainProcessor> swapchain_processor {};
    std::vector<std::unique_ptr<SwapChainProcessor>> retired_swapchain_processors {};
    bool permanent {};
    bool departing {};
  };

  struct ModeShape {
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    // Use standard 1080p CVT/CTA-ish totals for fallback paths. Requested
    // per-monitor modes are normalized to active-only timings below to match
    // the known-good SudoVDA IddCx mode contract for high refresh displays.
    std::uint32_t total_width {2200};
    std::uint32_t total_height {1125};
    std::uint64_t pixel_rate {148'500'000ull};
    std::uint32_t refresh_rate_millihz {60'000};
  };

  struct ModeSpec {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t refresh_rate_millihz;
  };

  constexpr std::array<ModeSpec, 47> kDefaultModes {{
    {800, 600, 30'000},
    {800, 600, 59'940},
    {800, 600, 60'000},
    {800, 600, 72'000},
    {800, 600, 90'000},
    {800, 600, 120'000},
    {800, 600, 144'000},
    {800, 600, 240'000},
    {1280, 720, 30'000},
    {1280, 720, 59'940},
    {1280, 720, 60'000},
    {1280, 720, 72'000},
    {1280, 720, 90'000},
    {1280, 720, 120'000},
    {1280, 720, 144'000},
    {1366, 768, 30'000},
    {1366, 768, 59'940},
    {1366, 768, 60'000},
    {1366, 768, 72'000},
    {1366, 768, 90'000},
    {1366, 768, 120'000},
    {1366, 768, 144'000},
    {1366, 768, 240'000},
    {1920, 1080, 30'000},
    {1920, 1080, 59'940},
    {1920, 1080, 60'000},
    {1920, 1080, 72'000},
    {1920, 1080, 90'000},
    {1920, 1080, 120'000},
    {1920, 1080, 144'000},
    {1920, 1080, 240'000},
    {2560, 1440, 30'000},
    {2560, 1440, 59'940},
    {2560, 1440, 60'000},
    {2560, 1440, 72'000},
    {2560, 1440, 90'000},
    {2560, 1440, 120'000},
    {2560, 1440, 144'000},
    {2560, 1440, 240'000},
    {3840, 2160, 30'000},
    {3840, 2160, 59'940},
    {3840, 2160, 60'000},
    {3840, 2160, 72'000},
    {3840, 2160, 90'000},
    {3840, 2160, 120'000},
    {3840, 2160, 144'000},
    {3840, 2160, 240'000}
  }};

  std::uint32_t clamp_u32(const std::uint64_t value) {
    return static_cast<std::uint32_t>(
      (std::min<std::uint64_t>)(value, (std::numeric_limits<std::uint32_t>::max)())
    );
  }

  ModeShape active_mode_shape(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_rate_millihz
  ) {
    return {
      width,
      height,
      width,
      height,
      static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height) *
        static_cast<std::uint64_t>((std::max)(refresh_rate_millihz, 1u)) /
        1000ull,
      refresh_rate_millihz
    };
  }

  ModeShape mode_shape_from_spec(const ModeSpec &spec) {
    return active_mode_shape(spec.width, spec.height, spec.refresh_rate_millihz);
  }

  std::pair<std::vector<ModeShape>, std::uint32_t> build_mode_shapes(
    const std::optional<ModeShape> &preferred
  ) {
    std::vector<ModeShape> modes;
    modes.reserve(kDefaultModes.size() + (preferred ? 1u : 0u));
    std::uint32_t preferred_index = 1;

    for (const auto &spec: kDefaultModes) {
      modes.push_back(mode_shape_from_spec(spec));
    }

    if (preferred) {
      preferred_index = static_cast<std::uint32_t>(modes.size());
      modes.push_back(*preferred);
    }

    return {std::move(modes), preferred_index};
  }

  std::wstring temporary_profile_key_name(const std::uint64_t display_id) {
    wchar_t key_name[32] {};
    swprintf_s(key_name, L"%016llX", static_cast<unsigned long long>(display_id));
    return key_name;
  }

  std::map<std::uint64_t, std::uint32_t> load_temporary_connector_reservations() {
    std::map<std::uint64_t, std::uint32_t> reservations;
    HKEY profiles_key {};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTemporaryDisplayProfilesKey, 0, KEY_READ, &profiles_key) != ERROR_SUCCESS) {
      return reservations;
    }

    for (DWORD index = 0;; ++index) {
      wchar_t key_name[256] {};
      DWORD key_name_length = static_cast<DWORD>(std::size(key_name));
      const auto enum_status = RegEnumKeyExW(
        profiles_key,
        index,
        key_name,
        &key_name_length,
        nullptr,
        nullptr,
        nullptr,
        nullptr
      );
      if (enum_status == ERROR_NO_MORE_ITEMS) {
        break;
      }
      if (enum_status != ERROR_SUCCESS) {
        continue;
      }

      HKEY profile_key {};
      if (RegOpenKeyExW(profiles_key, key_name, 0, KEY_READ, &profile_key) != ERROR_SUCCESS) {
        continue;
      }

      std::uint64_t display_id {};
      std::uint32_t connector_index {};
      DWORD display_id_size = sizeof(display_id);
      DWORD connector_index_size = sizeof(connector_index);
      if (RegGetValueW(profile_key, nullptr, L"DisplayId", RRF_RT_REG_QWORD, nullptr, &display_id, &display_id_size) == ERROR_SUCCESS &&
          RegGetValueW(profile_key, nullptr, L"ConnectorIndex", RRF_RT_REG_DWORD, nullptr, &connector_index, &connector_index_size) == ERROR_SUCCESS &&
          display_id != 0 &&
          connector_index < kMaxPermanentDisplays + kMaxTemporaryDisplays) {
        reservations.emplace(display_id, connector_index);
      }

      RegCloseKey(profile_key);
    }

    RegCloseKey(profiles_key);
    return reservations;
  }

  template <typename T>
  void write_registry_value_if_success(
    HKEY key,
    const wchar_t *value_name,
    const DWORD type,
    const T &value,
    LSTATUS &status
  ) {
    if (status == ERROR_SUCCESS) {
      status = RegSetValueExW(key, value_name, 0, type, reinterpret_cast<const BYTE *>(&value), sizeof(value));
    }
  }

  vdd::BackendError save_temporary_display_profile(const vdd::DisplayDescriptor &descriptor) {
    HKEY profiles_key {};
    LSTATUS status = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE,
      kTemporaryDisplayProfilesKey,
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      KEY_WRITE,
      nullptr,
      &profiles_key,
      nullptr
    );
    if (status != ERROR_SUCCESS) {
      return vdd::BackendError::None;
    }

    HKEY profile_key {};
    const auto key_name = temporary_profile_key_name(descriptor.display_id);
    status = RegCreateKeyExW(
      profiles_key,
      key_name.c_str(),
      0,
      nullptr,
      REG_OPTION_NON_VOLATILE,
      KEY_WRITE,
      nullptr,
      &profile_key,
      nullptr
    );
    RegCloseKey(profiles_key);
    if (status != ERROR_SUCCESS) {
      return vdd::BackendError::None;
    }

    const DWORD connector_index = descriptor.connector_index;
    const GUID container_id = vdd::to_windows_guid(descriptor.container_id);
    const DWORD edid_product_code = vdd::read_product_code(descriptor.edid);
    const DWORD edid_serial_number = vdd::read_serial_number(descriptor.edid);

    write_registry_value_if_success(profile_key, L"DisplayId", REG_QWORD, descriptor.display_id, status);
    write_registry_value_if_success(profile_key, L"ConnectorIndex", REG_DWORD, connector_index, status);
    write_registry_value_if_success(profile_key, L"ContainerId", REG_BINARY, container_id, status);
    write_registry_value_if_success(profile_key, L"EdidProductCode", REG_DWORD, edid_product_code, status);
    write_registry_value_if_success(profile_key, L"EdidSerialNumber", REG_DWORD, edid_serial_number, status);

    RegCloseKey(profile_key);
    return vdd::BackendError::None;
  }

  vdd::DisplayDescriptor make_permanent_descriptor(const std::uint32_t index) {
    const auto display_id = kPermanentDisplayIdBase | static_cast<std::uint64_t>(index + 1);

    vdd::EdidOptions options {};
    options.manufacturer_id = vdd::kSunshineDriverManufacturerId;
    options.product_code = static_cast<std::uint16_t>(0x4000u | (index & 0x0fffu));
    options.serial_number = vdd::serial_number_from_display_id(display_id);
    options.width = 1920;
    options.height = 1080;
    options.refresh_rate_millihz = 60'000;
    options.monitor_name = "Sunshine Display";
    options.hdr_supported = true;

    vdd::DisplayDescriptor descriptor {};
    descriptor.display_id = display_id;
    descriptor.container_id = vdd::container_guid_from_display_id(display_id);
    descriptor.connector_index = index;
    descriptor.width = options.width;
    descriptor.height = options.height;
    descriptor.refresh_rate_millihz = options.refresh_rate_millihz;
    descriptor.edid = vdd::create_edid(options);
    return descriptor;
  }

  ModeShape mode_shape_from_description(const IDDCX_MONITOR_DESCRIPTION &description) {
    if (description.Type != IDDCX_MONITOR_DESCRIPTION_TYPE_EDID ||
        !description.pData ||
        description.DataSize < vdd::kEdidSize) {
      return {};
    }

    const auto *edid_data = static_cast<const std::byte *>(description.pData);
    const std::span<const std::byte, vdd::kEdidSize> edid {edid_data, vdd::kEdidSize};
    const auto timing = vdd::read_preferred_timing(edid);
    if (timing.horizontal_active == 0 || timing.vertical_active == 0 || timing.pixel_clock_10khz == 0) {
      return {};
    }

    ModeShape shape {
      timing.horizontal_active,
      timing.vertical_active,
      static_cast<std::uint32_t>(timing.horizontal_active + timing.horizontal_blanking),
      static_cast<std::uint32_t>(timing.vertical_active + timing.vertical_blanking),
      static_cast<std::uint64_t>(timing.pixel_clock_10khz) * 10'000ull,
      60'000
    };

    const auto total_pixels =
      static_cast<std::uint64_t>((std::max)(shape.total_width, 1u)) *
      static_cast<std::uint64_t>((std::max)(shape.total_height, 1u));
    const auto derived_refresh_millihz =
      total_pixels == 0 ? 0 : (shape.pixel_rate * 1000ull) / total_pixels;
    shape.refresh_rate_millihz = clamp_u32(derived_refresh_millihz);

    return shape;
  }

  ModeShape mode_shape_from_descriptor(const vdd::DisplayDescriptor &descriptor) {
    if (descriptor.width == 0 || descriptor.height == 0 || descriptor.refresh_rate_millihz == 0) {
      return {};
    }

    return active_mode_shape(descriptor.width, descriptor.height, descriptor.refresh_rate_millihz);
  }

  std::optional<ModeShape> preferred_mode_shape_from_description(const IDDCX_MONITOR_DESCRIPTION &description) {
    const auto edid_shape = mode_shape_from_description(description);
    if (edid_shape.width == 0 || edid_shape.height == 0 || edid_shape.refresh_rate_millihz == 0) {
      return std::nullopt;
    }
    return edid_shape;
  }

  bool has_hdr_iddcx_ddi() {
    return IDD_IS_FIELD_AVAILABLE(IDD_CX_CLIENT_CONFIG, EvtIddCxAdapterQueryTargetInfo);
  }

  IDDCX_BITS_PER_COMPONENT supported_hdr_bits_per_component() {
    const auto capabilities = vdd::hdr_output_capabilities();
    UINT bits = 0;
    if (capabilities.output_bits.rgb_8bpc) {
      bits |= static_cast<UINT>(IDDCX_BITS_PER_COMPONENT_8);
    }
    if (capabilities.output_bits.rgb_10bpc) {
      bits |= static_cast<UINT>(IDDCX_BITS_PER_COMPONENT_10);
    }
    return static_cast<IDDCX_BITS_PER_COMPONENT>(
      bits
    );
  }

  void populate_rgb_wire_bits(IDDCX_WIRE_BITS_PER_COMPONENT &bits, const IDDCX_BITS_PER_COMPONENT rgb_bits) {
    bits = {};
    bits.Rgb = rgb_bits;
    bits.YCbCr444 = IDDCX_BITS_PER_COMPONENT_NONE;
    bits.YCbCr422 = IDDCX_BITS_PER_COMPONENT_NONE;
    bits.YCbCr420 = IDDCX_BITS_PER_COMPONENT_NONE;
  }

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_signal_info(
    const ModeShape &shape,
    const bool monitor_mode
  ) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal {};
    signal.pixelRate = shape.pixel_rate;
    signal.activeSize.cx = shape.width;
    signal.activeSize.cy = shape.height;
    signal.totalSize.cx = (std::max)(shape.total_width, shape.width);
    signal.totalSize.cy = (std::max)(shape.total_height, shape.height);
    signal.vSyncFreq.Numerator = (std::max)(shape.refresh_rate_millihz, 1u);
    signal.vSyncFreq.Denominator = 1000;
    signal.hSyncFreq.Numerator = clamp_u32(
      static_cast<std::uint64_t>(signal.vSyncFreq.Numerator) *
      static_cast<std::uint64_t>((std::max)(signal.totalSize.cy, 1u))
    );
    signal.hSyncFreq.Denominator = signal.vSyncFreq.Denominator;
    // DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY_OTHER is not accepted here; 255 is
    // the documented "not initialized" value Windows itself uses for EDID modes.
    signal.AdditionalSignalInfo.videoStandard = 255;
    signal.AdditionalSignalInfo.vSyncFreqDivider = monitor_mode ? 0 : 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return signal;
  }

  IDDCX_MONITOR_MODE make_monitor_mode(
    const ModeShape &shape,
    const IDDCX_MONITOR_MODE_ORIGIN origin
  ) {
    IDDCX_MONITOR_MODE mode {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo = make_signal_info(shape, true);
    return mode;
  }

  IDDCX_MONITOR_MODE2 make_monitor_mode2(
    const ModeShape &shape,
    const IDDCX_MONITOR_MODE_ORIGIN origin
  ) {
    IDDCX_MONITOR_MODE2 mode {};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    mode.MonitorVideoSignalInfo = make_signal_info(shape, true);
    populate_rgb_wire_bits(mode.BitsPerComponent, supported_hdr_bits_per_component());
    return mode;
  }

  IDDCX_TARGET_MODE make_target_mode(const ModeShape &shape) {
    IDDCX_TARGET_MODE mode {};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.targetVideoSignalInfo = make_signal_info(shape, false);
    return mode;
  }

  IDDCX_TARGET_MODE2 make_target_mode2(const ModeShape &shape) {
    IDDCX_TARGET_MODE2 mode {};
    mode.Size = sizeof(mode);
    mode.TargetVideoSignalInfo.targetVideoSignalInfo = make_signal_info(shape, false);
    populate_rgb_wire_bits(mode.BitsPerComponent, supported_hdr_bits_per_component());
    return mode;
  }

  NTSTATUS fill_monitor_modes(
    const IDARG_IN_PARSEMONITORDESCRIPTION *input,
    IDARG_OUT_PARSEMONITORDESCRIPTION *output
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(preferred_mode_shape_from_description(input->MonitorDescription));
    output->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    output->PreferredMonitorModeIdx = preferred_index;
    if (input->MonitorModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pMonitorModes || input->MonitorModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pMonitorModes[index] = make_monitor_mode(modes[index], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_monitor_modes2(
    const IDARG_IN_PARSEMONITORDESCRIPTION2 *input,
    IDARG_OUT_PARSEMONITORDESCRIPTION *output
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(preferred_mode_shape_from_description(input->MonitorDescription));
    output->MonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    output->PreferredMonitorModeIdx = preferred_index;
    if (input->MonitorModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pMonitorModes || input->MonitorModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pMonitorModes[index] = make_monitor_mode2(modes[index], IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_default_monitor_modes(
    const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *input,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *output
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(std::nullopt);
    output->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    output->PreferredMonitorModeIdx = preferred_index;
    if (input->DefaultMonitorModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pDefaultMonitorModes || input->DefaultMonitorModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pDefaultMonitorModes[index] = make_monitor_mode(modes[index], IDDCX_MONITOR_MODE_ORIGIN_DRIVER);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_target_modes(
    const IDARG_IN_QUERYTARGETMODES *input,
    IDARG_OUT_QUERYTARGETMODES *output,
    const ModeShape *requested_shape = nullptr
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(
      requested_shape ? std::optional<ModeShape> {*requested_shape} : std::optional<ModeShape> {mode_shape_from_description(input->MonitorDescription)}
    );
    (void) preferred_index;
    output->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (input->TargetModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pTargetModes || input->TargetModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pTargetModes[index] = make_target_mode(modes[index]);
    }

    return STATUS_SUCCESS;
  }

  NTSTATUS fill_target_modes2(
    const IDARG_IN_QUERYTARGETMODES2 *input,
    IDARG_OUT_QUERYTARGETMODES *output,
    const ModeShape *requested_shape = nullptr
  ) {
    if (!input || !output) {
      return STATUS_INVALID_PARAMETER;
    }

    const auto [modes, preferred_index] = build_mode_shapes(
      requested_shape ? std::optional<ModeShape> {*requested_shape} : std::optional<ModeShape> {mode_shape_from_description(input->MonitorDescription)}
    );
    (void) preferred_index;
    output->TargetModeBufferOutputCount = static_cast<UINT>(modes.size());
    if (input->TargetModeBufferInputCount == 0) {
      return STATUS_SUCCESS;
    }
    if (!input->pTargetModes || input->TargetModeBufferInputCount < modes.size()) {
      return STATUS_BUFFER_TOO_SMALL;
    }
    for (std::size_t index = 0; index < modes.size(); ++index) {
      input->pTargetModes[index] = make_target_mode2(modes[index]);
    }

    return STATUS_SUCCESS;
  }

  HRESULT create_dxgi_device_for_luid(
    const LUID &adapter_luid,
    Microsoft::WRL::ComPtr<ID3D11Device> &device,
    Microsoft::WRL::ComPtr<IDXGIDevice> &dxgi_device
  ) {
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      return hr;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    hr = factory->EnumAdapterByLuid(adapter_luid, IID_PPV_ARGS(&adapter));
    if (FAILED(hr)) {
      return hr;
    }

    static constexpr D3D_FEATURE_LEVEL kFeatureLevels[] {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0
    };

    D3D_FEATURE_LEVEL selected_feature_level {};
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    hr = D3D11CreateDevice(
      adapter.Get(),
      D3D_DRIVER_TYPE_UNKNOWN,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      kFeatureLevels,
      static_cast<UINT>(std::size(kFeatureLevels)),
      D3D11_SDK_VERSION,
      &device,
      &selected_feature_level,
      &context
    );
    if (FAILED(hr)) {
      return hr;
    }

    return device.As(&dxgi_device);
  }

  class SwapChainProcessor {
  public:
    SwapChainProcessor(IDDCX_SWAPCHAIN swapchain, HANDLE next_surface_available):
        swapchain_ {swapchain},
        next_surface_available_ {next_surface_available} {
    }

    ~SwapChainProcessor() {
      stop();
      delete_swapchain();
    }

    SwapChainProcessor(const SwapChainProcessor &) = delete;
    SwapChainProcessor &operator=(const SwapChainProcessor &) = delete;

    HRESULT start(const LUID &render_adapter_luid) {
      try {
        worker_ = std::thread([this, render_adapter_luid]() {
          process_frames(render_adapter_luid);
        });
      } catch (...) {
        return E_OUTOFMEMORY;
      }

      return S_OK;
    }

    void stop() {
      stop_requested_.store(true, std::memory_order_release);
      if (next_surface_available_) {
        SetEvent(next_surface_available_);
      }
      if (worker_.joinable()) {
        worker_.join();
      }
      dxgi_device_.Reset();
      device_.Reset();
    }

    void abandon_swapchain() {
      swapchain_ = nullptr;
    }

  private:
    void delete_swapchain() {
      if (swapchain_) {
        // Match the IddCx sample by closing the swapchain when processing
        // stops. Waiting until after monitor departure can leave us deleting
        // a UMDF object that IddCx has already invalidated.
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapchain_));
        swapchain_ = nullptr;
      }
    }

    void process_frames(const LUID render_adapter_luid) {
      HRESULT hr = create_dxgi_device_for_luid(render_adapter_luid, device_, dxgi_device_);
      if (FAILED(hr) || stop_requested_.load(std::memory_order_acquire)) {
        return;
      }

      IDARG_IN_SWAPCHAINSETDEVICE set_device {};
      set_device.pDevice = dxgi_device_.Get();
      // HandleNewSwapChain still owns IddCx's internal OPM cleanup while it
      // invokes AssignSwapChain. Setting the DXGI device from the worker thread
      // matches the WDK sample flow and avoids re-entering that cleanup path.
      hr = IddCxSwapChainSetDevice(swapchain_, &set_device);
      if (FAILED(hr)) {
        return;
      }

      while (!stop_requested_.load(std::memory_order_acquire)) {
        const DWORD wait_result = WaitForSingleObject(next_surface_available_, 1000);
        if (stop_requested_.load(std::memory_order_acquire)) {
          break;
        }
        if (wait_result == WAIT_TIMEOUT) {
          continue;
        }
        if (wait_result != WAIT_OBJECT_0) {
          break;
        }

        for (;;) {
          IDXGIResource *surface_ptr = nullptr;
          HRESULT acquire_result = E_FAIL;
          if (IDD_IS_FUNCTION_AVAILABLE(IddCxSwapChainReleaseAndAcquireBuffer2)) {
            IDARG_IN_RELEASEANDACQUIREBUFFER2 input {};
            input.Size = sizeof(input);
            IDARG_OUT_RELEASEANDACQUIREBUFFER2 acquired {};
            acquire_result = IddCxSwapChainReleaseAndAcquireBuffer2(swapchain_, &input, &acquired);
            surface_ptr = acquired.MetaData.pSurface;
          } else {
            IDARG_OUT_RELEASEANDACQUIREBUFFER acquired {};
            acquire_result = IddCxSwapChainReleaseAndAcquireBuffer(swapchain_, &acquired);
            surface_ptr = acquired.MetaData.pSurface;
          }
          if (acquire_result == E_PENDING) {
            break;
          }
          if (FAILED(acquire_result)) {
            return;
          }

          Microsoft::WRL::ComPtr<IDXGIResource> surface;
          surface.Attach(surface_ptr);
          // Drop the acquired surface before reporting the frame complete so
          // IddCx can reclaim the buffer during unassign/departure.
          surface.Reset();
          (void) IddCxSwapChainFinishedProcessingFrame(swapchain_);

          if (stop_requested_.load(std::memory_order_acquire)) {
            break;
          }
        }
      }

    }

    IDDCX_SWAPCHAIN swapchain_ {};
    HANDLE next_surface_available_ {};
    std::atomic<bool> stop_requested_ {false};
    std::thread worker_ {};
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device_;
  };

  class IddCxBackend: public vdd::DisplayDriverBackend {
  public:
    NTSTATUS initialize_adapter(WDFDEVICE device) {
      if (adapter_) {
        return STATUS_SUCCESS;
      }

      IDDCX_ENDPOINT_VERSION endpoint_version {};
      endpoint_version.Size = sizeof(endpoint_version);
      endpoint_version.MajorVer = 1;
      endpoint_version.MinorVer = 0;

      const auto hdr_capabilities = vdd::hdr_output_capabilities();
      IDDCX_ADAPTER_CAPS caps {};
      caps.Size = sizeof(caps);
      caps.Flags = has_hdr_iddcx_ddi() && hdr_capabilities.fp16_swapchain ?
        IDDCX_ADAPTER_FLAGS_CAN_PROCESS_FP16 :
        IDDCX_ADAPTER_FLAGS_NONE;
      caps.MaxMonitorsSupported = kMaxPermanentDisplays + kMaxTemporaryDisplays;
      caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
      // IddCx expects gamma support to be advertised when exposing high color
      // space; accepting SetGammaRamp below is enough for our software path.
      caps.EndPointDiagnostics.GammaSupport = hdr_capabilities.high_color_space ?
        IDDCX_FEATURE_IMPLEMENTATION_SOFTWARE :
        IDDCX_FEATURE_IMPLEMENTATION_NONE;
      caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
      caps.EndPointDiagnostics.pEndPointFriendlyName = const_cast<PWSTR>(L"Sunshine Virtual Display Adapter");
      caps.EndPointDiagnostics.pEndPointManufacturerName = const_cast<PWSTR>(L"Sunshine");
      caps.EndPointDiagnostics.pEndPointModelName = const_cast<PWSTR>(L"SunshineVirtualDisplay");
      caps.EndPointDiagnostics.pHardwareVersion = &endpoint_version;
      caps.EndPointDiagnostics.pFirmwareVersion = &endpoint_version;

      WDF_OBJECT_ATTRIBUTES adapter_attributes;
      WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&adapter_attributes, AdapterContext);

      IDARG_IN_ADAPTER_INIT adapter_init {};
      adapter_init.WdfDevice = device;
      adapter_init.pCaps = &caps;
      adapter_init.ObjectAttributes = &adapter_attributes;

      IDARG_OUT_ADAPTER_INIT adapter_out {};
      const auto status = IddCxAdapterInitAsync(&adapter_init, &adapter_out);
      if (NT_SUCCESS(status) && adapter_out.AdapterObject) {
        adapter_ = adapter_out.AdapterObject;
        auto *context = GetAdapterContext(adapter_);
        context->backend = this;
      }
      if (!NT_SUCCESS(status)) {
        return status;
      }

      return STATUS_SUCCESS;
    }

    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &descriptor) override {
      return arrive_display(descriptor, false);
    }

    vdd::BackendError reserve_temporary_display_identity(const vdd::DisplayDescriptor &descriptor) override {
      return save_temporary_display_profile(descriptor);
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id) override {
      return depart_display(display_id);
    }

    vdd::BackendError set_permanent_display_count(const std::uint32_t display_count) override {
      if (display_count > kMaxPermanentDisplays) {
        return vdd::BackendError::Failed;
      }

      std::vector<std::uint64_t> added;
      for (auto index = permanent_display_count_; index < display_count; ++index) {
        const auto descriptor = make_permanent_descriptor(index);
        const auto result = arrive_display(descriptor, true);
        if (result.error != vdd::BackendError::None) {
          for (const auto display_id: added) {
            (void) depart_display(display_id);
          }
          return vdd::BackendError::Failed;
        }
        added.push_back(descriptor.display_id);
      }

      for (auto index = permanent_display_count_; index > display_count; --index) {
        const auto display_id = make_permanent_descriptor(index - 1).display_id;
        if (depart_display(display_id) != vdd::BackendError::None) {
          return vdd::BackendError::Failed;
        }
      }

      permanent_display_count_ = display_count;
      return vdd::BackendError::None;
    }

    NTSTATUS adapter_init_finished(const IDARG_IN_ADAPTER_INIT_FINISHED *args) {
      if (!args) {
        return STATUS_INVALID_PARAMETER;
      }

      // The async callback status is the point where IddCx says monitor arrival
      // is legal. Keep DeviceAdd successful, but block display creation until then.
      adapter_ready_ = NT_SUCCESS(args->AdapterInitStatus);
      return STATUS_SUCCESS;
    }

  private:
    vdd::BackendDisplayResult arrive_display(const vdd::DisplayDescriptor &descriptor, const bool permanent) {
      IDDCX_ADAPTER adapter {};
      {
        std::lock_guard lock {mutex_};
        if (!adapter_ready_) {
          return {vdd::BackendError::Failed, {}, 0};
        }
        if (!adapter_ || monitors_.contains(descriptor.display_id)) {
          return {vdd::BackendError::Failed, {}, 0};
        }
        adapter = adapter_;
      }

      MonitorRecord record {};
      record.descriptor = descriptor;
      record.permanent = permanent;

      WDF_OBJECT_ATTRIBUTES monitor_attributes;
      WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&monitor_attributes, MonitorContext);

      IDDCX_MONITOR_INFO monitor_info {};
      monitor_info.Size = sizeof(monitor_info);
      // Use a digital sink type so Windows avoids WCG-only classification for
      // HDR, but avoid HDMI's legacy bandwidth ceiling that rejects 4K
      // high-refresh virtual modes.
      monitor_info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL;
      monitor_info.ConnectorIndex = descriptor.connector_index;
      monitor_info.MonitorContainerId = vdd::to_windows_guid(descriptor.container_id);
      monitor_info.MonitorDescription.Size = sizeof(monitor_info.MonitorDescription);
      monitor_info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
      monitor_info.MonitorDescription.DataSize = static_cast<UINT>(record.descriptor.edid.size());
      monitor_info.MonitorDescription.pData = record.descriptor.edid.data();

      IDARG_IN_MONITORCREATE create_args {};
      create_args.ObjectAttributes = &monitor_attributes;
      create_args.pMonitorInfo = &monitor_info;

      IDARG_OUT_MONITORCREATE create_out {};
      auto status = IddCxMonitorCreate(adapter, &create_args, &create_out);
      if (!NT_SUCCESS(status)) {
        return {vdd::BackendError::Failed, {}, 0};
      }

      record.monitor = create_out.MonitorObject;
      auto *monitor_context = GetMonitorContext(record.monitor);
      monitor_context->backend = this;
      monitor_context->display_id = descriptor.display_id;

      {
        std::lock_guard lock {mutex_};
        if (!adapter_ready_ || adapter_ != adapter || monitors_.contains(descriptor.display_id)) {
          status = STATUS_DEVICE_NOT_READY;
        } else {
          monitors_.emplace(descriptor.display_id, std::move(record));
          status = STATUS_SUCCESS;
        }
      }
      if (!NT_SUCCESS(status)) {
        WdfObjectDelete(create_out.MonitorObject);
        return {vdd::BackendError::Failed, {}, 0};
      }

      IDARG_OUT_MONITORARRIVAL arrival_out {};
      status = IddCxMonitorArrival(create_out.MonitorObject, &arrival_out);
      if (!NT_SUCCESS(status)) {
        {
          std::lock_guard lock {mutex_};
          monitors_.erase(descriptor.display_id);
        }
        WdfObjectDelete(create_out.MonitorObject);
        return {vdd::BackendError::Failed, {}, 0};
      }

      return {
        vdd::BackendError::None,
        vdd::from_windows_luid(arrival_out.OsAdapterLuid),
        arrival_out.OsTargetId
      };
    }

    vdd::BackendError depart_display(const std::uint64_t display_id) {
      IDDCX_MONITOR monitor_handle {};
      std::unique_ptr<SwapChainProcessor> processor_to_stop;
      std::vector<std::unique_ptr<SwapChainProcessor>> retired_processors_to_stop;
      {
        std::lock_guard lock {mutex_};
        const auto monitor = monitors_.find(display_id);
        if (monitor == monitors_.end() || monitor->second.departing) {
          return vdd::BackendError::None;
        }

        monitor->second.departing = true;
        monitor_handle = monitor->second.monitor;
      }

      // DisplayConfig can remove a just-activated path while IddCx is still
      // unwinding HandleNewSwapChain. Mark departure first, then let any racing
      // assign callback see ABANDON_SWAPCHAIN before we tear down the monitor.
      std::this_thread::sleep_for(std::chrono::milliseconds(250));

      {
        std::lock_guard lock {mutex_};
        const auto monitor = monitors_.find(display_id);
        if (monitor == monitors_.end() || monitor->second.monitor != monitor_handle) {
          return vdd::BackendError::None;
        }
        processor_to_stop = std::move(monitor->second.swapchain_processor);
        retired_processors_to_stop = std::move(monitor->second.retired_swapchain_processors);
      }

      // Stop frame processing and close swapchain handles before departure.
      // IddCxMonitorDeparture can invalidate active swapchain objects before
      // our local processors leave scope during temporary-display removal.
      if (processor_to_stop) {
        processor_to_stop->stop();
        processor_to_stop.reset();
      }
      for (auto &retired_processor: retired_processors_to_stop) {
        if (retired_processor) {
          retired_processor->stop();
          retired_processor.reset();
        }
      }

      // IddCx can synchronously or asynchronously issue swapchain callbacks
      // during departure. Calling it outside the backend mutex keeps those
      // callbacks from re-entering a locked monitor map.
      const auto status = IddCxMonitorDeparture(monitor_handle);
      if (!NT_SUCCESS(status)) {
        std::lock_guard lock {mutex_};
        if (const auto monitor = monitors_.find(display_id); monitor != monitors_.end()) {
          monitor->second.departing = false;
          monitor->second.swapchain_processor = std::move(processor_to_stop);
          monitor->second.retired_swapchain_processors = std::move(retired_processors_to_stop);
        }
        return vdd::BackendError::Failed;
      }

      std::lock_guard lock {mutex_};
      if (const auto monitor = monitors_.find(display_id);
          monitor != monitors_.end() &&
          monitor->second.monitor == monitor_handle) {
        monitors_.erase(monitor);
      }
      return vdd::BackendError::None;
    }

  public:
    NTSTATUS assign_swapchain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *args) {
      if (!monitor || !args || !args->hSwapChain || !args->hNextSurfaceAvailable) {
        return STATUS_INVALID_PARAMETER;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_DEVICE_NOT_READY;
      }

      {
        std::lock_guard lock {mutex_};
        const auto record = monitors_.find(context->display_id);
        if (record == monitors_.end() || record->second.departing) {
          // This status is the IddCx-approved way to decline a swapchain that
          // races with monitor teardown; generic failures trip verifier 0x700.
          return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
        }
      }

      auto processor = std::make_unique<SwapChainProcessor>(args->hSwapChain, args->hNextSurfaceAvailable);
      const HRESULT hr = processor->start(args->RenderAdapterLuid);
      if (FAILED(hr)) {
        return STATUS_UNSUCCESSFUL;
      }

      {
        std::lock_guard lock {mutex_};
        const auto record = monitors_.find(context->display_id);
        if (record == monitors_.end()) {
          processor->stop();
          processor->abandon_swapchain();
          return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
        }
        if (!record->second.departing) {
          auto previous_processor = std::move(record->second.swapchain_processor);
          if (previous_processor) {
            // IddCx rotates swapchains inside HandleNewSwapChain. Keep the old
            // WDF object alive until monitor teardown to avoid racing that path.
            previous_processor->stop();
            record->second.retired_swapchain_processors.push_back(std::move(previous_processor));
          }
          record->second.swapchain_processor = std::move(processor);
        } else {
          processor->stop();
          processor->abandon_swapchain();
          return STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN;
        }
      }

      return STATUS_SUCCESS;
    }

    NTSTATUS unassign_swapchain(IDDCX_MONITOR monitor) {
      if (!monitor) {
        return STATUS_INVALID_PARAMETER;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return STATUS_DEVICE_NOT_READY;
      }

      std::unique_ptr<SwapChainProcessor> processor_to_stop;
      bool erase_departing_record = false;
      {
        std::lock_guard lock {mutex_};
        const auto record = monitors_.find(context->display_id);
        if (record == monitors_.end()) {
          // Monitor departure may remove our bookkeeping before IddCx delivers a
          // final unassign callback. The swapchain is already gone in that case.
          return STATUS_SUCCESS;
        }

        processor_to_stop = std::move(record->second.swapchain_processor);
        erase_departing_record = record->second.departing;
        if (erase_departing_record) {
          monitors_.erase(record);
        }
      }

      processor_to_stop.reset();
      return STATUS_SUCCESS;
    }

    NTSTATUS query_target_modes(
      IDDCX_MONITOR monitor,
      const IDARG_IN_QUERYTARGETMODES *input,
      IDARG_OUT_QUERYTARGETMODES *output
    ) {
      const auto requested_shape = requested_mode_shape(monitor);
      if (requested_shape.has_value()) {
        return fill_target_modes(input, output, &*requested_shape);
      }

      return fill_target_modes(input, output);
    }

    NTSTATUS query_target_modes2(
      IDDCX_MONITOR monitor,
      const IDARG_IN_QUERYTARGETMODES2 *input,
      IDARG_OUT_QUERYTARGETMODES *output
    ) {
      const auto requested_shape = requested_mode_shape(monitor);
      if (requested_shape.has_value()) {
        return fill_target_modes2(input, output, &*requested_shape);
      }

      return fill_target_modes2(input, output);
    }

  private:
    std::optional<ModeShape> requested_mode_shape(IDDCX_MONITOR monitor) {
      if (!monitor) {
        return std::nullopt;
      }

      auto *context = GetMonitorContext(monitor);
      if (!context || !context->backend) {
        return std::nullopt;
      }

      std::lock_guard lock {mutex_};
      const auto record = monitors_.find(context->display_id);
      if (record == monitors_.end()) {
        return std::nullopt;
      }

      return mode_shape_from_descriptor(record->second.descriptor);
    }

    std::mutex mutex_ {};
    IDDCX_ADAPTER adapter_ {};
    bool adapter_ready_ {};
    std::uint32_t permanent_display_count_ {};
    std::map<std::uint64_t, MonitorRecord> monitors_ {};
  };

  class DeviceState {
  public:
    DeviceState():
        controller {
          vdd::DisplayStore {
            kMaxPermanentDisplays,
            kMaxTemporaryDisplays,
            load_temporary_connector_reservations()
          },
          backend
        },
        dispatcher {controller} {
      start_reaper();
    }

    ~DeviceState() {
      stop_reaper();
    }

    vdd::IoctlDispatchResult dispatch(
      const ULONG io_control_code,
      void *input,
      const std::size_t input_buffer_length,
      void *output,
      const std::size_t output_buffer_length,
      const std::chrono::steady_clock::time_point now
    ) {
      std::lock_guard lock {controller_mutex};
      return dispatcher.dispatch(
        io_control_code,
        input,
        input_buffer_length,
        output,
        output_buffer_length,
        now
      );
    }

    IddCxBackend backend {};
    vdd::DriverController controller;
    vdd::IoctlDispatcher dispatcher;

  private:
    static constexpr auto kReaperInterval = std::chrono::seconds(1);

    void start_reaper() {
      try {
        reaper_thread = std::thread([this]() {
          reaper_loop();
        });
      } catch (...) {
        // The control plane can still remove displays explicitly. If the
        // reaper cannot start, creation still works and lease feeds remain
        // validated by the store.
      }
    }

    void stop_reaper() {
      reaper_stop_requested.store(true, std::memory_order_release);
      reaper_cv.notify_all();
      if (reaper_thread.joinable()) {
        reaper_thread.join();
      }
    }

    void reaper_loop() {
      std::unique_lock wait_lock {reaper_wait_mutex};
      while (!reaper_stop_requested.load(std::memory_order_acquire)) {
        if (reaper_cv.wait_for(wait_lock, kReaperInterval, [this]() {
              return reaper_stop_requested.load(std::memory_order_acquire);
            })) {
          break;
        }

        std::lock_guard controller_lock {controller_mutex};
        (void) controller.reap_expired(std::chrono::steady_clock::now());
      }
    }

    std::mutex controller_mutex {};
    std::atomic<bool> reaper_stop_requested {false};
    std::mutex reaper_wait_mutex {};
    std::condition_variable reaper_cv {};
    std::thread reaper_thread {};
  };

  NTSTATUS ntstatus_from_ioctl_status(const vdd::IoctlStatus status) {
    switch (status) {
      case vdd::IoctlStatus::Success:
        return STATUS_SUCCESS;
      case vdd::IoctlStatus::InvalidIoctl:
        return STATUS_INVALID_DEVICE_REQUEST;
      case vdd::IoctlStatus::InvalidInputBuffer:
      case vdd::IoctlStatus::InvalidRequest:
        return STATUS_INVALID_PARAMETER;
      case vdd::IoctlStatus::InvalidOutputBuffer:
        return STATUS_BUFFER_TOO_SMALL;
      case vdd::IoctlStatus::AlreadyExists:
        return STATUS_DEVICE_BUSY;
      case vdd::IoctlStatus::LimitReached:
        return STATUS_INSUFFICIENT_RESOURCES;
      case vdd::IoctlStatus::NotFound:
        return STATUS_NOT_FOUND;
      case vdd::IoctlStatus::BackendFailed:
        return STATUS_UNSUCCESSFUL;
    }

    return STATUS_UNSUCCESSFUL;
  }

  void cleanup_device_context(WDFOBJECT object) {
    auto *context = GetDeviceContext(static_cast<WDFDEVICE>(object));
    delete context->state;
    context->state = nullptr;
  }

  NTSTATUS retrieve_request_buffer(
    WDFREQUEST request,
    const bool output,
    const std::size_t length,
    void **buffer
  ) {
    *buffer = nullptr;
    if (length == 0) {
      return STATUS_SUCCESS;
    }

    return output ?
      WdfRequestRetrieveOutputBuffer(request, length, buffer, nullptr) :
      WdfRequestRetrieveInputBuffer(request, length, buffer, nullptr);
  }
}  // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SunshineEvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY SunshineEvtDeviceD0Entry;
EVT_IDD_CX_DEVICE_IO_CONTROL SunshineEvtIddCxDeviceIoControl;
EVT_IDD_CX_ADAPTER_INIT_FINISHED SunshineEvtAdapterInitFinished;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES SunshineEvtGetDefaultDescriptionModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION SunshineEvtParseMonitorDescription;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES SunshineEvtQueryTargetModes;
EVT_IDD_CX_ADAPTER_COMMIT_MODES SunshineEvtCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION2 SunshineEvtParseMonitorDescription2;
EVT_IDD_CX_ADAPTER_QUERY_TARGET_INFO SunshineEvtAdapterQueryTargetInfo;
EVT_IDD_CX_ADAPTER_COMMIT_MODES2 SunshineEvtCommitModes2;
EVT_IDD_CX_MONITOR_SET_DEFAULT_HDR_METADATA SunshineEvtSetDefaultHdrMetadata;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES2 SunshineEvtQueryTargetModes2;
EVT_IDD_CX_MONITOR_SET_GAMMA_RAMP SunshineEvtSetGammaRamp;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN SunshineEvtAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN SunshineEvtUnassignSwapChain;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path) {
  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, SunshineEvtDeviceAdd);
  return WdfDriverCreate(driver_object, registry_path, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

NTSTATUS SunshineEvtDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT device_init) {
  WDF_PNPPOWER_EVENT_CALLBACKS pnp_callbacks;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp_callbacks);
  pnp_callbacks.EvtDeviceD0Entry = SunshineEvtDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &pnp_callbacks);

  IDD_CX_CLIENT_CONFIG idd_config;
  IDD_CX_CLIENT_CONFIG_INIT(&idd_config);
  idd_config.EvtIddCxDeviceIoControl = SunshineEvtIddCxDeviceIoControl;
  idd_config.EvtIddCxAdapterInitFinished = SunshineEvtAdapterInitFinished;
  idd_config.EvtIddCxMonitorGetDefaultDescriptionModes = SunshineEvtGetDefaultDescriptionModes;
  idd_config.EvtIddCxMonitorAssignSwapChain = SunshineEvtAssignSwapChain;
  idd_config.EvtIddCxMonitorUnassignSwapChain = SunshineEvtUnassignSwapChain;
  if (has_hdr_iddcx_ddi()) {
    idd_config.EvtIddCxParseMonitorDescription2 = SunshineEvtParseMonitorDescription2;
    idd_config.EvtIddCxAdapterQueryTargetInfo = SunshineEvtAdapterQueryTargetInfo;
    idd_config.EvtIddCxAdapterCommitModes2 = SunshineEvtCommitModes2;
    idd_config.EvtIddCxMonitorSetDefaultHdrMetaData = SunshineEvtSetDefaultHdrMetadata;
    idd_config.EvtIddCxMonitorQueryTargetModes2 = SunshineEvtQueryTargetModes2;
    idd_config.EvtIddCxMonitorSetGammaRamp = SunshineEvtSetGammaRamp;
  } else {
    idd_config.EvtIddCxParseMonitorDescription = SunshineEvtParseMonitorDescription;
    idd_config.EvtIddCxMonitorQueryTargetModes = SunshineEvtQueryTargetModes;
    idd_config.EvtIddCxAdapterCommitModes = SunshineEvtCommitModes;
  }

  NTSTATUS status = IddCxDeviceInitConfig(device_init, &idd_config);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_OBJECT_ATTRIBUTES device_attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&device_attributes, DeviceContext);
  device_attributes.EvtCleanupCallback = cleanup_device_context;

  WDFDEVICE device = nullptr;
  status = WdfDeviceCreate(&device_init, &device_attributes, &device);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  auto *context = GetDeviceContext(device);
  context->state = new (std::nothrow) DeviceState();
  if (!context->state) {
    return STATUS_INSUFFICIENT_RESOURCES;
  }

  status = WdfDeviceCreateDeviceInterface(device, &kControlInterfaceGuid, nullptr);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  status = IddCxDeviceInitialize(device);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
  auto *context = GetDeviceContext(device);
  if (!context || !context->state) {
    return STATUS_DEVICE_NOT_READY;
  }

  // IddCx adapter init requires the WDF device to be powered. Doing this in
  // DeviceAdd leaves the PDO installed but the adapter unusable on restart.
  return context->state->backend.initialize_adapter(device);
}

NTSTATUS SunshineEvtAdapterInitFinished(
  IDDCX_ADAPTER adapter,
  const IDARG_IN_ADAPTER_INIT_FINISHED *args
) {
  auto *context = GetAdapterContext(adapter);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->adapter_init_finished(args);
}

NTSTATUS SunshineEvtParseMonitorDescription(
  const IDARG_IN_PARSEMONITORDESCRIPTION *input,
  IDARG_OUT_PARSEMONITORDESCRIPTION *output
) {
  return fill_monitor_modes(input, output);
}

NTSTATUS SunshineEvtGetDefaultDescriptionModes(
  IDDCX_MONITOR,
  const IDARG_IN_GETDEFAULTDESCRIPTIONMODES *input,
  IDARG_OUT_GETDEFAULTDESCRIPTIONMODES *output
) {
  return fill_default_monitor_modes(input, output);
}

NTSTATUS SunshineEvtQueryTargetModes(
  IDDCX_MONITOR monitor,
  const IDARG_IN_QUERYTARGETMODES *input,
  IDARG_OUT_QUERYTARGETMODES *output
) {
  auto *context = GetMonitorContext(monitor);
  if (context && context->backend) {
    return context->backend->query_target_modes(monitor, input, output);
  }

  return fill_target_modes(input, output);
}

NTSTATUS SunshineEvtCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES *) {
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtParseMonitorDescription2(
  const IDARG_IN_PARSEMONITORDESCRIPTION2 *input,
  IDARG_OUT_PARSEMONITORDESCRIPTION *output
) {
  return fill_monitor_modes2(input, output);
}

NTSTATUS SunshineEvtAdapterQueryTargetInfo(
  IDDCX_ADAPTER,
  IDARG_IN_QUERYTARGET_INFO *,
  IDARG_OUT_QUERYTARGET_INFO *output
) {
  if (!output) {
    return STATUS_INVALID_PARAMETER;
  }

  output->TargetCaps = static_cast<IDDCX_TARGET_CAPS>(
    static_cast<UINT>(IDDCX_TARGET_CAPS_HIGH_COLOR_SPACE) |
    static_cast<UINT>(IDDCX_TARGET_CAPS_WIDE_COLOR_SPACE)
  );
  populate_rgb_wire_bits(output->DitheringSupport, IDDCX_BITS_PER_COMPONENT_10);
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtCommitModes2(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES2 *) {
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtSetDefaultHdrMetadata(
  IDDCX_MONITOR,
  const IDARG_IN_MONITOR_SET_DEFAULT_HDR_METADATA *
) {
  return STATUS_SUCCESS;
}

NTSTATUS SunshineEvtSetGammaRamp(
  IDDCX_MONITOR,
  const IDARG_IN_SET_GAMMARAMP *args
) {
  // The driver does not transform pixels itself, but Windows probes this DDI
  // after we advertise software gamma support for HDR/high-color targets.
  return args ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

NTSTATUS SunshineEvtQueryTargetModes2(
  IDDCX_MONITOR monitor,
  const IDARG_IN_QUERYTARGETMODES2 *input,
  IDARG_OUT_QUERYTARGETMODES *output
) {
  auto *context = GetMonitorContext(monitor);
  if (context && context->backend) {
    return context->backend->query_target_modes2(monitor, input, output);
  }

  return fill_target_modes2(input, output);
}

NTSTATUS SunshineEvtAssignSwapChain(IDDCX_MONITOR monitor, const IDARG_IN_SETSWAPCHAIN *args) {
  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->assign_swapchain(monitor, args);
}

NTSTATUS SunshineEvtUnassignSwapChain(IDDCX_MONITOR monitor) {
  auto *context = GetMonitorContext(monitor);
  if (!context || !context->backend) {
    return STATUS_DEVICE_NOT_READY;
  }

  return context->backend->unassign_swapchain(monitor);
}

void SunshineEvtIddCxDeviceIoControl(
  WDFDEVICE device,
  WDFREQUEST request,
  const std::size_t output_buffer_length,
  const std::size_t input_buffer_length,
  const ULONG io_control_code
) {
  auto *context = GetDeviceContext(device);
  if (!context || !context->state) {
    WdfRequestComplete(request, STATUS_DEVICE_NOT_READY);
    return;
  }

  void *input = nullptr;
  NTSTATUS status = retrieve_request_buffer(request, false, input_buffer_length, &input);
  if (!NT_SUCCESS(status)) {
    WdfRequestComplete(request, status);
    return;
  }

  void *output = nullptr;
  status = retrieve_request_buffer(request, true, output_buffer_length, &output);
  if (!NT_SUCCESS(status)) {
    WdfRequestComplete(request, status);
    return;
  }

  const auto result = context->state->dispatch(
    io_control_code,
    input,
    input_buffer_length,
    output,
    output_buffer_length,
    std::chrono::steady_clock::now()
  );

  WdfRequestCompleteWithInformation(
    request,
    ntstatus_from_ioctl_status(result.status),
    result.bytes_returned
  );
}
