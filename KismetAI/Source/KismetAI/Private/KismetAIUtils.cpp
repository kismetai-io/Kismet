// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIUtils.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Modules/ModuleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpression.h"
#include "WidgetBlueprint.h"
#include "Components/Widget.h"
#include "Blueprint/UserWidget.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimationStateMachineGraph.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateEntryNode.h"

UEdGraphPin* KismetAIUtils::GetStateEntryOutputPin(const UAnimStateEntryNode* EntryNode)
{
	if (!EntryNode)
	{
		return nullptr;
	}
	// The entry node allocates exactly one output pin (named "Entry") in AllocateDefaultPins, stored
	// at Pins[0]. 5.7's GetOutputPin() returns this same pin; doing it by index keeps 5.6 building.
	return EntryNode->Pins.Num() > 0 ? EntryNode->Pins[0] : nullptr;
}

UBlueprint* KismetAIUtils::FindBlueprint(const FString& AssetPath)
{
	// Try loading directly
	UObject* LoadedObject = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *AssetPath);
	if (UBlueprint* BP = Cast<UBlueprint>(LoadedObject))
	{
		return BP;
	}

	// Try with _C suffix stripped (in case user passes the generated class path)
	FString CleanPath = AssetPath;
	CleanPath.RemoveFromEnd(TEXT("_C"));

	LoadedObject = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *CleanPath);
	if (UBlueprint* BP = Cast<UBlueprint>(LoadedObject))
	{
		return BP;
	}

	// Fuzzy fallback: the literal path missed (wrong folder, partial path). Resolve by
	// the asset's leaf name via the asset registry, but ONLY auto-load when exactly one
	// Blueprint matches — never guess between several candidates.
	FString Leaf = CleanPath;
	int32 SlashIdx;
	if (Leaf.FindLastChar(TEXT('/'), SlashIdx)) Leaf = Leaf.Mid(SlashIdx + 1);
	int32 DotIdx;
	if (Leaf.FindChar(TEXT('.'), DotIdx)) Leaf = Leaf.Left(DotIdx);
	Leaf.TrimStartAndEndInline();
	if (Leaf.IsEmpty())
	{
		return nullptr;
	}

	FAssetRegistryModule* ARModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (!ARModule)
	{
		return nullptr;
	}
	IAssetRegistry& AssetRegistry = ARModule->Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true; // include WidgetBlueprint, AnimBlueprint, etc.
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	UBlueprint* SingleMatch = nullptr;
	int32 MatchCount = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Equals(Leaf, ESearchCase::IgnoreCase))
		{
			if (++MatchCount > 1)
			{
				return nullptr; // ambiguous — caller should report not-found; agent uses search_assets
			}
			SingleMatch = Cast<UBlueprint>(Asset.GetAsset());
		}
	}
	return SingleMatch;
}

UEdGraph* KismetAIUtils::FindAnimGraph(UBlueprint* Blueprint)
{
	if (!Cast<UAnimBlueprint>(Blueprint))
	{
		return nullptr;
	}

	// The anim graph is the UAnimationGraph in FunctionGraphs. By convention it is named
	// "AnimGraph", but resolve by type to be robust against renames. Prefer the one literally
	// named "AnimGraph" when several UAnimationGraphs exist (linked anim layers add more).
	UEdGraph* TypedMatch = nullptr;
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (UAnimationGraph* AnimGraph = Cast<UAnimationGraph>(Graph))
		{
			if (AnimGraph->GetName() == TEXT("AnimGraph"))
			{
				return AnimGraph;
			}
			if (!TypedMatch)
			{
				TypedMatch = AnimGraph;
			}
		}
	}
	return TypedMatch;
}

// Normalize a segment/graph name for tolerant comparison: case-insensitive, and treat spaces and
// underscores as equivalent (transition graphs are named like "Idle to Walk" but agents will address
// them as "Idle_to_Walk").
static FString NormalizeGraphSegment(const FString& In)
{
	FString Out = In;
	Out.ReplaceInline(TEXT(" "), TEXT("_"));
	Out.TrimStartAndEndInline();
	return Out.ToLower();
}

