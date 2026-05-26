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
  expect_contains(source, "--apply-manifest-topology");
  expect_contains(source, "--query-color-profiles");
  expect_contains(source, "--associate-color-profile <source_luid high:low> <source_id> <profile> [--advanced-color] [--default]");
  expect_contains(source, "--check");
  expect_contains(source, "--query-permanent");
  expect_contains(source, "--set-permanent <count>");
  expect_contains(source, "--self-test-permanent [count]");
  expect_contains(source, "--self-test-temp [width height refresh_hz]");
  expect_contains(source, "--self-test-4k240 [timeout_ms]");
  expect_contains(source, "--self-test-hdr [width height refresh_hz]");
  expect_contains(source, "--qa-multi-temp-lease [count timeout_ms]");
  expect_contains(source, "--qa-temp-identity-retention [width height refresh_hz timeout_ms]");
  expect_contains(source, "refresh_millihz_from_hz(refresh_hz)");
  expect_contains(source, "saturating_mul_u64");
  expect_contains(source, "saturating_u32(static_cast<std::uint64_t>(refresh_hz) * height)");
  expect_not_contains(source, "refresh_hz * 1000u");
  expect_contains(source, "std::from_chars(begin, end, value)");
  expect_contains(source, "result.ptr != end");
  expect_contains(source, "read_u32_arg(argc, argv");
  expect_not_contains(source, "std::stoul");
  expect_not_contains(source, "std::stol");
}

TEST(VirtualDisplayProbeContract, DiagnoseRunsBeforeControlDeviceOpen) {
  const auto source = read_probe_source();

  expect_contains(source, "vdd::enumerate_control_devices(&enumerate_error)");
  expect_contains(source, "control_interface_count=");
  expect_contains(source, "if (command == \"--diagnose\")");
  expect_contains(source, "if (command == \"--apply-extended-topology\")");
  expect_contains(source, "if (command == \"--query-color-profiles\")");
  expect_contains(source, "if (command == \"--associate-color-profile\")");
  expect_contains(source, "apply_extended_topology_result()");
  expect_contains(source, "ColorProfileGetDisplayUserScope");
  expect_contains(source, "ColorProfileGetDisplayList");
  expect_contains(source, "ColorProfileGetDisplayDefault");
  expect_contains(source, "ColorProfileAddDisplayAssociation");
  expect_contains(source, "LoadLibraryExW(L\"mscms.dll\", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)");
  expect_contains(source, "color_api->add_association(");
  expect_contains(source, "associate_color_profile(*source_luid, source_id");
  expect_not_contains(source, "AssociateColorProfileWithDevice");
  expect_not_contains(source, "SetICMProfile");

  const auto command_pos = source.find("const std::string command {argv[1]}");
  const auto diagnose_pos = source.find("if (command == \"--diagnose\")");
  const auto topology_pos = source.find("if (command == \"--apply-extended-topology\")");
  const auto color_profile_pos = source.find("if (command == \"--query-color-profiles\")");
  const auto color_association_pos = source.find("if (command == \"--associate-color-profile\")");
  const auto open_pos = source.find("auto opened = vdd::open_first_control_device()");
  ASSERT_NE(command_pos, std::string::npos);
  ASSERT_NE(diagnose_pos, std::string::npos);
  ASSERT_NE(topology_pos, std::string::npos);
  ASSERT_NE(color_profile_pos, std::string::npos);
  ASSERT_NE(color_association_pos, std::string::npos);
  ASSERT_NE(open_pos, std::string::npos);
  EXPECT_LT(command_pos, open_pos);
  EXPECT_LT(diagnose_pos, open_pos);
  EXPECT_LT(topology_pos, open_pos);
  EXPECT_LT(color_profile_pos, open_pos);
  EXPECT_LT(color_association_pos, open_pos);
}

TEST(VirtualDisplayProbeContract, ManifestTopologyAppliesStoredLayoutPolicy) {
  const auto source = read_probe_source();

  expect_contains(source, "if (command == \"--apply-manifest-topology\")");
  expect_contains(source, "apply_manifest_topology(client)");
  expect_contains(source, "client.query_display_manifest()");
  expect_contains(source, "profile_for_target(manifest.value, path)");
  expect_contains(source, "DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL");
  expect_contains(source, "profile.layout_policy != vdd::kDisplayManifestLayoutPolicyNone");
  expect_contains(source, "modes[*mode_index].sourceMode.position = POINTL {profile->position_x, profile->position_y}");
  expect_contains(source, "profile->layout_policy == vdd::kDisplayManifestLayoutPolicyApplyAndPersist");
  expect_contains(source, "SDC_SAVE_TO_DATABASE");
  expect_contains(source, "report_helper_event(");
  expect_contains(source, "kEventHelperTopologyApplied");
  expect_contains(source, "manifest_topology_applied=1");
  const auto manifest_topology = source.find("int apply_manifest_topology(vdd::ControlClient &client)");
  ASSERT_NE(manifest_topology, std::string::npos);
  const auto legacy_topology = source.find("void prepare_legacy_topology_path", manifest_topology);
  ASSERT_NE(legacy_topology, std::string::npos);
  EXPECT_EQ(
    source.substr(manifest_topology, legacy_topology - manifest_topology).find("apply_extended_topology"),
    std::string::npos
  );
}

