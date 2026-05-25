#include <gtest/gtest.h>
#include "virtual_display/driver/driver_controller.h"

#include <cstring>
#include <string>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {

  class FakeBackend: public vdd::DisplayDriverBackend {
  public:
    vdd::BackendError reserve_temporary_display_identity(const vdd::DisplayDescriptor &descriptor) override {
      reserved.push_back(descriptor);
      events.push_back("reserve");
      return fail_reserve ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &descriptor) override {
      events.push_back("arrive");
      arrived.push_back(descriptor);
      if (fail_arrive) {
        return {vdd::BackendError::Failed, 0, 0};
      }

      return {vdd::BackendError::None, adapter_luid, next_target_id++};
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id) override {
      departed.push_back(display_id);
      return fail_depart ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    vdd::BackendError set_permanent_display_count(const vdd::PermanentDisplayCountRequest &request) override {
      permanent_counts.push_back(request.display_count);
      permanent_settings.push_back(request);
      return fail_permanent ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    bool fail_arrive {};
    bool fail_depart {};
    bool fail_permanent {};
    bool fail_reserve {};
    vdd::AdapterLuid adapter_luid {44, 2};
    std::uint32_t next_target_id {7};
    std::vector<vdd::DisplayDescriptor> reserved {};
    std::vector<vdd::DisplayDescriptor> arrived {};
    std::vector<std::uint64_t> departed {};
    std::vector<std::uint32_t> permanent_counts {};
    std::vector<vdd::PermanentDisplayCountRequest> permanent_settings {};
    std::vector<std::string> events {};
  };

  vdd::CreateTemporaryDisplayRequest make_create_request(
    const std::uint64_t lease_id = 100,
    const std::uint64_t display_id = 0x12345678
  ) {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = lease_id;
    request.display_id = display_id;
    request.width = 2560;
    request.height = 1440;
    request.physical_width_mm = 590;
    request.physical_height_mm = 330;
    request.refresh_rate_millihz = 120'000;
    request.requested_timeout_ms = 30'000;
    std::memcpy(request.display_name, "Sunshine HDR", 13);
    return request;
  }

  vdd::DriverController make_controller(FakeBackend &backend) {
    return vdd::DriverController {vdd::DisplayStore {4, 8}, backend};
  }
}  // namespace

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayArrivesBackendAndReturnsOsIdentity) {
  FakeBackend backend;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_TRUE(created.status.ok());
  EXPECT_EQ(created.result.os_adapter_luid, (vdd::AdapterLuid {44, 2}));
  EXPECT_EQ(created.result.target_id, 7u);
  EXPECT_EQ(created.result.connector_index, 0u);
  ASSERT_EQ(backend.arrived.size(), 1u);
  EXPECT_EQ(backend.arrived[0].display_id, 0x12345678u);
  EXPECT_EQ(backend.arrived[0].container_id, vdd::container_guid_from_display_id(0x12345678u));
  EXPECT_EQ(backend.arrived[0].connector_index, 0u);
  EXPECT_EQ(backend.arrived[0].width, 2560u);
  EXPECT_EQ(backend.arrived[0].physical_width_mm, 590u);
  EXPECT_EQ(backend.arrived[0].physical_height_mm, 330u);
  EXPECT_TRUE(vdd::has_valid_edid_checksums(backend.arrived[0].edid));
  EXPECT_TRUE(vdd::has_hdr_static_metadata(backend.arrived[0].edid));
}

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayReservesIdentityBeforeArrival) {
  FakeBackend backend;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_TRUE(created.status.ok());
  ASSERT_EQ(backend.reserved.size(), 1u);
  ASSERT_EQ(backend.arrived.size(), 1u);
  EXPECT_EQ(backend.reserved[0].display_id, 0x12345678u);
  EXPECT_EQ(backend.reserved[0].connector_index, 0u);
  EXPECT_EQ(backend.reserved[0].container_id, vdd::container_guid_from_display_id(0x12345678u));
  EXPECT_EQ(backend.events, (std::vector<std::string> {"reserve", "arrive"}));
}

TEST(VirtualDisplayDriverController, CreateEphemeralTemporaryDisplaySkipsIdentityReserve) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  auto request = make_create_request();
  request.flags = vdd::kCreateTemporaryDisplayFlagEphemeralIdentity;

  const auto created = controller.create_temporary_display(request, std::chrono::steady_clock::now());

  EXPECT_TRUE(created.status.ok());
  EXPECT_TRUE(backend.reserved.empty());
  ASSERT_EQ(backend.arrived.size(), 1u);
  EXPECT_FALSE(backend.arrived[0].retain_identity);
  EXPECT_EQ(backend.events, (std::vector<std::string> {"arrive"}));
}

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayRollsBackStoreWhenIdentityReserveFails) {
  FakeBackend backend;
  backend.fail_reserve = true;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_FALSE(created.status.ok());
  EXPECT_EQ(created.status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_FALSE(controller.store().find_temporary_display(0x12345678));
  EXPECT_EQ(backend.reserved.size(), 1u);
  EXPECT_TRUE(backend.arrived.empty());
  EXPECT_EQ(backend.events, (std::vector<std::string> {"reserve"}));
}

TEST(VirtualDisplayDriverController, CreateTemporaryDisplayRollsBackStoreWhenBackendFails) {
  FakeBackend backend;
  backend.fail_arrive = true;
  auto controller = make_controller(backend);

  const auto created = controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now());

  EXPECT_FALSE(created.status.ok());
  EXPECT_EQ(created.status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_FALSE(controller.store().find_temporary_display(0x12345678));
}

TEST(VirtualDisplayDriverController, RemoveTemporaryDisplayDepartsBackend) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now()).status.ok());

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = 100;
  remove.display_id = 0x12345678;

  const auto status = controller.remove_temporary_display(remove);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  ASSERT_EQ(backend.departed.size(), 1u);
  EXPECT_EQ(backend.departed[0], 0x12345678u);
}

