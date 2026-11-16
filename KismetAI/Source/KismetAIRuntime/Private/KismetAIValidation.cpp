// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIValidation.h"
#include "KismetAISourceContract.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

FString FValidationResult::ToJsonString() const
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("success"), false);

	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetStringField(TEXT("error_code"), TEXT("VALIDATION_ERROR"));

	// Build message from first error
	if (Errors.Num() > 0)
	{
		ErrorObj->SetStringField(TEXT("message"), Errors[0].Message);
	}

	// Include all errors as suggestions
	TArray<TSharedPtr<FJsonValue>> SuggestionsArr;
	for (const FValidationError& Err : Errors)
	{
		SuggestionsArr.Add(MakeShared<FJsonValueString>(
			FString::Printf(TEXT("Field '%s': %s"), *Err.Field, *Err.Message)));
	}
	ErrorObj->SetArrayField(TEXT("suggestions"), SuggestionsArr);

	Root->SetObjectField(TEXT("error"), ErrorObj);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
	return Output;
}

FValidationResult FKismetAIValidation::ValidateReadBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
	{
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required and must be non-empty"));
	}

	// Validate mode if present
	if (Args->HasField(TEXT("mode")))
	{
		FString Mode = Args->GetStringField(TEXT("mode"));
		if (Mode != TEXT("describe") && Mode != TEXT("t3d") && Mode != TEXT("overview"))
		{
			Result.AddError(TEXT("mode"), TEXT("'mode' must be 'describe', 't3d', or 'overview'"));
		}
	}

	// Validate offset/limit types if present.
	// 0 (and absent) means "use default" — only negative values are invalid.
	// OpenAI/Codex models fill optional integer fields with 0; treat that as unset.
	if (Args->HasField(TEXT("offset")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("offset"), Val) || Val < 0)
		{
			Result.AddError(TEXT("offset"), TEXT("'offset' must be a non-negative integer"));
		}
	}
	if (Args->HasField(TEXT("limit")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("limit"), Val) || Val < 0)
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be a non-negative integer (0 = use default)"));
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateWriteBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
	{
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required and must be non-empty"));
	}

	if (!Args->HasField(TEXT("t3d")) || Args->GetStringField(TEXT("t3d")).IsEmpty())
	{
		Result.AddError(TEXT("t3d"), TEXT("'t3d' is required and must be non-empty"));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateCompileBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
	{
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required and must be non-empty"));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateEditBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
	{
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required and must be non-empty"));
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsArray;
	if (!Args->TryGetArrayField(TEXT("operations"), OpsArray))
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' array is required"));
		return Result;
	}

	if (OpsArray->Num() == 0)
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' array must not be empty"));
	}

	// Validate each operation
	for (int32 i = 0; i < OpsArray->Num(); i++)
	{
		TSharedPtr<FJsonObject> OpObj = (*OpsArray)[i]->AsObject();
		if (!OpObj.IsValid())
		{
			Result.AddError(
				FString::Printf(TEXT("operations[%d]"), i),
				TEXT("operation must be a JSON object"));
			continue;
		}

		FValidationResult OpResult = ValidateEditOperation(OpObj, i);
		if (!OpResult.bIsValid)
		{
			for (const FValidationError& Err : OpResult.Errors)
			{
				Result.AddError(Err.Field, Err.Message);
			}
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateLayoutGraph(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
	{
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required and must be non-empty"));
	}

	// 'nodes' is optional, but when present it must be an array of strings.
	if (Args->HasField(TEXT("nodes")))
	{
		const TArray<TSharedPtr<FJsonValue>>* NodesArray;
		if (!Args->TryGetArrayField(TEXT("nodes"), NodesArray))
		{
			Result.AddError(TEXT("nodes"), TEXT("'nodes' must be an array of node names"));
		}
		else
		{
			for (int32 i = 0; i < NodesArray->Num(); i++)
			{
				FString Name;
				if (!(*NodesArray)[i]->TryGetString(Name) || Name.IsEmpty())
				{
					Result.AddError(
						FString::Printf(TEXT("nodes[%d]"), i),
						TEXT("node name must be a non-empty string"));
				}
			}
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateEditOperation(const TSharedPtr<FJsonObject>& Op, int32 Index)
{
	FValidationResult Result;
	FString Prefix = FString::Printf(TEXT("operations[%d]"), Index);

	if (!Op->HasField(TEXT("action")))
	{
		Result.AddError(Prefix + TEXT(".action"), TEXT("'action' is required"));
		return Result;
	}

	FString Action = Op->GetStringField(TEXT("action"));

	static const TArray<FString> ValidActions = {
		TEXT("connect"), TEXT("disconnect"), TEXT("set_pin_default"),
		TEXT("set_node_property"), TEXT("set_pin_visibility"), TEXT("delete_node")
	};
	if (!ValidActions.Contains(Action))
	{
		Result.AddError(Prefix + TEXT(".action"),
			FString::Printf(TEXT("Unknown action '%s'. Must be one of: connect, disconnect, set_pin_default, set_node_property, set_pin_visibility, delete_node"), *Action));
		return Result;
	}

	if (Action == TEXT("connect") || Action == TEXT("disconnect"))
	{
		if (!Op->HasField(TEXT("source_node")) || Op->GetStringField(TEXT("source_node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".source_node"), TEXT("'source_node' is required for connect/disconnect"));
		if (!Op->HasField(TEXT("source_pin")) || Op->GetStringField(TEXT("source_pin")).IsEmpty())
			Result.AddError(Prefix + TEXT(".source_pin"), TEXT("'source_pin' is required for connect/disconnect"));
		if (!Op->HasField(TEXT("target_node")) || Op->GetStringField(TEXT("target_node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".target_node"), TEXT("'target_node' is required for connect/disconnect"));
		if (!Op->HasField(TEXT("target_pin")) || Op->GetStringField(TEXT("target_pin")).IsEmpty())
			Result.AddError(Prefix + TEXT(".target_pin"), TEXT("'target_pin' is required for connect/disconnect"));
	}
	else if (Action == TEXT("set_pin_default"))
	{
		if (!Op->HasField(TEXT("node")) || Op->GetStringField(TEXT("node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".node"), TEXT("'node' is required for set_pin_default"));
		if (!Op->HasField(TEXT("pin")) || Op->GetStringField(TEXT("pin")).IsEmpty())
			Result.AddError(Prefix + TEXT(".pin"), TEXT("'pin' is required for set_pin_default"));
		if (!Op->HasField(TEXT("value")))
			Result.AddError(Prefix + TEXT(".value"), TEXT("'value' is required for set_pin_default"));
	}
	else if (Action == TEXT("set_node_property"))
	{
		if (!Op->HasField(TEXT("node")) || Op->GetStringField(TEXT("node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".node"), TEXT("'node' is required for set_node_property"));
		if (!Op->HasField(TEXT("property")) || Op->GetStringField(TEXT("property")).IsEmpty())
			Result.AddError(Prefix + TEXT(".property"), TEXT("'property' is required for set_node_property"));
		if (!Op->HasField(TEXT("value")))
			Result.AddError(Prefix + TEXT(".value"), TEXT("'value' is required for set_node_property"));
	}
	else if (Action == TEXT("set_pin_visibility"))
	{
		if (!Op->HasField(TEXT("node")) || Op->GetStringField(TEXT("node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".node"), TEXT("'node' is required for set_pin_visibility"));
		if (!Op->HasField(TEXT("property")) || Op->GetStringField(TEXT("property")).IsEmpty())
			Result.AddError(Prefix + TEXT(".property"), TEXT("'property' is required for set_pin_visibility"));
		if (!Op->HasField(TEXT("visible")))
			Result.AddError(Prefix + TEXT(".visible"), TEXT("'visible' (bool) is required for set_pin_visibility"));
	}
	else if (Action == TEXT("delete_node"))
	{
		if (!Op->HasField(TEXT("node")) || Op->GetStringField(TEXT("node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".node"), TEXT("'node' is required for delete_node"));
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// manage_blueprint validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateManageBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required"));

	if (!Args->HasField(TEXT("operations")))
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' array is required"));
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (!Args->TryGetArrayField(TEXT("operations"), Ops) || !Ops || Ops->Num() == 0)
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' must be a non-empty array"));
		return Result;
	}

	for (int32 i = 0; i < Ops->Num(); ++i)
	{
		auto OpResult = ValidateManageOperation((*Ops)[i]->AsObject(), i);
		if (!OpResult.bIsValid)
		{
			Result.bIsValid = false;
			Result.Errors.Append(OpResult.Errors);
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateManageOperation(const TSharedPtr<FJsonObject>& Op, int32 Index)
{
	FValidationResult Result;
	FString Prefix = FString::Printf(TEXT("operations[%d]"), Index);

	if (!Op)
	{
		Result.AddError(Prefix, TEXT("Operation is null"));
		return Result;
	}

	if (!Op->HasField(TEXT("action")) || Op->GetStringField(TEXT("action")).IsEmpty())
	{
		Result.AddError(Prefix + TEXT(".action"), TEXT("'action' is required"));
		return Result;
	}

	FString Action = Op->GetStringField(TEXT("action"));

	static const TSet<FString> ValidActions = {
		TEXT("add_variable"), TEXT("remove_variable"), TEXT("set_variable_property"),
		TEXT("add_function"), TEXT("remove_function"),
		TEXT("add_function_param"), TEXT("remove_function_param"),
		TEXT("add_macro"), TEXT("remove_macro"),
		TEXT("add_custom_event"),
		TEXT("add_event_dispatcher"), TEXT("remove_event_dispatcher"),
		TEXT("add_interface"), TEXT("remove_interface"),
		TEXT("set_parent_class"), TEXT("set_class_default"), TEXT("get_class_default")
	};

	if (!ValidActions.Contains(Action))
	{
		Result.AddError(Prefix + TEXT(".action"), FString::Printf(TEXT("Unknown action: '%s'"), *Action));
		return Result;
	}

	// Per-action required fields
	if (Action == TEXT("add_variable"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for add_variable"));
		if (!Op->HasField(TEXT("type")) || Op->GetStringField(TEXT("type")).IsEmpty())
			Result.AddError(Prefix + TEXT(".type"), TEXT("'type' is required for add_variable"));
	}
	else if (Action == TEXT("remove_variable") || Action == TEXT("remove_function") || Action == TEXT("remove_macro") || Action == TEXT("remove_event_dispatcher"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), FString::Printf(TEXT("'name' is required for %s"), *Action));
	}
	else if (Action == TEXT("set_variable_property"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for set_variable_property"));
		if (!Op->HasField(TEXT("property")) || Op->GetStringField(TEXT("property")).IsEmpty())
			Result.AddError(Prefix + TEXT(".property"), TEXT("'property' is required for set_variable_property"));
		if (!Op->HasField(TEXT("value")))
			Result.AddError(Prefix + TEXT(".value"), TEXT("'value' is required for set_variable_property"));
	}
	else if (Action == TEXT("add_function") || Action == TEXT("add_macro") || Action == TEXT("add_custom_event") || Action == TEXT("add_event_dispatcher"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), FString::Printf(TEXT("'name' is required for %s"), *Action));
	}
	else if (Action == TEXT("add_function_param"))
	{
		if (!Op->HasField(TEXT("function")) || Op->GetStringField(TEXT("function")).IsEmpty())
			Result.AddError(Prefix + TEXT(".function"), TEXT("'function' is required for add_function_param"));
		if (!Op->HasField(TEXT("param_name")) || Op->GetStringField(TEXT("param_name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".param_name"), TEXT("'param_name' is required for add_function_param"));
		if (!Op->HasField(TEXT("type")) || Op->GetStringField(TEXT("type")).IsEmpty())
			Result.AddError(Prefix + TEXT(".type"), TEXT("'type' is required for add_function_param"));
	}
	else if (Action == TEXT("remove_function_param"))
	{
		if (!Op->HasField(TEXT("function")) || Op->GetStringField(TEXT("function")).IsEmpty())
			Result.AddError(Prefix + TEXT(".function"), TEXT("'function' is required for remove_function_param"));
		if (!Op->HasField(TEXT("param_name")) || Op->GetStringField(TEXT("param_name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".param_name"), TEXT("'param_name' is required for remove_function_param"));
	}
	else if (Action == TEXT("add_interface") || Action == TEXT("remove_interface"))
	{
		if (!Op->HasField(TEXT("interface_path")) || Op->GetStringField(TEXT("interface_path")).IsEmpty())
			Result.AddError(Prefix + TEXT(".interface_path"), FString::Printf(TEXT("'interface_path' is required for %s"), *Action));
	}
	else if (Action == TEXT("set_parent_class"))
	{
		if (!Op->HasField(TEXT("parent_class_path")) || Op->GetStringField(TEXT("parent_class_path")).IsEmpty())
			Result.AddError(Prefix + TEXT(".parent_class_path"), TEXT("'parent_class_path' is required for set_parent_class"));
	}
	else if (Action == TEXT("set_class_default"))
	{
		if (!Op->HasField(TEXT("property")) || Op->GetStringField(TEXT("property")).IsEmpty())
			Result.AddError(Prefix + TEXT(".property"), TEXT("'property' is required for set_class_default"));
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// manage_state_machine validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateStateMachine(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required"));

	if (!Args->HasField(TEXT("operations")))
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' array is required"));
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (!Args->TryGetArrayField(TEXT("operations"), Ops) || !Ops || Ops->Num() == 0)
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' must be a non-empty array"));
		return Result;
	}

	for (int32 i = 0; i < Ops->Num(); ++i)
	{
		auto OpResult = ValidateStateMachineOperation((*Ops)[i]->AsObject(), i);
		if (!OpResult.bIsValid)
		{
			Result.bIsValid = false;
			Result.Errors.Append(OpResult.Errors);
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateStateMachineOperation(const TSharedPtr<FJsonObject>& Op, int32 Index)
{
	FValidationResult Result;
	FString Prefix = FString::Printf(TEXT("operations[%d]"), Index);

	if (!Op)
	{
		Result.AddError(Prefix, TEXT("Operation is null"));
		return Result;
	}

	if (!Op->HasField(TEXT("action")) || Op->GetStringField(TEXT("action")).IsEmpty())
	{
		Result.AddError(Prefix + TEXT(".action"), TEXT("'action' is required"));
		return Result;
	}

	FString Action = Op->GetStringField(TEXT("action"));

	static const TSet<FString> ValidActions = {
		TEXT("get"),
		TEXT("add_state_machine"), TEXT("add_state"), TEXT("add_conduit"), TEXT("add_state_alias"),
		TEXT("add_transition"), TEXT("set_transition_properties"), TEXT("set_entry_state"),
		TEXT("rename_state"), TEXT("delete_state"), TEXT("delete_transition")
	};

	if (!ValidActions.Contains(Action))
	{
		Result.AddError(Prefix + TEXT(".action"), FString::Printf(TEXT("Unknown action: '%s'"), *Action));
		return Result;
	}

	auto RequireString = [&](const TCHAR* Field)
	{
		if (!Op->HasField(Field) || Op->GetStringField(Field).IsEmpty())
			Result.AddError(Prefix + TEXT(".") + Field, FString::Printf(TEXT("'%s' is required for %s"), Field, *Action));
	};

	if (Action == TEXT("add_state_machine") || Action == TEXT("add_state")
		|| Action == TEXT("add_conduit") || Action == TEXT("add_state_alias"))
	{
		RequireString(TEXT("name"));
	}
	else if (Action == TEXT("add_transition"))
	{
		RequireString(TEXT("from"));
		RequireString(TEXT("to"));
	}
	else if (Action == TEXT("set_transition_properties") || Action == TEXT("delete_transition"))
	{
		const bool bHasTransition = Op->HasField(TEXT("transition")) && !Op->GetStringField(TEXT("transition")).IsEmpty();
		const bool bHasFromTo = Op->HasField(TEXT("from")) && !Op->GetStringField(TEXT("from")).IsEmpty()
			&& Op->HasField(TEXT("to")) && !Op->GetStringField(TEXT("to")).IsEmpty();
		if (!bHasTransition && !bHasFromTo)
			Result.AddError(Prefix, FString::Printf(TEXT("%s needs either 'transition' (node name) or both 'from' and 'to'"), *Action));
	}
	else if (Action == TEXT("set_entry_state") || Action == TEXT("delete_state"))
	{
		RequireString(TEXT("state"));
	}
	else if (Action == TEXT("rename_state"))
	{
		RequireString(TEXT("state"));
		RequireString(TEXT("new_name"));
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// search_assets validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateSearchAssets(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	// At least one search criterion required
	bool bHasQuery = Args->HasField(TEXT("query")) && !Args->GetStringField(TEXT("query")).IsEmpty();
	bool bHasClassFilter = Args->HasField(TEXT("class_filter")) && !Args->GetStringField(TEXT("class_filter")).IsEmpty();
	bool bHasPath = Args->HasField(TEXT("path")) && !Args->GetStringField(TEXT("path")).IsEmpty();

	if (!bHasQuery && !bHasClassFilter && !bHasPath)
	{
		Result.AddError(TEXT("search"), TEXT("At least one of 'query', 'class_filter', or 'path' must be provided"));
	}

	// Validate class_filter enum
	if (bHasClassFilter)
	{
		FString ClassFilter = Args->GetStringField(TEXT("class_filter"));
		static const TSet<FString> ValidClasses = {
			TEXT("Blueprint"), TEXT("WidgetBlueprint"), TEXT("AnimBlueprint"),
			TEXT("UserDefinedStruct"), TEXT("UserDefinedEnum"), TEXT("DataTable"),
			TEXT("Material"), TEXT("MaterialInstance"), TEXT("MaterialInstanceConstant"),
			TEXT("Texture2D"), TEXT("StaticMesh"), TEXT("SkeletalMesh"),
			TEXT("SoundWave"), TEXT("World"), TEXT("ParticleSystem"),
			TEXT("NiagaraSystem"), TEXT("NiagaraEmitter"),
			// Animation asset types
			TEXT("AnimSequence"), TEXT("AnimMontage"), TEXT("AnimComposite"),
			TEXT("BlendSpace"), TEXT("BlendSpace1D"),
			TEXT("AimOffsetBlendSpace"), TEXT("AimOffsetBlendSpace1D"),
			TEXT("Skeleton")
		};
		if (!ValidClasses.Contains(ClassFilter))
		{
			Result.AddError(TEXT("class_filter"),
				FString::Printf(TEXT("Unknown class_filter '%s'. Use: Blueprint, WidgetBlueprint, AnimBlueprint, UserDefinedStruct, UserDefinedEnum, DataTable, Material, Texture2D, StaticMesh, SkeletalMesh, SoundWave, World, AnimSequence, AnimMontage, AnimComposite, BlendSpace, BlendSpace1D, AimOffsetBlendSpace, AimOffsetBlendSpace1D, Skeleton"), *ClassFilter));
		}
	}

	// Validate path starts with /Game/
	if (bHasPath)
	{
		FString Path = Args->GetStringField(TEXT("path"));
		if (!Path.StartsWith(TEXT("/Game/")) && !Path.Equals(TEXT("/Game")))
		{
			Result.AddError(TEXT("path"), TEXT("'path' must start with '/Game/'"));
		}
	}

	// Validate limit range (0 = use default; only negatives / over-max are invalid)
	if (Args->HasField(TEXT("limit")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("limit"), Val))
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be a number"));
		}
		else if (Val < 0 || Val > 200)
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be between 0 and 200 (0 = use default)"));
		}
	}

	// parent_class only valid when class_filter is Blueprint or absent
	if (Args->HasField(TEXT("parent_class")) && !Args->GetStringField(TEXT("parent_class")).IsEmpty())
	{
		if (bHasClassFilter)
		{
			FString ClassFilter = Args->GetStringField(TEXT("class_filter"));
			if (ClassFilter != TEXT("Blueprint"))
			{
				Result.AddError(TEXT("parent_class"), TEXT("'parent_class' filter is only valid when class_filter is 'Blueprint'"));
			}
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// search_nodes validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateInsertNode(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
	{
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required and must be non-empty"));
	}

	if (!Args->HasField(TEXT("node")) || Args->GetStringField(TEXT("node")).IsEmpty())
	{
		Result.AddError(TEXT("node"), TEXT("'node' is required and must be non-empty (the node's menu name)"));
	}

	// connect_from / connect_to are OPTIONAL. Inserting unwired (then wiring later with
	// edit_blueprint) is a documented, supported pattern, and the tool schema sends these
	// as objects with empty defaults ({"node":""}) — so an empty 'node' must mean "no
	// wiring on this side", NOT an error. The handler already skips an empty/missing wire
	// gracefully (the node still inserts). Only a partially-specified spec (a 'pin' given
	// with no 'node') is a real mistake worth flagging.
	auto ValidateWireSpec = [&Result, &Args](const TCHAR* Field)
	{
		if (!Args->HasField(Field)) return;
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Args->TryGetObjectField(Field, Obj) || !Obj || !Obj->IsValid())
		{
			// A non-object (e.g. null) means "no wiring" — tolerate it.
			return;
		}
		const FString NodeVal = (*Obj)->HasField(TEXT("node")) ? (*Obj)->GetStringField(TEXT("node")) : FString();
		if (NodeVal.IsEmpty())
		{
			const FString PinVal = (*Obj)->HasField(TEXT("pin")) ? (*Obj)->GetStringField(TEXT("pin")) : FString();
			if (!PinVal.IsEmpty())
			{
				Result.AddError(Field, FString::Printf(TEXT("'%s.pin' was given but '%s.node' is empty — name the existing node to wire, or omit both to insert unwired"), Field, Field));
			}
			// else: empty spec = insert unwired (valid, no error)
		}
	};
	ValidateWireSpec(TEXT("connect_from"));
	ValidateWireSpec(TEXT("connect_to"));

	return Result;
}

FValidationResult FKismetAIValidation::ValidateSearchNodes(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("query")) || Args->GetStringField(TEXT("query")).IsEmpty())
	{
		Result.AddError(TEXT("query"), TEXT("'query' is required and must be non-empty"));
	}

	if (Args->HasField(TEXT("limit")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("limit"), Val))
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be a number"));
		}
		else if (Val < 0 || Val > 100)
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be between 0 and 100 (0 = use default)"));
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// manage_asset validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateManageAsset(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	// action required, must be in enum
	if (!Args->HasField(TEXT("action")) || Args->GetStringField(TEXT("action")).IsEmpty())
	{
		Result.AddError(TEXT("action"), TEXT("'action' is required"));
		return Result;
	}

	FString Action = Args->GetStringField(TEXT("action"));
	static const TSet<FString> ValidActions = {
		TEXT("duplicate"), TEXT("rename"), TEXT("move"), TEXT("delete"), TEXT("save")
	};
	if (!ValidActions.Contains(Action))
	{
		Result.AddError(TEXT("action"), TEXT("'action' must be 'duplicate', 'rename', 'move', 'delete', or 'save'"));
		return Result;
	}

	// asset_path required
	if (!Args->HasField(TEXT("asset_path")) || Args->GetStringField(TEXT("asset_path")).IsEmpty())
	{
		Result.AddError(TEXT("asset_path"), TEXT("'asset_path' is required and must be non-empty"));
	}
	else
	{
		FString AssetPath = Args->GetStringField(TEXT("asset_path"));
		if (!AssetPath.StartsWith(TEXT("/Game/")) && !AssetPath.Equals(TEXT("/Game")))
		{
			Result.AddError(TEXT("asset_path"), TEXT("'asset_path' must start with '/Game/'"));
		}
	}

	// Action-specific required fields
	if (Action == TEXT("duplicate") || Action == TEXT("rename"))
	{
		if (!Args->HasField(TEXT("new_name")) || Args->GetStringField(TEXT("new_name")).IsEmpty())
		{
			Result.AddError(TEXT("new_name"), FString::Printf(TEXT("'new_name' is required for %s"), *Action));
		}
		else
		{
			// Validate new_name format
			FString NewName = Args->GetStringField(TEXT("new_name"));
			bool bValidName = true;
			if (NewName.Len() == 0)
			{
				bValidName = false;
			}
			else
			{
				TCHAR First = NewName[0];
				if (!FChar::IsAlpha(First) && First != TEXT('_'))
					bValidName = false;
				for (int32 i = 1; i < NewName.Len() && bValidName; i++)
				{
					TCHAR Ch = NewName[i];
					if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
						bValidName = false;
				}
			}
			if (!bValidName)
			{
				Result.AddError(TEXT("new_name"), TEXT("'new_name' must start with a letter or underscore and contain only alphanumeric characters or underscores"));
			}
		}
	}

	if (Action == TEXT("move"))
	{
		if (!Args->HasField(TEXT("new_path")) || Args->GetStringField(TEXT("new_path")).IsEmpty())
		{
			Result.AddError(TEXT("new_path"), TEXT("'new_path' is required for move"));
		}
		else
		{
			FString NewPath = Args->GetStringField(TEXT("new_path"));
			if (!NewPath.StartsWith(TEXT("/Game/")) && !NewPath.Equals(TEXT("/Game")))
			{
				Result.AddError(TEXT("new_path"), TEXT("'new_path' must start with '/Game/'"));
			}
		}
	}

	// new_path for duplicate must also be valid if provided
	if (Action == TEXT("duplicate") && Args->HasField(TEXT("new_path")) && !Args->GetStringField(TEXT("new_path")).IsEmpty())
	{
		FString NewPath = Args->GetStringField(TEXT("new_path"));
		if (!NewPath.StartsWith(TEXT("/Game/")) && !NewPath.Equals(TEXT("/Game")))
		{
			Result.AddError(TEXT("new_path"), TEXT("'new_path' must start with '/Game/'"));
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// create_asset validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateCreateAsset(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	// asset_type required, must be in enum
	if (!Args->HasField(TEXT("asset_type")) || Args->GetStringField(TEXT("asset_type")).IsEmpty())
	{
		Result.AddError(TEXT("asset_type"), TEXT("'asset_type' is required"));
	}
	else
	{
		FString AssetType = Args->GetStringField(TEXT("asset_type"));
		static const TSet<FString> ValidAssetTypes = {
			TEXT("Blueprint"), TEXT("WidgetBlueprint"), TEXT("UserDefinedStruct"), TEXT("UserDefinedEnum"),
			TEXT("Material"), TEXT("MaterialFunction"),
			TEXT("AnimBlueprint"), TEXT("AnimMontage"), TEXT("AnimComposite"),
			TEXT("BlendSpace"), TEXT("BlendSpace1D"), TEXT("AimOffsetBlendSpace"), TEXT("AimOffsetBlendSpace1D"),
			TEXT("AnimSequence")
		};
		if (!ValidAssetTypes.Contains(AssetType))
		{
			Result.AddError(TEXT("asset_type"), TEXT("'asset_type' must be one of: Blueprint, WidgetBlueprint, UserDefinedStruct, UserDefinedEnum, Material, MaterialFunction, AnimBlueprint, AnimMontage, AnimComposite, BlendSpace, BlendSpace1D, AimOffsetBlendSpace, AimOffsetBlendSpace1D, AnimSequence"));
		}
	}

	// name required, valid UE name
	if (!Args->HasField(TEXT("name")) || Args->GetStringField(TEXT("name")).IsEmpty())
	{
		Result.AddError(TEXT("name"), TEXT("'name' is required and must be non-empty"));
	}
	else
	{
		FString Name = Args->GetStringField(TEXT("name"));
		// UE asset names: start with letter or underscore, then alphanumeric or underscore
		bool bValidName = true;
		if (Name.Len() == 0)
		{
			bValidName = false;
		}
		else
		{
			TCHAR First = Name[0];
			if (!FChar::IsAlpha(First) && First != TEXT('_'))
			{
				bValidName = false;
			}
			else
			{
				for (int32 i = 1; i < Name.Len(); i++)
				{
					TCHAR Ch = Name[i];
					if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
					{
						bValidName = false;
						break;
					}
				}
			}
		}
		if (!bValidName)
		{
			Result.AddError(TEXT("name"), TEXT("'name' must start with a letter or underscore and contain only alphanumeric characters or underscores"));
		}
	}

	// path required, starts with /Game/
	if (!Args->HasField(TEXT("path")) || Args->GetStringField(TEXT("path")).IsEmpty())
	{
		Result.AddError(TEXT("path"), TEXT("'path' is required and must be non-empty"));
	}
	else
	{
		FString Path = Args->GetStringField(TEXT("path"));
		if (!Path.StartsWith(TEXT("/Game/")) && !Path.Equals(TEXT("/Game")))
		{
			Result.AddError(TEXT("path"), TEXT("'path' must start with '/Game/'"));
		}
	}

	// Type-specific validation
	FString AssetType = Args->HasField(TEXT("asset_type")) ? Args->GetStringField(TEXT("asset_type")) : TEXT("");

	if (AssetType == TEXT("Blueprint"))
	{
		// blueprint_type enum validation
		if (Args->HasField(TEXT("blueprint_type")))
		{
			FString BPType = Args->GetStringField(TEXT("blueprint_type"));
			if (BPType != TEXT("normal") && BPType != TEXT("interface") && BPType != TEXT("function_library") && BPType != TEXT("macro_library"))
			{
				Result.AddError(TEXT("blueprint_type"), TEXT("'blueprint_type' must be 'normal', 'interface', 'function_library', or 'macro_library'"));
			}
		}
	}
	else if (AssetType == TEXT("UserDefinedStruct"))
	{
		// fields validation
		const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
		if (Args->TryGetArrayField(TEXT("fields"), FieldsArr) && FieldsArr)
		{
			TSet<FString> FieldNames;
			for (int32 i = 0; i < FieldsArr->Num(); i++)
			{
				auto FieldObj = (*FieldsArr)[i]->AsObject();
				FString Prefix = FString::Printf(TEXT("fields[%d]"), i);

				if (!FieldObj)
				{
					Result.AddError(Prefix, TEXT("Field must be a JSON object"));
					continue;
				}

				if (!FieldObj->HasField(TEXT("name")) || FieldObj->GetStringField(TEXT("name")).IsEmpty())
				{
					Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for each field"));
				}
				else
				{
					FString FieldName = FieldObj->GetStringField(TEXT("name"));
					if (FieldNames.Contains(FieldName))
					{
						Result.AddError(Prefix + TEXT(".name"), FString::Printf(TEXT("Duplicate field name: '%s'"), *FieldName));
					}
					FieldNames.Add(FieldName);
				}

				if (!FieldObj->HasField(TEXT("type")) || FieldObj->GetStringField(TEXT("type")).IsEmpty())
				{
					Result.AddError(Prefix + TEXT(".type"), TEXT("'type' is required for each field"));
				}
			}
		}
	}
	else if (AssetType == TEXT("UserDefinedEnum"))
	{
		// entries validation
		const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
		if (Args->TryGetArrayField(TEXT("entries"), EntriesArr) && EntriesArr)
		{
			TSet<FString> EntryNames;
			for (int32 i = 0; i < EntriesArr->Num(); i++)
			{
				FString Entry = (*EntriesArr)[i]->AsString();
				if (Entry.IsEmpty())
				{
					Result.AddError(FString::Printf(TEXT("entries[%d]"), i), TEXT("Entry must be a non-empty string"));
				}
				else if (EntryNames.Contains(Entry))
				{
					Result.AddError(FString::Printf(TEXT("entries[%d]"), i), FString::Printf(TEXT("Duplicate entry: '%s'"), *Entry));
				}
				EntryNames.Add(Entry);
			}
		}
	}
	else
	{
		// Animation assets — all require a 'skeleton' path except a template Anim Blueprint.
		static const TSet<FString> SkeletonRequiredTypes = {
			TEXT("AnimMontage"), TEXT("AnimComposite"), TEXT("BlendSpace"), TEXT("BlendSpace1D"),
			TEXT("AimOffsetBlendSpace"), TEXT("AimOffsetBlendSpace1D"), TEXT("AnimSequence")
		};
		const bool bHasSkeleton = Args->HasField(TEXT("skeleton")) && !Args->GetStringField(TEXT("skeleton")).IsEmpty();
		if (AssetType == TEXT("AnimBlueprint"))
		{
			const bool bTemplate = Args->HasField(TEXT("template")) && Args->GetBoolField(TEXT("template"));
			if (!bTemplate && !bHasSkeleton)
			{
				Result.AddError(TEXT("skeleton"), TEXT("'skeleton' (path to a USkeleton) is required for AnimBlueprint unless 'template' is true"));
			}
		}
		else if (SkeletonRequiredTypes.Contains(AssetType) && !bHasSkeleton)
		{
			Result.AddError(TEXT("skeleton"), FString::Printf(TEXT("'skeleton' (path to a USkeleton) is required for %s"), *AssetType));
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// manage_animation validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateAnimation(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("asset")) || Args->GetStringField(TEXT("asset")).IsEmpty())
		Result.AddError(TEXT("asset"), TEXT("'asset' (path to an animation asset) is required"));

	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (!Args->TryGetArrayField(TEXT("operations"), Ops) || !Ops || Ops->Num() == 0)
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' must be a non-empty array"));
		return Result;
	}

	for (int32 i = 0; i < Ops->Num(); ++i)
	{
		auto OpResult = ValidateAnimationOperation((*Ops)[i]->AsObject(), i);
		if (!OpResult.bIsValid)
		{
			Result.bIsValid = false;
			Result.Errors.Append(OpResult.Errors);
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateAnimationOperation(const TSharedPtr<FJsonObject>& Op, int32 Index)
{
	FValidationResult Result;
	const FString Prefix = FString::Printf(TEXT("operations[%d]"), Index);

	if (!Op)
	{
		Result.AddError(Prefix, TEXT("Operation is null"));
		return Result;
	}

	if (!Op->HasField(TEXT("action")) || Op->GetStringField(TEXT("action")).IsEmpty())
	{
		Result.AddError(Prefix + TEXT(".action"), TEXT("'action' is required"));
		return Result;
	}

	const FString Action = Op->GetStringField(TEXT("action"));
	static const TSet<FString> ValidActions = {
		TEXT("get"),
		TEXT("add_slot_track"), TEXT("add_segment"), TEXT("remove_segment"),
		TEXT("add_section"), TEXT("set_section_next"), TEXT("remove_section"), TEXT("set_sync_group"),
		TEXT("add_notify"), TEXT("remove_notify"),
		TEXT("set_axis"), TEXT("add_sample"), TEXT("edit_sample"), TEXT("remove_sample")
	};
	if (!ValidActions.Contains(Action))
	{
		Result.AddError(Prefix + TEXT(".action"), FString::Printf(TEXT("Unknown action: '%s'"), *Action));
		return Result;
	}

	auto RequireString = [&](const TCHAR* Field)
	{
		if (!Op->HasField(Field) || Op->GetStringField(Field).IsEmpty())
			Result.AddError(Prefix + TEXT(".") + Field, FString::Printf(TEXT("'%s' is required for %s"), Field, *Action));
	};
	auto RequireNumber = [&](const TCHAR* Field)
	{
		double Dummy;
		if (!Op->TryGetNumberField(Field, Dummy))
			Result.AddError(Prefix + TEXT(".") + Field, FString::Printf(TEXT("'%s' (number) is required for %s"), Field, *Action));
	};

	if (Action == TEXT("add_slot_track"))
	{
		RequireString(TEXT("slot_name"));
	}
	else if (Action == TEXT("add_segment"))
	{
		RequireString(TEXT("animation"));
	}
	else if (Action == TEXT("remove_segment"))
	{
		RequireString(TEXT("slot"));
		RequireNumber(TEXT("index"));
	}
	else if (Action == TEXT("add_section"))
	{
		RequireString(TEXT("name"));
		RequireNumber(TEXT("start_time"));
	}
	else if (Action == TEXT("set_section_next"))
	{
		RequireString(TEXT("section"));
		// next_section may be null/"" to clear — presence not required.
	}
	else if (Action == TEXT("remove_section"))
	{
		RequireString(TEXT("name"));
	}
	else if (Action == TEXT("set_sync_group"))
	{
		RequireString(TEXT("group"));
	}
	else if (Action == TEXT("add_notify"))
	{
		RequireNumber(TEXT("time"));
		const bool bHasName = Op->HasField(TEXT("name")) && !Op->GetStringField(TEXT("name")).IsEmpty();
		const bool bHasClass = Op->HasField(TEXT("notify_class")) && !Op->GetStringField(TEXT("notify_class")).IsEmpty();
		if (!bHasName && !bHasClass)
			Result.AddError(Prefix, TEXT("add_notify needs either 'name' (named/skeleton notify) or 'notify_class' (class-based notify)"));
	}
	else if (Action == TEXT("remove_notify"))
	{
		const bool bHasName = Op->HasField(TEXT("name")) && !Op->GetStringField(TEXT("name")).IsEmpty();
		double Dummy;
		const bool bHasIndex = Op->TryGetNumberField(TEXT("index"), Dummy);
		if (!bHasName && !bHasIndex)
			Result.AddError(Prefix, TEXT("remove_notify needs either 'index' or 'name'"));
	}
	else if (Action == TEXT("set_axis"))
	{
		if (!Op->HasField(TEXT("axis")) || (Op->GetStringField(TEXT("axis")) != TEXT("x") && Op->GetStringField(TEXT("axis")) != TEXT("y")))
			Result.AddError(Prefix + TEXT(".axis"), TEXT("'axis' must be 'x' or 'y'"));
		RequireString(TEXT("name"));
		RequireNumber(TEXT("min"));
		RequireNumber(TEXT("max"));
	}
	else if (Action == TEXT("add_sample"))
	{
		RequireString(TEXT("animation"));
		RequireNumber(TEXT("x"));
	}
	else if (Action == TEXT("edit_sample"))
	{
		RequireNumber(TEXT("index"));
		RequireNumber(TEXT("x"));
	}
	else if (Action == TEXT("remove_sample"))
	{
		RequireNumber(TEXT("index"));
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// inspect_class validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateInspectClass(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("class_name")) || Args->GetStringField(TEXT("class_name")).IsEmpty())
	{
		Result.AddError(TEXT("class_name"), TEXT("'class_name' is required and must be non-empty"));
	}

	// Validate filter enum
	if (Args->HasField(TEXT("filter")))
	{
		FString Filter = Args->GetStringField(TEXT("filter"));
		if (Filter != TEXT("all") && Filter != TEXT("properties") && Filter != TEXT("functions"))
		{
			Result.AddError(TEXT("filter"), TEXT("'filter' must be 'all', 'properties', or 'functions'"));
		}
	}

	// Validate limit range (0 = use default; only negatives / over-max are invalid)
	if (Args->HasField(TEXT("limit")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("limit"), Val))
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be a number"));
		}
		else if (Val < 0 || Val > 100)
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be between 0 and 100 (0 = use default)"));
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// get_references validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateGetReferences(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("asset_path")) || Args->GetStringField(TEXT("asset_path")).IsEmpty())
	{
		Result.AddError(TEXT("asset_path"), TEXT("'asset_path' is required and must be non-empty"));
	}
	else
	{
		FString AssetPath = Args->GetStringField(TEXT("asset_path"));
		if (!AssetPath.StartsWith(TEXT("/Game/")) && !AssetPath.Equals(TEXT("/Game")))
		{
			Result.AddError(TEXT("asset_path"), TEXT("'asset_path' must start with '/Game/'"));
		}
	}

	// Validate direction enum
	if (Args->HasField(TEXT("direction")))
	{
		FString Direction = Args->GetStringField(TEXT("direction"));
		if (Direction != TEXT("dependencies") && Direction != TEXT("referencers") && Direction != TEXT("both"))
		{
			Result.AddError(TEXT("direction"), TEXT("'direction' must be 'dependencies', 'referencers', or 'both'"));
		}
	}

	// Validate limit range (0 = use default; only negatives / over-max are invalid)
	if (Args->HasField(TEXT("limit")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("limit"), Val))
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be a number"));
		}
		else if (Val < 0 || Val > 200)
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be between 0 and 200 (0 = use default)"));
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// manage_components validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateManageComponents(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args->HasField(TEXT("blueprint")) || Args->GetStringField(TEXT("blueprint")).IsEmpty())
		Result.AddError(TEXT("blueprint"), TEXT("'blueprint' is required"));

	if (!Args->HasField(TEXT("operations")))
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' array is required"));
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
	if (!Args->TryGetArrayField(TEXT("operations"), Ops) || !Ops || Ops->Num() == 0)
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' must be a non-empty array"));
		return Result;
	}

	for (int32 i = 0; i < Ops->Num(); ++i)
	{
		auto OpResult = ValidateComponentOperation((*Ops)[i]->AsObject(), i);
		if (!OpResult.bIsValid)
		{
			Result.bIsValid = false;
			Result.Errors.Append(OpResult.Errors);
		}
	}

	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// read_source validation
// ─────────────────────────────────────────────────────────────────────────────

FValidationResult FKismetAIValidation::ValidateReadLogs(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;
	// Every field is optional; only the enumerated 'level' is constrained.
	if (Args.IsValid() && Args->HasField(TEXT("level")))
	{
		const FString Level = Args->GetStringField(TEXT("level")).ToLower();
		if (Level != TEXT("error") && Level != TEXT("warning") && Level != TEXT("display")
			&& Level != TEXT("all") && Level != TEXT("verbose"))
		{
			Result.AddError(TEXT("level"), TEXT("'level' must be one of: error, warning, display, all, verbose"));
		}
	}
	return Result;
}

FValidationResult FKismetAIValidation::ValidateReadSource(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	FString Mode = Args->HasField(TEXT("mode")) ? Args->GetStringField(TEXT("mode")) : TEXT("outline");

	if (Mode != TEXT("find") && Mode != TEXT("outline") && Mode != TEXT("raw"))
	{
		Result.AddError(TEXT("mode"), TEXT("'mode' must be 'find', 'outline', or 'raw'"));
		return Result;
	}

	if (Mode == TEXT("find"))
	{
		if (!Args->HasField(TEXT("pattern")) || Args->GetStringField(TEXT("pattern")).IsEmpty())
		{
			Result.AddError(TEXT("pattern"), TEXT("'pattern' is required for find mode"));
		}
		else
		{
			const FString Error = KismetAISourceContract::PatternError(Args->GetStringField(TEXT("pattern")));
			if (!Error.IsEmpty()) Result.AddError(TEXT("pattern"), Error);
		}
	}
	else
	{
		if (!Args->HasField(TEXT("file")) || Args->GetStringField(TEXT("file")).IsEmpty())
		{
			Result.AddError(TEXT("file"), TEXT("'file' is required for outline/raw modes"));
		}
		else
		{
			FString File = Args->GetStringField(TEXT("file"));
			if (File.Contains(TEXT("..")))
			{
				Result.AddError(TEXT("file"), TEXT("'file' must not contain '..'"));
			}
		}
	}

	if (Args->HasField(TEXT("scope")))
	{
		FString Scope = Args->GetStringField(TEXT("scope"));
		if (Scope != TEXT("project") && Scope != TEXT("engine") && Scope != TEXT("plugins"))
		{
			Result.AddError(TEXT("scope"), TEXT("'scope' must be 'project', 'engine', or 'plugins'"));
		}
	}

	if (Args->HasField(TEXT("limit")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("limit"), Val))
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be a number"));
		}
		else if (Val < 0 || Val > 500)
		{
			Result.AddError(TEXT("limit"), TEXT("'limit' must be between 0 and 500 (0 = use default)"));
		}
	}

	if (Args->HasField(TEXT("offset")))
	{
		double Val;
		if (!Args->TryGetNumberField(TEXT("offset"), Val))
		{
			Result.AddError(TEXT("offset"), TEXT("'offset' must be a number"));
		}
		else if (Val < 0)
		{
			Result.AddError(TEXT("offset"), TEXT("'offset' must be non-negative"));
		}
	}

	for (const auto& Field : {TPair<const TCHAR*, int32>(TEXT("file"), 4096), TPair<const TCHAR*, int32>(TEXT("query"), 256)})
	{
		FString Value;
		if (Args->HasField(Field.Key) && (!Args->TryGetStringField(Field.Key, Value) || Value.Len() > Field.Value))
			Result.AddError(Field.Key, TEXT("Source path/query must be a bounded string (path4096, query256 characters)."));
	}
	for (const TCHAR* Field : {TEXT("cursor"), TEXT("request_id")})
	{
		if (Args->HasField(Field))
		{
			FString Value;
			if (!Args->TryGetStringField(Field, Value) || Value.Len() > 128)
				Result.AddError(Field, TEXT("Must be a string of at most 128 characters."));
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateComponentOperation(const TSharedPtr<FJsonObject>& Op, int32 Index)
{
	FValidationResult Result;
	FString Prefix = FString::Printf(TEXT("operations[%d]"), Index);

	if (!Op)
	{
		Result.AddError(Prefix, TEXT("Operation is null"));
		return Result;
	}

	if (!Op->HasField(TEXT("action")) || Op->GetStringField(TEXT("action")).IsEmpty())
	{
		Result.AddError(Prefix + TEXT(".action"), TEXT("'action' is required"));
		return Result;
	}

	FString Action = Op->GetStringField(TEXT("action"));

	static const TSet<FString> ValidActions = {
		TEXT("add_component"), TEXT("remove_component"),
		TEXT("set_component_property"), TEXT("get_component_property"),
		TEXT("get_animation"), TEXT("set_animation"),
		TEXT("reparent_component"),
		TEXT("set_collision"), TEXT("set_material"),
		TEXT("set_post_process_material")
	};

	if (!ValidActions.Contains(Action))
	{
		Result.AddError(Prefix + TEXT(".action"), FString::Printf(TEXT("Unknown action: '%s'"), *Action));
		return Result;
	}

	if (Action == TEXT("add_component"))
	{
		if (!Op->HasField(TEXT("class")) || Op->GetStringField(TEXT("class")).IsEmpty())
			Result.AddError(Prefix + TEXT(".class"), TEXT("'class' is required for add_component"));
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for add_component"));
	}
	else if (Action == TEXT("remove_component"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for remove_component"));
	}
	else if (Action == TEXT("set_component_property"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for set_component_property"));
		if (!Op->HasField(TEXT("property")) || Op->GetStringField(TEXT("property")).IsEmpty())
			Result.AddError(Prefix + TEXT(".property"), TEXT("'property' is required for set_component_property"));
		if (!Op->HasField(TEXT("value")))
			Result.AddError(Prefix + TEXT(".value"), TEXT("'value' is required for set_component_property"));
	}
	else if (Action == TEXT("get_component_property"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for get_component_property"));
	}
	else if (Action == TEXT("get_animation"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for get_animation"));
	}
	else if (Action == TEXT("set_animation"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for set_animation"));
		if (!Op->HasField(TEXT("anim_class")) && !Op->HasField(TEXT("animation_mode")) && !Op->HasField(TEXT("anim_to_play")))
			Result.AddError(Prefix, TEXT("set_animation needs at least one of: anim_class, animation_mode, anim_to_play"));
		if (Op->HasField(TEXT("animation_mode")))
		{
			const FString Mode = Op->GetStringField(TEXT("animation_mode"));
			if (Mode != TEXT("AnimationBlueprint") && Mode != TEXT("AnimationSingleNode") && Mode != TEXT("AnimationCustomMode"))
				Result.AddError(Prefix + TEXT(".animation_mode"), TEXT("'animation_mode' must be AnimationBlueprint, AnimationSingleNode, or AnimationCustomMode"));
		}
	}
	else if (Action == TEXT("reparent_component"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for reparent_component"));
		if (!Op->HasField(TEXT("new_parent")))
			Result.AddError(Prefix + TEXT(".new_parent"), TEXT("'new_parent' is required for reparent_component"));
	}
	else if (Action == TEXT("set_collision"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for set_collision"));
	}
	else if (Action == TEXT("set_material"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for set_material"));
		if (!Op->HasField(TEXT("material")) || Op->GetStringField(TEXT("material")).IsEmpty())
			Result.AddError(Prefix + TEXT(".material"), TEXT("'material' is required for set_material"));
	}
	else if (Action == TEXT("set_post_process_material"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for set_post_process_material"));
		if (!Op->HasField(TEXT("material")) || Op->GetStringField(TEXT("material")).IsEmpty())
			Result.AddError(Prefix + TEXT(".material"), TEXT("'material' is required for set_post_process_material"));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateReadMaterial(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("material")) || Args->GetStringField(TEXT("material")).IsEmpty())
	{
		Result.AddError(TEXT("material"), TEXT("'material' is required and must be non-empty"));
	}

	if (Args->HasField(TEXT("mode")))
	{
		const FString Mode = Args->GetStringField(TEXT("mode"));
		if (Mode != TEXT("describe") && Mode != TEXT("overview"))
		{
			Result.AddError(TEXT("mode"), TEXT("'mode' must be 'describe' or 'overview'"));
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateCompileMaterial(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("material")) || Args->GetStringField(TEXT("material")).IsEmpty())
	{
		Result.AddError(TEXT("material"), TEXT("'material' is required and must be non-empty"));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateCreateMaterialExpression(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("material")) || Args->GetStringField(TEXT("material")).IsEmpty())
	{
		Result.AddError(TEXT("material"), TEXT("'material' is required and must be non-empty"));
	}
	if (!Args->HasField(TEXT("node")) || Args->GetStringField(TEXT("node")).IsEmpty())
	{
		Result.AddError(TEXT("node"), TEXT("'node' (expression type, e.g. \"Multiply\") is required"));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateMaterialEditOperation(const TSharedPtr<FJsonObject>& Op, int32 Index)
{
	FValidationResult Result;
	const FString Prefix = FString::Printf(TEXT("operations[%d]"), Index);

	if (!Op.IsValid())
	{
		Result.AddError(Prefix, TEXT("operation must be an object"));
		return Result;
	}

	const FString Action = Op->HasField(TEXT("action")) ? Op->GetStringField(TEXT("action")) : FString();
	if (Action.IsEmpty())
	{
		Result.AddError(Prefix + TEXT(".action"), TEXT("'action' is required"));
		return Result;
	}

	if (Action == TEXT("connect"))
	{
		if (!Op->HasField(TEXT("from_node")) || Op->GetStringField(TEXT("from_node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".from_node"), TEXT("'from_node' is required for connect"));
		const bool bToProp = Op->HasField(TEXT("to_property")) && !Op->GetStringField(TEXT("to_property")).IsEmpty();
		const bool bToNode = Op->HasField(TEXT("to_node")) && !Op->GetStringField(TEXT("to_node")).IsEmpty();
		if (!bToProp && !bToNode)
			Result.AddError(Prefix, TEXT("connect needs either 'to_node' (+'to_input') or 'to_property'"));
	}
	else if (Action == TEXT("disconnect"))
	{
		const bool bToProp = Op->HasField(TEXT("to_property")) && !Op->GetStringField(TEXT("to_property")).IsEmpty();
		const bool bToNode = Op->HasField(TEXT("to_node")) && !Op->GetStringField(TEXT("to_node")).IsEmpty();
		if (!bToProp && !bToNode)
			Result.AddError(Prefix, TEXT("disconnect needs either 'to_node' (+'to_input') or 'to_property'"));
	}
	else if (Action == TEXT("set_property"))
	{
		if (!Op->HasField(TEXT("node")) || Op->GetStringField(TEXT("node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".node"), TEXT("'node' is required for set_property"));
		if (!Op->HasField(TEXT("property")) || Op->GetStringField(TEXT("property")).IsEmpty())
			Result.AddError(Prefix + TEXT(".property"), TEXT("'property' is required for set_property"));
		if (!Op->HasField(TEXT("value")))
			Result.AddError(Prefix + TEXT(".value"), TEXT("'value' is required for set_property"));
	}
	else if (Action == TEXT("delete_node"))
	{
		if (!Op->HasField(TEXT("node")) || Op->GetStringField(TEXT("node")).IsEmpty())
			Result.AddError(Prefix + TEXT(".node"), TEXT("'node' is required for delete_node"));
	}
	else
	{
		Result.AddError(Prefix + TEXT(".action"),
			FString::Printf(TEXT("unknown action '%s' (expected connect, disconnect, set_property, delete_node)"), *Action));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateEditMaterial(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("material")) || Args->GetStringField(TEXT("material")).IsEmpty())
	{
		Result.AddError(TEXT("material"), TEXT("'material' is required and must be non-empty"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Operations;
	if (!Args->TryGetArrayField(TEXT("operations"), Operations) || Operations->Num() == 0)
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' must be a non-empty array"));
		return Result;
	}

	for (int32 i = 0; i < Operations->Num(); ++i)
	{
		FValidationResult OpResult = ValidateMaterialEditOperation((*Operations)[i]->AsObject(), i);
		if (!OpResult.bIsValid)
		{
			Result.bIsValid = false;
			Result.Errors.Append(OpResult.Errors);
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateCreateMaterialInstance(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("name")) || Args->GetStringField(TEXT("name")).IsEmpty())
		Result.AddError(TEXT("name"), TEXT("'name' is required and must be non-empty"));
	if (!Args->HasField(TEXT("path")) || Args->GetStringField(TEXT("path")).IsEmpty())
		Result.AddError(TEXT("path"), TEXT("'path' (destination folder under /Game/) is required"));
	else if (!Args->GetStringField(TEXT("path")).StartsWith(TEXT("/Game")))
		Result.AddError(TEXT("path"), TEXT("'path' must be under /Game/"));
	if (!Args->HasField(TEXT("parent")) || Args->GetStringField(TEXT("parent")).IsEmpty())
		Result.AddError(TEXT("parent"), TEXT("'parent' (material to inherit from) is required"));

	return Result;
}

FValidationResult FKismetAIValidation::ValidateSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("instance")) || Args->GetStringField(TEXT("instance")).IsEmpty())
		Result.AddError(TEXT("instance"), TEXT("'instance' is required and must be non-empty"));

	const TArray<TSharedPtr<FJsonValue>>* Params;
	if (!Args->TryGetArrayField(TEXT("parameters"), Params) || Params->Num() == 0)
	{
		Result.AddError(TEXT("parameters"), TEXT("'parameters' must be a non-empty array"));
		return Result;
	}

	for (int32 i = 0; i < Params->Num(); ++i)
	{
		const TSharedPtr<FJsonObject> P = (*Params)[i]->AsObject();
		const FString Prefix = FString::Printf(TEXT("parameters[%d]"), i);
		if (!P.IsValid()) { Result.AddError(Prefix, TEXT("must be an object")); continue; }

		// The classic double wrap — {"parameter": {"type":..,"name":..,"value":..}}.
		// Named precisely here, not only in the handler, so every entry point
		// (agent, MCP, REST) gets the same actionable message.
		if (!P->HasField(TEXT("name")) && !P->HasField(TEXT("value")))
		{
			FString WrapperKey;
			for (const auto& Pair : P->Values)
			{
				const TSharedPtr<FJsonObject>* Inner = nullptr;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Inner) && Inner
					&& ((*Inner)->HasField(TEXT("name")) || (*Inner)->HasField(TEXT("value"))))
				{
					WrapperKey = FString(Pair.Key);
					break;
				}
			}
			if (!WrapperKey.IsEmpty())
			{
				Result.AddError(Prefix, FString::Printf(
					TEXT("entry nests the parameter under '%s' — remove the wrapper; each parameters[] entry must be a flat object {\"type\":\"scalar|vector|texture|switch\",\"name\":\"<param>\",\"value\":<v>}"),
					*WrapperKey));
				continue;
			}
		}

		const FString Type = P->HasField(TEXT("type")) ? P->GetStringField(TEXT("type")) : FString();
		if (Type != TEXT("scalar") && Type != TEXT("vector") && Type != TEXT("texture") && Type != TEXT("switch"))
			Result.AddError(Prefix + TEXT(".type"), TEXT("'type' must be scalar, vector, texture, or switch"));
		if (!P->HasField(TEXT("name")) || P->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required"));
		if (!P->HasField(TEXT("value")))
			Result.AddError(Prefix + TEXT(".value"), TEXT("'value' is required"));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateManageMaterial(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("material")) || Args->GetStringField(TEXT("material")).IsEmpty())
	{
		Result.AddError(TEXT("material"), TEXT("'material' is required and must be non-empty"));
	}

	// At least one setting field must be present.
	if (!Args->HasField(TEXT("blend_mode")) && !Args->HasField(TEXT("shading_model"))
		&& !Args->HasField(TEXT("domain")) && !Args->HasField(TEXT("two_sided")))
	{
		Result.AddError(TEXT("arguments"),
			TEXT("provide at least one setting: blend_mode, shading_model, domain, or two_sided"));
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateReadWidget(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("widget")) || Args->GetStringField(TEXT("widget")).IsEmpty())
	{
		Result.AddError(TEXT("widget"), TEXT("'widget' is required and must be non-empty"));
	}

	if (Args->HasField(TEXT("mode")))
	{
		const FString Mode = Args->GetStringField(TEXT("mode"));
		if (Mode != TEXT("tree") && Mode != TEXT("overview"))
		{
			Result.AddError(TEXT("mode"), TEXT("'mode' must be 'tree' or 'overview'"));
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateEditWidget(const TSharedPtr<FJsonObject>& Args)
{
	FValidationResult Result;

	if (!Args.IsValid())
	{
		Result.AddError(TEXT("arguments"), TEXT("Arguments object is null"));
		return Result;
	}

	if (!Args->HasField(TEXT("widget")) || Args->GetStringField(TEXT("widget")).IsEmpty())
	{
		Result.AddError(TEXT("widget"), TEXT("'widget' is required and must be non-empty"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Args->TryGetArrayField(TEXT("operations"), Operations) || Operations->Num() == 0)
	{
		Result.AddError(TEXT("operations"), TEXT("'operations' must be a non-empty array"));
		return Result;
	}

	for (int32 i = 0; i < Operations->Num(); ++i)
	{
		FValidationResult OpResult = ValidateWidgetEditOperation((*Operations)[i]->AsObject(), i);
		if (!OpResult.bIsValid)
		{
			Result.bIsValid = false;
			Result.Errors.Append(OpResult.Errors);
		}
	}

	return Result;
}

FValidationResult FKismetAIValidation::ValidateWidgetEditOperation(const TSharedPtr<FJsonObject>& Op, int32 Index)
{
	FValidationResult Result;
	const FString Prefix = FString::Printf(TEXT("operations[%d]"), Index);

	if (!Op.IsValid())
	{
		Result.AddError(Prefix, TEXT("operation must be an object"));
		return Result;
	}

	const FString Action = Op->HasField(TEXT("action")) ? Op->GetStringField(TEXT("action")) : FString();
	if (Action.IsEmpty())
	{
		Result.AddError(Prefix + TEXT(".action"), TEXT("'action' is required"));
		return Result;
	}

	if (Action == TEXT("add_widget"))
	{
		if (!Op->HasField(TEXT("class")) || Op->GetStringField(TEXT("class")).IsEmpty())
			Result.AddError(Prefix + TEXT(".class"), TEXT("'class' is required for add_widget"));
	}
	else if (Action == TEXT("remove_widget"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for remove_widget"));
	}
	else if (Action == TEXT("reparent_widget"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for reparent_widget"));
		if (!Op->HasField(TEXT("new_parent")) || Op->GetStringField(TEXT("new_parent")).IsEmpty())
			Result.AddError(Prefix + TEXT(".new_parent"), TEXT("'new_parent' is required for reparent_widget"));
	}
	else if (Action == TEXT("set_slot_property") || Action == TEXT("set_widget_property"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), FString::Printf(TEXT("'name' is required for %s"), *Action));
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Op->TryGetObjectField(TEXT("properties"), Props) || !Props)
			Result.AddError(Prefix + TEXT(".properties"), FString::Printf(TEXT("'properties' object is required for %s"), *Action));
	}
	else if (Action == TEXT("rename_widget"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for rename_widget"));
		if (!Op->HasField(TEXT("new_name")) || Op->GetStringField(TEXT("new_name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".new_name"), TEXT("'new_name' is required for rename_widget"));
	}
	else if (Action == TEXT("replace_widget"))
	{
		if (!Op->HasField(TEXT("name")) || Op->GetStringField(TEXT("name")).IsEmpty())
			Result.AddError(Prefix + TEXT(".name"), TEXT("'name' is required for replace_widget"));
		if (!Op->HasField(TEXT("class")) || Op->GetStringField(TEXT("class")).IsEmpty())
			Result.AddError(Prefix + TEXT(".class"), TEXT("'class' is required for replace_widget"));
	}
	else
	{
		Result.AddError(Prefix + TEXT(".action"),
			FString::Printf(TEXT("unknown action '%s' (expected add_widget, remove_widget, reparent_widget, set_slot_property, set_widget_property, rename_widget, replace_widget)"), *Action));
	}

	return Result;
}
