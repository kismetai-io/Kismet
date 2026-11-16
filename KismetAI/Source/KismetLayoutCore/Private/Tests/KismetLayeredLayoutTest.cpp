// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Ported from ibrews/blueprint-auto-layout (MIT License).
// Copyright (c) 2026 Alex Coulombe.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "KismetLayeredLayout.h"

// Use EngineFilter + ApplicationContextMask so tests run in headless (no-editor) mode.
// EAutomationTestFlags_ApplicationContextMask is a global constexpr (not an enum member).
#define KISMET_LAYOUT_TEST_FLAGS \
	(EAutomationTestFlags::EngineFilter | EAutomationTestFlags_ApplicationContextMask)

// ---------------------------------------------------------------------------
// Test 1: Straight 3-chain, uniform sizes + uniform pin offsets
//   A(rank0) -> B(rank1) -> C(rank2), all 200x80, port at Y=40 (center).
//   After Solve(): Source.Y + PortFromY == Dest.Y + PortToY for every edge.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_StraightChainUniform,
	"KismetLayoutCore.Solver.StraightChainUniform",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_StraightChainUniform::RunTest(const FString& Parameters)
{
	FLayeredLayout L;
	int32 A = L.AddVertex(200.f, 80.f);
	int32 B = L.AddVertex(200.f, 80.f);
	int32 C = L.AddVertex(200.f, 80.f);

	L.SetSeedRank(A, 0); L.SetSeedOrder(A, 0);
	L.SetSeedRank(B, 1); L.SetSeedOrder(B, 0);
	L.SetSeedRank(C, 2); L.SetSeedOrder(C, 0);

	L.AddEdge(A, B, 40.f, 40.f, /*bExec=*/true);
	L.AddEdge(B, C, 40.f, 40.f, /*bExec=*/true);

	L.Solve();

	const float Eps = 0.5f;

	FVector2D PA = L.GetPosition(A);
	FVector2D PB = L.GetPosition(B);
	FVector2D PC = L.GetPosition(C);

	// Edge A->B: Source.Y + PortFromY == Dest.Y + PortToY
	TestTrue(TEXT("Chain AB pin Y"), FMath::Abs((PA.Y + 40.f) - (PB.Y + 40.f)) < Eps);
	// Edge B->C
	TestTrue(TEXT("Chain BC pin Y"), FMath::Abs((PB.Y + 40.f) - (PC.Y + 40.f)) < Eps);

	// Positions must be finite
	TestTrue(TEXT("A.X finite"), FMath::IsFinite(PA.X));
	TestTrue(TEXT("B.X finite"), FMath::IsFinite(PB.X));
	TestTrue(TEXT("C.X finite"), FMath::IsFinite(PC.X));

	// X increases with rank
	TestTrue(TEXT("A.X < B.X"), PA.X < PB.X);
	TestTrue(TEXT("B.X < C.X"), PB.X < PC.X);

	return true;
}

// ---------------------------------------------------------------------------
// Test 2: Straight chain, mixed heights, exec pins at differing offsets
//   A(rank0,200x60,portFrom=10) -> B(rank1,200x120,portTo=80)
//   Same check.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_StraightChainMixedHeights,
	"KismetLayoutCore.Solver.StraightChainMixedHeights",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_StraightChainMixedHeights::RunTest(const FString& Parameters)
{
	FLayeredLayout L;
	int32 A = L.AddVertex(200.f, 60.f);
	int32 B = L.AddVertex(200.f, 120.f);

	L.SetSeedRank(A, 0); L.SetSeedOrder(A, 0);
	L.SetSeedRank(B, 1); L.SetSeedOrder(B, 0);

	L.AddEdge(A, B, 10.f, 80.f, /*bExec=*/true);

	L.Solve();

	FVector2D PA = L.GetPosition(A);
	FVector2D PB = L.GetPosition(B);

	const float Eps = 0.5f;
	const float SourcePinY = PA.Y + 10.f;
	const float DestPinY   = PB.Y + 80.f;
	TestTrue(TEXT("Mixed height pin Y"), FMath::Abs(SourcePinY - DestPinY) < Eps);

	return true;
}

