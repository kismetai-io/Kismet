// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIUtils.h"
#include "EdGraphSchema_K2.h"

constexpr auto PinTypeTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Primitive types
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Bool, "KismetAI.Utils.ParsePinType_Bool", PinTypeTestFlags)
bool FParsePinType_Bool::RunTest(const FString& Parameters)
{
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("bool"));
	TestEqual(TEXT("Category is PC_Boolean"), PinType.PinCategory, UEdGraphSchema_K2::PC_Boolean);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Int, "KismetAI.Utils.ParsePinType_Int", PinTypeTestFlags)
bool FParsePinType_Int::RunTest(const FString& Parameters)
{
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("int"));
	TestEqual(TEXT("Category is PC_Int"), PinType.PinCategory, UEdGraphSchema_K2::PC_Int);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Float, "KismetAI.Utils.ParsePinType_Float", PinTypeTestFlags)
bool FParsePinType_Float::RunTest(const FString& Parameters)
{
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("float"));
	TestEqual(TEXT("Category is PC_Real"), PinType.PinCategory, UEdGraphSchema_K2::PC_Real);
	TestEqual(TEXT("SubCategory is float"), PinType.PinSubCategory, FName(TEXT("float")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Double, "KismetAI.Utils.ParsePinType_Double", PinTypeTestFlags)
bool FParsePinType_Double::RunTest(const FString& Parameters)
{
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("double"));
	TestEqual(TEXT("Category is PC_Real"), PinType.PinCategory, UEdGraphSchema_K2::PC_Real);
	TestEqual(TEXT("SubCategory is double"), PinType.PinSubCategory, FName(TEXT("double")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_String, "KismetAI.Utils.ParsePinType_String", PinTypeTestFlags)
bool FParsePinType_String::RunTest(const FString& Parameters)
{
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("string"));
	TestEqual(TEXT("Category is PC_String"), PinType.PinCategory, UEdGraphSchema_K2::PC_String);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Name, "KismetAI.Utils.ParsePinType_Name", PinTypeTestFlags)
bool FParsePinType_Name::RunTest(const FString& Parameters)
{
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("name"));
	TestEqual(TEXT("Category is PC_Name"), PinType.PinCategory, UEdGraphSchema_K2::PC_Name);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Text, "KismetAI.Utils.ParsePinType_Text", PinTypeTestFlags)
bool FParsePinType_Text::RunTest(const FString& Parameters)
{
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("text"));
	TestEqual(TEXT("Category is PC_Text"), PinType.PinCategory, UEdGraphSchema_K2::PC_Text);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Compound types (object, struct)
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_ObjectShortName, "KismetAI.Utils.ParsePinType_ObjectShortName", PinTypeTestFlags)
bool FParsePinType_ObjectShortName::RunTest(const FString& Parameters)
{
	FString Error;
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("object:Actor"), &Error);
	TestEqual(TEXT("Category is PC_Object"), PinType.PinCategory, UEdGraphSchema_K2::PC_Object);
	TestNotNull(TEXT("SubCategoryObject is non-null"), PinType.PinSubCategoryObject.Get());
	TestTrue(TEXT("No error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_StructPath, "KismetAI.Utils.ParsePinType_StructPath", PinTypeTestFlags)
bool FParsePinType_StructPath::RunTest(const FString& Parameters)
{
	FString Error;
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("struct:/Script/CoreUObject.Vector"), &Error);
	TestEqual(TEXT("Category is PC_Struct"), PinType.PinCategory, UEdGraphSchema_K2::PC_Struct);
	TestNotNull(TEXT("SubCategoryObject is non-null"), PinType.PinSubCategoryObject.Get());
	TestTrue(TEXT("No error"), Error.IsEmpty());
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Error cases
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Invalid, "KismetAI.Utils.ParsePinType_Invalid", PinTypeTestFlags)
bool FParsePinType_Invalid::RunTest(const FString& Parameters)
{
	FString Error;
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT("foobar"), &Error);
	TestTrue(TEXT("Category is empty for invalid type"), PinType.PinCategory == NAME_None);
	TestTrue(TEXT("Error is set"), !Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FParsePinType_Empty, "KismetAI.Utils.ParsePinType_Empty", PinTypeTestFlags)
bool FParsePinType_Empty::RunTest(const FString& Parameters)
{
	FString Error;
	FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TEXT(""), &Error);
	TestTrue(TEXT("Category is empty for empty input"), PinType.PinCategory == NAME_None);
	TestTrue(TEXT("Error is set"), !Error.IsEmpty());
	return true;
}
