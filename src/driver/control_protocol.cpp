#include "virtual_display/driver/control_protocol.h"

#include <algorithm>

namespace virtual_display::driver {
  namespace {
    bool valid_manufacturer_id(const char (&manufacturer_id)[4]) {
      return manufacturer_id[0] >= 'A' && manufacturer_id[0] <= 'Z' &&
             manufacturer_id[1] >= 'A' && manufacturer_id[1] <= 'Z' &&
             manufacturer_id[2] >= 'A' && manufacturer_id[2] <= 'Z' &&
             manufacturer_id[3] == '\0';
    }
  }  // namespace

  bool is_valid_api_namespace(const Guid &guid) {
    return guid == kApiNamespaceGuid;
  }

  void set_default_permanent_display_settings(PermanentDisplayCountRequest &request) {
    if (request.width == 0) {
      request.width = 1920;
    }
    if (request.height == 0) {
      request.height = 1080;
    }
    if (request.physical_width_mm == 0) {
      request.physical_width_mm = kDefaultPhysicalWidthMillimeters;
    }
    if (request.physical_height_mm == 0) {
      request.physical_height_mm = kDefaultPhysicalHeightMillimeters;
    }
    if (request.refresh_rate_millihz == 0) {
      request.refresh_rate_millihz = 60'000;
    }
    if (trim_display_name(request.display_name).empty()) {
      std::fill(std::begin(request.display_name), std::end(request.display_name), '\0');
      constexpr char kDefaultName[] = "Sunshine Display";
      std::copy_n(kDefaultName, sizeof(kDefaultName) - 1, request.display_name);
    }
  }

  std::uint32_t normalize_timeout_ms(const std::uint32_t requested_timeout_ms) {
    if (requested_timeout_ms == 0) {
      return kDefaultLeaseTimeoutMs;
    }

    return std::clamp(requested_timeout_ms, kMinLeaseTimeoutMs, kMaxLeaseTimeoutMs);
  }

  std::string_view trim_display_name(const char (&display_name)[kDisplayNameChars]) {
    const auto end = std::find(std::begin(display_name), std::end(display_name), '\0');
    const auto raw_size = static_cast<std::size_t>(std::distance(std::begin(display_name), end));
    std::string_view value {display_name, raw_size};

    while (!value.empty() && value.back() == ' ') {
      value.remove_suffix(1);
    }

    return value;
  }

