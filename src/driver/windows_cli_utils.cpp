#include "virtual_display/driver/windows_cli_utils.h"

namespace virtual_display::driver {
  std::wstring quote_windows_command_argument(const std::wstring_view argument) {
    std::wstring quoted {L"\""};
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
      if (ch == L'"') {
        quoted.append(backslashes * 2 + 1, L'\\');
        quoted += ch;
        backslashes = 0;
        continue;
      }
      if (ch == L'\\') {
        ++backslashes;
        continue;
      }
      if (backslashes != 0) {
        quoted.append(backslashes, L'\\');
        backslashes = 0;
      }
      quoted += ch;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
  }

  DriverInstallInfPathResult parse_driver_install_inf_path(
    const std::span<const std::string> args,
    const std::filesystem::path &default_inf_path
  ) {
    auto inf_path = default_inf_path;

    for (std::size_t index = 2; index < args.size(); ++index) {
      const auto &arg = args[index];
      if (arg != "--inf") {
        return {DriverInstallInfPathStatus::UnknownOption, {}, arg};
      }

      if (index + 1 >= args.size()) {
        return {DriverInstallInfPathStatus::MissingInfValue, {}, arg};
      }
      inf_path = std::filesystem::absolute(args[++index]);
    }

    if (inf_path.empty()) {
      return {DriverInstallInfPathStatus::EmptyDefaultPath, {}, {}};
    }
    return {DriverInstallInfPathStatus::Ok, inf_path, {}};
  }
}  // namespace virtual_display::driver
