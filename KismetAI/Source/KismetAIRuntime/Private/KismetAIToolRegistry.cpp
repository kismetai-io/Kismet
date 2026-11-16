// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIToolRegistry.h"
#include "KismetAISourceContract.h"

namespace
{
	// Every value here was transcribed from the table it replaces. The sources,
	// all in KismetAIAgent.cpp earlier:
	//   bIsMutation         <- MutationToolNames
	//   bIsIdempotentRead   <- ReadOnlyToolNames
	//   Capability          <- IsToolAllowedInPlanMode (inverted: plan mode is
	//                          read-only, so "allowed" == Inspect/AgentState)
	//   MaxResultChars      <- the per-tool truncation if/else chain
	//   BudgetHint          <- the adaptive max_tokens classification
	//   DiscoveryCategories <- the discover_tools CategoryMap
	struct FRegistryBuilder
	{
		TMap<FString, FKismetAIToolDefinition> Defs;

		FKismetAIToolDefinition& Add(const TCHAR* Name, EToolCapability Cap, bool bCore)
		{
			FKismetAIToolDefinition D;
			D.Name = Name;
			D.Capability = Cap;
			D.bIsCore = bCore;
			return Defs.Add(FString(Name), MoveTemp(D));
		}
	};

	const TMap<FString, FKismetAIToolDefinition>& GetDefs()
	{
		static const TMap<FString, FKismetAIToolDefinition> Table = []
		{
			FRegistryBuilder B;

			// ── core (tools_core.json — always advertised) ──────────────────
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("read_blueprint"), EToolCapability::Inspect, true);
				D.Validate = &FKismetAIValidation::ValidateReadBlueprint;
				D.bIsIdempotentRead = true;
				// Mode-dependent; see GetMaxResultChars.
				D.BudgetHint = ENextTurnBudget::High;   // read -> next turn writes
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("write_blueprint"), EToolCapability::AssetMutate, true);
				D.Validate = &FKismetAIValidation::ValidateWriteBlueprint;
				D.bIsMutation = true;
				D.MaxResultChars = 15000;
				D.BudgetHint = ENextTurnBudget::Low;    // wrote -> next turn verifies
			}
			{
				// AssetMutate, not Inspect: compiling writes the generated class and
				// dirties the package, and plan mode has always refused it
				// (system_prompt.txt:164 tells the model so explicitly). Classifying
				// it as Inspect would silently widen plan mode.
				FKismetAIToolDefinition& D = B.Add(TEXT("compile_blueprint"), EToolCapability::AssetMutate, true);
				D.Validate = &FKismetAIValidation::ValidateCompileBlueprint;
				D.MaxResultChars = 5000;
				D.BudgetHint = ENextTurnBudget::High;
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("edit_blueprint"), EToolCapability::AssetMutate, true);
				D.Validate = &FKismetAIValidation::ValidateEditBlueprint;
				D.bIsMutation = true;
				D.MaxResultChars = 10000;
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("insert_node"), EToolCapability::AssetMutate, true);
				D.Validate = &FKismetAIValidation::ValidateInsertNode;
				D.bIsMutation = true;
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("search_assets"), EToolCapability::Inspect, true);
				D.Validate = &FKismetAIValidation::ValidateSearchAssets;
				D.bIsIdempotentRead = true;
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("read_source"), EToolCapability::Inspect, true);
				D.Validate = &FKismetAIValidation::ValidateReadSource;
				D.bIsIdempotentRead = true;
				D.MaxResultChars = KismetAISourceContract::MaxSerializedResultBytes;
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("read_logs"), EToolCapability::Inspect, true);
				D.Validate = &FKismetAIValidation::ValidateReadLogs;
				D.MaxResultChars = 15000;
			}
			// In-process: these touch agent run state, not the editor.
			B.Add(TEXT("enter_plan_mode"), EToolCapability::AgentState, true).bIsInProcess = true;
			B.Add(TEXT("exit_plan_mode"),  EToolCapability::AgentState, true).bIsInProcess = true;
			B.Add(TEXT("ask_user"),        EToolCapability::AgentState, true).bIsInProcess = true;
			B.Add(TEXT("update_todos"),    EToolCapability::AgentState, true).bIsInProcess = true;
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("discover_tools"), EToolCapability::AgentState, true);
				D.bIsInProcess = true;
				D.bIsIdempotentRead = true;
				D.MaxResultChars = 5000;
				D.BudgetHint = ENextTurnBudget::High;   // discovered -> next turn acts
			}

			// ── extended (tools_extended.json) ─────────────────────────────
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("manage_blueprint"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateManageBlueprint;
				D.bIsMutation = true;
				D.MaxResultChars = 15000;
				D.BudgetHint = ENextTurnBudget::High;
				D.DiscoveryCategories = { TEXT("blueprint_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("manage_components"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateManageComponents;
				D.bIsMutation = true;
				D.MaxResultChars = 10000;
				D.BudgetHint = ENextTurnBudget::High;
				D.DiscoveryCategories = { TEXT("blueprint_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("manage_state_machine"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateStateMachine;
				D.bIsMutation = true;
				D.MaxResultChars = 15000;
				D.BudgetHint = ENextTurnBudget::High;
				D.DiscoveryCategories = { TEXT("blueprint_tools") };
			}
			{
				// Lives in tools_extended.json but is advertised under
				// project_tools — a mismatch that predates this registry and is
				// preserved here rather than silently corrected.
				FKismetAIToolDefinition& D = B.Add(TEXT("manage_animation"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateAnimation;
				D.bIsMutation = true;
				D.DiscoveryCategories = { TEXT("project_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("layout_graph"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateLayoutGraph;
				D.DiscoveryCategories = { TEXT("blueprint_tools") };
			}

			// ── project (tools_project.json) ───────────────────────────────
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("get_references"), EToolCapability::Inspect, false);
				D.Validate = &FKismetAIValidation::ValidateGetReferences;
				D.bIsIdempotentRead = true;
				D.DiscoveryCategories = { TEXT("project_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("create_asset"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateCreateAsset;
				D.bIsMutation = true;
				D.DiscoveryCategories = { TEXT("project_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("manage_asset"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateManageAsset;
				D.bIsMutation = true;
				D.DiscoveryCategories = { TEXT("project_tools") };
			}

			// ── discovery (tools_discovery.json) ───────────────────────────
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("inspect_class"), EToolCapability::Inspect, false);
				D.Validate = &FKismetAIValidation::ValidateInspectClass;
				D.bIsIdempotentRead = true;
				D.DiscoveryCategories = { TEXT("discovery_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("search_nodes"), EToolCapability::Inspect, false);
				D.Validate = &FKismetAIValidation::ValidateSearchNodes;
				D.bIsIdempotentRead = true;
				D.DiscoveryCategories = { TEXT("discovery_tools") };
			}

			// ── material (tools_material.json) ─────────────────────────────
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("read_material"), EToolCapability::Inspect, false);
				D.Validate = &FKismetAIValidation::ValidateReadMaterial;
				D.bIsIdempotentRead = true;
				D.DiscoveryCategories = { TEXT("material_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("create_material_expression"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateCreateMaterialExpression;
				D.DiscoveryCategories = { TEXT("material_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("edit_material"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateEditMaterial;
				D.DiscoveryCategories = { TEXT("material_tools") };
			}
			// AssetMutate for the same reason as compile_blueprint.
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("compile_material"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateCompileMaterial;
				D.DiscoveryCategories = { TEXT("material_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("create_material_instance"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateCreateMaterialInstance;
				D.DiscoveryCategories = { TEXT("material_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("set_material_instance_parameter"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateSetMaterialInstanceParameter;
				D.DiscoveryCategories = { TEXT("material_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("manage_material"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateManageMaterial;
				D.DiscoveryCategories = { TEXT("material_tools") };
			}

			// ── widget (tools_widget.json) ─────────────────────────────────
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("read_widget"), EToolCapability::Inspect, false);
				D.Validate = &FKismetAIValidation::ValidateReadWidget;
				D.bIsIdempotentRead = true;
				D.DiscoveryCategories = { TEXT("widget_tools") };
			}
			{
				FKismetAIToolDefinition& D = B.Add(TEXT("edit_widget"), EToolCapability::AssetMutate, false);
				D.Validate = &FKismetAIValidation::ValidateEditWidget;
				D.bIsMutation = true;
				D.DiscoveryCategories = { TEXT("widget_tools") };
			}

			return B.Defs;
		}();
		return Table;
	}
}

const FKismetAIToolDefinition* FKismetAIToolRegistry::Find(const FString& ToolName)
{
	return GetDefs().Find(ToolName);
}

TSet<FString> FKismetAIToolRegistry::GetAllNames()
{
	TArray<FString> Keys;
	GetDefs().GetKeys(Keys);
	return TSet<FString>(Keys);
}

TArray<FString> FKismetAIToolRegistry::GetNamesInCategory(const FString& Category)
{
	TArray<FString> Out;
	for (const TPair<FString, FKismetAIToolDefinition>& Pair : GetDefs())
	{
		if (Pair.Value.DiscoveryCategories.Contains(Category))
		{
			Out.Add(Pair.Key);
		}
	}
	// Stable order: the category list is shown to the model, and a set iteration
	// order that shifts between runs would churn the prompt cache.
	Out.Sort();
	return Out;
}

TSet<FString> FKismetAIToolRegistry::GetCategoryNames()
{
	TSet<FString> Out;
	for (const TPair<FString, FKismetAIToolDefinition>& Pair : GetDefs())
	{
		for (const FString& Cat : Pair.Value.DiscoveryCategories)
		{
			Out.Add(Cat);
		}
	}
	return Out;
}

bool FKismetAIToolRegistry::IsMutation(const FString& ToolName)
{
	const FKismetAIToolDefinition* D = Find(ToolName);
	return D && D->bIsMutation;
}

bool FKismetAIToolRegistry::IsIdempotentRead(const FString& ToolName)
{
	const FKismetAIToolDefinition* D = Find(ToolName);
	return D && D->bIsIdempotentRead;
}

bool FKismetAIToolRegistry::IsAllowedInPlanMode(const FString& ToolName)
{
	const FKismetAIToolDefinition* D = Find(ToolName);
	// An unknown tool is not allowed: plan mode should fail closed.
	return D && D->IsAllowedInPlanMode();
}

ENextTurnBudget FKismetAIToolRegistry::GetBudgetHint(const FString& ToolName)
{
	const FKismetAIToolDefinition* D = Find(ToolName);
	return D ? D->BudgetHint : ENextTurnBudget::Default;
}

int32 FKismetAIToolRegistry::GetMaxResultChars(const FString& ToolName, const FString& Mode, int32 DefaultChars)
{
	// read_blueprint is the one tool whose output size depends on its mode: raw
	// T3D is far larger than a describe, and an overview sits between them.
	if (ToolName == TEXT("read_blueprint"))
	{
		if (Mode == TEXT("t3d"))      { return 30000; }
		if (Mode == TEXT("overview")) { return 20000; }
		return 15000;
	}

	const FKismetAIToolDefinition* D = Find(ToolName);
	return (D && D->MaxResultChars > 0) ? D->MaxResultChars : DefaultChars;
}

const TCHAR* FKismetAIToolRegistry::RevisionTargetField(const FString& ToolName)
{
	static const TSet<FString> BlueprintTools = {
		TEXT("read_blueprint"), TEXT("write_blueprint"), TEXT("edit_blueprint"),
		TEXT("insert_node"), TEXT("layout_graph"), TEXT("compile_blueprint"),
		TEXT("manage_blueprint"), TEXT("manage_components"), TEXT("manage_state_machine")
	};
	if (BlueprintTools.Contains(ToolName)) return TEXT("blueprint");
	if (ToolName == TEXT("read_widget") || ToolName == TEXT("edit_widget")) return TEXT("widget");
	return nullptr;
}

FValidationResult FKismetAIToolRegistry::ValidateArgs(const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
{
	const FKismetAIToolDefinition* D = Find(ToolName);
	if (!D || !D->Validate)
	{
		// In-process tools validate inline against run state; an unknown tool is
		// rejected by dispatch with UNKNOWN_TOOL, which reads better than a
		// validation error.
		return FValidationResult();
	}
	FValidationResult Result = D->Validate(Args);
	if (Args && Args->HasField(TEXT("expected_revision")))
	{
		if (!RevisionTargetField(ToolName)) Result.AddError(TEXT("expected_revision"), TEXT("This tool does not support Blueprint revision preconditions."));
		FString Revision;
		if (!Args->HasTypedField<EJson::String>(TEXT("expected_revision")) || !Args->TryGetStringField(TEXT("expected_revision"), Revision) || Revision.IsEmpty() || Revision.Len() > 256)
			Result.AddError(TEXT("expected_revision"), TEXT("Must be a nonempty revision string of at most 256 characters."));
	}
	return Result;
}

TArray<FString> FKismetAIToolRegistry::GetPlanModeAllowedNames()
{
	TArray<FString> Out;
	for (const TPair<FString, FKismetAIToolDefinition>& Pair : GetDefs())
	{
		if (Pair.Value.IsAllowedInPlanMode())
		{
			Out.Add(Pair.Key);
		}
	}
	Out.Sort();
	return Out;
}