// ---------------------------------------------------------------------------
// Test 3: Edge spanning 3 ranks — dummy chain exists + no real node overlaps
//   A(rank0) -> C(rank2); B(rank1) is a separate real node at rank1.
//   GetEdgeChain(0) must be non-empty (dummy exists at rank1 lane).
//   The dummy's Y must not overlap B's bounding box.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_DummyLane,
	"KismetLayoutCore.Solver.DummyLane",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_DummyLane::RunTest(const FString& Parameters)
{
	FLayeredLayout L;
	int32 A = L.AddVertex(200.f, 80.f);
	int32 B = L.AddVertex(200.f, 80.f);  // real node at rank 1, order 0
	int32 C = L.AddVertex(200.f, 80.f);

	L.SetSeedRank(A, 0); L.SetSeedOrder(A, 0);
	L.SetSeedRank(B, 1); L.SetSeedOrder(B, 0);
	L.SetSeedRank(C, 2); L.SetSeedOrder(C, 0);

	// Edge from A to C spans rank 0->2 (span=2), so a dummy is inserted at rank 1.
	// Edge index 0.
	L.AddEdge(A, C, 40.f, 40.f, /*bExec=*/false);
	// Also connect B -> C so B has a neighbour (makes ordering well-defined).
	L.AddEdge(B, C, 40.f, 40.f, /*bExec=*/false);

	L.Solve();

	const TArray<int32>& Chain = L.GetEdgeChain(0);
	TestTrue(TEXT("Dummy chain non-empty for span-2 edge"), Chain.Num() > 0);

	// The dummy at rank 1 shares the rank with real node B, so its lane must not
	// overlap B's bounding box in Y. B's box is [PB.Y, PB.Y + 80]; the dummy's box
	// is [PDummy.Y, PDummy.Y + DummyHeight(1)]. The two Y intervals must be disjoint.
	FVector2D PB = L.GetPosition(B);
	FVector2D PDummy = L.GetPosition(Chain[0]);

	TestTrue(TEXT("Dummy Y finite"), FMath::IsFinite(PDummy.Y));
	TestTrue(TEXT("B Y finite"), FMath::IsFinite(PB.Y));

	const float BTop = PB.Y, BBot = PB.Y + 80.f;
	const float DTop = PDummy.Y, DBot = PDummy.Y + 1.f;
	const bool bDisjoint = (DBot <= BTop) || (DTop >= BBot);
	TestTrue(TEXT("Dummy lane does not overlap B's Y box"), bDisjoint);

	return true;
}

// ---------------------------------------------------------------------------
// Test 4: Determinism — same inputs built twice yield identical positions.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_Determinism,
	"KismetLayoutCore.Solver.Determinism",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_Determinism::RunTest(const FString& Parameters)
{
	auto BuildAndSolve = [](TArray<FVector2D>& OutPositions)
	{
		FLayeredLayout L;
		int32 A = L.AddVertex(200.f, 80.f);
		int32 B = L.AddVertex(150.f, 60.f);
		int32 C = L.AddVertex(200.f, 100.f);
		int32 D = L.AddVertex(180.f, 80.f);

		L.SetSeedRank(A, 0); L.SetSeedOrder(A, 0);
		L.SetSeedRank(B, 1); L.SetSeedOrder(B, 0);
		L.SetSeedRank(C, 1); L.SetSeedOrder(C, 1);
		L.SetSeedRank(D, 2); L.SetSeedOrder(D, 0);

		L.AddEdge(A, B, 40.f, 30.f, /*bExec=*/true);
		L.AddEdge(A, C, 40.f, 50.f, /*bExec=*/false);
		L.AddEdge(B, D, 30.f, 40.f, /*bExec=*/true);
		L.AddEdge(C, D, 50.f, 40.f, /*bExec=*/false);

		L.Solve();

		OutPositions.SetNum(4);
		OutPositions[0] = L.GetPosition(A);
		OutPositions[1] = L.GetPosition(B);
		OutPositions[2] = L.GetPosition(C);
		OutPositions[3] = L.GetPosition(D);
	};

	TArray<FVector2D> Pos1, Pos2;
	BuildAndSolve(Pos1);
	BuildAndSolve(Pos2);

	for (int32 I = 0; I < Pos1.Num(); ++I)
	{
		TestTrue(FString::Printf(TEXT("X[%d] identical"), I),
			FMath::Abs(Pos1[I].X - Pos2[I].X) < 0.01f);
		TestTrue(FString::Printf(TEXT("Y[%d] identical"), I),
			FMath::Abs(Pos1[I].Y - Pos2[I].Y) < 0.01f);
	}

	return true;
}