// Endpoint transition addressing: a graph-path segment of the form "From->To" addresses a
// transition rule graph inside a state-machine graph by its endpoints, since editor-authored
// transition graphs have opaque object names ("AnimationTransitionGraph_3"). Spaces around the
// "->" are tolerated, endpoint names use the same space/underscore-tolerant, case-insensitive
// matching as flat names. Optional suffixes:
//   - "#N" (1-based, creation order) disambiguates duplicate From->To pairs.
//   - "@blend" resolves the transition's CustomTransitionGraph instead of the rule BoundGraph.
// Returns true if the segment parses as an endpoint form (even if it later fails to resolve),
// filling OutFrom/OutTo/OutIndex1Based/OutWantBlend. OutHasIndex distinguishes "no #N given".
static bool ParseEndpointSegment(const FString& Segment, FString& OutFrom, FString& OutTo,
	bool& OutHasIndex, int32& OutIndex1Based, bool& OutWantBlend)
{
	OutHasIndex = false;
	OutIndex1Based = 1;
	OutWantBlend = false;

	int32 ArrowIdx = Segment.Find(TEXT("->"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	if (ArrowIdx == INDEX_NONE)
	{
		return false;
	}

	FString Left = Segment.Left(ArrowIdx);
	FString Right = Segment.Mid(ArrowIdx + 2);
	Left.TrimStartAndEndInline();
	Right.TrimStartAndEndInline();

	// Trailing "@blend" on the right endpoint.
	int32 AtIdx;
	if (Right.FindLastChar(TEXT('@'), AtIdx))
	{
		const FString Tag = Right.Mid(AtIdx + 1).TrimStartAndEnd().ToLower();
		if (Tag == TEXT("blend"))
		{
			OutWantBlend = true;
			Right = Right.Left(AtIdx).TrimStartAndEnd();
		}
	}

	// Trailing "#N" disambiguator on the right endpoint.
	int32 HashIdx;
	if (Right.FindLastChar(TEXT('#'), HashIdx))
	{
		const FString NumStr = Right.Mid(HashIdx + 1).TrimStartAndEnd();
		if (!NumStr.IsEmpty() && NumStr.IsNumeric())
		{
			OutHasIndex = true;
			OutIndex1Based = FCString::Atoi(*NumStr);
			Right = Right.Left(HashIdx).TrimStartAndEnd();
		}
	}

	OutFrom = Left;
	OutTo = Right;
	return !OutFrom.IsEmpty() && !OutTo.IsEmpty();
}

// All transitions in a state-machine graph matching From->To (tolerant endpoint matching), in
// node (creation) order.
static void CollectMatchingTransitions(UEdGraph* SMGraph, const FString& From, const FString& To,
	TArray<UAnimStateTransitionNode*>& Out)
{
	UAnimationStateMachineGraph* SM = Cast<UAnimationStateMachineGraph>(SMGraph);
	if (!SM) return;
	const FString FromNorm = NormalizeGraphSegment(From);
	const FString ToNorm = NormalizeGraphSegment(To);
	for (UEdGraphNode* Node : SM->Nodes)
	{
		UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node);
		if (!T) continue;
		UAnimStateNodeBase* P = T->GetPreviousState();
		UAnimStateNodeBase* N = T->GetNextState();
		const FString PName = P ? P->GetStateName() : FString();
		const FString NName = N ? N->GetStateName() : FString();
		if (NormalizeGraphSegment(PName) == FromNorm && NormalizeGraphSegment(NName) == ToNorm)
		{
			Out.Add(T);
		}
	}
}

// Resolve a "From->To[#N][@blend]" endpoint segment inside SMGraph to its rule BoundGraph (or
// CustomTransitionGraph for @blend). Returns null on no match / ambiguity; on ambiguity fills
// OutCandidates with endpoint-form labels (e.g. "Idle->Walk#1", "Idle->Walk#2").
static UEdGraph* ResolveEndpointInSM(UEdGraph* SMGraph, const FString& Segment, TArray<FString>* OutCandidates)
{
	FString From, To;
	bool bHasIndex = false, bWantBlend = false;
	int32 Index1 = 1;
	if (!ParseEndpointSegment(Segment, From, To, bHasIndex, Index1, bWantBlend))
	{
		return nullptr;
	}

	TArray<UAnimStateTransitionNode*> Matches;
	CollectMatchingTransitions(SMGraph, From, To, Matches);
	if (Matches.Num() == 0)
	{
		return nullptr;
	}

	auto GraphFor = [&](UAnimStateTransitionNode* T) -> UEdGraph*
	{
		return bWantBlend ? T->CustomTransitionGraph.Get() : T->BoundGraph.Get();
	};

	if (Matches.Num() == 1 && !bHasIndex)
	{
		return GraphFor(Matches[0]);
	}
	if (bHasIndex)
	{
		const int32 Idx0 = Index1 - 1; // #N is 1-based, creation order
		if (Matches.IsValidIndex(Idx0))
		{
			return GraphFor(Matches[Idx0]);
		}
		return nullptr;
	}
	// Multiple matches and no #N → ambiguous: report endpoint-form candidates.
	if (OutCandidates)
	{
		for (int32 i = 0; i < Matches.Num(); ++i)
		{
			OutCandidates->Add(FString::Printf(TEXT("%s->%s#%d"), *From, *To, i + 1));
		}
	}
	return nullptr;
}

// The nested graphs directly reachable from a graph: its UEdGraph::SubGraphs plus, for any
// state-machine node, the EditorStateMachineGraph, and within a state-machine graph the BoundGraphs of
// its state / transition / conduit nodes (these live on the nodes, NOT in SubGraphs).
static void GetChildGraphs(UEdGraph* Graph, TArray<UEdGraph*>& OutChildren)
{
	if (!Graph)
	{
		return;
	}

	for (UEdGraph* Sub : Graph->SubGraphs)
	{
		if (Sub)
		{
			OutChildren.AddUnique(Sub);
		}
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (UAnimGraphNode_StateMachineBase* SMNode = Cast<UAnimGraphNode_StateMachineBase>(Node))
		{
			if (SMNode->EditorStateMachineGraph)
			{
				OutChildren.AddUnique(SMNode->EditorStateMachineGraph);
			}
		}
		// Test UAnimStateTransitionNode FIRST: it derives from UAnimStateNodeBase, so the
		// state branch would otherwise swallow transitions (their rule graph is BoundGraph,
		// not GetBoundGraph()) and the CustomTransitionGraph would be unreachable.
		else if (UAnimStateTransitionNode* TransNode = Cast<UAnimStateTransitionNode>(Node))
		{
			if (TransNode->BoundGraph)
			{
				OutChildren.AddUnique(TransNode->BoundGraph);
			}
			if (TransNode->CustomTransitionGraph)
			{
				OutChildren.AddUnique(TransNode->CustomTransitionGraph);
			}
		}
		else if (UAnimStateNodeBase* StateNode = Cast<UAnimStateNodeBase>(Node))
		{
			if (UEdGraph* Bound = StateNode->GetBoundGraph())
			{
				OutChildren.AddUnique(Bound);
			}
		}
	}
}

void KismetAIUtils::CollectGraphsRecursive(UEdGraph* Root, const FString& PathPrefix, TMap<UEdGraph*, FString>& OutGraphs)
{
	if (!Root || OutGraphs.Contains(Root))
	{
		return;
	}

	const FString ThisPath = PathPrefix.IsEmpty() ? Root->GetName() : (PathPrefix + TEXT("/") + Root->GetName());
	OutGraphs.Add(Root, ThisPath);

	TArray<UEdGraph*> Children;
	GetChildGraphs(Root, Children);
	for (UEdGraph* Child : Children)
	{
		CollectGraphsRecursive(Child, ThisPath, OutGraphs);
	}
}

// Resolve a path like "AnimGraph/Locomotion/Idle" starting from the Blueprint's top-level graphs.
// A segment of the form "From->To[#N][@blend]" addresses a transition rule graph by endpoints
// inside the preceding state-machine graph.
static UEdGraph* ResolveGraphPath(UBlueprint* Blueprint, const TArray<FString>& Segments, TArray<FString>* OutCandidates = nullptr)
{
	if (Segments.Num() == 0)
	{
		return nullptr;
	}

	// Resolve the first segment against the top-level graph arrays.
	auto MatchTopLevel = [&](const FString& Seg) -> UEdGraph*
	{
		const FString Norm = NormalizeGraphSegment(Seg);
		auto Search = [&](const TArray<TObjectPtr<UEdGraph>>& Graphs) -> UEdGraph*
		{
			for (UEdGraph* Graph : Graphs)
			{
				if (Graph && NormalizeGraphSegment(Graph->GetName()) == Norm)
				{
					return Graph;
				}
			}
			return nullptr;
		};
		if (UEdGraph* G = Search(Blueprint->UbergraphPages)) return G;
		if (UEdGraph* G = Search(Blueprint->FunctionGraphs)) return G;
		if (UEdGraph* G = Search(Blueprint->MacroGraphs)) return G;
		if (UEdGraph* G = Search(Blueprint->EventGraphs)) return G;
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			if (UEdGraph* G = Search(InterfaceDesc.Graphs)) return G;
		}
		return nullptr;
	};

	UEdGraph* Current = MatchTopLevel(Segments[0]);
	if (!Current)
	{
		return nullptr;
	}

	// Walk the remaining segments through nested child graphs.
	for (int32 i = 1; i < Segments.Num(); ++i)
	{
		// Endpoint form ("From->To") resolves against the current (state-machine) graph.
		if (Segments[i].Contains(TEXT("->")))
		{
			UEdGraph* Endpoint = ResolveEndpointInSM(Current, Segments[i], OutCandidates);
			if (!Endpoint)
			{
				return nullptr;
			}
			Current = Endpoint;
			continue;
		}

		const FString Norm = NormalizeGraphSegment(Segments[i]);
		TArray<UEdGraph*> Children;
		GetChildGraphs(Current, Children);

		UEdGraph* Next = nullptr;
		for (UEdGraph* Child : Children)
		{
			if (Child && NormalizeGraphSegment(Child->GetName()) == Norm)
			{
				Next = Child;
				break;
			}
		}
		if (!Next)
		{
			return nullptr;
		}
		Current = Next;
	}

	return Current;
}

