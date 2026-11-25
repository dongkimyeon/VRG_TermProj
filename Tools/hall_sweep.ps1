# 64-rack hall equipment-count sweep (2026-11-25, Tools/큰홀_장비대수_계획.md). Headless, one process per case, in order.
# A case whose report Saved/Reports/<Label>.json already exists is skipped, so the sweep can be stopped and resumed.
# Usage: .\Tools\hall_sweep.ps1 [-Only S_C3_R16,S_C4_R8] [-Steps 15000]
# Prerequisite: vrg.Layout.Save LTest_Base once (see make_hall_layout.py), then this script makes the layouts itself.
param([string[]]$Only = @(), [int]$Steps = 15000)
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo
if (-not (Test-Path 'Saved\Layouts\LTest_Base.json')) { throw 'Saved\Layouts\LTest_Base.json 없음: 게임에서 vrg.Layout.Save LTest_Base 먼저' }

# Layouts: name -> generator arguments.
$layouts = [ordered]@{
    'Hall64_C1_R16' = '--cdus 1 --crah-per-row 2 --plant 0'
    'Hall64_C2_R16' = '--cdus 2 --crah-per-row 2 --plant 0'
    'Hall64_C3_R16' = '--cdus 3 --crah-per-row 2 --plant 0'
    'Hall64_C4_R16' = '--cdus 4 --crah-per-row 2 --plant 0'
    'Hall64_C4_R8'  = '--cdus 4 --crah-per-row 1 --plant 0'
    'Hall64_C4_R24' = '--cdus 4 --crah-per-row 3 --plant 0'
}
foreach ($k in $layouts.Keys) { & py -I Tools\make_hall_layout.py --pods 4 $($layouts[$k] -split ' ') --out $k | Out-Null }

# Cases: label, layout, rated (vrg.Size.Rated), extra console commands (';'-separated).
$cases = @(
    @('S_C4_R16',      'Hall64_C4_R16', 1, 'vrg.Plant.Enable 0'),
    @('S_C1_R16',      'Hall64_C1_R16', 1, 'vrg.Plant.Enable 0'),
    @('S_C2_R16',      'Hall64_C2_R16', 1, 'vrg.Plant.Enable 0'),
    @('S_C3_R16',      'Hall64_C3_R16', 1, 'vrg.Plant.Enable 0'),
    @('S_C4_R8',       'Hall64_C4_R8',  1, 'vrg.Plant.Enable 0'),
    @('S_C4_R24',      'Hall64_C4_R24', 1, 'vrg.Plant.Enable 0'),
    @('S_C1_R16_auto', 'Hall64_C1_R16', 0, 'vrg.Plant.Enable 0'),
    @('P_C4_R16',      'Hall64_C4_R16', 1, 'vrg.Plant.Enable 1')
)
foreach ($c in $cases) {
    if ($Only.Count -gt 0 -and $Only -notcontains $c[0]) { continue }
    if (Test-Path "Saved\Reports\$($c[0]).json") { "$($c[0]): 리포트 있음, 건너뜀"; continue }
    & (Join-Path $PSScriptRoot 'run_hall_case.ps1') -Layout $c[1] -Label $c[0] -Steps $Steps -Rated $c[2] -Extra $c[3]
}