// ---------------------------------------------------------------------------
// Test 5a: 2-cycle graph — Solve() returns finite positions, no crash/NaN.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_TwoCycle,
	"KismetLayoutCore.Solver.TwoCycle",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_TwoCycle::RunTest(const FString& Parameters)
{
	FLayeredLayout L;
	int32 A = L.AddVertex(200.f, 80.f);
	int32 B = L.AddVertex(200.f, 80.f);

	// Two edges forming a cycle; no seed ranks (let it use longest-path).
	L.AddEdge(A, B, 40.f, 40.f, false);
	L.AddEdge(B, A, 40.f, 40.f, false);

	// Should not crash or produce NaN.
	L.Solve();

	FVector2D PA = L.GetPosition(A);
	FVector2D PB = L.GetPosition(B);

	TestTrue(TEXT("A.X finite (cycle)"), FMath::IsFinite(PA.X));
	TestTrue(TEXT("A.Y finite (cycle)"), FMath::IsFinite(PA.Y));
	TestTrue(TEXT("B.X finite (cycle)"), FMath::IsFinite(PB.X));
	TestTrue(TEXT("B.Y finite (cycle)"), FMath::IsFinite(PB.Y));

	return true;
}

// ---------------------------------------------------------------------------
// Test 5b: Zero-vertex graph — Solve() does not crash.
// ---------------------------------------------------------------------------
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_ZeroVertex,
	"KismetLayoutCore.Solver.ZeroVertex",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_ZeroVertex::RunTest(const FString& Parameters)
{
	FLayeredLayout L;
	L.Solve();  // Must not crash.
	return true;
}

// ---------------------------------------------------------------------------
// 4-direction balanced Brandes-Köpf (Config.bFourDirBalance) tests.
// ---------------------------------------------------------------------------

static FLayeredConfig FourDirConfig()
{
	FLayeredConfig Cfg;
	Cfg.bFourDirBalance = true;
	return Cfg;
}

// Test 6: With the balanced pass on, a straight uniform chain stays straight.
// Median-of-four shifts exact values slightly vs the single pass, so the pin-Y
// tolerance is looser than the single-pass test's 0.5.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_FourDirStraightChain,
	"KismetLayoutCore.Solver.FourDirStraightChain",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_FourDirStraightChain::RunTest(const FString& Parameters)
{
	FLayeredLayout L(FourDirConfig());
	int32 A = L.AddVertex(200.f, 80.f);
	int32 B = L.AddVertex(200.f, 80.f);
	int32 C = L.AddVertex(200.f, 80.f);

	L.SetSeedRank(A, 0); L.SetSeedOrder(A, 0);
	L.SetSeedRank(B, 1); L.SetSeedOrder(B, 0);
	L.SetSeedRank(C, 2); L.SetSeedOrder(C, 0);

	L.AddEdge(A, B, 40.f, 40.f, /*bExec=*/true);
	L.AddEdge(B, C, 40.f, 40.f, /*bExec=*/true);

	L.Solve();

	FVector2D PA = L.GetPosition(A);
	FVector2D PB = L.GetPosition(B);
	FVector2D PC = L.GetPosition(C);

	const float Eps = 2.0f;
	TestTrue(TEXT("4dir chain AB pin Y"), FMath::Abs((PA.Y + 40.f) - (PB.Y + 40.f)) < Eps);
	TestTrue(TEXT("4dir chain BC pin Y"), FMath::Abs((PB.Y + 40.f) - (PC.Y + 40.f)) < Eps);
	TestTrue(TEXT("4dir A.Y finite"), FMath::IsFinite(PA.Y));
	TestTrue(TEXT("4dir B.Y finite"), FMath::IsFinite(PB.Y));
	TestTrue(TEXT("4dir C.Y finite"), FMath::IsFinite(PC.Y));
	return true;
}

// Test 7: The balanced pass is deterministic with the flag on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_FourDirDeterminism,
	"KismetLayoutCore.Solver.FourDirDeterminism",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_FourDirDeterminism::RunTest(const FString& Parameters)
{
	auto BuildAndSolve = [](TArray<FVector2D>& Out)
	{
		FLayeredLayout L(FourDirConfig());
		int32 A = L.AddVertex(200.f, 80.f);
		int32 B = L.AddVertex(150.f, 60.f);
		int32 C = L.AddVertex(200.f, 100.f);
		int32 D = L.AddVertex(180.f, 80.f);

		L.SetSeedRank(A, 0); L.SetSeedOrder(A, 0);
		L.SetSeedRank(B, 1); L.SetSeedOrder(B, 0);
		L.SetSeedRank(C, 1); L.SetSeedOrder(C, 1);
		L.SetSeedRank(D, 2); L.SetSeedOrder(D, 0);

		L.AddEdge(A, B, 40.f, 30.f, /*bExec=*/true);
		L.AddEdge(A, C, 40.f, 50.f, /*bExec=*/false);
		L.AddEdge(B, D, 30.f, 40.f, /*bExec=*/true);
		L.AddEdge(C, D, 50.f, 40.f, /*bExec=*/false);

		L.Solve();
		Out.SetNum(4);
		Out[0] = L.GetPosition(A); Out[1] = L.GetPosition(B);
		Out[2] = L.GetPosition(C); Out[3] = L.GetPosition(D);
	};

	TArray<FVector2D> P1, P2;
	BuildAndSolve(P1);
	BuildAndSolve(P2);
	for (int32 I = 0; I < P1.Num(); ++I)
	{
		TestTrue(FString::Printf(TEXT("4dir X[%d] identical"), I), FMath::Abs(P1[I].X - P2[I].X) < 0.01f);
		TestTrue(FString::Printf(TEXT("4dir Y[%d] identical"), I), FMath::Abs(P1[I].Y - P2[I].Y) < 0.01f);
	}
	return true;
}

