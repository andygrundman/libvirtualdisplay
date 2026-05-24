#include <gtest/gtest.h>
#include "virtual_display/driver/control_client.h"

#include <cstring>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  struct IoctlCall {
    std::uint32_t ioctl_code {};
    std::vector<std::byte> input {};
  };

  class FakeTransport: public vdd::ControlTransport {
  public:
    bool ioctl(
      const std::uint32_t ioctl_code,
      const void *input,
      const std::size_t input_size,
      void *output,
      const std::size_t output_size,
      std::size_t &bytes_returned,
      std::uint32_t &native_error
    ) override {
      calls.push_back({ioctl_code, {}});
      if (input && input_size > 0) {
        const auto *begin = static_cast<const std::byte *>(input);
        calls.back().input.assign(begin, begin + input_size);
      }

      native_error = next_native_error;
      if (!next_success) {
        bytes_returned = 0;
        return false;
      }

      bytes_returned = 0;
      if (!next_output.empty()) {
        const auto copy_size = std::min(output_size, next_output.size());
        if (copy_size > 0 && output) {
          std::memcpy(output, next_output.data(), copy_size);
        }
        bytes_returned = forced_bytes_returned ? forced_bytes_returned : copy_size;
      }
      return true;
    }

    template<class T>
    void set_output(const T &value) {
      next_output.resize(sizeof(T));
      std::memcpy(next_output.data(), &value, sizeof(T));
      forced_bytes_returned = 0;
    }

    std::vector<IoctlCall> calls {};
    std::vector<std::byte> next_output {};
    bool next_success {true};
    std::size_t forced_bytes_returned {};
    std::uint32_t next_native_error {};
  };

  template<class T>
  T input_as(const IoctlCall &call) {
    T value {};
    EXPECT_EQ(call.input.size(), sizeof(T));
    if (call.input.size() == sizeof(T)) {
      std::memcpy(&value, call.input.data(), sizeof(T));
    }
    return value;
  }
}  // namespace

TEST(VirtualDisplayDriverControlClient, QueryProtocolVersionUsesProtocolIoctl) {
  FakeTransport transport;
  transport.set_output(vdd::ProtocolVersion {});
  vdd::ControlClient client {transport};

  const auto result = client.query_protocol_version();

  ASSERT_TRUE(result.ok());
  ASSERT_EQ(transport.calls.size(), 1u);
  EXPECT_EQ(transport.calls[0].ioctl_code, vdd::kIoctlGetProtocolVersion);
  EXPECT_EQ(result.value.major, vdd::kProtocolVersionMajor);
}

TEST(VirtualDisplayDriverControlClient, RejectsIncompatibleProtocolNamespace) {
  FakeTransport transport;
  vdd::ProtocolVersion version {};
  version.api_namespace.data1 ^= 0x1000u;
  transport.set_output(version);
  vdd::ControlClient client {transport};

  const auto result = client.check_protocol_compatible();

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, vdd::ControlStatus::ProtocolIncompatible);
}

TEST(VirtualDisplayDriverControlClient, RejectsResultNamespaceMismatch) {
  FakeTransport transport;
  vdd::CreateTemporaryDisplayResult expected {};
  expected.api_namespace.data1 ^= 0x1000u;
  expected.lease_id = 10;
  expected.display_id = 20;
  transport.set_output(expected);
  vdd::ControlClient client {transport};

  vdd::CreateTemporaryDisplayRequest request {};
  request.lease_id = 10;
  request.display_id = 20;
  request.width = 1920;
  request.height = 1080;
  request.refresh_rate_millihz = 60'000;

  const auto result = client.create_temporary_display(request);

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, vdd::ControlStatus::ProtocolIncompatible);
}

