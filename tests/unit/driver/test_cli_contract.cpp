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
  expect_contains(source, "helper-query-color-profiles");
  expect_contains(source, "display query");
  expect_contains(source, "driver install [--inf PATH]");
  expect_contains(source, "spawn [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent query");
  expect_contains(source, "permanent set --count N [--width N] [--height N] [--physical-width-mm N] [--physical-height-mm N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent off");
  expect_contains(source, "client.set_permanent_display_count(request)");
  expect_contains(source, "client.query_display_state()");
}

TEST(VirtualDisplayCliContract, BrokerCommandsUseSecuredIpcPath) {
  const auto source = read_cli_source();

  expect_contains(source, "kBrokerPipeName[] = L\"\\\\\\\\.\\\\pipe\\\\SunshineVirtualDisplayBroker\"");
  expect_contains(source, "int query_broker(const std::string_view command)");
  expect_contains(source, "CreateFileW(");
  expect_contains(source, "WriteFile(");
  expect_contains(source, "ReadFile(");
  expect_contains(source, "if (args[0] == \"broker\")");
  expect_contains(source, "args[1] == \"query-state\"");
  expect_contains(source, "args[1] == \"query-manifest\"");
  expect_contains(source, "args[1] == \"helper-diagnose\"");
  expect_contains(source, "args[1] == \"helper-apply-extended-topology\"");
  expect_contains(source, "args[1] == \"helper-query-color-profiles\"");

  const auto broker_command = source.find("if (args[0] == \"broker\")");
  const auto direct_open = source.find("const auto opened = vdd::open_first_control_device()");
  ASSERT_NE(broker_command, std::string::npos);
  ASSERT_NE(direct_open, std::string::npos);
  EXPECT_LT(broker_command, direct_open);
}

TEST(VirtualDisplayCliContract, BrokerServiceCommandsManageWindowsService) {
  const auto source = read_cli_source();

  expect_contains(source, "kBrokerServiceName[] = L\"SunshineVirtualDisplayBroker\"");
  expect_contains(source, "broker_executable_path()");
  expect_contains(source, "virtualdisplay_broker.exe");
  expect_contains(source, "OpenSCManagerW");
  expect_contains(source, "CreateServiceW");
  expect_contains(source, "ChangeServiceConfigW");
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
}

TEST(VirtualDisplayCliContract, PermanentCommandsApplyModeAndNameSettings) {
  const auto source = read_cli_source();

  expect_contains(source, "request.width = options.width");
  expect_contains(source, "request.height = options.height");
  expect_contains(source, "request.physical_width_mm = options.physical_width_mm");
  expect_contains(source, "request.physical_height_mm = options.physical_height_mm");
  expect_contains(source, "request.refresh_rate_millihz = options.refresh_rate_millihz");
  expect_contains(source, "set_display_name(request.display_name, options.name)");
  expect_contains(source, "parse_refresh_millihz");
}

TEST(VirtualDisplayCliContract, DisplayQueryPrintsIdentityFields) {
  const auto source = read_cli_source();

  expect_contains(source, "display_id=");
  expect_contains(source, "connector_index=");
  expect_contains(source, "container_id=");
  expect_contains(source, "product_code=");
  expect_contains(source, "serial_number=");
  expect_contains(source, "retain_identity=");
}

TEST(VirtualDisplayCliContract, DriverInstallSelfElevatesAndInstallsRootDevice) {
  const auto source = read_cli_source();

  expect_contains(source, "ShellExecuteExW(&execute)");
  expect_contains(source, "execute.lpVerb = L\"runas\"");
  expect_contains(source, "Root\\\\SunshineVirtualDisplay");
  expect_contains(source, "UpdateDriverForPlugAndPlayDevicesW");
  expect_contains(source, "driver_installed=1");
}
