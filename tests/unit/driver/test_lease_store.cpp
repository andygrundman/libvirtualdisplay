#include <gtest/gtest.h>
#include "virtual_display/driver/lease_store.h"

#include <cstring>

namespace vdd = virtual_display::driver;

namespace {

  vdd::CreateTemporaryDisplayRequest make_create_request(
    const std::uint64_t lease_id = 100,
    const std::uint64_t display_id = 200
  ) {
    vdd::CreateTemporaryDisplayRequest request {};
    request.lease_id = lease_id;
    request.display_id = display_id;
    request.width = 1920;
    request.height = 1080;
    request.refresh_rate_millihz = 60'000;
    request.requested_timeout_ms = 30'000;
    std::memcpy(request.display_name, "Sunshine Display", 16);
    return request;
  }
}  // namespace

TEST(VirtualDisplayDriverLeaseStore, CreatesTemporaryDisplayWithConnectorIndex) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  const auto created = store.create_temporary_display(make_create_request(), now);

  EXPECT_EQ(created.status.error, vdd::StoreError::None);
  EXPECT_EQ(created.result.lease_id, 100u);
  EXPECT_EQ(created.result.display_id, 200u);
  EXPECT_EQ(created.result.connector_index, 2u);
  EXPECT_EQ(created.result.effective_timeout_ms, 30'000u);
  EXPECT_EQ(store.temporary_display_count(), 1u);

  const auto record = store.find_temporary_display(200);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->width, 1920u);
  EXPECT_EQ(record->height, 1080u);
  EXPECT_EQ(record->refresh_rate_millihz, 60'000u);
  EXPECT_EQ(record->display_name, "Sunshine Display");
}

TEST(VirtualDisplayDriverLeaseStore, RejectsDuplicateDisplayId) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(), now).status.error, vdd::StoreError::None);
  EXPECT_EQ(
    store.create_temporary_display(make_create_request(101, 200), now).status.error,
    vdd::StoreError::DisplayAlreadyExists
  );
}

TEST(VirtualDisplayDriverLeaseStore, RejectsTemporaryDisplayLimit) {
  vdd::DisplayStore store {2, 1};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(1, 10), now).status.error, vdd::StoreError::None);
  EXPECT_EQ(
    store.create_temporary_display(make_create_request(2, 20), now).status.error,
    vdd::StoreError::TemporaryDisplayLimitReached
  );
}

TEST(VirtualDisplayDriverLeaseStore, QueriesAndFeedsLease) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(), now).status.error, vdd::StoreError::None);

  auto query = store.query_lease(100, now + std::chrono::seconds(10));
  EXPECT_EQ(query.lease_exists, 1u);
  EXPECT_EQ(query.temporary_display_count, 1u);
  EXPECT_EQ(query.effective_timeout_ms, 30'000u);
  EXPECT_EQ(query.remaining_ms, 20'000u);

  const vdd::LeaseRequest feed {
    vdd::kApiNamespaceGuid,
    100,
    60'000,
    0
  };
  EXPECT_EQ(store.feed_lease(feed, now + std::chrono::seconds(20)).error, vdd::StoreError::None);

  query = store.query_lease(100, now + std::chrono::seconds(20));
  EXPECT_EQ(query.effective_timeout_ms, 60'000u);
  EXPECT_EQ(query.remaining_ms, 60'000u);
}

TEST(VirtualDisplayDriverLeaseStore, RemovesSingleDisplayFromLease) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(100, 200), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(100, 201), now).status.error, vdd::StoreError::None);

  const vdd::LeaseDisplayRequest remove {
    vdd::kApiNamespaceGuid,
    100,
    200
  };

  EXPECT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::None);
  EXPECT_EQ(store.temporary_display_count(), 1u);
  EXPECT_FALSE(store.find_temporary_display(200));
  EXPECT_TRUE(store.find_temporary_display(201));
  EXPECT_EQ(store.query_lease(100, now).lease_exists, 1u);
}

TEST(VirtualDisplayDriverLeaseStore, RemoveRejectsWrongLeaseAndMissingDisplay) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(100, 200), now).status.error, vdd::StoreError::None);

  auto remove = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    101,
    200
  };
  EXPECT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::DisplayNotFound);
  EXPECT_EQ(store.temporary_display_count(), 1u);

  remove = vdd::LeaseDisplayRequest {
    vdd::kApiNamespaceGuid,
    100,
    201
  };
  EXPECT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::DisplayNotFound);
  EXPECT_EQ(store.temporary_display_count(), 1u);
  EXPECT_EQ(store.query_lease(100, now).lease_exists, 1u);
}

