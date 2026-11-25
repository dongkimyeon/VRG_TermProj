# One unattended hall case (2026-11-25): load a layout, set the options, run N steps, write a report, quit.
# Usage: .\Tools\run_hall_case.ps1 -Layout Hall64_C4_R16 -Label Hall64_C4_R16 -Steps 15000 [-Extra "vrg.Plant.Enable 1"]
# Engine path: set $env:UE_ROOT (default: C:\Program Files\Epic Games\UE_5.8). Runs headless (-NullRHI); not an FPS test.
param(
    [Parameter(Mandatory = $true)][string]$Layout,
    [Parameter(Mandatory = $true)][string]$Label,
    [int]$Steps = 15000,
    [string]$Extra = '',
    [int]$Rated = 1
)
$repo = Split-Path -Parent $PSScriptRoot
$ue = if ($env:UE_ROOT) { $env:UE_ROOT } else { 'C:\Program Files\Epic Games\UE_5.8' }
$cmds = @("vrg.Layout.Load $Layout", "vrg.Size.Rated $Rated")
if ($Extra) { $cmds += ($Extra -split ';') }
$cmds += @("vrg.Eval.ReportWhenDone $Label quit", "vrg.Air.RunSteps $Steps")
$exec = '-ExecCmds=' + ($cmds -join ',')
$log = Join-Path $repo "Saved\Logs\Hall_$Label.log"
$t0 = Get-Date
& "$ue\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" (Join-Path $repo 'VRG_TermProj.uproject') /Game/Maps/L_Test -game -NullRHI -unattended -nosplash -nopause $exec "-abslog=$log" | Out-Null
$secs = ((Get-Date) - $t0).TotalSeconds
$report = Join-Path $repo "Saved\Reports\$Label.json"
"{0}: exit {1}, {2:N0} s, report {3}" -f $Label, $LASTEXITCODE, $secs, (Test-Path $report)
