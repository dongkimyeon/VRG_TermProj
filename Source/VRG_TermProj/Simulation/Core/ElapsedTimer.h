#pragma once
#include "CoreMinimal.h"

// Observer only: instance/local output, no global state and no solver policy changes.
class FElapsedTimer
{
public:
 explicit FElapsedTimer(double& Ms) : OutMs(Ms), Start(FPlatformTime::Cycles64()) {}
 ~FElapsedTimer() { OutMs += FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - Start); }
private:
 double& OutMs;
 uint64 Start;
};