UEdGraph* KismetAIUtils::FindGraph(UBlueprint* Blueprint, const FString& GraphName, TArray<FString>* OutCandidates)
{
	if (!Blueprint)
	{
		return nullptr;
	}

	// Default graph: AnimGraph for anim BPs, first ubergraph page otherwise.
	if (GraphName.IsEmpty())
	{
		if (UEdGraph* AnimGraph = FindAnimGraph(Blueprint))
		{
			return AnimGraph;
		}
		if (Blueprint->UbergraphPages.Num() > 0)
		{
			return Blueprint->UbergraphPages[0];
		}
		return nullptr;
	}

	// Path-style addressing: "A/B/C".
	if (GraphName.Contains(TEXT("/")))
	{
		TArray<FString> Segments;
		GraphName.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty=*/true);
		return ResolveGraphPath(Blueprint, Segments, OutCandidates);
	}

	// Flat endpoint form ("From->To[#N][@blend]") with no path prefix: search every
	// state-machine graph reachable from the anim graph. A unique match resolves; ambiguity
	// across SMs (or duplicate pairs within one) reports endpoint-form candidates.
	if (GraphName.Contains(TEXT("->")))
	{
		TArray<UEdGraph*> SMGraphs;
		if (UEdGraph* AnimRoot = FindAnimGraph(Blueprint))
		{
			TMap<UEdGraph*, FString> All;
			CollectGraphsRecursive(AnimRoot, FString(), All);
			for (const TPair<UEdGraph*, FString>& Pair : All)
			{
				if (Pair.Key && Pair.Key->IsA<UAnimationStateMachineGraph>())
				{
					SMGraphs.Add(Pair.Key);
				}
			}
		}
		TArray<UEdGraph*> Found;
		TArray<FString> Candidates;
		for (UEdGraph* SM : SMGraphs)
		{
			TArray<FString> LocalCandidates;
			if (UEdGraph* G = ResolveEndpointInSM(SM, GraphName, &LocalCandidates))
			{
				Found.AddUnique(G);
			}
			Candidates.Append(LocalCandidates);
		}
		if (Found.Num() == 1)
		{
			return Found[0];
		}
		if (Found.Num() == 0 && Candidates.Num() > 0 && OutCandidates)
		{
			Candidates.Sort();
			OutCandidates->Append(Candidates);
		}
		return nullptr;
	}

	// Flat name: exact top-level match first, preserving original (case-sensitive) semantics so
	// regular-Blueprint behavior is unchanged.
	auto SearchExact = [&GraphName](const TArray<TObjectPtr<UEdGraph>>& Graphs) -> UEdGraph*
	{
		for (UEdGraph* Graph : Graphs)
		{
			if (Graph && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}
		return nullptr;
	};

	if (UEdGraph* Found = SearchExact(Blueprint->UbergraphPages)) return Found;
	if (UEdGraph* Found = SearchExact(Blueprint->FunctionGraphs)) return Found;
	if (UEdGraph* Found = SearchExact(Blueprint->MacroGraphs)) return Found;
	if (UEdGraph* Found = SearchExact(Blueprint->EventGraphs)) return Found;
	for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (UEdGraph* Found = SearchExact(InterfaceDesc.Graphs)) return Found;
	}

	// Fallback: recursive search through all graphs + nested SubGraphs / state machine BoundGraphs.
	// Tolerant matching (case-insensitive, space/underscore equivalent). A unique match resolves;
	// ambiguity reports candidate paths.
	{
		TMap<UEdGraph*, FString> AllGraphs;
		auto CollectFrom = [&](const TArray<TObjectPtr<UEdGraph>>& Roots, const FString& Prefix)
		{
			for (UEdGraph* Root : Roots)
			{
				CollectGraphsRecursive(Root, Prefix, AllGraphs);
			}
		};
		CollectFrom(Blueprint->UbergraphPages, FString());
		CollectFrom(Blueprint->FunctionGraphs, FString());
		CollectFrom(Blueprint->MacroGraphs, FString());
		CollectFrom(Blueprint->EventGraphs, FString());
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			CollectFrom(InterfaceDesc.Graphs, FString());
		}

		const FString Norm = NormalizeGraphSegment(GraphName);
		TArray<UEdGraph*> Matches;
		TArray<FString> MatchPaths;
		for (const TPair<UEdGraph*, FString>& Pair : AllGraphs)
		{
			if (Pair.Key && NormalizeGraphSegment(Pair.Key->GetName()) == Norm)
			{
				Matches.Add(Pair.Key);
				MatchPaths.Add(Pair.Value);
			}
		}

		if (Matches.Num() == 1)
		{
			return Matches[0];
		}
		if (Matches.Num() > 1 && OutCandidates)
		{
			MatchPaths.Sort();
			OutCandidates->Append(MatchPaths);
		}
	}

	return nullptr;
}

