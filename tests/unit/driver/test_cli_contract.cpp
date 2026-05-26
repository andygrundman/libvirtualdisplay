#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
  std::string read_cli_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} / "tools/virtualdisplay.cpp";
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  void expect_contains(const std::string &content, const std::string &needle) {
    EXPECT_NE(content.find(needle), std::string::npos) << "missing: " << needle;
  }
}  // namespace

TEST(VirtualDisplayCliContract, ExposesFriendlyPermanentDisplayCommands) {
  const auto source = read_cli_source();

  expect_contains(source, "status");
  expect_contains(source, "broker install|start|stop|status|uninstall");
  expect_contains(source, "broker protocol|query-state|query-manifest");
  expect_contains(source, "helper-apply-extended-topology");
  expect_contains(source, "helper-apply-manifest-topology");
  expect_contains(source, "helper-query-color-profiles");
  expect_contains(source, "broker helper-associate-color-profile <source_luid high:low> <source_id> <profile> [--advanced-color] [--default]");
  expect_contains(source, "display query");
  expect_contains(source, "driver install [--inf PATH]");
  expect_contains(source, "spawn [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent query");
  expect_contains(source, "permanent set --count N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent profile set --slot N");
  expect_contains(source, "permanent off");
  expect_contains(source, "require_broker_command(\"permanent-query\", true)");
  expect_contains(source, "require_broker_command(\"display-query\", true)");
}

TEST(VirtualDisplayCliContract, BrokerCommandsUseSecuredIpcPath) {
  const auto source = read_cli_source();

  expect_contains(source, "kBrokerPipeName[] = L\"\\\\\\\\.\\\\pipe\\\\SunshineVirtualDisplayBroker\"");
  expect_contains(source, "int query_broker(const std::string_view command)");
  expect_contains(source, "BrokerResponse request_broker(const std::string_view command)");
  expect_contains(source, "int require_broker_command(const std::string_view command, const bool print_payload)");
  expect_contains(source, "require_broker_command(\"permanent-query\", true)");
  expect_contains(source, "require_broker_command(\"display-query\", true)");
  expect_contains(source, "require_broker_command(permanent_set_broker_command(*options), true)");
  expect_contains(source, "start the broker service before using display management commands");
  expect_contains(source, "permanent-set ");
  expect_contains(source, "CreateFileW(");
  expect_contains(source, "WriteFile(");
  expect_contains(source, "ReadFile(");
  expect_contains(source, "ERROR_MORE_DATA");
  expect_contains(source, "kMaxBrokerResponseBytes");
  expect_contains(source, "error response_too_large");
  expect_contains(source, "if (args[0] == \"broker\")");
  expect_contains(source, "args[1] == \"query-state\"");
  expect_contains(source, "args[1] == \"query-manifest\"");
  expect_contains(source, "args[1] == \"helper-diagnose\"");
  expect_contains(source, "args[1] == \"helper-apply-extended-topology\"");
  expect_contains(source, "args[1] == \"helper-apply-manifest-topology\"");
  expect_contains(source, "args[1] == \"helper-query-color-profiles\"");
  expect_contains(source, "args[1] == \"helper-associate-color-profile\"");
  expect_contains(source, "color_profile_association_broker_command(args)");
  expect_contains(source, "<< (advanced_color ? \"advanced\" : \"standard\")");
  expect_contains(source, "<< (set_default ? \"default\" : \"nodefault\")");

  const auto broker_command = source.find("if (args[0] == \"broker\")");
  const auto direct_open = source.find("vdd::open_first_control_device()");
  ASSERT_NE(broker_command, std::string::npos);
  EXPECT_EQ(direct_open, std::string::npos);
}

