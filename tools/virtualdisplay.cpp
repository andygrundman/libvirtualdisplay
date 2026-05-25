#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vdd = virtual_display::driver;

namespace {
  void print_usage() {
    std::cout
      << "virtualdisplay commands:\n"
      << "  status\n"
      << "  spawn [--width N] [--height N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent query\n"
      << "  permanent set --count N [--width N] [--height N] [--refresh HZ] [--name TEXT]\n"
      << "  permanent off\n";
  }

  int fail(const std::string &message, const vdd::ControlOperationResult &result) {
    std::cerr << message << ": " << vdd::to_string(result.status);
    if (result.native_error != 0) {
      std::cerr << " native_error=" << result.native_error;
    }
    std::cerr << '\n';
    return 1;
  }

  template<class T>
  int fail(const std::string &message, const vdd::ControlResult<T> &result) {
    return fail(message, {result.status, result.native_error});
  }

  bool parse_u32(const std::string_view value, std::uint32_t &parsed) {
    std::uint32_t output {};
    const auto *begin = value.data();
    const auto *end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, output);
    if (result.ec != std::errc {} || result.ptr != end) {
      return false;
    }
    parsed = output;
    return true;
  }

  bool parse_refresh_millihz(const std::string_view value, std::uint32_t &parsed) {
    std::string text {value};
    try {
      const auto refresh = std::stod(text);
      if (refresh <= 0.0) {
        return false;
      }
      parsed = static_cast<std::uint32_t>(refresh <= 1000.0 ? refresh * 1000.0 : refresh);
      return true;
    } catch (...) {
      return false;
    }
  }

  void set_display_name(char (&target)[vdd::kDisplayNameChars], const std::string &name) {
    std::fill(std::begin(target), std::end(target), '\0');
    std::memcpy(target, name.data(), (std::min)(name.size(), static_cast<std::size_t>(vdd::kDisplayNameChars - 1)));
  }

  std::string display_name(const char (&value)[vdd::kDisplayNameChars]) {
    return std::string {vdd::trim_display_name(value)};
  }

  void print_permanent_state(const vdd::PermanentDisplayCountResult &state) {
    std::cout
      << "permanent_displays=" << state.current_display_count << '\n'
      << "max_permanent_displays=" << state.max_display_count << '\n'
      << "temporary_displays=" << state.temporary_display_count << '\n'
      << "mode=" << state.width << 'x' << state.height << '@'
      << (state.refresh_rate_millihz / 1000.0) << "Hz\n"
      << "name=" << display_name(state.display_name) << '\n';
  }

  struct PermanentOptions {
    std::uint32_t count {1};
    std::uint32_t width {1920};
    std::uint32_t height {1080};
    std::uint32_t refresh_rate_millihz {60'000};
    std::string name {"Sunshine Display"};
  };

  std::optional<PermanentOptions> parse_permanent_options(
    const std::vector<std::string> &args,
    const std::size_t first,
    const bool require_count
  ) {
    PermanentOptions options {};
    bool saw_count = false;

    for (std::size_t index = first; index < args.size(); ++index) {
      const auto &arg = args[index];
      const auto need_value = [&]() -> std::optional<std::string> {
        if (index + 1 >= args.size()) {
          std::cerr << arg << " requires a value\n";
          return std::nullopt;
        }
        return args[++index];
      };

      if (arg == "--count") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.count)) {
          std::cerr << "invalid --count value\n";
          return std::nullopt;
        }
        saw_count = true;
      } else if (arg == "--width") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.width)) {
          std::cerr << "invalid --width value\n";
          return std::nullopt;
        }
      } else if (arg == "--height") {
        const auto value = need_value();
        if (!value || !parse_u32(*value, options.height)) {
          std::cerr << "invalid --height value\n";
          return std::nullopt;
        }
      } else if (arg == "--refresh") {
        const auto value = need_value();
        if (!value || !parse_refresh_millihz(*value, options.refresh_rate_millihz)) {
          std::cerr << "invalid --refresh value\n";
          return std::nullopt;
        }
      } else if (arg == "--name") {
        const auto value = need_value();
        if (!value || value->empty()) {
          std::cerr << "invalid --name value\n";
          return std::nullopt;
        }
        options.name = *value;
      } else {
        std::cerr << "unknown option: " << arg << '\n';
        return std::nullopt;
      }
    }

    if (require_count && !saw_count) {
      std::cerr << "permanent set requires --count\n";
      return std::nullopt;
    }
    return options;
  }

  vdd::PermanentDisplayCountRequest make_request(const PermanentOptions &options) {
    vdd::PermanentDisplayCountRequest request {};
    request.display_count = options.count;
    request.width = options.width;
    request.height = options.height;
    request.refresh_rate_millihz = options.refresh_rate_millihz;
    set_display_name(request.display_name, options.name);
    return request;
  }

  int set_permanent(vdd::ControlClient &client, const PermanentOptions &options) {
    auto request = make_request(options);
    const auto validation = vdd::validate_permanent_display_count(request, 64);
    if (validation != vdd::ValidationError::None && validation != vdd::ValidationError::PermanentDisplayCountTooHigh) {
      std::cerr << "invalid display settings: " << vdd::to_string(validation) << '\n';
      return 2;
    }

    const auto result = client.set_permanent_display_count(request);
    if (!result.ok()) {
      return fail("set permanent display failed", result);
    }

    print_permanent_state(result.value);
    return 0;
  }
}  // namespace

int main(int argc, char **argv) {
  if (argc < 2 || std::string_view {argv[1]} == "--help" || std::string_view {argv[1]} == "help") {
    print_usage();
    return argc < 2 ? 2 : 0;
  }

  const std::vector<std::string> args {argv + 1, argv + argc};
  const auto opened = vdd::open_first_control_device();
  if (!opened.ok()) {
    return fail("open control device failed", {opened.status, opened.native_error});
  }

  vdd::ControlClient client {*opened.transport};
  const auto protocol = client.check_protocol_compatible();
  if (!protocol.ok()) {
    return fail("control protocol check failed", protocol);
  }

  if (args[0] == "status" || (args[0] == "permanent" && args.size() >= 2 && args[1] == "query")) {
    const auto result = client.query_permanent_display_count();
    if (!result.ok()) {
      return fail("query permanent display failed", result);
    }
    print_permanent_state(result.value);
    return 0;
  }

  if (args[0] == "spawn") {
    const auto options = parse_permanent_options(args, 1, false);
    if (!options) {
      return 2;
    }
    return set_permanent(client, *options);
  }

  if (args[0] == "permanent" && args.size() >= 2 && args[1] == "off") {
    PermanentOptions options {};
    options.count = 0;
    return set_permanent(client, options);
  }

  if (args[0] == "permanent" && args.size() >= 2 && args[1] == "set") {
    const auto options = parse_permanent_options(args, 2, true);
    if (!options) {
      return 2;
    }
    return set_permanent(client, *options);
  }

  print_usage();
  return 2;
}