TEST(VirtualDisplayDriverController, RemoveTemporaryDisplayKeepsStoreWhenBackendDepartFails) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), std::chrono::steady_clock::now()).status.ok());
  backend.fail_depart = true;

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = 100;
  remove.display_id = 0x12345678;

  const auto status = controller.remove_temporary_display(remove);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 1u);
  EXPECT_TRUE(controller.store().find_temporary_display(0x12345678));
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {0x12345678}));
}

TEST(VirtualDisplayDriverController, ReleaseLeaseDepartsEveryDisplayInLease) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(100, 200), std::chrono::steady_clock::now()).status.ok());
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(100, 201), std::chrono::steady_clock::now()).status.ok());

  vdd::LeaseRequest release {};
  release.lease_id = 100;

  const auto status = controller.release_lease(release);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {200, 201}));
}

TEST(VirtualDisplayDriverController, ReleaseLeaseKeepsStoreWhenBackendDepartFails) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(100, 200), now).status.ok());
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(100, 201), now).status.ok());
  backend.fail_depart = true;

  vdd::LeaseRequest release {};
  release.lease_id = 100;

  const auto status = controller.release_lease(release);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.store().temporary_display_count(), 2u);
  EXPECT_EQ(controller.query_lease(100, now).lease_exists, 1u);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {200, 201}));
}

TEST(VirtualDisplayDriverController, FeedLeaseExtendsStoreWithoutBackendArrival) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());

  vdd::LeaseRequest feed {};
  feed.lease_id = 100;
  feed.requested_timeout_ms = 60'000;

  const auto status = controller.feed_lease(feed, now + std::chrono::seconds(10));

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(backend.arrived.size(), 1u);
  const auto query = controller.query_lease(100, now + std::chrono::seconds(10));
  EXPECT_EQ(query.effective_timeout_ms, 60'000u);
  EXPECT_EQ(query.remaining_ms, 60'000u);
}

TEST(VirtualDisplayDriverController, ReapExpiredDisplaysDepartsBackend) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());

  EXPECT_EQ(controller.reap_expired(now + std::chrono::milliseconds(30'000)), 1u);
  EXPECT_EQ(controller.store().temporary_display_count(), 0u);
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {0x12345678}));
}

TEST(VirtualDisplayDriverController, ReapExpiredDisplaysKeepsStoreWhenBackendDepartFails) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  const auto now = std::chrono::steady_clock::now();
  ASSERT_TRUE(controller.create_temporary_display(make_create_request(), now).status.ok());
  backend.fail_depart = true;

  EXPECT_EQ(controller.reap_expired(now + std::chrono::milliseconds(30'000)), 0u);
  EXPECT_EQ(controller.store().temporary_display_count(), 1u);
  EXPECT_TRUE(controller.store().find_temporary_display(0x12345678));
  EXPECT_EQ(backend.departed, (std::vector<std::uint64_t> {0x12345678}));
}

TEST(VirtualDisplayDriverController, SetPermanentDisplayCountReconcilesBackendBeforeStore) {
  FakeBackend backend;
  auto controller = make_controller(backend);
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;
  request.width = 3840;
  request.height = 2160;
  request.physical_width_mm = 700;
  request.physical_height_mm = 390;
  request.refresh_rate_millihz = 144'000;
  std::memcpy(request.display_name, "Desk Display", 13);

  const auto status = controller.set_permanent_display_count(request);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(backend.permanent_counts, (std::vector<std::uint32_t> {2}));
  ASSERT_EQ(backend.permanent_settings.size(), 1u);
  EXPECT_EQ(backend.permanent_settings[0].width, 3840u);
  EXPECT_EQ(backend.permanent_settings[0].height, 2160u);
  EXPECT_EQ(backend.permanent_settings[0].physical_width_mm, 700u);
  EXPECT_EQ(backend.permanent_settings[0].physical_height_mm, 390u);
  EXPECT_EQ(controller.query_permanent_display_count().current_display_count, 2u);
  EXPECT_EQ(controller.query_permanent_display_count().refresh_rate_millihz, 144'000u);
}

TEST(VirtualDisplayDriverController, SetPermanentDisplayCountKeepsStoreUnchangedWhenBackendFails) {
  FakeBackend backend;
  backend.fail_permanent = true;
  auto controller = make_controller(backend);
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;

  const auto status = controller.set_permanent_display_count(request);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.backend_error, vdd::BackendError::Failed);
  EXPECT_EQ(controller.query_permanent_display_count().current_display_count, 0u);
}
