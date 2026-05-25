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

  std::string read_windows_driver_inf() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/windows_driver/SunshineVirtualDisplayDriver.inf";
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

  const auto arrival = source.find("status = IddCxMonitorArrival(create_out.MonitorObject, &arrival_out);");
  ASSERT_NE(arrival, std::string::npos);

  const auto failure = source.find("if (!NT_SUCCESS(status))", arrival);
  ASSERT_NE(failure, std::string::npos);

  const auto cleanup = source.find("WdfObjectDelete(create_out.MonitorObject);", failure);
  ASSERT_NE(cleanup, std::string::npos);

  const auto backend_failure = source.find("return {vdd::BackendError::Failed", failure);
  ASSERT_NE(backend_failure, std::string::npos);
  EXPECT_LT(cleanup, backend_failure);
}

TEST(VirtualDisplayWindowsDriverContract, StopsAndAbandonsSwapChainBeforeDeparture) {
  const auto source = read_windows_driver_source();

  const auto depart = source.find("vdd::BackendError depart_display");
  ASSERT_NE(depart, std::string::npos);

  const auto stop = source.find("stop_swapchain_processor_without_delete(processor_to_stop);", depart);
  ASSERT_NE(stop, std::string::npos);

  const auto departure = source.find("IddCxMonitorDeparture(monitor_handle);", stop);
  ASSERT_NE(departure, std::string::npos);
  EXPECT_LT(stop, departure);
  EXPECT_NE(source.find("IddCxMonitorDeparture can invalidate swapchain objects", depart), std::string::npos);

  const auto helper = source.find("void stop_swapchain_processor_without_delete");
  ASSERT_NE(helper, std::string::npos);
  const auto abandon = source.find("processor->abandon_swapchain();", helper);
  ASSERT_NE(abandon, std::string::npos);

  const auto cleanup = source.find("processor.reset();", abandon);
  ASSERT_NE(cleanup, std::string::npos);
  EXPECT_LT(abandon, cleanup);
}

