#include <gtest/gtest.h>
#include "virtual_display/driver/ioctl_dispatcher.h"

#include <cstring>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {

  class FakeBackend: public vdd::DisplayDriverBackend {
  public:
    vdd::BackendDisplayResult arrive_temporary_display(const vdd::DisplayDescriptor &descriptor) override {
      arrived.push_back(descriptor);
      if (fail_arrive) {
        return {vdd::BackendError::Failed, 0, 0};
      }

      return {vdd::BackendError::None, {99, 3}, next_target_id++};
    }

    vdd::BackendError depart_temporary_display(const std::uint64_t display_id) override {
      departed.push_back(display_id);
      return fail_depart ? vdd::BackendError::Failed : vdd::BackendError::None;
    }

    vdd::BackendError set_permanent_display_count(const std::uint32_t display_count) override {
      permanent_counts.push_back(display_count);
      return vdd::BackendError::None;
    }

    bool fail_arrive {};
    bool fail_depart {};
    std::uint32_t next_target_id {12};
    std::vector<vdd::DisplayDescriptor> arrived {};
    std::vector<std::uint64_t> departed {};
    std::vector<std::uint32_t> permanent_counts {};
  };

  struct Harness {
    FakeBackend backend {};
    vdd::DriverController controller {vdd::DisplayStore {4, 8}, backend};
    vdd::IoctlDispatcher dispatcher {controller};
  };

  vdd::CreateTemporaryDisplayRequest make_create_request() {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = 100;
    request.display_id = 200;
    request.width = 1920;
    request.height = 1080;
    request.refresh_rate_millihz = 60'000;
    request.requested_timeout_ms = 30'000;
    std::memcpy(request.display_name, "Sunshine", 9);
    return request;
  }
}  // namespace

TEST(VirtualDisplayDriverIoctlDispatcher, ReturnsProtocolVersion) {
  Harness harness;
  vdd::ProtocolVersion version {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlGetProtocolVersion,
    nullptr,
    0,
    &version,
    sizeof(version),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(result.bytes_returned, sizeof(version));
  EXPECT_EQ(version.api_namespace, vdd::kApiNamespaceGuid);
}

TEST(VirtualDisplayDriverIoctlDispatcher, RejectsShortCreateInput) {
  Harness harness;
  vdd::CreateTemporaryDisplayResult output {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    nullptr,
    0,
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidInputBuffer);
}

TEST(VirtualDisplayDriverIoctlDispatcher, CreateTemporaryDisplayWritesResultAndArrivesBackend) {
  Harness harness;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    &request,
    sizeof(request),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(result.bytes_returned, sizeof(output));
  EXPECT_EQ(output.lease_id, request.lease_id);
  EXPECT_EQ(output.display_id, request.display_id);
  EXPECT_EQ(output.os_adapter_luid, (vdd::AdapterLuid {99, 3}));
  EXPECT_EQ(output.target_id, 12u);
  ASSERT_EQ(harness.backend.arrived.size(), 1u);
  EXPECT_TRUE(vdd::has_hdr_static_metadata(harness.backend.arrived[0].edid));
}

TEST(VirtualDisplayDriverIoctlDispatcher, CreateTemporaryDisplayRejectsShortOutputBeforeBackendArrival) {
  Harness harness;
  auto request = make_create_request();

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    &request,
    sizeof(request),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidOutputBuffer);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 0u);
  EXPECT_TRUE(harness.backend.arrived.empty());
  EXPECT_TRUE(harness.backend.departed.empty());
}

TEST(VirtualDisplayDriverIoctlDispatcher, CreateTemporaryDisplayMapsBackendFailure) {
  Harness harness;
  harness.backend.fail_arrive = true;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlCreateTemporaryDisplay,
    &request,
    sizeof(request),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::BackendFailed);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, RemoveTemporaryDisplayDepartsBackend) {
  Harness harness;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &request, sizeof(request), &output, sizeof(output), std::chrono::steady_clock::now()).status,
    vdd::IoctlStatus::Success
  );

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = request.lease_id;
  remove.display_id = request.display_id;
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlRemoveTemporaryDisplay,
    &remove,
    sizeof(remove),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(harness.backend.departed, (std::vector<std::uint64_t> {request.display_id}));
}

TEST(VirtualDisplayDriverIoctlDispatcher, RemoveTemporaryDisplayBackendFailureKeepsStore) {
  Harness harness;
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &request, sizeof(request), &output, sizeof(output), std::chrono::steady_clock::now()).status,
    vdd::IoctlStatus::Success
  );
  harness.backend.fail_depart = true;

  vdd::LeaseDisplayRequest remove {};
  remove.lease_id = request.lease_id;
  remove.display_id = request.display_id;
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlRemoveTemporaryDisplay,
    &remove,
    sizeof(remove),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::BackendFailed);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 1u);
  EXPECT_TRUE(harness.controller.store().find_temporary_display(request.display_id));
  EXPECT_EQ(harness.backend.departed, (std::vector<std::uint64_t> {request.display_id}));
}

