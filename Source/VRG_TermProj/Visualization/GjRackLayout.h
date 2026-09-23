#pragma once
#include "CoreMinimal.h"

// GJ GB200 rack geometry shared by the rack, spine cabinet and patch planner.
// UE rack-local metres: +X intake face (sled pull direction), +Y UE right, +Z up, pivot = floor centre.
// Must match Art/GJ_PIPE&CABLE/scripts/rear_data_layout.py (Blender Y is mirrored: y_ue = -y_blender).
// Display geometry only. Nothing here feeds the thermal, hydraulic or air solvers.
namespace GjRackLayout
{
    inline constexpr double FrameDepthM = 1.068, FrameWidthM = .6, FrameHeightM = 2.236;
    inline constexpr double UnitM = .04445;              // 1U
    inline constexpr double NodeXM = .055, NodeZ0M = .392;
    inline constexpr int32 NumNodes = 18;
    inline constexpr double SwitchXM = -.134, SwitchZM = 1.91;   // mounted reversed (yaw 180)
    inline constexpr double SwitchPortXM = -.476, PlugTailM = .044;
    inline constexpr double LowerPortZM = .012, UpperPortZM = .031; // above the switch pivot
    inline constexpr int32 PortsPerSwitchRow = 18, PortsPerSwitch = 36;
    // Usable mounting height: deck top to the front upper header.
    inline constexpr double MountBottomM = .081, MountTopM = 2.186;
    // Blank plates sit on the front rail face.
    inline constexpr double BlankFaceXM = .476;

    inline double NodeZ(int32 Node) { return NodeZ0M + Node * UnitM; }

    // Reversed switch: 1-based cage column c faces -X at y = -0.197 + (c-1)*0.022.
    inline FVector SwitchPortM(double SwitchZ, int32 Column, bool bUpper)
    {
        return FVector(SwitchPortXM, -.197 + (Column - 1) * .022, SwitchZ + (bUpper ? UpperPortZM : LowerPortZM));
    }

    struct FBlankSlot { double BottomM, HeightM; };

    // Fill every free gap between occupied [bottom, top) intervals with whole 1U plates. A gap that is
    // not an exact multiple of 1U is split evenly (a stretched plate rather than a visible sliver).
    inline TArray<FBlankSlot> BlankSlots(TArray<TPair<double, double>> Occupied)
    {
        Occupied.Sort([](const TPair<double, double>& A, const TPair<double, double>& B) { return A.Key < B.Key; });
        TArray<FBlankSlot> Out; double Z = MountBottomM;
        auto Fill = [&](double From, double To)
        {
            const int32 N = FMath::FloorToInt32((To - From) / UnitM + .3);
            for (int32 I = 0; I < N; ++I) Out.Add({From + I * (To - From) / N, (To - From) / N});
        };
        for (const TPair<double, double>& R : Occupied) { if (R.Key > Z) Fill(Z, R.Key); Z = FMath::Max(Z, R.Value); }
        if (MountTopM > Z) Fill(Z, MountTopM);
        return Out;
    }

    // Compute rack: 18 contiguous 1U nodes and one 1U switch.
    inline TArray<FBlankSlot> ComputeRackBlankSlots()
    {
        return BlankSlots({{NodeZ(0), NodeZ(NumNodes)}, {SwitchZM, SwitchZM + UnitM}});
    }
}
