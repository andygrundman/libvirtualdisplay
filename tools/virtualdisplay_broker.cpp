#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <sddl.h>
#endif

namespace vdd = virtual_display::driver;

namespace {
  constexpr wchar_t kServiceName[] = L"SunshineVirtualDisplayBroker";
  constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SunshineVirtualDisplayBroker";
  constexpr wchar_t kPipeSecurityDescriptor[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
  constexpr DWORD kPipeBufferBytes = 4096;

  void print_usage() {
    std::cout
      << "virtualdisplay_broker commands:\n"
      << "  --run-console\n"
      << "  --service\n";
  }

  std::string trim_command(std::string value) {
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
      value.pop_back();
    }
    while (!value.empty() && value.front() == ' ') {
      value.erase(value.begin());
    }
    return value;
  }

  std::string format_status(const vdd::ControlOperationResult &result) {
    std::string text {vdd::to_string(result.status)};
    if (result.native_error != 0) {
      text += " native_error=" + std::to_string(result.native_error);
    }
    return text;
  }

  template<class T>
  std::string format_status(const vdd::ControlResult<T> &result) {
    return format_status(vdd::ControlOperationResult {result.status, result.native_error});
  }

#ifdef _WIN32
  class LocalMemory {
  public:
    explicit LocalMemory(void *value):
        value_ {value} {
    }

    ~LocalMemory() {
      if (value_) {
        LocalFree(value_);
      }
    }

    LocalMemory(const LocalMemory &) = delete;
    LocalMemory &operator=(const LocalMemory &) = delete;

    [[nodiscard]] void *get() const {
      return value_;
    }

  private:
    void *value_ {};
  };

  struct PipeSecurity {
    SECURITY_ATTRIBUTES attributes {};
    LocalMemory descriptor {nullptr};

    PipeSecurity(SECURITY_ATTRIBUTES security_attributes, void *security_descriptor):
        attributes {security_attributes},
        descriptor {security_descriptor} {
    }
  };

