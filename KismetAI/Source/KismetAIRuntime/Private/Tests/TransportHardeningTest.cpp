// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Credentials must never reach the curl command line, and each call must
// get its own scratch directory. These tests pin both properties on the pure
// helpers so a future edit to CallLLMOnce cannot quietly reintroduce -H on argv.

#include "KismetAIAgent.h"
#include "KismetAIModelClient.h"
#include "KismetAIAnthropicProvider.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

constexpr auto TransportTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	const TCHAR* const SampleKey = TEXT("sk-ant-TESTKEY-do-not-leak");
}

// The argv string must carry no header values at all — only the URL and file paths.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_ArgsCarryNoSecrets, "KismetAI.Transport.ArgsCarryNoSecrets", TransportTestFlags)
bool FTransport_ArgsCarryNoSecrets::RunTest(const FString& Parameters)
{
	const FString Args = FKismetAICurlModelClient::BuildCurlArgs(
		TEXT("https://api.anthropic.com/v1/messages"),
		TEXT("/tmp/scratch/curl.cfg"),
		TEXT("/tmp/scratch/body.json"),
		TEXT("/tmp/scratch/response.sse"));

	TestFalse(TEXT("argv has no api key"), Args.Contains(SampleKey));
	TestFalse(TEXT("argv has no x-api-key header"), Args.Contains(TEXT("x-api-key")));
	TestFalse(TEXT("argv has no Authorization header"), Args.Contains(TEXT("Authorization")));
	TestFalse(TEXT("argv has no anthropic-beta header"), Args.Contains(TEXT("anthropic-beta")));
	TestFalse(TEXT("argv passes no headers at all"), Args.Contains(TEXT("-H ")));

	// The parts that must still be there.
	TestTrue(TEXT("argv references the config file"), Args.Contains(TEXT("--config \"/tmp/scratch/curl.cfg\"")));
	TestTrue(TEXT("argv posts the body file"), Args.Contains(TEXT("-d \"@/tmp/scratch/body.json\"")));
	TestTrue(TEXT("argv writes the response file"), Args.Contains(TEXT("-o \"/tmp/scratch/response.sse\"")));
	TestTrue(TEXT("argv still asks for the status code"), Args.Contains(TEXT("-w \"%{http_code}\"")));
	TestTrue(TEXT("argv keeps the endpoint"), Args.Contains(TEXT("https://api.anthropic.com/v1/messages")));
	return true;
}

// The config file is where the credential belongs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_ConfigCarriesHeaders, "KismetAI.Transport.ConfigCarriesHeaders", TransportTestFlags)
bool FTransport_ConfigCarriesHeaders::RunTest(const FString& Parameters)
{
	TArray<FString> Headers;
	Headers.Add(FString::Printf(TEXT("x-api-key: %s"), SampleKey));
	Headers.Add(TEXT("anthropic-version: 2023-06-01"));

	const FString Cfg = FKismetAICurlModelClient::BuildCurlConfigFile(Headers);

	TestTrue(TEXT("config carries the key"), Cfg.Contains(SampleKey));
	TestTrue(TEXT("config emits x-api-key as a header line"),
		Cfg.Contains(FString::Printf(TEXT("header = \"x-api-key: %s\""), SampleKey)));
	TestTrue(TEXT("config emits the version header"),
		Cfg.Contains(TEXT("header = \"anthropic-version: 2023-06-01\"")));
	TestTrue(TEXT("config always emits Content-Type"),
		Cfg.Contains(TEXT("header = \"Content-Type: application/json\"")));

	// One line per header plus Content-Type; trailing newline yields an empty last split.
	TArray<FString> Lines;
	Cfg.ParseIntoArrayLines(Lines);
	TestEqual(TEXT("one line per header plus Content-Type"), Lines.Num(), 3);
	return true;
}

