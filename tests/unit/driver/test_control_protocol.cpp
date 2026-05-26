#include <gtest/gtest.h>
#include "virtual_display/driver/control_protocol.h"
#include "virtual_display/driver/windows_control_protocol.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace vdd = virtual_display::driver;

namespace {
  constexpr std::uint64_t lease_id(const std::uint64_t suffix) {
    return vdd::kMinOpaqueLeaseId | suffix;
  }

  vdd::CreateTemporaryDisplayRequest valid_create_request() {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = lease_id(10);
    request.display_id = 20;
    request.width = 2560;
    request.height = 1440;
    request.physical_width_mm = 600;
    request.physical_height_mm = 340;
    request.refresh_rate_millihz = 120'000;
    request.requested_timeout_ms = 30'000;
    std::memcpy(request.display_name, "Sunshine Display", 16);
    return request;
  }

  vdd::DisplayManifest valid_display_manifest() {
    vdd::DisplayManifest manifest {};
    manifest.profile_count = 1;
    manifest.max_profile_count = 2;
    auto &profile = manifest.profiles[0];
    profile.flags = vdd::kDisplayManifestProfileFlagHdrSupported |
      vdd::kDisplayManifestProfileFlagRetainIdentity |
      vdd::kDisplayManifestProfileFlagPermanentIdentity;
    profile.connector_index = 0;
    profile.display_id = 0x7000000000000000ull;
    profile.container_id = {
      0x9161d0d5,
      0x7000,
      0x0000,
      {0x93, 0x7d, 0x00, 0x00, 0x00, 0x00, 0x53, 0x44}
    };
    std::memcpy(profile.manufacturer_id, "SDD", 4);
    profile.product_code = 0x4000;
    profile.serial_number = 1;
    profile.physical_width_mm = 700;
    profile.physical_height_mm = 390;
    profile.native_mode_index = 0;
    profile.allowed_mode_count = 2;
    profile.layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist;
    profile.position_x = 1920;
    profile.position_y = -100;
    profile.orientation = vdd::kDisplayManifestOrientationDefault;
    profile.allowed_modes[0] = {3840, 2160, 144'000};
    profile.allowed_modes[1] = {2560, 1440, 120'000};
    std::memcpy(profile.display_name, "Desk Display", 13);
    return manifest;
  }

  std::string read_readme() {
    const std::string readme_path =
      std::string {LIBVIRTUALDISPLAY_SOURCE_DIR} +
      "/README.md";
    std::ifstream readme_file {readme_path};
    if (!readme_file.is_open()) {
      ADD_FAILURE() << readme_path;
      return {};
    }

    std::ostringstream buffer;
    buffer << readme_file.rdbuf();
    return buffer.str();
  }
}  // namespace

TEST(VirtualDisplayDriverControlProtocol, ComputesBufferedUnknownDeviceIoctlCodes) {
  EXPECT_EQ(vdd::kIoctlGetProtocolVersion, 0x00222400u);
  EXPECT_EQ(vdd::kIoctlCreateTemporaryDisplay, 0x0022e404u);
  EXPECT_EQ(vdd::kIoctlRemoveTemporaryDisplay, 0x0022e408u);
  EXPECT_EQ(vdd::kIoctlFeedLease, 0x0022e40cu);
  EXPECT_EQ(vdd::kIoctlReleaseLease, 0x0022e410u);
  EXPECT_EQ(vdd::kIoctlQueryLease, 0x0022e414u);
  EXPECT_EQ(vdd::kIoctlSetPermanentDisplayCount, 0x0022e418u);
  EXPECT_EQ(vdd::kIoctlQueryPermanentDisplayCount, 0x0022e41cu);
  EXPECT_EQ(vdd::kIoctlQueryDisplayState, 0x00226420u);
  EXPECT_EQ(vdd::kIoctlSetDisplayManifest, 0x0022e424u);
  EXPECT_EQ(vdd::kIoctlQueryDisplayManifest, 0x00226428u);
}

TEST(VirtualDisplayDriverControlProtocol, ProtocolVersionUsesDedicatedNamespace) {
  const vdd::ProtocolVersion version {};

  EXPECT_EQ(version.api_namespace, vdd::kApiNamespaceGuid);
  EXPECT_EQ(version.major, vdd::kProtocolVersionMajor);
  EXPECT_EQ(version.minor, vdd::kProtocolVersionMinor);
  EXPECT_EQ(version.patch, vdd::kProtocolVersionPatch);
}

TEST(VirtualDisplayDriverControlProtocol, ReadmeDocumentsCurrentProtocolVersion) {
  const auto readme = read_readme();
  const std::string expected_version =
    std::to_string(vdd::kProtocolVersionMajor) + "." +
    std::to_string(vdd::kProtocolVersionMinor) + "." +
    std::to_string(vdd::kProtocolVersionPatch);

  EXPECT_NE(
    readme.find("The current protocol version is `" + expected_version + "`."),
    std::string::npos
  );
}

TEST(VirtualDisplayDriverControlProtocol, WindowsGuidAdapterPreservesProtocolGuid) {
#ifdef _WIN32
  const auto win_guid = vdd::to_windows_guid(vdd::kApiNamespaceGuid);

  EXPECT_EQ(vdd::from_windows_guid(win_guid), vdd::kApiNamespaceGuid);
#endif
}

TEST(VirtualDisplayDriverControlProtocol, InfRegistersControlInterfaceWithServiceOnlyAccess) {
  const std::string inf_path =
    std::string {LIBVIRTUALDISPLAY_SOURCE_DIR} +
    "/src/driver/windows_driver/SunshineVirtualDisplayDriver.inf";
  std::ifstream inf_file {inf_path};
  ASSERT_TRUE(inf_file.is_open()) << inf_path;

  std::ostringstream buffer;
  buffer << inf_file.rdbuf();
  const auto inf = buffer.str();

  EXPECT_NE(
    inf.find("AddInterface={5f894d6c-3a69-48a2-86ef-e4c671932d63},,ControlInterface"),
    std::string::npos
  );
  EXPECT_NE(inf.find("[ControlInterface_AddReg]"), std::string::npos);
  EXPECT_NE(
    inf.find("HKR,,Security,,\"D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)\""),
    std::string::npos
  );
  EXPECT_EQ(inf.find("(A;;GA;;;BA)"), std::string::npos);
  EXPECT_EQ(inf.find("(A;;GRGW;;;AU)"), std::string::npos);
}

TEST(VirtualDisplayDriverControlProtocol, InfRestrictsDeviceSecurityToSystemAndBrokerService) {
  const std::string inf_path =
    std::string {LIBVIRTUALDISPLAY_SOURCE_DIR} +
    "/src/driver/windows_driver/SunshineVirtualDisplayDriver.inf";
  std::ifstream inf_file {inf_path};
  ASSERT_TRUE(inf_file.is_open()) << inf_path;

  std::ostringstream buffer;
  buffer << inf_file.rdbuf();
  const auto inf = buffer.str();

  EXPECT_NE(
    inf.find("HKR,,Security,,\"D:P(A;;GA;;;SY)(A;;GA;;;S-1-5-80-2333729190-1599198784-3320592948-2337414441-3098439965)\""),
    std::string::npos
  );
  EXPECT_EQ(inf.find("(A;;GA;;;BA)"), std::string::npos);
  EXPECT_EQ(inf.find("(A;;GRGW;;;WD)"), std::string::npos);
  EXPECT_EQ(inf.find("(A;;GA;;;WD)"), std::string::npos);
  EXPECT_EQ(inf.find("(A;;GA;;;AU)"), std::string::npos);
}

TEST(VirtualDisplayDriverControlProtocol, NormalizesLeaseTimeouts) {
  EXPECT_EQ(vdd::normalize_timeout_ms(0), vdd::kDefaultLeaseTimeoutMs);
  EXPECT_EQ(vdd::normalize_timeout_ms(1), vdd::kMinLeaseTimeoutMs);
  EXPECT_EQ(vdd::normalize_timeout_ms(vdd::kMinLeaseTimeoutMs + 1), vdd::kMinLeaseTimeoutMs + 1);
  EXPECT_EQ(vdd::normalize_timeout_ms(vdd::kMaxLeaseTimeoutMs + 1), vdd::kMaxLeaseTimeoutMs);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesCreateRequest) {
  const auto request = valid_create_request();
  vdd::ValidatedCreateTemporaryDisplay validated {};

  EXPECT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);
  EXPECT_EQ(validated.effective_timeout_ms, request.requested_timeout_ms);
  EXPECT_EQ(validated.request.physical_width_mm, 600u);
  EXPECT_EQ(validated.request.physical_height_mm, 340u);
  EXPECT_EQ(validated.display_name, "Sunshine Display");
}

TEST(VirtualDisplayDriverControlProtocol, ValidatedCreateCopiesRebindDisplayNameView) {
  const auto request = valid_create_request();
  vdd::ValidatedCreateTemporaryDisplay validated {};
  ASSERT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);

