// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Ported from ibrews/blueprint-auto-layout (MIT License).
// Copyright (c) 2026 Alex Coulombe.
//
// Layered (Sugiyama) coordinate-assignment core:
//   1. AssignRanks   — normalize seed ranks (or DFS longest-path if no seeds).
//   2. InsertDummies — split edges spanning >1 rank into unit-width dummy chains.
//   3. AssignOrders  — apply seed orders to real vertices; interpolate for dummies.
//   4. AssignX       — X = Rank * RankSpacing.
//   5. AssignYBrandesKopf — port-aware BK Y placement.

#include "KismetLayeredLayout.h"
#include "Algo/Reverse.h"

// ============================================================================
// Public API
// ============================================================================

int32 FLayeredLayout::AddVertex(float Width, float Height)
{
	FCoreVertex V;
	V.W = Width  > 0.f ? Width  : 1.f;
	V.H = Height > 0.f ? Height : 1.f;
	int32 Id = Vertices.Num();
	Vertices.Add(V);
	return Id;
}

void FLayeredLayout::AddEdge(int32 From, int32 To, float PortFromY, float PortToY, bool bExec)
{
	if (From < 0 || To < 0 || From >= Vertices.Num() || To >= Vertices.Num() || From == To)
	{
		return;
	}
	FCoreEdge E;
	E.From     = From;
	E.To       = To;
	E.PortFromY = PortFromY;
	E.PortToY   = PortToY;
	E.bExec    = bExec;
	Edges.Add(E);
}

void FLayeredLayout::SetSeedRank(int32 Vertex, int32 Rank)
{
	if (Vertex < 0 || Vertex >= Vertices.Num()) return;
	Vertices[Vertex].Rank      = Rank;
	Vertices[Vertex].bSeedRank = true;
	bAnySeedRank               = true;
}

void FLayeredLayout::SetSeedOrder(int32 Vertex, int32 Order)
{
	if (Vertex < 0 || Vertex >= Vertices.Num()) return;
	Vertices[Vertex].Order      = Order;
	Vertices[Vertex].bSeedOrder = true;
}

void FLayeredLayout::Solve()
{
	if (Vertices.Num() == 0) return;

	OriginalEdgeCount = Edges.Num();
	EdgeChains.SetNum(OriginalEdgeCount);
	for (TArray<int32>& Chain : EdgeChains) Chain.Reset();

	AssignRanks();
	InsertDummies();
	AssignOrders();

	TArray<TArray<int32>> RankOrders = BuildRankOrders();

	AssignX();
	AssignYBrandesKopf(RankOrders);

	// Normalize Y so min is 0.
	float MinY = TNumericLimits<float>::Max();
	for (const FCoreVertex& V : Vertices) MinY = FMath::Min(MinY, V.Y);
	if (MinY != 0.f && FMath::IsFinite(MinY))
	{
		for (FCoreVertex& V : Vertices) V.Y -= MinY;
	}
}

FVector2D FLayeredLayout::GetPosition(int32 Vertex) const
{
	if (Vertex < 0 || Vertex >= Vertices.Num())
	{
		return FVector2D::ZeroVector;
	}
	return FVector2D(Vertices[Vertex].X, Vertices[Vertex].Y);
}

const TArray<int32>& FLayeredLayout::GetEdgeChain(int32 EdgeIndex) const
{
	static const TArray<int32> Empty;
	if (EdgeIndex < 0 || EdgeIndex >= EdgeChains.Num()) return Empty;
	return EdgeChains[EdgeIndex];
}

// ============================================================================
// Stage 1: Ranking
// ============================================================================

