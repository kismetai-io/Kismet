// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIPlatformCommands.h"
#include "KismetAIModule.h"
#include "KismetAISessionController.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAISkill.h"
#include "ScopedTempHome.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeExit.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

constexpr auto SkillRemovalFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
namespace
{
	bool InstallRemovalFixture(const FString& Version, FKismetAISkill& Skill)
	{
		FString Error;
		const FString Json = FString::Printf(TEXT(R"({"schema_version":1,"publisher":"local","name":"removal-test","version":"%s","description":"Removal test","license":"MIT","kismet_min_version":"0.9.1","engine_versions":["%s"],"required_tools":[],"instructions":"Do nothing","resources":{}})"), *Version, *KismetAISkills::EngineVersion());
		return KismetAISkills::Parse(Json, KismetAISkills::PluginVersion(), KismetAISkills::EngineVersion(), Skill, Error) && KismetAISkills::Install(KismetAISkills::DefaultRoot(), Skill, Error);
	}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillRemoveVersion, "KismetAI.Skills.RemoveOnlyPinnedVersion", SkillRemovalFlags)
bool FSkillRemoveVersion::RunTest(const FString&)
{
	FScopedTempHome Home(TEXT("SkillRemove"));
	FKismetAISkill First, Second, Loaded;
	FString Error;
	TestTrue(TEXT("install first version"), InstallRemovalFixture(TEXT("1.0.0"), First));
	TestTrue(TEXT("install second version"), InstallRemovalFixture(TEXT("1.0.1"), Second));
	const FString Sentinel = FPaths::GetPath(KismetAISkills::DefaultRoot()) / TEXT("settings.json");
	FFileHelper::SaveStringToFile(TEXT("preserve"), *Sentinel);
	TestFalse(TEXT("reject traversal"), KismetAISkills::Remove(KismetAISkills::DefaultRoot(), TEXT("../../settings"), Error));
	FString Contents;
	FFileHelper::LoadFileToString(Contents, *Sentinel);
	TestEqual(TEXT("outside file unchanged"), Contents, FString(TEXT("preserve")));
	TestTrue(TEXT("remove exact version"), KismetAISkills::Remove(KismetAISkills::DefaultRoot(), First.Id(), Error));
	TestFalse(TEXT("removed version is absent"), KismetAISkills::Load(KismetAISkills::DefaultRoot(), First.Id(), Loaded, Error));
	TestTrue(TEXT("other version survives"), KismetAISkills::Load(KismetAISkills::DefaultRoot(), Second.Id(), Loaded, Error));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillRemoveSelection, "KismetAI.Skills.RemovalClearsOnlyMatchingSelection", SkillRemovalFlags)
bool FSkillRemoveSelection::RunTest(const FString&)
{
	FScopedTempHome Home(TEXT("SkillRemoveSelection"));
	auto& Module = FKismetAIModule::Get();
	const FKismetAIAgentConfig Original = Module.GetAgentConfig();
	ON_SCOPE_EXIT { Module.SetAgentConfig(Original); };
	FKismetAISkill First, Second;
	TestTrue(TEXT("first fixture"), InstallRemovalFixture(TEXT("1.0.0"), First));
	TestTrue(TEXT("second fixture"), InstallRemovalFixture(TEXT("1.0.1"), Second));
	FKismetAIAgentConfig Config = Original;
	Config.SelectedSkill = Second.Id();
	Module.SetAgentConfig(Config);
	TestFalse(TEXT("remove unselected version"), KismetAIPlatformCommands::Local(TEXT("remove"), First.Id()).Contains(TEXT("\"error\"")));
	TestEqual(TEXT("other selection preserved"), Module.GetAgentConfig().SelectedSkill, Second.Id());
	TestFalse(TEXT("remove selected version"), KismetAIPlatformCommands::Local(TEXT("remove"), Second.Id()).Contains(TEXT("\"error\"")));
	TestTrue(TEXT("selected skill cleared"), Module.GetAgentConfig().SelectedSkill.IsEmpty());
	TestEqual(TEXT("both removed from local store"), KismetAISkills::List(KismetAISkills::DefaultRoot()).Num(), 0);
	FString SavedJson;
	FFileHelper::LoadFileToString(SavedJson, *(FPaths::GetPath(KismetAISkills::DefaultRoot()) / TEXT("settings.json")));
	TSharedPtr<FJsonObject> Saved;
	TestTrue(TEXT("updated settings persisted"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SavedJson), Saved));
	if (Saved.IsValid()) TestTrue(TEXT("persisted selection cleared"), Saved->GetStringField(TEXT("selected_skill")).IsEmpty());
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillActiveRunGuard, "KismetAI.Skills.ActiveRunGuardIncludesHttp", SkillRemovalFlags)
bool FSkillActiveRunGuard::RunTest(const FString&)
{
	FKismetAISessionController Session;
	auto MakeAgent = []()
	{
		return MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get()), FKismetAIAgentConfig());
	};
	const FKismetAIAdmission Panel = Session.TryAdmit(EKismetAIRunOrigin::Panel, MakeAgent());
	TestTrue(TEXT("panel run active"), Panel.IsAdmitted() && Session.IsRunning());
	Session.Release(Panel.Run.ToSharedRef());
	const FKismetAIAdmission Http = Session.TryAdmit(EKismetAIRunOrigin::HttpAgentRun, MakeAgent());
	TestTrue(TEXT("HTTP run also active"), Http.IsAdmitted() && Session.IsRunning());
	Session.Release(Http.Run.ToSharedRef());
	TestFalse(TEXT("idle after both run paths finish"), Session.IsRunning());
	return true;
}

