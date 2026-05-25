# libvirtualdisplay

Clean-room virtual display driver support for Sunshine.

This repository owns Sunshine's dedicated virtual display control protocol,
identity model, EDID generation, and Windows UMDF/IddCx driver implementation.
It is intentionally separate from Sunshine's existing `libdisplaydevice`
dependency, which remains only a display enumeration/settings library.

The expected local checkout for Sunshine integration is:

```text
D:\sources\libvirtualdisplay
```

Sunshine discovers this checkout from `D:\sources\sunshine` via
`../libvirtualdisplay` unless `SUNSHINE_LIBVIRTUALDISPLAY_SOURCE_DIR` is set.

## Scope

The driver is dedicated to Sunshine virtual-display use cases:

- create and remove temporary displays through the control API
- set and query a small permanent-display pool
- preserve stable display identity through `DisplayId`
- keep lease lifetime separate from display identity through `LeaseId`
- advertise HDR-capable EDID and IddCx target capabilities
- provide runtime probes that verify the control path and HDR target behavior

This is a clean-room implementation. Do not import source from SudoVDA, MttVDD,
or Virtual-Display-Driver. Compatibility work should happen at the API/behavior
level, not by copying driver internals.

## Build

```powershell
cmake -S . -B build -G Ninja -DBUILD_TESTS=ON -DLIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR=D:/sources/sunshine/third-party/googletest
cmake --build build -j 10
.\build\tests\test_libvirtualdisplay.exe
```

To build only the Windows driver, CLI, and runtime probe:

```powershell
cmake -S . -B build-driver -G Ninja -DBUILD_TESTS=OFF -DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=ON -DBUILD_VIRTUALDISPLAY_PROBE=ON
cmake --build build-driver --target SunshineVirtualDisplayDriver virtualdisplay virtualdisplay_probe -j 10
```

The commands below assume the driver package has already been installed and
Windows has started the Sunshine virtual display device.

## CLI usage

`virtualdisplay.exe` is the user-facing command line tool. It talks to the
installed driver through the same control interface Sunshine uses.

Query the current permanent-display state:

```powershell
.\build-driver\src\driver\virtualdisplay.exe status
.\build-driver\src\driver\virtualdisplay.exe permanent query
```

Create one permanent display with the default settings:

```powershell
.\build-driver\src\driver\virtualdisplay.exe spawn
```

Create or update a permanent display with explicit settings:

```powershell
.\build-driver\src\driver\virtualdisplay.exe spawn --width 2560 --height 1440 --refresh 120
.\build-driver\src\driver\virtualdisplay.exe permanent set --count 1 --width 3840 --height 2160 --refresh 144 --name "Desk Display"
```

Create several permanent displays with the same EDID mode/name settings:

```powershell
.\build-driver\src\driver\virtualdisplay.exe permanent set --count 2 --width 1920 --height 1080 --refresh 60 --name "Virtual Display"
```

Remove all permanent displays:

```powershell
.\build-driver\src\driver\virtualdisplay.exe permanent off
```

Supported settings:

- `--count`: permanent display count, from `0` through the driver's maximum.
- `--width`: preferred display width in pixels.
- `--height`: preferred display height in pixels.
- `--refresh`: preferred refresh rate. Values up to `1000` are treated as Hz, so `120` means `120 Hz`; larger values are treated as millihertz.
- `--name`: monitor name advertised in EDID, truncated to the protocol limit.

The driver validates modes against the protocol range:

- width: `320` through `16384`
- height: `200` through `16384`
- refresh: `23 Hz` through `480 Hz`

Use `virtualdisplay_probe.exe` for diagnostics and runtime QA rather than normal
display management:

```powershell
.\build-driver\src\driver\virtualdisplay_probe.exe --check
.\build-driver\src\driver\virtualdisplay_probe.exe --diagnose
.\build-driver\src\driver\virtualdisplay_probe.exe --query-permanent
.\build-driver\src\driver\virtualdisplay_probe.exe --self-test-permanent
.\build-driver\src\driver\virtualdisplay_probe.exe --self-test-temp 1920 1080 60
.\build-driver\src\driver\virtualdisplay_probe.exe --self-test-hdr 1920 1080 60
```

