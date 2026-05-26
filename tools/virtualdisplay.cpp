#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
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
#include <Aclapi.h>
#include <NewDev.h>
#include <SetupAPI.h>
#include <Shellapi.h>
#include <sddl.h>
#endif

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::string_view kDriverInstallCommand {"driver"};
  constexpr std::string_view kDriverInstallSubcommand {"install"};

  void print_usage() {
    std::cout
      << "virtualdisplay commands:\n"
      << "  driver install [--inf PATH]\n"
      << "  broker install|start|stop|status|uninstall\n"
      << "  broker protocol|query-state|query-manifest|helper-diagnose|helper-apply-extended-topology|helper-apply-manifest-topology|helper-query-color-profiles\n"
      << "  broker helper-associate-color-profile <source_luid high:low> <source_id> <profile> [--advanced-color] [--default]\n"
      << "  status\n"
      << "  display query\n"
      << "  spawn [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent query\n"
      << "  permanent set --count N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent profile set --slot N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT] [--layout none|apply|persist] [--x N] [--y N] [--hdr 0|1]\n"
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
  constexpr wchar_t kBrokerPipeName[] = L"\\\\.\\pipe\\SunshineVirtualDisplayBroker";
  constexpr wchar_t kBrokerServiceName[] = L"SunshineVirtualDisplayBroker";
  constexpr wchar_t kBrokerServiceDisplayName[] = L"Sunshine Virtual Display Broker";
  constexpr wchar_t kBrokerServiceSecurityDescriptor[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
  constexpr wchar_t kBrokerServiceRequiredPrivileges[] =
    L"SeAssignPrimaryTokenPrivilege\0"
    L"SeIncreaseQuotaPrivilege\0"
    L"SeTcbPrivilege\0";

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

  struct ServiceHandle {
    SC_HANDLE value {};

    explicit ServiceHandle(const SC_HANDLE handle):
        value {handle} {
    }

    ~ServiceHandle() {
      if (value) {
        CloseServiceHandle(value);
      }
    }

    ServiceHandle(const ServiceHandle &) = delete;
    ServiceHandle &operator=(const ServiceHandle &) = delete;
  };

  struct LocalMemory {
    HLOCAL value {};

    explicit LocalMemory(const HLOCAL handle):
        value {handle} {
    }

    ~LocalMemory() {
      if (value) {
        LocalFree(value);
      }
    }

    void *get() const {
      return value;
    }

    LocalMemory(const LocalMemory &) = delete;
    LocalMemory &operator=(const LocalMemory &) = delete;
  };

  struct BrokerResponse {
    bool pipe_available {};
    bool ok {};
    std::string text;
    std::uint32_t native_error {};
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

  const char *service_state_name(const DWORD state) {
    switch (state) {
      case SERVICE_STOPPED:
        return "stopped";
      case SERVICE_START_PENDING:
        return "start_pending";
      case SERVICE_STOP_PENDING:
        return "stop_pending";
      case SERVICE_RUNNING:
        return "running";
      case SERVICE_CONTINUE_PENDING:
        return "continue_pending";
      case SERVICE_PAUSE_PENDING:
        return "pause_pending";
      case SERVICE_PAUSED:
        return "paused";
      default:
        return "unknown";
    }
  }

  std::filesystem::path broker_executable_path() {
    wchar_t executable[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    if (length == 0 || length >= std::size(executable)) {
      return {};
    }
    return std::filesystem::path {executable}.parent_path() / "virtualdisplay_broker.exe";
  }

  std::optional<SERVICE_STATUS_PROCESS> query_broker_service_status(
    SC_HANDLE service,
    std::uint32_t &native_error
  ) {
    SERVICE_STATUS_PROCESS status {};
    DWORD bytes_needed {};
    if (!QueryServiceStatusEx(
          service,
          SC_STATUS_PROCESS_INFO,
          reinterpret_cast<LPBYTE>(&status),
          sizeof(status),
          &bytes_needed
        )) {
      native_error = GetLastError();
      return std::nullopt;
    }
    native_error = ERROR_SUCCESS;
    return status;
  }

  int print_broker_service_status(SC_HANDLE service) {
    std::uint32_t native_error {};
    const auto status = query_broker_service_status(service, native_error);
    if (!status) {
      std::cerr << "query broker service failed native_error=" << native_error << '\n';
      return 1;
    }

    std::cout << "broker_service_state=" << service_state_name(status->dwCurrentState) << '\n';
    std::cout << "broker_service_state_code=" << status->dwCurrentState << '\n';
    return 0;
  }

  bool harden_broker_service(SC_HANDLE service) {
    SERVICE_SID_INFO sid_info {};
    sid_info.dwServiceSidType = SERVICE_SID_TYPE_UNRESTRICTED;
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_SERVICE_SID_INFO, &sid_info)) {
      std::cerr << "configure broker service SID failed native_error=" << GetLastError() << '\n';
      return false;
    }

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kBrokerServiceSecurityDescriptor,
          SDDL_REVISION_1,
          &descriptor,
          nullptr
        )) {
      std::cerr << "build broker service security failed native_error=" << GetLastError() << '\n';
      return false;
    }

    const LocalMemory security_descriptor {descriptor};
    if (!SetServiceObjectSecurity(service, DACL_SECURITY_INFORMATION, security_descriptor.get())) {
      std::cerr << "apply broker service security failed native_error=" << GetLastError() << '\n';
      return false;
    }

    SERVICE_REQUIRED_PRIVILEGES_INFOW privileges {};
    privileges.pmszRequiredPrivileges = const_cast<LPWSTR>(kBrokerServiceRequiredPrivileges);
    if (!ChangeServiceConfig2W(service, SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO, &privileges)) {
      std::cerr << "configure broker service privileges failed native_error=" << GetLastError() << '\n';
      return false;
    }

    return true;
  }

  int install_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    const auto broker_path = broker_executable_path();
    if (broker_path.empty() || !std::filesystem::exists(broker_path)) {
      std::cerr << "broker executable not found: " << broker_path.string() << '\n';
      return 1;
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    const std::wstring command_line = quote_argument(broker_path.wstring()) + L" --service";
    SC_HANDLE service_handle = CreateServiceW(
      manager.value,
      kBrokerServiceName,
      kBrokerServiceDisplayName,
      SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START,
      SERVICE_WIN32_OWN_PROCESS,
      SERVICE_AUTO_START,
      SERVICE_ERROR_NORMAL,
      command_line.c_str(),
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr
    );

    bool updated = false;
    if (!service_handle && GetLastError() == ERROR_SERVICE_EXISTS) {
      service_handle = OpenServiceW(manager.value, kBrokerServiceName, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS);
      if (!service_handle) {
        std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
        return 1;
      }
      ServiceHandle service {service_handle};
      if (!ChangeServiceConfigW(
            service.value,
            SERVICE_NO_CHANGE,
            SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL,
            command_line.c_str(),
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            kBrokerServiceDisplayName
          )) {
        std::cerr << "update broker service failed native_error=" << GetLastError() << '\n';
        return 1;
      }
      if (!harden_broker_service(service.value)) {
        return 1;
      }
      updated = true;
      std::cout << "broker_service_installed=1\n";
      std::cout << "broker_service_updated=1\n";
      return print_broker_service_status(service.value);
    }

    if (!service_handle) {
      std::cerr << "create broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {service_handle};
    if (!harden_broker_service(service.value)) {
      return 1;
    }
    std::cout << "broker_service_installed=1\n";
    std::cout << "broker_service_updated=" << (updated ? 1 : 0) << '\n';
    return print_broker_service_status(service.value);
  }

  int start_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, SERVICE_QUERY_STATUS | SERVICE_START)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    const auto before = query_broker_service_status(service.value, native_error);
    if (!before) {
      std::cerr << "query broker service failed native_error=" << native_error << '\n';
      return 1;
    }
    if (before->dwCurrentState != SERVICE_RUNNING && !StartServiceW(service.value, 0, nullptr)) {
      const auto error = GetLastError();
      if (error != ERROR_SERVICE_ALREADY_RUNNING) {
        std::cerr << "start broker service failed native_error=" << error << '\n';
        return 1;
      }
    }

    std::cout << "broker_service_started=1\n";
    return print_broker_service_status(service.value);
  }

  int stop_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, SERVICE_QUERY_STATUS | SERVICE_STOP)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    const auto before = query_broker_service_status(service.value, native_error);
    if (!before) {
      std::cerr << "query broker service failed native_error=" << native_error << '\n';
      return 1;
    }
    if (before->dwCurrentState != SERVICE_STOPPED) {
      SERVICE_STATUS status {};
      if (!ControlService(service.value, SERVICE_CONTROL_STOP, &status)) {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE) {
          std::cerr << "stop broker service failed native_error=" << error << '\n';
          return 1;
        }
      }
    }

    std::cout << "broker_service_stopped=1\n";
    return print_broker_service_status(service.value);
  }

  int uninstall_broker_service(const std::vector<std::string> &args) {
    if (!is_process_elevated()) {
      return relaunch_elevated(args);
    }

    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, DELETE | SERVICE_QUERY_STATUS | SERVICE_STOP)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::uint32_t native_error {};
    const auto before = query_broker_service_status(service.value, native_error);
    if (before && before->dwCurrentState != SERVICE_STOPPED) {
      SERVICE_STATUS status {};
      (void) ControlService(service.value, SERVICE_CONTROL_STOP, &status);
    }

    if (!DeleteService(service.value)) {
      std::cerr << "delete broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    std::cout << "broker_service_uninstalled=1\n";
    return 0;
  }

  int query_broker_service() {
    ServiceHandle manager {OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) {
      std::cerr << "open service manager failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    ServiceHandle service {OpenServiceW(manager.value, kBrokerServiceName, SERVICE_QUERY_STATUS)};
    if (!service.value) {
      std::cerr << "open broker service failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    return print_broker_service_status(service.value);
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

  BrokerResponse request_broker(const std::string_view command) {
    HANDLE pipe = CreateFileW(
      kBrokerPipeName,
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr
    );
    if (pipe == INVALID_HANDLE_VALUE) {
      return {false, false, {}, GetLastError()};
    }

    DWORD bytes_written = 0;
    if (!WriteFile(
          pipe,
          command.data(),
          static_cast<DWORD>(command.size()),
          &bytes_written,
          nullptr
        )) {
      const auto error = GetLastError();
      CloseHandle(pipe);
      return {true, false, "error write_failed\n", error};
    }

    std::array<char, 4096> response {};
    DWORD bytes_read = 0;
    if (!ReadFile(
          pipe,
          response.data(),
          static_cast<DWORD>(response.size() - 1),
          &bytes_read,
          nullptr
        )) {
      const auto error = GetLastError();
      CloseHandle(pipe);
      return {true, false, "error read_failed\n", error};
    }

    response[(std::min<DWORD>)(bytes_read, static_cast<DWORD>(response.size() - 1))] = '\0';
    CloseHandle(pipe);
    const std::string text {response.data()};
    return {true, text.starts_with("ok ") || text.starts_with("ok\n"), text, ERROR_SUCCESS};
  }

  int query_broker(const std::string_view command) {
    const auto response = request_broker(command);
    if (!response.pipe_available) {
      std::cerr << "open broker pipe failed native_error=" << response.native_error << '\n';
      return 1;
    }
    std::cout << response.text;
    return response.ok ? 0 : 1;
  }

  std::string broker_payload(const std::string &response) {
    if (response.starts_with("ok\n")) {
      return response.substr(3);
    }
    if (response.starts_with("ok ")) {
      return response.substr(3);
    }
    return response;
  }

  int require_broker_command(const std::string_view command, const bool print_payload) {
    const auto response = request_broker(command);
    if (!response.pipe_available) {
      std::cerr << "open broker pipe failed native_error=" << response.native_error << '\n';
      std::cerr << "start the broker service before using display management commands\n";
      return 1;
    }
    if (response.ok) {
      std::cout << (print_payload ? broker_payload(response.text) : response.text);
      return 0;
    }

    std::cerr << response.text;
    return 1;
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

  bool parse_i32(const std::string_view value, std::int32_t &parsed) {
    std::int32_t output {};
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

  std::string guid_string(const vdd::Guid &guid) {
    char text[37] {};
    std::snprintf(
      text,
      sizeof(text),
      "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      guid.data1,
      guid.data2,
      guid.data3,
      guid.data4[0],
      guid.data4[1],
      guid.data4[2],
      guid.data4[3],
      guid.data4[4],
      guid.data4[5],
      guid.data4[6],
      guid.data4[7]
    );
    return text;
  }

  const char *display_kind(const std::uint32_t kind) {
    if (kind == vdd::kDisplayStateKindPermanent) {
      return "permanent";
    }
    if (kind == vdd::kDisplayStateKindTemporary) {
      return "temporary";
    }
    return "unknown";
  }

  void print_permanent_state(const vdd::PermanentDisplayCountResult &state) {
    std::cout
      << "permanent_displays=" << state.current_display_count << '\n'
      << "max_permanent_displays=" << state.max_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "mode=" << state.width << 'x' << state.height << '@'
      << (state.refresh_rate_millihz / 1000.0) << "Hz\n"
      << "physical_size_mm=" << state.physical_width_mm << 'x' << state.physical_height_mm << '\n'
      << "name=" << display_name(state.display_name) << '\n';
  }

  void print_display_state(const vdd::QueryDisplayStateResult &state) {
    std::cout
      << "permanent_displays=" << state.permanent_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "display_entries=" << state.entry_count << '\n';

    for (std::uint32_t index = 0; index < state.entry_count && index < vdd::kMaxDisplayStateEntries; ++index) {
      const auto &entry = state.entries[index];
      std::cout
        << "display." << index << ".kind=" << display_kind(entry.kind) << '\n'
        << "display." << index << ".display_id=" << entry.display_id << '\n'
        << "display." << index << ".lease_id=" << entry.lease_id << '\n'
        << "display." << index << ".connector_index=" << entry.connector_index << '\n'
        << "display." << index << ".container_id=" << guid_string(entry.container_id) << '\n'
        << "display." << index << ".product_code=" << entry.product_code << '\n'
        << "display." << index << ".serial_number=" << entry.serial_number << '\n'
        << "display." << index << ".mode=" << entry.width << 'x' << entry.height << '@'
        << (entry.refresh_rate_millihz / 1000.0) << "Hz\n"
        << "display." << index << ".physical_size_mm=" << entry.physical_width_mm << 'x'
        << entry.physical_height_mm << '\n'
        << "display." << index << ".hdr_supported="
        << ((entry.flags & vdd::kDisplayStateFlagHdrSupported) ? 1 : 0) << '\n'
        << "display." << index << ".retain_identity="
        << ((entry.flags & vdd::kDisplayStateFlagRetainIdentity) ? 1 : 0) << '\n'
        << "display." << index << ".name=" << display_name(entry.display_name) << '\n';
    }
  }

  struct PermanentOptions {
    std::uint32_t count {1};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t physical_width_mm {vdd::kDefaultPhysicalWidthMillimeters};
    std::uint32_t physical_height_mm {vdd::kDefaultPhysicalHeightMillimeters};
    std::uint32_t refresh_rate_millihz {60'000};
    std::string name {"Sunshine Display"};
  };

  struct ProfileOptions: PermanentOptions {
    std::uint32_t slot {};
    std::uint32_t hdr_supported {1};
    std::uint32_t layout_policy {vdd::kDisplayManifestLayoutPolicyNone};
    std::int32_t position_x {};
    std::int32_t position_y {};
  };

#ifdef _WIN32
  std::string permanent_set_broker_command(const PermanentOptions &options) {
    std::ostringstream command;
    command
      << "permanent-set " << options.count
      << ' ' << options.width
      << ' ' << options.height
      << ' ' << options.physical_width_mm
      << ' ' << options.physical_height_mm
      << ' ' << options.refresh_rate_millihz
      << ' ' << options.name;
    return command.str();
  }

  std::string manifest_profile_set_broker_command(const ProfileOptions &options) {
    std::ostringstream command;
    command
      << "manifest-profile-set " << options.slot
      << ' ' << options.width
      << ' ' << options.height
      << ' ' << options.physical_width_mm
      << ' ' << options.physical_height_mm
      << ' ' << options.refresh_rate_millihz
      << ' ' << options.hdr_supported
      << ' ' << options.layout_policy
      << ' ' << options.position_x
      << ' ' << options.position_y
      << ' ' << options.name;
    return command.str();
  }

  std::optional<std::string> color_profile_association_broker_command(const std::vector<std::string> &args) {
    if (args.size() < 5) {
      return std::nullopt;
    }

    bool advanced_color = false;
    bool set_default = false;
    for (std::size_t index = 5; index < args.size(); ++index) {
      if (args[index] == "--advanced-color") {
        advanced_color = true;
      } else if (args[index] == "--default") {
        set_default = true;
      } else {
        return std::nullopt;
      }
    }

    std::ostringstream command;
    command
      << "helper-associate-color-profile "
      << args[2] << ' '
      << args[3] << ' '
      << (advanced_color ? "advanced" : "standard") << ' '
      << (set_default ? "default" : "nodefault") << ' '
      << args[4];
    return command.str();
  }
#endif

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
      } else if (arg == "--physical-width-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_width_mm)) {
          std::cerr << "invalid --physical-width-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-height-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_height_mm)) {
          std::cerr << "invalid --physical-height-mm value\n";
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

  std::optional<ProfileOptions> parse_profile_options(
    const std::vector<std::string> &args,
    const std::size_t first
  ) {
    ProfileOptions options {};
    bool saw_slot = false;

    for (std::size_t index = first; index < args.size(); ++index) {
      const auto &arg = args[index];
      const auto need_value = [&]() -> std::optional<std::string> {
        if (index + 1 >= args.size()) {
          std::cerr << arg << " requires a value\n";
          return std::nullopt;
        }
        return args[++index];
      };

      if (arg == "--slot") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.slot)) {
          std::cerr << "invalid --slot value\n";
          return std::nullopt;
        }
        saw_slot = true;
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
      } else if (arg == "--physical-width-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_width_mm)) {
          std::cerr << "invalid --physical-width-mm value\n";
          return std::nullopt;
        }
      } else if (arg == "--physical-height-mm") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.physical_height_mm)) {
          std::cerr << "invalid --physical-height-mm value\n";
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
      } else if (arg == "--layout") {
        const auto value = need_value();
        if (!value) {
          return std::nullopt;
        }
        if (*value == "none") {
          options.layout_policy = vdd::kDisplayManifestLayoutPolicyNone;
        } else if (*value == "apply") {
          options.layout_policy = vdd::kDisplayManifestLayoutPolicyApply;
        } else if (*value == "persist") {
          options.layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist;
        } else {
          std::cerr << "invalid --layout value\n";
          return std::nullopt;
        }
      } else if (arg == "--x") {
        const auto value = need_value();
        if (!value || !parse_i32(*value, options.position_x)) {
          std::cerr << "invalid --x value\n";
          return std::nullopt;
        }
      } else if (arg == "--y") {
        const auto value = need_value();
        if (!value || !parse_i32(*value, options.position_y)) {
          std::cerr << "invalid --y value\n";
          return std::nullopt;
        }
      } else if (arg == "--hdr") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.hdr_supported) || options.hdr_supported > 1) {
          std::cerr << "invalid --hdr value\n";
          return std::nullopt;
        }
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }
    }

    if (!saw_slot) {
      std::cerr << "permanent profile set requires --slot\n";
      return std::nullopt;
    }
    return options;
  }

  vdd::PermanentDisplayCountRequest make_request(const PermanentOptions &options) {
    vdd::PermanentDisplayCountRequest request {};
    request.display_count = options.count;
    request.width = options.width;
    request.height = options.height;
    request.physical_width_mm = options.physical_width_mm;
    request.physical_height_mm = options.physical_height_mm;
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

  if (args[0] == "broker") {
    if (args.size() == 2 && args[1] == "install") {
#ifdef _WIN32
      return install_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "start") {
#ifdef _WIN32
      return start_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "stop") {
#ifdef _WIN32
      return stop_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "status") {
#ifdef _WIN32
      return query_broker_service();
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 && args[1] == "uninstall") {
#ifdef _WIN32
      return uninstall_broker_service(args);
#else
      std::cerr << "broker service management is only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() == 2 &&
        (args[1] == "protocol" ||
         args[1] == "query-state" ||
         args[1] == "query-manifest" ||
         args[1] == "helper-diagnose" ||
         args[1] == "helper-apply-extended-topology" ||
         args[1] == "helper-apply-manifest-topology" ||
         args[1] == "helper-query-color-profiles")) {
#ifdef _WIN32
      return query_broker(args[1]);
#else
      std::cerr << "broker queries are only supported on Windows.\n";
      return 1;
#endif
    }
    if (args.size() >= 5 && args[1] == "helper-associate-color-profile") {
#ifdef _WIN32
      const auto command = color_profile_association_broker_command(args);
      if (!command) {
        print_usage();
        return 2;
      }
      return query_broker(*command);
#else
      std::cerr << "broker queries are only supported on Windows.\n";
      return 1;
#endif
    }

    print_usage();
    return 2;
  }

#ifdef _WIN32
  if (args[0] == "status" || (args[0] == "permanent" && args.size() >= 2 && args[1] == "query")) {
    return require_broker_command("permanent-query", true);
  }

  if (args[0] == "display" && args.size() >= 2 && args[1] == "query") {
    return require_broker_command("display-query", true);
  }

  if (args[0] == "spawn") {
    const auto options = parse_permanent_options(args, 1, false);
    if (!options) {
      return 2;
    }
    return require_broker_command(permanent_set_broker_command(*options), true);
  }

  if (args[0] == "permanent" && args.size() >= 2 && args[1] == "off") {
    PermanentOptions options {};
    options.count = 0;
    return require_broker_command(permanent_set_broker_command(options), true);
  }

  if (args[0] == "permanent" && args.size() >= 2 && args[1] == "set") {
    const auto options = parse_permanent_options(args, 2, true);
    if (!options) {
      return 2;
    }
    return require_broker_command(permanent_set_broker_command(*options), true);
  }

  if (args[0] == "permanent" && args.size() >= 3 && args[1] == "profile" && args[2] == "set") {
    const auto options = parse_profile_options(args, 3);
    if (!options) {
      return 2;
    }
    return require_broker_command(manifest_profile_set_broker_command(*options), true);
  }
#endif

  print_usage();
  return 2;
}
