#pragma once

#include "virtual_display/driver/edid.h"
#include "virtual_display/driver/lease_store.h"

#include <cstdint>

namespace virtual_display::driver {
  inline constexpr std::array<char, 3> kSunshineDriverManufacturerId {'S', 'D', 'D'};
  inline constexpr std::uint16_t kTemporaryDisplayProductCodeBase = 0x5000;

  std::uint32_t serial_number_from_display_id(std::uint64_t display_id);
  std::uint16_t product_code_from_display_id(std::uint64_t display_id);
  Guid container_guid_from_display_id(std::uint64_t display_id);
  EdidOptions edid_options_for_temporary_display(const TemporaryDisplayRecord &record);
}  // namespace virtual_display::driver
