// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FValidationError
{
	FString Field;
	FString Message;
};

struct KISMETAIRUNTIME_API FValidationResult
{
	bool bIsValid = true;
	TArray<FValidationError> Errors;

	void AddError(const FString& Field, const FString& Message)
	{
		bIsValid = false;
		Errors.Add({Field, Message});
	}

	// Build a structured JSON error string
	FString ToJsonString() const;
};

class KISMETAIRUNTIME_API FKismetAIValidation
{
public:
	// Validate read_blueprint arguments
	static FValidationResult ValidateReadBlueprint(const TSharedPtr<FJsonObject>& Args);

	// Validate write_blueprint arguments
	static FValidationResult ValidateWriteBlueprint(const TSharedPtr<FJsonObject>& Args);

	// Validate compile_blueprint arguments
	static FValidationResult ValidateCompileBlueprint(const TSharedPtr<FJsonObject>& Args);

	// Validate edit_blueprint arguments
	static FValidationResult ValidateEditBlueprint(const TSharedPtr<FJsonObject>& Args);

	// Validate a single edit operation object
	static FValidationResult ValidateEditOperation(const TSharedPtr<FJsonObject>& Op, int32 Index);

	// Validate layout_graph arguments
	static FValidationResult ValidateLayoutGraph(const TSharedPtr<FJsonObject>& Args);

	// Validate manage_blueprint arguments
	static FValidationResult ValidateManageBlueprint(const TSharedPtr<FJsonObject>& Args);

	// Validate a single manage_blueprint operation
	static FValidationResult ValidateManageOperation(const TSharedPtr<FJsonObject>& Op, int32 Index);

	// Validate manage_components arguments
	static FValidationResult ValidateManageComponents(const TSharedPtr<FJsonObject>& Args);

	// Validate manage_state_machine arguments
	static FValidationResult ValidateStateMachine(const TSharedPtr<FJsonObject>& Args);

	// Validate a single manage_state_machine operation
	static FValidationResult ValidateStateMachineOperation(const TSharedPtr<FJsonObject>& Op, int32 Index);

	// Validate search_assets arguments
	static FValidationResult ValidateSearchAssets(const TSharedPtr<FJsonObject>& Args);

	// Validate get_references arguments
	static FValidationResult ValidateGetReferences(const TSharedPtr<FJsonObject>& Args);

	// Validate inspect_class arguments
	static FValidationResult ValidateInspectClass(const TSharedPtr<FJsonObject>& Args);

	// Validate create_asset arguments
	static FValidationResult ValidateCreateAsset(const TSharedPtr<FJsonObject>& Args);

	// Validate manage_asset arguments
	static FValidationResult ValidateManageAsset(const TSharedPtr<FJsonObject>& Args);

	// Validate manage_animation arguments
	static FValidationResult ValidateAnimation(const TSharedPtr<FJsonObject>& Args);

	// Validate a single manage_animation operation
	static FValidationResult ValidateAnimationOperation(const TSharedPtr<FJsonObject>& Op, int32 Index);

	// Validate search_nodes arguments
	static FValidationResult ValidateSearchNodes(const TSharedPtr<FJsonObject>& Args);

	// Validate insert_node arguments
	static FValidationResult ValidateInsertNode(const TSharedPtr<FJsonObject>& Args);

	// Validate read_source arguments
	static FValidationResult ValidateReadSource(const TSharedPtr<FJsonObject>& Args);

	// Validate read_material arguments
	static FValidationResult ValidateReadMaterial(const TSharedPtr<FJsonObject>& Args);

	// Validate compile_material arguments
	static FValidationResult ValidateCompileMaterial(const TSharedPtr<FJsonObject>& Args);

	// Validate create_material_expression arguments
	static FValidationResult ValidateCreateMaterialExpression(const TSharedPtr<FJsonObject>& Args);

	// Validate edit_material arguments
	static FValidationResult ValidateEditMaterial(const TSharedPtr<FJsonObject>& Args);

	// Validate a single edit_material operation object
	static FValidationResult ValidateMaterialEditOperation(const TSharedPtr<FJsonObject>& Op, int32 Index);

	// Validate create_material_instance arguments
	static FValidationResult ValidateCreateMaterialInstance(const TSharedPtr<FJsonObject>& Args);

	// Validate set_material_instance_parameter arguments
	static FValidationResult ValidateSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Args);

	// Validate manage_material arguments
	static FValidationResult ValidateManageMaterial(const TSharedPtr<FJsonObject>& Args);

	// Validate read_widget arguments
	static FValidationResult ValidateReadWidget(const TSharedPtr<FJsonObject>& Args);

	// Validate edit_widget arguments
	static FValidationResult ValidateEditWidget(const TSharedPtr<FJsonObject>& Args);

	// Validate a single edit_widget operation object
	static FValidationResult ValidateWidgetEditOperation(const TSharedPtr<FJsonObject>& Op, int32 Index);

	// Validate read_logs arguments
	static FValidationResult ValidateReadLogs(const TSharedPtr<FJsonObject>& Args);

	// Validate a single manage_components operation
	static FValidationResult ValidateComponentOperation(const TSharedPtr<FJsonObject>& Op, int32 Index);
};
