// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelClient.h"
#include "Async/Async.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"

namespace
{
	class FRetrySignal final : public FOutputDevice
	{
	public:
		FRetrySignal() { GLog->AddOutputDevice(this); }
		virtual ~FRetrySignal() override { GLog->RemoveOutputDevice(this); }
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			if (Category == TEXT("LogKismetAIAgent") && FCString::Strstr(Text, TEXT("Retrying API call"))) Ready->Trigger();
		}
		FEventRef Ready{EEventMode::ManualReset};
	};
}

// Opt-in loopback proof: the local fixture must return HTTP503 to /v1/messages.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBYOK_BackoffCancellation,
	"KismetAI.BYOK.TransportBackoffCancellation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FBYOK_BackoffCancellation::RunTest(const FString& Parameters)
{
	FString Url = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TEST_PROVIDER_URL"));
	if (Url.IsEmpty())
	{
		AddInfo(TEXT("Loopback transport probe not selected; set KISMETAI_TEST_PROVIDER_URL to run it."));
		return true;
	}
	Url.RemoveFromEnd(TEXT("/"));
	const FString Prefix = TEXT("http://127.0.0.1:");
	const FString Port = Url.Mid(Prefix.Len());
	if (!Url.StartsWith(Prefix) || !Port.IsNumeric() || FCString::Atoi(*Port) < 1 || FCString::Atoi(*Port) > 65535)
	{
		AddError(TEXT("Transport fixture must be an explicit http://127.0.0.1:port loopback address"));
		return false;
	}
	FKismetAIAgentConfig Config;
	Config.ApiBaseUrl = Url;
	Config.ApiKey = TEXT("unused-loopback-test-key");
	Config.MaxRetries = 1;
	Config.RetryBaseDelayMs = 5000;
	Config.RetryMaxDelayMs = 5000;
	Config.RetryJitterFactor = 0;
	FKismetAICurlModelClient Client(Config.Model);
	TArray<FAgentMessage> Messages;
	TArray<TSharedPtr<FJsonValue>> Tools;
	const FString Prompt = TEXT("Local cancellation probe");
	const FModelCallRequest Request{Messages, Tools, Prompt, Config};
	AddExpectedError(TEXT("LLM API returned status 503"), EAutomationExpectedErrorFlags::Contains, 1);
	FRetrySignal Signal;
	auto Future = Async(EAsyncExecution::Thread, [&]() { return Client.CallWithRetry(Request, FModelCallbacks{}); });
	const bool bReachedRetry = Signal.Ready->Wait(15000);
	const double Started = FPlatformTime::Seconds();
	Client.Cancel();
	const bool bStoppedPromptly = Future.WaitFor(FTimespan::FromSeconds(1));
	// Join even on failure: no worker or credential scratch file may outlive the test.
	const FModelCallOutcome Outcome = Future.Get();
	TestTrue(TEXT("actual HTTP failure reached retry backoff"), bReachedRetry);
	TestTrue(TEXT("cancellation interrupts five-second backoff within one second"), bStoppedPromptly);
	TestEqual(TEXT("cancellation prevents a second HTTP request"), Outcome.RequestCount, 1);
	TestEqual(TEXT("call reports cancellation"), Outcome.ErrorMessage, FString(TEXT("Aborted")));
	AddInfo(FString::Printf(TEXT("Cancelled transport in %.3f seconds"), FPlatformTime::Seconds() - Started));
	return true;
}