TEST(VirtualDisplayWindowsDriverContract, AbandonsSwapChainAssignedDuringDeparture) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("STATUS_GRAPHICS_INDIRECT_DISPLAY_ABANDON_SWAPCHAIN"), std::string::npos);
  EXPECT_NE(source.find("generic failures trip verifier 0x700"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, UsesDigitalSinkTechnologyForHdrClassification) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL"), std::string::npos);
  EXPECT_NE(source.find("legacy bandwidth ceiling"), std::string::npos);
  EXPECT_NE(source.find("WCG-only"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, StoresTemporaryIdentityInWdfPersistentState) {
  const auto source = read_windows_driver_source();
  const auto inf = read_windows_driver_inf();

  EXPECT_NE(source.find("WdfDriverOpenPersistentStateRegistryKey(driver, desired_access"), std::string::npos);
  EXPECT_NE(source.find("WdfDeviceOpenRegistryKey("), std::string::npos);
  EXPECT_NE(source.find("KEY_READ"), std::string::npos);
  EXPECT_NE(source.find("KEY_SET_VALUE"), std::string::npos);
  EXPECT_NE(source.find("kPersistentStateSchemaVersion"), std::string::npos);
  EXPECT_NE(source.find("TemporaryDisplayProfiles"), std::string::npos);
  EXPECT_NE(source.find("REG_BINARY"), std::string::npos);
  EXPECT_NE(source.find("entry.connector_index == descriptor.connector_index"), std::string::npos);
  EXPECT_EQ(source.find("KEY_ALL_ACCESS"), std::string::npos);
  EXPECT_EQ(source.find("KEY_WRITE"), std::string::npos);
  EXPECT_EQ(source.find("WdfRegistryCreateKey"), std::string::npos);
  EXPECT_EQ(source.find("RegEnumKeyExW"), std::string::npos);
  EXPECT_EQ(source.find("SOFTWARE\\\\Sunshine\\\\VirtualDisplayDriver"), std::string::npos);
  EXPECT_EQ(source.find("RegCreateKeyExW"), std::string::npos);
  EXPECT_NE(inf.find("HKR,,\"ConfigVersion\",0x00010001,1"), std::string::npos);
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

TEST(VirtualDisplayWindowsDriverContract, AbandonsInvalidatedSwapchainHandlesDuringTeardown) {
  const auto source = read_windows_driver_source();

  const auto helper = source.find("void stop_swapchain_processor_without_delete");
  ASSERT_NE(helper, std::string::npos);
  EXPECT_NE(source.find("processor->stop();", helper), std::string::npos);
  EXPECT_NE(source.find("processor->abandon_swapchain();", helper), std::string::npos);

  const auto depart_display = source.find("vdd::BackendError depart_display");
  ASSERT_NE(depart_display, std::string::npos);
  EXPECT_NE(source.find("stop_swapchain_processor_without_delete(processor_to_stop);", depart_display), std::string::npos);
  EXPECT_NE(source.find("stop_swapchain_processors_without_delete(retired_processors_to_stop);", depart_display), std::string::npos);

  const auto unassign_swapchain = source.find("NTSTATUS unassign_swapchain");
  ASSERT_NE(unassign_swapchain, std::string::npos);
  EXPECT_NE(
    source.find("retired_processors_to_stop = std::move(record->second.retired_swapchain_processors);", unassign_swapchain),
    std::string::npos
  );
  EXPECT_NE(source.find("stop_swapchain_processor_without_delete(processor_to_stop);", unassign_swapchain), std::string::npos);
  EXPECT_NE(
    source.find("stop_swapchain_processors_without_delete(retired_processors_to_stop);", unassign_swapchain),
    std::string::npos
  );
}

TEST(VirtualDisplayWindowsDriverContract, TargetModesUseRequestedDescriptorTiming) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("ModeShape mode_shape_from_descriptor(const vdd::DisplayDescriptor &descriptor)"), std::string::npos);
  EXPECT_NE(source.find("descriptor.refresh_rate_millihz"), std::string::npos);
  EXPECT_EQ(source.find("caps.MaxDisplayPipelineRate = kMaxDisplayPipelineRate;"), std::string::npos);
  EXPECT_EQ(source.find("mode.RequiredBandwidth = shape.pixel_rate;"), std::string::npos);

  const auto query_target_modes = source.find("NTSTATUS query_target_modes(");
  ASSERT_NE(query_target_modes, std::string::npos);

  const auto requested_shape = source.find("requested_mode_shape(monitor)", query_target_modes);
  ASSERT_NE(requested_shape, std::string::npos);

  const auto fill_with_requested_shape = source.find("fill_target_modes(input, output, &*requested_shape)", requested_shape);
  ASSERT_NE(fill_with_requested_shape, std::string::npos);

  const auto query_target_modes2 = source.find("NTSTATUS query_target_modes2(");
  ASSERT_NE(query_target_modes2, std::string::npos);

  const auto requested_shape2 = source.find("requested_mode_shape(monitor)", query_target_modes2);
  ASSERT_NE(requested_shape2, std::string::npos);

  const auto fill_with_requested_shape2 = source.find("fill_target_modes2(input, output, &*requested_shape)", requested_shape2);
  ASSERT_NE(fill_with_requested_shape2, std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, PermanentDisplaysUseControlPlaneModeSettings) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("make_permanent_descriptor("), std::string::npos);
  EXPECT_NE(source.find("options.width = settings.width;"), std::string::npos);
  EXPECT_NE(source.find("options.height = settings.height;"), std::string::npos);
  EXPECT_NE(source.find("options.refresh_rate_millihz = settings.refresh_rate_millihz;"), std::string::npos);
  EXPECT_NE(source.find("options.monitor_name = vdd::trim_display_name(settings.display_name);"), std::string::npos);
  EXPECT_NE(source.find("vdd::set_default_permanent_display_settings(normalized);"), std::string::npos);
}
