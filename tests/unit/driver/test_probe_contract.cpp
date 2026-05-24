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

  void expect_contains(const std::string &content, const std::string &needle) {
    EXPECT_NE(content.find(needle), std::string::npos) << "missing: " << needle;
  }
}  // namespace

TEST(VirtualDisplayProbeContract, ExposesTemporaryAndPermanentRuntimeChecks) {
  const auto source = read_probe_source();

  expect_contains(source, "--diagnose");
  expect_contains(source, "--check");
  expect_contains(source, "--query-permanent");
  expect_contains(source, "--set-permanent <count>");
  expect_contains(source, "--self-test-permanent [count]");
  expect_contains(source, "--self-test-temp [width height refresh_hz]");
  expect_contains(source, "--self-test-hdr [width height refresh_hz]");
}

TEST(VirtualDisplayProbeContract, DiagnoseRunsBeforeControlDeviceOpen) {
  const auto source = read_probe_source();

  expect_contains(source, "vdd::enumerate_control_devices(&enumerate_error)");
  expect_contains(source, "control_interface_count=");
  expect_contains(source, "if (command == \"--diagnose\")");

  const auto command_pos = source.find("const std::string command {argv[1]}");
  const auto diagnose_pos = source.find("if (command == \"--diagnose\")");
  const auto open_pos = source.find("auto opened = vdd::open_first_control_device()");
  ASSERT_NE(command_pos, std::string::npos);
  ASSERT_NE(diagnose_pos, std::string::npos);
  ASSERT_NE(open_pos, std::string::npos);
  EXPECT_LT(command_pos, open_pos);
  EXPECT_LT(diagnose_pos, open_pos);
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

  expect_contains(source, "DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO_2");
  expect_contains(source, "DISPLAYCONFIG_DEVICE_INFO_SET_HDR_STATE");
  expect_contains(source, "wait_for_advanced_color(created.value.os_adapter_luid, created.value.target_id, true)");
  expect_contains(source, "temporary display is not reported as HDR-supported by Windows");
  expect_contains(source, "temporary display did not enter HDR 10-bit mode after request");
}