void FLayeredLayout::AssignRanks()
{
	const int32 N = Vertices.Num();

	if (bAnySeedRank)
	{
		// Normalize seed ranks so minimum is 0.
		// Real vertices that have no seed get rank 0 (safe default).
		int32 MinR = TNumericLimits<int32>::Max();
		int32 MaxR = TNumericLimits<int32>::Min();
		for (const FCoreVertex& V : Vertices)
		{
			if (V.bSeedRank)
			{
				MinR = FMath::Min(MinR, V.Rank);
				MaxR = FMath::Max(MaxR, V.Rank);
			}
		}
		if (MinR > MaxR) { MinR = 0; MaxR = 0; }

		for (FCoreVertex& V : Vertices)
		{
			if (V.bSeedRank) V.Rank -= MinR;
			else             V.Rank  = 0;
		}
		NumRanks = (MaxR - MinR) + 1;
		return;
	}

	// --- No seeds: DFS cycle-break + longest-path ranking (path) ---

	// Build out-adjacency by edge index.
	TArray<TArray<int32>> Out; // vertex -> list of edge indices
	Out.SetNum(N);
	for (int32 E = 0; E < Edges.Num(); ++E)
	{
		Out[Edges[E].From].Add(E);
	}

	// DFS cycle-break: reverse back-edges so the graph becomes a DAG.
	TArray<int32> EdgeDir; // +1 = From->To, -1 = reversed
	EdgeDir.Init(1, Edges.Num());

	TArray<uint8> State; // 0=unvisited, 1=on-stack, 2=done
	State.Init(0, N);

	struct FFrame { int32 V; int32 I; };
	for (int32 S = 0; S < N; ++S)
	{
		if (State[S] != 0) continue;
		TArray<FFrame> Stack;
		Stack.Push({ S, 0 });
		State[S] = 1;
		while (Stack.Num() > 0)
		{
			FFrame& F = Stack.Last();
			if (F.I < Out[F.V].Num())
			{
				const int32 EIdx = Out[F.V][F.I++];
				const int32 W    = Edges[EIdx].To;
				if (State[W] == 1)
				{
					EdgeDir[EIdx] = -1; // back-edge: reverse for ranking
				}
				else if (State[W] == 0)
				{
					State[W] = 1;
					Stack.Push({ W, 0 });
				}
			}
			else
			{
				State[F.V] = 2;
				Stack.Pop();
			}
		}
	}

	// Build oriented adjacency + in-degrees.
	TArray<TArray<int32>> OrientedSucc;
	OrientedSucc.SetNum(N);
	TArray<int32> InDeg;
	InDeg.Init(0, N);
	for (int32 E = 0; E < Edges.Num(); ++E)
	{
		int32 A = Edges[E].From, B = Edges[E].To;
		if (EdgeDir[E] < 0) Swap(A, B);
		OrientedSucc[A].Add(B);
		InDeg[B]++;
	}

	// Kahn topological sort.
	TArray<int32> Topo;
	Topo.Reserve(N);
	TArray<int32> Queue;
	for (int32 V = 0; V < N; ++V) if (InDeg[V] == 0) Queue.Add(V);
	TArray<int32> InDegLeft = InDeg;
	int32 QI = 0;
	while (QI < Queue.Num())
	{
		const int32 V = Queue[QI++];
		Topo.Add(V);
		for (int32 W : OrientedSucc[V])
		{
			if (--InDegLeft[W] == 0) Queue.Add(W);
		}
	}
	// Append any unreached vertices (safety; shouldn't happen after cycle-break).
	if (Topo.Num() < N)
	{
		TArray<uint8> InTopo;
		InTopo.Init(0, N);
		for (int32 V : Topo) InTopo[V] = 1;
		for (int32 V = 0; V < N; ++V) if (!InTopo[V]) Topo.Add(V);
	}

	// Longest-path rank.
	for (FCoreVertex& V : Vertices) V.Rank = 0;
	for (int32 V : Topo)
	{
		for (int32 W : OrientedSucc[V])
		{
			if (Vertices[W].Rank < Vertices[V].Rank + 1)
				Vertices[W].Rank = Vertices[V].Rank + 1;
		}
	}

	// Normalize so min rank == 0 and compute NumRanks.
	int32 MinRank = TNumericLimits<int32>::Max();
	int32 MaxRank = TNumericLimits<int32>::Min();
	for (const FCoreVertex& V : Vertices)
	{
		MinRank = FMath::Min(MinRank, V.Rank);
		MaxRank = FMath::Max(MaxRank, V.Rank);
	}
	if (MinRank > MaxRank) { MinRank = 0; MaxRank = 0; }
	for (FCoreVertex& V : Vertices) V.Rank -= MinRank;
	NumRanks = (MaxRank - MinRank) + 1;
}

// ============================================================================
// Stage 2: Dummy insertion
// ============================================================================

void FLayeredLayout::InsertDummies()
{
	TArray<FCoreEdge> NewEdges;
	NewEdges.Reserve(Edges.Num());

	for (int32 EIdx = 0; EIdx < Edges.Num(); ++EIdx)
	{
		const FCoreEdge E = Edges[EIdx];
		const int32 R0 = Vertices[E.From].Rank;
		const int32 R1 = Vertices[E.To].Rank;
		const int32 Span = FMath::Abs(R1 - R0);

		if (Span <= 1)
		{
			NewEdges.Add(E);
			continue;
		}

		// Build dummy chain across intermediate ranks.
		const int32 Step = (R1 > R0) ? +1 : -1;
		int32 PrevVertex = E.From;
		float PrevPort   = PortFromYOf(E);

		TArray<int32>& Chain = (EIdx < EdgeChains.Num()) ? EdgeChains[EIdx] : EdgeChains.AddDefaulted_GetRef();

		for (int32 R = R0 + Step; R != R1; R += Step)
		{
			const int32 D = AddVertex(Config.DummyWidth, Config.DummyHeight);
			Vertices[D].bDummy = true;
			Vertices[D].Rank   = R;
			Chain.Add(D);

			FCoreEdge Seg;
			Seg.From     = PrevVertex;
			Seg.To       = D;
			Seg.PortFromY = PrevPort;
			Seg.PortToY  = -1.f;   // dummy center
			Seg.bExec    = E.bExec;
			NewEdges.Add(Seg);

			PrevVertex = D;
			PrevPort   = -1.f; // subsequent segments leave from dummy center
		}

		// Final segment into the real destination.
		FCoreEdge Last;
		Last.From     = PrevVertex;
		Last.To       = E.To;
		Last.PortFromY = PrevPort;
		Last.PortToY  = PortToYOf(E);
		Last.bExec    = E.bExec;
		NewEdges.Add(Last);
	}

	Edges = MoveTemp(NewEdges);
}