## Programmatic usage

The public C++ control surface is in:

- `virtual_display/driver/control_protocol.h`
- `virtual_display/driver/control_client.h`
- `virtual_display/driver/windows_control_client.h`

On Windows, open the first installed control device, check protocol
compatibility, and construct a `ControlClient`:

```cpp
#include "virtual_display/driver/control_client.h"
#include "virtual_display/driver/windows_control_client.h"

namespace vdd = virtual_display::driver;

auto opened = vdd::open_first_control_device();
if (!opened.ok()) {
  // opened.status and opened.native_error explain the failure.
  return 1;
}

vdd::ControlClient client {*opened.transport};
auto protocol = client.check_protocol_compatible();
if (!protocol.ok()) {
  return 1;
}
```

### Permanent displays

Permanent displays stay present until the count is changed or the driver/device
is restarted. Use them for simple desktop setups where a user wants an always-on
virtual monitor.

```cpp
vdd::PermanentDisplayCountRequest request {};
request.display_count = 1;
request.width = 2560;
request.height = 1440;
request.refresh_rate_millihz = 120'000;
std::strncpy(request.display_name, "Desk Display", sizeof(request.display_name) - 1);

auto result = client.set_permanent_display_count(request);
if (!result.ok()) {
  return 1;
}

// result.value reports current_display_count, max_display_count,
// temporary_display_count, width, height, refresh_rate_millihz, and display_name.
```

Query the current permanent-display state:

```cpp
auto state = client.query_permanent_display_count();
if (!state.ok()) {
  return 1;
}
```

Remove all permanent displays:

```cpp
vdd::PermanentDisplayCountRequest request {};
request.display_count = 0;
auto result = client.set_permanent_display_count(request);
```

### Temporary displays

Temporary displays are lease-backed. They are useful when an application needs a
display only while a session is active. Keep feeding the lease for as long as the
display should remain present, then remove the display or release the lease.

```cpp
vdd::CreateTemporaryDisplayRequest request {};
request.lease_id = 0x1001;
request.display_id = 0x2001;
request.width = 1920;
request.height = 1080;
request.refresh_rate_millihz = 60'000;
request.requested_timeout_ms = 30'000;
std::strncpy(request.display_name, "Session Display", sizeof(request.display_name) - 1);

auto created = client.create_temporary_display(request);
if (!created.ok()) {
  return 1;
}

vdd::LeaseRequest lease {
  vdd::kApiNamespaceGuid,
  request.lease_id,
  request.requested_timeout_ms,
  0
};

// Call periodically before remaining_ms reaches zero.
(void) client.feed_lease(lease);

vdd::LeaseDisplayRequest remove {
  vdd::kApiNamespaceGuid,
  request.lease_id,
  request.display_id
};
(void) client.remove_temporary_display(remove);
```

Release every temporary display owned by a lease:

```cpp
vdd::LeaseRequest lease {
  vdd::kApiNamespaceGuid,
  request.lease_id,
  0,
  0
};
(void) client.release_lease(lease);
```

### Protocol notes

- The current protocol version is `2.0.0`.
- `ControlClient::check_protocol_compatible()` should be called before issuing
  state-changing requests.
- IDs are caller-owned. `DisplayId` is the stable display identity;
  `LeaseId` is only the temporary-display lifetime owner.
- Names are fixed-size protocol strings with a `32` byte limit.
- Permanent-display settings are shared by the active permanent pool; setting
  the same count with new mode/name settings refreshes existing permanent
  monitors.

Sunshine refreshes package assets from this repo through:

```powershell
cmake --build D:/sources/sunshine/build --target refresh_sunshine_virtual_display_driver_assets
```

The Sunshine package refresh copies the driver and tool artifacts into
Sunshine's staged Windows driver assets.
