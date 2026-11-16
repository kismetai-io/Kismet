// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Canonical tool definitions.
//
// Every tool name used to appear at nine independent C++ sites with nothing
// checking that they agreed. For manage_components: argument marshalling,
// discovery category map, discovery suggestion string, validation dispatch,
// handler dispatch, the mutation set, the truncation table, the adaptive
// max_tokens table, and route registration. layout_graph is what happens when
// one of the nine is forgotten — advertised, validated, routed, and
// undispatchable, silently, for the whole life of the feature.
//
// This registry owns the metadata half of that: what a tool costs, whether it
// mutates, whether plan mode permits it, how large a result it may return, what
// the next turn's token budget should be, and which discovery category reveals
// it.
//
// The invariant a registry buys, and the reason it is worth the churn: a fact is
// written once, and KismetAI.ToolRegistry asserts the registry and the shipped
// tool schemas describe the same set of tools.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIValidation.h"

// What a tool is allowed to touch. Declared here, enforced by the tool policy.
// Deliberately coarse — a rule language before there are real cases to serve
// would be speculative.
enum class EToolCapability : uint8
{
	// Reads graphs, assets, source, logs. Safe in plan mode.
	Inspect,
	// Changes an asset: graphs, components, structure, new assets.
	AssetMutate,
	// Writes outside the asset system (plan files under Saved/).
	ProjectFileWrite,
	// Touches only agent run state — plan mode, todos, the ask_user channel.
	AgentState,
};

// How much output budget the NEXT turn is likely to need, given that this tool
// was called on THIS turn. Replaces the if/else chain in RunTask's adaptive
// max_tokens block.
enum class ENextTurnBudget : uint8
{
	// No opinion — leave the default budget.
	Default,
	// This turn produced something to verify; the next turn reads or compiles.
	Low,
	// This turn gathered context; the next turn likely writes T3D.
	High,
};

// One tool, described once.
struct KISMETAIRUNTIME_API FKismetAIToolDefinition
{
	FString Name;

	EToolCapability Capability = EToolCapability::Inspect;

	// Mutating tools reset the loop's no-progress counter when they SUCCEED, and
	// have their failed calls recorded so an identical resend is short-circuited
	// (R3b). Read-only tools have identical repeats short-circuited outright (R3).
	// A tool is at most one of these.
	bool bIsMutation = false;
	bool bIsIdempotentRead = false;

	// Plan mode is read-only: the agent may inspect and plan, not mutate.
	// Derived from Capability rather than stored, so the two cannot disagree.
	bool IsAllowedInPlanMode() const
	{
		return Capability == EToolCapability::Inspect || Capability == EToolCapability::AgentState;
	}

	// Cap on the tool result handed back to the model, in characters. 0 means
	// "use the config default". read_blueprint varies by mode — see
	// GetMaxResultChars.
	int32 MaxResultChars = 0;

	ENextTurnBudget BudgetHint = ENextTurnBudget::Default;

	// discover_tools categories that reveal this tool. Empty for core tools,
	// which are always advertised.
	TArray<FString> DiscoveryCategories;

	// True for the 13 tools in tools_core.json, which are sent every turn.
	bool bIsCore = false;

	// Handled inside the agent rather than by an editor handler: these touch run
	// state (plan mode, todos, the ask_user channel, tool discovery).
	bool bIsInProcess = false;

	// Argument validation. Null for the in-process tools, which validate inline
	// against agent run state. This replaced a 27-branch if/else chain in
	// ExecuteTool — the last place a tool's identity was spelled out separately
	// from its definition.
	FValidationResult (*Validate)(const TSharedPtr<FJsonObject>&) = nullptr;
};

// The single source of truth for tool metadata.
//
// Static because the set is fixed at build time and every consumer — the loop,
// the plan gate, discovery — needs it without threading a reference through.
// Adds validators and handlers to the definitions, at which point
// registration becomes explicit and this becomes an instance.
class KISMETAIRUNTIME_API FKismetAIToolRegistry
{
public:
	// Null when the name is not a known tool.
	static const FKismetAIToolDefinition* Find(const FString& ToolName);

	static TSet<FString> GetAllNames();
	static TArray<FString> GetNamesInCategory(const FString& Category);
	static TSet<FString> GetCategoryNames();

	// --- convenience accessors used by the loop -------------------------
	// Each answers a question the loop used to answer with its own table.

	// Null for domains without the Blueprint package revision contract.
	static const TCHAR* RevisionTargetField(const FString& ToolName);

	static bool IsMutation(const FString& ToolName);
	static bool IsIdempotentRead(const FString& ToolName);
	static bool IsAllowedInPlanMode(const FString& ToolName);
	static ENextTurnBudget GetBudgetHint(const FString& ToolName);

	// Result cap for a call. Mode matters for read_blueprint, whose t3d output is
	// far larger than its describe output. DefaultChars is the config fallback.
	static int32 GetMaxResultChars(const FString& ToolName, const FString& Mode, int32 DefaultChars);

	// Tools plan mode permits, for the list enter_plan_mode returns to the model.
	// Derived, so what the model is told and what the gate enforces cannot drift.
	static TArray<FString> GetPlanModeAllowedNames();

	// Validate a call's arguments. Tools with no validator (the in-process ones)
	// pass. An unknown tool passes too — dispatch rejects it with UNKNOWN_TOOL,
	// which is the clearer error.
	static FValidationResult ValidateArgs(const FString& ToolName, const TSharedPtr<FJsonObject>& Args);
};
