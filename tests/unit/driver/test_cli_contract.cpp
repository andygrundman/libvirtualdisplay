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
  expect_contains(source, "spawn [--width N] [--height N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent query");
  expect_contains(source, "permanent set --count N [--width N] [--height N] [--refresh HZ] [--name TEXT]");
  expect_contains(source, "permanent off");
  expect_contains(source, "client.set_permanent_display_count(request)");
}

TEST(VirtualDisplayCliContract, PermanentCommandsApplyModeAndNameSettings) {
  const auto source = read_cli_source();

  expect_contains(source, "request.width = options.width");
  expect_contains(source, "request.height = options.height");
  expect_contains(source, "request.refresh_rate_millihz = options.refresh_rate_millihz");
  expect_contains(source, "set_display_name(request.display_name, options.name)");
  expect_contains(source, "parse_refresh_millihz");
}
