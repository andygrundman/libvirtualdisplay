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

## Useful targets

```powershell
cmake -S . -B build -G Ninja -DBUILD_TESTS=ON -DLIBVIRTUALDISPLAY_GOOGLETEST_SOURCE_DIR=D:/sources/sunshine/third-party/googletest
cmake --build build -j 10
.\build\tests\test_libvirtualdisplay.exe
```

To build only the Windows driver and runtime probe for Sunshine packaging:

```powershell
cmake -S . -B build-driver -G Ninja -DBUILD_TESTS=OFF -DBUILD_SUNSHINE_VIRTUAL_DISPLAY_DRIVER=ON -DBUILD_VIRTUALDISPLAY_PROBE=ON
cmake --build build-driver --target SunshineVirtualDisplayDriver virtualdisplay_probe -j 10
```

On Windows, the default build also produces `virtualdisplay_probe.exe`. After
the driver is installed, use it for runtime protocol checks:

```powershell
.\build\src\driver\virtualdisplay_probe.exe --check
.\build\src\driver\virtualdisplay_probe.exe --diagnose
.\build\src\driver\virtualdisplay_probe.exe --query-permanent
.\build\src\driver\virtualdisplay_probe.exe --self-test-permanent
.\build\src\driver\virtualdisplay_probe.exe --self-test-temp 1920 1080 60
.\build\src\driver\virtualdisplay_probe.exe --self-test-hdr 1920 1080 60
```

Sunshine refreshes package assets from this repo through:

```powershell
cmake --build D:/sources/sunshine/build --target refresh_sunshine_virtual_display_driver_assets
```

The Sunshine installer runs the packaged probe after driver installation. It
requires both `--query-permanent` and `--self-test-hdr` to pass before reporting
the driver install as successful.
