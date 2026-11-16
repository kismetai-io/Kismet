// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAISkill.h"
#include "Fakes/ScriptedAgentSeams.h"
#include "Misc/AutomationTest.h"

using namespace KismetAITestFakes;
constexpr auto SkillTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
namespace
{
	FString Artifact()
	{
		return TEXT(R"({"schema_version":1,"publisher":"local","name":"inspect","version":"1.0.0","description":"Inspect graphs","license":"MIT","kismet_min_version":"0.9.1","engine_versions":["5.8"],"required_tools":["read_blueprint"],"instructions":"Inspect first","resources":{"guide.md":"Use the graph"}})");
	}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillValid, "KismetAI.Skills.ValidPackage", SkillTestFlags)
bool FSkillValid::RunTest(const FString&)
{
	FKismetAISkill Skill;
	FString Error;
	TestTrue(TEXT("valid compatible JSON package"), KismetAISkills::Parse(Artifact(), TEXT("0.9.1"), TEXT("5.8"), Skill, Error));
	TestEqual(TEXT("pinned identity"), Skill.Id(), FString(TEXT("local/inspect/1.0.0")));
	TestTrue(TEXT("explicit tool permission"), Skill.RequiredTools.Contains(TEXT("read_blueprint")));
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillInvalid, "KismetAI.Skills.RejectUnsafePackages", SkillTestFlags)
bool FSkillInvalid::RunTest(const FString&)
{
	FKismetAISkill Skill;
	FString Error;
	for (const FString& Json : {
		Artifact().Replace(TEXT("guide.md"), TEXT("../guide.md")),
		Artifact().Replace(TEXT("\"schema_version\":1"), TEXT("\"schema_version\":\"1\"")),
		Artifact().Replace(TEXT("\"license\":\"MIT\""), TEXT("\"license\":123")),
		Artifact().Replace(TEXT("Use the graph"), TEXT("bad\\u0000text")),
		Artifact().Replace(TEXT("\"resources\":{\"guide.md\":\"Use the graph\"}"), TEXT("\"resources\":{\"guide.md\":42}")),
		Artifact().Replace(TEXT("\"resources\":{\"guide.md\":\"Use the graph\"}"), TEXT("\"resources\":{\"guide.md\":\"a\",\"Guide.md\":\"b\"}")),
		Artifact().Replace(TEXT("\"schema_version\":1"), TEXT("\"schema_version\":1,\"schema_version\":1")),
		Artifact().Replace(TEXT("guide.md"), TEXT("code.js")),
		Artifact().Replace(TEXT("read_blueprint"), TEXT("unknown_tool")),
		Artifact().Replace(TEXT("1.0.0"), TEXT("../bad")),
		Artifact().Replace(TEXT("\"local\""), TEXT("\"Local\"")),
		Artifact().Replace(TEXT("\"5.8\""), TEXT("\"5.9\"")),
		Artifact().Replace(TEXT("0.9.1"), TEXT("99.0.0")) })
	{
		TestFalse(TEXT("reject unsafe/incompatible input"), KismetAISkills::Parse(Json, TEXT("0.9.1"), TEXT("5.8"), Skill, Error));
		TestFalse(TEXT("actionable error"), Error.IsEmpty());
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillDispatch, "KismetAI.Skills.PermissionBeforeDispatch", SkillTestFlags)
bool FSkillDispatch::RunTest(const FString&)
{
	FScriptedModelClient Model({MakeToolTurn(TEXT("1"), TEXT("enter_plan_mode")), MakeTextTurn(TEXT("done"))});
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgentConfig Config;
	Config.bSkillRestricted = true;
	Config.MaxSteps = 3;
	Config.MaxRetries = 0;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("plan"), Rec.MakeCallback());
	const FString* Result = Rec.FirstJsonOfType(TEXT("tool_result"));
	TestTrue(TEXT("empty allowlist blocks even agent-local tools"), Result && Result->Contains(TEXT("SKILL_TOOL_DENIED")));
	return true;
}

