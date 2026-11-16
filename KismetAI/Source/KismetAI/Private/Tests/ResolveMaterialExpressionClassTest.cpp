// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIUtils.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"

constexpr auto MatResolveTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Exact friendly name (prefix stripped) resolves to the concrete class.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveMatExpr_Exact, "KismetAI.Materials.ResolveExpressionClass_Exact", MatResolveTestFlags)
bool FResolveMatExpr_Exact::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Multiply"), KismetAIUtils::ResolveMaterialExpressionClass(TEXT("Multiply"))
		== UMaterialExpressionMultiply::StaticClass());
	TestTrue(TEXT("Constant3Vector"), KismetAIUtils::ResolveMaterialExpressionClass(TEXT("Constant3Vector"))
		== UMaterialExpressionConstant3Vector::StaticClass());
	return true;
}

// The leading "MaterialExpression" prefix is optional, and matching is case-insensitive.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveMatExpr_PrefixAndCase, "KismetAI.Materials.ResolveExpressionClass_PrefixAndCase", MatResolveTestFlags)
bool FResolveMatExpr_PrefixAndCase::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("With prefix"), KismetAIUtils::ResolveMaterialExpressionClass(TEXT("MaterialExpressionTextureSample"))
		== UMaterialExpressionTextureSample::StaticClass());
	TestTrue(TEXT("Lowercase"), KismetAIUtils::ResolveMaterialExpressionClass(TEXT("scalarparameter"))
		== UMaterialExpressionScalarParameter::StaticClass());
	TestTrue(TEXT("Spaces"), KismetAIUtils::ResolveMaterialExpressionClass(TEXT("Texture Sample"))
		== UMaterialExpressionTextureSample::StaticClass());
	return true;
}

// Garbage resolves to null rather than guessing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FResolveMatExpr_Unknown, "KismetAI.Materials.ResolveExpressionClass_Unknown", MatResolveTestFlags)
bool FResolveMatExpr_Unknown::RunTest(const FString& Parameters)
{
	TestNull(TEXT("Nonsense -> null"), KismetAIUtils::ResolveMaterialExpressionClass(TEXT("NotARealNodeXyzzy")));
	TestNull(TEXT("Empty -> null"), KismetAIUtils::ResolveMaterialExpressionClass(TEXT("")));
	return true;
}
