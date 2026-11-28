param(
    [Parameter(Mandatory)][string]$Engine,
    [string]$Project = (Join-Path (Split-Path $PSScriptRoot -Parent) 'VRG_TermProj.uproject'),
    [ValidateSet('Normal','Leak')][string]$Scenario = 'Normal',
    [ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Label = 'NoiseN6',
    [switch]$Offscreen
)
$ErrorActionPreference = 'Stop'
$Project = (Resolve-Path -LiteralPath $Project).Path
$Root = Split-Path $Project -Parent
$Exe = (Resolve-Path -LiteralPath (Join-Path $Engine 'Engine/Binaries/Win64/UnrealEditor.exe')).Path
$Directory = Join-Path $Root 'Saved/Logs'

foreach ($Mode in 'On','Off') {
    if (Get-Process -Name UnrealEditor,UnrealEditor-Cmd -ErrorAction SilentlyContinue) {
        throw 'Close other Unreal processes before taking paired measurements.'
    }
    $Case = '{0}_{1}_{2}' -f $Label,$Scenario,$Mode
    $Capture = Join-Path $Root "Saved/Profiling/VrgPerf-$Case.csv"
    $Log = Join-Path $Directory "$Case.log"
    if (Test-Path -LiteralPath $Capture) { throw "Choose another label. Existing capture: $Capture" }
    $Commands = @('r.VSync 0','t.MaxFPS 0','vrg.Perf.Overlay 0','vrg.Web.AutoOpen 0','vrg.Web.Enable 0',
        'vrg.Sim.Speed 1','vrg.Plant.Enable 1','vrg.Eval.Event clear')
    if ($Scenario -eq 'Leak') {
        $Commands += 'vrg.Eval.Event 3 leakat rack:0:tray:0:plate 3', 'vrg.Water.Impact 1'
    }
    if ($Mode -eq 'On') {
        $Commands += 'vrg.Noise.IndoorMap 1.5 1.5', 'vrg.Noise.Probe 45 0 2', 'vrg.Noise.Boundary 5 2 45 -10 45 10', 'vrg.Noise.Show 1'
    }
    $WarmName = $Case + '_Warm'
    $Commands += 'vrg.Air.RunSteps 200', 'vrg.Cam 16 -12 10 -24 132', "vrg.Eval.ReportWhenDone $WarmName", "vrg.Perf.Capture 20 $Case quit"
    $Arguments = @('"' + $Project + '"', '/Game/Maps/L_Test', '-game', '-d3d12', '-windowed',
        '-ResX=1280', '-ResY=720', '-nosplash', '-nosound', '-unattended', ('-abslog="' + $Log + '"'),
        ('-ExecCmds="' + ($Commands -join ',') + '"'))
    $WindowStyle = 'Normal'
    if ($Offscreen) { $Arguments += '-RenderOffscreen'; $WindowStyle = 'Hidden' }
    Write-Output "Capturing $Case"
    $Process = Start-Process -FilePath $Exe -ArgumentList $Arguments -WorkingDirectory $Root -WindowStyle $WindowStyle -PassThru
    try {
        if (-not $Process.WaitForExit(300000)) { throw "Capture timed out. See $Log" }
        if ($Process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $Capture)) { throw "Capture failed. See $Log" }
        $Warm = Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $Root "Saved/Reports/$WarmName.json") | ConvertFrom-Json
        if ([Math]::Abs($Warm.simTimeS - 4.0) -gt 1e-9) { throw "Unexpected warm-up SimTime: $($Warm.simTimeS)" }
        $Groups = @($Warm.noise.groups | ForEach-Object { $_.group } | Sort-Object) -join ','
        if (($Mode -eq 'On' -and $Groups -ne 'boundary,indoor,probe') -or ($Mode -eq 'Off' -and $Groups)) {
            throw "Unexpected receiver groups: $Groups"
        }
        Get-Content -Encoding UTF8 -LiteralPath (Join-Path $Root "Saved/Profiling/VrgPerf-$Case.txt")
    } finally {
        if (-not $Process.HasExited) { Stop-Process -InputObject $Process -ErrorAction SilentlyContinue }
    }
}