/** Shared loader: direct StaticLoadObject by path, then a single-match fuzzy fallback by leaf
 *  name through the asset registry. Mirrors FindBlueprint but parameterized by asset class so it
 *  serves materials, instances, and material functions. Never guesses between multiple matches.
 *
 *  An object already marked as garbage (deleted, or torn down by a reset that has not been
 *  collected yet) is still resolvable by path. It is treated as not found: handing it to an
 *  editing or compile API (UMaterialEditingLibrary::RecompileMaterial) crashes the editor. */
template<typename T>
static T* LoadAssetByPathOrUniqueLeaf(const FString& AssetPath)
{
	if (T* Direct = Cast<T>(StaticLoadObject(T::StaticClass(), nullptr, *AssetPath)))
	{
		return IsValid(Direct) ? Direct : nullptr;
	}

	FString Leaf = AssetPath;
	int32 SlashIdx;
	if (Leaf.FindLastChar(TEXT('/'), SlashIdx)) Leaf = Leaf.Mid(SlashIdx + 1);
	int32 DotIdx;
	if (Leaf.FindChar(TEXT('.'), DotIdx)) Leaf = Leaf.Left(DotIdx);
	Leaf.TrimStartAndEndInline();
	if (Leaf.IsEmpty())
	{
		return nullptr;
	}

	FAssetRegistryModule* ARModule = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (!ARModule)
	{
		return nullptr;
	}
	IAssetRegistry& AssetRegistry = ARModule->Get();

	FARFilter Filter;
	Filter.ClassPaths.Add(T::StaticClass()->GetClassPathName());
	Filter.bRecursiveClasses = true; // include UMaterialInstanceConstant under UMaterialInterface, etc.
	TArray<FAssetData> Assets;
	AssetRegistry.GetAssets(Filter, Assets);

	T* SingleMatch = nullptr;
	int32 MatchCount = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetName.ToString().Equals(Leaf, ESearchCase::IgnoreCase))
		{
			if (++MatchCount > 1)
			{
				return nullptr; // ambiguous — caller reports not-found; agent uses search_assets
			}
			SingleMatch = Cast<T>(Asset.GetAsset());
		}
	}
	return IsValid(SingleMatch) ? SingleMatch : nullptr;
}