  ValidationError validate_create_temporary_display(
    const CreateTemporaryDisplayRequest &request,
    ValidatedCreateTemporaryDisplay *validated
  ) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.lease_id == 0) {
      return ValidationError::MissingLeaseId;
    }
    if (request.display_id == 0) {
      return ValidationError::MissingDisplayId;
    }
    if ((request.flags & ~kCreateTemporaryDisplayKnownFlags) != 0) {
      return ValidationError::InvalidFlags;
    }
    if (request.width < kMinWidth || request.width > kMaxWidth) {
      return ValidationError::InvalidWidth;
    }
    if (request.height < kMinHeight || request.height > kMaxHeight) {
      return ValidationError::InvalidHeight;
    }
    const auto physical_width_mm = request.physical_width_mm == 0 ?
      kDefaultPhysicalWidthMillimeters :
      request.physical_width_mm;
    const auto physical_height_mm = request.physical_height_mm == 0 ?
      kDefaultPhysicalHeightMillimeters :
      request.physical_height_mm;
    if (physical_width_mm < kMinPhysicalSizeMillimeters ||
        physical_width_mm > kMaxPhysicalSizeMillimeters ||
        physical_height_mm < kMinPhysicalSizeMillimeters ||
        physical_height_mm > kMaxPhysicalSizeMillimeters) {
      return ValidationError::InvalidPhysicalSize;
    }
    if (request.refresh_rate_millihz < kMinRefreshRateMilliHz ||
        request.refresh_rate_millihz > kMaxRefreshRateMilliHz) {
      return ValidationError::InvalidRefreshRate;
    }

    const auto display_name = trim_display_name(request.display_name);
    if (display_name.empty()) {
      return ValidationError::InvalidDisplayName;
    }

    if (validated) {
      validated->request = request;
      validated->request.physical_width_mm = physical_width_mm;
      validated->request.physical_height_mm = physical_height_mm;
      validated->effective_timeout_ms = normalize_timeout_ms(request.requested_timeout_ms);
      validated->display_name = std::string_view {
        validated->request.display_name,
        std::min(display_name.size(), static_cast<std::size_t>(kDisplayNameChars))
      };
    }

    return ValidationError::None;
  }

  ValidationError validate_lease_display_request(const LeaseDisplayRequest &request) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.lease_id == 0) {
      return ValidationError::MissingLeaseId;
    }
    if (request.display_id == 0) {
      return ValidationError::MissingDisplayId;
    }

    return ValidationError::None;
  }

  ValidationError validate_lease_request(const LeaseRequest &request) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.lease_id == 0) {
      return ValidationError::MissingLeaseId;
    }

    return ValidationError::None;
  }

  ValidationError validate_permanent_display_count(
    const PermanentDisplayCountRequest &request,
    const std::uint32_t max_display_count
  ) {
    if (!is_valid_api_namespace(request.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (request.display_count > max_display_count) {
      return ValidationError::PermanentDisplayCountTooHigh;
    }
    if (request.width < kMinWidth || request.width > kMaxWidth) {
      return ValidationError::InvalidWidth;
    }
    if (request.height < kMinHeight || request.height > kMaxHeight) {
      return ValidationError::InvalidHeight;
    }
    if (request.physical_width_mm < kMinPhysicalSizeMillimeters ||
        request.physical_width_mm > kMaxPhysicalSizeMillimeters ||
        request.physical_height_mm < kMinPhysicalSizeMillimeters ||
        request.physical_height_mm > kMaxPhysicalSizeMillimeters) {
      return ValidationError::InvalidPhysicalSize;
    }
    if (request.refresh_rate_millihz < kMinRefreshRateMilliHz ||
        request.refresh_rate_millihz > kMaxRefreshRateMilliHz) {
      return ValidationError::InvalidRefreshRate;
    }
    if (trim_display_name(request.display_name).empty()) {
      return ValidationError::InvalidDisplayName;
    }

    return ValidationError::None;
  }

  ValidationError validate_display_manifest(
    const DisplayManifest &manifest,
    const std::uint32_t max_display_count
  ) {
    if (!is_valid_api_namespace(manifest.api_namespace)) {
      return ValidationError::WrongApiNamespace;
    }
    if (manifest.version != kDisplayManifestVersion) {
      return ValidationError::InvalidManifestVersion;
    }
    if (manifest.profile_count > max_display_count ||
        manifest.profile_count > kMaxPermanentDisplayProfiles) {
      return ValidationError::PermanentDisplayCountTooHigh;
    }
    if (manifest.max_profile_count < manifest.profile_count ||
        manifest.max_profile_count > max_display_count ||
        manifest.max_profile_count > kMaxPermanentDisplayProfiles) {
      return ValidationError::PermanentDisplayCountTooHigh;
    }

    std::array<bool, kMaxPermanentDisplayProfiles> used_connectors {};
    for (std::uint32_t index = 0; index < manifest.profile_count; ++index) {
      const auto &profile = manifest.profiles[index];
      if ((profile.flags & ~kDisplayManifestProfileKnownFlags) != 0) {
        return ValidationError::InvalidFlags;
      }
      if ((profile.flags & kDisplayManifestProfileFlagPermanentIdentity) == 0) {
        return ValidationError::InvalidFlags;
      }
      if (profile.connector_index >= max_display_count ||
          profile.connector_index >= kMaxPermanentDisplayProfiles ||
          used_connectors[profile.connector_index]) {
        return ValidationError::InvalidConnectorIndex;
      }
      used_connectors[profile.connector_index] = true;

      if (profile.display_id == 0) {
        return ValidationError::MissingDisplayId;
      }
      if (!valid_manufacturer_id(profile.manufacturer_id)) {
        return ValidationError::InvalidManufacturerId;
      }
      if (profile.physical_width_mm < kMinPhysicalSizeMillimeters ||
          profile.physical_width_mm > kMaxPhysicalSizeMillimeters ||
          profile.physical_height_mm < kMinPhysicalSizeMillimeters ||
          profile.physical_height_mm > kMaxPhysicalSizeMillimeters) {
        return ValidationError::InvalidPhysicalSize;
      }
      if (profile.allowed_mode_count == 0 ||
          profile.allowed_mode_count > kMaxAllowedModesPerProfile ||
          profile.native_mode_index >= profile.allowed_mode_count) {
        return ValidationError::InvalidModeCount;
      }
      if (profile.layout_policy > kDisplayManifestLayoutPolicyApplyAndPersist) {
        return ValidationError::InvalidLayoutPolicy;
      }

      for (std::uint32_t mode_index = 0; mode_index < profile.allowed_mode_count; ++mode_index) {
        const auto &mode = profile.allowed_modes[mode_index];
        if (mode.width < kMinWidth || mode.width > kMaxWidth) {
          return ValidationError::InvalidWidth;
        }
        if (mode.height < kMinHeight || mode.height > kMaxHeight) {
          return ValidationError::InvalidHeight;
        }
        if (mode.refresh_rate_millihz < kMinRefreshRateMilliHz ||
            mode.refresh_rate_millihz > kMaxRefreshRateMilliHz) {
          return ValidationError::InvalidRefreshRate;
        }
      }

      if (trim_display_name(profile.display_name).empty()) {
        return ValidationError::InvalidDisplayName;
      }
    }

    return ValidationError::None;
  }

  const char *to_string(const ValidationError error) {
    switch (error) {
      case ValidationError::None:
        return "none";
      case ValidationError::WrongApiNamespace:
        return "wrong_api_namespace";
      case ValidationError::MissingLeaseId:
        return "missing_lease_id";
      case ValidationError::MissingDisplayId:
        return "missing_display_id";
      case ValidationError::InvalidFlags:
        return "invalid_flags";
      case ValidationError::InvalidWidth:
        return "invalid_width";
      case ValidationError::InvalidHeight:
        return "invalid_height";
      case ValidationError::InvalidPhysicalSize:
        return "invalid_physical_size";
      case ValidationError::InvalidRefreshRate:
        return "invalid_refresh_rate";
      case ValidationError::InvalidDisplayName:
        return "invalid_display_name";
      case ValidationError::PermanentDisplayCountTooHigh:
        return "permanent_display_count_too_high";
      case ValidationError::InvalidManifestVersion:
        return "invalid_manifest_version";
      case ValidationError::InvalidConnectorIndex:
        return "invalid_connector_index";
      case ValidationError::InvalidModeCount:
        return "invalid_mode_count";
      case ValidationError::InvalidLayoutPolicy:
        return "invalid_layout_policy";
      case ValidationError::InvalidManufacturerId:
        return "invalid_manufacturer_id";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