TEST(VirtualDisplayDriverIoctlDispatcher, QueryAndFeedLeaseUseLeaseApi) {
  Harness harness;
  const auto now = std::chrono::steady_clock::now();
  auto request = make_create_request();
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &request, sizeof(request), &output, sizeof(output), now).status,
    vdd::IoctlStatus::Success
  );

  vdd::LeaseRequest feed {};
  feed.lease_id = request.lease_id;
  feed.requested_timeout_ms = 60'000;
  EXPECT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlFeedLease, &feed, sizeof(feed), nullptr, 0, now + std::chrono::seconds(10)).status,
    vdd::IoctlStatus::Success
  );

  vdd::QueryLeaseResult query {};
  const auto query_result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryLease,
    &feed,
    sizeof(feed),
    &query,
    sizeof(query),
    now + std::chrono::seconds(10)
  );

  EXPECT_EQ(query_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(query.lease_exists, 1u);
  EXPECT_EQ(query.effective_timeout_ms, 60'000u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, QueryLeaseRejectsWrongNamespace) {
  Harness harness;
  vdd::LeaseRequest query_request {};
  query_request.lease_id = 100;
  query_request.api_namespace.data1 ^= 1;
  vdd::QueryLeaseResult query {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryLease,
    &query_request,
    sizeof(query_request),
    &query,
    sizeof(query),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidRequest);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(result.controller_status.validation_error, vdd::ValidationError::WrongApiNamespace);
}

TEST(VirtualDisplayDriverIoctlDispatcher, QueryLeaseRejectsMissingLeaseId) {
  Harness harness;
  vdd::LeaseRequest query_request {};
  vdd::QueryLeaseResult query {};

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryLease,
    &query_request,
    sizeof(query_request),
    &query,
    sizeof(query),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidRequest);
  EXPECT_EQ(result.bytes_returned, 0u);
  EXPECT_EQ(result.controller_status.validation_error, vdd::ValidationError::MissingLeaseId);
}

TEST(VirtualDisplayDriverIoctlDispatcher, ReleaseLeaseBackendFailureKeepsStore) {
  Harness harness;
  const auto now = std::chrono::steady_clock::now();
  auto first = make_create_request();
  auto second = make_create_request();
  second.display_id = 201;
  vdd::CreateTemporaryDisplayResult output {};
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &first, sizeof(first), &output, sizeof(output), now).status,
    vdd::IoctlStatus::Success
  );
  ASSERT_EQ(
    harness.dispatcher.dispatch(vdd::kIoctlCreateTemporaryDisplay, &second, sizeof(second), &output, sizeof(output), now).status,
    vdd::IoctlStatus::Success
  );
  harness.backend.fail_depart = true;

  vdd::LeaseRequest release {};
  release.lease_id = first.lease_id;
  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlReleaseLease,
    &release,
    sizeof(release),
    nullptr,
    0,
    now
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::BackendFailed);
  EXPECT_EQ(harness.controller.store().temporary_display_count(), 2u);
  EXPECT_EQ(harness.controller.query_lease(first.lease_id, now).lease_exists, 1u);
  EXPECT_EQ(harness.backend.departed, (std::vector<std::uint64_t> {first.display_id, second.display_id}));
}

TEST(VirtualDisplayDriverIoctlDispatcher, SetAndQueryPermanentDisplayCountUsePermanentApi) {
  Harness harness;
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;
  vdd::PermanentDisplayCountResult output {};

  const auto set_result = harness.dispatcher.dispatch(
    vdd::kIoctlSetPermanentDisplayCount,
    &request,
    sizeof(request),
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(set_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(output.current_display_count, 2u);
  EXPECT_EQ(harness.backend.permanent_counts, (std::vector<std::uint32_t> {2}));

  output = {};
  const auto query_result = harness.dispatcher.dispatch(
    vdd::kIoctlQueryPermanentDisplayCount,
    nullptr,
    0,
    &output,
    sizeof(output),
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(query_result.status, vdd::IoctlStatus::Success);
  EXPECT_EQ(output.current_display_count, 2u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, SetPermanentDisplayCountRejectsShortOutputBeforeBackendMutation) {
  Harness harness;
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;

  const auto result = harness.dispatcher.dispatch(
    vdd::kIoctlSetPermanentDisplayCount,
    &request,
    sizeof(request),
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidOutputBuffer);
  EXPECT_TRUE(harness.backend.permanent_counts.empty());
  EXPECT_EQ(harness.controller.query_permanent_display_count().current_display_count, 0u);
}

TEST(VirtualDisplayDriverIoctlDispatcher, RejectsUnknownIoctl) {
  Harness harness;

  const auto result = harness.dispatcher.dispatch(
    0xdeadbeefu,
    nullptr,
    0,
    nullptr,
    0,
    std::chrono::steady_clock::now()
  );

  EXPECT_EQ(result.status, vdd::IoctlStatus::InvalidIoctl);
}
