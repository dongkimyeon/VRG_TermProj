#pragma once
#include "CoreMinimal.h"

// Patch manager: decides which rack uplink goes to which spine port, and the path each cable takes
// through the overhead hot-aisle tray. Plain C++ (no UObject, no world) so it runs in automation tests.
// Units: world metres. The room actor converts to cm at its boundary (rule 1).
//
// This is a cabling plan for display. It does not model bandwidth, latency, cable length limits,
// tension, bend radius or cable-to-cable collision. The only hard rule is that no port is used twice.
namespace Patch
{
	struct FDevice
	{
		FTransform ToWorld;              // metres, unit scale
		FVector HalfExtentM = FVector(.534, .3, 0);  // footprint half size X/Y
		double HeightM = 2.236;
		int32 Row = INDEX_NONE;
		bool bSpine = false;
		TArray<FVector> PortM;           // port face centres, world metres
		FVector Outward = FVector::BackwardVector;   // out of every port (the cabinet rear)
		TArray<int32> UplinkPorts;       // racks: which ports to patch, in order
		FString Label;
	};

	// Tray centreline of one row: Origin at tray height, Along the row, Out horizontal away from the racks.
	struct FTrayRow { FVector OriginM, Along, Out; };

	struct FInput
	{
		TArray<FDevice> Devices;
		TArray<FTrayRow> Rows;
		double PlugTailM = .044;
		double LanePitchM = .012;        // centre spacing of cables inside the tray
		int32 LanesPerLayer = 10;
		double EndMarginM = .6;          // tray run past the last cabinet of a row
		int32 SpineLinksPerRowPair = 4;
	};

	struct FCable
	{
		int32 FromDevice = INDEX_NONE, FromPort = INDEX_NONE, ToDevice = INDEX_NONE, ToPort = INDEX_NONE;
		bool bSpineToSpine = false;
		TArray<FVector> PointsM;         // port, tail, ..., tail, port
	};

	struct FUnconnected { int32 Device = INDEX_NONE; FString Reason; };

	struct FPlan
	{
		TArray<FCable> Cables;
		TArray<FUnconnected> Unconnected;
		TArray<TPair<FVector, FVector>> TrayRunsM;  // straight tray segments (centreline)
		int32 RejectedClaims = 0;
		int32 NumUplinks() const;
		FString Summary() const;
	};

	// Port bookkeeping. A port can be claimed once; a second claim fails and is counted.
	class VRG_TERMPROJ_API FPortLedger
	{
	public:
		bool Claim(int32 Device, int32 Port);
		bool IsClaimed(int32 Device, int32 Port) const { return Claimed.Contains(Key(Device, Port)); }
		int32 Num() const { return Claimed.Num(); }
		int32 GetRejected() const { return Rejected; }
	private:
		static uint64 Key(int32 D, int32 P) { return (uint64(uint32(D)) << 32) | uint32(P); }
		TSet<uint64> Claimed;
		int32 Rejected = 0;
	};

	VRG_TERMPROJ_API FPlan PlanPatches(const FInput& In);
}
