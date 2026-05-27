#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace virtual_display::driver {
  enum class DriverInstallInfPathStatus {
    Ok,
    UnknownOption,
    MissingInfValue,
    InvalidInfPath,
    EmptyDefaultPath,
  };

  struct DriverInstallInfPathResult {
    DriverInstallInfPathStatus status {DriverInstallInfPathStatus::Ok};
    std::filesystem::path inf_path;
    std::string option;
  };

  std::wstring quote_windows_command_argument(std::wstring_view argument);
  std::wstring build_windows_command_parameters(std::span<const std::wstring> arguments);

  DriverInstallInfPathResult parse_driver_install_inf_path(
    std::span<const std::string> args,
    const std::filesystem::path &default_inf_path
  );
}  // namespace virtual_display::driver
