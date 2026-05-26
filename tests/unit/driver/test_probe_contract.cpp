#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
  std::string read_probe_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} / "tools/virtualdisplay_probe.cpp";
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  std::string read_broker_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} / "tools/virtualdisplay_broker.cpp";
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  std::string read_driver_cmake() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} / "src/driver/CMakeLists.txt";
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

  void expect_not_contains(const std::string &content, const std::string &needle) {
    EXPECT_EQ(content.find(needle), std::string::npos) << "unexpected: " << needle;
  }
}  // namespace

TEST(VirtualDisplayProbeContract, ExposesTemporaryAndPermanentRuntimeChecks) {
  const auto source = read_probe_source();

  expect_contains(source, "--diagnose");
  expect_contains(source, "--apply-extended-topology");
  expect_contains(source, "--query-color-profiles");
  expect_contains(source, "--check");
  expect_contains(source, "--query-permanent");
  expect_contains(source, "--set-permanent <count>");
  expect_contains(source, "--self-test-permanent [count]");
  expect_contains(source, "--self-test-temp [width height refresh_hz]");
  expect_contains(source, "--self-test-4k240 [timeout_ms]");
  expect_contains(source, "--self-test-hdr [width height refresh_hz]");
  expect_contains(source, "--qa-multi-temp-lease [count timeout_ms]");
  expect_contains(source, "--qa-temp-identity-retention [width height refresh_hz timeout_ms]");
}

TEST(VirtualDisplayProbeContract, DiagnoseRunsBeforeControlDeviceOpen) {
  const auto source = read_probe_source();

  expect_contains(source, "vdd::enumerate_control_devices(&enumerate_error)");
  expect_contains(source, "control_interface_count=");
  expect_contains(source, "if (command == \"--diagnose\")");
  expect_contains(source, "if (command == \"--apply-extended-topology\")");
  expect_contains(source, "if (command == \"--query-color-profiles\")");
  expect_contains(source, "apply_extended_topology_result()");
  expect_contains(source, "ColorProfileGetDisplayUserScope");
  expect_contains(source, "ColorProfileGetDisplayDefault");
  expect_not_contains(source, "AssociateColorProfileWithDevice");
  expect_not_contains(source, "SetICMProfile");

  const auto command_pos = source.find("const std::string command {argv[1]}");
  const auto diagnose_pos = source.find("if (command == \"--diagnose\")");
  const auto topology_pos = source.find("if (command == \"--apply-extended-topology\")");
  const auto color_profile_pos = source.find("if (command == \"--query-color-profiles\")");
  const auto open_pos = source.find("auto opened = vdd::open_first_control_device()");
  ASSERT_NE(command_pos, std::string::npos);
  ASSERT_NE(diagnose_pos, std::string::npos);
  ASSERT_NE(topology_pos, std::string::npos);
  ASSERT_NE(color_profile_pos, std::string::npos);
  ASSERT_NE(open_pos, std::string::npos);
  EXPECT_LT(command_pos, open_pos);
  EXPECT_LT(diagnose_pos, open_pos);
  EXPECT_LT(topology_pos, open_pos);
  EXPECT_LT(color_profile_pos, open_pos);
}

TEST(VirtualDisplayProbeContract, PermanentSelfTestRestoresOriginalCount) {
  const auto source = read_probe_source();

  expect_contains(source, "const auto before = client.query_permanent_display_count()");
  expect_contains(source, "const auto changed = client.set_permanent_display_count(request)");
  expect_contains(source, "restore.display_count = before.value.current_display_count");
  expect_contains(source, "const auto restored = client.set_permanent_display_count(restore)");
}

TEST(VirtualDisplayProbeContract, HdrSelfTestVerifiesWindowsAdvancedColorState) {
  const auto source = read_probe_source();
  const auto cmake = read_driver_cmake();

  expect_contains(source, "DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2");
  expect_contains(source, "DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE");
  expect_contains(source, "ColorProfileGetDisplayUserScope");
  expect_contains(source, "ColorProfileGetDisplayDefault");
  expect_contains(cmake, "target_link_libraries(virtualdisplay_probe PRIVATE libvirtualdisplay::driver mscms)");
  expect_contains(source, "const auto after = wait_for_advanced_color(");
  expect_contains(source, "created.value.os_adapter_luid");
  expect_contains(source, "created.value.target_id");
  expect_contains(source, "temporary display is not reported as HDR-supported by Windows");
  expect_contains(source, "temporary display did not enter HDR 10-bit mode after request");
}

TEST(VirtualDisplayProbeContract, DisplayConfigCommandsRequireInteractiveSession) {
  const auto source = read_probe_source();

  expect_contains(source, "command_uses_display_config");
  expect_contains(source, "--apply-extended-topology");
  expect_contains(source, "--query-color-profiles");
  expect_contains(source, "--self-test-4k240");
  expect_contains(source, "--self-test-hdr");
  expect_contains(source, "--qa-temp-identity-retention");
  expect_contains(source, "--qa-temp-lease");
  expect_contains(source, "--debug-temp-config");
  expect_contains(source, "WTSGetActiveConsoleSessionId()");
  expect_contains(source, "ProcessIdToSessionId(GetCurrentProcessId(), &current_session_id)");
  expect_contains(source, "requires an active console session for DisplayConfig and color APIs");
  expect_contains(source, "must run in the active console session for DisplayConfig and color APIs");

  const auto guard = source.find("if (command_uses_display_config(command))");
  const auto hdr = source.find("if (command == \"--self-test-hdr\")");
  ASSERT_NE(guard, std::string::npos);
  ASSERT_NE(hdr, std::string::npos);
  EXPECT_LT(guard, hdr);
}

