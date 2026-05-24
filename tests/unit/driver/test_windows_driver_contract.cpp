#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {
  std::string read_windows_driver_source() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/windows_driver/driver_main.cpp";
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }
}  // namespace

TEST(VirtualDisplayWindowsDriverContract, DeletesMonitorObjectWhenArrivalFails) {
  const auto source = read_windows_driver_source();

  const auto arrival = source.find("status = IddCxMonitorArrival(record.monitor, &arrival_out);");
  ASSERT_NE(arrival, std::string::npos);

  const auto failure = source.find("if (!NT_SUCCESS(status))", arrival);
  ASSERT_NE(failure, std::string::npos);

  const auto cleanup = source.find("WdfObjectDelete(record.monitor);", failure);
  ASSERT_NE(cleanup, std::string::npos);

  const auto backend_failure = source.find("return {vdd::BackendError::Failed", failure);
  ASSERT_NE(backend_failure, std::string::npos);
  EXPECT_LT(cleanup, backend_failure);
}
