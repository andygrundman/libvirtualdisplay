#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
      << "  --self-test-hdr [width height refresh_hz]\n"
      << "  --self-test-lease-expiry [width height refresh_hz timeout_ms]\n"
      << "  --qa-multi-temp-lease [count timeout_ms]\n"
      << "  --qa-temp-lease [width height refresh_hz timeout_ms]\n"
      << "  --debug-temp-config [width height refresh_hz timeout_ms]\n";
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
    const std::uint32_t target_id,
    LONG *native_error = nullptr
  ) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    if (native_error) {
      *native_error = ERROR_SUCCESS;
    }

    DisplayConfigGetAdvancedColorInfo2 info {};
    info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2;
    info.header.size = sizeof(info);
    info.header.adapterId = luid;
    info.header.id = target_id;
    auto result = DisplayConfigGetDeviceInfo(&info.header);
    if (result == ERROR_SUCCESS) {
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
    result = DisplayConfigGetDeviceInfo(&fallback.header);
    if (result != ERROR_SUCCESS) {
      if (native_error) {
        *native_error = result;
      }
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

  struct DisplayConfigData {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    UINT32 flags = 0;
  };

  bool same_luid(const LUID &left, const LUID &right) {
    return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
  }

  std::optional<DisplayConfigData> query_display_config(const UINT32 flags) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    auto result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
    if (result != ERROR_SUCCESS) {
      return std::nullopt;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    for (int attempt = 0; attempt < 4; ++attempt) {
      UINT32 query_path_count = static_cast<UINT32>(paths.size());
      UINT32 query_mode_count = static_cast<UINT32>(modes.size());
      result = QueryDisplayConfig(
        flags,
        &query_path_count,
        query_path_count == 0 ? nullptr : paths.data(),
        &query_mode_count,
        query_mode_count == 0 ? nullptr : modes.data(),
        nullptr
      );

      if (result == ERROR_SUCCESS) {
        paths.resize(query_path_count);
        modes.resize(query_mode_count);
        return DisplayConfigData {std::move(paths), std::move(modes), flags};
      }

      if (result != ERROR_INSUFFICIENT_BUFFER) {
        return std::nullopt;
      }

      paths.resize((std::max)(query_path_count, static_cast<UINT32>(paths.size() + 1)));
      modes.resize((std::max)(query_mode_count, static_cast<UINT32>(modes.size() + 1)));
    }

    return std::nullopt;
  }

  void clear_virtual_mode_indexes(DISPLAYCONFIG_PATH_INFO &path) {
    // With SDC_VIRTUAL_MODE_AWARE these union fields are group/index halves, not
    // raw modeInfoIdx values. Supplying stale query indices makes SetDisplayConfig
    // reject the topology or create a path that cannot be queried afterward.
    path.sourceInfo.sourceModeInfoIdx = DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID;
    path.targetInfo.targetModeInfoIdx = DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID;
    path.targetInfo.desktopModeInfoIdx = DISPLAYCONFIG_PATH_DESKTOP_IMAGE_IDX_INVALID;
  }

  void prepare_virtual_topology_path(DISPLAYCONFIG_PATH_INFO &path, const UINT32 clone_group_id, const bool active) {
    clear_virtual_mode_indexes(path);
    path.sourceInfo.cloneGroupId = clone_group_id;
    if (active) {
      path.flags |= DISPLAYCONFIG_PATH_ACTIVE;
    } else {
      path.flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
      path.sourceInfo.cloneGroupId = DISPLAYCONFIG_PATH_CLONE_GROUP_INVALID;
    }
  }

  bool apply_extended_topology() {
    return SetDisplayConfig(
      0,
      nullptr,
      0,
      nullptr,
      SDC_APPLY | SDC_TOPOLOGY_EXTEND
    ) == ERROR_SUCCESS;
  }

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_signal_info(
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t refresh_hz
  );

  LONG activate_target_path_result(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    (void) width;
    (void) height;
    (void) refresh_hz;
    const auto luid = vdd::to_windows_luid(adapter_luid);

    constexpr UINT32 kQueryFlags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto display_config = query_display_config(kQueryFlags);
    if (!display_config) {
      return ERROR_INVALID_PARAMETER;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> topology_paths;
    UINT32 clone_group_id = 0;
    for (auto path: display_config->paths) {
      if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0) {
        continue;
      }
      if (same_luid(path.targetInfo.adapterId, luid) && path.targetInfo.id == target_id) {
        return ERROR_SUCCESS;
      }
      prepare_virtual_topology_path(path, clone_group_id++, true);
      topology_paths.push_back(path);
    }

    std::optional<DISPLAYCONFIG_PATH_INFO> target_path;
    for (auto path: display_config->paths) {
      if (!same_luid(path.targetInfo.adapterId, luid) ||
          path.targetInfo.id != target_id ||
          !path.targetInfo.targetAvailable) {
        continue;
      }

      path.targetInfo.targetAvailable = TRUE;
      prepare_virtual_topology_path(path, clone_group_id, true);
      target_path = path;
      break;
    }

    if (!target_path) {
      return ERROR_NOT_FOUND;
    }
    topology_paths.push_back(*target_path);

    LONG result = SetDisplayConfig(
      static_cast<UINT32>(topology_paths.size()),
      topology_paths.data(),
      0,
      nullptr,
      SDC_APPLY | SDC_TOPOLOGY_SUPPLIED | SDC_ALLOW_PATH_ORDER_CHANGES | SDC_VIRTUAL_MODE_AWARE
    );
    if (result == ERROR_SUCCESS) {
      return result;
    }

    if (result == ERROR_GEN_FAILURE || result == ERROR_INVALID_PARAMETER) {
      auto full_paths = display_config->paths;
      clone_group_id = 0;
      for (auto &path: full_paths) {
        const bool already_active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        const bool is_target =
          same_luid(path.targetInfo.adapterId, luid) &&
          path.targetInfo.id == target_id &&
          path.sourceInfo.id == target_path->sourceInfo.id;
        prepare_virtual_topology_path(path, already_active || is_target ? clone_group_id++ : 0, already_active || is_target);
        if (is_target) {
          path.targetInfo.targetAvailable = TRUE;
        }
      }

      result = SetDisplayConfig(
        static_cast<UINT32>(full_paths.size()),
        full_paths.data(),
        0,
        nullptr,
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_VIRTUAL_MODE_AWARE
      );
      if (result == ERROR_SUCCESS) {
        return result;
      }
    }

    return result;
  }

  bool activate_target_path(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    return activate_target_path_result(adapter_luid, target_id, width, height, refresh_hz) == ERROR_SUCCESS;
  }

  void dump_display_config_paths(
    const std::optional<vdd::AdapterLuid> &adapter_luid = std::nullopt,
    const std::optional<std::uint32_t> &target_id = std::nullopt
  ) {
    constexpr UINT32 kQueryFlags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;
    auto display_config = query_display_config(kQueryFlags);
    if (!display_config) {
      std::cout << "display_config_query_error=1\n";
      return;
    }

    const auto filter_luid = adapter_luid ? vdd::to_windows_luid(*adapter_luid) : LUID {};
    std::cout << "display_config_paths=" << display_config->paths.size()
              << " modes=" << display_config->modes.size() << '\n';
    for (std::size_t index = 0; index < display_config->paths.size(); ++index) {
      const auto &path = display_config->paths[index];
      const bool matches_filter =
        !adapter_luid ||
        same_luid(path.targetInfo.adapterId, filter_luid) ||
        (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
      const bool matches_target = !target_id || path.targetInfo.id == *target_id;
      if (!matches_filter || (!matches_target && (path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0)) {
        continue;
      }

      std::cout << "path[" << index << "]"
                << " active=" << ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) ? 1 : 0)
                << " source_luid=" << path.sourceInfo.adapterId.HighPart << ':' << path.sourceInfo.adapterId.LowPart
                << " source_id=" << path.sourceInfo.id
                << " source_mode_idx=" << path.sourceInfo.sourceModeInfoIdx
                << " clone_group=" << path.sourceInfo.cloneGroupId
                << " target_luid=" << path.targetInfo.adapterId.HighPart << ':' << path.targetInfo.adapterId.LowPart
                << " target_id=" << path.targetInfo.id
                << " target_mode_idx=" << path.targetInfo.targetModeInfoIdx
                << " desktop_idx=" << path.targetInfo.desktopModeInfoIdx
                << " available=" << (path.targetInfo.targetAvailable ? 1 : 0)
                << " tech=" << static_cast<unsigned int>(path.targetInfo.outputTechnology)
                << " status=" << static_cast<unsigned int>(path.targetInfo.statusFlags)
                << '\n';
    }
  }

  void dump_active_paths_for_adapter(const vdd::AdapterLuid &adapter_luid) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    constexpr std::array<UINT32, 2> kQueryFlags {
      QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
      QDC_ONLY_ACTIVE_PATHS
    };

    for (const auto flags: kQueryFlags) {
      auto display_config = query_display_config(flags);
      if (!display_config) {
        std::cout << "active_paths_query_error flags=" << flags << '\n';
        continue;
      }

      std::cout << "active_paths flags=" << flags
                << " count=" << display_config->paths.size()
                << " modes=" << display_config->modes.size() << '\n';
      for (std::size_t index = 0; index < display_config->paths.size(); ++index) {
        const auto &path = display_config->paths[index];
        if (!same_luid(path.targetInfo.adapterId, luid)) {
          continue;
        }

        std::cout << "active_path[" << index << "]"
                  << " source_id=" << path.sourceInfo.id
                  << " source_mode_idx=" << (flags & QDC_VIRTUAL_MODE_AWARE ? path.sourceInfo.sourceModeInfoIdx : path.sourceInfo.modeInfoIdx)
                  << " clone_group=" << path.sourceInfo.cloneGroupId
                  << " target_id=" << path.targetInfo.id
                  << " target_mode_idx=" << (flags & QDC_VIRTUAL_MODE_AWARE ? path.targetInfo.targetModeInfoIdx : path.targetInfo.modeInfoIdx)
                  << " refresh=" << path.targetInfo.refreshRate.Numerator
                  << '/' << path.targetInfo.refreshRate.Denominator
                  << " flags=" << path.flags
                  << " status=" << path.targetInfo.statusFlags << '\n';
      }
    }
  }

  std::optional<AdvancedColorInfo> wait_for_advanced_color(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool require_hdr_enabled,
    LONG *native_error = nullptr
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    std::optional<AdvancedColorInfo> latest;
    do {
      latest = query_advanced_color(adapter_luid, target_id, native_error);
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

  struct DisplayPathInfo {
    bool active = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t refresh_millihz = 0;
  };

  std::optional<UINT32> source_mode_index(const DISPLAYCONFIG_PATH_INFO &path, const UINT32 flags) {
    if ((flags & QDC_VIRTUAL_MODE_AWARE) != 0) {
      if (path.sourceInfo.sourceModeInfoIdx == DISPLAYCONFIG_PATH_SOURCE_MODE_IDX_INVALID) {
        return std::nullopt;
      }
      return path.sourceInfo.sourceModeInfoIdx;
    }
    if (path.sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
      return std::nullopt;
    }
    return path.sourceInfo.modeInfoIdx;
  }

  std::optional<UINT32> target_mode_index(const DISPLAYCONFIG_PATH_INFO &path, const UINT32 flags) {
    if ((flags & QDC_VIRTUAL_MODE_AWARE) != 0) {
      if (path.targetInfo.targetModeInfoIdx == DISPLAYCONFIG_PATH_TARGET_MODE_IDX_INVALID) {
        return std::nullopt;
      }
      return path.targetInfo.targetModeInfoIdx;
    }
    if (path.targetInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
      return std::nullopt;
    }
    return path.targetInfo.modeInfoIdx;
  }

  DISPLAYCONFIG_VIDEO_SIGNAL_INFO make_signal_info(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO signal {};
    const auto total_width = (std::max)(width + width / 5u, width);
    const auto total_height = (std::max)(height + height / 20u, height);
    signal.pixelRate = static_cast<std::uint64_t>(total_width) * total_height * refresh_hz;
    signal.hSyncFreq.Numerator = static_cast<UINT32>((std::min<std::uint64_t>)(signal.pixelRate, UINT32_MAX));
    signal.hSyncFreq.Denominator = total_width;
    signal.vSyncFreq.Numerator = static_cast<UINT32>((std::min<std::uint64_t>)(signal.pixelRate, UINT32_MAX));
    signal.vSyncFreq.Denominator = total_width * total_height;
    signal.activeSize.cx = width;
    signal.activeSize.cy = height;
    signal.totalSize.cx = total_width;
    signal.totalSize.cy = total_height;
    signal.AdditionalSignalInfo.videoStandard = 255;
    signal.AdditionalSignalInfo.vSyncFreqDivider = 1;
    signal.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    return signal;
  }

  std::optional<DisplayPathInfo> query_display_path(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id
  ) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    constexpr std::array<UINT32, 2> kQueryFlags {
      QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE,
      QDC_ONLY_ACTIVE_PATHS
    };

    for (const auto flags: kQueryFlags) {
      auto display_config = query_display_config(flags);
      if (!display_config) {
        continue;
      }

      for (const auto &path: display_config->paths) {
        if (!same_luid(path.targetInfo.adapterId, luid) ||
            path.targetInfo.id != target_id) {
          continue;
        }

        DisplayPathInfo info {};
        info.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        info.refresh_millihz = path.targetInfo.refreshRate.Denominator == 0 ?
          0 :
          static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(path.targetInfo.refreshRate.Numerator) * 1000ull) /
            path.targetInfo.refreshRate.Denominator
          );

        const auto target_idx = target_mode_index(path, flags);
        if (target_idx && *target_idx < display_config->modes.size()) {
          const auto &mode = display_config->modes[*target_idx];
          if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            info.width = mode.targetMode.targetVideoSignalInfo.activeSize.cx;
            info.height = mode.targetMode.targetVideoSignalInfo.activeSize.cy;
            if (info.refresh_millihz == 0 &&
                mode.targetMode.targetVideoSignalInfo.vSyncFreq.Denominator != 0) {
              info.refresh_millihz = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(mode.targetMode.targetVideoSignalInfo.vSyncFreq.Numerator) * 1000ull) /
                mode.targetMode.targetVideoSignalInfo.vSyncFreq.Denominator
              );
            }
          }
        }

        const auto source_idx = source_mode_index(path, flags);
        if ((info.width == 0 || info.height == 0) &&
            source_idx &&
            *source_idx < display_config->modes.size()) {
          const auto &mode = display_config->modes[*source_idx];
          if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            info.width = mode.sourceMode.width;
            info.height = mode.sourceMode.height;
          }
        }

        return info;
      }
    }

    return std::nullopt;
  }

  std::optional<DisplayPathInfo> wait_for_display_path(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const bool present
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<DisplayPathInfo> latest;
    do {
      latest = query_display_path(adapter_luid, target_id);
      if (present == latest.has_value()) {
        return latest;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    return latest;
  }

  bool display_mode_matches(
    const DisplayPathInfo &path,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    const auto requested_refresh = refresh_hz * 1000u;
    const auto refresh_delta = path.refresh_millihz > requested_refresh ?
      path.refresh_millihz - requested_refresh :
      requested_refresh - path.refresh_millihz;
    return path.active &&
           path.width == width &&
           path.height == height &&
           refresh_delta <= 1000u;
  }

  std::optional<UINT32> active_source_id_for_target(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id
  ) {
    const auto luid = vdd::to_windows_luid(adapter_luid);
    auto display_config = query_display_config(QDC_ONLY_ACTIVE_PATHS);
    if (!display_config) {
      return std::nullopt;
    }

    for (const auto &path: display_config->paths) {
      if (same_luid(path.targetInfo.adapterId, luid) && path.targetInfo.id == target_id) {
        return path.sourceInfo.id;
      }
    }

    return std::nullopt;
  }

  std::optional<std::wstring> gdi_device_name_for_source(
    const vdd::AdapterLuid &adapter_luid,
    const UINT32 source_id
  ) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source_name {};
    source_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source_name.header.size = sizeof(source_name);
    source_name.header.adapterId = vdd::to_windows_luid(adapter_luid);
    source_name.header.id = source_id;
    if (DisplayConfigGetDeviceInfo(&source_name.header) != ERROR_SUCCESS) {
      return std::nullopt;
    }

    return std::wstring {source_name.viewGdiDeviceName};
  }

  bool set_active_display_mode(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    const auto source_id = active_source_id_for_target(adapter_luid, target_id);
    if (!source_id) {
      return false;
    }
    const auto gdi_name = gdi_device_name_for_source(adapter_luid, *source_id);
    if (!gdi_name) {
      return false;
    }

    DEVMODEW mode {};
    mode.dmSize = sizeof(mode);
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmDisplayFrequency = refresh_hz;
    mode.dmBitsPerPel = 32;
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY | DM_BITSPERPEL;
    return ChangeDisplaySettingsExW(gdi_name->c_str(), &mode, nullptr, 0, nullptr) == DISP_CHANGE_SUCCESSFUL;
  }

  bool ensure_active_display_mode(
    const vdd::AdapterLuid &adapter_luid,
    const std::uint32_t target_id,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint32_t refresh_hz
  ) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool mode_set_attempted = false;
    do {
      const auto path = query_display_path(adapter_luid, target_id);
      if (path && display_mode_matches(*path, width, height, refresh_hz)) {
        return true;
      }

      if (path && path->active && !mode_set_attempted) {
        // Windows can reuse the previous mode on a recycled target id. Apply
        // the requested GDI mode explicitly, then poll DisplayConfig again.
        mode_set_attempted = true;
        (void) set_active_display_mode(adapter_luid, target_id, width, height, refresh_hz);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
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

    if (!activate_target_path(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz)) {
      (void) apply_extended_topology();
    }
    if (!ensure_active_display_mode(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz)) {
      (void) remove_created();
      std::cerr << "temporary HDR display resolution mismatch after activation\n";
      return 1;
    }

    LONG advanced_color_error = ERROR_SUCCESS;
    const auto before = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      false,
      &advanced_color_error
    );
    if (!before) {
      (void) remove_created();
      std::cerr << "advanced color query failed for target " << created.value.target_id
                << " native_error=" << advanced_color_error << '\n';
      return 1;
    }

    const bool hdr_set = set_hdr_state(created.value.os_adapter_luid, created.value.target_id, true);
    const bool advanced_color_set = set_advanced_color(created.value.os_adapter_luid, created.value.target_id, true);
    const auto after = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      true,
      &advanced_color_error
    );

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

  if (command == "--self-test-lease-expiry") {
    const std::uint32_t width = argc >= 3 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 1920u;
    const std::uint32_t height = argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 1080u;
    const std::uint32_t refresh_hz = argc >= 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 60u;
    const std::uint32_t timeout_ms = argc >= 6 ? static_cast<std::uint32_t>(std::stoul(argv[5])) : 3'000u;
    auto request = make_temporary_request(width, height, refresh_hz);
    request.requested_timeout_ms = timeout_ms;

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create lease-expiry display failed", created);
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };

    const auto active = client.query_lease(lease_request);
    if (!active.ok()) {
      (void) client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      return fail("query active lease failed", active);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(created.value.effective_timeout_ms + 2'000u));

    const auto expired = client.query_lease(lease_request);
    if (!expired.ok()) {
      (void) client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      return fail("query expired lease failed", expired);
    }

    if (active.value.lease_exists == 0 ||
        active.value.temporary_display_count != 1 ||
        expired.value.lease_exists != 0 ||
        expired.value.temporary_display_count != 0) {
      (void) client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
      std::cerr << "lease expiry state mismatch"
                << " active_exists=" << active.value.lease_exists
                << " active_temporary=" << active.value.temporary_display_count
                << " expired_exists=" << expired.value.lease_exists
                << " expired_temporary=" << expired.value.temporary_display_count << '\n';
      return 1;
    }

    std::cout << "lease_expiry_self_test=1"
              << " display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " effective_timeout_ms=" << created.value.effective_timeout_ms
              << " expired=1\n";
    return 0;
  }

  if (command == "--qa-multi-temp-lease") {
    const std::uint32_t count = argc >= 3 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 3u;
    const std::uint32_t timeout_ms = argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 3'000u;
    if (count == 0 || count > 8) {
      std::cerr << "multi-temp QA count must be in the range 1..8\n";
      return 2;
    }

    const auto before_count = client.query_permanent_display_count();
    if (!before_count.ok()) {
      return fail("query permanent count failed", before_count);
    }

    const auto lease_id = transient_id(0x717a0000);
    std::vector<vdd::CreateTemporaryDisplayResult> created_displays;
    std::vector<std::uint32_t> connector_indexes;
    for (std::uint32_t index = 0; index < count; ++index) {
      constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 4> kSizes {{
        {1280u, 720u},
        {1920u, 1080u},
        {2560u, 1440u},
        {3840u, 2160u}
      }};
      const auto [width, height] = kSizes[index % kSizes.size()];
      auto request = make_temporary_request(width, height, index % 2 == 0 ? 60u : 120u);
      request.lease_id = lease_id;
      request.display_id = transient_id(0x517a0000 + index);
      request.requested_timeout_ms = timeout_ms;

      const auto created = client.create_temporary_display(request);
      if (!created.ok()) {
        (void) client.release_lease({vdd::kApiNamespaceGuid, lease_id, timeout_ms, 0});
        return fail("create multi-temp QA display failed", created);
      }
      if (std::find(connector_indexes.begin(), connector_indexes.end(), created.value.connector_index) != connector_indexes.end()) {
        (void) client.release_lease({vdd::kApiNamespaceGuid, lease_id, timeout_ms, 0});
        std::cerr << "multi-temp QA reused connector index " << created.value.connector_index << '\n';
        return 1;
      }
      connector_indexes.push_back(created.value.connector_index);
      created_displays.push_back(created.value);
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      lease_id,
      timeout_ms,
      0
    };
    const auto active = client.query_lease(lease_request);
    if (!active.ok()) {
      (void) client.release_lease(lease_request);
      return fail("query multi-temp QA lease failed", active);
    }
    if (active.value.lease_exists == 0 || active.value.temporary_display_count != count) {
      (void) client.release_lease(lease_request);
      std::cerr << "multi-temp QA lease count mismatch: expected " << count
                << " got " << active.value.temporary_display_count << '\n';
      return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms + 2'000u));
    const auto expired = client.query_lease(lease_request);
    if (!expired.ok()) {
      (void) client.release_lease(lease_request);
      return fail("query expired multi-temp QA lease failed", expired);
    }
    if (expired.value.lease_exists != 0 || expired.value.temporary_display_count != 0) {
      (void) client.release_lease(lease_request);
      std::cerr << "multi-temp QA lease did not expire cleanly"
                << " exists=" << expired.value.lease_exists
                << " temporary=" << expired.value.temporary_display_count << '\n';
      return 1;
    }

    const auto after_count = client.query_permanent_display_count();
    if (!after_count.ok()) {
      return fail("query permanent count after multi-temp QA failed", after_count);
    }
    if (after_count.value.temporary_display_count != before_count.value.temporary_display_count) {
      std::cerr << "multi-temp QA leaked temporary displays: before="
                << before_count.value.temporary_display_count
                << " after=" << after_count.value.temporary_display_count << '\n';
      return 1;
    }

    std::cout << "qa_multi_temp_lease=1"
              << " count=" << count
              << " effective_timeout_ms=" << active.value.effective_timeout_ms
              << " expired=1";
    for (const auto connector_index: connector_indexes) {
      std::cout << " connector_index=" << connector_index;
    }
    std::cout << '\n';
    return 0;
  }

  if (command == "--debug-temp-config") {
    const std::uint32_t width = argc >= 3 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 1920u;
    const std::uint32_t height = argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 1080u;
    const std::uint32_t refresh_hz = argc >= 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 60u;
    const std::uint32_t timeout_ms = argc >= 6 ? static_cast<std::uint32_t>(std::stoul(argv[5])) : 10'000u;
    auto request = make_temporary_request(width, height, refresh_hz);
    request.requested_timeout_ms = timeout_ms;

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create debug temporary display failed", created);
    }

    std::cout << "debug_display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " adapter_luid=" << vdd::to_windows_luid(created.value.os_adapter_luid).HighPart
              << ':' << vdd::to_windows_luid(created.value.os_adapter_luid).LowPart << '\n';

    dump_display_config_paths(created.value.os_adapter_luid, created.value.target_id);
    const auto activate_result = activate_target_path_result(
      created.value.os_adapter_luid,
      created.value.target_id,
      width,
      height,
      refresh_hz
    );
    std::cout << "activate_result=" << activate_result << '\n';
    if (activate_result != ERROR_SUCCESS) {
      (void) apply_extended_topology();
    }
    const auto mode_ready = ensure_active_display_mode(
      created.value.os_adapter_luid,
      created.value.target_id,
      width,
      height,
      refresh_hz
    );
    dump_active_paths_for_adapter(created.value.os_adapter_luid);
    const auto active_path = query_display_path(created.value.os_adapter_luid, created.value.target_id);
    if (active_path) {
      std::cout << "debug_active_path=1"
                << " width=" << active_path->width
                << " height=" << active_path->height
                << " refresh_millihz=" << active_path->refresh_millihz << '\n';
    } else {
      std::cout << "debug_active_path=0\n";
    }
    dump_display_config_paths(created.value.os_adapter_luid, created.value.target_id);

    const auto removed = client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    if (!removed.ok()) {
      return fail("remove debug temporary display failed", removed);
    }
    if (!mode_ready || !active_path || !display_mode_matches(*active_path, width, height, refresh_hz)) {
      std::cerr << "debug display resolution mismatch: expected "
                << width << 'x' << height << '@' << refresh_hz
                << " got "
                << (active_path ? active_path->width : 0) << 'x'
                << (active_path ? active_path->height : 0) << '@'
                << (active_path ? active_path->refresh_millihz : 0) << "mHz\n";
      return 1;
    }
    return activate_result == ERROR_SUCCESS && mode_ready ? 0 : 1;
  }

  if (command == "--qa-temp-lease") {
    const std::uint32_t width = argc >= 3 ? static_cast<std::uint32_t>(std::stoul(argv[2])) : 1920u;
    const std::uint32_t height = argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 1080u;
    const std::uint32_t refresh_hz = argc >= 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 60u;
    const std::uint32_t timeout_ms = argc >= 6 ? static_cast<std::uint32_t>(std::stoul(argv[5])) : 3'000u;
    auto request = make_temporary_request(width, height, refresh_hz);
    request.requested_timeout_ms = timeout_ms;

    const auto before_count = client.query_permanent_display_count();
    if (!before_count.ok()) {
      return fail("query permanent count failed", before_count);
    }

    const auto created = client.create_temporary_display(request);
    if (!created.ok()) {
      return fail("create QA temporary display failed", created);
    }

    const auto cleanup_created = [&]() {
      return client.remove_temporary_display({vdd::kApiNamespaceGuid, request.lease_id, request.display_id});
    };

    if (!activate_target_path(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz)) {
      (void) apply_extended_topology();
    }
    if (!ensure_active_display_mode(created.value.os_adapter_luid, created.value.target_id, width, height, refresh_hz)) {
      (void) cleanup_created();
      std::cerr << "QA display resolution mismatch after activation\n";
      return 1;
    }

    const vdd::LeaseRequest lease_request {
      vdd::kApiNamespaceGuid,
      request.lease_id,
      request.requested_timeout_ms,
      0
    };

    const auto queried = client.query_lease(lease_request);
    if (!queried.ok()) {
      (void) cleanup_created();
      return fail("query QA lease failed", queried);
    }
    if (queried.value.lease_exists == 0 || queried.value.temporary_display_count != 1) {
      (void) cleanup_created();
      std::cerr << "QA lease was not active immediately after create\n";
      return 1;
    }

    const auto path = wait_for_display_path(created.value.os_adapter_luid, created.value.target_id, true);
    if (!path || !path->active) {
      (void) cleanup_created();
      std::cerr << "created display did not appear in active Windows display paths\n";
      return 1;
    }
    if (path->width != width || path->height != height) {
      (void) cleanup_created();
      std::cerr << "created display resolution mismatch: expected "
                << width << 'x' << height << " got "
                << path->width << 'x' << path->height << '\n';
      return 1;
    }

    LONG advanced_color_error = ERROR_SUCCESS;
    const auto before_color = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      false,
      &advanced_color_error
    );
    if (!before_color) {
      (void) cleanup_created();
      std::cerr << "advanced color query failed for QA target " << created.value.target_id
                << " native_error=" << advanced_color_error << '\n';
      return 1;
    }
    const bool hdr_set = set_hdr_state(created.value.os_adapter_luid, created.value.target_id, true);
    const bool advanced_color_set = set_advanced_color(created.value.os_adapter_luid, created.value.target_id, true);
    const auto after_color = wait_for_advanced_color(
      created.value.os_adapter_luid,
      created.value.target_id,
      true,
      &advanced_color_error
    );
    if (!after_color) {
      (void) cleanup_created();
      std::cerr << "advanced color query did not return after QA HDR request\n";
      return 1;
    }

    print_advanced_color(*after_color);
    if (!after_color->v2 || !after_color->hdr_supported) {
      (void) cleanup_created();
      std::cerr << "QA display is not reported as HDR-supported by Windows\n";
      return 1;
    }
    if (!hdr_set && !advanced_color_set) {
      (void) cleanup_created();
      std::cerr << "Windows rejected both QA HDR and Advanced Color enable requests\n";
      return 1;
    }
    if (after_color->limited_by_policy ||
        !after_color->supported ||
        !after_color->hdr_enabled ||
        after_color->bits_per_color_channel < 10) {
      (void) cleanup_created();
      std::cerr << "QA display did not enter HDR 10-bit mode after request\n";
      return 1;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(created.value.effective_timeout_ms + 2'000u));

    const auto expired = client.query_lease(lease_request);
    if (!expired.ok()) {
      (void) cleanup_created();
      return fail("query expired QA lease failed", expired);
    }
    if (expired.value.lease_exists != 0 || expired.value.temporary_display_count != 0) {
      (void) cleanup_created();
      std::cerr << "QA lease did not expire after " << created.value.effective_timeout_ms << " ms\n";
      return 1;
    }

    const auto departed_path = wait_for_display_path(created.value.os_adapter_luid, created.value.target_id, false);
    if (departed_path) {
      (void) cleanup_created();
      std::cerr << "expired QA display is still present in active Windows display paths\n";
      return 1;
    }

    const auto after_count = client.query_permanent_display_count();
    if (!after_count.ok()) {
      return fail("query permanent count after QA failed", after_count);
    }
    if (after_count.value.temporary_display_count != before_count.value.temporary_display_count) {
      std::cerr << "temporary count mismatch after QA lease expiry: before="
                << before_count.value.temporary_display_count
                << " after=" << after_count.value.temporary_display_count << '\n';
      return 1;
    }

    std::cout << "qa_temp_lease=1"
              << " display_id=" << created.value.display_id
              << " target_id=" << created.value.target_id
              << " connector_index=" << created.value.connector_index
              << " resolution=" << path->width << 'x' << path->height
              << " refresh_millihz=" << path->refresh_millihz
              << " effective_timeout_ms=" << created.value.effective_timeout_ms
              << " expired=1\n";
    return 0;
  }

  print_usage();
  return 2;
#endif
}
