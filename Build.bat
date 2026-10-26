@echo off
setlocal

set "UE=C:\Program Files\Epic Games\UE_5.8"
set "REPO=C:\Github-repo\VRG_TermProj"

call "%UE%\Engine\Build\BatchFiles\Build.bat" VRG_TermProjEditor Win64 Development "-Project=%REPO%\VRG_TermProj.uproject" -WaitMutex -MaxParallelActions=4

endlocal
