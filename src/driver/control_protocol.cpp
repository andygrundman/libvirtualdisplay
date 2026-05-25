#include "virtual_display/driver/control_protocol.h"

#include <algorithm>

namespace virtual_display::driver {
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
    if (request.width < kMinWidth || request.width > kMaxWidth) {
      return ValidationError::InvalidWidth;
    }
    if (request.height < kMinHeight || request.height > kMaxHeight) {
      return ValidationError::InvalidHeight;
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
    if (request.refresh_rate_millihz < kMinRefreshRateMilliHz ||
        request.refresh_rate_millihz > kMaxRefreshRateMilliHz) {
      return ValidationError::InvalidRefreshRate;
    }
    if (trim_display_name(request.display_name).empty()) {
      return ValidationError::InvalidDisplayName;
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
      case ValidationError::InvalidWidth:
        return "invalid_width";
      case ValidationError::InvalidHeight:
        return "invalid_height";
      case ValidationError::InvalidRefreshRate:
        return "invalid_refresh_rate";
      case ValidationError::InvalidDisplayName:
        return "invalid_display_name";
      case ValidationError::PermanentDisplayCountTooHigh:
        return "permanent_display_count_too_high";
    }

    return "unknown";
  }
}  // namespace virtual_display::driver