// A header value containing a quote or a backslash must not break out of the
// quoted region — curl's config parser honours backslash escapes inside quotes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_ConfigEscaping, "KismetAI.Transport.ConfigEscaping", TransportTestFlags)
bool FTransport_ConfigEscaping::RunTest(const FString& Parameters)
{
	TArray<FString> Headers;
	Headers.Add(TEXT("x-weird: he said \"hi\" and C:\\path"));

	const FString Cfg = FKismetAICurlModelClient::BuildCurlConfigFile(Headers);

	TestTrue(TEXT("quotes are escaped"), Cfg.Contains(TEXT("\\\"hi\\\"")));
	TestTrue(TEXT("backslashes are escaped"), Cfg.Contains(TEXT("C:\\\\path")));
	// No bare quote may terminate the value early: the only unescaped quotes are
	// the opening and closing ones on each line.
	TArray<FString> Lines;
	Cfg.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		int32 Unescaped = 0;
		for (int32 i = 0; i < Line.Len(); ++i)
		{
			if (Line[i] == TEXT('"') && (i == 0 || Line[i - 1] != TEXT('\\')))
			{
				++Unescaped;
			}
		}
		TestEqual(FString::Printf(TEXT("exactly two unescaped quotes in: %s"), *Line), Unescaped, 2);
	}
	return true;
}

// A header whose value contains spaces and colons must round-trip intact.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_ConfigSpacesAndColons, "KismetAI.Transport.ConfigSpacesAndColons", TransportTestFlags)
bool FTransport_ConfigSpacesAndColons::RunTest(const FString& Parameters)
{
	TArray<FString> Headers;
	Headers.Add(TEXT("anthropic-beta: interleaved-thinking-2025-05-14,effort-2025-11-24"));

	const FString Cfg = FKismetAICurlModelClient::BuildCurlConfigFile(Headers);
	TestTrue(TEXT("comma-joined beta value survives"),
		Cfg.Contains(TEXT("header = \"anthropic-beta: interleaved-thinking-2025-05-14,effort-2025-11-24\"")));
	return true;
}

// Two calls must not share a scratch directory — the old fixed request_body.json
// / response_sse.txt pair let concurrent calls overwrite each other.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_ScratchDirIsUnique, "KismetAI.Transport.ScratchDirIsUnique", TransportTestFlags)
bool FTransport_ScratchDirIsUnique::RunTest(const FString& Parameters)
{
	const FString A = FKismetAICurlModelClient::MakeCallScratchDir();
	const FString B = FKismetAICurlModelClient::MakeCallScratchDir();

	TestNotEqual(TEXT("two calls get different scratch dirs"), A, B);
	TestTrue(TEXT("scratch dir is absolute"), FPaths::IsRelative(A) == false);
	TestTrue(TEXT("scratch dir lives under KismetAI/llm"), A.Contains(TEXT("KismetAI")) && A.Contains(TEXT("llm")));
	TestFalse(TEXT("no fixed request_body.json path"), A.Contains(TEXT("request_body.json")));
	return true;
}

// End to end on the real provider: the key the provider emits lands in the
// config text and never in the args text.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_AnthropicKeyNeverInArgs, "KismetAI.Transport.AnthropicKeyNeverInArgs", TransportTestFlags)
bool FTransport_AnthropicKeyNeverInArgs::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	Config.ApiKey = SampleKey;
	Config.Model = TEXT("claude-sonnet-4-6");

	FAnthropicProvider Provider;
	FLLMRequestShape Shape;
	const TArray<FString> Headers = Provider.GetExtraCurlHeaders(Config, Shape);

	const FString Cfg = FKismetAICurlModelClient::BuildCurlConfigFile(Headers);
	const FString Args = FKismetAICurlModelClient::BuildCurlArgs(
		Provider.GetEndpointUrl(Config),
		TEXT("/tmp/scratch/curl.cfg"),
		TEXT("/tmp/scratch/body.json"),
		TEXT("/tmp/scratch/response.sse"));

	TestTrue(TEXT("provider key reaches the config file"), Cfg.Contains(SampleKey));
	TestFalse(TEXT("provider key never reaches argv"), Args.Contains(SampleKey));
	return true;
}
