// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "RunEntryHarness.h"
#include "KismetAIRunSetup.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderAdmission_InvalidSettings,
	"KismetAI.ProviderProfile.InvalidSettingsAdmission", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderAdmission_InvalidSettings::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunEntryHarness;
	FScopedModuleSeams Seams;
	const FKismetAIAgentConfig Original = FAccess::Config(Seams.Module);
	const FString OriginalDir = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_CONFIG_DIR"));
	const FString Directory = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetP303Settings"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *Directory);
	ON_SCOPE_EXIT
	{
		Seams.Seams->Model.Unblock();
		FAccess::Session(Seams.Module).CancelActive();
		PumpUntil([&Seams]() { return !FAccess::Session(Seams.Module).IsRunning(); });
		FAccess::Config(Seams.Module) = Original;
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *OriginalDir);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	const FString Path = Directory / TEXT("settings.json");
	const FString Invalid = TEXT(R"({"provider":"misspelled-provider","model":"claude-opus-5-5","unrelated":"preserve"})");
	FAccess::Config(Seams.Module) = FKismetAIAgentConfig();
	if (!TestTrue(TEXT("write isolated settings"), FFileHelper::SaveStringToFile(Invalid, *Path))) return false;
	FAccess::LoadSettings(Seams.Module);
	TestEqual(TEXT("invalid provider identity survives settings load"), FAccess::Config(Seams.Module).ProviderId, FString(TEXT("misspelled-provider")));
	Seams.Module.SaveSettings();
	FString Saved;
	FFileHelper::LoadFileToString(Saved, *Path);
	TestEqual(TEXT("unrelated save does not overwrite invalid settings"), Saved, Invalid);
	const FRunReply Reply = Call(&FAccess::Submit, MakeRequest(TEXT(R"({"input":"must refuse before model dispatch"})")));
	TestEqual(TEXT("HTTP refuses invalid selected provider"), Reply.Code, 400);
	TestEqual(TEXT("HTTP explains setup refusal"), Reply.ErrorCode(), FString(TEXT("run_setup_failed")));
	if (Reply.Code == 202 && Reply.Data().IsValid())
	{
		SettleRun(Seams.Module, Reply.Data()->GetStringField(TEXT("run_id")), Seams.Seams->Model);
	}
	const int32 CallsBeforePanel = Seams.Seams->Model.Calls.load();
	Seams.Module.LaunchAgent(TEXT("must also refuse panel launch"), FString(), FString(), FString());
	TestFalse(TEXT("panel takes no run lease"), FAccess::Session(Seams.Module).IsRunning());
	TestEqual(TEXT("panel makes no model call"), Seams.Seams->Model.Calls.load(), CallsBeforePanel);
	TestEqual(TEXT("neither launch dispatched a model request"), Seams.Seams->Model.Calls.load(), 0);
	TestEqual(TEXT("neither launch admitted editor mutation"), Seams.Policy.Admissions.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderAdmission_PolicySettings,
	"KismetAI.ProviderProfile.PolicySettings", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FProviderAdmission_PolicySettings::RunTest(const FString& Parameters)
{
	using namespace KismetAIRunEntryHarness;
	FKismetAIModule& Module = FKismetAIModule::Get();
	const FKismetAIAgentConfig Original = FAccess::Config(Module);
	const FString OriginalDir = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_CONFIG_DIR"));
	const FString Directory = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetP303Policy"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *Directory);
	ON_SCOPE_EXIT
	{
		FAccess::Config(Module) = Original;
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_CONFIG_DIR"), *OriginalDir);
		IFileManager::Get().DeleteDirectory(*Directory, false, true);
	};
	const FString Path = Directory / TEXT("settings.json");
	for (const int32 Limit : {0, 32768})
	{
		FAccess::Config(Module) = FKismetAIAgentConfig();
		const FString Valid = FString::Printf(TEXT("{\"operating_context_limit\":%d}"), Limit);
		if (!TestTrue(TEXT("write valid context setting"), FFileHelper::SaveStringToFile(Valid, *Path))) return false;
		FAccess::LoadSettings(Module);
		TestEqual(TEXT("explicit operating limit reaches config"), FAccess::Config(Module).OperatingContextLimit, Limit);
		FString Error;
		TestTrue(TEXT("valid policy admits"), KismetAIRunSetup::Prepare(FAccess::Config(Module), Error));
		Module.SaveSettings();
		FString Saved;
		FFileHelper::LoadFileToString(Saved, *Path);
		TSharedPtr<FJsonObject> Object;
		if (TestTrue(TEXT("saved policy parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Saved), Object)))
		{
			double SavedLimit = -1;
			TestTrue(TEXT("saved policy has operating limit"), Object->TryGetNumberField(TEXT("operating_context_limit"), SavedLimit));
			TestEqual(TEXT("saved limit round trips"), SavedLimit, static_cast<double>(Limit));
		}
	}
	for (const FString& Invalid : {
		FString(TEXT(R"({"operating_context_limit":-1})")),
		FString(TEXT(R"({"operating_context_limit":32768.5})")),
		FString(TEXT(R"({"operating_context_limit":null})")),
		FString(TEXT(R"({"operating_context_limit":"32768"})")),
		FString(TEXT(R"({"operating_context_limit":2147483648})")),
		FString(TEXT(R"({"provider":null})")),
		FString(TEXT(R"({"provider":42})")),
		FString(TEXT(R"({"provider":{}})"))})
	{
		FAccess::Config(Module) = FKismetAIAgentConfig();
		FFileHelper::SaveStringToFile(Invalid, *Path);
		FAccess::LoadSettings(Module);
		TestFalse(TEXT("invalid explicit policy cannot silently become a default"), FAccess::Config(Module).RunPolicyError.IsEmpty());
		FString Error;
		TestFalse(TEXT("invalid explicit policy refuses admission"), KismetAIRunSetup::Prepare(FAccess::Config(Module), Error));
		Module.SaveSettings();
		FString Saved;
		FFileHelper::LoadFileToString(Saved, *Path);
		TestEqual(TEXT("invalid settings remain byte-identical"), Saved, Invalid);
	}
	FAccess::Config(Module).ProviderId = TEXT("openrouter");
	FAccess::Config(Module).OperatingContextLimit = 32768;
	FFileHelper::SaveStringToFile(TEXT("{}"), *Path);
	FAccess::LoadSettings(Module);
	TestTrue(TEXT("absent provider restores legacy derivation"), FAccess::Config(Module).ProviderId.IsEmpty());
	TestEqual(TEXT("absent context restriction resets to unset"), FAccess::Config(Module).OperatingContextLimit, 0);
	TestTrue(TEXT("corrected settings clear policy refusal"), FAccess::Config(Module).RunPolicyError.IsEmpty());
	return true;
}
