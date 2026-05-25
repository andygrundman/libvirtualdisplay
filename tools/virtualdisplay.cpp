#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

#include <algorithm>
#include <charconv>
#include <cwchar>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <NewDev.h>
#include <SetupAPI.h>
#include <Shellapi.h>
#endif

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::string_view kDriverInstallCommand {"driver"};
  constexpr std::string_view kDriverInstallSubcommand {"install"};

  void print_usage() {
    std::cout
      << "virtualdisplay commands:\n"
      << "  driver install [--inf PATH]\n"
      << "  status\n"
      << "  spawn [--width N] [--height N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent query\n"
      << "  permanent set --count N [--width N] [--height N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent off\n";
  }

  int fail(const std::string &message, const vdd::ControlOperationResult &result) {
    std::cerr << message << ": " << vdd::to_string(result.status);
    if (result.native_error != 0) {
      std::cerr << " native_error=" << result.native_error;
    }
    std::cerr << '\n';
    return 1;
  }

#ifdef _WIN32
  struct DevInfoSet {
    HDEVINFO value {INVALID_HANDLE_VALUE};

    explicit DevInfoSet(const HDEVINFO handle):
        value {handle} {
    }

    ~DevInfoSet() {
      if (value != INVALID_HANDLE_VALUE) {
        SetupDiDestroyDeviceInfoList(value);
      }
    }

    DevInfoSet(const DevInfoSet &) = delete;
    DevInfoSet &operator=(const DevInfoSet &) = delete;
  };

  std::wstring widen(const std::string_view value) {
    if (value.empty()) {
      return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) {
      return {};
    }

    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), output.data(), size);
    return output;
  }

  std::wstring quote_argument(const std::wstring &argument) {
    std::wstring quoted {L"\""};
    for (wchar_t ch : argument) {
      if (ch == L'"') {
        quoted += L'\\';
      }
      quoted += ch;
    }
    quoted += L'"';
    return quoted;
  }

  bool is_process_elevated() {
    HANDLE token {};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
      return false;
    }

    TOKEN_ELEVATION elevation {};
    DWORD returned {};
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &returned);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
  }

  int relaunch_elevated(const std::vector<std::string> &args) {
    wchar_t executable[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
      std::cerr << "failed to resolve executable path for elevation native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::wostringstream parameters;
    for (const auto &arg : args) {
      if (parameters.tellp() > 0) {
        parameters << L' ';
      }
      parameters << quote_argument(widen(arg));
    }

    SHELLEXECUTEINFOW execute {};
    execute.cbSize = sizeof(execute);
    execute.fMask = SEE_MASK_NOCLOSEPROCESS;
    execute.lpVerb = L"runas";
    execute.lpFile = executable;
    const auto parameter_text = parameters.str();
    execute.lpParameters = parameter_text.c_str();
    execute.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&execute)) {
      std::cerr << "elevation failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    WaitForSingleObject(execute.hProcess, INFINITE);
    DWORD exit_code {};
    if (!GetExitCodeProcess(execute.hProcess, &exit_code)) {
      std::cerr << "elevated process finished, but exit code was unavailable native_error=" << GetLastError() << '\n';
      CloseHandle(execute.hProcess);
      return 1;
    }
    CloseHandle(execute.hProcess);
    return static_cast<int>(exit_code);
  }

  std::filesystem::path default_driver_inf_path() {
    wchar_t executable[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
      return {};
    }

    const auto tool_dir = std::filesystem::path {executable}.parent_path();
    const std::vector<std::filesystem::path> candidates {
      tool_dir.parent_path() / "driver" / "SunshineVirtualDisplayDriver.inf",
      tool_dir / "windows_driver" / "SunshineVirtualDisplayDriver.inf",
      tool_dir / "SunshineVirtualDisplayDriver.inf"
    };

    for (const auto &candidate : candidates) {
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
    }

    return candidates.front();
  }

  std::optional<std::filesystem::path> parse_driver_inf_path(const std::vector<std::string> &args) {
    auto inf_path = default_driver_inf_path();

    for (std::size_t index = 2; index < args.size(); ++index) {
      const auto &arg = args[index];
      if (arg != "--inf") {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }

      if (index + 1 >= args.size()) {
        std::cerr << "--inf requires a value\n";
        return std::nullopt;
      }
      inf_path = std::filesystem::absolute(args[++index]);
    }

    if (inf_path.empty()) {
      std::cerr << "failed to resolve default driver INF path\n";
      return std::nullopt;
    }
    return inf_path;
  }

  bool multi_sz_contains(const wchar_t *values, const std::wstring_view expected) {
    for (const wchar_t *cursor = values; cursor && *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
      if (expected == cursor) {
        return true;
      }
    }
    return false;
  }

  bool root_device_exists(std::uint32_t &native_error) {
    DevInfoSet devices {SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES)};
    if (devices.value == INVALID_HANDLE_VALUE) {
      native_error = GetLastError();
      return false;
    }

    SP_DEVINFO_DATA device_info {};
    device_info.cbSize = sizeof(device_info);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(devices.value, index, &device_info); ++index) {
      DWORD property_type {};
      DWORD required_size {};
      (void) SetupDiGetDeviceRegistryPropertyW(
        devices.value,
        &device_info,
        SPDRP_HARDWAREID,
        &property_type,
        nullptr,
        0,
        &required_size
      );

      if (required_size == 0 || property_type != REG_MULTI_SZ) {
        continue;
      }

      std::vector<std::byte> buffer(required_size);
      if (!SetupDiGetDeviceRegistryPropertyW(
            devices.value,
            &device_info,
            SPDRP_HARDWAREID,
            &property_type,
            reinterpret_cast<PBYTE>(buffer.data()),
            static_cast<DWORD>(buffer.size()),
            nullptr
          )) {
        continue;
      }

      if (multi_sz_contains(reinterpret_cast<const wchar_t *>(buffer.data()), L"Root\\SunshineVirtualDisplay")) {
        native_error = ERROR_SUCCESS;
        return true;
      }
    }

    native_error = GetLastError();
    if (native_error == ERROR_NO_MORE_ITEMS) {
      native_error = ERROR_FILE_NOT_FOUND;
    }
    return false;
  }

  bool create_root_device(std::uint32_t &native_error) {
    if (root_device_exists(native_error)) {
      native_error = ERROR_SUCCESS;
      return true;
    }

    if (native_error != ERROR_FILE_NOT_FOUND) {
      return false;
    }

    const GUID display_class_guid {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x02, 0xbe, 0x10, 0x31, 0x8}};
    DevInfoSet devices {SetupDiCreateDeviceInfoList(&display_class_guid, nullptr)};
    if (devices.value == INVALID_HANDLE_VALUE) {
      native_error = GetLastError();
      return false;
    }

    SP_DEVINFO_DATA device_info {};
    device_info.cbSize = sizeof(device_info);
    if (!SetupDiCreateDeviceInfoW(
          devices.value,
          L"SunshineVirtualDisplay",
          &display_class_guid,
          nullptr,
          nullptr,
          DICD_GENERATE_ID,
          &device_info
        )) {
      native_error = GetLastError();
      return false;
    }

    const wchar_t hardware_id[] = L"Root\\SunshineVirtualDisplay\0";
    if (!SetupDiSetDeviceRegistryPropertyW(
          devices.value,
          &device_info,
          SPDRP_HARDWAREID,
          reinterpret_cast<const BYTE *>(hardware_id),
          sizeof(hardware_id)
        )) {
      native_error = GetLastError();
      return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devices.value, &device_info)) {
      native_error = GetLastError();
      return false;
    }

    native_error = ERROR_SUCCESS;
    return true;
  }

  int install_driver(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    const auto inf_path = parse_driver_inf_path(args);
    if (!inf_path) {
      return 2;
    }

    if (!std::filesystem::exists(*inf_path)) {
      std::cerr << "driver INF not found: " << inf_path->string() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    (void) create_root_device(native_error);
    if (native_error != ERROR_SUCCESS && native_error != ERROR_DEVINST_ALREADY_EXISTS) {
      std::cerr << "create root device failed native_error=" << native_error << '\n';
      return 1;
    }

    BOOL reboot_required = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(
          nullptr,
          L"Root\\SunshineVirtualDisplay",
          inf_path->wstring().c_str(),
          INSTALLFLAG_FORCE,
          &reboot_required
        )) {
      std::cerr << "driver install failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::cout << "driver_installed=1\n";
    std::cout << "reboot_required=" << (reboot_required ? 1 : 0) << '\n';
    return reboot_required ? 3 : 0;
  }
#else
  int install_driver(const std::vector<std::string> &) {
    std::cerr << "driver install is only supported on Windows\n";
    return 2;
  }
#endif

  template<class T>
  int fail(const std::string &message, const vdd::ControlResult<T> &result) {
    return fail(message, {result.status, result.native_error});
  }

  bool parse_u32(const std::string_view value, std::uint32_t &parsed) {
    std::uint32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool parse_refresh_millihz(const std::string_view value, std::uint32_t &parsed) {
    std::string text {value};
    try {
      const auto refresh = std::stod(text);
      if (refresh <= 0.0) {
        return false;
      }
      parsed = static_cast<std::uint32_t>(refresh <= 1000.0 ? refresh * 1000.0 : refresh);
      return true;
    } catch (...) {
      return false;
    }
  }

  void set_display_name(char (&target)[vdd::kDisplayNameChars], const std::string &name) {
    std::fill(std::begin(target), std::end(target), '\0');
    std::memcpy(target, name.data(), (std::min)(name.size(), static_cast<std::size_t>(vdd::kDisplayNameChars - 1)));
  }

  std::string display_name(const char (&value)[vdd::kDisplayNameChars]) {
    return std::string {vdd::trim_display_name(value)};
  }

  void print_permanent_state(const vdd::PermanentDisplayCountResult &state) {
    std::cout
      << "permanent_displays=" << state.current_display_count << '\n'
      << "max_permanent_displays=" << state.max_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "mode=" << state.width << 'x' << state.height << '@'
      << (state.refresh_rate_millihz / 1000.0) << "Hz\n"
      << "name=" << display_name(state.display_name) << '\n';
  }

  struct PermanentOptions {
    std::uint32_t count {1};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t refresh_rate_millihz {60'000};
    std::string name {"Sunshine Display"};
  };

  std::optional<PermanentOptions> parse_permanent_options(
    const std::vector<std::string> &args,
    const std::size_t first,
    const bool require_count
  ) {
    PermanentOptions options {};
    bool saw_count = false;

    for (std::size_t index = first; index < args.size(); ++index) {
      const auto &arg = args[index];
      const auto need_value = [&]() -> std::optional<std::string> {
        if (index + 1 >= args.size()) {
          std::cerr << arg << " requires a value\n";
          return std::nullopt;
        }
        return args[++index];
      };

      if (arg == "--count") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.count)) {
          std::cerr << "invalid --count value\n";
          return std::nullopt;
        }
        saw_count = true;
      } else if (arg == "--width") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.width)) {
          std::cerr << "invalid --width value\n";
          return std::nullopt;
        }
      } else if (arg == "--height") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.height)) {
          std::cerr << "invalid --height value\n";
          return std::nullopt;
        }
      } else if (arg == "--refresh") {
        const auto value = need_value();
        if (!value || !parse_refresh_millihz(*value, options.refresh_rate_millihz)) {
          std::cerr << "invalid --refresh value\n";
          return std::nullopt;
        }
      } else if (arg == "--name") {
        const auto value = need_value();
        if (!value || value->empty()) {
          std::cerr << "invalid --name value\n";
          return std::nullopt;
        }
        options.name = *value;
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }
    }

    if (require_count && !saw_count) {
      std::cerr << "permanent set requires --count\n";
      return std::nullopt;
    }
    return options;
  }

  vdd::PermanentDisplayCountRequest make_request(const PermanentOptions &options) {
    vdd::PermanentDisplayCountRequest request {};
    request.display_count = options.count;
    request.width = options.width;
    request.height = options.height;
    request.refresh_rate_millihz = options.refresh_rate_millihz;
    set_display_name(request.display_name, options.name);
    return request;
  }

  int set_permanent(vdd::ControlClient &client, const PermanentOptions &options) {
    auto request = make_request(options);
    const auto validation = vdd::validate_permanent_display_count(request, 64);
    if (validation != vdd::ValidationError::None && validation != vdd::ValidationError::PermanentDisplayCountTooHigh) {
      std::cerr << "invalid display settings: " << vdd::to_string(validation) << '\n';
      return 2;
    }

    const auto result = client.set_permanent_display_count(request);
    if (!result.ok()) {
      return fail("set permanent display failed", result);
    }

    print_permanent_state(result.value);
    return 0;
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc < 2 || std::string_view {argv[1]} == "--help" || std::string_view {argv[1]} == "help") {
    print_usage();
    return argc < 2 ? 2 : 0;
  }

  const std::vector<std::string> args {argv + 1, argv + argc};
  if (args[0] == kDriverInstallCommand) {
    if (args.size() >= 2 && args[1] == kDriverInstallSubcommand) {
      return install_driver(args);
    }

    print_usage();
    return 2;
  }

  const auto opened = vdd::open_first_control_device();
  if (!opened.ok()) {
    return fail("open control device failed", {opened.status, opened.native_error});
  }

  vdd::ControlClient client {*opened.transport};
  const auto protocol = client.check_protocol_compatible();
  if (!protocol.ok()) {
    return fail("control protocol check failed", protocol);
  }

  if (args[0] == "status" || (args[0] == "permanent" && args.size() >= 2 && args[1] == "query")) {
    const auto result = client.query_permanent_display_count();
    if (!result.ok()) {
      return fail("query permanent display failed", result);
    }
    print_permanent_state(result.value);
    return 0;
  }

  if (args[0] == "spawn") {
    const auto options = parse_permanent_options(args, 1, false);
    if (!options) {
      return 2;
    }
    return set_permanent(client, *options);
  }

  if (args[0] == "permanent" && args.size() >= 2 && args[1] == "off") {
    PermanentOptions options {};
    options.count = 0;
    return set_permanent(client, options);
  }

  if (args[0] == "permanent" && args.size() >= 2 && args[1] == "set") {
    const auto options = parse_permanent_options(args, 2, true);
    if (!options) {
      return 2;
    }
    return set_permanent(client, *options);
  }

  print_usage();
  return 2;
}