  const auto copied = validated;
  ASSERT_EQ(copied.display_name, "Sunshine Display");
  EXPECT_EQ(copied.display_name.data(), copied.request.display_name);
  EXPECT_NE(copied.display_name.data(), validated.display_name.data());

  auto assigned = vdd::ValidatedCreateTemporaryDisplay {};
  assigned = validated;
  ASSERT_EQ(assigned.display_name, "Sunshine Display");
  EXPECT_EQ(assigned.display_name.data(), assigned.request.display_name);
  EXPECT_NE(assigned.display_name.data(), validated.display_name.data());
}

TEST(VirtualDisplayDriverControlProtocol, DefaultsCreatePhysicalSize) {
  auto request = valid_create_request();
  request.physical_width_mm = 0;
  request.physical_height_mm = 0;
  vdd::ValidatedCreateTemporaryDisplay validated {};

  EXPECT_EQ(vdd::validate_create_temporary_display(request, &validated), vdd::ValidationError::None);
  EXPECT_EQ(validated.request.physical_width_mm, vdd::kDefaultPhysicalWidthMillimeters);
  EXPECT_EQ(validated.request.physical_height_mm, vdd::kDefaultPhysicalHeightMillimeters);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsWrongNamespace) {
  auto request = valid_create_request();
  request.api_namespace.data1 ^= 1;

  EXPECT_EQ(
    vdd::validate_create_temporary_display(request),
    vdd::ValidationError::WrongApiNamespace
  );
}

