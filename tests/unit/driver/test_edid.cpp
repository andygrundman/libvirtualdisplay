#include <gtest/gtest.h>
#include "virtual_display/driver/edid.h"

namespace vdd = virtual_display::driver;

namespace {

  vdd::EdidOptions default_options() {
    vdd::EdidOptions options {};
    options.manufacturer_id = {'S', 'D', 'D'};
    options.product_code = 0x4101;
    options.serial_number = 0x12345678;
    options.width = 2560;
    options.height = 1440;
    options.physical_width_mm = 590;
    options.physical_height_mm = 330;
    options.refresh_rate_millihz = 120'000;
    options.monitor_name = "Sunshine HDR";
    options.hdr_supported = true;
    return options;
  }

  std::uint16_t read_detailed_physical_width_mm(const std::array<std::byte, vdd::kEdidSize> &edid) {
    return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(edid[66]) |
      ((static_cast<std::uint16_t>(edid[68]) >> 4u) << 8u)
    );
  }

  std::uint16_t read_detailed_physical_height_mm(const std::array<std::byte, vdd::kEdidSize> &edid) {
    return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(edid[67]) |
      ((static_cast<std::uint16_t>(edid[68]) & 0x0fu) << 8u)
    );
  }
}  // namespace

TEST(VirtualDisplayDriverEdid, BuildsParseableTwoBlockEdidWithStableIdentity) {
  const auto edid = vdd::create_edid(default_options());

  EXPECT_EQ(edid[0], std::byte {0x00});
  EXPECT_EQ(edid[1], std::byte {0xff});
  EXPECT_EQ(edid[126], std::byte {0x01});
  EXPECT_EQ(edid[128], std::byte {0x02});
  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));

  EXPECT_EQ(vdd::read_manufacturer_id(edid), (std::array<char, 3> {'S', 'D', 'D'}));
  EXPECT_EQ(vdd::read_product_code(edid), 0x4101u);
  EXPECT_EQ(vdd::read_serial_number(edid), 0x12345678u);
}

TEST(VirtualDisplayDriverEdid, RejectsInvalidManufacturerCodes) {
  auto edid = vdd::create_edid(default_options());

  edid[8] = std::byte {0x00};
  edid[9] = std::byte {0x00};
  EXPECT_EQ(vdd::read_manufacturer_id(edid), std::nullopt);

  edid = vdd::create_edid(default_options());
  edid[8] = std::byte {0x7f};
  edid[9] = std::byte {0xff};
  EXPECT_EQ(vdd::read_manufacturer_id(edid), std::nullopt);
}

TEST(VirtualDisplayDriverEdid, StopsMonitorNameAtDescriptorTerminator) {
  auto edid = vdd::create_edid(default_options());
  edid[90 + 5 + 4] = std::byte {'\n'};
  edid[90 + 5 + 5] = std::byte {'X'};

  EXPECT_EQ(vdd::read_monitor_name(edid), "Suns");
}

TEST(VirtualDisplayDriverEdid, RejectsUnsafeMonitorNameCharacters) {
  auto edid = vdd::create_edid(default_options());
  edid[90 + 5 + 4] = std::byte {'\t'};

  EXPECT_EQ(vdd::read_monitor_name(edid), std::nullopt);
}

TEST(VirtualDisplayDriverEdid, EncodesPhysicalSizeForDpiScaling) {
  auto options = default_options();
  options.physical_width_mm = 700;
  options.physical_height_mm = 390;

  const auto edid = vdd::create_edid(options);

  EXPECT_EQ(static_cast<std::uint8_t>(edid[21]), 70u);
  EXPECT_EQ(static_cast<std::uint8_t>(edid[22]), 39u);
  EXPECT_EQ(read_detailed_physical_width_mm(edid), 700u);
  EXPECT_EQ(read_detailed_physical_height_mm(edid), 390u);
}

TEST(VirtualDisplayDriverEdid, HdrEdidUsesRequestedPreferredTiming) {
  const auto edid = vdd::create_edid(default_options());
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(timing.horizontal_active, 2560u);
  EXPECT_EQ(timing.vertical_active, 1440u);
  EXPECT_GT(timing.horizontal_blanking, 0u);
  EXPECT_GT(timing.vertical_blanking, 0u);
  EXPECT_GT(timing.pixel_clock_10khz, 0u);
}

