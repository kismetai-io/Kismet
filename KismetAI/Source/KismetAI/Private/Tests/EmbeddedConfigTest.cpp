// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIEmbeddedConfig.h"
#include "Misc/AutomationTest.h"

constexpr auto KismetAIEmbeddedConfigTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// TryLoadEmbedded must return content for a known config file when the
// payload is compiled in. When the payload is NOT compiled in (development
// build), it must report false without touching OutContent. This guards the
// codegen ↔ decode pairing in KismetAIEmbeddedConfig.cpp / embed_config.py
// against silent breakage (e.g. a hash function drift between the two sides
// would cause every lookup to miss).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmbeddedConfig_TryLoadKnownFile,
	"KismetAI.EmbeddedConfig.TryLoadKnownFile",
	KismetAIEmbeddedConfigTestFlags)

bool FEmbeddedConfig_TryLoadKnownFile::RunTest(const FString& Parameters)
{
	FString Content;
	const bool bOk = KismetAIEmbeddedConfig::TryLoadEmbedded(TEXT("system_prompt.txt"), Content);

	if (KismetAIEmbeddedConfig::IsEmbedded())
	{
		TestTrue(TEXT("embedded build returns true for system_prompt.txt"), bOk);
		TestTrue(TEXT("embedded payload is non-empty"), Content.Len() > 0);
		// The first line of the prompt is "## Identity" — assert it round-trips
		// through UTF-8 loading. If the payload or lookup drifts, this fails.
		TestTrue(
			TEXT("decoded content starts with expected prompt header"),
			Content.StartsWith(TEXT("## Identity")));
	}
	else
	{
		TestFalse(TEXT("non-embedded build returns false (no payload compiled in)"), bOk);
	}
	return true;
}

// Lookup miss must report false without crashing. Catches a regression where
// an unknown name accidentally hashes to a matching entry; the name-hash table
// rejects unknown names cleanly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FEmbeddedConfig_TryLoadMissingFile,
	"KismetAI.EmbeddedConfig.TryLoadMissingFile",
	KismetAIEmbeddedConfigTestFlags)

bool FEmbeddedConfig_TryLoadMissingFile::RunTest(const FString& Parameters)
{
	FString Content;
	const bool bOk = KismetAIEmbeddedConfig::TryLoadEmbedded(
		TEXT("definitely_not_a_real_config_file.txt"), Content);
	TestFalse(TEXT("missing file returns false in all build modes"), bOk);
	return true;
}
