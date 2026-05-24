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

TEST(VirtualDisplayWindowsDriverContract, StopsAndDeletesSwapChainBeforeDeparture) {
  const auto source = read_windows_driver_source();

  const auto depart = source.find("vdd::BackendError depart_display");
  ASSERT_NE(depart, std::string::npos);

  const auto stop = source.find("processor_to_stop->stop();", depart);
  ASSERT_NE(stop, std::string::npos);

  const auto departure = source.find("IddCxMonitorDeparture(monitor_handle);", stop);
  ASSERT_NE(departure, std::string::npos);
  EXPECT_LT(stop, departure);

  const auto cleanup = source.find("processor_to_stop.reset();", stop);
  ASSERT_NE(cleanup, std::string::npos);
  EXPECT_LT(cleanup, departure);
  EXPECT_NE(source.find("IddCxMonitorDeparture can invalidate active swapchain objects", depart), std::string::npos);

  const auto cleanup_owner = source.find("void delete_swapchain()");
  ASSERT_NE(cleanup_owner, std::string::npos);

  const auto delete_call = source.find("WdfObjectDelete(reinterpret_cast<WDFOBJECT>(swapchain_));", cleanup_owner);
  ASSERT_NE(delete_call, std::string::npos);

  const auto clear = source.find("swapchain_ = nullptr;", delete_call);
  ASSERT_NE(clear, std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, AbandonsSwapChainAssignedDuringDeparture) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN"), std::string::npos);
  EXPECT_NE(source.find("generic failures trip verifier 0x700"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, UsesDigitalSinkTechnologyForHdrClassification) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI"), std::string::npos);
  EXPECT_NE(source.find("WCG-only"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, SetsSwapChainDeviceFromProcessingThread) {
  const auto source = read_windows_driver_source();

  const auto start = source.find("HRESULT start(const LUID &render_adapter_luid)");
  ASSERT_NE(start, std::string::npos);
  const auto process_call = source.find("process_frames(render_adapter_luid);", start);
  ASSERT_NE(process_call, std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto set_device = source.find("IddCxSwapChainSetDevice(swapchain_, &set_device);", process_frames);
  ASSERT_NE(set_device, std::string::npos);
  EXPECT_NE(source.find("HandleNewSwapChain still owns IddCx's internal OPM cleanup", process_frames), std::string::npos);
}