TEST(VirtualDisplayDriverControlProtocol, RejectsMissingIdentifiers) {
  auto request = valid_create_request();
  request.lease_id = 0;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::MissingLeaseId);

  request = valid_create_request();
  request.display_id = 0;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::MissingDisplayId);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsOutOfRangeMode) {
  auto request = valid_create_request();
  request.flags = 0x80000000u;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidFlags);

  request = valid_create_request();
  request.width = vdd::kMinWidth - 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidWidth);

  request = valid_create_request();
  request.height = vdd::kMaxHeight + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidHeight);

  request = valid_create_request();
  request.physical_width_mm = vdd::kMaxPhysicalSizeMillimeters + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidPhysicalSize);

  request = valid_create_request();
  request.refresh_rate_millihz = vdd::kMaxRefreshRateMilliHz + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidRefreshRate);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsBlankDisplayName) {
  auto request = valid_create_request();
  std::memset(request.display_name, ' ', sizeof(request.display_name));

  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidDisplayName);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesLeaseRequests) {
  const vdd::LeaseRequest request {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    0,
    0
  };
  const vdd::LeaseDisplayRequest display_request {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    2
  };

  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::None);
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::None);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsInvalidLeaseRequests) {
  auto request = vdd::LeaseRequest {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    0,
    0
  };
  request.api_namespace.data1 ^= 1;
  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::WrongApiNamespace);

  request = vdd::LeaseRequest {
    vdd::kApiNamespaceGuid,
    0,
    0,
    0
  };
  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::MissingLeaseId);

  auto display_request = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    2
  };
  display_request.api_namespace.data1 ^= 1;
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::WrongApiNamespace);

  display_request = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    0,
    2
  };
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::MissingLeaseId);

  display_request = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    lease_id(1),
    0
  };
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::MissingDisplayId);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesPermanentDisplayCount) {
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;

  EXPECT_EQ(vdd::validate_permanent_display_count(request, 2), vdd::ValidationError::None);

  request.display_count = 3;
  EXPECT_EQ(
    vdd::validate_permanent_display_count(request, 2),
    vdd::ValidationError::PermanentDisplayCountTooHigh
  );

  request = {};
  request.api_namespace.data1 ^= 1;
  EXPECT_EQ(
    vdd::validate_permanent_display_count(request, 2),
    vdd::ValidationError::WrongApiNamespace
  );
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesPermanentDisplaySettings) {
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 1;
  request.width = 3840;
  request.height = 2160;
  request.physical_width_mm = 700;
  request.physical_height_mm = 390;
  request.refresh_rate_millihz = 144'000;
  std::memcpy(request.display_name, "Desk Display", 13);

  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::None);

  request.width = vdd::kMinWidth - 1;
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidWidth);

  request.width = 3840;
  request.physical_height_mm = vdd::kMinPhysicalSizeMillimeters - 1;
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidPhysicalSize);

  request.physical_height_mm = 390;
  request.refresh_rate_millihz = vdd::kMaxRefreshRateMilliHz + 1;
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidRefreshRate);

  request.refresh_rate_millihz = 144'000;
  std::memset(request.display_name, ' ', sizeof(request.display_name));
  EXPECT_EQ(vdd::validate_permanent_display_count(request, 4), vdd::ValidationError::InvalidDisplayName);
}

