#include <gtest/gtest.h>
#include "virtual_display/driver/control_protocol.h"
#include "virtual_display/driver/windows_control_protocol.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace vdd = virtual_display::driver;

namespace {

  vdd::CreateTemporaryDisplayRequest valid_create_request() {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = 10;
    request.display_id = 20;
    request.width = 2560;
    request.height = 1440;
    request.refresh_rate_millihz = 120'000;
    request.requested_timeout_ms = 30'000;
    std::memcpy(request.display_name, "Sunshine Display", 16);
    return request;
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
}

TEST(VirtualDisplayDriverControlProtocol, ProtocolVersionUsesDedicatedNamespace) {
  const vdd::ProtocolVersion version {};

  EXPECT_EQ(version.api_namespace, vdd::kApiNamespaceGuid);
  EXPECT_EQ(version.major, vdd::kProtocolVersionMajor);
  EXPECT_EQ(version.minor, vdd::kProtocolVersionMinor);
  EXPECT_EQ(version.patch, vdd::kProtocolVersionPatch);
}

TEST(VirtualDisplayDriverControlProtocol, WindowsGuidAdapterPreservesProtocolGuid) {
#ifdef _WIN32
  const auto win_guid = vdd::to_windows_guid(vdd::kApiNamespaceGuid);

  EXPECT_EQ(vdd::from_windows_guid(win_guid), vdd::kApiNamespaceGuid);
#endif
}

TEST(VirtualDisplayDriverControlProtocol, InfRegistersControlInterfaceWithAuthenticatedUserAccess) {
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
    inf.find("HKR,,Security,,\"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)\""),
    std::string::npos
  );
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
  EXPECT_EQ(validated.display_name, "Sunshine Display");
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
  request.width = vdd::kMinWidth - 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidWidth);

  request = valid_create_request();
  request.height = vdd::kMaxHeight + 1;
  EXPECT_EQ(vdd::validate_create_temporary_display(request), vdd::ValidationError::InvalidHeight);

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
    1,
    0,
    0
  };
  const vdd::LeaseDisplayRequest display_request {
    vdd::kApiNamespaceGuid,
    1,
    2
  };

  EXPECT_EQ(vdd::validate_lease_request(request), vdd::ValidationError::None);
  EXPECT_EQ(vdd::validate_lease_display_request(display_request), vdd::ValidationError::None);
}

TEST(VirtualDisplayDriverControlProtocol, RejectsInvalidLeaseRequests) {
  auto request = vdd::LeaseRequest {
    vdd::kApiNamespaceGuid,
    1,
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
    1,
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
    1,
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