TEST(VirtualDisplayDriverControlClient, CreateTemporaryDisplayRoundTripsRequestAndResult) {
  FakeTransport transport;
  vdd::CreateTemporaryDisplayResult expected {};
  expected.lease_id = 10;
  expected.display_id = 20;
  expected.target_id = 3;
  expected.connector_index = 4;
  expected.effective_timeout_ms = 10'000;
  transport.set_output(expected);
  vdd::ControlClient client {transport};

  vdd::CreateTemporaryDisplayRequest request {};
  request.lease_id = 10;
  request.display_id = 20;
  request.width = 2560;
  request.height = 1440;
  request.refresh_rate_millihz = 120'000;

  const auto result = client.create_temporary_display(request);

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value.display_id, expected.display_id);
  ASSERT_EQ(transport.calls.size(), 1u);
  EXPECT_EQ(transport.calls[0].ioctl_code, vdd::kIoctlCreateTemporaryDisplay);
  const auto sent = input_as<vdd::CreateTemporaryDisplayRequest>(transport.calls[0]);
  EXPECT_EQ(sent.width, 2560u);
  EXPECT_EQ(sent.height, 1440u);
}

TEST(VirtualDisplayDriverControlClient, LeaseOperationsUseExpectedIoctls) {
  FakeTransport transport;
  vdd::ControlClient client {transport};
  const vdd::LeaseDisplayRequest display_request {vdd::kApiNamespaceGuid, 10, 20};
  const vdd::LeaseRequest lease_request {vdd::kApiNamespaceGuid, 10, 10'000, 0};

  EXPECT_TRUE(client.remove_temporary_display(display_request).ok());
  EXPECT_TRUE(client.feed_lease(lease_request).ok());
  EXPECT_TRUE(client.release_lease(lease_request).ok());

  ASSERT_EQ(transport.calls.size(), 3u);
  EXPECT_EQ(transport.calls[0].ioctl_code, vdd::kIoctlRemoveTemporaryDisplay);
  EXPECT_EQ(transport.calls[1].ioctl_code, vdd::kIoctlFeedLease);
  EXPECT_EQ(transport.calls[2].ioctl_code, vdd::kIoctlReleaseLease);
  EXPECT_EQ(input_as<vdd::LeaseDisplayRequest>(transport.calls[0]).display_id, 20u);
  EXPECT_EQ(input_as<vdd::LeaseRequest>(transport.calls[1]).lease_id, 10u);
}

TEST(VirtualDisplayDriverControlClient, QueryLeaseReturnsLeaseState) {
  FakeTransport transport;
  vdd::QueryLeaseResult expected {};
  expected.lease_id = 10;
  expected.lease_exists = 1;
  expected.temporary_display_count = 2;
  transport.set_output(expected);
  vdd::ControlClient client {transport};

  const auto result = client.query_lease({vdd::kApiNamespaceGuid, 10, 10'000, 0});

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value.temporary_display_count, 2u);
  ASSERT_EQ(transport.calls.size(), 1u);
  EXPECT_EQ(transport.calls[0].ioctl_code, vdd::kIoctlQueryLease);
}

TEST(VirtualDisplayDriverControlClient, PermanentDisplayOperationsReturnCounts) {
  FakeTransport transport;
  vdd::PermanentDisplayCountResult expected {};
  expected.current_display_count = 2;
  expected.max_display_count = 4;
  transport.set_output(expected);
  vdd::ControlClient client {transport};

  const auto set_result = client.set_permanent_display_count({vdd::kApiNamespaceGuid, 2, 0, {}});
  const auto query_result = client.query_permanent_display_count();

  ASSERT_TRUE(set_result.ok());
  ASSERT_TRUE(query_result.ok());
  ASSERT_EQ(transport.calls.size(), 2u);
  EXPECT_EQ(transport.calls[0].ioctl_code, vdd::kIoctlSetPermanentDisplayCount);
  EXPECT_EQ(transport.calls[1].ioctl_code, vdd::kIoctlQueryPermanentDisplayCount);
}

TEST(VirtualDisplayDriverControlClient, DetectsShortOutput) {
  FakeTransport transport;
  transport.set_output(vdd::ProtocolVersion {});
  transport.forced_bytes_returned = sizeof(vdd::ProtocolVersion) - 1;
  vdd::ControlClient client {transport};

  const auto result = client.query_protocol_version();

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, vdd::ControlStatus::InvalidOutput);
}

TEST(VirtualDisplayDriverControlClient, ReportsTransportFailureNativeError) {
  FakeTransport transport;
  transport.next_success = false;
  transport.next_native_error = 1234;
  vdd::ControlClient client {transport};

  const auto result = client.query_permanent_display_count();

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status, vdd::ControlStatus::TransportFailed);
  EXPECT_EQ(result.native_error, 1234u);
}
