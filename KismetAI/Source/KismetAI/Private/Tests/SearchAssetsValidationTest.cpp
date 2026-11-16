// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIValidation.h"
#include "KismetAIModule.h"
#include "KismetAIBridge.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto SearchAssetsTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace SearchAssetsTestHelper
{
	TSharedPtr<FJsonObject> MakeArgs()
	{
		return MakeShared<FJsonObject>();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_EmptyInput, "KismetAI.Validation.SearchAssets_EmptyInput", SearchAssetsTestFlags)
bool FSearchAssets_EmptyInput::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestFalse(TEXT("Empty input is invalid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_QueryOnly, "KismetAI.Validation.SearchAssets_QueryOnly", SearchAssetsTestFlags)
bool FSearchAssets_QueryOnly::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("query"), TEXT("Player"));
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestTrue(TEXT("Query-only search is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_ClassFilterOnly, "KismetAI.Validation.SearchAssets_ClassFilterOnly", SearchAssetsTestFlags)
bool FSearchAssets_ClassFilterOnly::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("class_filter"), TEXT("Blueprint"));
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestTrue(TEXT("ClassFilter-only search is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_PathOnly, "KismetAI.Validation.SearchAssets_PathOnly", SearchAssetsTestFlags)
bool FSearchAssets_PathOnly::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("path"), TEXT("/Game/Blueprints"));
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestTrue(TEXT("Path-only search is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_InvalidClassFilter, "KismetAI.Validation.SearchAssets_InvalidClassFilter", SearchAssetsTestFlags)
bool FSearchAssets_InvalidClassFilter::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("class_filter"), TEXT("NotARealClass"));
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestFalse(TEXT("Invalid class_filter is rejected"), R.bIsValid);
	return true;
}

// Animation asset class_filters must be accepted by the allowlist.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_AnimClassFilters, "KismetAI.Validation.SearchAssets_AnimClassFilters", SearchAssetsTestFlags)
bool FSearchAssets_AnimClassFilters::RunTest(const FString& Parameters)
{
	const TArray<FString> AnimClasses = {
		TEXT("AnimSequence"), TEXT("AnimMontage"), TEXT("AnimComposite"),
		TEXT("BlendSpace"), TEXT("BlendSpace1D"),
		TEXT("AimOffsetBlendSpace"), TEXT("AimOffsetBlendSpace1D"),
		TEXT("Skeleton"), TEXT("SkeletalMesh"), TEXT("AnimBlueprint")
	};
	for (const FString& ClassName : AnimClasses)
	{
		auto Args = SearchAssetsTestHelper::MakeArgs();
		Args->SetStringField(TEXT("class_filter"), ClassName);
		FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
		TestTrue(*FString::Printf(TEXT("class_filter '%s' is accepted"), *ClassName), R.bIsValid);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_InvalidPath, "KismetAI.Validation.SearchAssets_InvalidPath", SearchAssetsTestFlags)
bool FSearchAssets_InvalidPath::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("path"), TEXT("/Engine/Content"));
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestFalse(TEXT("Path not starting with /Game/ is rejected"), R.bIsValid);
	return true;
}

// 0 means "use default" — OpenAI/Codex models fill optional ints with 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_LimitZeroAccepted, "KismetAI.Validation.SearchAssets_LimitZeroAccepted", SearchAssetsTestFlags)
bool FSearchAssets_LimitZeroAccepted::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("query"), TEXT("Test"));
	Args->SetNumberField(TEXT("limit"), 0);
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestTrue(TEXT("Limit 0 is accepted (treated as default)"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_NegativeLimitRejected, "KismetAI.Validation.SearchAssets_NegativeLimitRejected", SearchAssetsTestFlags)
bool FSearchAssets_NegativeLimitRejected::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("query"), TEXT("Test"));
	Args->SetNumberField(TEXT("limit"), -1);
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestFalse(TEXT("Negative limit is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_LimitTooHigh, "KismetAI.Validation.SearchAssets_LimitTooHigh", SearchAssetsTestFlags)
bool FSearchAssets_LimitTooHigh::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("query"), TEXT("Test"));
	Args->SetNumberField(TEXT("limit"), 500);
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestFalse(TEXT("Limit 500 is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_ParentClassWithNonBlueprint, "KismetAI.Validation.SearchAssets_ParentClassWithNonBlueprint", SearchAssetsTestFlags)
bool FSearchAssets_ParentClassWithNonBlueprint::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("class_filter"), TEXT("StaticMesh"));
	Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestFalse(TEXT("parent_class with non-Blueprint class_filter is rejected"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_ParentClassWithBlueprint, "KismetAI.Validation.SearchAssets_ParentClassWithBlueprint", SearchAssetsTestFlags)
bool FSearchAssets_ParentClassWithBlueprint::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("class_filter"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("parent_class"), TEXT("Character"));
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestTrue(TEXT("parent_class with Blueprint class_filter is valid"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_FullValid, "KismetAI.Validation.SearchAssets_FullValid", SearchAssetsTestFlags)
bool FSearchAssets_FullValid::RunTest(const FString& Parameters)
{
	auto Args = SearchAssetsTestHelper::MakeArgs();
	Args->SetStringField(TEXT("query"), TEXT("Enemy"));
	Args->SetStringField(TEXT("class_filter"), TEXT("Blueprint"));
	Args->SetStringField(TEXT("path"), TEXT("/Game/Characters"));
	Args->SetStringField(TEXT("parent_class"), TEXT("Character"));
	Args->SetNumberField(TEXT("limit"), 25);
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(Args);
	TestTrue(TEXT("Full valid search passes"), R.bIsValid);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssets_NullArgs, "KismetAI.Validation.SearchAssets_NullArgs", SearchAssetsTestFlags)
bool FSearchAssets_NullArgs::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> NullArgs;
	FValidationResult R = FKismetAIValidation::ValidateSearchAssets(NullArgs);
	TestFalse(TEXT("Null args is invalid"), R.bIsValid);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// QueryAssets + SearchAssets bridge tests (PH1)
//
// These tests verify:
//   - QueryAssets returns an array (possibly empty in headless) without crash
//   - QueryAssets respects the Max cap
//   - SearchAssets bridge returns a JSON array with {path,name,class} shape
//   - Empty query/typefilter returns an array (not null or error string)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueryAssets_ReturnsArray, "KismetAI.Bridge.QueryAssets_ReturnsArray", SearchAssetsTestFlags)
bool FQueryAssets_ReturnsArray::RunTest(const FString& Parameters)
{
	// QueryAssets must return an array (possibly empty in headless) without crashing.
	TArray<FAssetData> Out;
	FKismetAIModule::QueryAssets(TEXT(""), TEXT(""), 50, Out);
	// Result is valid as long as we didn't crash and Out is a TArray.
	TestTrue(TEXT("QueryAssets returns a TArray (may be empty in headless)"), Out.Num() >= 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueryAssets_CapRespected, "KismetAI.Bridge.QueryAssets_CapRespected", SearchAssetsTestFlags)
bool FQueryAssets_CapRespected::RunTest(const FString& Parameters)
{
	// Results must never exceed Max cap.
	TArray<FAssetData> Out;
	FKismetAIModule::QueryAssets(TEXT(""), TEXT(""), 5, Out);
	TestTrue(TEXT("QueryAssets result count <= cap of 5"), Out.Num() <= 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueryAssets_NameSubstringMatch, "KismetAI.Bridge.QueryAssets_NameSubstringMatch", SearchAssetsTestFlags)
bool FQueryAssets_NameSubstringMatch::RunTest(const FString& Parameters)
{
	// With an absurdly specific query, result should be empty (no match expected in headless).
	// The important thing is it does not crash and respects the filter.
	TArray<FAssetData> Out;
	FKismetAIModule::QueryAssets(TEXT("XxThisWillNeverMatchAnyAssetNameXx"), TEXT(""), 50, Out);
	TestEqual(TEXT("No assets match the absurd query"), Out.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueryAssets_TypeFilterBlueprint, "KismetAI.Bridge.QueryAssets_TypeFilterBlueprint", SearchAssetsTestFlags)
bool FQueryAssets_TypeFilterBlueprint::RunTest(const FString& Parameters)
{
	// Filtering by "Blueprint" type must not crash and result must be <= cap.
	TArray<FAssetData> Out;
	FKismetAIModule::QueryAssets(TEXT(""), TEXT("Blueprint"), 10, Out);
	TestTrue(TEXT("Blueprint type filter result count <= cap of 10"), Out.Num() <= 10);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FQueryAssets_UnknownTypeFilterReturnsEmpty, "KismetAI.Bridge.QueryAssets_UnknownTypeFilterReturnsEmpty", SearchAssetsTestFlags)
bool FQueryAssets_UnknownTypeFilterReturnsEmpty::RunTest(const FString& Parameters)
{
	// Unknown TypeFilter should return an empty array (not crash).
	TArray<FAssetData> Out;
	FKismetAIModule::QueryAssets(TEXT(""), TEXT("NotARealClassXyz"), 50, Out);
	TestEqual(TEXT("Unknown TypeFilter yields empty result"), Out.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssetsBridge_ReturnsJsonArray, "KismetAI.Bridge.SearchAssets_ReturnsJsonArray", SearchAssetsTestFlags)
bool FSearchAssetsBridge_ReturnsJsonArray::RunTest(const FString& Parameters)
{
	// SearchAssets must return a valid JSON array string.
	UKismetAIBridge* Bridge = NewObject<UKismetAIBridge>();
	FString Result = Bridge->SearchAssets(TEXT(""), TEXT(""));
	TestFalse(TEXT("SearchAssets result is not empty"), Result.IsEmpty());

	TArray<TSharedPtr<FJsonValue>> Array;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result);
	bool bParsed = FJsonSerializer::Deserialize(Reader, Array);
	TestTrue(TEXT("SearchAssets result parses as JSON array"), bParsed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSearchAssetsBridge_JsonShape, "KismetAI.Bridge.SearchAssets_JsonShape", SearchAssetsTestFlags)
bool FSearchAssetsBridge_JsonShape::RunTest(const FString& Parameters)
{
	// If any results are returned, each item must have path, name, and class fields.
	UKismetAIBridge* Bridge = NewObject<UKismetAIBridge>();
	FString Result = Bridge->SearchAssets(TEXT(""), TEXT(""));

	TArray<TSharedPtr<FJsonValue>> Array;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result);
	FJsonSerializer::Deserialize(Reader, Array);

	for (const TSharedPtr<FJsonValue>& Val : Array)
	{
		const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
		if (!Val->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
		{
			AddError(TEXT("Array element is not an object"));
			return false;
		}
		TSharedPtr<FJsonObject> Obj = *ObjPtr;
		TestTrue(TEXT("Element has 'path' field"),  Obj->HasField(TEXT("path")));
		TestTrue(TEXT("Element has 'name' field"),  Obj->HasField(TEXT("name")));
		TestTrue(TEXT("Element has 'class' field"), Obj->HasField(TEXT("class")));
	}
	return true;
}