TEST(VirtualDisplayProbeContract, ActiveDisplayChecksApplyRequestedResolution) {
  const auto source = read_probe_source();

  expect_contains(source, "ChangeDisplaySettingsExW");
  expect_contains(source, "ensure_active_display_mode");
  expect_contains(source, "run_temporary_mode_probe");
  expect_contains(source, "if (command == \"--self-test-4k240\")");
  expect_contains(source, "return run_temporary_mode_probe(client, 3840u, 2160u, 240u");
  expect_contains(source, "Windows can reuse the previous mode on a recycled target id");
  expect_contains(source, "debug display resolution mismatch");
  expect_contains(source, "QA display resolution mismatch after activation");
  expect_contains(source, "make_active_signal_info(width, height, refresh_hz)");
  expect_contains(source, "requested_target.sourceInfo.sourceModeInfoIdx = source_mode_index");
  expect_contains(source, "requested_target.targetInfo.targetModeInfoIdx = target_mode_index");
  expect_contains(source, "requested_target.targetInfo.desktopModeInfoIdx = desktop_mode_index");
  expect_not_contains(source, "return activate_result == ERROR_SUCCESS && mode_ready ? 0 : 1");
}

TEST(VirtualDisplayProbeContract, TemporaryLeaseQaFeedsWhileValidatingHdr) {
  const auto source = read_probe_source();

  expect_contains(source, "const auto feed_qa_lease = [&]() {");
  expect_contains(source, "client.feed_lease(lease_request)");
  expect_contains(source, "This QA path intentionally uses very short leases");
  expect_contains(source, "wait_for_advanced_color(");
  expect_contains(source, "feed_qa_lease");
}

TEST(VirtualDisplayProbeContract, MultiTemporaryLeaseQaCreatesAndExpiresSeveralDisplays) {
  const auto source = read_probe_source();

  expect_contains(source, "if (command == \"--qa-multi-temp-lease\")");
  expect_contains(source, "multi-temp QA reused connector index");
  expect_contains(source, "active.value.temporary_display_count != count");
  expect_contains(source, "multi-temp QA lease did not expire cleanly");
  expect_contains(source, "qa_multi_temp_lease=1");
}

TEST(VirtualDisplayProbeContract, TemporaryIdentityRetentionQaRequiresRestoredHdrProfile) {
  const auto source = read_probe_source();

  expect_contains(source, "if (command == \"--qa-temp-identity-retention\")");
  expect_contains(source, "const auto stable_display_id = transient_id(0x51dd1000)");
  expect_contains(source, "second.value.connector_index != first.value.connector_index");
  expect_contains(source, "second.value.target_id != first.value.target_id");
  expect_contains(source, "filler display reused retained identity connector");
  expect_contains(source, "HDR profile was not retained for recreated temporary display");
  expect_contains(source, "qa_temp_identity_retention=1");
}

TEST(VirtualDisplayProbeContract, BrokerOwnsDriverAccessBehindSecuredPipe) {
  const auto source = read_broker_source();
  const auto cmake = read_driver_cmake();

  expect_contains(cmake, "add_executable(virtualdisplay_broker");
  expect_contains(cmake, "target_link_libraries(virtualdisplay_broker PRIVATE libvirtualdisplay::driver advapi32 userenv wtsapi32)");
  expect_contains(cmake, "target_link_libraries(virtualdisplay PRIVATE libvirtualdisplay::driver advapi32 shell32 newdev)");
  expect_contains(source, "kPipeName[] = L\"\\\\\\\\.\\\\pipe\\\\SunshineVirtualDisplayBroker\"");
  expect_contains(source, "kPipeSecurityDescriptor[] = L\"D:P(A;;GA;;;SY)(A;;GA;;;BA)\"");
  expect_contains(source, "kSessionHelperExecutable[] = L\"virtualdisplay_probe.exe\"");
  expect_contains(source, "ConvertStringSecurityDescriptorToSecurityDescriptorW");
  expect_contains(source, "CreateNamedPipeW");
  expect_contains(source, "open_first_control_device()");
  expect_contains(source, "query_display_state()");
  expect_contains(source, "query_display_manifest()");
  expect_contains(source, "helper_arguments_for_broker_command");
  expect_contains(source, "return L\"--diagnose\"");
  expect_contains(source, "return L\"--apply-extended-topology\"");
  expect_contains(source, "return L\"--query-color-profiles\"");
  expect_contains(source, "launch_console_helper(*helper_arguments)");
  expect_contains(source, "WTSGetActiveConsoleSessionId()");
  expect_contains(source, "WTSQueryUserToken(session_id");
  expect_contains(source, "CreateProcessAsUserW");
  expect_contains(source, "RegisterServiceCtrlHandlerW");
  expect_contains(source, "StartServiceCtrlDispatcherW");
  expect_contains(source, "--run-console");
  expect_contains(source, "--service");
}
