// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphPin.h"

class UBlueprint;
class UEdGraph;
class UMaterial;
class UMaterialInterface;
class UMaterialFunction;
class UMaterialExpression;
class UWidgetBlueprint;
class UAnimStateEntryNode;

namespace KismetAIUtils
{
	/** Returns the single output pin of a state-machine entry node (named "Entry"), or null.
	 *  Version-agnostic: UAnimStateEntryNode::GetOutputPin() is 5.7+ only, but the entry node
	 *  always has exactly one output pin allocated as Pins[0], so this works on 5.6 and 5.7 alike. */
	KISMETAI_API UEdGraphPin* GetStateEntryOutputPin(const UAnimStateEntryNode* EntryNode);

	/** Find Blueprint by asset path (e.g. "/Game/Blueprints/BP_Player"). Strips _C suffix if present. */
	KISMETAI_API UBlueprint* FindBlueprint(const FString& AssetPath);

	/** Find a specific graph in a Blueprint by name or path.
	 *
	 *  Addressing scheme:
	 *  - Empty name: for an Animation Blueprint returns the AnimGraph (the UAnimationGraph in
	 *    FunctionGraphs); for a regular Blueprint returns the first ubergraph page (unchanged).
	 *  - Flat name (no '/'): matches a top-level graph by GetName(); if none matches, falls back to a
	 *    recursive search through every graph and its nested SubGraphs (state-machine graphs, state /
	 *    transition / conduit BoundGraphs, custom-transition graphs). Matching is case-insensitive and
	 *    tolerant of space/underscore differences (transition graphs are named like "Idle to Walk" but
	 *    may be addressed as "Idle_to_Walk"). A unique nested match resolves; ambiguity returns null and
	 *    fills OutCandidates (if provided) with the qualified paths of the matches.
	 *  - Path "A/B/C": resolves segment by segment — the first segment is a top-level graph, each later
	 *    segment a nested graph reachable from the previous (SubGraphs + state/transition BoundGraphs).
	 *
	 *  Regular (non-anim) Blueprint behavior for flat top-level names is 100% unchanged. */
	KISMETAI_API UEdGraph* FindGraph(UBlueprint* Blueprint, const FString& GraphName, TArray<FString>* OutCandidates = nullptr);

	/** Returns the AnimGraph (UAnimationGraph) for an Animation Blueprint, or null if not an anim BP or
	 *  no anim graph is present. The anim graph is the UAnimationGraph stored in FunctionGraphs (named
	 *  "AnimGraph" by convention; resolved by type to be robust). */
	KISMETAI_API UEdGraph* FindAnimGraph(UBlueprint* Blueprint);

	/** Collect every graph reachable from a Blueprint root graph, including nested SubGraphs and the
	 *  BoundGraphs of state-machine state/transition/conduit nodes (which are NOT in SubGraphs). Each
	 *  entry maps a graph to a human-readable qualified path ("AnimGraph/Locomotion/Idle"). */
	KISMETAI_API void CollectGraphsRecursive(UEdGraph* Root, const FString& PathPrefix, TMap<UEdGraph*, FString>& OutGraphs);

	/** Find a UMaterialInterface (UMaterial or UMaterialInstance) by asset path.
	 *  Direct load first, then a single-match fuzzy fallback by leaf name via the asset registry. */
	KISMETAI_API UMaterialInterface* FindMaterialInterface(const FString& AssetPath);

	/** Find a base UMaterial by asset path. Returns null for instances (which have no editable graph). */
	KISMETAI_API UMaterial* FindMaterial(const FString& AssetPath);

	/** Find a UMaterialFunction by asset path (direct load + single-match fuzzy fallback). */
	KISMETAI_API UMaterialFunction* FindMaterialFunction(const FString& AssetPath);

	/** Resolve a friendly node name (e.g. "Multiply", "Constant3Vector", "TextureSample",
	 *  "ScalarParameter") to a concrete UMaterialExpression subclass. The leading
	 *  "MaterialExpression" prefix is optional and matching is case-insensitive: exact match
	 *  wins, then unique prefix, then unique substring. Returns null if no unambiguous match.
	 *  On ambiguity, OutCandidates (if provided) is filled with the friendly names that matched. */
	KISMETAI_API UClass* ResolveMaterialExpressionClass(const FString& NodeName, TArray<FString>* OutCandidates = nullptr);

	/** Find a Widget Blueprint (UMG) by asset path. Resolves through FindBlueprint, then casts to
	 *  UWidgetBlueprint — returns null if the asset is a Blueprint but not a Widget Blueprint. */
	KISMETAI_API UWidgetBlueprint* FindWidgetBlueprint(const FString& AssetPath);

	/** Resolve a friendly widget class name (e.g. "TextBlock", "Button", "CanvasPanel") to a
	 *  concrete UWidget subclass. Matching is case-insensitive: exact match wins, then unique
	 *  prefix, then unique substring. Enumerates native UWidget subclasses (skipping abstract /
	 *  deprecated / hidden / HideDropdown classes and UUserWidget itself, but not its subclasses).
	 *  Names that look like an asset path (start with "/") resolve to a Blueprint widget's generated
	 *  class. Returns null if no unambiguous match; on ambiguity OutCandidates (if provided) is
	 *  filled with the candidate class names. */
	KISMETAI_API UClass* ResolveWidgetClass(const FString& Name, TArray<FString>* OutCandidates = nullptr);

	/** Parse a type string into FEdGraphPinType. Returns empty type with OutError set on failure.
	 *  Formats: "bool", "int", "float", "double", "string", "name", "text",
	 *           "object:ClassName", "object:/Script/Engine.Actor",
	 *           "softobject:ClassName", "class:ClassName",
	 *           "struct:/Script/CoreUObject.Vector", "enum:/Script/Engine.EBlendMode"
	 *  Short names (without /) resolve to /Script/Engine module. */
	KISMETAI_API FEdGraphPinType ParsePinTypeString(const FString& TypeStr, FString* OutError = nullptr);
}
