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

  std::string read_windows_driver_cmake() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "src/driver/windows_driver/CMakeLists.txt";
    std::ifstream file {path, std::ios::binary};
    if (!file) {
      ADD_FAILURE() << "Failed to open " << path.string();
      return {};
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  std::string read_readme() {
    const auto path = std::filesystem::path {LIBVIRTUALDISPLAY_SOURCE_DIR} /
                      "README.md";
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

TEST(VirtualDisplayWindowsDriverContract, LeavesPermanentPolicyPersistenceToBroker) {
  const auto source = read_windows_driver_source();

  EXPECT_EQ(source.find("kDisplayManifestValue"), std::string::npos);
  EXPECT_EQ(source.find("load_persistent_display_manifest"), std::string::npos);
  EXPECT_EQ(source.find("save_display_manifest(driver_, device_, manifest)"), std::string::npos);
  EXPECT_EQ(source.find("persisted_manifest_"), std::string::npos);
  EXPECT_NE(source.find("vdd::BackendError apply_display_manifest(const vdd::DisplayManifest &manifest) override"), std::string::npos);
  EXPECT_NE(source.find("permanent_display_count_ = manifest.profile_count;"), std::string::npos);
  EXPECT_NE(source.find("load_temporary_connector_reservations(driver, device)"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, SetsSwapChainDeviceFromProcessingThread) {
  const auto source = read_windows_driver_source();

  const auto start = source.find("HRESULT start(const LUID &render_adapter_luid)");
  ASSERT_NE(start, std::string::npos);
  const auto process_call = source.find("process_frames(render_adapter_luid);", start);
  ASSERT_NE(process_call, std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto reset = source.find("reset_render_device(render_adapter_luid);", process_frames);
  ASSERT_NE(reset, std::string::npos);

  const auto assign = source.find("HRESULT assign_swapchain_device()");
  ASSERT_NE(assign, std::string::npos);
  const auto set_device = source.find("IddCxSwapChainSetDevice(swapchain_, &set_device);", assign);
  ASSERT_NE(set_device, std::string::npos);
  EXPECT_NE(source.find("HandleNewSwapChain still owns IddCx's internal OPM cleanup", assign), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RegistersSwapChainWorkerWithMmcss) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("#include <avrt.h>"), std::string::npos);
  EXPECT_NE(source.find("kSwapchainMmcssTask[] = L\"DisplayPostProcessing\""), std::string::npos);
  EXPECT_NE(source.find("AvSetMmThreadCharacteristicsW(task_name, &task_index_)"), std::string::npos);
  EXPECT_NE(source.find("AvRevertMmThreadCharacteristics(handle_)"), std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto mmcss = source.find("MmcssRegistration mmcss {kSwapchainMmcssTask};", process_frames);
  ASSERT_NE(mmcss, std::string::npos);
  const auto create_device = source.find("reset_render_device(render_adapter_luid);", process_frames);
  ASSERT_NE(create_device, std::string::npos);
  EXPECT_LT(mmcss, create_device);
}

TEST(VirtualDisplayWindowsDriverContract, RecoversRenderDeviceBeforeAbandoningSwapChain) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("D3D_DRIVER_TYPE_WARP"), std::string::npos);
  EXPECT_NE(source.find("is_device_lost_hresult"), std::string::npos);
  EXPECT_NE(source.find("DXGI_ERROR_DEVICE_REMOVED"), std::string::npos);
  EXPECT_NE(source.find("DXGI_ERROR_DEVICE_RESET"), std::string::npos);

  const auto process_frames = source.find("void process_frames(const LUID render_adapter_luid)");
  ASSERT_NE(process_frames, std::string::npos);
  const auto acquire_failure = source.find("if (FAILED(acquire_result))", process_frames);
  ASSERT_NE(acquire_failure, std::string::npos);
  const auto device_lost = source.find("is_device_lost_hresult(acquire_result)", acquire_failure);
  ASSERT_NE(device_lost, std::string::npos);
  const auto reset = source.find("reset_render_device(render_adapter_luid)", device_lost);
  ASSERT_NE(reset, std::string::npos);
  const auto abandon = source.find("return;", reset);
  ASSERT_NE(abandon, std::string::npos);
  EXPECT_LT(reset, abandon);
}

TEST(VirtualDisplayWindowsDriverContract, RegistersTraceLoggingProvider) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("#include <TraceLoggingProvider.h>"), std::string::npos);
  EXPECT_NE(source.find("TRACELOGGING_DEFINE_PROVIDER("), std::string::npos);
  EXPECT_NE(source.find("TraceLoggingRegister(g_trace_provider);"), std::string::npos);
  EXPECT_NE(source.find("config.EvtDriverUnload = SunshineEvtDriverUnload;"), std::string::npos);
  EXPECT_NE(source.find("TraceLoggingUnregister(g_trace_provider);"), std::string::npos);
  EXPECT_NE(source.find("TraceLoggingWrite("), std::string::npos);
  EXPECT_NE(source.find("\"DeviceIoControl\""), std::string::npos);
  EXPECT_NE(source.find("\"RenderDeviceLost\""), std::string::npos);
  EXPECT_NE(source.find("\"RenderDeviceCreated\""), std::string::npos);
  EXPECT_NE(source.find("\"MonitorArrived\""), std::string::npos);
  EXPECT_NE(source.find("\"MonitorDeparted\""), std::string::npos);
  EXPECT_NE(source.find("\"SwapChainAssigned\""), std::string::npos);
  EXPECT_NE(source.find("\"SwapChainUnassigned\""), std::string::npos);
  EXPECT_NE(source.find("\"DefaultHdrMetadataSet\""), std::string::npos);
  EXPECT_NE(source.find("\"GammaRampSet\""), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, RecordsAdvancedColorCallbacksPerMonitor) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("IDDCX_DEFAULT_HDR_METADATA_TYPE default_hdr_metadata_type"), std::string::npos);
  EXPECT_NE(source.find("IDDCX_GAMMARAMP_TYPE gamma_ramp_type"), std::string::npos);
  EXPECT_NE(source.find("NTSTATUS set_default_hdr_metadata("), std::string::npos);
  EXPECT_NE(source.find("record->second.default_hdr_metadata_type = args->Type;"), std::string::npos);
  EXPECT_NE(source.find("record->second.default_hdr_metadata_size = args->Size;"), std::string::npos);
  EXPECT_NE(source.find("NTSTATUS set_gamma_ramp("), std::string::npos);
  EXPECT_NE(source.find("record->second.gamma_ramp_type = args->Type;"), std::string::npos);
  EXPECT_NE(source.find("record->second.gamma_ramp_size = args->GammaRampSizeInBytes;"), std::string::npos);
  EXPECT_NE(source.find("return context->backend->set_default_hdr_metadata(monitor, args);"), std::string::npos);
  EXPECT_NE(source.find("return context->backend->set_gamma_ramp(monitor, args);"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, LinksTraceLoggingRuntime) {
  const auto cmake = read_windows_driver_cmake();
  EXPECT_NE(cmake.find("advapi32"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, PackagesDriverSymbolsWithReleaseZip) {
  const auto cmake = read_windows_driver_cmake();
  const auto readme = read_readme();

  EXPECT_NE(
    cmake.find("install(FILES \"$<TARGET_PDB_FILE:${SUNSHINE_DRIVER_TARGET}>\" DESTINATION driver OPTIONAL)"),
    std::string::npos
  );
  EXPECT_NE(
    readme.find("driver/SunshineVirtualDisplayDriver.pdb"),
    std::string::npos
  );
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
  EXPECT_NE(source.find("options.physical_width_mm = settings.physical_width_mm;"), std::string::npos);
  EXPECT_NE(source.find("options.physical_height_mm = settings.physical_height_mm;"), std::string::npos);
  EXPECT_NE(source.find("options.refresh_rate_millihz = settings.refresh_rate_millihz;"), std::string::npos);
  EXPECT_NE(source.find("options.monitor_name = vdd::trim_display_name(settings.display_name);"), std::string::npos);
  EXPECT_NE(source.find("vdd::set_default_permanent_display_settings(normalized);"), std::string::npos);
  EXPECT_NE(source.find("make_permanent_descriptor(const vdd::DisplayManifestProfile &profile)"), std::string::npos);
  EXPECT_NE(source.find("profile.manufacturer_id[0]"), std::string::npos);
  EXPECT_NE(source.find("options.product_code = static_cast<std::uint16_t>(profile.product_code);"), std::string::npos);
  EXPECT_NE(source.find("vdd::BackendError apply_display_manifest(const vdd::DisplayManifest &manifest) override"), std::string::npos);
}

TEST(VirtualDisplayWindowsDriverContract, PermanentManifestUpdatesRestorePreviousDisplaysOnFailure) {
  const auto source = read_windows_driver_source();

  EXPECT_NE(source.find("return apply_display_manifest(vdd::display_manifest_from_permanent_settings(normalized, kMaxPermanentDisplays));"), std::string::npos);
  EXPECT_NE(source.find("std::vector<vdd::DisplayDescriptor> active_permanent_descriptors"), std::string::npos);
  EXPECT_NE(source.find("active_permanent_descriptors.push_back(record.descriptor);"), std::string::npos);
  EXPECT_NE(source.find("std::vector<vdd::DisplayDescriptor> departed;"), std::string::npos);
  EXPECT_NE(source.find("departed.push_back(descriptor);"), std::string::npos);
  EXPECT_NE(source.find("(void) arrive_display(restore_descriptor, true);"), std::string::npos);
  EXPECT_EQ(source.find("save_display_manifest(driver_, device_, manifest)"), std::string::npos);
}
