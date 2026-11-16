// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAILLMProvider.h"
#include "Misc/AutomationTest.h"

namespace
{
	void FinishTool(ILLMStreamState& State, bool bOpenAI, const FString& Arguments)
	{
		FString Delta;
		auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("type"), bOpenAI ? TEXT("function_call") : TEXT("tool_use"));
		Item->SetStringField(TEXT("name"), TEXT("enter_plan_mode"));
		Item->SetStringField(bOpenAI ? TEXT("call_id") : TEXT("id"), TEXT("tool1"));
		auto Data = MakeShared<FJsonObject>();
		if (bOpenAI)
		{
			Item->SetStringField(TEXT("arguments"), Arguments);
			Data->SetObjectField(TEXT("item"), Item);
			State.ProcessEvent(TEXT("response.output_item.done"), Data, Delta);
		}
		else
		{
			Item->SetObjectField(TEXT("input"), MakeShared<FJsonObject>());
			Data->SetObjectField(TEXT("content_block"), Item);
			State.ProcessEvent(TEXT("content_block_start"), Data, Delta);
			if (!Arguments.IsEmpty())
			{
				auto Change = MakeShared<FJsonObject>();
				Change->SetStringField(TEXT("type"), TEXT("input_json_delta"));
				Change->SetStringField(TEXT("partial_json"), Arguments);
				auto Event = MakeShared<FJsonObject>();
				Event->SetObjectField(TEXT("delta"), Change);
				State.ProcessEvent(TEXT("content_block_delta"), Event, Delta);
			}
			State.ProcessEvent(TEXT("content_block_stop"), MakeShared<FJsonObject>(), Delta);
		}
	}

	void FinishTruncatedTurn(ILLMStreamState& State, bool bOpenAI)
	{
		FString Delta;
		auto Event = MakeShared<FJsonObject>();
		if (bOpenAI)
		{
			State.ProcessEvent(TEXT("response.incomplete"), Event, Delta);
		}
		else
		{
			auto Change = MakeShared<FJsonObject>();
			Change->SetStringField(TEXT("stop_reason"), TEXT("max_tokens"));
			Event->SetObjectField(TEXT("delta"), Change);
			State.ProcessEvent(TEXT("message_delta"), Event, Delta);
			State.ProcessEvent(TEXT("message_stop"), MakeShared<FJsonObject>(), Delta);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_MalformedToolArguments,
	"KismetAI.BYOK.MalformedToolArgumentsRejectWholeTurn", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBYOK_MalformedToolArguments::RunTest(const FString& Parameters)
{
	for (const TCHAR* Model : {TEXT("claude-sonnet-4-6"), TEXT("gpt-5-mini")})
	{
		TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(Model);
		const auto State = Provider->CreateStreamState();
		const bool bOpenAI = IsOpenAIModel(Model);
		FinishTool(*State, bOpenAI, TEXT("{}"));
		FinishTool(*State, bOpenAI, TEXT("{\"unfinished\":"));
		FinishTruncatedTurn(*State, bOpenAI);
		FString Message, Code;
		TestTrue(TEXT("malformed tool blocks reject the whole completed stream"), State->HasStreamError(Message, Code));
		TestEqual(TEXT("parse failure is explicit"), Code, FString(TEXT("malformed_tool_arguments")));
		TestFalse(TEXT("parse failure explains recovery"), Message.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_EmptyToolArguments,
	"KismetAI.BYOK.ValidEmptyToolArgumentsRemainValid", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBYOK_EmptyToolArguments::RunTest(const FString& Parameters)
{
	for (const TCHAR* Model : {TEXT("claude-sonnet-4-6"), TEXT("gpt-5-mini")})
	{
		TUniquePtr<ILLMProvider> Provider = MakeProviderForModel(Model);
		const auto State = Provider->CreateStreamState();
		const bool bOpenAI = IsOpenAIModel(Model);
		FinishTool(*State, bOpenAI, TEXT("{}"));
		if (!bOpenAI) FinishTool(*State, false, TEXT(""));
		FinishTruncatedTurn(*State, bOpenAI);
		FString Message, Code;
		TestFalse(TEXT("valid empty arguments are not a stream error"), State->HasStreamError(Message, Code));
		TestEqual(TEXT("zero-argument tools remain present"), State->BuildResponseJson()->GetArrayField(TEXT("content")).Num(), bOpenAI ? 1 : 2);
	}
	return true;
}
