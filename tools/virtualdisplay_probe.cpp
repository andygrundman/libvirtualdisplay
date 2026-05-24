#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#endif

namespace vdd = virtual_display::driver;

namespace {
  void print_usage() {
    std::cout
      << "virtualdisplay_probe commands:\n"
      << "  --diagnose\n"
      << "  --check\n"
      << "  --query-permanent\n"
      << "  --set-permanent <count>\n"
      << "  --self-test-permanent [count]\n"
      << "  --self-test-temp [width height refresh_hz]\n"
      << "  --self-test-hdr [width height refresh_hz]\n";
  }

  std::uint64_t transient_id(const std::uint64_t salt) {
    const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count()
    );
    return 0x6000000000000000ull | ((ticks ^ salt) & 0x0fffffffffffffffull);
  }

  vdd::CreateTemporaryDisplayRequest make_temporary_request(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = transient_id(0x0d15ea5e);
    request.display_id = transient_id(0x51dd15c0);
    request.width = width;
    request.height = height;
    request.refresh_rate_millihz = refresh_hz * 1000u;
    request.requested_timeout_ms = 10'000;
    std::strncpy(request.display_name, "Sunshine Probe", sizeof(request.display_name) - 1);
    return request;
  }

  int fail(const std::string &message, const vdd::ControlOperationResult &result) {
    std::cerr << message << ": " << vdd::to_string(result.status);
    if (result.native_error != 0) {
      std::cerr << " native_error=" << result.native_error;
    }
    std::cerr << '\n';
    return 1;
  }

  template<class T>
  int fail(const std::string &message, const vdd::ControlResult<T> &result) {
    return fail(message, {result.status, result.native_error});
  }