UMaterialInterface* KismetAIUtils::FindMaterialInterface(const FString& AssetPath)
{
	return LoadAssetByPathOrUniqueLeaf<UMaterialInterface>(AssetPath);
}

UMaterial* KismetAIUtils::FindMaterial(const FString& AssetPath)
{
	// A direct UMaterial load is preferred; fall back through the interface finder so that a
	// path resolving to an instance still loads (caller decides what to do with a non-base material).
	if (UMaterial* Direct = LoadAssetByPathOrUniqueLeaf<UMaterial>(AssetPath))
	{
		return Direct;
	}
	return Cast<UMaterial>(FindMaterialInterface(AssetPath));
}

UMaterialFunction* KismetAIUtils::FindMaterialFunction(const FString& AssetPath)
{
	return LoadAssetByPathOrUniqueLeaf<UMaterialFunction>(AssetPath);
}

UClass* KismetAIUtils::ResolveMaterialExpressionClass(const FString& NodeName, TArray<FString>* OutCandidates)
{
	static const FString Prefix = TEXT("MaterialExpression");

	// Normalize: strip whitespace and an optional leading "MaterialExpression", lowercase.
	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In;
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		Out.TrimStartAndEndInline();
		return Out.ToLower();
	};

	FString Query = NodeName.TrimStartAndEnd();
	if (Query.StartsWith(Prefix, ESearchCase::IgnoreCase))
	{
		Query = Query.RightChop(Prefix.Len());
	}
	const FString QueryNorm = Normalize(Query);
	if (QueryNorm.IsEmpty())
	{
		return nullptr;
	}

	TArray<UClass*> Derived;
	GetDerivedClasses(UMaterialExpression::StaticClass(), Derived, /*bRecursive=*/true);

	UClass* ExactMatch = nullptr;
	int32 ExactCount = 0;
	TArray<UClass*> PrefixMatches;
	TArray<UClass*> SubstringMatches;

	for (UClass* Cls : Derived)
	{
		if (!Cls) continue;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;

		// Friendly name = class name with the "MaterialExpression" prefix removed.
		FString ClassName = Cls->GetName();
		FString Friendly = ClassName;
		Friendly.RemoveFromStart(Prefix, ESearchCase::IgnoreCase);
		const FString FriendlyNorm = Normalize(Friendly);
		if (FriendlyNorm.IsEmpty()) continue;

		if (FriendlyNorm == QueryNorm)
		{
			ExactMatch = Cls;
			++ExactCount;
		}
		else if (FriendlyNorm.StartsWith(QueryNorm))
		{
			PrefixMatches.Add(Cls);
		}
		else if (FriendlyNorm.Contains(QueryNorm))
		{
			SubstringMatches.Add(Cls);
		}
	}

	auto FriendlyNameOf = [](UClass* Cls) -> FString
	{
		FString N = Cls->GetName();
		N.RemoveFromStart(TEXT("MaterialExpression"), ESearchCase::IgnoreCase);
		return N;
	};

	if (ExactCount == 1)
	{
		return ExactMatch;
	}
	if (ExactCount == 0 && PrefixMatches.Num() == 1)
	{
		return PrefixMatches[0];
	}
	if (ExactCount == 0 && PrefixMatches.Num() == 0 && SubstringMatches.Num() == 1)
	{
		return SubstringMatches[0];
	}

	// Ambiguous or none — report candidate friendly names so the caller can guide the agent.
	if (OutCandidates)
	{
		const TArray<UClass*>& Tier = (PrefixMatches.Num() > 0) ? PrefixMatches : SubstringMatches;
		for (UClass* Cls : Tier)
		{
			OutCandidates->Add(FriendlyNameOf(Cls));
		}
	}
	return nullptr;
}

