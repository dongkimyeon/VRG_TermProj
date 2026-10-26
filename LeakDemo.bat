@echo off
rem Leak spray screen check: opens L_Test, enters simulation, opens a 3 mm leak at t=1 s and frames it.
rem Optional argument: hole diameter in mm (e.g. LeakDemo.bat 5).
setlocal

set "UE=C:\Program Files\Epic Games\UE_5.8"
set "REPO=C:\Github-repo\VRG_TermProj"
set "MM=%~1"
if "%MM%"=="" set "MM=3"

"%UE%\Engine\Binaries\Win64\UnrealEditor.exe" "%REPO%\VRG_TermProj.uproject" /Game/Maps/L_Test -game -windowed -ResX=1600 -ResY=900 "-ExecCmds=vrg.Water.Demo %MM%"

endlocal