// ============================================================================
// Stage 3: Assign within-rank order
// ============================================================================

void FLayeredLayout::AssignOrders()
{
	const int32 N = Vertices.Num();

	// Build rank buckets for real (non-dummy) vertices.
	// For each rank, collect [real vertices with seed order, real vertices without seed order].
	// Assign contiguous Order values preserving seed orders for real nodes.

	// Separate vertices by rank into two groups: seeded reals and un-seeded reals.
	// Dummies get assigned after based on their endpoints' orders.

	// Step A: assign orders to real vertices.
	// Sort real vertices per rank by (bSeedOrder desc, SeedOrder asc, VertexId asc)
	// to give a stable order. Un-seeded reals slot in last.
	TMap<int32, TArray<int32>> RankToReals;
	for (int32 V = 0; V < N; ++V)
	{
		if (!Vertices[V].bDummy)
		{
			RankToReals.FindOrAdd(Vertices[V].Rank).Add(V);
		}
	}

	// Sort map keys for determinism.
	TArray<int32> RankKeys;
	RankToReals.GetKeys(RankKeys);
	RankKeys.Sort();

	for (int32 Rank : RankKeys)
	{
		TArray<int32>& Members = RankToReals[Rank];
		// Sort: seeded first (by seed order), then un-seeded by vertex id.
		Members.Sort([&](int32 A, int32 B)
		{
			const bool bSA = Vertices[A].bSeedOrder;
			const bool bSB = Vertices[B].bSeedOrder;
			if (bSA != bSB) return bSA > bSB; // seeded first
			if (bSA && bSB) return Vertices[A].Order < Vertices[B].Order;
			return A < B; // stable by vertex id
		});

		// Assign contiguous order, keeping seed values if provided.
		// We just stamp 0..N-1 in the sorted order (seed orders may not be contiguous).
		for (int32 I = 0; I < Members.Num(); ++I)
		{
			Vertices[Members[I]].Order = I;
		}
	}

	// Step B: assign orders to dummies using linear interpolation between their
	// predecessor's and successor's order in the dummy chain.
	// For determinism, process dummies by vertex id order within each rank.
	// Strategy: for each dummy, find its predecessor and successor in the segment
	// edges (Edges now contains the split segments). Dummy order = interpolated.
	//
	// Build: for each vertex, which edges are incident as From/To.
	TArray<TArray<int32>> VertexOutEdges, VertexInEdges;
	VertexOutEdges.SetNum(N);
	VertexInEdges.SetNum(N);
	for (int32 E = 0; E < Edges.Num(); ++E)
	{
		VertexOutEdges[Edges[E].From].Add(E);
		VertexInEdges[Edges[E].To].Add(E);
	}

	// Collect dummies sorted by (rank, vertexId) for determinism.
	TArray<int32> Dummies;
	for (int32 V = 0; V < N; ++V)
	{
		if (Vertices[V].bDummy) Dummies.Add(V);
	}
	Dummies.Sort([&](int32 A, int32 B)
	{
		if (Vertices[A].Rank != Vertices[B].Rank) return Vertices[A].Rank < Vertices[B].Rank;
		return A < B;
	});

	// For each dummy, find its order as the average of the orders of its in-neighbor
	// (From vertex, may itself be a dummy) and out-neighbor (To vertex), as floats.
	// Then within each rank, sort dummies by that float, assign contiguous integers
	// interleaved with real nodes.
	// Simpler approach that satisfies the spec: assign dummy order by walking the
	// chain in edge order and inserting between predecessor order and successor order.

	// Build next/prev dummy pointers per edge chain.
	// We'll assign order by: for each dummy D at rank R, find the predecessor vertex P
	// (real or dummy) with the smallest rank < R that has an edge leading to D (directly
	// or transitively), and the successor S at rank > R. Then dummy order = interpolated
	// between P.Order and S.Order.
	// Since dummies are processed rank by rank, predecessors' orders are already set.

	// Map: dummy vertex -> its predecessor (the From of the incoming segment edge).
	// and -> its successor (the To of the outgoing segment edge).
	TArray<int32> DummyPred, DummySucc;
	DummyPred.Init(-1, N);
	DummySucc.Init(-1, N);
	for (int32 V = 0; V < N; ++V)
	{
		if (!Vertices[V].bDummy) continue;
		// Predecessor: the From of the first incoming edge (only one for a chain member).
		if (VertexInEdges[V].Num() > 0) DummyPred[V] = Edges[VertexInEdges[V][0]].From;
		// Successor: the To of the first outgoing edge.
		if (VertexOutEdges[V].Num() > 0) DummySucc[V] = Edges[VertexOutEdges[V][0]].To;
	}

	// Assign dummy order per rank.
	TMap<int32, TArray<int32>> RankToDummies;
	for (int32 V : Dummies) RankToDummies.FindOrAdd(Vertices[V].Rank).Add(V);

	TArray<int32> DummyRankKeys;
	RankToDummies.GetKeys(DummyRankKeys);
	DummyRankKeys.Sort();

	for (int32 Rank : DummyRankKeys)
	{
		const TArray<int32>& DumInRank = RankToDummies[Rank];

		// Compute a float sort key per dummy.
		struct FDummySort { int32 V; float Key; };
		TArray<FDummySort> Sortable;
		Sortable.Reserve(DumInRank.Num());

		for (int32 D : DumInRank)
		{
			const int32 P = DummyPred[D];
			const int32 S = DummySucc[D];
			float POrder = (P >= 0) ? (float)Vertices[P].Order : 0.f;
			float SOrder = (S >= 0) ? (float)Vertices[S].Order : POrder;
			float Key    = (POrder + SOrder) * 0.5f;
			Sortable.Add({ D, Key });
		}

		// Sort by Key, tie-break by edge index then vertex id.
		// (Edge index isn't easily available here; tie-break by vertex id is sufficient
		// for determinism per the spec.)
		Sortable.Sort([](const FDummySort& A, const FDummySort& B)
		{
			if (A.Key != B.Key) return A.Key < B.Key;
			return A.V < B.V;
		});

		// Now merge dummies with the real nodes in this rank and re-assign contiguous orders.
		// Real nodes keep their relative order; dummies are inserted by their Key.
		if (!RankToReals.Contains(Rank))
		{
			// Only dummies in this rank (unusual but legal).
			for (int32 I = 0; I < Sortable.Num(); ++I)
				Vertices[Sortable[I].V].Order = I;
		}
		else
		{
			const TArray<int32>& Reals = RankToReals[Rank];

			// Produce merged list: interleave reals (sorted by Order) with dummies (by Key).
			TArray<int32> Merged;
			Merged.Reserve(Reals.Num() + Sortable.Num());

			// Reals are already sorted (0..K-1) from Step A.
			int32 RI = 0, DI = 0;
			while (RI < Reals.Num() && DI < Sortable.Num())
			{
				float RealKey = (float)Vertices[Reals[RI]].Order; // 0-based from Step A
				if (RealKey <= Sortable[DI].Key)
					Merged.Add(Reals[RI++]);
				else
					Merged.Add(Sortable[DI++].V);
			}
			while (RI < Reals.Num()) Merged.Add(Reals[RI++]);
			while (DI < Sortable.Num()) Merged.Add(Sortable[DI++].V);

			// Stamp contiguous orders.
			for (int32 I = 0; I < Merged.Num(); ++I)
				Vertices[Merged[I]].Order = I;
		}
	}
}