UWidgetBlueprint* KismetAIUtils::FindWidgetBlueprint(const FString& AssetPath)
{
	return Cast<UWidgetBlueprint>(FindBlueprint(AssetPath));
}

UClass* KismetAIUtils::ResolveWidgetClass(const FString& Name, TArray<FString>* OutCandidates)
{
	const FString Query = Name.TrimStartAndEnd();
	if (Query.IsEmpty())
	{
		return nullptr;
	}

	// An asset path resolves to a Blueprint widget's generated class (e.g. another WBP_*).
	if (Query.StartsWith(TEXT("/")))
	{
		if (UBlueprint* BP = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *Query)))
		{
			if (BP->GeneratedClass && BP->GeneratedClass->IsChildOf(UWidget::StaticClass()))
			{
				return BP->GeneratedClass;
			}
		}
		return nullptr;
	}

	auto Normalize = [](const FString& In) -> FString
	{
		FString Out = In;
		Out.ReplaceInline(TEXT(" "), TEXT(""));
		Out.ReplaceInline(TEXT("_"), TEXT(""));
		Out.TrimStartAndEndInline();
		return Out.ToLower();
	};

	const FString QueryNorm = Normalize(Query);
	if (QueryNorm.IsEmpty())
	{
		return nullptr;
	}

	UClass* ExactMatch = nullptr;
	int32 ExactCount = 0;
	TArray<UClass*> PrefixMatches;
	TArray<UClass*> SubstringMatches;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Cls = *It;
		if (!Cls || !Cls->IsChildOf(UWidget::StaticClass())) continue;
		if (Cls->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden)) continue;
		if (Cls->HasMetaData(TEXT("HideDropdown"))) continue;
		// UUserWidget is the base for every Blueprint widget; skip the base itself but keep subclasses.
		if (Cls == UUserWidget::StaticClass()) continue;

		const FString ClassName = Cls->GetName();
		const FString FriendlyNorm = Normalize(ClassName);
		if (FriendlyNorm.IsEmpty()) continue;

		if (FriendlyNorm == QueryNorm)
		{
			ExactMatch = Cls;
			++ExactCount;
		}
		else if (FriendlyNorm.StartsWith(QueryNorm))
		{
			PrefixMatches.Add(Cls);
		}
		else if (FriendlyNorm.Contains(QueryNorm))
		{
			SubstringMatches.Add(Cls);
		}
	}

	if (ExactCount == 1)
	{
		return ExactMatch;
	}
	if (ExactCount == 0 && PrefixMatches.Num() == 1)
	{
		return PrefixMatches[0];
	}
	if (ExactCount == 0 && PrefixMatches.Num() == 0 && SubstringMatches.Num() == 1)
	{
		return SubstringMatches[0];
	}

	// Ambiguous or none — report candidate class names so the caller can guide the agent.
	if (OutCandidates)
	{
		const TArray<UClass*>& Tier = (PrefixMatches.Num() > 0) ? PrefixMatches : SubstringMatches;
		for (UClass* Cls : Tier)
		{
			OutCandidates->Add(Cls->GetName());
		}
	}
	return nullptr;
}

