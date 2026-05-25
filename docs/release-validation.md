# Release Validation

Production driver releases must be validated as Windows driver packages before
they are distributed as supported builds.

## Signing Channel

- Production distribution targets HLK/WHQL.
- Attestation signing is reserved for controlled lab, pilot, or developer
  side-load builds.
- Builds signed only through attestation must not be described as Windows
  Certified.

## Required Release Evidence

Keep these artifacts with each release record:

- Driver package with INF, CAT, DLL, and PDB.
- Tool package with `virtualdisplay.exe` and `virtualdisplay_probe.exe`.
- Exact commit SHA, tag, Windows SDK version, and WDK version.
- Signing submission result and returned catalog details.
- Test machine OS build, GPU/render adapter, and driver package version.

## HLK Graphics Gate

Run the IDD-focused graphics HLK coverage on each production candidate:

- Indirect Display Inactive Path.
- Indirect Display Mode Change.
- Indirect Display PnP Stop-Start Indirect Display Adapter.
- Indirect Display PnP Stop-Start Render Adapter.
- Indirect Display Render Adapter TDR.

The release record must include the HLK project or playlist, pass/fail summary,
test environment, and any accepted waivers.

## HVCI Gate

Run the HyperVisor Code Integrity Readiness Test for every driver-signing
candidate. Also run functional validation with Windows Memory Integrity enabled:

- Install the package.
- Create and remove a permanent virtual display.
- Create, feed, and release a temporary virtual display.
- Run the HDR probe on a supported Windows 11 HDR test system.

The release record must state whether Memory Integrity was enabled during each
functional pass.

## Functional Matrix

At minimum, release validation must cover:

- Windows 10 22H2 SDR behavior.
- Windows 11 23H2 or newer HDR-capable behavior.
- Fresh install.
- Upgrade install from the previous released package.
- Driver disable/enable or PnP stop/start.
- Permanent display identity retention across restart.
- Temporary display lease expiry and explicit release.

## Release Blockers

A production release is blocked until resolved when any of these are true:

- The driver package has not passed the required HLK/WHQL path.
- HVCI readiness fails.
- Memory Integrity functional validation fails.
- Required IDD HLK graphics coverage fails without an accepted waiver.
- The shipped package contains sample identifiers, a permissive control
  interface ACL, or undocumented protocol/ABI changes.