// ============================================================================
// Stage 4: Assign X
// ============================================================================

void FLayeredLayout::AssignX()
{
	// Simple: X = Rank * RankSpacing.
	for (FCoreVertex& V : Vertices)
	{
		V.X = (float)V.Rank * Config.RankSpacing;
	}
}

// ============================================================================
// Stage 5: Brandes-Köpf Y assignment
//
// Default path is a single upper-left pass (enough for pin-Y straightening on
// blueprint-shaped graphs). When Config.bFourDirBalance is set, the canonical
// 4-direction balanced variant runs instead (AssignYBrandesKopf4Dir): four
// directional alignments (up/down × left/right) combined by min-width alignment
// + median-of-two-middle averaging, for straighter, better-centered wires on
// mixed-height / forked graphs.
// ============================================================================

void FLayeredLayout::ComputeBKAdjacency(
	const TArray<TArray<int32>>& RankOrders,
	TArray<int32>& OutPos,
	TArray<TArray<FNbr>>& OutUp,
	TArray<TArray<FNbr>>& OutDown,
	TSet<int64>& OutConflicts) const
{
	const int32 N = Vertices.Num();

	// Position of each vertex within its rank (by Order).
	OutPos.Init(0, N);
	for (const TArray<int32>& Rank : RankOrders)
		for (int32 P = 0; P < Rank.Num(); ++P) OutPos[Rank[P]] = P;

	// Center-relative port offsets.
	auto FromCenterFrom = [&](const FCoreEdge& E) -> float
	{
		return PortFromYOf(E) - Vertices[E.From].H * 0.5f;
	};
	auto FromCenterTo = [&](const FCoreEdge& E) -> float
	{
		return PortToYOf(E) - Vertices[E.To].H * 0.5f;
	};

	OutUp.Empty();   OutUp.SetNum(N);   // Up: neighbor in rank-1
	OutDown.Empty(); OutDown.SetNum(N); // Down: neighbor in rank+1

	for (const FCoreEdge& E : Edges)
	{
		const int32 A  = E.From, B = E.To;
		const int32 RA = Vertices[A].Rank, RB = Vertices[B].Rank;
		if (FMath::Abs(RA - RB) != 1) continue; // BK works on proper (1-rank) segments
		const int32 Lo = (RA < RB) ? A : B;     // smaller-rank (left) endpoint
		const int32 Hi = (RA < RB) ? B : A;     // larger-rank (right) endpoint
		const float PortLo = (Lo == E.From) ? FromCenterFrom(E) : FromCenterTo(E);
		const float PortHi = (Hi == E.From) ? FromCenterFrom(E) : FromCenterTo(E);
		// Hi's upper neighbor is Lo: centerY(Hi) = centerY(Lo) + (portLo - portHi)
		OutUp[Hi].Add({ Lo, PortLo - PortHi, E.bExec });
		// Lo's lower neighbor is Hi: centerY(Lo) = centerY(Hi) + (portHi - portLo)
		OutDown[Lo].Add({ Hi, PortHi - PortLo, E.bExec });
	}

	// Type-1 conflicts: mark (upper, lower) pairs that cross an inner (dummy-dummy)
	// segment. Marked once on the original ordering; the (upper,lower) keys are by
	// vertex id, so the same set is reused by every directional pass.
	OutConflicts.Empty();
	auto Key = [&](int32 Up2, int32 Lo2) -> int64 { return (int64)Up2 * N + Lo2; };

	for (int32 RI = 0; RI + 1 < NumRanks; ++RI)
	{
		const TArray<int32>& Lupper = RankOrders[RI];
		const TArray<int32>& Llower = RankOrders[RI + 1];
		int32 K0 = 0, L = 0;
		const int32 NLow = Llower.Num();
		for (int32 L1 = 0; L1 < NLow; ++L1)
		{
			const int32 V = Llower[L1];
			int32 InnerUpPos = -1;
			if (Vertices[V].bDummy)
			{
				for (const FNbr& NB : OutUp[V])
				{
					if (Vertices[NB.Id].bDummy) { InnerUpPos = OutPos[NB.Id]; break; }
				}
			}
			const bool bLast = (L1 == NLow - 1);
			if (bLast || InnerUpPos >= 0)
			{
				const int32 K1 = bLast ? Lupper.Num() - 1 : InnerUpPos;
				while (L <= L1)
				{
					const int32 VL = Llower[L];
					for (const FNbr& NB : OutUp[VL])
					{
						const int32 KU = OutPos[NB.Id];
						if (KU < K0 || KU > K1) OutConflicts.Add(Key(NB.Id, VL));
					}
					++L;
				}
				K0 = K1;
			}
		}
	}
}

