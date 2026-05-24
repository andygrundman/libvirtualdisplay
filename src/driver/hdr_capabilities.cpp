#include "virtual_display/driver/hdr_capabilities.h"

namespace virtual_display::driver {
  HdrOutputCapabilities hdr_output_capabilities() {
    return HdrOutputCapabilities {
      true,
      true,
      true,
      WireColorSupport {
        true,
        true,
        false,
        false,
        false
      },
      WireColorSupport {}
    };
  }

  bool supports_windows_hdr_toggle(const HdrOutputCapabilities &capabilities) {
    return capabilities.fp16_swapchain &&
           capabilities.wide_color_space &&
           capabilities.high_color_space &&
           (capabilities.output_bits.rgb_10bpc || capabilities.dithering_bits.rgb_8bpc);
  }
}  // namespace virtual_display::driver