#include "KismetAIAnthropicProvider.h"
#include "KismetAIOpenAIProvider.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOwnProviderRoute, "KismetAI.Skills.OwnProviderCredentials", SkillTestFlags)
bool FOwnProviderRoute::RunTest(const FString&)
{
	FKismetAIAgentConfig Config;
	Config.ApiBaseUrl = TEXT("http://127.0.0.1:55443");
	Config.ApiKey = TEXT("private-anthropic-key");
	Config.OpenAIApiKey = TEXT("private-openai-key");
	FLLMRequestShape Shape;
	FAnthropicProvider Anthropic;
	FOpenAIProvider OpenAI;
	TestEqual(TEXT("custom own-provider endpoint"), Anthropic.GetEndpointUrl(Config), FString(TEXT("http://127.0.0.1:55443/v1/messages")));
	TestTrue(TEXT("own Anthropic key"), Anthropic.GetExtraCurlHeaders(Config, Shape).Contains(TEXT("x-api-key: private-anthropic-key")));
	TestTrue(TEXT("own OpenAI key"), OpenAI.GetExtraCurlHeaders(Config, Shape).Contains(TEXT("Authorization: Bearer private-openai-key")));
	for (const auto& Headers : {Anthropic.GetExtraCurlHeaders(Config, Shape), OpenAI.GetExtraCurlHeaders(Config, Shape)})
		for (const FString& Header : Headers) TestFalse(TEXT("account credential never used for inference"), Header.Contains(TEXT("service-token")));

	return true;
}

#include "KismetAIRunSetup.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillPinnedStore, "KismetAI.Skills.PinnedStoreAndTamper", SkillTestFlags)
bool FSkillPinnedStore::RunTest(const FString&)
{
	const FString Root = FPaths::AutomationTransientDir() / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	FKismetAISkill Skill, Reloaded, Changed;
	FString Error;
	const FString Json = Artifact().Replace(TEXT("5.8"), *KismetAISkills::EngineVersion());
	TestTrue(TEXT("parse package"), KismetAISkills::Parse(Json, KismetAISkills::PluginVersion(), KismetAISkills::EngineVersion(), Skill, Error));
	TestTrue(TEXT("install validated package"), KismetAISkills::Install(Root, Skill, Error));
	TestTrue(TEXT("load without any service"), KismetAISkills::Load(Root, Skill.Id(), Reloaded, Error));
	TestEqual(TEXT("exact bytes preserved"), Reloaded.Json, Json);
	TestEqual(TEXT("pinned digest preserved"), Reloaded.Sha256, Skill.Sha256);
	TestTrue(TEXT("reinstall same bytes is idempotent"), KismetAISkills::Install(Root, Skill, Error));
	TestTrue(TEXT("parse different content same identity"), KismetAISkills::Parse(Json.Replace(TEXT("Inspect first"), TEXT("Changed")), KismetAISkills::PluginVersion(), KismetAISkills::EngineVersion(), Changed, Error));
	TestFalse(TEXT("immutable version cannot be overwritten"), KismetAISkills::Install(Root, Changed, Error));
	const FString Path = Root / (Skill.Id() + TEXT(".json"));
	FString Stored;
	FFileHelper::LoadFileToString(Stored, *Path);
	Stored.ReplaceInline(TEXT("Inspect first"), TEXT("Tampered"));
	FFileHelper::SaveStringToFile(Stored, *Path);
	TestFalse(TEXT("changed content fails pinned hash"), KismetAISkills::Load(Root, Skill.Id(), Reloaded, Error));
	TestFalse(TEXT("path traversal cannot load another file"), KismetAISkills::Load(Root, TEXT("../../settings"), Reloaded, Error));
	IFileManager::Get().DeleteDirectory(*Root, false, true);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillAllowedDispatch, "KismetAI.Skills.AllowlistReachesOnlyDeclaredTool", SkillTestFlags)
