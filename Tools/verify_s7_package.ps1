param(
    [Parameter(Mandatory = $true)][string]$PackageSaved,
    [Parameter(Mandatory = $true)][string]$LogDirectory
)
# Read-only verification of the 2026-10-07 package smoke artifacts.
# Physics limits are existing CLAUDE.md criteria, not newly fitted tolerances.
$ErrorActionPreference = 'Stop'
function Require-S7([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Read-S7Json([string]$Relative) {
    Get-Content -Raw -LiteralPath (Join-Path $PackageSaved $Relative) | ConvertFrom-Json
}
$firstText = Get-Content -Raw -LiteralPath (Join-Path $PackageSaved 'Layouts/S7Pkg_Roundtrip1.json')
$secondText = Get-Content -Raw -LiteralPath (Join-Path $PackageSaved 'Layouts/S7Pkg_Roundtrip2.json')
$rejectedText = Get-Content -Raw -LiteralPath (Join-Path $PackageSaved 'Layouts/S7Pkg_AfterInvalid.json')
Require-S7 ($firstText -ceq $secondText) 'Repeated Load changed the saved inventory/configuration.'
Require-S7 ($secondText -ceq $rejectedText) 'Rejected Load partially changed saved state.'
$saved = $firstText | ConvertFrom-Json
$fixture = Read-S7Json 'Layouts/S7Pkg_IntegrationFixture.json'
$expectedIds = @($fixture.equipment | Where-Object class -eq 'RackThermalActor' | ForEach-Object rackId | Sort-Object)
$actualIds = @($saved.equipment | Where-Object class -eq 'RackThermalActor' | ForEach-Object rackId | Sort-Object)
Require-S7 (($expectedIds -join '|') -ceq ($actualIds -join '|')) 'Rack identities differ from the loaded fixture.'
Require-S7 (@($actualIds | Sort-Object -Unique).Count -eq $actualIds.Count) 'Duplicate rack identity.'
Require-S7 ($saved.equipment.Count -eq $fixture.equipment.Count) 'Equipment count changed.'
foreach ($field in @('manual','minXM','minYM','maxXM','maxYM','ceilingHeightM')) {
    Require-S7 ($saved.settings.roomAirDomain.$field -eq $fixture.settings.roomAirDomain.$field) "Room field mismatch: $field"
}
Require-S7 ($saved.settings.roomCeilingPlates.Count -eq 1) 'Ceiling plate count mismatch.'
Require-S7 ($saved.settings.roomCeilingPlates[0].holes.Count -eq 1) 'Ceiling hole count mismatch.'
foreach ($field in @('minXM','minYM','maxXM','maxYM','heightM')) {
    Require-S7 ($saved.settings.roomCeilingPlates[0].$field -eq $fixture.settings.roomCeilingPlates[0].$field) "Plate field mismatch: $field"
}
foreach ($field in @('minXM','minYM','maxXM','maxYM')) {
    Require-S7 ($saved.settings.roomCeilingPlates[0].holes[0].$field -eq $fixture.settings.roomCeilingPlates[0].holes[0].$field) "Hole field mismatch: $field"
}
foreach ($case in @(
    @{Name='S7Pkg_FinalBase'; Cells=33210; Steps=1500},
    @{Name='S7Pkg_FinalManual'; Cells=64512; Steps=100}
)) {
    $report = Read-S7Json ("Reports/{0}.json" -f $case.Name)
    Require-S7 ($report.config.cells -eq $case.Cells) 'Actual grid cell count mismatch.'
    $residual = [double]$report.checks.globalNormalizedEnergyResidual
    Require-S7 (-not [double]::IsNaN($residual) -and -not [double]::IsInfinity($residual) -and [math]::Abs($residual) -lt 1e-9) 'Global energy balance failed.'
    Require-S7 ($report.checks.pressureSolves -eq $case.Steps -and $report.checks.pressureFailures -eq 0) 'Pressure convergence failed.'
    Require-S7 ([bool]$report.checks.hydraulicsConverged) 'Hydraulic convergence failed.'
    $pumpRatio = $report.power.pumpW / $report.power.itW
    Require-S7 ($pumpRatio -gt 0 -and $pumpRatio -lt 0.05) 'Pump/IT plausibility failed.'
    foreach ($inlet in $report.inlets) {
        Require-S7 ($inlet.windowMeanC -ge 15 -and $inlet.peakC -le 35) 'Inlet temperature plausibility failed.'
    }
    Require-S7 ($report.summary.inletsAboveRecommended -eq 0 -and $report.summary.inletsAboveAllowable -eq 0) 'Inlet limits exceeded.'
    '{0}: cells={1}, steps={2}, absEnergy={3:E6}, pump/IT={4:F6}' -f $case.Name,$report.config.cells,$report.checks.pressureSolves,[math]::Abs($report.checks.globalNormalizedEnergyResidual),$pumpRatio
}
foreach ($name in @('S7Final_Package_CompatUI.log','S7Final_Package_Manual.log')) {
    $log = Get-Content -Raw -LiteralPath (Join-Path $LogDirectory $name)
    Require-S7 ($log -notmatch ': Error:|: Fatal:') "Runtime error in $name"
    $lastInlets = [regex]::Matches($log, '흡기 .*마지막 (-?\d+\.\d+) C')
    Require-S7 ($lastInlets.Count -ge 1) "No final inlet samples in $name"
    foreach ($sample in $lastInlets) {
        $celsius = [double]::Parse($sample.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
        Require-S7 ($celsius -ge 15 -and $celsius -le 35) "Final inlet outside the existing range in $name"
    }
}
'PASS: repeated/new-world Load, rejected-Load state, IDs, room/plate/hole, and existing physics criteria.'
