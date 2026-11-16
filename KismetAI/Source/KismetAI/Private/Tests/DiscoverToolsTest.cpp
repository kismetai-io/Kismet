// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "Misc/AutomationTest.h"
#include "KismetAIAgent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

constexpr auto DiscoverToolsTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace DiscoverToolsTestHelper
{
	/** Build an assistant message containing a discover_tools tool_use block. */
	TSharedPtr<FJsonValue> MakeDiscoverToolsMessage(const FString& ToolId, const FString& ToolName)
	{
		// Build the tool_use content block
		auto Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("tool_name"), ToolName);

		auto ToolUse = MakeShared<FJsonObject>();
		ToolUse->SetStringField(TEXT("type"), TEXT("tool_use"));
		ToolUse->SetStringField(TEXT("id"), ToolId);
		ToolUse->SetStringField(TEXT("name"), TEXT("discover_tools"));
		ToolUse->SetObjectField(TEXT("input"), Input);

		TArray<TSharedPtr<FJsonValue>> Content;
		Content.Add(MakeShared<FJsonValueObject>(ToolUse));

		auto Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("assistant"));
		Msg->SetArrayField(TEXT("content"), Content);

		return MakeShared<FJsonValueObject>(Msg);
	}

	/** Build an assistant message containing a non-discover tool_use block. */
	TSharedPtr<FJsonValue> MakeOtherToolMessage(const FString& ToolId, const FString& ToolName)
	{
		auto Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));

		auto ToolUse = MakeShared<FJsonObject>();
		ToolUse->SetStringField(TEXT("type"), TEXT("tool_use"));
		ToolUse->SetStringField(TEXT("id"), ToolId);
		ToolUse->SetStringField(TEXT("name"), ToolName);
		ToolUse->SetObjectField(TEXT("input"), Input);

		TArray<TSharedPtr<FJsonValue>> Content;
		Content.Add(MakeShared<FJsonValueObject>(ToolUse));

		auto Msg = MakeShared<FJsonObject>();
		Msg->SetStringField(TEXT("role"), TEXT("assistant"));
		Msg->SetArrayField(TEXT("content"), Content);

		return MakeShared<FJsonValueObject>(Msg);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiscoverTools_ExtractEmpty, "KismetAI.ToolDiscovery.ExtractEmpty", DiscoverToolsTestFlags)
bool FDiscoverTools_ExtractEmpty::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	TSet<FString> KnownTools = { TEXT("manage_blueprint"), TEXT("manage_components") };

	TSet<FString> Result = FKismetAIAgent::ExtractDiscoveredToolNames(Messages, KnownTools);
	TestEqual(TEXT("Empty messages returns empty set"), Result.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiscoverTools_ExtractSingleDiscovery, "KismetAI.ToolDiscovery.ExtractSingleDiscovery", DiscoverToolsTestFlags)
bool FDiscoverTools_ExtractSingleDiscovery::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(DiscoverToolsTestHelper::MakeDiscoverToolsMessage(TEXT("toolu_001"), TEXT("manage_blueprint")));

	TSet<FString> KnownTools = { TEXT("manage_blueprint"), TEXT("manage_components") };
	TSet<FString> Result = FKismetAIAgent::ExtractDiscoveredToolNames(Messages, KnownTools);

	TestEqual(TEXT("One tool discovered"), Result.Num(), 1);
	TestTrue(TEXT("manage_blueprint is in the set"), Result.Contains(TEXT("manage_blueprint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiscoverTools_ExtractMultipleDiscoveries, "KismetAI.ToolDiscovery.ExtractMultipleDiscoveries", DiscoverToolsTestFlags)
bool FDiscoverTools_ExtractMultipleDiscoveries::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(DiscoverToolsTestHelper::MakeDiscoverToolsMessage(TEXT("toolu_001"), TEXT("manage_blueprint")));
	Messages.Add(DiscoverToolsTestHelper::MakeDiscoverToolsMessage(TEXT("toolu_002"), TEXT("manage_components")));

	TSet<FString> KnownTools = { TEXT("manage_blueprint"), TEXT("manage_components") };
	TSet<FString> Result = FKismetAIAgent::ExtractDiscoveredToolNames(Messages, KnownTools);

	TestEqual(TEXT("Two tools discovered"), Result.Num(), 2);
	TestTrue(TEXT("manage_blueprint is in the set"), Result.Contains(TEXT("manage_blueprint")));
	TestTrue(TEXT("manage_components is in the set"), Result.Contains(TEXT("manage_components")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiscoverTools_ExtractDuplicateIgnored, "KismetAI.ToolDiscovery.ExtractDuplicateIgnored", DiscoverToolsTestFlags)
bool FDiscoverTools_ExtractDuplicateIgnored::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(DiscoverToolsTestHelper::MakeDiscoverToolsMessage(TEXT("toolu_001"), TEXT("manage_blueprint")));
	Messages.Add(DiscoverToolsTestHelper::MakeDiscoverToolsMessage(TEXT("toolu_002"), TEXT("manage_blueprint")));

	TSet<FString> KnownTools = { TEXT("manage_blueprint"), TEXT("manage_components") };
	TSet<FString> Result = FKismetAIAgent::ExtractDiscoveredToolNames(Messages, KnownTools);

	TestEqual(TEXT("Duplicate yields one entry"), Result.Num(), 1);
	TestTrue(TEXT("manage_blueprint is in the set"), Result.Contains(TEXT("manage_blueprint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDiscoverTools_ExtractIgnoresNonDiscover, "KismetAI.ToolDiscovery.ExtractIgnoresNonDiscover", DiscoverToolsTestFlags)
bool FDiscoverTools_ExtractIgnoresNonDiscover::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Messages;
	Messages.Add(DiscoverToolsTestHelper::MakeOtherToolMessage(TEXT("toolu_001"), TEXT("read_blueprint")));
	Messages.Add(DiscoverToolsTestHelper::MakeOtherToolMessage(TEXT("toolu_002"), TEXT("compile_blueprint")));

	TSet<FString> KnownTools = { TEXT("manage_blueprint"), TEXT("manage_components") };
	TSet<FString> Result = FKismetAIAgent::ExtractDiscoveredToolNames(Messages, KnownTools);

	TestEqual(TEXT("Non-discover tool_use blocks are ignored"), Result.Num(), 0);
	return true;
}