// Test 8: Regression guard for the point of the change. A single parent A fans
// out to two children B and C in the next rank. The balanced pass should center
// A vertically between its children at least as well as the single upper-left
// pass (which, having no upper neighbour, leaves A top-biased). Self-validating:
// compares 4-dir against single-pass on the same graph, so it can't go stale.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_FourDirForkCentering,
	"KismetLayoutCore.Solver.FourDirForkCentering",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_FourDirForkCentering::RunTest(const FString& Parameters)
{
	auto BuildFork = [](const FLayeredConfig& Cfg, float& OutA, float& OutLo, float& OutHi)
	{
		FLayeredLayout L(Cfg);
		int32 A = L.AddVertex(200.f, 80.f);
		int32 B = L.AddVertex(200.f, 80.f);
		int32 C = L.AddVertex(200.f, 80.f);

		L.SetSeedRank(A, 0); L.SetSeedOrder(A, 0);
		L.SetSeedRank(B, 1); L.SetSeedOrder(B, 0);
		L.SetSeedRank(C, 1); L.SetSeedOrder(C, 1);

		L.AddEdge(A, B, 40.f, 40.f, /*bExec=*/true);
		L.AddEdge(A, C, 40.f, 40.f, /*bExec=*/true);

		L.Solve();
		OutA  = L.GetPosition(A).Y + 40.f;
		const float Bc = L.GetPosition(B).Y + 40.f;
		const float Cc = L.GetPosition(C).Y + 40.f;
		OutLo = FMath::Min(Bc, Cc);
		OutHi = FMath::Max(Bc, Cc);
	};

	float SingleA, SingleLo, SingleHi;
	BuildFork(FLayeredConfig(), SingleA, SingleLo, SingleHi);

	float FourA, FourLo, FourHi;
	BuildFork(FourDirConfig(), FourA, FourLo, FourHi);

	const float SingleOff = FMath::Abs(SingleA - (SingleLo + SingleHi) * 0.5f);
	const float FourOff   = FMath::Abs(FourA   - (FourLo   + FourHi)   * 0.5f);

	TestTrue(TEXT("4dir A finite"), FMath::IsFinite(FourA));
	// The parent must sit within its children's vertical span (it coincides with
	// one child under the single pass, the midpoint under the balanced pass).
	TestTrue(TEXT("4dir A within children span"),
		FourA >= FourLo - 0.5f && FourA <= FourHi + 0.5f);
	// And it must be centered no worse than — and meaningfully better than — the
	// single upper-left pass, which leaves the parent top-biased onto one child.
	TestTrue(TEXT("4dir centers fork parent no worse"), FourOff <= SingleOff + 0.5f);
	TestTrue(TEXT("4dir centers fork parent better"), FourOff <= SingleOff * 0.5f + 0.5f);
	return true;
}

// Test 9: Robustness — cycle + zero-vertex with the balanced pass on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetLayout_FourDirRobust,
	"KismetLayoutCore.Solver.FourDirRobust",
	KISMET_LAYOUT_TEST_FLAGS)

bool FKismetLayout_FourDirRobust::RunTest(const FString& Parameters)
{
	{
		FLayeredLayout L(FourDirConfig());
		int32 A = L.AddVertex(200.f, 80.f);
		int32 B = L.AddVertex(200.f, 80.f);
		L.AddEdge(A, B, 40.f, 40.f, false);
		L.AddEdge(B, A, 40.f, 40.f, false);
		L.Solve();
		TestTrue(TEXT("4dir cycle A.Y finite"), FMath::IsFinite(L.GetPosition(A).Y));
		TestTrue(TEXT("4dir cycle B.Y finite"), FMath::IsFinite(L.GetPosition(B).Y));
	}
	{
		FLayeredLayout L(FourDirConfig());
		L.Solve(); // must not crash
	}
	return true;
}