TEST(VirtualDisplayProbeContract, PermanentSelfTestRestoresOriginalCount) {
  const auto source = read_probe_source();

  expect_contains(source, "const auto before = client.query_permanent_display_count()");
  expect_contains(source, "const auto changed = client.set_permanent_display_count(request)");
  expect_contains(source, "RestorePermanentCountOnExit");
  expect_contains(source, "restore_on_exit");
  expect_contains(source, "restore.display_count = before.value.current_display_count");
  expect_contains(source, "const auto restored = restore_previous_count();");
}

TEST(VirtualDisplayProbeContract, HdrSelfTestVerifiesWindowsAdvancedColorState) {
  const auto source = read_probe_source();
  const auto cmake = read_driver_cmake();

  expect_contains(source, "DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2");
  expect_contains(source, "DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE");
  expect_contains(source, "ColorProfileGetDisplayUserScope");
  expect_contains(source, "ColorProfileGetDisplayList");
  expect_contains(source, "ColorProfileGetDisplayDefault");
  expect_contains(cmake, "target_link_libraries(virtualdisplay_probe PRIVATE libvirtualdisplay::driver advapi32 mscms)");
  expect_contains(source, "kEventHelperColorQueryCompleted");
  expect_contains(source, "const auto after = wait_for_advanced_color(");
  expect_contains(source, "latest->active");
  expect_contains(source, "DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR");
  expect_contains(source, "created.value.os_adapter_luid");
  expect_contains(source, "created.value.target_id");
  expect_contains(source, "temporary display is not reported as HDR-supported by Windows");
  expect_contains(source, "temporary display did not enter HDR 10-bit mode after request");
}

TEST(VirtualDisplayProbeContract, DisplayConfigCommandsRequireInteractiveSession) {
  const auto source = read_probe_source();

  expect_contains(source, "command_uses_display_config");
  expect_contains(source, "--apply-extended-topology");
  expect_contains(source, "--apply-manifest-topology");
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
  expect_contains(source, "read_u32_arg(argc, argv, 2, 3u, \"count\", count)");
  expect_contains(source, "active.value.temporary_display_count != count");
  expect_contains(source, "std::chrono::milliseconds(active.value.effective_timeout_ms + 2'000u)");
  expect_contains(source, "multi-temp QA lease did not expire cleanly");
  expect_contains(source, "qa_multi_temp_lease=1");
}

TEST(VirtualDisplayProbeContract, TemporaryIdentityRetentionQaRequiresRestoredHdrProfile) {
  const auto source = read_probe_source();

  expect_contains(source, "if (command == \"--qa-temp-identity-retention\")");
  expect_contains(source, "read_u32_arg(argc, argv, 5, 30'000u, \"timeout_ms\", timeout_ms)");
  expect_contains(source, "const auto stable_display_id = transient_id(0x51dd1000)");
  expect_contains(source, "second.value.connector_index != first.value.connector_index");
  expect_contains(source, "second.value.target_id != first.value.target_id");
  expect_contains(source, "filler display reused retained identity connector");
  expect_contains(source, "HDR profile was not retained for recreated temporary display");
  expect_contains(source, "qa_temp_identity_retention=1");
}