TEST(VirtualDisplayCliContract, BrokerServiceCommandsManageWindowsService) {
  const auto source = read_cli_source();

  expect_contains(source, "kBrokerServiceName[] = L\"SunshineVirtualDisplayBroker\"");
  expect_contains(source, "broker_executable_path()");
  expect_contains(source, "virtualdisplay_broker.exe");
  expect_contains(source, "OpenSCManagerW");
  expect_contains(source, "CreateServiceW");
  expect_contains(source, "ChangeServiceConfigW");
  expect_contains(source, "kBrokerServiceSecurityDescriptor[] = L\"D:P(A;;GA;;;SY)(A;;GA;;;BA)\"");
  expect_contains(source, "ChangeServiceConfig2W");
  expect_contains(source, "SERVICE_CONFIG_SERVICE_SID_INFO");
  expect_contains(source, "SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO");
  expect_contains(source, "SERVICE_AUTO_START");
  expect_contains(source, "SERVICE_SID_TYPE_UNRESTRICTED");
  expect_contains(source, "SeTcbPrivilege");
  expect_contains(source, "ConvertStringSecurityDescriptorToSecurityDescriptorW");
  expect_contains(source, "SetServiceObjectSecurity");
  expect_contains(source, "DACL_SECURITY_INFORMATION");
  expect_contains(source, "harden_broker_service(service.value)");
  expect_contains(source, "StartServiceW");
  expect_contains(source, "ControlService");
  expect_contains(source, "QueryServiceStatusEx");
  expect_contains(source, "DeleteService");
  expect_contains(source, "broker_service_state=");
  expect_contains(source, "args[1] == \"install\"");
  expect_contains(source, "args[1] == \"start\"");
  expect_contains(source, "args[1] == \"stop\"");
  expect_contains(source, "args[1] == \"status\"");
  expect_contains(source, "args[1] == \"uninstall\"");
  expect_contains(source, "wait_for_broker_service_state(service.value, SERVICE_RUNNING)");
  expect_contains(source, "wait_for_broker_service_state(service.value, SERVICE_STOPPED)");
  expect_contains(source, "broker_service_started=1");
  expect_contains(source, "broker_service_stopped=1");
}

TEST(VirtualDisplayCliContract, PermanentCommandsApplyModeAndNameSettings) {
  const auto source = read_cli_source();

  expect_contains(source, "request.width = options.width");
  expect_contains(source, "request.height = options.height");
  expect_contains(source, "request.physical_width_mm = options.physical_width_mm");
  expect_contains(source, "request.physical_height_mm = options.physical_height_mm");
  expect_contains(source, "request.refresh_rate_millihz = options.refresh_rate_millihz");
  expect_contains(source, "set_display_name(request.display_name, options.name)");
  expect_contains(source, "manifest-profile-set ");
  expect_contains(source, "parse_refresh_millihz");
  expect_contains(source, "std::strtod(text.c_str(), &end)");
  expect_contains(source, "end != text.c_str() + text.size()");
  expect_contains(source, "std::isfinite(refresh)");
  expect_contains(source, "(std::numeric_limits<std::uint32_t>::max)()");
  expect_contains(source, "args[0] == \"status\" && args.size() == 1");
  expect_contains(source, "args[0] == \"permanent\" && args.size() == 2 && args[1] == \"query\"");
  expect_contains(source, "args[0] == \"display\" && args.size() == 2 && args[1] == \"query\"");
  expect_contains(source, "args[0] == \"permanent\" && args.size() == 2 && args[1] == \"off\"");
}

TEST(VirtualDisplayCliContract, DisplayQueryPrintsIdentityFields) {
  const auto source = read_cli_source();

  expect_contains(source, "display_id=");
  expect_contains(source, "connector_index=");
  expect_contains(source, "container_id=");
  expect_contains(source, "product_code=");
  expect_contains(source, "serial_number=");
  expect_contains(source, "retain_identity=");
  expect_contains(source, "output_display_name(entry.display_name)");
  expect_contains(source, "escape_key_value(display_name(value))");
  expect_contains(source, "output += \"\\\\n\"");
  expect_contains(source, "output += \"\\\\x\"");
}

TEST(VirtualDisplayCliContract, DriverInstallSelfElevatesAndInstallsRootDevice) {
  const auto source = read_cli_source();

  expect_contains(source, "ShellExecuteExW(&execute)");
  expect_contains(source, "execute.lpVerb = L\"runas\"");
  expect_contains(source, "quoted.append(backslashes * 2 + 1, L'\\\\')");
  expect_contains(source, "quoted.append(backslashes * 2, L'\\\\')");
  expect_contains(source, "Root\\\\SunshineVirtualDisplay");
  expect_contains(source, "UpdateDriverForPlugAndPlayDevicesW");
  expect_contains(source, "driver_installed=1");
  expect_contains(source, "multi_sz_contains(buffer.data(), required_size");
  expect_contains(source, "byte_count % sizeof(wchar_t)");
  expect_contains(source, "wide_values[char_count - 1] != L'\\0'");
}