#ifdef _WIN32
  struct AdvancedColorInfo {
    bool v2 = false;
    bool supported = false;
    bool active = false;
    bool limited_by_policy = false;
    bool hdr_supported = false;
    bool hdr_enabled = false;
    DISPLAYCONFIG_COLOR_ENCODING color_encoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
    std::uint32_t bits_per_color_channel = 0;
    std::uint32_t active_color_mode = 0;
  };

  struct DisplayConfigGetAdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
    union {
      struct {
        std::uint32_t advanced_color_supported : 1;
        std::uint32_t advanced_color_active : 1;
        std::uint32_t reserved1 : 1;
        std::uint32_t advanced_color_limited_by_policy : 1;
        std::uint32_t high_dynamic_range_supported : 1;
        std::uint32_t high_dynamic_range_user_enabled : 1;
        std::uint32_t wide_color_supported : 1;
        std::uint32_t wide_color_user_enabled : 1;
        std::uint32_t reserved : 24;
      };
      std::uint32_t value;
    };
    DISPLAYCONFIG_COLOR_ENCODING color_encoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
    std::uint32_t bits_per_color_channel = 0;
    std::uint32_t active_color_mode = 0;
  };

  struct DisplayConfigSetHdrState {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header {};
    union {
      struct {
        std::uint32_t enable_hdr : 1;
        std::uint32_t reserved : 31;
      };
      std::uint32_t value;
    };
  };

  std::optional<AdvancedColorInfo> query_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id
  ) {
    const auto luid = vdd::to_windows_luid(adapter_luid);

    DisplayConfigGetAdvancedColorInfo2 info {};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    info.header.size = sizeof(info);
    info.header.adapterId = luid;
    info.header.id = target_id;
    if (DisplayConfigGetDeviceInfo(&info.header) == ERROR_SUCCESS) {
      return AdvancedColorInfo {
        true,
        info.advanced_color_supported != 0,
        info.advanced_color_active != 0,
        info.advanced_color_limited_by_policy != 0,
        info.high_dynamic_range_supported != 0,
        info.high_dynamic_range_user_enabled != 0,
        info.color_encoding,
        info.bits_per_color_channel,
        info.active_color_mode
      };
    }

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO fallback {};
    fallback.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    fallback.header.size = sizeof(fallback);
    fallback.header.adapterId = luid;
    fallback.header.id = target_id;
    if (DisplayConfigGetDeviceInfo(&fallback.header) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    return AdvancedColorInfo {
      false,
      fallback.advancedColorSupported != 0,
      fallback.advancedColorEnabled != 0,
      fallback.advancedColorForceDisabled != 0,
      false,
      false,
      fallback.colorEncoding,
      fallback.bitsPerColorChannel,
      0
    };
  }

  bool set_hdr_state(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool enabled
  ) {
    DisplayConfigSetHdrState state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = vdd::to_windows_luid(adapter_luid);
    state.header.id = target_id;
    state.enable_hdr = enabled ? 1u : 0u;
    return DisplayConfigSetDeviceInfo(&state.header) == ERROR_SUCCESS;
  }

  bool set_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool enabled
  ) {
    DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE state {};
    state.header.type = DISPLAYCONFIG_DEVICE_INFO_SET_ADVANCED_COLOR_STATE;
    state.header.size = sizeof(state);
    state.header.adapterId = vdd::to_windows_luid(adapter_luid);
    state.header.id = target_id;
    state.enableAdvancedColor = enabled ? 1u : 0u;
    return DisplayConfigSetDeviceInfo(&state.header) == ERROR_SUCCESS;
  }

  std::optional<AdvancedColorInfo> wait_for_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool require_hdr_enabled
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<AdvancedColorInfo> latest;
    do {
      latest = query_advanced_color(adapter_luid, target_id);
      if (latest) {
        const bool hdr_ready =
          latest->v2 &&
          latest->supported &&
          latest->hdr_supported &&
          latest->hdr_enabled &&
          !latest->limited_by_policy &&
          latest->bits_per_color_channel >= 10;
        if (!require_hdr_enabled || hdr_ready) {
          return latest;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    return latest;
  }

  void print_advanced_color(const AdvancedColorInfo &info) {
    std::cout << "advanced_color_v2=" << (info.v2 ? 1 : 0)
              << " supported=" << (info.supported ? 1 : 0)
              << " active=" << (info.active ? 1 : 0)
              << " limited_by_policy=" << (info.limited_by_policy ? 1 : 0)
              << " hdr_supported=" << (info.hdr_supported ? 1 : 0)
              << " hdr_enabled=" << (info.hdr_enabled ? 1 : 0)
              << " bits_per_color_channel=" << info.bits_per_color_channel
              << " active_color_mode=" << info.active_color_mode
              << " color_encoding=" << static_cast<unsigned int>(info.color_encoding)
              << '\n';
  }

  std::string to_utf8(const std::wstring &value) {
    if (value.empty()) {
      return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
      return "<utf8 conversion failed>";
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr) <= 0) {
      return "<utf8 conversion failed>";
    }
    if (!result.empty() && result.back() == '\0') {
      result.pop_back();
    }
    return result;
  }

  int diagnose_control_devices() {
    std::uint32_t enumerate_error = 0;
    const auto devices = vdd::enumerate_control_devices(&enumerate_error);
    bool any_openable = false;

    std::cout << "control_interface_count=" << devices.size()
              << " enumerate_error=" << enumerate_error << '\n';
    for (std::size_t index = 0; index < devices.size(); ++index) {
      const auto &device = devices[index];
      any_openable = any_openable || device.openable;
      std::cout << "control_interface[" << index << "]"
                << " openable=" << (device.openable ? 1 : 0)
                << " native_error=" << device.native_error
                << " path=\"" << to_utf8(device.device_path) << "\"\n";
    }

    return any_openable ? 0 : 1;
  }
#endif
}  // namespace

