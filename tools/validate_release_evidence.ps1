param(
  [string] $EvidenceJson,
  [string] $EvidencePath
)

$ErrorActionPreference = 'Stop'

function Read-EvidenceText {
  if (-not [string]::IsNullOrWhiteSpace($EvidenceJson)) {
    return $EvidenceJson
  }

  if (-not [string]::IsNullOrWhiteSpace($EvidencePath)) {
    if (-not (Test-Path -LiteralPath $EvidencePath)) {
      throw "Release evidence file not found: $EvidencePath"
    }
    return Get-Content -LiteralPath $EvidencePath -Raw
  }

  throw 'Release evidence JSON is required before publishing a production package.'
}

function Require-Value($Object, [string] $Name) {
  $value = $Object.$Name
  if ($null -eq $value -or ([string] $value).Trim().Length -eq 0) {
    throw "Release evidence is missing '$Name'."
  }
  return $value
}

function Require-Pass($Object, [string] $Name) {
  $value = Require-Value $Object $Name
  if ($value -ne $true) {
    throw "Release evidence '$Name' must be true."
  }
}

$evidence = Read-EvidenceText | ConvertFrom-Json

Require-Value $evidence 'commit_sha' | Out-Null
Require-Value $evidence 'tag' | Out-Null
Require-Value $evidence 'windows_sdk_version' | Out-Null
Require-Value $evidence 'wdk_version' | Out-Null
Require-Value $evidence 'driver_package' | Out-Null
Require-Value $evidence 'tool_package' | Out-Null

$signing = Require-Value $evidence 'signing'
if ($signing.channel -ne 'HLK/WHQL') {
  throw "Production release signing channel must be HLK/WHQL."
}
Require-Value $signing 'submission_id' | Out-Null
Require-Value $signing 'catalog_result' | Out-Null

$requiredHlkTests = @(
  'Indirect Display Inactive Path',
  'Indirect Display Mode Change',
  'Indirect Display PnP Stop-Start Indirect Display Adapter',
  'Indirect Display PnP Stop-Start Render Adapter',
  'Indirect Display Render Adapter TDR'
)

$hlk = Require-Value $evidence 'hlk'
Require-Value $hlk 'project' | Out-Null
foreach ($testName in $requiredHlkTests) {
  $entry = $hlk.tests | Where-Object { $_.name -eq $testName } | Select-Object -First 1
  if ($null -eq $entry) {
    throw "HLK evidence is missing '$testName'."
  }
  if ($entry.status -ne 'passed' -and $entry.waiver_accepted -ne $true) {
    throw "HLK test '$testName' must pass or have an accepted waiver."
  }
}

Require-Pass $evidence 'hvci_readiness_passed'
Require-Pass $evidence 'memory_integrity_functional_passed'
Require-Pass $evidence 'fresh_install_passed'
Require-Pass $evidence 'upgrade_install_passed'
Require-Pass $evidence 'permanent_identity_retention_passed'
Require-Pass $evidence 'temporary_cleanup_passed'

Write-Host 'Release evidence gate passed.'
