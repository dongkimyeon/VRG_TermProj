#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace VrgNoise
{
void AppendNoiseMarkdown(const FJsonObject& Noise, FString& Markdown);
void AppendNoiseComparison(const FJsonObject& ReportA, const FJsonObject& ReportB, FString& Markdown);
}