void FLayeredLayout::AssignYBrandesKopf(const TArray<TArray<int32>>& RankOrders)
{
	const int32 N = Vertices.Num();
	if (N == 0) return;

	if (Config.bFourDirBalance)
	{
		AssignYBrandesKopf4Dir(RankOrders);
		return;
	}

	TArray<int32> Pos;
	TArray<TArray<FNbr>> Up, Down;
	TSet<int64> Conflicts;
	ComputeBKAdjacency(RankOrders, Pos, Up, Down, Conflicts);

	auto Key = [&](int32 Up2, int32 Lo2) -> int64 { return (int64)Up2 * N + Lo2; };

	// One BK pass (align-to-upper). Port the upstream's runOnce(true) faithfully.
	TArray<int32> Root, Align;
	TArray<float> Inner;
	Root.SetNum(N);
	Align.SetNum(N);
	Inner.Init(0.f, N);
	for (int32 V = 0; V < N; ++V) { Root[V] = V; Align[V] = V; }

	for (int32 Step = 0; Step < NumRanks; ++Step)
	{
		// bToUpper = true -> scan from rank 0 upward.
		const int32 RI = Step;
		const TArray<int32>& L = RankOrders[RI];
		int32 PrevPos = -1; // left-biased: aligned neighbor position must strictly increase
		for (int32 K = 0; K < L.Num(); ++K)
		{
			const int32 V = L[K];
			const TArray<FNbr>& AllN = Up[V];
			if (AllN.Num() == 0) continue;

			// Exec-preferred: if any exec neighbors, restrict to those.
			TArray<FNbr> Nbrs;
			for (const FNbr& NB : AllN) if (NB.bExec) Nbrs.Add(NB);
			if (Nbrs.Num() == 0) Nbrs = AllN;

			// Sort neighbors by position (deterministic — sort by Pos then Id).
			Nbrs.Sort([&](const FNbr& A2, const FNbr& B2)
			{
				if (Pos[A2.Id] != Pos[B2.Id]) return Pos[A2.Id] < Pos[B2.Id];
				return A2.Id < B2.Id;
			});

			const int32 D  = Nbrs.Num();
			const int32 M0 = (D - 1) / 2;
			const int32 M1 = D / 2;

			for (int32 MM = M0; MM <= M1; ++MM)
			{
				if (Align[V] != V) break; // already aligned
				const FNbr& NB = Nbrs[MM];
				const int32 UpV  = NB.Id; // upper-rank vertex (bToUpper=true: neighbor is in rank-1)
				const int32 LoV  = V;     // lower-rank vertex
				if (Pos[NB.Id] > PrevPos && !Conflicts.Contains(Key(UpV, LoV)))
				{
					Align[NB.Id] = V;
					Root[V]      = Root[NB.Id];
					Align[V]     = Root[V];
					Inner[V]     = Inner[NB.Id] + NB.Off;
					PrevPos      = Pos[NB.Id];
				}
			}
		}
	}

	// Horizontal compaction (places each block root; members add inner shift).
	TArray<int32> Sink;
	TArray<float> Shift;
	TArray<float> X;
	Sink.SetNum(N);
	Shift.Init(TNumericLimits<float>::Max(), N);
	X.Init(TNumericLimits<float>::Max(), N); // use Max as sentinel for "unplaced"

	for (int32 V = 0; V < N; ++V) Sink[V] = V;

	// Horizontal compaction via the classic Brandes-Köpf place_block recursion.
	// Place() (below) is recursive: placing a block first places the block of its
	// left-rank predecessor, so recursion depth is bounded by the longest chain of
	// block dependencies (worst case O(N) for one densely-stacked rank). Blueprint
	// ranks are small in practice; this mirrors the upstream reference, which also
	// recurses. PlaceOrder seeds the roots in rank order; Place fills in on demand.
	TArray<int32> PlaceOrder;
	PlaceOrder.Reserve(N);
	{
		// Topological order of roots: process in rank order so predecessors come first.
		// Only root vertices need explicit placement.
		for (const TArray<int32>& Rank : RankOrders)
		{
			for (int32 V : Rank)
			{
				if (Root[V] == V) PlaceOrder.Add(V);
			}
		}

		// For each root, we need all its block members' predecessors to be placed first.
		// Since RankOrders already orders ranks left-to-right, iterating in that order
		// and placing on demand (with a simple "already placed?" guard) suffices.
		TArray<bool> Placed;
		Placed.Init(false, N);

		// place(vRoot): if not placed, set its X=0, then walk the block's members;
		// for each member w with a left predecessor predV in its rank, recurse to
		// place Root[predV] first, then separate the two blocks by min spacing.
		TFunction<void(int32)> Place = [&](int32 VRoot)
		{
			if (Placed[VRoot]) return;
			Placed[VRoot] = true;
			X[VRoot] = 0.f;

			// Walk the block cycle: VRoot -> Align[VRoot] -> ... -> VRoot
			int32 W = VRoot;
			do
			{
				const int32 RI = Vertices[W].Rank;
				const int32 K  = Pos[W];
				if (K > 0)
				{
					const int32 PredV = RankOrders[RI][K - 1];
					const int32 U     = Root[PredV];
					Place(U);
					if (Sink[VRoot] == VRoot) Sink[VRoot] = Sink[U];
					const float BaseSep = (Vertices[PredV].H + Vertices[W].H) * 0.5f + Config.NodeSpacingY;
					const float Sep     = BaseSep + Inner[PredV] - Inner[W];
					if (Sink[VRoot] != Sink[U])
					{
						float& ShiftRef = Shift[Sink[U]];
						ShiftRef = FMath::Min(ShiftRef, X[VRoot] - X[U] - Sep);
					}
					else
					{
						X[VRoot] = FMath::Max(X[VRoot], X[U] + Sep);
					}
				}
				W = Align[W];
			} while (W != VRoot);
		};

		// Place all roots in rank order.
		for (int32 VRoot : PlaceOrder) Place(VRoot);
	}

	// Build final center-Y per vertex.
	TArray<float> Center;
	Center.SetNum(N);
	for (int32 V = 0; V < N; ++V)
	{
		float C = X[Root[V]];
		if (FMath::IsFinite(C) && Shift[Sink[Root[V]]] < TNumericLimits<float>::Max())
			C += Shift[Sink[Root[V]]];
		if (!FMath::IsFinite(C)) C = 0.f; // safety: should never be NaN but guard anyway
		Center[V] = C + Inner[V];
	}

	// Store top-left Y (center minus half-height).
	for (int32 V = 0; V < N; ++V)
	{
		Vertices[V].Y = Center[V] - Vertices[V].H * 0.5f;
	}
}

