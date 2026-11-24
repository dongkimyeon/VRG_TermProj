# 2026-11-24 UI 데모 실행: L_Test를 창 모드 게임으로 열고 새 UI(vrg.Ui.Style 1)로 시작한다.
# 실행 중 F2로 기존 UI와 번갈아 볼 수 있다. Editor 빌드가 먼저 되어 있어야 한다.
# 사용: powershell -ExecutionPolicy Bypass -File Tools\UI데모_실행.ps1 [-Engine E:\UE_5.8] [-Scenario leak|leakprotect|flood|pump|valve|heatwave]
param(
    [string]$Engine = "E:\UE_5.8",
    [string]$Scenario = "",
    [int]$ResX = 1600,
    [int]$ResY = 900
)
$Project = Join-Path (Split-Path $PSScriptRoot -Parent) "VRG_TermProj.uproject"
$Exe = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.exe"
if (-not (Test-Path $Exe)) { Write-Error "엔진을 찾지 못했습니다: $Exe (-Engine 으로 경로를 주세요)"; exit 1 }
$Cmds = "vrg.Ui.Style 1"
# 시나리오를 주면 시뮬레이션에 들어가 2 s 뒤부터 예약한다(결과는 모델 계산).
if ($Scenario -ne "") { $Cmds += ", vrg.Air.RunSteps 1, vrg.Demo $Scenario" }
& $Exe $Project /Game/Maps/L_Test -game -windowed "-ResX=$ResX" "-ResY=$ResY" "-ExecCmds=$Cmds"