TEST(VirtualDisplayDriverLeaseStore, TemporaryConnectorIndexesStayAfterPermanentRange) {
  vdd::DisplayStore store {3, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(1, 10), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(2, 20), now).status.error, vdd::StoreError::None);

  EXPECT_EQ(store.find_temporary_display(10)->connector_index, 3u);
  EXPECT_EQ(store.find_temporary_display(20)->connector_index, 4u);

  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 0;
  EXPECT_EQ(store.set_permanent_display_count(request).error, vdd::StoreError::None);
  EXPECT_EQ(store.find_temporary_display(10)->connector_index, 3u);
  EXPECT_EQ(store.find_temporary_display(20)->connector_index, 4u);
}

TEST(VirtualDisplayDriverLeaseStore, ReusesLowestAvailableTemporaryConnectorIndex) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  ASSERT_EQ(store.create_temporary_display(make_create_request(1, 10), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(2, 20), now).status.error, vdd::StoreError::None);

  const vdd::LeaseDisplayRequest remove {
    vdd::kApiNamespaceGuid,
    1,
    10
  };
  ASSERT_EQ(store.remove_temporary_display(remove).error, vdd::StoreError::None);

  ASSERT_EQ(store.create_temporary_display(make_create_request(3, 30), now).status.error, vdd::StoreError::None);
  EXPECT_EQ(store.find_temporary_display(30)->connector_index, 2u);
}

TEST(VirtualDisplayDriverLeaseStore, ReleasesWholeLease) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(100, 200), now).status.error, vdd::StoreError::None);
  ASSERT_EQ(store.create_temporary_display(make_create_request(100, 201), now).status.error, vdd::StoreError::None);

  const vdd::LeaseRequest release {
    vdd::kApiNamespaceGuid,
    100,
    0,
    0
  };

  EXPECT_EQ(store.release_lease(release).error, vdd::StoreError::None);
  EXPECT_EQ(store.temporary_display_count(), 0u);
  EXPECT_EQ(store.query_lease(100, now).lease_exists, 0u);
}

TEST(VirtualDisplayDriverLeaseStore, FeedAndReleaseMissingLeaseReturnNotFound) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();

  const vdd::LeaseRequest request {
    vdd::kApiNamespaceGuid,
    404,
    0,
    0
  };

  EXPECT_EQ(store.feed_lease(request, now).error, vdd::StoreError::LeaseNotFound);
  EXPECT_EQ(store.release_lease(request).error, vdd::StoreError::LeaseNotFound);
}

TEST(VirtualDisplayDriverLeaseStore, ReapsExpiredDisplays) {
  vdd::DisplayStore store {2, 4};
  const auto now = std::chrono::steady_clock::now();
  ASSERT_EQ(store.create_temporary_display(make_create_request(100, 200), now).status.error, vdd::StoreError::None);

  EXPECT_EQ(store.reap_expired(now + std::chrono::milliseconds(29'999)), 0u);
  EXPECT_EQ(store.reap_expired(now + std::chrono::milliseconds(30'000)), 1u);
  EXPECT_EQ(store.temporary_display_count(), 0u);
  EXPECT_EQ(store.query_lease(100, now).lease_exists, 0u);
}

TEST(VirtualDisplayDriverLeaseStore, SetsAndQueriesPermanentDisplayCount) {
  vdd::DisplayStore store {2, 4};
  ASSERT_EQ(
    store.create_temporary_display(make_create_request(100, 200), std::chrono::steady_clock::now()).status.error,
    vdd::StoreError::None
  );

  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 2;

  EXPECT_EQ(store.set_permanent_display_count(request).error, vdd::StoreError::None);

  const auto result = store.query_permanent_display_count();
  EXPECT_EQ(result.current_display_count, 2u);
  EXPECT_EQ(result.max_display_count, 2u);
  EXPECT_EQ(result.temporary_display_count, 1u);
}

TEST(VirtualDisplayDriverLeaseStore, RejectsTooManyPermanentDisplays) {
  vdd::DisplayStore store {2, 4};
  vdd::PermanentDisplayCountRequest request {};
  request.display_count = 3;

  const auto result = store.set_permanent_display_count(request);
  EXPECT_EQ(result.error, vdd::StoreError::ValidationFailed);
  EXPECT_EQ(result.validation_error, vdd::ValidationError::PermanentDisplayCountTooHigh);
  EXPECT_EQ(store.permanent_display_count(), 0u);
}