// ============================================================================
// Stage 5b: Balanced 4-direction Brandes-Köpf
// ============================================================================

TArray<float> FLayeredLayout::RunBKPass(
	const TArray<TArray<int32>>& RankOrders,
	const TArray<TArray<FNbr>>& Up,
	const TArray<TArray<FNbr>>& Down,
	const TSet<int64>& Conflicts,
	bool bToUpper, bool bRightBias) const
{
	const int32 N = Vertices.Num();
	auto Key = [&](int32 U, int32 Lo) -> int64 { return (int64)U * N + Lo; };

	// For the right-bias passes, reverse the within-rank order and negate the
	// result at the end. This lets the single left-bias alignment + compaction
	// below serve both biases unchanged (reversing the within-layer axis turns a
	// top/left bias into a bottom/right one; negation restores a comparable frame).
	TArray<TArray<int32>> RO = RankOrders;
	if (bRightBias)
		for (TArray<int32>& Rank : RO) Algo::Reverse(Rank);

	TArray<int32> Pos;
	Pos.Init(0, N);
	for (const TArray<int32>& Rank : RO)
		for (int32 P = 0; P < Rank.Num(); ++P) Pos[Rank[P]] = P;

	// ---- Alignment ----
	TArray<int32> Root, Align;
	TArray<float> Inner;
	Root.SetNum(N);
	Align.SetNum(N);
	Inner.Init(0.f, N);
	for (int32 V = 0; V < N; ++V) { Root[V] = V; Align[V] = V; }

	for (int32 Step = 0; Step < NumRanks; ++Step)
	{
		// bToUpper: scan ranks 0..N-1, align each vertex to a rank-1 (Up) neighbor.
		// !bToUpper: scan ranks N-1..0, align to a rank+1 (Down) neighbor.
		const int32 RI = bToUpper ? Step : (NumRanks - 1 - Step);
		const TArray<int32>& L = RO[RI];
		int32 PrevPos = -1; // left-biased (in RO frame): aligned neighbor pos must strictly increase
		for (int32 K = 0; K < L.Num(); ++K)
		{
			const int32 V = L[K];
			const TArray<FNbr>& AllN = bToUpper ? Up[V] : Down[V];
			if (AllN.Num() == 0) continue;

			// Exec-preferred: if any exec neighbors, restrict to those.
			TArray<FNbr> Nbrs;
			for (const FNbr& NB : AllN) if (NB.bExec) Nbrs.Add(NB);
			if (Nbrs.Num() == 0) Nbrs = AllN;

			Nbrs.Sort([&](const FNbr& A2, const FNbr& B2)
			{
				if (Pos[A2.Id] != Pos[B2.Id]) return Pos[A2.Id] < Pos[B2.Id];
				return A2.Id < B2.Id;
			});

			const int32 D  = Nbrs.Num();
			const int32 M0 = (D - 1) / 2;
			const int32 M1 = D / 2;

			for (int32 MM = M0; MM <= M1; ++MM)
			{
				if (Align[V] != V) break; // already aligned
				const FNbr& NB = Nbrs[MM];
				// Conflict key is always (upper-rank id, lower-rank id). For an
				// Up neighbor NB is the upper vertex; for a Down neighbor V is.
				const int64 CK = bToUpper ? Key(NB.Id, V) : Key(V, NB.Id);
				if (Pos[NB.Id] > PrevPos && !Conflicts.Contains(CK))
				{
					Align[NB.Id] = V;
					Root[V]      = Root[NB.Id];
					Align[V]     = Root[V];
					Inner[V]     = Inner[NB.Id] + NB.Off;
					PrevPos      = Pos[NB.Id];
				}
			}
		}
	}

	// ---- Horizontal compaction (left-bias, on the RO/Pos frame) ----
	TArray<int32> Sink;
	TArray<float> Shift, X;
	Sink.SetNum(N);
	Shift.Init(TNumericLimits<float>::Max(), N);
	X.Init(TNumericLimits<float>::Max(), N);
	for (int32 V = 0; V < N; ++V) Sink[V] = V;

	TArray<bool> Placed;
	Placed.Init(false, N);
	TFunction<void(int32)> Place = [&](int32 VRoot)
	{
		if (Placed[VRoot]) return;
		Placed[VRoot] = true;
		X[VRoot] = 0.f;

		int32 W = VRoot;
		do
		{
			const int32 RI = Vertices[W].Rank;
			const int32 K  = Pos[W];
			if (K > 0)
			{
				const int32 PredV = RO[RI][K - 1];
				const int32 U     = Root[PredV];
				Place(U);
				if (Sink[VRoot] == VRoot) Sink[VRoot] = Sink[U];
				const float BaseSep = (Vertices[PredV].H + Vertices[W].H) * 0.5f + Config.NodeSpacingY;
				const float Sep     = BaseSep + Inner[PredV] - Inner[W];
				if (Sink[VRoot] != Sink[U])
				{
					float& ShiftRef = Shift[Sink[U]];
					ShiftRef = FMath::Min(ShiftRef, X[VRoot] - X[U] - Sep);
				}
				else
				{
					X[VRoot] = FMath::Max(X[VRoot], X[U] + Sep);
				}
			}
			W = Align[W];
		} while (W != VRoot);
	};
	for (const TArray<int32>& Rank : RO)
		for (int32 V : Rank)
			if (Root[V] == V) Place(V);

	TArray<float> Center;
	Center.SetNum(N);
	for (int32 V = 0; V < N; ++V)
	{
		float C = X[Root[V]];
		if (FMath::IsFinite(C) && Shift[Sink[Root[V]]] < TNumericLimits<float>::Max())
			C += Shift[Sink[Root[V]]];
		if (!FMath::IsFinite(C)) C = 0.f;
		Center[V] = C + Inner[V];
	}

	if (bRightBias)
		for (int32 V = 0; V < N; ++V) Center[V] = -Center[V];

	return Center;
}