TEST(VirtualDisplayDriverControlProtocol, ValidatesDisplayManifest) {
  auto manifest = valid_display_manifest();

  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::None);

  manifest.version = vdd::kDisplayManifestVersion + 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidManifestVersion);

  manifest = valid_display_manifest();
  manifest.profiles[0].flags &= ~vdd::kDisplayManifestProfileFlagPermanentIdentity;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidFlags);

  manifest = valid_display_manifest();
  std::memcpy(manifest.profiles[0].manufacturer_id, "sdd", 4);
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidManufacturerId);

  manifest = valid_display_manifest();
  manifest.profiles[0].connector_index = 2;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidConnectorIndex);

  manifest = valid_display_manifest();
  manifest.profiles[0].allowed_mode_count = 0;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidModeCount);

  manifest = valid_display_manifest();
  manifest.profiles[0].allowed_modes[0].refresh_rate_millihz = vdd::kMaxRefreshRateMilliHz + 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidRefreshRate);

  manifest = valid_display_manifest();
  manifest.profiles[0].layout_policy = vdd::kDisplayManifestLayoutPolicyApplyAndPersist + 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidLayoutPolicy);

  manifest = valid_display_manifest();
  manifest.profiles[0].orientation = vdd::kDisplayManifestOrientationDefault + 1;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::InvalidOrientation);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsDuplicateManifestIdentities) {
  auto manifest = valid_display_manifest();
  manifest.profile_count = 2;
  manifest.max_profile_count = 2;
  manifest.profiles[1] = manifest.profiles[0];
  manifest.profiles[1].connector_index = 1;
  manifest.profiles[1].display_id = 0x7000000000000001ull;
  manifest.profiles[1].container_id = {
    0x9161d0d5,
    0x7000,
    0x0000,
    {0x93, 0x7d, 0x00, 0x00, 0x00, 0x01, 0x53, 0x44}
  };
  manifest.profiles[1].product_code = 0x4001;
  manifest.profiles[1].serial_number = 2;

  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::None);

  auto duplicate = manifest;
  duplicate.profiles[1].display_id = duplicate.profiles[0].display_id;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);

  duplicate = manifest;
  duplicate.profiles[1].container_id = duplicate.profiles[0].container_id;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);

  duplicate = manifest;
  duplicate.profiles[1].product_code = duplicate.profiles[0].product_code;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);

  duplicate = manifest;
  duplicate.profiles[1].serial_number = duplicate.profiles[0].serial_number;
  EXPECT_EQ(vdd::validate_display_manifest(duplicate, 2), vdd::ValidationError::DuplicateManifestIdentity);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsMissingManifestIdentityFields) {
  auto manifest = valid_display_manifest();
  manifest.profiles[0].container_id = {};
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::DuplicateManifestIdentity);

  manifest = valid_display_manifest();
  manifest.profiles[0].product_code = 0;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::DuplicateManifestIdentity);

  manifest = valid_display_manifest();
  manifest.profiles[0].serial_number = 0;
  EXPECT_EQ(vdd::validate_display_manifest(manifest, 2), vdd::ValidationError::DuplicateManifestIdentity);
}