#if PLATFORM_MAC || PLATFORM_LINUX
#include <unistd.h>
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillStoreNoFollow, "KismetAI.Skills.StoreRejectsRealSymlinks", SkillRemovalFlags)
bool FSkillStoreNoFollow::RunTest(const FString&)
{
	FScopedTempHome Home(TEXT("SkillSymlinks"));
	const FString OutsideRoot = KismetAISkills::DefaultRoot();
	const FString AliasRoot = FPaths::GetPath(OutsideRoot) / TEXT("alias-store");
	FKismetAISkill Skill, Loaded;
	FString Error;
	for (int32 Level = 0; Level < 4; ++Level)
	{
		TestTrue(TEXT("install external sentinel artifact"), InstallRemovalFixture(TEXT("1.0.0"), Skill));
		const FString OutsideFile = OutsideRoot / (Skill.Id() + TEXT(".json"));
		FString Original;
		FFileHelper::LoadFileToString(Original, *OutsideFile);
		const FString Suffix = Level == 0 ? FString() : Level == 1 ? TEXT("local") : Level == 2 ? TEXT("local/removal-test") : TEXT("local/removal-test/1.0.0.json");
		const FString Link = Suffix.IsEmpty() ? AliasRoot : AliasRoot / Suffix;
		const FString Target = Suffix.IsEmpty() ? OutsideRoot : OutsideRoot / Suffix;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Link), true);
		if (!TestEqual(TEXT("create real filesystem symlink"), ::symlink(TCHAR_TO_UTF8(*Target), TCHAR_TO_UTF8(*Link)), 0)) return false;
		ON_SCOPE_EXIT
		{
			::unlink(TCHAR_TO_UTF8(*Link));
			IFileManager::Get().DeleteDirectory(*AliasRoot, false, true);
		};
		TestFalse(TEXT("load does not follow root/publisher/skill/leaf link"), KismetAISkills::Load(AliasRoot, Skill.Id(), Loaded, Error));
		TestEqual(TEXT("listing does not traverse link"), KismetAISkills::List(AliasRoot).Num(), 0);
		TestFalse(TEXT("install does not follow link"), KismetAISkills::Install(AliasRoot, Skill, Error));
		TestFalse(TEXT("remove does not follow link"), KismetAISkills::Remove(AliasRoot, Skill.Id(), Error));
		FString After;
		TestTrue(TEXT("external artifact survives"), FFileHelper::LoadFileToString(After, *OutsideFile));
		TestEqual(TEXT("external artifact bytes unchanged"), After, Original);
	}
	return true;
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRetiredInferenceCommands, "KismetAI.Skills.RetiredInferenceCommands", SkillRemovalFlags)
bool FRetiredInferenceCommands::RunTest(const FString&)
{
	FScopedTempHome Home(TEXT("RetiredInference"));
	auto& Module = FKismetAIModule::Get();
	const FKismetAIAgentConfig Original = Module.GetAgentConfig();
	ON_SCOPE_EXIT { Module.SetAgentConfig(Original); };
	const FString Result = KismetAIPlatformCommands::Local(TEXT("route"), TEXT(R"({"hosted":true,"endpoint":"https://service.example"})"));
	TestTrue(TEXT("old route command is explicitly retired"), Result.Contains(TEXT("retired")));
	FString QuotaResult;
	KismetAIPlatformCommands::Registry(TEXT("quota"), TEXT(""), [&QuotaResult](const FString& Json) { QuotaResult = Json; });
	TestTrue(TEXT("old quota command is retired without network request"), QuotaResult.Contains(TEXT("retired")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderConfirmationPersistence, "KismetAI.Skills.ProviderChoicePersistsExplicitly", SkillRemovalFlags)
bool FProviderConfirmationPersistence::RunTest(const FString&)
{
	FScopedTempHome Home(TEXT("ProviderConfirmation"));
	auto& Module = FKismetAIModule::Get();
	const FKismetAIAgentConfig Original = Module.GetAgentConfig();
	ON_SCOPE_EXIT { Module.SetAgentConfig(Original); };
	FKismetAIAgentConfig Config = Original;
	Config.bProviderConfirmationRequired = true;
	Module.SetAgentConfig(Config);
	const FString SettingsPath = FPaths::GetPath(KismetAISkills::DefaultRoot()) / TEXT("settings.json");
	FFileHelper::SaveStringToFile(TEXT(R"({"hosted_route":true,"hosted_endpoint":"https://retired.example","unrelated_setting":"preserve"})"), *SettingsPath);
	Module.SaveSettings();
	FString SavedJson;
	FFileHelper::LoadFileToString(SavedJson, *SettingsPath);
	TSharedPtr<FJsonObject> Saved;
	if (!TestTrue(TEXT("saved settings parse"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SavedJson), Saved))) return false;
	TestTrue(TEXT("migration remains blocked when unrelated setting saved"), Saved->GetBoolField(TEXT("provider_confirmation_required")));
	TestFalse(TEXT("old route removed"), Saved->HasField(TEXT("hosted_route")));
	TestFalse(TEXT("old service endpoint removed"), Saved->HasField(TEXT("hosted_endpoint")));
	TestEqual(TEXT("unrelated settings preserved"), Saved->GetStringField(TEXT("unrelated_setting")), FString(TEXT("preserve")));
	TestFalse(TEXT("explicit own-provider choice succeeds"), KismetAIPlatformCommands::Local(TEXT("confirm_provider"), TEXT("")).Contains(TEXT("\"error\"")));
	TestFalse(TEXT("memory block cleared"), Module.GetAgentConfig().bProviderConfirmationRequired);
	FFileHelper::LoadFileToString(SavedJson, *SettingsPath);
	if (TestTrue(TEXT("confirmed settings parse"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SavedJson), Saved)))
		TestFalse(TEXT("choice persists on disk"), Saved->GetBoolField(TEXT("provider_confirmation_required")));
	return true;
}