void FLayeredLayout::AssignYBrandesKopf4Dir(const TArray<TArray<int32>>& RankOrders)
{
	const int32 N = Vertices.Num();
	if (N == 0) return;

	TArray<int32> Pos; // filled by ComputeBKAdjacency; each pass recomputes its own
	TArray<TArray<FNbr>> Up, Down;
	TSet<int64> Conflicts;
	ComputeBKAdjacency(RankOrders, Pos, Up, Down, Conflicts);
	(void)Pos;

	// Four directional candidate layouts. Index order: 0=UL 1=UR 2=DL 3=DR;
	// odd indices are the right-bias passes.
	TArray<float> Cand[4];
	Cand[0] = RunBKPass(RankOrders, Up, Down, Conflicts, /*bToUpper=*/true,  /*bRightBias=*/false);
	Cand[1] = RunBKPass(RankOrders, Up, Down, Conflicts, /*bToUpper=*/true,  /*bRightBias=*/true);
	Cand[2] = RunBKPass(RankOrders, Up, Down, Conflicts, /*bToUpper=*/false, /*bRightBias=*/false);
	Cand[3] = RunBKPass(RankOrders, Up, Down, Conflicts, /*bToUpper=*/false, /*bRightBias=*/true);

	// Pick the minimum-width layout as the alignment reference.
	int32 RefIdx = 0;
	float RefMin = 0.f, RefMax = 0.f, BestWidth = TNumericLimits<float>::Max();
	for (int32 I = 0; I < 4; ++I)
	{
		float Mn = TNumericLimits<float>::Max(), Mx = -TNumericLimits<float>::Max();
		for (int32 V = 0; V < N; ++V) { Mn = FMath::Min(Mn, Cand[I][V]); Mx = FMath::Max(Mx, Cand[I][V]); }
		const float Wdt = Mx - Mn;
		if (Wdt < BestWidth) { BestWidth = Wdt; RefIdx = I; RefMin = Mn; RefMax = Mx; }
	}
	(void)RefIdx;

	// Shift each candidate to the reference: left-bias (even) layouts to RefMin,
	// right-bias (odd) layouts to RefMax — the canonical BK balancing alignment.
	for (int32 I = 0; I < 4; ++I)
	{
		float Mn = TNumericLimits<float>::Max(), Mx = -TNumericLimits<float>::Max();
		for (int32 V = 0; V < N; ++V) { Mn = FMath::Min(Mn, Cand[I][V]); Mx = FMath::Max(Mx, Cand[I][V]); }
		const bool bRight = (I & 1) != 0;
		const float Delta = bRight ? (RefMax - Mx) : (RefMin - Mn);
		for (int32 V = 0; V < N; ++V) Cand[I][V] += Delta;
	}

	// Per-vertex center = average of the two middle candidate coordinates.
	for (int32 V = 0; V < N; ++V)
	{
		float C[4] = { Cand[0][V], Cand[1][V], Cand[2][V], Cand[3][V] };
		for (int32 A = 0; A < 4; ++A)
			for (int32 B = A + 1; B < 4; ++B)
				if (C[B] < C[A]) { const float T = C[A]; C[A] = C[B]; C[B] = T; }
		const float CenterY = (C[1] + C[2]) * 0.5f;
		Vertices[V].Y = CenterY - Vertices[V].H * 0.5f;
	}
}

