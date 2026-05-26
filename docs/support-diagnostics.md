# Support Diagnostics

Use this playbook when collecting evidence for display creation, topology,
HDR, color profile, swapchain, or driver restart issues.

## Driver ETW Trace

The UMDF driver registers the `Sunshine.VirtualDisplayDriver` TraceLogging
provider:

- Provider GUID: `{3d5d3bd9-8500-4523-9334-583f4b5e6f80}`
- WPP provider GUID: `{b0dcb744-045b-463b-9c2f-6a3c897d3458}`
- Event examples: `DriverEntry`, `DeviceAdd`, `MonitorArrived`,
  `MonitorDeparted`, `SwapChainAssigned`, `SwapChainUnassigned`,
  `RenderDeviceCreated`, `RenderDeviceLost`, `DefaultHdrMetadataSet`, and
  `GammaRampSet`.

Collect a live trace with:

```powershell
logman start SunshineVDD -p "{3d5d3bd9-8500-4523-9334-583f4b5e6f80}" 0xffffffff 0xff -ets
# Reproduce the issue.
logman stop SunshineVDD -ets
```

Export or archive the generated ETL with the matching driver package, PDB, and
commit SHA.

The driver also builds WPP with Inflight Trace Recorder enabled. When a live
debugger is attached to the WUDFHost process that hosts the driver, dump the
recent in-memory trace ring with:

```text
!wdfkd.wdflogdump
```

## Broker And Helper Events

The broker and active-session helper write Windows Event Log entries through
the `SunshineVirtualDisplayBroker` source. Capture recent entries with:

```powershell
wevtutil qe Application /q:"*[System[Provider[@Name='SunshineVirtualDisplayBroker']]]" /f:text /c:100
```

Include these helper command outputs when they are relevant:

```powershell
.\tools\virtualdisplay.exe broker protocol
.\tools\virtualdisplay.exe broker query-state
.\tools\virtualdisplay.exe broker query-manifest
.\tools\virtualdisplay.exe broker helper-diagnose
.\tools\virtualdisplay.exe broker helper-query-color-profiles
```

For profile association issues, first run `helper-query-color-profiles` and use
the printed `source_luid` plus `source_id` with
`broker helper-associate-color-profile`.

## IddCx Debug Knobs

When Microsoft IddCx diagnostics are needed, use the documented IddCx debugging
registry settings under:

`HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers`

Record the original values, apply only the requested debug settings, reproduce
the issue, collect the driver ETW and broker/helper Event Log evidence above,
then restore the original registry state. Microsoft's IddCx debugging reference
documents these common values:

- `TerminateIndirectOnStall`: disabling this watchdog is for local debugging
  only because it will fail HLK.
- `IddCxDebugCtrl=0x0001`: break into the debugger when IddCx detects an error.
- `IddCxDebugCtrl=0x0f4`: capture normal IddCx WPP logging on Windows build
  `19041` and later.
- `IddCxDebugCtrl=0x1f4`: include high-frequency frame-related IddCx WPP
  logging when frame timing is the issue.

Capture the IddCx class-extension WPP trace alongside the driver provider:

```powershell
logman create trace IddCx -o IddCx.etl -ets -ow -mode sequential -p "{D92BCB52-FA78-406F-A9A5-2037509FADEA}" 0x0f4 0xff
# Reproduce the issue.
logman stop IddCx -ets
```

Source: <https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-debugging>