bool FSkillAllowedDispatch::RunTest(const FString&)
{
	const auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	FScriptedModelClient Model({MakeToolTurn(TEXT("1"), TEXT("compile_blueprint"), Args), MakeToolTurn(TEXT("2"), TEXT("read_blueprint"), Args), MakeTextTurn(TEXT("done"))});
	FScriptedToolInvoker Tools;
	Tools.SetResult(TEXT("read_blueprint"), TEXT("{\"success\":true}"));
	FKismetAIAgentConfig Config;
	Config.bSkillRestricted = true;
	Config.SkillTools.Add(TEXT("read_blueprint"));
	Config.SkillInstructions = TEXT("Skill instruction sentinel");
	Config.MaxSteps = 4;
	Config.MaxRetries = 0;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("inspect"));
	TestEqual(TEXT("only the declared read reaches editor invoker"), Tools.CallLog.Num(), 1);
	if (!Model.GetRequests().IsEmpty())
	{
		const auto& Request = Model.GetRequests()[0];
		TestTrue(TEXT("skill instructions included"), Request.SystemPrompt.Contains(TEXT("Skill instruction sentinel")));
		TestEqual(TEXT("only declared tool advertised"), Request.Tools.Num(), 1);
	}
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillPlanIntersection, "KismetAI.Skills.PlanModeStillRestrictsDeclaredTools", SkillTestFlags)
bool FSkillPlanIntersection::RunTest(const FString&)
{
	const auto Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Test"));
	FScriptedModelClient Model({MakeToolTurn(TEXT("1"), TEXT("enter_plan_mode")), MakeToolTurn(TEXT("2"), TEXT("compile_blueprint"), Args), MakeTextTurn(TEXT("done"))});
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FKismetAIAgentConfig Config;
	Config.bSkillRestricted = true;
	Config.SkillTools = {TEXT("enter_plan_mode"), TEXT("compile_blueprint")};
	Config.MaxSteps = 4;
	Config.MaxRetries = 0;
	FKismetAIAgent Agent(Tools, Model, Config);
	Agent.RunTask(TEXT("plan"), Rec.MakeCallback());
	TestEqual(TEXT("skill cannot grant mutation in plan mode"), Tools.CallLog.Num(), 0);
	return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkillRunSetup, "KismetAI.Skills.DirectRouteNoAccountRequired", SkillTestFlags)
bool FSkillRunSetup::RunTest(const FString&)
{
	FKismetAIAgentConfig Config;
	FString Error;
	TestTrue(TEXT("direct route requires no account session"), KismetAIRunSetup::Prepare(Config, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProviderMigration, "KismetAI.Skills.LegacyProviderChoiceMigration", SkillTestFlags)
bool FProviderMigration::RunTest(const FString&)
{
	FJsonObject Settings;
	TestFalse(TEXT("new install requires no migration"), KismetAIRunSetup::RequiresProviderConfirmation(Settings));
	Settings.SetBoolField(TEXT("hosted_route"), false);
	TestFalse(TEXT("existing direct provider retained"), KismetAIRunSetup::RequiresProviderConfirmation(Settings));
	Settings.SetBoolField(TEXT("hosted_route"), true);
	Settings.SetBoolField(TEXT("provider_confirmation_required"), false);
	FKismetAIAgentConfig Config;
	Config.bProviderConfirmationRequired = KismetAIRunSetup::RequiresProviderConfirmation(Settings);
	FString Error;
	TestFalse(TEXT("legacy service choice cannot silently spend own key"), KismetAIRunSetup::Prepare(Config, Error));
	TestTrue(TEXT("clear retirement message"), Error.Contains(TEXT("retired")));
	Settings.RemoveField(TEXT("hosted_route"));
	Settings.SetBoolField(TEXT("provider_confirmation_required"), true);
	TestTrue(TEXT("migration block persists across restart"), KismetAIRunSetup::RequiresProviderConfirmation(Settings));
	Settings.SetBoolField(TEXT("provider_confirmation_required"), false);
	TestFalse(TEXT("explicit choice clears migration"), KismetAIRunSetup::RequiresProviderConfirmation(Settings));
	Settings.SetStringField(TEXT("hosted_route"), TEXT("invalid"));
	TestTrue(TEXT("malformed old route fails closed"), KismetAIRunSetup::RequiresProviderConfirmation(Settings));
	return true;
}