  std::optional<PipeSecurity> make_pipe_security() {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          kPipeSecurityDescriptor,
          SDDL_REVISION_1,
          &descriptor,
          nullptr
        )) {
      return std::nullopt;
    }

    SECURITY_ATTRIBUTES attributes {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return std::optional<PipeSecurity> {std::in_place, attributes, descriptor};
  }

  struct BrokerContext {
    SERVICE_STATUS_HANDLE service_status_handle {};
    SERVICE_STATUS service_status {};
    HANDLE stop_event {};
    std::atomic<bool> stop_requested {false};
  };

  BrokerContext g_context {};

  void report_service_status(const DWORD state, const DWORD win32_exit_code = NO_ERROR) {
    if (!g_context.service_status_handle) {
      return;
    }

    g_context.service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_context.service_status.dwCurrentState = state;
    g_context.service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP : 0;
    g_context.service_status.dwWin32ExitCode = win32_exit_code;
    g_context.service_status.dwServiceSpecificExitCode = 0;
    g_context.service_status.dwCheckPoint = 0;
    g_context.service_status.dwWaitHint = 0;
    (void) SetServiceStatus(g_context.service_status_handle, &g_context.service_status);
  }

  std::string handle_command(vdd::ControlClient &client, const std::string_view command) {
    if (command == "protocol") {
      const auto result = client.query_protocol_version();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok protocol=" + std::to_string(result.value.major) + "." +
             std::to_string(result.value.minor) + "." +
             std::to_string(result.value.patch) + "\n";
    }

    if (command == "query-state") {
      const auto result = client.query_display_state();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok permanent=" + std::to_string(result.value.permanent_display_count) +
             " temporary=" + std::to_string(result.value.temporary_display_count) +
             " entries=" + std::to_string(result.value.entry_count) + "\n";
    }

    if (command == "query-manifest") {
      const auto result = client.query_display_manifest();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok version=" + std::to_string(result.value.version) +
             " profiles=" + std::to_string(result.value.profile_count) +
             " max_profiles=" + std::to_string(result.value.max_profile_count) + "\n";
    }

    return "error unknown_command\n";
  }

  void serve_pipe_client(HANDLE pipe, vdd::ControlClient &client) {
    char input[kPipeBufferBytes] {};
    DWORD bytes_read = 0;
    const BOOL read_ok = ReadFile(pipe, input, sizeof(input) - 1, &bytes_read, nullptr);
    std::string response;
    if (!read_ok || bytes_read == 0) {
      response = "error read_failed\n";
    } else {
      input[(std::min<DWORD>)(bytes_read, sizeof(input) - 1)] = '\0';
      response = handle_command(client, trim_command(input));
    }

    DWORD bytes_written = 0;
    (void) WriteFile(
      pipe,
      response.data(),
      static_cast<DWORD>((std::min<std::size_t>)(response.size(), (std::numeric_limits<DWORD>::max)())),
      &bytes_written,
      nullptr
    );
    FlushFileBuffers(pipe);
  }

  int run_broker_loop() {
    auto opened = vdd::open_first_control_device();
    if (!opened.ok()) {
      std::cerr << "open driver control device failed: "
                << vdd::to_string(opened.status)
                << " native_error=" << opened.native_error << '\n';
      return 1;
    }

    vdd::ControlClient client {*opened.transport};
    const auto protocol = client.query_protocol_version();
    if (!protocol.ok()) {
      std::cerr << "driver protocol check failed: " << format_status(protocol) << '\n';
      return 1;
    }

    auto pipe_security = make_pipe_security();
    if (!pipe_security) {
      std::cerr << "create pipe security failed native_error=" << GetLastError() << '\n';
      return 1;
    }

    while (!g_context.stop_requested.load(std::memory_order_acquire)) {
      HANDLE pipe = CreateNamedPipeW(
        kPipeName,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        kPipeBufferBytes,
        kPipeBufferBytes,
        0,
        &pipe_security->attributes
      );
      if (pipe == INVALID_HANDLE_VALUE) {
        std::cerr << "create broker pipe failed native_error=" << GetLastError() << '\n';
        return 1;
      }

      const BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
      if (connected && !g_context.stop_requested.load(std::memory_order_acquire)) {
        serve_pipe_client(pipe, client);
      }

      DisconnectNamedPipe(pipe);
      CloseHandle(pipe);
    }

    return 0;
  }

  void WINAPI service_control_handler(const DWORD control_code) {
    if (control_code != SERVICE_CONTROL_STOP) {
      return;
    }

    report_service_status(SERVICE_STOP_PENDING);
    g_context.stop_requested.store(true, std::memory_order_release);
    if (g_context.stop_event) {
      SetEvent(g_context.stop_event);
    }

    HANDLE client = CreateFileW(
      kPipeName,
      GENERIC_READ | GENERIC_WRITE,
      0,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL,
      nullptr
    );
    if (client != INVALID_HANDLE_VALUE) {
      CloseHandle(client);
    }
  }

  void WINAPI service_main(DWORD, LPWSTR *) {
    g_context.service_status_handle = RegisterServiceCtrlHandlerW(kServiceName, service_control_handler);
    if (!g_context.service_status_handle) {
      return;
    }

    g_context.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    report_service_status(SERVICE_START_PENDING);
    report_service_status(SERVICE_RUNNING);
    const int result = run_broker_loop();
    if (g_context.stop_event) {
      CloseHandle(g_context.stop_event);
      g_context.stop_event = nullptr;
    }
    report_service_status(SERVICE_STOPPED, result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
  }

  int run_service() {
    SERVICE_TABLE_ENTRYW service_table[] {
      {const_cast<LPWSTR>(kServiceName), service_main},
      {nullptr, nullptr}
    };
    if (!StartServiceCtrlDispatcherW(service_table)) {
      std::cerr << "start service dispatcher failed native_error=" << GetLastError() << '\n';
      return 1;
    }
    return 0;
  }
#endif
}  // namespace

int main(const int argc, char **argv) {
#ifndef _WIN32
  (void) argc;
  (void) argv;
  std::cerr << "virtualdisplay_broker is only supported on Windows.\n";
  return 1;
#else
  if (argc < 2) {
    print_usage();
    return 2;
  }

  const std::string_view command {argv[1]};
  if (command == "--run-console") {
    return run_broker_loop();
  }
  if (command == "--service") {
    return run_service();
  }

  print_usage();
  return 2;
#endif
}