/** Helper: try to find a UObject by full path, or by short name with fallback module prefixes. */
template<typename T>
static T* FindObjectByPathOrShortName(const FString& Path, const TArray<FString>& FallbackPrefixes, FString* OutError, const FString& TypeLabel)
{
	if (Path.Contains(TEXT("/")))
	{
		T* Found = FindObject<T>(nullptr, *Path);
		if (!Found)
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("%s not found: %s"), *TypeLabel, *Path);
			}
		}
		return Found;
	}

	// Short name — try each prefix
	for (const FString& Prefix : FallbackPrefixes)
	{
		FString FullPath = Prefix + TEXT(".") + Path;
		if (T* Found = FindObject<T>(nullptr, *FullPath))
		{
			return Found;
		}
	}

	if (OutError)
	{
		*OutError = FString::Printf(TEXT("%s not found by short name: %s"), *TypeLabel, *Path);
	}
	return nullptr;
}

FEdGraphPinType KismetAIUtils::ParsePinTypeString(const FString& TypeStr, FString* OutError)
{
	FEdGraphPinType PinType;

	FString Trimmed = TypeStr.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		if (OutError)
		{
			*OutError = TEXT("Type string is empty");
		}
		return PinType;
	}

	FString Lower = Trimmed.ToLower();

	// --- Container types: array<T> / set<T> / map<K,V> / T[] ---
	// Wrap the element type(s) parsed recursively. Done before primitives so
	// "array<int>" doesn't fall through to the unknown-type error.
	{
		auto SplitTopLevelComma = [](const FString& S, FString& OutA, FString& OutB) -> bool
		{
			int32 Depth = 0;
			for (int32 i = 0; i < S.Len(); ++i)
			{
				const TCHAR C = S[i];
				if (C == TEXT('<')) ++Depth;
				else if (C == TEXT('>')) --Depth;
				else if (C == TEXT(',') && Depth == 0)
				{
					OutA = S.Left(i).TrimStartAndEnd();
					OutB = S.Mid(i + 1).TrimStartAndEnd();
					return true;
				}
			}
			return false;
		};
		auto MatchGeneric = [&Lower, &Trimmed](const TCHAR* Kw, FString& OutInner) -> bool
		{
			const FString Open = FString(Kw) + TEXT("<");
			if (Lower.StartsWith(Open) && Trimmed.EndsWith(TEXT(">")))
			{
				OutInner = Trimmed.Mid(Open.Len(), Trimmed.Len() - Open.Len() - 1).TrimStartAndEnd();
				return true;
			}
			return false;
		};

		FString Inner;
		const bool bArray = MatchGeneric(TEXT("array"), Inner) || MatchGeneric(TEXT("tarray"), Inner)
			|| (Trimmed.EndsWith(TEXT("[]")) ? (Inner = Trimmed.LeftChop(2).TrimStartAndEnd(), true) : false);
		if (bArray)
		{
			FEdGraphPinType Elem = ParsePinTypeString(Inner, OutError);
			if (Elem.PinCategory.IsNone()) return FEdGraphPinType();
			Elem.ContainerType = EPinContainerType::Array;
			return Elem;
		}
		if (MatchGeneric(TEXT("set"), Inner) || MatchGeneric(TEXT("tset"), Inner))
		{
			FEdGraphPinType Elem = ParsePinTypeString(Inner, OutError);
			if (Elem.PinCategory.IsNone()) return FEdGraphPinType();
			Elem.ContainerType = EPinContainerType::Set;
			return Elem;
		}
		if (MatchGeneric(TEXT("map"), Inner) || MatchGeneric(TEXT("tmap"), Inner))
		{
			FString KeyStr, ValStr;
			if (!SplitTopLevelComma(Inner, KeyStr, ValStr))
			{
				if (OutError) *OutError = FString::Printf(TEXT("map needs 'key,value' (e.g. map<int,string>): %s"), *Trimmed);
				return FEdGraphPinType();
			}
			FEdGraphPinType KeyType = ParsePinTypeString(KeyStr, OutError);
			if (KeyType.PinCategory.IsNone()) return FEdGraphPinType();
			FEdGraphPinType ValType = ParsePinTypeString(ValStr, OutError);
			if (ValType.PinCategory.IsNone()) return FEdGraphPinType();
			KeyType.ContainerType = EPinContainerType::Map;
			KeyType.PinValueType = FEdGraphTerminalType::FromPinType(ValType);
			return KeyType;
		}
	}

	// --- Primitive types ---
	if (Lower == TEXT("bool"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		return PinType;
	}
	if (Lower == TEXT("byte"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
		return PinType;
	}
	if (Lower == TEXT("int"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
		return PinType;
	}
	if (Lower == TEXT("int64"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
		return PinType;
	}
	if (Lower == TEXT("float"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = TEXT("float");
		return PinType;
	}
	if (Lower == TEXT("double"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
		PinType.PinSubCategory = TEXT("double");
		return PinType;
	}
	if (Lower == TEXT("string"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		return PinType;
	}
	if (Lower == TEXT("name"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
		return PinType;
	}
	if (Lower == TEXT("text"))
	{
		PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
		return PinType;
	}

	// --- Compound types (prefix:path) ---
	FString Prefix;
	FString Path;
	if (Trimmed.Split(TEXT(":"), &Prefix, &Path))
	{
		Prefix = Prefix.ToLower().TrimStartAndEnd();
		Path = Path.TrimStartAndEnd();

		if (Path.IsEmpty())
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("Missing path after '%s:'"), *Prefix);
			}
			return FEdGraphPinType();
		}

		static const TArray<FString> ClassPrefixes = {
			TEXT("/Script/Engine"),
			TEXT("/Script/CoreUObject")
		};
		static const TArray<FString> StructPrefixes = {
			TEXT("/Script/CoreUObject"),
			TEXT("/Script/Engine")
		};
		static const TArray<FString> EnumPrefixes = {
			TEXT("/Script/Engine")
		};

		if (Prefix == TEXT("object"))
		{
			UClass* FoundClass = FindObjectByPathOrShortName<UClass>(Path, ClassPrefixes, OutError, TEXT("Class"));
			if (!FoundClass)
			{
				return FEdGraphPinType();
			}
			PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
			PinType.PinSubCategoryObject = FoundClass;
			return PinType;
		}
		if (Prefix == TEXT("softobject"))
		{
			UClass* FoundClass = FindObjectByPathOrShortName<UClass>(Path, ClassPrefixes, OutError, TEXT("Class"));
			if (!FoundClass)
			{
				return FEdGraphPinType();
			}
			PinType.PinCategory = UEdGraphSchema_K2::PC_SoftObject;
			PinType.PinSubCategoryObject = FoundClass;
			return PinType;
		}
		if (Prefix == TEXT("class"))
		{
			UClass* FoundClass = FindObjectByPathOrShortName<UClass>(Path, ClassPrefixes, OutError, TEXT("Class"));
			if (!FoundClass)
			{
				return FEdGraphPinType();
			}
			PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
			PinType.PinSubCategoryObject = FoundClass;
			return PinType;
		}
		if (Prefix == TEXT("struct"))
		{
			UScriptStruct* FoundStruct = FindObjectByPathOrShortName<UScriptStruct>(Path, StructPrefixes, OutError, TEXT("Struct"));
			if (!FoundStruct)
			{
				return FEdGraphPinType();
			}
			PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			PinType.PinSubCategoryObject = FoundStruct;
			return PinType;
		}
		if (Prefix == TEXT("enum"))
		{
			UEnum* FoundEnum = FindObjectByPathOrShortName<UEnum>(Path, EnumPrefixes, OutError, TEXT("Enum"));
			if (!FoundEnum)
			{
				return FEdGraphPinType();
			}
			PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			PinType.PinSubCategoryObject = FoundEnum;
			return PinType;
		}

		// Unknown prefix — fall through to error below
	}

	if (OutError)
	{
		*OutError = FString::Printf(TEXT("Unrecognized type string: %s"), *Trimmed);
	}
	return FEdGraphPinType();
}