TEST(VirtualDisplayProbeContract, BrokerOwnsDriverAccessBehindSecuredPipe) {
  const auto source = read_broker_source();
  const auto probe_source = read_probe_source();
  const auto cmake = read_driver_cmake();

  expect_contains(cmake, "add_executable(virtualdisplay_broker");
  expect_contains(cmake, "target_link_libraries(virtualdisplay_broker PRIVATE libvirtualdisplay::driver advapi32 userenv wtsapi32)");
  expect_contains(cmake, "target_link_libraries(virtualdisplay PRIVATE libvirtualdisplay::driver advapi32 shell32 newdev)");
  expect_contains(source, "kPipeName[] = L\"\\\\\\\\.\\\\pipe\\\\SunshineVirtualDisplayBroker\"");
  expect_contains(source, "kPipeSecurityDescriptor[] = L\"D:P(A;;GA;;;SY)(A;;GA;;;BA)\"");
  expect_contains(source, "kBrokerStateSubkey[] = L\"SOFTWARE\\\\Sunshine\\\\VirtualDisplayBroker\"");
  expect_contains(source, "kBrokerDisplayManifestValue[] = L\"DisplayManifest\"");
  expect_contains(source, "kBrokerRegistrySecurityDescriptor[] = L\"D:P(A;;GA;;;SY)(A;;GA;;;BA)\"");
  expect_contains(source, "kSessionHelperExecutable[] = L\"virtualdisplay_probe.exe\"");
  expect_contains(source, "ConvertStringSecurityDescriptorToSecurityDescriptorW");
  expect_contains(source, "RegCreateKeyExW");
  expect_contains(source, "RegSetKeySecurity");
  expect_contains(source, "RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION, security->attributes.lpSecurityDescriptor) != ERROR_SUCCESS");
  expect_contains(source, "RegQueryValueExW");
  expect_contains(source, "RegSetValueExW");
  expect_contains(source, "profile.native_mode_index < profile.allowed_mode_count");
  expect_contains(source, "kHelperProcessTimeoutMs");
  expect_contains(source, "WaitForMultipleObjects");
  expect_contains(source, "TerminateProcess(process.hProcess");
  expect_contains(source, "quoted.append(backslashes * 2 + 1, L'\\\\')");
  expect_contains(source, "quoted.append(backslashes * 2, L'\\\\')");
  expect_contains(source, "kPipeClientReadTimeoutMs");
  expect_contains(source, "PeekNamedPipe");
  expect_contains(source, "load_persisted_display_manifest");
  expect_contains(source, "save_display_manifest");
  expect_contains(source, "restore_persisted_display_manifest(client)");
  expect_contains(source, "CreateNamedPipeW");
  expect_contains(source, "ImpersonateNamedPipeClient");
  expect_contains(source, "if (!RevertToSelf())");
  expect_contains(source, "CheckTokenMembership");
  expect_contains(source, "authorize_pipe_client(pipe)");
  expect_contains(source, "open_first_control_device()");
  expect_contains(source, "query_display_state()");
  expect_contains(source, "command == \"display-query\"");
  expect_contains(source, "format_display_state(result.value)");
  expect_contains(source, "query_display_manifest()");
  expect_contains(source, "format_display_manifest(result.value)");
  expect_contains(source, "query_permanent_display_count()");
  expect_contains(source, "vdd::display_manifest_from_permanent_settings(*request");
  expect_contains(source, "client.set_display_manifest(manifest)");
  expect_contains(source, "command.starts_with(\"manifest-profile-set \")");
  expect_contains(source, "manifest.value.profiles[index].connector_index == profile->connector_index");
  expect_contains(source, "return \"error manifest_full\\n\"");
  expect_contains(source, "if (!save_display_manifest(manifest.value, current.value.max_display_count))");
  expect_contains(source, "(void) save_display_manifest(original_manifest, current.value.max_display_count)");
  expect_contains(source, "command == \"permanent-query\"");
  expect_contains(source, "command.starts_with(\"permanent-set \")");
  expect_contains(source, "helper_arguments_for_broker_command");
  expect_contains(source, "return L\"--diagnose\"");
  expect_contains(source, "return L\"--apply-extended-topology\"");
  expect_contains(source, "return L\"--apply-manifest-topology\"");
  expect_contains(source, "return L\"--query-color-profiles\"");
  expect_contains(source, "helper-associate-color-profile ");
  expect_contains(source, "--associate-color-profile ");
  expect_contains(source, "--advanced-color");
  expect_contains(source, "--default");
  expect_contains(source, "launch_console_helper(*helper_arguments)");
  expect_contains(source, "WTSGetActiveConsoleSessionId()");
  expect_contains(probe_source, "(std::min)(manifest.profile_count, vdd::kMaxPermanentDisplayProfiles)");
  expect_contains(probe_source, "color_profile_active_paths=");
  expect_contains(probe_source, "color profile query failed for every active path");
  expect_contains(probe_source, "parse_i32_token(text.substr(0, separator))");
  expect_contains(probe_source, "parse_u32_token(text.substr(separator + 1))");
  expect_contains(probe_source, "remove temporary display after query lease failed");
  expect_contains(source, "WTSQueryUserToken(session_id");
  expect_contains(source, "CreateProcessAsUserW");
  expect_contains(source, "RegisterServiceCtrlHandlerW");
  expect_contains(source, "StartServiceCtrlDispatcherW");
  expect_contains(source, "RegisterEventSourceW");
  expect_contains(source, "ReportEventW");
  expect_contains(source, "DeregisterEventSource");
  expect_contains(source, "kEventServiceStarting");
  expect_contains(source, "kEventHelperFinished");
  expect_contains(source, "--run-console");
  expect_contains(source, "--service");

  const auto connected = source.find("if (connected && !g_context.stop_requested.load(std::memory_order_acquire))");
  const auto authorize = source.find("authorize_pipe_client(pipe)", connected);
  const auto serve = source.find("serve_pipe_client(pipe, client)", connected);
  const auto denied = source.find("\"error access_denied\\n\"", connected);
  ASSERT_NE(connected, std::string::npos);
  ASSERT_NE(authorize, std::string::npos);
  ASSERT_NE(serve, std::string::npos);
  ASSERT_NE(denied, std::string::npos);
  EXPECT_LT(authorize, serve);
  EXPECT_LT(authorize, denied);
}