TEST(VirtualDisplayDriverEdid, HdrEdidDoesNotEncodeRequestedHighRefreshInBaseTiming) {
  auto options = default_options();
  options.width = 3840;
  options.height = 2160;
  options.refresh_rate_millihz = 240'000;

  const auto edid = vdd::create_edid(options);
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(timing.horizontal_active, 3840u);
  EXPECT_EQ(timing.vertical_active, 2160u);
  EXPECT_NE(timing.pixel_clock_10khz, 0xffffu);

  const auto horizontal_total =
    static_cast<std::uint64_t>(timing.horizontal_active) + static_cast<std::uint64_t>(timing.horizontal_blanking);
  const auto vertical_total =
    static_cast<std::uint64_t>(timing.vertical_active) + static_cast<std::uint64_t>(timing.vertical_blanking);
  ASSERT_GT(horizontal_total, 0u);
  ASSERT_GT(vertical_total, 0u);

  const auto total_pixels = horizontal_total * vertical_total;
  const auto effective_millihz = static_cast<std::uint64_t>(timing.pixel_clock_10khz) * 10'000'000ull / total_pixels;

  EXPECT_NEAR(static_cast<double>(effective_millihz), 60'000.0, 10.0);
}

TEST(VirtualDisplayDriverEdid, HdrEdidUsesRequestedRangeDescriptor) {
  auto options = default_options();
  options.width = 3840;
  options.height = 2160;
  options.refresh_rate_millihz = 240'000;

  const auto edid = vdd::create_edid(options);

  EXPECT_EQ(edid[108 + 3], std::byte {0xfd});
  EXPECT_EQ(edid[108 + 6], std::byte {240});
  EXPECT_EQ(edid[108 + 9], std::byte {251});
}

TEST(VirtualDisplayDriverEdid, ClampsTimingInputsBeforeEncoding) {
  auto options = default_options();
  options.width = 100'000;
  options.height = 100'000;
  options.physical_width_mm = 100'000;
  options.physical_height_mm = 100'000;
  options.refresh_rate_millihz = 10'000'000;

  const auto edid = vdd::create_edid(options);
  const auto timing = vdd::read_preferred_timing(edid);

  EXPECT_EQ(timing.horizontal_active, 4095u);
  EXPECT_EQ(timing.vertical_active, 4095u);
  EXPECT_EQ(read_detailed_physical_width_mm(edid), 4095u);
  EXPECT_EQ(read_detailed_physical_height_mm(edid), 4095u);
  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
}

TEST(VirtualDisplayDriverEdid, HandlesEmptyMonitorName) {
  auto options = default_options();
  options.monitor_name = std::string_view {};

  const auto edid = vdd::create_edid(options);

  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
}

TEST(VirtualDisplayDriverEdid, EmbedsHdrStaticMetadataWhenRequested) {
  const auto edid = vdd::create_edid(default_options());

  EXPECT_TRUE(vdd::has_hdr_static_metadata(edid));
}

TEST(VirtualDisplayDriverEdid, EmbedsBt2020ColorimetryWhenHdrRequested) {
  const auto edid = vdd::create_edid(default_options());

  EXPECT_TRUE(vdd::has_bt2020_colorimetry(edid));
}

TEST(VirtualDisplayDriverEdid, EmitsWindowsHdrClassifiedCtaMetadata) {
  const auto edid = vdd::create_edid(default_options());
  const auto cta = vdd::kEdidBlockSize;

  EXPECT_EQ(edid[cta + 2], std::byte {0x44});
  EXPECT_EQ(edid[cta + 3], std::byte {0xf0});
  EXPECT_EQ(edid[cta + 52], std::byte {0xe3});
  EXPECT_EQ(edid[cta + 53], std::byte {0x05});
  EXPECT_EQ(edid[cta + 54], std::byte {0xe0});
  EXPECT_EQ(edid[cta + 55], std::byte {0x01});
  EXPECT_EQ(edid[cta + 61], std::byte {0xe6});
  EXPECT_EQ(edid[cta + 62], std::byte {0x06});
  EXPECT_EQ(edid[cta + 63], std::byte {0x0f});
  EXPECT_EQ(edid[cta + 64], std::byte {0x01});
  EXPECT_EQ(edid[cta + 65], std::byte {0xc8});
  EXPECT_EQ(edid[cta + 66], std::byte {0xc8});
  EXPECT_EQ(edid[cta + 67], std::byte {0x00});
}

TEST(VirtualDisplayDriverEdid, OmitsHdrStaticMetadataWhenDisabled) {
  auto options = default_options();
  options.hdr_supported = false;

  const auto edid = vdd::create_edid(options);

  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
  EXPECT_FALSE(vdd::has_hdr_static_metadata(edid));
}

TEST(VirtualDisplayDriverEdid, OmitsBt2020ColorimetryWhenHdrDisabled) {
  auto options = default_options();
  options.hdr_supported = false;

  const auto edid = vdd::create_edid(options);

  EXPECT_TRUE(vdd::has_valid_edid_checksums(edid));
  EXPECT_FALSE(vdd::has_bt2020_colorimetry(edid));
}
