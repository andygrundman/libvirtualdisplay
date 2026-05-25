#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace virtual_display::driver {
  inline constexpr std::size_t kEdidBlockSize = 128;
  inline constexpr std::size_t kEdidSize = kEdidBlockSize * 2;

  struct EdidOptions {
    std::array<char, 3> manufacturer_id {'S', 'D', 'D'};
    std::uint16_t product_code {0x1000};
    std::uint32_t serial_number {1};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t physical_width_mm {600};
    std::uint32_t physical_height_mm {340};
    std::uint32_t refresh_rate_millihz {60'000};
    std::string_view monitor_name {"Sunshine Display"};
    bool hdr_supported {true};
  };

  struct PreferredTiming {
    std::uint32_t pixel_clock_10khz {};
    std::uint16_t horizontal_active {};
    std::uint16_t horizontal_blanking {};
    std::uint16_t vertical_active {};
    std::uint16_t vertical_blanking {};
  };

  std::array<std::byte, kEdidSize> create_edid(const EdidOptions &options);
  bool has_valid_edid_checksums(std::span<const std::byte, kEdidSize> edid);
  bool has_hdr_static_metadata(std::span<const std::byte, kEdidSize> edid);
  bool has_bt2020_colorimetry(std::span<const std::byte, kEdidSize> edid);
  std::array<char, 3> read_manufacturer_id(std::span<const std::byte, kEdidSize> edid);
  std::uint16_t read_product_code(std::span<const std::byte, kEdidSize> edid);
  std::uint32_t read_serial_number(std::span<const std::byte, kEdidSize> edid);
  PreferredTiming read_preferred_timing(std::span<const std::byte, kEdidSize> edid);
}  // namespace virtual_display::driver
