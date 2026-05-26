#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <Windows.h>
  #include <sddl.h>
  #include <userenv.h>
  #include <wtsapi32.h>
#endif

namespace vdd = virtual_display::driver;

namespace {
  constexpr wchar_t kServiceName[] = L"SunshineVirtualDisplayBroker";
  constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\SunshineVirtualDisplayBroker";
  constexpr wchar_t kPipeSecurityDescriptor[] = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
  constexpr wchar_t kSessionHelperExecutable[] = L"virtualdisplay_probe.exe";
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

  std::vector<std::string_view> split_words(const std::string_view text, const std::size_t max_words) {
    std::vector<std::string_view> words;
    std::size_t cursor = 0;
    while (cursor < text.size() && words.size() < max_words) {
      while (cursor < text.size() && text[cursor] == ' ') {
        ++cursor;
      }
      if (cursor >= text.size()) {
        break;
      }

      const auto begin = cursor;
      while (cursor < text.size() && text[cursor] != ' ') {
        ++cursor;
      }
      words.emplace_back(text.substr(begin, cursor - begin));
    }
    return words;
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

  void set_display_name(char (&target)[vdd::kDisplayNameChars], const std::string_view name) {
    std::fill(std::begin(target), std::end(target), '\0');
    std::memcpy(target, name.data(), (std::min)(name.size(), static_cast<std::size_t>(vdd::kDisplayNameChars - 1)));
  }

  std::string format_permanent_state(const vdd::PermanentDisplayCountResult &state) {
    std::ostringstream output;
    output
      << "permanent_displays=" << state.current_display_count << '\n'
      << "max_permanent_displays=" << state.max_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "mode=" << state.width << 'x' << state.height << '@'
      << (state.refresh_rate_millihz / 1000.0) << "Hz\n"
      << "physical_size_mm=" << state.physical_width_mm << 'x' << state.physical_height_mm << '\n'
      << "name=" << display_name(state.display_name) << '\n';
    return output.str();
  }

  std::string format_display_state(const vdd::QueryDisplayStateResult &state) {
    std::ostringstream output;
    output
      << "permanent_displays=" << state.permanent_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "display_entries=" << state.entry_count << '\n';

    for (std::uint32_t index = 0; index < state.entry_count && index < vdd::kMaxDisplayStateEntries; ++index) {
      const auto &entry = state.entries[index];
      output
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

    return output.str();
  }

  std::optional<vdd::PermanentDisplayCountRequest> parse_permanent_set_command(const std::string_view command) {
    constexpr std::string_view prefix {"permanent-set "};
    if (!command.starts_with(prefix)) {
      return std::nullopt;
    }

    const auto payload = command.substr(prefix.size());
    const auto fields = split_words(payload, 6);
    if (fields.size() != 6) {
      return std::nullopt;
    }

    std::size_t name_offset = 0;
    for (const auto field: fields) {
      name_offset = static_cast<std::size_t>((field.data() + field.size()) - payload.data());
    }
    while (name_offset < payload.size() && payload[name_offset] == ' ') {
      ++name_offset;
    }
    if (name_offset >= payload.size()) {
      return std::nullopt;
    }

    vdd::PermanentDisplayCountRequest request {};
    if (!parse_u32(fields[0], request.display_count) ||
        !parse_u32(fields[1], request.width) ||
        !parse_u32(fields[2], request.height) ||
        !parse_u32(fields[3], request.physical_width_mm) ||
        !parse_u32(fields[4], request.physical_height_mm) ||
        !parse_u32(fields[5], request.refresh_rate_millihz)) {
      return std::nullopt;
    }
    set_display_name(request.display_name, payload.substr(name_offset));
    return request;
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

  class ScopedHandle {
  public:
    ScopedHandle() = default;

    explicit ScopedHandle(HANDLE value):
        value_ {value} {
    }

    ~ScopedHandle() {
      reset();
    }

    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;

    [[nodiscard]] HANDLE get() const {
      return value_;
    }

    [[nodiscard]] HANDLE *put() {
      reset();
      return &value_;
    }

    [[nodiscard]] bool valid() const {
      return value_ && value_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE value = nullptr) {
      if (valid()) {
        CloseHandle(value_);
      }
      value_ = value;
    }

  private:
    HANDLE value_ {};
  };

  class EnvironmentBlock {
  public:
    explicit EnvironmentBlock(void *value):
        value_ {value} {
    }

    ~EnvironmentBlock() {
      if (value_) {
        DestroyEnvironmentBlock(value_);
      }
    }

    EnvironmentBlock(const EnvironmentBlock &) = delete;
    EnvironmentBlock &operator=(const EnvironmentBlock &) = delete;

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

  std::wstring executable_directory() {
    wchar_t path[MAX_PATH] {};
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(MAX_PATH));
    if (length == 0 || length >= MAX_PATH) {
      return {};
    }

    std::wstring value {path, length};
    const auto slash = value.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      return {};
    }
    value.resize(slash);
    return value;
  }

  std::wstring quote_argument(const std::wstring &value) {
    std::wstring quoted {L"\""};
    for (wchar_t ch: value) {
      if (ch == L'"') {
        quoted += L'\\';
      }
      quoted += ch;
    }
    quoted += L'"';
    return quoted;
  }

  DWORD launch_console_helper(const std::wstring &arguments) {
    const DWORD session_id = WTSGetActiveConsoleSessionId();
    if (session_id == 0xffffffffu) {
      return ERROR_NO_TOKEN;
    }

    ScopedHandle impersonation_token;
    if (!WTSQueryUserToken(session_id, impersonation_token.put())) {
      return GetLastError();
    }

    ScopedHandle primary_token;
    if (!DuplicateTokenEx(
          impersonation_token.get(),
          TOKEN_ASSIGN_PRIMARY | TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID,
          nullptr,
          SecurityImpersonation,
          TokenPrimary,
          primary_token.put()
        )) {
      return GetLastError();
    }

    void *environment = nullptr;
    if (!CreateEnvironmentBlock(&environment, primary_token.get(), FALSE)) {
      return GetLastError();
    }
    EnvironmentBlock environment_block {environment};

    const auto directory = executable_directory();
    if (directory.empty()) {
      return ERROR_FILE_NOT_FOUND;
    }

    const std::wstring helper_path = directory + L"\\" + kSessionHelperExecutable;
    std::wstring command_line = quote_argument(helper_path);
    if (!arguments.empty()) {
      command_line += L" ";
      command_line += arguments;
    }

    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION process {};
    if (!CreateProcessAsUserW(
          primary_token.get(),
          helper_path.c_str(),
          command_line.data(),
          nullptr,
          nullptr,
          FALSE,
          CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
          environment_block.get(),
          directory.c_str(),
          &startup,
          &process
        )) {
      return GetLastError();
    }

    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = ERROR_SUCCESS;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
      exit_code = GetLastError();
    }
    CloseHandle(process.hProcess);
    return exit_code;
  }

  std::optional<std::wstring> helper_arguments_for_broker_command(const std::string_view command) {
    if (command == "helper-diagnose") {
      return L"--diagnose";
    }
    if (command == "helper-apply-extended-topology") {
      return L"--apply-extended-topology";
    }
    if (command == "helper-query-color-profiles") {
      return L"--query-color-profiles";
    }
    return std::nullopt;
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

    if (command == "display-query") {
      const auto result = client.query_display_state();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok\n" + format_display_state(result.value);
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

    if (command == "permanent-query") {
      const auto result = client.query_permanent_display_count();
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok\n" + format_permanent_state(result.value);
    }

    if (command.starts_with("permanent-set ")) {
      const auto request = parse_permanent_set_command(command);
      if (!request) {
        return "error invalid_permanent_set\n";
      }

      const auto result = client.set_permanent_display_count(*request);
      if (!result.ok()) {
        return "error " + format_status(result) + "\n";
      }
      return "ok\n" + format_permanent_state(result.value);
    }

    if (const auto helper_arguments = helper_arguments_for_broker_command(command)) {
      const DWORD helper_result = launch_console_helper(*helper_arguments);
      if (helper_result != ERROR_SUCCESS) {
        return "error helper_result=" + std::to_string(helper_result) + "\n";
      }
      return "ok helper_result=0\n";
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