// ============================================================================
// Helpers
// ============================================================================

TArray<TArray<int32>> FLayeredLayout::BuildRankOrders() const
{
	TArray<TArray<int32>> RankOrders;
	RankOrders.SetNum(NumRanks);

	TArray<int32> ByVertex;
	const int32 N = Vertices.Num();
	ByVertex.SetNum(N);
	for (int32 V = 0; V < N; ++V) ByVertex[V] = V;

	// Stable sort by (Rank, Order, VertexId) — fully deterministic.
	ByVertex.Sort([&](int32 A, int32 B)
	{
		if (Vertices[A].Rank != Vertices[B].Rank) return Vertices[A].Rank < Vertices[B].Rank;
		if (Vertices[A].Order != Vertices[B].Order) return Vertices[A].Order < Vertices[B].Order;
		return A < B;
	});

	for (int32 V : ByVertex) RankOrders[Vertices[V].Rank].Add(V);
	return RankOrders;
}

float FLayeredLayout::PortFromYOf(const FCoreEdge& E) const
{
	return E.PortFromY >= 0.f ? E.PortFromY : Vertices[E.From].H * 0.5f;
}

float FLayeredLayout::PortToYOf(const FCoreEdge& E) const
{
	return E.PortToY >= 0.f ? E.PortToY : Vertices[E.To].H * 0.5f;
}
