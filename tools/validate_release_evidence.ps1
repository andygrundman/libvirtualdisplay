param(
  [string] $EvidenceJson,
  [string] $EvidencePath,
  [string] $ExpectedTag,
  [string] $ExpectedCommit,
  [string] $PackagePath
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

function Require-BooleanTrue($Object, [string] $Name) {
  $property = $Object.PSObject.Properties[$Name]
  if ($null -eq $property) {
    throw "Release evidence is missing '$Name'."
  }

  $value = $property.Value
  if ($value -isnot [bool] -or $value -ne $true) {
    throw "Release evidence '$Name' must be JSON boolean true."
  }
}

$evidence = Read-EvidenceText | ConvertFrom-Json

$commitSha = Require-Value $evidence 'commit_sha'
$tag = Require-Value $evidence 'tag'
if (-not [string]::IsNullOrWhiteSpace($ExpectedTag) -and $tag -ne $ExpectedTag) {
  throw "Release evidence tag '$tag' does not match expected tag '$ExpectedTag'."
}
if (-not [string]::IsNullOrWhiteSpace($ExpectedCommit) -and $commitSha.ToLowerInvariant() -ne $ExpectedCommit.ToLowerInvariant()) {
  throw "Release evidence commit '$commitSha' does not match expected commit '$ExpectedCommit'."
}
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
  $matches = @($hlk.tests | Where-Object { $_.name -eq $testName })
  if ($matches.Count -ne 1) {
    throw "HLK evidence must contain exactly one '$testName' result; found $($matches.Count)."
  }

  $entry = $matches[0]
  $waiver = $entry.PSObject.Properties['waiver_accepted']
  $waiverAccepted = $null -ne $waiver -and $waiver.Value -is [bool] -and $waiver.Value
  if ($entry.status -ne 'passed' -and -not $waiverAccepted) {
    throw "HLK test '$testName' must pass or have an accepted JSON boolean waiver."
  }
}

Require-BooleanTrue $evidence 'hvci_readiness_passed'
Require-BooleanTrue $evidence 'memory_integrity_functional_passed'
Require-BooleanTrue $evidence 'fresh_install_passed'
Require-BooleanTrue $evidence 'upgrade_install_passed'
Require-BooleanTrue $evidence 'permanent_identity_retention_passed'
Require-BooleanTrue $evidence 'temporary_cleanup_passed'

if (-not [string]::IsNullOrWhiteSpace($PackagePath)) {
  $packages = @(Get-ChildItem -Path $PackagePath -File)
  if ($packages.Count -ne 1) {
    throw "Expected exactly one release package for '$PackagePath', found $($packages.Count)."
  }
  $expectedPackageHash = Require-Value $evidence 'package_sha256'
  $actualPackageHash = (Get-FileHash -LiteralPath $packages[0].FullName -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actualPackageHash -ne $expectedPackageHash.ToLowerInvariant()) {
    throw "Release package SHA256 '$actualPackageHash' does not match evidence '$expectedPackageHash'."
  }
}

Write-Host 'Release evidence gate passed.'