int main(const int argc, char **argv) {
#ifndef _WIN32
  (void) argc;
  (void) argv;
  std::cerr << "virtualdisplay_probe is only supported on Windows.\n";
  return 1;
#else
  if (argc < 2) {
    print_usage();
    return 2;
  }

  const std::string command {argv[1]};
  if (command == "--diagnose") {
    return diagnose_control_devices();
  }

  auto opened = vdd::open_first_control_device();
  if (!opened.ok()) {
    return fail("open control device failed", {opened.status, opened.native_error});
  }

  vdd::ControlClient client {*opened.transport};
  const auto protocol = client.query_protocol_version();
  if (!protocol.ok()) {
    return fail("protocol check failed", protocol);
  }

  if (command == "--check") {
    std::cout << "protocol=" << protocol.value.major << '.'
              << protocol.value.minor << '.' << protocol.value.patch << '\n';
    return 0;
  }

  if (command == "--query-permanent") {
    const auto result = client.query_permanent_display_count();
    if (!result.ok()) {
      return fail("query permanent count failed", result);
    }
    std::cout << "permanent=" << result.value.current_display_count
              << " max=" << result.value.max_display_count
              << " temporary=" << result.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--set-permanent") {
    if (argc < 3) {
      print_usage();
      return 2;
    }
    vdd::PermanentDisplayCountRequest request {};
    request.display_count = static_cast<std::uint32_t>(std::stoul(argv[2]));
    const auto result = client.set_permanent_display_count(request);
    if (!result.ok()) {
      return fail("set permanent count failed", result);
    }
    std::cout << "permanent=" << result.value.current_display_count
              << " max=" << result.value.max_display_count
              << " temporary=" << result.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--self-test-permanent") {
    const auto before = client.query_permanent_display_count();
    if (!before.ok()) {
      return fail("query permanent count failed", before);
    }

    const auto requested = argc >= 3 ?
      static_cast<std::uint32_t>(std::stoul(argv[2])) :
      (before.value.current_display_count == 0 ? 1u : 0u);
    if (requested > before.value.max_display_count) {
      std::cerr << "requested permanent count " << requested
                << " exceeds max " << before.value.max_display_count << '\n';
      return 2;
    }

    vdd::PermanentDisplayCountRequest request {};
    request.display_count = requested;
    const auto changed = client.set_permanent_display_count(request);
    if (!changed.ok()) {
      return fail("set permanent count failed", changed);
    }
    if (changed.value.current_display_count != requested) {
      std::cerr << "set permanent count returned " << changed.value.current_display_count
                << " after requesting " << requested << '\n';
      return 1;
    }

    vdd::PermanentDisplayCountRequest restore {};
    restore.display_count = before.value.current_display_count;
    const auto restored = client.set_permanent_display_count(restore);
    if (!restored.ok()) {
      return fail("restore permanent count failed", restored);
    }
    if (restored.value.current_display_count != before.value.current_display_count) {
      std::cerr << "restore permanent count returned " << restored.value.current_display_count
                << " after requesting " << before.value.current_display_count << '\n';
      return 1;
    }

    std::cout << "permanent_self_test=" << requested
              << " restored=" << restored.value.current_display_count
              << " max=" << before.value.max_display_count
              << " temporary=" << restored.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--self-test-temp") {
    const std::uint32_t width = argc >= 3 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 1920u;
    const std::uint32_t height = argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 1080u;
    const std::uint32_t refresh_hz = argc >= 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 60u;
    const auto request = make_temporary_request(width, height, refresh_hz);

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create temporary display failed", created);
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };
    const auto queried = client.query_lease(lease_request);
    if (!queried.ok()) {
      (void) client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      return fail("query lease failed", queried);
    }

    const auto removed = client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    if (!removed.ok()) {
      return fail("remove temporary display failed", removed);
    }

    std::cout << "created_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " lease_temporary_count=" << queried.value.temporary_display_count << '\n';
    return 0;
  }

  if (command == "--self-test-hdr") {
    const std::uint32_t width = argc >= 3 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 1920u;
    const std::uint32_t height = argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 1080u;
    const std::uint32_t refresh_hz = argc >= 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 60u;
    const auto request = make_temporary_request(width, height, refresh_hz);

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create temporary HDR display failed", created);
    }

    const auto remove_created = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    };

    const auto before = wait_for_advanced_color(created.value.os_adapter_luid, created.value.target_id, false);
    if (!before) {
      (void) remove_created();
      std::cerr << "advanced color query failed for target " << created.value.target_id << '\n';
      return 1;
    }

    const bool hdr_set = set_hdr_state(created.value.os_adapter_luid, created.value.target_id, true);
    const bool advanced_color_set = set_advanced_color(created.value.os_adapter_luid, created.value.target_id, true);
    const auto after = wait_for_advanced_color(created.value.os_adapter_luid, created.value.target_id, true);

    const auto removed = remove_created();
    if (!removed.ok()) {
      return fail("remove temporary HDR display failed", removed);
    }

    if (!after) {
      std::cerr << "advanced color query did not return after HDR request\n";
      return 1;
    }

    print_advanced_color(*after);

    if (!after->v2 || !after->hdr_supported) {
      std::cerr << "temporary display is not reported as HDR-supported by Windows\n";
      return 1;
    }
    if (!hdr_set && !advanced_color_set) {
      std::cerr << "Windows rejected both HDR and Advanced Color enable requests\n";
      return 1;
    }
    if (after->limited_by_policy || !after->supported || !after->hdr_enabled || after->bits_per_color_channel < 10) {
      std::cerr << "temporary display did not enter HDR 10-bit mode after request\n";
      return 1;
    }

    std::cout << "hdr_self_test=1"
              << " created_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index << '\n';
    return 0;
  }

  print_usage();
  return 2;
#endif
}
