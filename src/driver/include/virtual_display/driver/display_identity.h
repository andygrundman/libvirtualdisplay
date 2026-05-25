#pragma once

#include "virtual_display/driver/edid.h"
#include "virtual_display/driver/lease_store.h"

#include <cstdint>

namespace virtual_display::driver {
  inline constexpr std::array<char, 3> kSunshineDriverManufacturerId {'S', 'D', 'D'};
  inline constexpr std::uint64_t kPermanentDisplayIdBase = 0x7000000000000000ull;
  inline constexpr std::uint16_t kPermanentDisplayProductCodeBase = 0x4000;
  inline constexpr std::uint16_t kTemporaryDisplayProductCodeBase = 0x5000;

  std::uint64_t permanent_display_id(std::uint32_t index);
  std::uint16_t permanent_product_code(std::uint32_t index);
  std::uint32_t serial_number_from_display_id(std::uint64_t display_id);
  std::uint16_t product_code_from_display_id(std::uint64_t display_id);
  Guid container_guid_from_display_id(std::uint64_t display_id);
  EdidOptions edid_options_for_temporary_display(const TemporaryDisplayRecord &record);
}  // namespace virtual_display::driver
