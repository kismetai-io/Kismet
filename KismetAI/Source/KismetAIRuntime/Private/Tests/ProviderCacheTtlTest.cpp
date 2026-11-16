// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Guards CacheTtlSeconds() is the single source of truth for per-provider TTL.
// Any silent change to a provider's TTL constant will fail this test.

#include "KismetAIAnthropicProvider.h"
#include "KismetAIMinimaxProvider.h"
#include "KismetAIOpenAIProvider.h"
#include "Misc/AutomationTest.h"

constexpr auto ProviderCacheTtlTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderCacheTtl_Anthropic,
	"KismetAI.Provider.CacheTtl.AnthropicIs300",
	ProviderCacheTtlTestFlags)
bool FProviderCacheTtl_Anthropic::RunTest(const FString& Parameters)
{
	FAnthropicProvider Provider;
	TestEqual(TEXT("FAnthropicProvider::CacheTtlSeconds() == 300"), Provider.CacheTtlSeconds(), 300);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderCacheTtl_OpenAI,
	"KismetAI.Provider.CacheTtl.OpenAIIs300",
	ProviderCacheTtlTestFlags)
bool FProviderCacheTtl_OpenAI::RunTest(const FString& Parameters)
{
	FOpenAIProvider Provider;
	TestEqual(TEXT("FOpenAIProvider::CacheTtlSeconds() == 300"), Provider.CacheTtlSeconds(), 300);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderCacheTtl_Minimax,
	"KismetAI.Provider.CacheTtl.MinimaxIs300",
	ProviderCacheTtlTestFlags)
bool FProviderCacheTtl_Minimax::RunTest(const FString& Parameters)
{
	FMinimaxProvider Provider;
	TestEqual(TEXT("FMinimaxProvider::CacheTtlSeconds() == 300"), Provider.CacheTtlSeconds(), 300);
	return true;
}
