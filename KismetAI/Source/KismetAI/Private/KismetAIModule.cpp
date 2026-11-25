// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModule.h"
#include "KismetAIAssetState.h"
#include "KismetAIEditorMutation.h"
#include "KismetAIJsonCompat.h"
#include "KismetAICheckpoints.h"
#include "KismetAIToolResult.h"
#include "KismetAIPaths.h"
#include "KismetAIRunSetup.h"
#include "KismetAISkill.h"
#include "KismetAIPlatformCommands.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIToolExecutor.h"
#include "KismetAISessionController.h"
#include "KismetAIExternalRuns.h"
#include "KismetAISettings.h"
#include "KismetAIAuth.h"
#include "KismetAIAgent.h"
#include "KismetAILLMProvider.h"
#include "KismetAIProviderProfile.h"
#include "KismetAIModelRegistry.h"
#include "Misc/App.h"
#include "KismetAIUpdater.h"
#include "KismetAIUtils.h"
#include "KismetAIPersistence.h"
#include "KismetAIBatchRecovery.h"
#include "KismetAIMutationOutcome.h"
#include "KismetAITruncation.h"
#include "KismetAILayout.h"
#include "KismetAISessionStorage.h"
#include "KismetAIDiffNodeFactory.h"
#include "KismetAIKeyStore.h"
#include "KismetAIServerAuth.h"
#include "KismetAIMcpRoute.h"
#include "KismetAILog.h"
#include "KismetAITelemetry.h"
#include "KismetAISourceService.h"
#include "KismetAISourceJobs.h"
#include "KismetAISourceSearch.h"
#include "CoreGlobals.h"
#include "K2Node_Event.h"
#include "K2Node_CallFunction.h"
#include "BlueprintTypePromotion.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Composite.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_Knot.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/PrimitiveComponent.h"
#include "Materials/MaterialInterface.h"
#include "HAL/PlatformOutputDevices.h"
#include "Internationalization/Regex.h"
#include "Engine/SCS_Node.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Scene.h"

#include "KismetAIHttpTransport.h"
#include "KismetAIEngineCompat.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "HttpPath.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

#include "EdGraphUtilities.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "K2Node.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/BlueprintFactory.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInstanceConstant.h"
#include "StaticParameterSet.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionParameter.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "SceneTypes.h"
#include "RHI.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "IMaterialEditor.h"
#include "MaterialGraph/MaterialGraph.h"
#include "Factories/MaterialInstanceConstantFactoryNew.h"
#include "Factories/MaterialFactoryNew.h"
#include "Factories/MaterialFunctionFactoryNew.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Engine/Texture.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintFactory.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Components/Widget.h"
// Animation Blueprint support
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimationAsset.h"
#include "AnimationGraph.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateGraph.h"
#include "AnimationTransitionGraph.h"
#include "AnimationGraphSchema.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_StateResult.h"
#include "AnimGraphNode_TransitionResult.h"
#include "AnimGraphNode_StateMachineBase.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimationStateMachineSchema.h"
#include "AnimGraphNode_AssetPlayerBase.h"
#include "AnimStateNode.h"
#include "AnimStateNodeBase.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateConduitNode.h"
#include "AnimStateAliasNode.h"
#include "AnimStateEntryNode.h"
// Animation asset support (create_asset + manage_animation)
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/AnimCompositeBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Factories/AnimBlueprintFactory.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/AnimCompositeFactory.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/AimOffsetBlendSpaceFactoryNew.h"
#include "Factories/AimOffsetBlendSpaceFactory1D.h"
#include "Factories/AnimSequenceFactory.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkinnedAsset.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "K2Node_Variable.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Kismet2/EnumEditorUtils.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Engine/UserDefinedEnum.h"
#include "ObjectTools.h"
#include "EdGraphNode_Comment.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintNodeSpawner.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintAssetNodeSpawner.h"
#include "BlueprintNodeBinder.h"
#include "K2Node_MacroInstance.h"
#include "UObject/SavePackage.h"
#include "Runtime/Launch/Resources/Version.h"
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
// UE 5.7+ moved FStringOutputDevice into its own header; 5.6 declares it in Containers/UnrealString.h (pulled in via CoreMinimal).
#include "Misc/StringOutputDevice.h"
#endif
#include "ScopedTransaction.h"
#include "Misc/Guid.h"

#include "KismetAIStyle.h"
#include "KismetAILayoutCommands.h"
#include "Styling/AppStyle.h"
#include "SKismetAIPanel.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SWindow.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Docking/LayoutExtender.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "BlueprintEditorModule.h"
#include "BlueprintEditor.h"
#include "BlueprintEditorTabs.h"
#include "WorkflowOrientedApp/WorkflowTabFactory.h"

// Forward declaration: the full AnimSpawnerHelpers namespace is defined lower in the file
// (next to search_nodes/insert_node), but HandleInjectT3D — which lives earlier — needs the
// anim-graph schema test to diagnose schema-mismatch import rejections.
namespace AnimSpawnerHelpers
{
	static bool IsAnimNodeGraph(const UEdGraph* Graph);
	static bool NodeHasExecPin(const UEdGraphNode* Node);
}

// Component resolution + skeletal-mesh animation helpers are defined lower (next to the
// manage_components handler), but read_blueprint's overview — which lives earlier —
// annotates skeletal mesh components with their animation config, so forward-declare them.
class UActorComponent;
class UBlueprint;
static UActorComponent* ResolveComponentTemplate(UBlueprint* Blueprint, const FString& Name, FString& OutSource, bool bForWrite);
static FString AnimBlueprintPathFromGeneratedClass(UClass* AnimClass);
static FString AnimationModeToString(EAnimationMode::Type Mode);

namespace
{
FString GetEnvString(const TCHAR* Name)
{
	return FPlatformMisc::GetEnvironmentVariable(Name).TrimStartAndEnd();
}

TOptional<bool> ParseBoolString(const FString& Value)
{
	const FString Lower = Value.TrimStartAndEnd().ToLower();
	if (Lower == TEXT("1") || Lower == TEXT("true") || Lower == TEXT("yes") || Lower == TEXT("on"))
	{
		return true;
	}
	if (Lower == TEXT("0") || Lower == TEXT("false") || Lower == TEXT("no") || Lower == TEXT("off"))
	{
		return false;
	}
	return TOptional<bool>();
}

FString CondenseJsonObject(const TSharedPtr<FJsonObject>& Obj)
{
	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
	return Output;
}

FString MakeNoProviderKeyErrorJson(const FKismetAIAgentConfig& Config, const TCHAR* ProviderName)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetStringField(TEXT("type"), TEXT("error"));
	ErrorObj->SetNumberField(TEXT("http_status"), 0);
	ErrorObj->SetStringField(TEXT("model"), Config.Model);

	// Only reached for an empty provider key.
	TArray<TSharedPtr<FJsonValue>> Suggestions;
	ErrorObj->SetStringField(TEXT("error_code"), TEXT("no_api_key"));
	ErrorObj->SetStringField(TEXT("code"), TEXT("no_api_key"));
	ErrorObj->SetStringField(TEXT("message"),
		FString::Printf(TEXT("No %s API key configured. Open Settings to add one."), ProviderName));

	ErrorObj->SetArrayField(TEXT("suggestions"), Suggestions);
	return CondenseJsonObject(ErrorObj);
}

bool TryParseThinkingMode(const FString& Value, EThinkingMode& OutMode)
{
	const FString Lower = Value.TrimStartAndEnd().ToLower();
	if (Lower == TEXT("disabled") || Lower == TEXT("off") || Lower == TEXT("none"))
	{
		OutMode = EThinkingMode::Disabled;
		return true;
	}
	if (Lower == TEXT("enabled") || Lower == TEXT("budget"))
	{
		OutMode = EThinkingMode::Enabled;
		return true;
	}
	if (Lower == TEXT("adaptive"))
	{
		OutMode = EThinkingMode::Adaptive;
		return true;
	}
	return false;
}
}
#include "WorkflowOrientedApp/WorkflowTabManager.h"
#include "Toolkits/AssetEditorToolkitMenuContext.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Event.h"
#include "Async/Async.h"


DEFINE_LOG_CATEGORY_STATIC(LogKismetAI, Log, All);

using namespace KismetAIEditorMutation;

IMPLEMENT_MODULE(FKismetAIModule, KismetAI)

const FName FKismetAIModule::KismetAIPanelTabId(TEXT("KismetAIPanel"));

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint Editor tab factory
// ─────────────────────────────────────────────────────────────────────────────

class FKismetAIBlueprintTabFactory : public FWorkflowTabFactory
{
public:
	FKismetAIBlueprintTabFactory(TSharedPtr<FAssetEditorToolkit> InHostingApp)
		: FWorkflowTabFactory(FKismetAIModule::KismetAIPanelTabId, InHostingApp)
	{
		TabLabel = NSLOCTEXT("KismetAI", "TabTitle", "Kismet");
		TabIcon = FSlateIcon(FKismetAIStyle::GetStyleSetName(), "KismetAI.ToolbarIcon");
		bIsSingleton = true;
		ViewMenuDescription = NSLOCTEXT("KismetAI", "TabTitle", "Kismet");
		ViewMenuTooltip = NSLOCTEXT("KismetAI", "TabTooltip", "AI-driven Blueprint editing");
	}

	virtual TSharedRef<SWidget> CreateTabBody(const FWorkflowTabSpawnInfo& Info) const override
	{
		return SNew(SKismetAIPanel);
	}

	virtual TSharedRef<SDockTab> SpawnTab(const FWorkflowTabSpawnInfo& Info) const override
	{
		TSharedRef<SDockTab> Tab = FWorkflowTabFactory::SpawnTab(Info);
		FKismetAIModule::Get().BlueprintEditorKismetTab = Tab;
		return Tab;
	}
};

// ─────────────────────────────────────────────────────────────────────────────
// Module lifecycle
// ─────────────────────────────────────────────────────────────────────────────

// Created first: everything below assumes a session exists, including the
// startup retention sweep.
static TUniquePtr<FKismetAISessionController> MakeSessionController()
{
	return MakeUnique<FKismetAISessionController>();
}

bool FKismetAIModule::IsAgentRunning() const
{
	return Session.IsValid() && Session->IsRunning();
}

const FString& FKismetAIModule::GetCurrentSessionId() const
{
	static const FString Empty;
	return Session.IsValid() ? Session->GetSessionId() : Empty;
}

void FKismetAIModule::StartupModule()
{
	SourceJobs = MakeShared<FKismetAISourceJobs>();
	Session = MakeSessionController();
	ExternalRuns = MakeShared<FKismetAIExternalRunRegistry, ESPMode::ThreadSafe>();

	// Scoped authority: admission resolves existing-asset targets the way
	// the handlers do, through the asset registry only, and the editor user's
	// panel starts with ordinary edit/create authority under /Game.
	ToolPolicySlot->Scoped.SetTargetResolver(&FKismetAIModule::ResolveAuthorityTarget);
	InstallDefaultAuthority();

	// Give the runtime a way to resolve the backend URL without seeing
	// UKismetAISettings, which is a UDeveloperSettings and would drag Engine into
	// a module that must not have it. The runtime keeps the env-var
	// override and the defaults; this only supplies the settings-derived value.
	KismetAIAuth::SetBackendUrlResolver([]() -> FString
	{
		if (const UKismetAISettings* Settings = GetDefault<UKismetAISettings>())
		{
			switch (Settings->Backend)
			{
			case EKismetAIBackend::Development:
				return KismetAIAuth::GetDevelopmentBackendUrl();
			case EKismetAIBackend::Custom:
				return Settings->CustomUrl;   // empty falls back to the default
			case EKismetAIBackend::Production:
			default:
				return FString();             // empty means "use the default"
			}
		}
		return FString();
	});

	KISMETAI_LOG(LogKismetAI, Log, TEXT("Kismet starting up..."));

	// Lifetime sentinel for cross-thread callbacks; reset in ShutdownModule.
	Lifeline = MakeShared<uint8>(0);

	// Sweep .old files left by a previous self-update. Safe here: by the time we
	// run, the new DLL is already mapped, so the renamed-aside copies are no
	// longer needed for rollback.
	FKismetAIUpdater::CleanupStaleFiles();

	// Sweep expired session JSONL files. Runs before the HTTP server starts so
	// no writer can race the deletion. RetentionDays=0 disables age deletion
	// but still retroactively chmods files to 0600.
	{
		FString DaysStr = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_SESSION_RETENTION_DAYS"));
		int32 RetentionDays = 30;
		if (!DaysStr.IsEmpty())
		{
			if (DaysStr.IsNumeric())
			{
				RetentionDays = FCString::Atoi(*DaysStr);
			}
			else
			{
				KISMETAI_LOG(LogKismetAI, Warning,
					TEXT("KISMETAI_SESSION_RETENTION_DAYS=%s not numeric; using default 30"), *DaysStr);
			}
		}
		RetentionDays = FMath::Max(0, RetentionDays);
		auto SweepResult = FKismetAISessionStorage::SweepExpiredSessions(FKismetAISessionStorage::GetSessionsDirectory(), RetentionDays);
		KISMETAI_LOG(LogKismetAI, Log, TEXT("Session sweep: deleted=%d, kept=%d, rechmodded=%d (retention=%dd)"),
			SweepResult.Deleted, SweepResult.Kept, SweepResult.Rechmodded, RetentionDays);
	}

	// Load API key. Order:
	//   1. ANTHROPIC_API_KEY env var (dev override; warns if it shadows a stored key)
	//   2. Encrypted keystore at ~/.kismet/anthropic_key.enc
	//   3. Migrate legacy plaintext at ~/.kismet/credentials or ~/.kismetai/credentials
	AgentConfig.ApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_API_KEY"));
	if (!AgentConfig.ApiKey.IsEmpty())
	{
		if (KismetAIKeyStore::HasAnthropicKey())
		{
			KISMETAI_LOG(LogKismetAI, Warning,
				TEXT("ANTHROPIC_API_KEY env var is set; ignoring stored Anthropic key. Unset the env var to use the key from /login."));
		}
	}
	else
	{
		KismetAIKeyStore::LoadAnthropicKey(AgentConfig.ApiKey);
	}
	if (AgentConfig.ApiKey.IsEmpty())
	{
		const FString PlaintextPath = GetConfigDir() / TEXT("credentials");
		const FString LegacyPath = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")) / TEXT(".kismetai") / TEXT("credentials");
		FString Migrated;
		if (!FFileHelper::LoadFileToString(Migrated, *PlaintextPath))
		{
			FFileHelper::LoadFileToString(Migrated, *LegacyPath);
		}
		Migrated.TrimStartAndEndInline();
		if (!Migrated.IsEmpty() && KismetAIKeyStore::SaveAnthropicKey(Migrated))
		{
			IFileManager& FileMgr = IFileManager::Get();
			FileMgr.Delete(*PlaintextPath, false, true);
			FileMgr.Delete(*LegacyPath, false, true);
			AgentConfig.ApiKey = Migrated;
			KISMETAI_LOG(LogKismetAI, Log, TEXT("Migrated plaintext API key to encrypted keystore"));
		}
	}
	if (AgentConfig.ApiKey.IsEmpty())
	{
		KISMETAI_LOG(LogKismetAI, Warning, TEXT("No API key found (checked ANTHROPIC_API_KEY env var and encrypted keystore)"));
	}

	// Load OpenAI API key. Order:
	//   1. OPENAI_API_KEY env var (dev override; warns if it shadows a stored key)
	//   2. Encrypted keystore at ~/.kismet/openai_key.enc
	AgentConfig.OpenAIApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_API_KEY"));
	if (!AgentConfig.OpenAIApiKey.IsEmpty())
	{
		if (KismetAIKeyStore::HasOpenAIKey())
		{
			KISMETAI_LOG(LogKismetAI, Warning,
				TEXT("OPENAI_API_KEY env var is set; ignoring stored OpenAI key. Unset the env var to use the key from /login."));
		}
	}
	else
	{
		KismetAIKeyStore::LoadOpenAIKey(AgentConfig.OpenAIApiKey);
	}

	// Load MiniMax API key. Order:
	//   1. MINIMAX_API_KEY env var (dev override; warns if it shadows a stored key)
	//   2. Encrypted keystore at ~/.kismet/minimax_key.enc
	AgentConfig.MinimaxApiKey = FPlatformMisc::GetEnvironmentVariable(TEXT("MINIMAX_API_KEY"));
	if (!AgentConfig.MinimaxApiKey.IsEmpty())
	{
		if (KismetAIKeyStore::HasMinimaxKey())
		{
			KISMETAI_LOG(LogKismetAI, Warning,
				TEXT("MINIMAX_API_KEY env var is set; ignoring stored MiniMax key. Unset the env var to use the key from /login."));
		}
	}
	else
	{
		KismetAIKeyStore::LoadMinimaxKey(AgentConfig.MinimaxApiKey);
	}

	// Keys for the other provider profiles (deepseek, openrouter, google, ...).
	// Same order as above: the profile's environment variable, then
	// ~/.kismet/<slot>_key.enc. The three historical slots are handled above.
	for (const FKismetAIProviderProfile& Profile : FKismetAIProviderRegistry::Get().GetProfiles())
	{
		const FString& Slot = Profile.KeySlot;
		if (Slot.IsEmpty() || Slot == TEXT("anthropic") || Slot == TEXT("openai")
			|| Slot == TEXT("minimax") || AgentConfig.ProviderKeys.Contains(Slot))
		{
			continue;
		}
		FString Key = Profile.KeyEnvVar.IsEmpty() ? FString() : FPlatformMisc::GetEnvironmentVariable(*Profile.KeyEnvVar);
		if (Key.IsEmpty())
		{
			KismetAIKeyStore::LoadProviderKey(Slot, Key);
		}
		if (!Key.IsEmpty())
		{
			AgentConfig.ProviderKeys.Add(Slot, Key);
		}
	}

	// Load persisted settings (model choice, etc.)
	const FString AnthropicBase = FPlatformMisc::GetEnvironmentVariable(TEXT("ANTHROPIC_BASE_URL"));
	if (!AnthropicBase.IsEmpty())
	{
		AgentConfig.ApiBaseUrl = AnthropicBase;
		AgentConfig.ApiBaseUrl.RemoveFromEnd(TEXT("/"));
	}
	LoadSettings();

	// Model lists: refresh each provider's Models API and discover local models
	// on the thread pool. Never blocks startup; a failure keeps the curated
	// rows. Automation and unattended runs stay on the shipped catalog so tests
	// and evals see the same models on every machine.
	{
		const FKismetAIProviderProfile* Chosen = AgentConfig.ProviderId.IsEmpty() ? nullptr : FKismetAIProviderRegistry::Get().Find(AgentConfig.ProviderId);
		if (Chosen && KismetAIProviders::IsSelfHosted(*Chosen))
		{
			FKismetAIModelRegistry::Get().RegisterTypedModel(AgentConfig, Chosen->Id, AgentConfig.Model);
		}
		if (!FApp::IsUnattended() && !GIsAutomationTesting)
		{
			FKismetAIModelRegistry::Get().RefreshAllAsync(AgentConfig);
		}
	}

	const FString EvalModelEnv = GetEnvString(TEXT("KISMETAI_EVAL_MODEL"));
	if (!EvalModelEnv.IsEmpty())
	{
		const FString Resolved = FKismetAIAgent::ResolveModelAlias(EvalModelEnv);
		if (!Resolved.IsEmpty())
		{
			AgentConfig.Model = Resolved;
			KISMETAI_LOG(LogKismetAI, Log, TEXT("Eval model override: KISMETAI_EVAL_MODEL=%s resolved to %s"),
				*EvalModelEnv, *Resolved);
		}
		else
		{
			KISMETAI_LOG(LogKismetAI, Warning, TEXT("Ignoring unknown KISMETAI_EVAL_MODEL='%s'"), *EvalModelEnv);
		}
	}

	FString EvalThinkingEnv = GetEnvString(TEXT("KISMETAI_EVAL_THINKING"));
	if (EvalThinkingEnv.IsEmpty())
	{
		EvalThinkingEnv = GetEnvString(TEXT("KISMETAI_EVAL_THINKING_MODE"));
	}
	if (!EvalThinkingEnv.IsEmpty())
	{
		EThinkingMode ParsedMode = AgentConfig.ThinkingMode;
		if (TryParseThinkingMode(EvalThinkingEnv, ParsedMode))
		{
			AgentConfig.ThinkingMode = ParsedMode;
			KISMETAI_LOG(LogKismetAI, Log, TEXT("Eval thinking override: %s"),
				*FKismetAIAgent::ThinkingModeToString(ParsedMode));
		}
		else
		{
			KISMETAI_LOG(LogKismetAI, Warning,
				TEXT("Ignoring unknown KISMETAI_EVAL_THINKING='%s' (expected disabled, enabled, or adaptive)"),
				*EvalThinkingEnv);
		}
	}

	FString EvalBudgetEnv = GetEnvString(TEXT("KISMETAI_EVAL_THINKING_BUDGET_TOKENS"));
	if (EvalBudgetEnv.IsEmpty())
	{
		EvalBudgetEnv = GetEnvString(TEXT("KISMETAI_EVAL_THINKING_BUDGET"));
	}
	if (!EvalBudgetEnv.IsEmpty())
	{
		const int32 BudgetTokens = FCString::Atoi(*EvalBudgetEnv);
		if (BudgetTokens > 0)
		{
			AgentConfig.ThinkingBudgetTokens = BudgetTokens;
			KISMETAI_LOG(LogKismetAI, Log, TEXT("Eval thinking budget override: %d"), BudgetTokens);
		}
		else
		{
			KISMETAI_LOG(LogKismetAI, Warning,
				TEXT("Ignoring invalid KISMETAI_EVAL_THINKING_BUDGET_TOKENS='%s'"), *EvalBudgetEnv);
		}
	}

	FString EvalEffortEnv = GetEnvString(TEXT("KISMETAI_EVAL_EFFORT"));
	if (EvalEffortEnv.IsEmpty())
	{
		EvalEffortEnv = GetEnvString(TEXT("KISMETAI_EVAL_EFFORT_LEVEL"));
	}
	if (!EvalEffortEnv.IsEmpty())
	{
		TOptional<EEffortLevel> ParsedEffort = FKismetAIAgent::StringToEffortLevel(EvalEffortEnv);
		if (ParsedEffort.IsSet())
		{
			AgentConfig.EffortLevel = ParsedEffort.GetValue();
			KISMETAI_LOG(LogKismetAI, Log, TEXT("Eval effort override: %s"),
				*FKismetAIAgent::EffortLevelToString(AgentConfig.EffortLevel));
		}
		else
		{
			KISMETAI_LOG(LogKismetAI, Warning,
				TEXT("Ignoring unknown KISMETAI_EVAL_EFFORT='%s' (expected low, medium, high, xhigh, or max)"),
				*EvalEffortEnv);
		}
	}

	const FString EvalFastModeEnv = GetEnvString(TEXT("KISMETAI_EVAL_FAST_MODE"));
	if (!EvalFastModeEnv.IsEmpty())
	{
		TOptional<bool> ParsedFastMode = ParseBoolString(EvalFastModeEnv);
		if (ParsedFastMode.IsSet())
		{
			AgentConfig.bFastMode = ParsedFastMode.GetValue();
			KISMETAI_LOG(LogKismetAI, Log, TEXT("Eval fast mode override: %s"),
				AgentConfig.bFastMode ? TEXT("true") : TEXT("false"));
		}
		else
		{
			KISMETAI_LOG(LogKismetAI, Warning,
				TEXT("Ignoring invalid KISMETAI_EVAL_FAST_MODE='%s' (expected true/false)"),
				*EvalFastModeEnv);
		}
	}

	// KISMETAI_FAST_MODE env var force-enables fast mode (dev override), applied after
	// LoadSettings so it holds even when no settings.json exists. Never force-disables:
	// absent/other values leave the persisted toggle untouched.
	const FString FastModeEnv = GetEnvString(TEXT("KISMETAI_FAST_MODE"));
	TOptional<bool> ParsedFastMode = ParseBoolString(FastModeEnv);
	if (ParsedFastMode.IsSet() && ParsedFastMode.GetValue())
	{
		AgentConfig.bFastMode = true;
		KISMETAI_LOG(LogKismetAI, Log, TEXT("Fast mode force-enabled via KISMETAI_FAST_MODE (2x token pricing)"));
	}

	KISMETAI_LOG(LogKismetAI, Log, TEXT("Kismet ready: model=%s, thinking=%s"),
		*AgentConfig.Model,
		*FKismetAIAgent::ThinkingModeToString(AgentConfig.ThinkingMode));
	KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Settings: budget=%d, effort=%s"),
		AgentConfig.ThinkingBudgetTokens,
		*FKismetAIAgent::EffortLevelToString(AgentConfig.EffortLevel));

	// Per-launch HTTP bearer-token must exist before the listener accepts
	// connections — otherwise the Origin/no-token gate inside CheckRequest
	// would reject everything before the token file lands on disk.
	KismetAIServerAuth::Initialize();

	StartHttpServer();

	FKismetAIStyle::Initialize();
	FKismetAILayoutCommands::Register();
	RegisterTabSpawner();
	RegisterToolbarButton();
	RegisterBlueprintEditorExtension();

	// Register diff node factory for node tinting
	DiffNodeFactory = MakeShared<FKismetAIDiffNodeFactory>();
	FEdGraphUtilities::RegisterVisualNodeFactory(DiffNodeFactory);

	// Restore diff states from disk. Run checkpoints: prune resolved and
	// expired ones, then give every session with a pending checkpoint its Reject back.
	Checkpoints = MakeUnique<FKismetAICheckpointStore>();
	Checkpoints->Prune();
	LoadDiffStates();
	RecoverPendingCheckpoints();

	// Telemetry pipeline: HTTP+JSONL writer + UE_LOG forwarder + crash scan.
	// Initialize last so its plugin_loaded heartbeat carries the final state.
	KismetAITelemetry::Initialize();
}

// ─────────────────────────────────────────────────────────────────────────────
// Settings persistence (~/.kismet/, with fallback to legacy ~/.kismetai/)
// ─────────────────────────────────────────────────────────────────────────────

// Forwarder. The implementation moved to KismetAIPaths (runtime module) so
// KismetAIAuth and KismetAIKeyStore can resolve it without depending on the
// editor module. Kept because ~15 call sites use this spelling.
FString FKismetAIModule::GetConfigDir()
{
	return KismetAIPaths::GetConfigDir();
}

FString FKismetAIModule::GetSettingsPath()
{
	return GetConfigDir() / TEXT("settings.json");
}

void FKismetAIModule::LoadSettings()
{
	AgentConfig.RunPolicyError.Reset();
	AgentConfig.OperatingContextLimit = 0;
	AgentConfig.ProviderId.Reset();
	AgentConfig.RunBudgetError.Reset();
	AgentConfig.RunLimits = FKismetAIRunLimits();
	FString SettingsPath = GetSettingsPath();
	FString JsonString;
	if (!FFileHelper::LoadFileToString(JsonString, *SettingsPath))
	{
		if (IFileManager::Get().FileExists(*SettingsPath)) AgentConfig.RunBudgetError = TEXT("Existing settings could not be read. Correct the file and restart before running the agent.");
		return; // No settings file yet — use defaults
	}

	TSharedPtr<FJsonObject> Settings;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, Settings) || !Settings.IsValid())
	{
		AgentConfig.RunBudgetError = TEXT("settings.json is malformed; its run limits are unknown. Correct the file and restart before running the agent.");
		KISMETAI_LOG(LogKismetAI, Warning, TEXT("Failed to parse settings.json"));
		return;
	}

	FKismetAIRunLimits::ReadSettings(*Settings, AgentConfig.RunLimits, AgentConfig.RunBudgetError);
	KismetAIRunSetup::ReadOperatingContextLimit(*Settings, AgentConfig.OperatingContextLimit, AgentConfig.RunPolicyError);

	// Provider profile (Config/providers.json). Absent = derive from the model,
	// which is what every settings file written before profiles means.
	if (const TSharedPtr<FJsonValue> ProviderValue = Settings->TryGetField(TEXT("provider")); ProviderValue.IsValid())
	{
		// Retain invalid string selections so admission refuses the user's actual route.
		if (ProviderValue->Type != EJson::String || !ProviderValue->TryGetString(AgentConfig.ProviderId))
			AgentConfig.RunPolicyError = TEXT("provider must be a string naming a configured provider, or empty for legacy model-based selection. Correct settings before running.");
	}
	const TSharedPtr<FJsonObject>* BaseUrls = nullptr;
	if (Settings->TryGetObjectField(TEXT("provider_base_urls"), BaseUrls) && BaseUrls && BaseUrls->IsValid())
	{
		for (const auto& Pair : (*BaseUrls)->Values)
		{
			FString Url;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Url) && !Url.IsEmpty())
			{
				AgentConfig.ProviderBaseUrls.Add(FString(Pair.Key), Url);
			}
		}
	}

	AgentConfig.ModelContextWindows.Reset();
	const TSharedPtr<FJsonObject>* ContextWindows = nullptr;
	if (Settings->TryGetObjectField(TEXT("model_context_windows"), ContextWindows) && ContextWindows && ContextWindows->IsValid())
	{
		for (const auto& Pair : (*ContextWindows)->Values)
		{
			double Tokens = 0.0;
			if (Pair.Value.IsValid() && Pair.Value->TryGetNumber(Tokens) && Tokens >= 1024.0 && Tokens <= 100000000.0)
			{
				AgentConfig.ModelContextWindows.Add(FString(Pair.Key).ToLower(), static_cast<int32>(Tokens));
			}
		}
	}

	FString Model;
	if (Settings->TryGetStringField(TEXT("model"), Model) && !Model.IsEmpty())
	{
		// Resolve in case user stored an alias
		FString Resolved = FKismetAIAgent::ResolveModelAlias(Model);
		const FKismetAIProviderProfile* Chosen = AgentConfig.ProviderId.IsEmpty()
			? nullptr : FKismetAIProviderRegistry::Get().Find(AgentConfig.ProviderId);
		if (!Resolved.IsEmpty())
		{
			AgentConfig.Model = Resolved;
		}
		else if (Chosen && Chosen->bCustomModels)
		{
			// Local runtimes, OpenRouter and custom endpoints take any model id.
			AgentConfig.Model = Model;
		}
		else
		{
			KISMETAI_LOG(LogKismetAI, Warning, TEXT("Unknown model '%s' in settings.json, using default"), *Model);
		}
	}

	// Thinking mode: "disabled", "enabled", "adaptive" (default: adaptive)
	FString ThinkingStr;
	if (Settings->TryGetStringField(TEXT("thinking_mode"), ThinkingStr) && !ThinkingStr.IsEmpty())
	{
		AgentConfig.ThinkingMode = FKismetAIAgent::StringToThinkingMode(ThinkingStr);
	}

	// Thinking budget tokens (only relevant when mode == Enabled)
	int32 BudgetTokens = 0;
	if (Settings->TryGetNumberField(TEXT("thinking_budget_tokens"), BudgetTokens) && BudgetTokens > 0)
	{
		AgentConfig.ThinkingBudgetTokens = BudgetTokens;
	}

	// Effort level: "low", "medium", "high" (default: unset = API uses "high")
	FString EffortStr;
	if (Settings->TryGetStringField(TEXT("effort_level"), EffortStr) && !EffortStr.IsEmpty())
	{
		TOptional<EEffortLevel> Parsed = FKismetAIAgent::StringToEffortLevel(EffortStr);
		if (Parsed.IsSet())
		{
			AgentConfig.EffortLevel = Parsed.GetValue();
		}
	}

	// Fast mode (research preview): OFF by default, opt-in only. Trades 2x token price
	// for ~2.5x speed on models with a fast tier (see Config/models.json). Persisted UI
	// toggle; the KISMETAI_FAST_MODE env override is applied at the call site so it
	// holds even when no settings file exists.
	bool bFast = false;
	if (Settings->TryGetBoolField(TEXT("fast_mode"), bFast))
	{
		AgentConfig.bFastMode = bFast;
	}

	// Onboarding banner dismissal (project-context /init prompt). Default false.
	bool bDismissed = false;
	if (Settings->TryGetBoolField(TEXT("onboarding_dismissed"), bDismissed))
	{
		AgentConfig.bOnboardingDismissed = bDismissed;
	}

	// First-run account welcome. Default false: shown once.
	bool bAccountWelcomeSeen = false;
	if (Settings->TryGetBoolField(TEXT("account_welcome_seen"), bAccountWelcomeSeen))
	{
		AgentConfig.bAccountWelcomeSeen = bAccountWelcomeSeen;
	}

	// Recently picked models, most recent first. Default empty.
	const TArray<TSharedPtr<FJsonValue>>* RecentModels = nullptr;
	if (Settings->TryGetArrayField(TEXT("recent_models"), RecentModels))
	{
		AgentConfig.RecentModels.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *RecentModels)
		{
			FString ModelId;
			if (Value.IsValid() && Value->TryGetString(ModelId) && !ModelId.IsEmpty()
				&& AgentConfig.RecentModels.Num() < FKismetAIAgentConfig::MaxRecentModels)
			{
				AgentConfig.RecentModels.AddUnique(ModelId);
			}
		}
	}

	// Preferred "View Plan" editor (source-code accessor name). Default empty.
	FString PlanEditor;
	if (Settings->TryGetStringField(TEXT("plan_editor"), PlanEditor))
	{
		AgentConfig.PlanEditor = PlanEditor;
	}

	AgentConfig.bProviderConfirmationRequired = KismetAIRunSetup::RequiresProviderConfirmation(*Settings);
	Settings->TryGetStringField(TEXT("selected_skill"), AgentConfig.SelectedSkill);

}

void FKismetAIModule::SaveSettings()
{
	// Do not erase an invalid or unreadable limit by writing default unlimited values.
	if (!AgentConfig.RunBudgetError.IsEmpty() || !AgentConfig.RunPolicyError.IsEmpty()) return;
	FString ProviderError;
	if (!KismetAIProviders::ValidateSelection(AgentConfig, ProviderError)) return;
	TSharedPtr<FJsonObject> Settings = MakeShareable(new FJsonObject());

	// Load existing settings first to preserve other fields
	FString SettingsPath = GetSettingsPath();
	FString ExistingJson;
	if (FFileHelper::LoadFileToString(ExistingJson, *SettingsPath))
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ExistingJson);
		TSharedPtr<FJsonObject> Existing;
		if (!FJsonSerializer::Deserialize(Reader, Existing) || !Existing.IsValid())
		{
			AgentConfig.RunBudgetError = TEXT("Existing settings are malformed; no settings were overwritten. Correct the file and restart.");
			return;
		}
		FKismetAIRunLimits ExistingLimits;
		if (!FKismetAIRunLimits::ReadSettings(*Existing, ExistingLimits, AgentConfig.RunBudgetError)) return;
		int32 ExistingContextLimit = 0;
		if (!KismetAIRunSetup::ReadOperatingContextLimit(*Existing, ExistingContextLimit, AgentConfig.RunPolicyError)) return;
		if (const TSharedPtr<FJsonValue> ProviderValue = Existing->TryGetField(TEXT("provider"));
			ProviderValue.IsValid() && ProviderValue->Type != EJson::String)
		{
			AgentConfig.RunPolicyError = TEXT("Existing provider setting is not a string; no settings were overwritten. Correct the file and reload.");
			return;
		}
		Settings = Existing;
	}
	else if (IFileManager::Get().FileExists(*SettingsPath))
	{
		AgentConfig.RunBudgetError = TEXT("Existing settings could not be read; no settings were overwritten. Correct the file and restart.");
		return;
	}

	Settings->SetObjectField(TEXT("run_budget"), AgentConfig.RunLimits.ToJson());
	Settings->SetNumberField(TEXT("operating_context_limit"), AgentConfig.OperatingContextLimit);
	Settings->RemoveField(TEXT("hosted_route"));
	Settings->RemoveField(TEXT("hosted_endpoint"));
	Settings->SetBoolField(TEXT("provider_confirmation_required"), AgentConfig.bProviderConfirmationRequired);
	Settings->SetStringField(TEXT("selected_skill"), AgentConfig.SelectedSkill);
	Settings->SetStringField(TEXT("model"), AgentConfig.Model);
	Settings->SetStringField(TEXT("provider"), AgentConfig.ProviderId);
	{
		TSharedPtr<FJsonObject> BaseUrls = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : AgentConfig.ProviderBaseUrls)
		{
			BaseUrls->SetStringField(Pair.Key, Pair.Value);
		}
		Settings->SetObjectField(TEXT("provider_base_urls"), BaseUrls);
	}
	{
		TSharedPtr<FJsonObject> ContextWindows = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Pair : AgentConfig.ModelContextWindows)
		{
			ContextWindows->SetNumberField(Pair.Key, Pair.Value);
		}
		Settings->SetObjectField(TEXT("model_context_windows"), ContextWindows);
	}
	Settings->SetStringField(TEXT("thinking_mode"), FKismetAIAgent::ThinkingModeToString(AgentConfig.ThinkingMode));
	Settings->SetNumberField(TEXT("thinking_budget_tokens"), AgentConfig.ThinkingBudgetTokens);

	Settings->SetStringField(TEXT("effort_level"), FKismetAIAgent::EffortLevelToString(AgentConfig.EffortLevel));

	Settings->SetBoolField(TEXT("fast_mode"), AgentConfig.bFastMode);

	Settings->SetBoolField(TEXT("onboarding_dismissed"), AgentConfig.bOnboardingDismissed);

	Settings->SetBoolField(TEXT("account_welcome_seen"), AgentConfig.bAccountWelcomeSeen);

	{
		TArray<TSharedPtr<FJsonValue>> RecentModels;
		for (const FString& ModelId : AgentConfig.RecentModels)
		{
			RecentModels.Add(MakeShared<FJsonValueString>(ModelId));
		}
		Settings->SetArrayField(TEXT("recent_models"), RecentModels);
	}

	Settings->SetStringField(TEXT("plan_editor"), AgentConfig.PlanEditor);

	FString OutputJson;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputJson);
	FJsonSerializer::Serialize(Settings.ToSharedRef(), Writer);

	// Ensure directory exists
	FString Dir = FPaths::GetPath(SettingsPath);
	IFileManager::Get().MakeDirectory(*Dir, true);

	if (FFileHelper::SaveStringToFile(OutputJson, *SettingsPath))
	{
		KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Saved settings to %s"), *SettingsPath);
	}
	else
	{
		KISMETAI_LOG(LogKismetAI, Warning, TEXT("Failed to save settings to %s"), *SettingsPath);
	}
}

void FKismetAIModule::ShutdownModule()
{
	KISMETAI_LOG(LogKismetAI, Log, TEXT("Kismet shutting down..."));

	// Drop the lifetime sentinel first. Any worker-thread AsyncTask that races
	// us onto the game thread queue after this point will see WeakLife.Pin()
	// fail and bail before touching the dying module.
	Lifeline.Reset();

	// Flush plugin_unloaded + remove log forwarder before any subsystem teardown
	// can produce noise the forwarder would try to capture.
	KismetAITelemetry::Shutdown();

	// One owner settles every run before anything else is torn down —
	// close run admission, close the game-thread dispatcher (queued tool
	// callbacks are abandoned and their workers woken), cancel every run (model
	// call killed, ask_user/approval wait woken), then wait for every worker,
	// leased or not. The wait needs nothing from the game thread. A worker that
	// misses the deadline holds only shared state (agent, invoker, dispatcher,
	// policy slot, run record), never this module.
	if (Session.IsValid())
	{
		const FKismetAISessionController::FDrainReport Drain = Session->Shutdown(&GameThreadDispatcher.Get(), 5000);
		KISMETAI_LOG(LogKismetAI, Log, TEXT("Agent runs at shutdown: %d cancelled, %d drained, %d still running"),
			Drain.Cancelled, Drain.Drained, Drain.UndrainedRunIds.Num());
	}
	else
	{
		GameThreadDispatcher->Close();
	}

	if (SourceJobs.IsValid()) SourceJobs->Close();
	KismetAISourceSearch::Close();

	// Run finalizations still waiting for the editor to leave a save or
	// slow task. Lifeline is already gone, so each skips the module's state and
	// only settles what it owns (an external run's terminal outcome). Their
	// tickers must not outlive the module's code.
	{
		TMap<uint64, TPair<FTSTicker::FDelegateHandle, TFunction<void()>>> Pending = MoveTemp(DeferredObjectWork);
		DeferredObjectWork.Reset();
		for (TPair<uint64, TPair<FTSTicker::FDelegateHandle, TFunction<void()>>>& Entry : Pending)
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Entry.Value.Key);
			Entry.Value.Value();
		}
	}
	RegisteredPanels.Empty();

	if (DiffNodeFactory.IsValid())
	{
		FEdGraphUtilities::UnregisterVisualNodeFactory(DiffNodeFactory);
		DiffNodeFactory.Reset();
	}

	UnregisterTabSpawner();

	if (FModuleManager::Get().IsModuleLoaded("Kismet"))
	{
		FBlueprintEditorModule& BlueprintEditorModule = FModuleManager::GetModuleChecked<FBlueprintEditorModule>("Kismet");
		BlueprintEditorModule.OnRegisterTabsForEditor().Remove(BlueprintEditorTabsHandle);
		BlueprintEditorModule.OnRegisterLayoutExtensions().Remove(BlueprintEditorLayoutHandle);
	}

	if (UObjectInitialized())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

	FKismetAILayoutCommands::Unregister();
	FKismetAIStyle::Shutdown();
	StopHttpServer();
}

// ─────────────────────────────────────────────────────────────────────────────
// Panel registry & agent lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIModule::RegisterPanel(SKismetAIPanel* Panel)
{
	RegisteredPanels.AddUnique(Panel);
	KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Panel registered (%d total)"), RegisteredPanels.Num());

	// Diff state is NOT pushed here — it's too early (JS hasn't restored tabs yet).
	// Instead, JS calls RequestDiffState after tab restoration is complete.
	// We do sync the factory though, in case the session was already set.
	SyncFactoryWithDiffState();
}

void FKismetAIModule::UnregisterPanel(SKismetAIPanel* Panel)
{
	RegisteredPanels.Remove(Panel);
	KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Panel unregistered (%d remaining)"), RegisteredPanels.Num());
}

void FKismetAIModule::BroadcastJS(const FString& Script)
{
	for (SKismetAIPanel* Panel : RegisteredPanels)
	{
		if (Panel)
		{
			Panel->ExecuteJavascriptSafe(Script);
		}
	}
}

void FKismetAIModule::LaunchAgent(const FString& UserMessage, const FString& ImagesJson, const FString& SessionId, const FString& RefsJson)
{
	if (Session->IsClosed()) return;

	// Lease: while any run holds it (a panel run, or an /agent/run the
	// panel did not start), a panel message waits in the queue rather than
	// being dropped or run alongside. Whichever run releases the lease starts it.
	if (Session->IsRunning())
	{
		Session->Queue().Enqueue(UserMessage, RefsJson, ImagesJson);
		BroadcastQueueState();
		return;
	}
	FKismetAIAgentConfig Config = AgentConfig;
	FString SetupError;
	if (!KismetAIRunSetup::Prepare(Config, SetupError))
	{
		BroadcastJS(FString::Printf(TEXT("window.onAgentEvent && window.onAgentEvent('error', {message:'%s',code:'run_setup_failed'})"), *SetupError.ReplaceCharWithEscapedChar()));
		return;
	}


	// Provider-aware key gate (panel path mirror of HandleAgentRun). Without a
	// key for the selected model's provider, surface a clean no_api_key error
	// the UI can act on (auto-open Settings) instead of a raw provider 401.
	if (!AgentFactoryForTests)
	{
		const FKismetAIProviderProfile& Profile = KismetAIProviders::ResolveProfile(AgentConfig);
		if (!KismetAIProviders::HasUsableKey(AgentConfig, Profile))
		{
			const FString ErrJson = MakeNoProviderKeyErrorJson(AgentConfig, *Profile.DisplayName);
			BroadcastJS(FString::Printf(
				TEXT("window.onAgentEvent && window.onAgentEvent('error', %s)"), *ErrJson));
			return;
		}
	}

	// Create the agent and take the lease before any side effect below (refs,
	// snapshots, broadcasts). Admission was checked above on this same thread,
	// so a refusal here means the controller closed or a run was admitted from
	// another thread in between: queue, exactly as above.
	TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe> AgentPtr = AgentFactoryForTests
		? AgentFactoryForTests(Config)
		: MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(MakeUnique<FKismetAIModuleToolInvoker>(*this), Config);
	// Live panel path: a real user is present, so ask_user may suspend and wait.
	AgentPtr->SetInteractiveAllowed(true);
	// The panel's caller is the editor user who pressed Send.
	AgentPtr->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Panel, TEXT("editor-user")));
	const FKismetAIAdmission Admission = Session->TryAdmit(EKismetAIRunOrigin::Panel, AgentPtr);
	if (!Admission.IsAdmitted())
	{
		if (Admission.Refusal == EKismetAIAdmissionRefusal::Busy)
		{
			Session->Queue().Enqueue(UserMessage, RefsJson, ImagesJson);
			BroadcastQueueState();
		}
		return;
	}
	const FKismetAIRunRef Run = Admission.Run.ToSharedRef();
	// The controller's run id is the journal and event run id too.
	AgentPtr->SetRunIdForNextRun(Run->GetRunId());

	// Resolve @-references from the UI, build the <references> block, collect Blueprint paths for snapshotting.
	// All registry access runs here on the game thread.
	TArray<FString> BlueprintPaths;
	FString RefsBlock = BuildReferencesBlock(RefsJson, BlueprintPaths);

	// Prepend the references block to the user message when refs are present.
	// Agent sees the block verbatim; the SessionBlueprintPath is set from the first focused
	// (or first Blueprint) ref path passed via BlueprintPaths (3.5 approach).
	FString Message = RefsBlock.IsEmpty() ? UserMessage : (RefsBlock + UserMessage);

	// Broadcast user message to all panels so they all show the bubble
	{
		FString Escaped = UserMessage
			.Replace(TEXT("\\"), TEXT("\\\\"))
			.Replace(TEXT("'"), TEXT("\\'"))
			.Replace(TEXT("\n"), TEXT("\\n"))
			.Replace(TEXT("\r"), TEXT("\\r"));
		BroadcastJS(FString::Printf(
			TEXT("window.onUserMessageBroadcast && window.onUserMessageBroadcast('%s')"),
			*Escaped));
	}

	// Ensure we have a session ID for the snapshot (generate one if new session)
	FString SnapshotSessionId = SessionId.IsEmpty() ? Session->GetSessionId() : SessionId;
	const bool bStartNewSession = SnapshotSessionId.IsEmpty();
	if (bStartNewSession)
	{
		SnapshotSessionId = FKismetAISessionStorage::CreateSessionId();
		Session->SetSessionId(SnapshotSessionId);
	}

	// A new agent run establishes a fresh diff baseline. If a previous run in
	// the same chat left pending diff metadata behind, comparing against that old
	// primary Blueprint can surface stale "nodes added" UI for read-only runs.
	if (BlueprintPaths.Num() > 0)
	{
		GetOrCreateDiffState(SnapshotSessionId) = FDiffState();
	}

	// Snapshot all Blueprint-class refs before agent runs (for diff/accept/reject)
	for (const FString& BPPath : BlueprintPaths)
	{
		TakeGraphSnapshot(SnapshotSessionId, BPPath, TEXT("EventGraph"));
	}

	// Notify all panels that agent is starting
	BroadcastJS(TEXT("window.onAgentStarted && window.onAgentStarted()"));

	FString CapturedMessage = Message;
	FString CapturedImages = ImagesJson;
	FString CapturedSessionId = SessionId.IsEmpty() ? Session->GetSessionId() : SessionId;
	TWeakPtr<uint8> WeakLife = Lifeline;

	// Holds the terminal error/done status so the
	// game-thread finalization below can fire the right completion notification.
	// Written on the worker thread inside the step callback, read after RunTask
	// returns — the AsyncTask hop provides the necessary memory barrier.
	TSharedRef<FString> FinalStatus = MakeShared<FString>(FString(TEXT("failed")));

	// The controller runs this as the run's worker and owns its lifetime: it
	// is cancelled and drained by Session->Shutdown. The body touches `this`
	// only inside game-thread tasks guarded by WeakLife.
	const bool bStarted = Session->StartWorker(Run, [this, WeakLife, AgentPtr, Run, CapturedMessage, CapturedImages, CapturedSessionId, bStartNewSession, FinalStatus]()
	{
		FOnAgentStep StepCallback;
		StepCallback.BindLambda([this, WeakLife, FinalStatus](const FString& EventType, const FString& EventJson)
		{
			FString CapturedType = EventType;
			FString CapturedJson = EventJson;

			if (CapturedType == TEXT("done") || CapturedType == TEXT("error"))
			{
				TSharedPtr<FJsonObject> Terminal;
				*FinalStatus = CapturedType == TEXT("error") ? TEXT("failed") : TEXT("completed");
				if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CapturedJson), Terminal) && Terminal.IsValid())
				{
					Terminal->TryGetStringField(TEXT("status"), *FinalStatus);
				}
			}

			AsyncTask(ENamedThreads::GameThread, [this, WeakLife, CapturedType, CapturedJson]()
			{
				if (!WeakLife.IsValid()) return;
				// Broadcast agent events to ALL registered panels
				FString Script = FString::Printf(
					TEXT("window.onAgentEvent && window.onAgentEvent('%s', %s)"),
					*CapturedType, *CapturedJson);
				BroadcastJS(Script);
			});
		});

		AgentPtr->RunTask(CapturedMessage, StepCallback, CapturedImages, CapturedSessionId, bStartNewSession);

		// Capture the session ID assigned by the agent
		FString ResultSessionId = AgentPtr->GetSessionId();

		// Finalize on the game thread: this is where the lease is released.
		// FinalizeRunDiff loads the Blueprint and fingerprints packages, so it
		// waits out a save or slow task that pumps this task.
		auto Finalize = [this, WeakLife, Run, ResultSessionId, FinalStatus]()
		{
			if (!WeakLife.IsValid()) return;

			Session->SetSessionId(ResultSessionId);

			// Push session ID to all panels
			BroadcastJS(FString::Printf(
				TEXT("window.onSessionIdAssigned && window.onSessionIdAssigned('%s')"),
				*ResultSessionId));

			Session->Release(Run);

			// Compute diff and mark changed nodes BEFORE any next launch snapshots,
			// and record the run's result in its checkpoints.
			FinalizeRunDiff(Run->GetRunId());

			// Check whether a queued/steered message is waiting to run next.
			TOptional<FQueuedMessage> Next = Session->Queue().PickNext();
			const bool bWillContinue = Next.IsSet();

			// Notify all panels that agent stopped, passing the continue flag.
			for (SKismetAIPanel* Panel : RegisteredPanels)
			{
				if (Panel)
				{
					Panel->NotifyAgentStopped(bWillContinue);
				}
			}

			if (bWillContinue)
			{
				// Broadcast updated queue state (item was consumed by PickNext).
				BroadcastQueueState();
				// Launch the next queued/steered message; the lease was released
				// above, so LaunchAgent admits it. Same session id preserves
				// history and lets ValidateToolPairing repair any aborted-mid-tool turn.
				LaunchAgent(Next->Text, Next->ImagesJson, Session->GetSessionId(), Next->RefsJson);
			}
			else
			{
				// Editor-side "generation complete" feedback (toast + attention + sound).
				// Only fires when there is no further queued work.
				NotifyAgentRunComplete(*FinalStatus);
			}
		};
		AsyncTask(ENamedThreads::GameThread, [this, WeakLife, Finalize = MoveTemp(Finalize)]()
		{
			if (!WeakLife.IsValid()) return;
			RunWhenObjectAccessAllowed(Finalize);
		});
	});
	if (!bStarted)
	{
		// Shutdown closed admission in between; the body never runs.
		Session->Release(Run);
	}
}

void FKismetAIModule::LaunchNextQueuedMessage()
{
	if (Session->IsRunning() || Session->IsClosed())
	{
		return;
	}
	TOptional<FQueuedMessage> Next = Session->Queue().PickNext();
	if (!Next.IsSet())
	{
		return;
	}
	BroadcastQueueState();
	LaunchAgent(Next->Text, Next->ImagesJson, Session->GetSessionId(), Next->RefsJson);
}

// ---------------------------------------------------------------------------
// Message queue — all methods run on the game thread.
// ---------------------------------------------------------------------------

void FKismetAIModule::BroadcastQueueState()
{
	BroadcastJS(FString::Printf(TEXT("window.onQueueState && window.onQueueState(%s)"), *Session->Queue().ToJson()));
}

FString FKismetAIModule::GetQueueJson()
{
	return Session->Queue().ToJson();
}

void FKismetAIModule::EnqueueUserMessage(const FString& Text, const FString& RefsJson, const FString& ImagesJson)
{
	if (!Session->IsRunning())
	{
		LaunchAgent(Text, ImagesJson, Session->GetSessionId(), RefsJson);
	}
	else
	{
		Session->Queue().Enqueue(Text, RefsJson, ImagesJson);
		BroadcastQueueState();
	}
}

void FKismetAIModule::SteerNow(const FString& Text, const FString& RefsJson, const FString& ImagesJson)
{
	FQueuedMessage M;
	M.Id = FKismetAIMessageQueue::MakeId();
	M.Text = Text;
	M.RefsJson = RefsJson;
	M.ImagesJson = ImagesJson;

	if (!Session->IsRunning())
	{
		LaunchAgent(M.Text, M.ImagesJson, Session->GetSessionId(), M.RefsJson);
	}
	else
	{
		Session->Queue().PromoteToSteer(M);
		BroadcastQueueState();
		AbortAgent();
		// Do NOT launch here — the finalize hook dispatches the pending steer
		// after the aborted run unwinds on the game thread.
	}
}

void FKismetAIModule::SteerQueued(const FString& Id)
{
	if (!Session->IsRunning())
	{
		if (Session->Queue().PromoteQueuedToSteer(Id))
		{
			TOptional<FQueuedMessage> M = Session->Queue().PickNext();
			BroadcastQueueState();
			if (M.IsSet())
			{
				LaunchAgent(M->Text, M->ImagesJson, Session->GetSessionId(), M->RefsJson);
			}
		}
	}
	else
	{
		if (Session->Queue().PromoteQueuedToSteer(Id))
		{
			BroadcastQueueState();
			AbortAgent();
		}
	}
}

void FKismetAIModule::RemoveQueued(const FString& Id)
{
	if (Session->Queue().Remove(Id))
	{
		BroadcastQueueState();
	}
}

void FKismetAIModule::MoveQueued(const FString& Id, int32 NewIndex)
{
	if (Session->Queue().Move(Id, NewIndex))
	{
		BroadcastQueueState();
	}
}

void FKismetAIModule::EditQueued(const FString& Id, const FString& NewText)
{
	if (Session->Queue().Edit(Id, NewText))
	{
		BroadcastQueueState();
	}
}

// ---------------------------------------------------------------------------

void FKismetAIModule::NotifyAgentRunComplete(const FString& Status)
{
	// Slate + editor APIs are game-thread only. Callers fire this from the
	// finalization AsyncTask, which already runs on the game thread.
	check(IsInGameThread());

	// User-initiated aborts are intentional — stay silent (no toast/sound/attention).
	if (Status == TEXT("aborted"))
	{
		return;
	}

	// Only an accepted answer or a prepared plan is a successful handoff.
	const bool bSuccess = Status == TEXT("completed") || Status == TEXT("plan_ready");

	// 1. In-editor toast (bottom-right). Cross-platform via Slate.
	{
		const FText Message = bSuccess
			? FText::FromString(TEXT("Kismet — generation complete"))
			: FText::FromString(Status == TEXT("max_steps") ? TEXT("Kismet — stopped at step limit")
				: Status == TEXT("uncertain") ? TEXT("Kismet — check partially applied changes")
				: TEXT("Kismet — run failed"));

		FNotificationInfo Info(Message);
		Info.ExpireDuration = 4.0f;
		Info.bFireAndForget = true;
		Info.bUseSuccessFailIcons = true;

		TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (Item.IsValid())
		{
			Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		}
	}

	// 2. Dock bounce (Mac) / taskbar flash (Windows) so the user notices when the
	//    editor isn't focused. DrawAttention is a cross-platform Slate/window API.
	if (FSlateApplication::IsInitialized())
	{
		TSharedPtr<SWindow> Window = FSlateApplication::Get().GetActiveTopLevelWindow();
		if (!Window.IsValid())
		{
			// Editor unfocused: fall back to the window hosting a registered panel.
			for (SKismetAIPanel* Panel : RegisteredPanels)
			{
				if (Panel)
				{
					Window = FSlateApplication::Get().FindWidgetWindow(Panel->AsShared());
					if (Window.IsValid()) break;
				}
			}
		}
		if (Window.IsValid())
		{
			Window->DrawAttention(FWindowDrawAttentionParameters(EWindowDrawAttentionRequestType::UntilActivated));
		}
	}

	// 3. Sound. PlayEditorSound respects the editor's "Enable Editor Sounds" setting.
	if (GEditor)
	{
		GEditor->PlayEditorSound(bSuccess
			? TEXT("/Engine/EditorSounds/Notifications/CompileSuccess_Cue.CompileSuccess_Cue")
			: TEXT("/Engine/EditorSounds/Notifications/CompileFailed_Cue.CompileFailed_Cue"));
	}
}

void FKismetAIModule::PushDiffStateToPanel(SKismetAIPanel* Panel)
{
	if (!Panel) return;

	SyncFactoryWithDiffState();

	FDiffState* State = GetActiveDiffState();
	if (State && State->bHasPendingDiff)
	{
		int32 Total = State->AddedNodeGuids.Num() + State->ModifiedNodeGuids.Num();
		// The path and extra-Blueprint count let the pinned review bar name what changed.
		Panel->ExecuteJavascriptSafe(FString::Printf(
			TEXT("window.onDiffReady && window.onDiffReady(%d, false, '%s', %d)"),
			Total, *State->BlueprintPath.ReplaceCharWithEscapedChar(), State->AdditionalBlueprints.Num()));
	}
	else if (State && State->bCanReapply)
	{
		Panel->ExecuteJavascriptSafe(TEXT("window.onDiffResolved && window.onDiffResolved('rejected', true)"));
	}
}

void FKismetAIModule::AbortAgent()
{
	// Signal and return; never wait here. Waiting on the game thread for a
	// worker parked on a game-thread tool call froze the editor until a
	// timeout. The run's own finalization releases the lease and starts any
	// pending steer; only ShutdownModule drains (Session->Shutdown).
	Session->CancelActive();
}

void FKismetAIModule::SetCurrentSessionId(const FString& SessionId)
{
	bool bChanged = (Session->GetSessionId() != SessionId);
	Session->SetSessionId(SessionId);

	if (bChanged)
	{
		// Sync factory GUIDs for the new session's diff state
		SyncFactoryWithDiffState();
	}
	// JS calls RequestDiffState explicitly when it needs the diff bar
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint diff (per-session, accept/reject/reapply)
// ─────────────────────────────────────────────────────────────────────────────

FKismetAIModule::FDiffState& FKismetAIModule::GetOrCreateDiffState(const FString& SessionId)
{
	return DiffStates.FindOrAdd(SessionId);
}

FKismetAIModule::FDiffState* FKismetAIModule::GetActiveDiffState()
{
	if (Session->GetSessionId().IsEmpty()) return nullptr;
	return DiffStates.Find(Session->GetSessionId());
}

bool FKismetAIModule::HasPendingDiff() const
{
	if (Session->GetSessionId().IsEmpty()) return false;
	const FDiffState* State = DiffStates.Find(Session->GetSessionId());
	return State && State->bHasPendingDiff;
}

bool FKismetAIModule::CanReapply() const
{
	if (Session->GetSessionId().IsEmpty()) return false;
	const FDiffState* State = DiffStates.Find(Session->GetSessionId());
	return State && State->bCanReapply;
}

void FKismetAIModule::TakeSessionBaseline(const FString& SessionId, const FString& BlueprintPath, const FString& GraphName)
{
	TakeGraphSnapshot(SessionId, BlueprintPath, GraphName);
}

FString FKismetAIModule::ArmDiffSessionForRun(const FString& RequestedSessionId, const FString& BlueprintPath)
{
	// Establish the session id this run will use. The HTTP request normally does
	// not carry one (the agent generates and returns it), so generate up-front and
	// hand the same id to RunTask so the agent and the module diff state agree.
	FString SessionId = RequestedSessionId;
	if (SessionId.IsEmpty())
	{
		SessionId = FKismetAISessionStorage::CreateSessionId();
	}
	Session->SetSessionId(SessionId);

	if (BlueprintPath.IsEmpty())
	{
		// No target blueprint hint — still set the session id (so a state exists for
		// the run); reposition-on-first-wire simply has no baseline to arm against.
		return SessionId;
	}

	// A new run establishes a fresh baseline: reset any prior diff state for this
	// session (stale "nodes added"/island marks from an earlier run in the same
	// session must not leak into this one). Mirrors LaunchAgent.
	GetOrCreateDiffState(SessionId) = FDiffState();

	// Snapshot the blueprint's primary editing graph. For an Animation Blueprint the
	// pose AnimGraph is the working surface (where insert_node/inject_t3d land nodes);
	// otherwise the EventGraph, matching the panel path.
	FString GraphName = TEXT("EventGraph");
	if (UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(BlueprintPath))
	{
		if (Cast<UAnimBlueprint>(Blueprint))
		{
			if (UEdGraph* AnimGraph = KismetAIUtils::FindAnimGraph(Blueprint))
			{
				GraphName = AnimGraph->GetName();
			}
		}
	}
	TakeGraphSnapshot(SessionId, BlueprintPath, GraphName);

	return SessionId;
}

void FKismetAIModule::TakeGraphSnapshot(const FString& SessionId, const FString& BlueprintPath, const FString& GraphName)
{
	FDiffState& State = GetOrCreateDiffState(SessionId);

	// Helper lambda to snapshot a graph's nodes
	auto SnapshotNodes = [&](TArray<FNodeSnapshot>& OutNodes)
	{
		OutNodes.Empty();
		UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(BlueprintPath);
		if (!Blueprint) return;
		UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName);
		if (!Graph) return;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			FNodeSnapshot Snap;
			Snap.NodeName = Node->GetName();
			Snap.NodeGuid = Node->NodeGuid;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input) continue;
				if (Pin->DefaultValue.IsEmpty() && Pin->DefaultTextValue.IsEmpty()) continue;
				FPinSnapshot PinSnap;
				PinSnap.PinName = Pin->PinName.ToString();
				PinSnap.DefaultValue = Pin->DefaultValue;
				Snap.PinDefaults.Add(MoveTemp(PinSnap));
			}

			OutNodes.Add(MoveTemp(Snap));
		}
	};

	// If this is the first blueprint in the session, or the same one, use primary slot
	if (State.BlueprintPath.IsEmpty() || State.BlueprintPath == BlueprintPath)
	{
		State.BaselineNodes.Empty();
		State.AddedNodeNames.Empty();
		State.AddedNodeGuids.Empty();
		State.bHasPendingDiff = false;
		State.bCanReapply = false;
		State.BlueprintPath = BlueprintPath;
		State.GraphName = GraphName;
		SnapshotNodes(State.BaselineNodes);

		KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Snapshot [%s]: %d nodes in %s/%s"), *SessionId, State.BaselineNodes.Num(), *BlueprintPath, *GraphName);
	}
	else
	{
		// Different blueprint — track in AdditionalBlueprints if not already snapshotted
		if (!State.AdditionalBlueprints.Contains(BlueprintPath))
		{
			FBlueprintDiffEntry& Entry = State.AdditionalBlueprints.Add(BlueprintPath);
			Entry.BlueprintPath = BlueprintPath;
			Entry.GraphName = GraphName;
			SnapshotNodes(Entry.BaselineNodes);

			KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Snapshot [%s] (additional): %d nodes in %s/%s"), *SessionId, Entry.BaselineNodes.Num(), *BlueprintPath, *GraphName);
		}
	}
}

void FKismetAIModule::ComputeDiffAndMarkNodes()
{
	FDiffState* State = GetActiveDiffState();
	if (!State || State->BlueprintPath.IsEmpty() || State->BaselineNodes.Num() == 0) return;

	const bool bHadDiffUiState = State->bHasPendingDiff || State->bCanReapply
		|| State->AddedNodeGuids.Num() > 0
		|| State->ModifiedNodeGuids.Num() > 0;

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(State->BlueprintPath);
	if (!Blueprint) return;

	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, State->GraphName);
	if (!Graph) return;

	// Build baseline lookup: GUID -> snapshot
	TMap<FGuid, const FNodeSnapshot*> BaselineByGuid;
	for (const FNodeSnapshot& Snap : State->BaselineNodes)
	{
		BaselineByGuid.Add(Snap.NodeGuid, &Snap);
	}

	// Find added nodes and modified nodes
	State->AddedNodeNames.Empty();
	State->AddedNodeGuids.Empty();
	State->ModifiedNodeGuids.Empty();

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		const FNodeSnapshot** BaselinePtr = BaselineByGuid.Find(Node->NodeGuid);
		if (!BaselinePtr)
		{
			// New node — not in baseline
			State->AddedNodeNames.Add(Node->GetName());
			State->AddedNodeGuids.Add(Node->NodeGuid);

			// For node types the factory can't handle (events, tunnels),
			// add a comment bubble as visual fallback
			if (Node->IsA<UK2Node_Event>() || Node->IsA<UK2Node_Tunnel>() || Node->IsA<UK2Node_Composite>())
			{
				Node->NodeComment = TEXT("Added by Kismet");
				Node->bCommentBubblePinned = true;
				Node->bCommentBubbleVisible = true;
			}
		}
		else
		{
			// Existing node — check pin defaults for changes
			const FNodeSnapshot& Baseline = **BaselinePtr;
			bool bModified = false;

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin || Pin->Direction != EGPD_Input) continue;

				FString CurrentValue = Pin->DefaultValue;

				// Find baseline value for this pin
				FString OldValue;
				for (const FPinSnapshot& PinSnap : Baseline.PinDefaults)
				{
					if (PinSnap.PinName == Pin->PinName.ToString())
					{
						OldValue = PinSnap.DefaultValue;
						break;
					}
				}

				if (CurrentValue != OldValue)
				{
					bModified = true;
				}
			}

			if (bModified)
			{
				State->ModifiedNodeGuids.Add(Node->NodeGuid);
			}
		}
	}

	int32 TotalChanges = State->AddedNodeGuids.Num() + State->ModifiedNodeGuids.Num();
	if (TotalChanges > 0)
	{
		State->bHasPendingDiff = true;
		State->bCanReapply = false;

		// Sync factory and refresh
		SyncFactoryWithDiffState();
		RefreshDiffedGraphNodes();
		SaveDiffStates();

		KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Diff: %d added, %d modified"),
			State->AddedNodeGuids.Num(), State->ModifiedNodeGuids.Num());

		BroadcastJS(FString::Printf(
			TEXT("window.onDiffReady && window.onDiffReady(%d, false, '%s', %d)"),
			TotalChanges, *State->BlueprintPath.ReplaceCharWithEscapedChar(), State->AdditionalBlueprints.Num()));
	}
	else
	{
		State->bHasPendingDiff = false;
		State->bCanReapply = false;
		State->AdditionalBlueprints.Empty();

		SyncFactoryWithDiffState();
		RefreshDiffedGraphNodes();
		SaveDiffStates();

		if (bHadDiffUiState)
		{
			BroadcastJS(TEXT("window.onDiffResolved && window.onDiffResolved('unchanged')"));
		}

		KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Diff: no graph changes"));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Reposition-on-first-wire
//
// insert_node's dominant flow is "insert with no connect_from" (the node spawns
// with zero links → the layout has no anchor → it lands as an island off to the
// right) THEN a separate edit_blueprint connect. Without this, that node stays
// stranded across the canvas from the node it feeds. We record the island spot at
// insert/import time; when the node is first wired, if it is still parked exactly
// there, we pull it next to its new peer with the same containment placement that
// respects free space, obstacles, and comment membership. One-shot, and only for
// nodes the agent created this session — user/pre-session nodes never move.
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIModule::IsAgentCreatedNode(const FString& BlueprintPath, const UEdGraphNode* Node) const
{
	if (!Node) return false;
	if (Session->GetSessionId().IsEmpty()) return false;
	const FDiffState* State = DiffStates.Find(Session->GetSessionId());
	if (!State) return false;

	// Pick the baseline snapshot for this blueprint: the primary slot or one of the
	// additional-blueprint entries. A node is agent-created iff its GUID is absent
	// from that baseline.
	const TArray<FNodeSnapshot>* Baseline = nullptr;
	if (State->BlueprintPath == BlueprintPath)
	{
		Baseline = &State->BaselineNodes;
	}
	else if (const FBlueprintDiffEntry* Entry = State->AdditionalBlueprints.Find(BlueprintPath))
	{
		Baseline = &Entry->BaselineNodes;
	}
	if (!Baseline)
	{
		// No baseline was taken for this blueprint in this session — we can't prove
		// the node is agent-created, so be conservative and treat it as not.
		return false;
	}
	for (const FNodeSnapshot& Snap : *Baseline)
	{
		if (Snap.NodeGuid == Node->NodeGuid)
		{
			return false; // present in baseline → pre-session / user node
		}
	}
	return true;
}

void FKismetAIModule::RecordUnanchoredSpawns(const FString& BlueprintPath, const TSet<UEdGraphNode*>& IslandNodes)
{
	if (IslandNodes.Num() == 0 || Session->GetSessionId().IsEmpty()) return;
	FDiffState* State = DiffStates.Find(Session->GetSessionId());
	if (!State) return;
	for (UEdGraphNode* Node : IslandNodes)
	{
		if (!Node) continue;
		// Only track nodes the agent actually created this session.
		if (!IsAgentCreatedNode(BlueprintPath, Node)) continue;
		State->UnanchoredSpawnPositions.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
	}
}

void FKismetAIModule::MaybeRepositionOnFirstWire(UEdGraph* Graph, const FString& BlueprintPath,
	UEdGraphNode* WiredNode, UEdGraphNode* PeerNode)
{
	if (!Graph || !WiredNode || Session->GetSessionId().IsEmpty()) return;
	FDiffState* State = DiffStates.Find(Session->GetSessionId());
	if (!State) return;

	if (!State->UnanchoredSpawnPositions.Contains(WiredNode->NodeGuid)) return; // not a tracked island

	// A wire to a node that is ITSELF still an unanchored island does not anchor
	// anything to the existing graph — it just links two stranded nodes. Leave both
	// marks in place and wait for a real anchor (a pre-existing node, or a new node
	// that has already been placed). Acting now would consume the island marks early
	// and strand the upstream feeders when the chain's end finally reaches content.
	if (PeerNode && State->UnanchoredSpawnPositions.Contains(PeerNode->NodeGuid)) return;

	// A node is a movable island iff it is still tracked, still sitting exactly at its
	// recorded spawn spot (the user/a prior op never nudged it), and agent-created.
	auto StillParked = [&](UEdGraphNode* N) -> bool
	{
		const FIntPoint* S = N ? State->UnanchoredSpawnPositions.Find(N->NodeGuid) : nullptr;
		return S && N->NodePosX == S->X && N->NodePosY == S->Y && IsAgentCreatedNode(BlueprintPath, N);
	};

	// If the wired node was nudged since spawn, respect that: drop its mark and bail.
	if (!StillParked(WiredNode))
	{
		State->UnanchoredSpawnPositions.Remove(WiredNode->NodeGuid);
		return;
	}

	// Gather the whole connected component of still-parked island nodes reachable from
	// the wired node. Wiring a multi-node sub-chain's END into existing content must
	// pull the ENTIRE sub-chain along, not just the one node that happened to be wired
	// — otherwise the upstream feeders (themselves linked only new->new) stay stranded
	// off to the right with long wires. The peer (real anchor) is never in this set, so
	// it stays put as an immovable obstacle.
	TSet<UEdGraphNode*> Cluster;
	TArray<UEdGraphNode*> Stack;
	Cluster.Add(WiredNode);
	Stack.Add(WiredNode);
	while (Stack.Num() > 0)
	{
		UEdGraphNode* Cur = Stack.Pop();
		for (UEdGraphPin* Pin : Cur->Pins)
		{
			if (!Pin) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				UEdGraphNode* Other = Linked ? Linked->GetOwningNode() : nullptr;
				if (!Other || Cluster.Contains(Other)) continue;
				if (StillParked(Other))
				{
					Cluster.Add(Other);
					Stack.Add(Other);
				}
			}
		}
	}

	// One-shot: every node in the cluster is now (transitively) wired to anchored
	// content, so drop the island marks before re-placing.
	for (UEdGraphNode* N : Cluster)
	{
		State->UnanchoredSpawnPositions.Remove(N->NodeGuid);
		N->Modify();
	}

	// Re-place the whole cluster as one unit. The real link to anchored content is
	// already in place, so the anchor machinery lands the cluster adjacent to its peer.
	// Force the internal hierarchical layout: the cluster's current coordinates are
	// throwaway island-parking spots, so re-flow them into a clean left->right chain
	// rather than preserving that arrangement. Pre-existing nodes stay immovable.
	FKismetAILayout::PlaceNewNodes(Graph, Cluster, nullptr, /*bForceInternalLayout=*/true);
}

void FKismetAIModule::SyncFactoryWithDiffState()
{
	if (!DiffNodeFactory.IsValid()) return;

	DiffNodeFactory->Clear();

	FDiffState* State = GetActiveDiffState();
	if (State && State->bHasPendingDiff)
	{
		for (const FGuid& Guid : State->AddedNodeGuids)
		{
			DiffNodeFactory->AddedNodeGuids.Add(Guid);
		}
		for (const FGuid& Guid : State->ModifiedNodeGuids)
		{
			DiffNodeFactory->ModifiedNodeGuids.Add(Guid);
		}
	}
}

void FKismetAIModule::RefreshDiffedGraphNodes()
{
	FDiffState* State = GetActiveDiffState();
	if (!State || State->BlueprintPath.IsEmpty()) return;

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(State->BlueprintPath);
	if (!Blueprint) return;

	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, State->GraphName);
	if (!Graph) return;

	Graph->NotifyGraphChanged();
}

void FKismetAIModule::AcceptChanges()
{
	FDiffState* State = GetActiveDiffState();
	if (!State || !State->bHasPendingDiff) return;

	// Clear comment bubble fallbacks on event/tunnel nodes
	UBlueprint* BP = KismetAIUtils::FindBlueprint(State->BlueprintPath);
	if (BP)
	{
		UEdGraph* Graph = KismetAIUtils::FindGraph(BP, State->GraphName);
		if (Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeComment == TEXT("Added by Kismet"))
				{
					Node->NodeComment.Empty();
					Node->bCommentBubblePinned = false;
					Node->bCommentBubbleVisible = false;
				}
			}
		}
	}

	// The run's changes are kept: its checkpoints are no longer needed.
	const FString RunId = GetDiffRunId(Session->GetSessionId());
	if (!RunId.IsEmpty())
	{
		Checkpoints->Accept(Session->GetSessionId(), RunId);
	}

	State->bHasPendingDiff = false;
	State->bCanReapply = false;
	State->AddedNodeNames.Empty();
	State->AddedNodeGuids.Empty();
	State->ModifiedNodeGuids.Empty();
	State->BaselineNodes.Empty();
	State->RunId.Empty();

	// Clear additional blueprint tracking
	State->AdditionalBlueprints.Empty();

	SyncFactoryWithDiffState();
	RefreshDiffedGraphNodes();
	SaveDiffStates();

	BroadcastJS(TEXT("window.onDiffResolved && window.onDiffResolved('accepted')"));
	KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Changes accepted"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Reject / Reapply through run checkpoints (KismetAICheckpoints.h).
//
// Reject used to delete the nodes the run added (by name) and put back pin
// defaults from an in-memory baseline: a node the run deleted stayed deleted, a
// link or variable it changed stayed changed, an edit made after the run was
// overwritten, the file the autosave had written was untouched, and nothing
// survived a restart. It always said "Changes rejected". Now every asset the
// run checkpointed is restored as a whole — or the reason it was not is shown.
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIModule::SetCheckpointRootForTests(const FString& Root)
{
	check(IsInGameThread());
	Checkpoints = MakeUnique<FKismetAICheckpointStore>(Root);
}

FString FKismetAIModule::GetDiffRunId(const FString& SessionId) const
{
	if (const FDiffState* State = DiffStates.Find(SessionId))
	{
		if (!State->RunId.IsEmpty())
		{
			return State->RunId;
		}
	}
	return Checkpoints.IsValid() ? Checkpoints->FindLatestPendingRun(SessionId) : FString();
}

TArray<FKismetAIAssetFingerprint> FKismetAIModule::CaptureAssetStates(const TArray<FString>& PackagePaths)
{
	TArray<FKismetAIAssetFingerprint> Out;
	for (const FString& Path : PackagePaths)
	{
		Out.Add(KismetAIAssetState::Capture(Path));
	}
	return Out;
}

TArray<FKismetAIAssetFingerprint> FKismetAIModule::BeginAgentMutation(const FKismetAIMutationContext& Context)
{
	if (Context.SessionId.IsEmpty() || Context.RunId.IsEmpty())
	{
		return CaptureAssetStates(Context.Targets);
	}
	GetOrCreateDiffState(Context.SessionId).RunId = Context.RunId;
	for (const FKismetAIAssetFingerprint& State : CaptureAssetStates(Context.Targets))
	{
		Checkpoints->BeginMutation(Context.SessionId, Context.RunId, State.AssetPath, State);
	}
	// Captured again: writing a checkpoint can compile an out-of-date
	// Blueprint, and the intent must record the state the call starts from.
	return CaptureAssetStates(Context.Targets);
}

void FKismetAIModule::EndAgentMutation(const FKismetAIMutationContext& Context)
{
	if (Context.SessionId.IsEmpty() || Context.RunId.IsEmpty())
	{
		return;
	}
	for (const FKismetAIAssetFingerprint& State : CaptureAssetStates(Context.Targets))
	{
		Checkpoints->EndMutation(Context.SessionId, Context.RunId, State.AssetPath, State);
	}
}

void FKismetAIModule::FinalizeRunDiff(const FString& RunId)
{
	const FString SessionId = Session->GetSessionId();
	TArray<FString> Assets;
	if (!RunId.IsEmpty() && !SessionId.IsEmpty())
	{
		for (const FKismetAICheckpointInfo& Info : Checkpoints->ListRun(SessionId, RunId))
		{
			if (Info.Status == TEXT("pending") && !Info.AssetPath.IsEmpty())
			{
				Assets.Add(Info.AssetPath);
			}
		}
	}

	// A change between the run's last call and its end was not the run's.
	for (const FKismetAIAssetFingerprint& State : CaptureAssetStates(Assets))
	{
		Checkpoints->BeginMutation(SessionId, RunId, State.AssetPath, State);
	}
	ComputeDiffAndMarkNodes();
	// The diff markers ("Added by Kismet") are part of what Reject undoes.
	TArray<FString> Changed;
	for (const FKismetAIAssetFingerprint& State : CaptureAssetStates(Assets))
	{
		Checkpoints->EndMutation(SessionId, RunId, State.AssetPath, State);
	}
	for (const FKismetAICheckpointInfo& Info : Checkpoints->ListRun(SessionId, RunId))
	{
		if (Info.Status == TEXT("pending") && Info.bRestorable && Info.bChangedByRun)
		{
			Changed.Add(Info.AssetPath);
		}
	}

	// Reject is offered whenever a checkpointed asset changed, not only when a
	// node of the primary graph did (a variable, a material, another Blueprint).
	FDiffState* State = Changed.Num() > 0 ? &GetOrCreateDiffState(SessionId) : nullptr;
	if (State && !State->bHasPendingDiff)
	{
		State->bHasPendingDiff = true;
		State->bCanReapply = false;
		State->RunId = RunId;
		if (State->BlueprintPath.IsEmpty())
		{
			State->BlueprintPath = Changed[0];
		}
		SaveDiffStates();
		BroadcastJS(FString::Printf(
			TEXT("window.onDiffReady && window.onDiffReady(%d, false, '%s', %d)"),
			Changed.Num(), *Changed[0].ReplaceCharWithEscapedChar(), Changed.Num() - 1));
	}
}

TArray<FKismetAIRestoreReport> FKismetAIModule::RestoreRunCheckpoints(const FString& SessionId, const FString& RunId, bool bReapply)
{
	TArray<FKismetAIRestoreReport> Reports;
	const FString Run = RunId.IsEmpty() ? GetDiffRunId(SessionId) : RunId;
	const TArray<FKismetAICheckpointInfo> Infos = Run.IsEmpty() ? TArray<FKismetAICheckpointInfo>() : Checkpoints->ListRun(SessionId, Run);
	for (const FKismetAICheckpointInfo& Info : Infos)
	{
		if (Info.Status == TEXT("accepted"))
		{
			continue;
		}
		if (Info.AssetPath.IsEmpty())
		{
			FKismetAIRestoreReport Corrupt;
			Corrupt.SessionId = SessionId;
			Corrupt.RunId = Run;
			Corrupt.AssetPath = Info.Directory;
			Corrupt.Status = EKismetAIRestoreStatus::CheckpointCorrupt;
			Corrupt.MemoryState = TEXT("unchanged");
			Corrupt.DiskState = TEXT("unchanged");
			Corrupt.Explanation = TEXT("A checkpoint manifest of this run cannot be read, so the asset it covers is unknown. Nothing was restored for it.");
			Reports.Add(MoveTemp(Corrupt));
			continue;
		}
		Reports.Add(Checkpoints->Restore(SessionId, Run, Info.AssetPath,
			bReapply ? FKismetAICheckpointStore::ETarget::RunResult : FKismetAICheckpointStore::ETarget::BeforeRun));
	}
	if (Reports.Num() == 0)
	{
		FKismetAIRestoreReport Missing;
		Missing.SessionId = SessionId;
		Missing.RunId = Run;
		const FDiffState* State = DiffStates.Find(SessionId);
		Missing.AssetPath = State ? State->BlueprintPath : FString();
		Missing.Status = EKismetAIRestoreStatus::CheckpointUnavailable;
		Missing.MemoryState = TEXT("unchanged");
		Missing.DiskState = TEXT("unchanged");
		Missing.Explanation = TEXT("No checkpoint exists for this session's latest run (deleted, never written, or the run changed nothing). Nothing was restored; use Edit > Undo or source control.");
		Reports.Add(MoveTemp(Missing));
	}
	return Reports;
}

void FKismetAIModule::BroadcastCheckpointReports(const TArray<FKismetAIRestoreReport>& Reports)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FKismetAIRestoreReport& Report : Reports)
	{
		Items.Add(MakeShared<FJsonValueObject>(Report.ToJson()));
	}
	FString Json;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Items, Writer);
	BroadcastJS(FString::Printf(TEXT("window.onCheckpointReport && window.onCheckpointReport(%s)"), *Json));
}

void FKismetAIModule::RejectChanges()
{
	FDiffState* State = GetActiveDiffState();
	if (!State || !State->bHasPendingDiff) return;
	const FString SessionId = Session->GetSessionId();

	// Restoring reloads packages: the tinted node objects are replaced.
	if (DiffNodeFactory.IsValid())
	{
		DiffNodeFactory->Clear();
	}
	const TArray<FKismetAIRestoreReport> Reports = RestoreRunCheckpoints(SessionId, State->RunId, /*bReapply=*/false);
	State = GetActiveDiffState();
	if (!State) return;

	bool bAllRestored = true;
	bool bAnyResultKept = false;
	for (const FKismetAIRestoreReport& Report : Reports)
	{
		bAllRestored &= Report.Succeeded();
		bAnyResultKept |= Report.Succeeded();
	}

	if (bAllRestored)
	{
		State->bHasPendingDiff = false;
		State->bCanReapply = bAnyResultKept;
		State->AddedNodeNames.Empty();
		State->AddedNodeGuids.Empty();
		State->ModifiedNodeGuids.Empty();
		State->BaselineNodes.Empty();
		State->AdditionalBlueprints.Empty();
	}
	// Otherwise the pending diff stays: the run's changes (or part of them)
	// are still in the asset, and Accept or a manual fix is the user's call.

	SyncFactoryWithDiffState();
	RefreshDiffedGraphNodes();
	SaveDiffStates();

	BroadcastCheckpointReports(Reports);
	if (bAllRestored)
	{
		BroadcastJS(FString::Printf(
			TEXT("window.onDiffResolved && window.onDiffResolved('rejected', %s)"),
			State->bCanReapply ? TEXT("true") : TEXT("false")));
	}
	KISMETAI_LOG(LogKismetAI, Log, TEXT("Reject: %d asset(s), %s"), Reports.Num(), bAllRestored ? TEXT("all restored") : TEXT("not all restored"));
}

void FKismetAIModule::ReapplyChanges()
{
	FDiffState* State = GetActiveDiffState();
	if (!State || !State->bCanReapply) return;
	const FString SessionId = Session->GetSessionId();

	const TArray<FKismetAIRestoreReport> Reports = RestoreRunCheckpoints(SessionId, State->RunId, /*bReapply=*/true);
	State = GetActiveDiffState();
	if (!State) return;

	int32 Restored = 0;
	for (const FKismetAIRestoreReport& Report : Reports)
	{
		Restored += Report.Succeeded() ? 1 : 0;
	}
	BroadcastCheckpointReports(Reports);
	if (Restored > 0)
	{
		State->bHasPendingDiff = true;
		State->bCanReapply = Restored < Reports.Num();
		SaveDiffStates();
		BroadcastJS(FString::Printf(
			TEXT("window.onDiffReady && window.onDiffReady(%d, false, '%s', %d)"),
			Restored, *State->BlueprintPath.ReplaceCharWithEscapedChar(), Restored - 1));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Diff state persistence (Saved/KismetAI/diff_state.json)
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIModule::SaveDiffStates()
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Sessions = MakeShared<FJsonObject>();

	for (const auto& Pair : DiffStates)
	{
		const FString& SessId = Pair.Key;
		const FDiffState& State = Pair.Value;

		// Only persist states that have pending diff or reapply data
		if (!State.bHasPendingDiff && !State.bCanReapply) continue;

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("blueprint"), State.BlueprintPath);
		Entry->SetStringField(TEXT("graph"), State.GraphName);
		Entry->SetBoolField(TEXT("has_pending_diff"), State.bHasPendingDiff);
		Entry->SetBoolField(TEXT("can_reapply"), State.bCanReapply);

		if (!State.RunId.IsEmpty())
		{
			Entry->SetStringField(TEXT("run_id"), State.RunId);
		}

		TArray<TSharedPtr<FJsonValue>> GuidArray;
		for (const FGuid& Guid : State.AddedNodeGuids)
		{
			GuidArray.Add(MakeShared<FJsonValueString>(Guid.ToString()));
		}
		Entry->SetArrayField(TEXT("added_node_guids"), GuidArray);

		TArray<TSharedPtr<FJsonValue>> NameArray;
		for (const FString& Name : State.AddedNodeNames)
		{
			NameArray.Add(MakeShared<FJsonValueString>(Name));
		}
		Entry->SetArrayField(TEXT("added_node_names"), NameArray);

		Sessions->SetObjectField(SessId, Entry);
	}

	Root->SetObjectField(TEXT("sessions"), Sessions);

	FString Json;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	FString Path = FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("diff_state.json");
	FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, true);
	FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FKismetAIModule::LoadDiffStates()
{
	FString Path = FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("diff_state.json");
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path)) return;

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;

	const TSharedPtr<FJsonObject>* SessionsObj;
	if (!Root->TryGetObjectField(TEXT("sessions"), SessionsObj)) return;

	for (const auto& Pair : (*SessionsObj)->Values)
	{
		const FString SessId = KismetJsonKey(Pair.Key);
		TSharedPtr<FJsonObject> Entry = Pair.Value->AsObject();
		if (!Entry.IsValid()) continue;

		FDiffState State;
		Entry->TryGetStringField(TEXT("blueprint"), State.BlueprintPath);
		Entry->TryGetStringField(TEXT("graph"), State.GraphName);
		Entry->TryGetBoolField(TEXT("has_pending_diff"), State.bHasPendingDiff);
		Entry->TryGetBoolField(TEXT("can_reapply"), State.bCanReapply);
		Entry->TryGetStringField(TEXT("run_id"), State.RunId);

		const TArray<TSharedPtr<FJsonValue>>* GuidArray;
		if (Entry->TryGetArrayField(TEXT("added_node_guids"), GuidArray))
		{
			for (const auto& Val : *GuidArray)
			{
				FGuid Guid;
				if (FGuid::Parse(Val->AsString(), Guid))
				{
					State.AddedNodeGuids.Add(Guid);
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* NameArray;
		if (Entry->TryGetArrayField(TEXT("added_node_names"), NameArray))
		{
			for (const auto& Val : *NameArray)
			{
				State.AddedNodeNames.Add(Val->AsString());
			}
		}

		// Validate: if pending diff, check nodes still exist in graph. A diff
		// backed by run checkpoints (RunId) is validated by RecoverPendingCheckpoints.
		if (State.bHasPendingDiff && !State.BlueprintPath.IsEmpty() && State.RunId.IsEmpty())
		{
			UBlueprint* BP = KismetAIUtils::FindBlueprint(State.BlueprintPath);
			if (BP)
			{
				UEdGraph* Graph = KismetAIUtils::FindGraph(BP, State.GraphName);
				if (Graph)
				{
					TSet<FGuid> ExistingGuids;
					for (UEdGraphNode* Node : Graph->Nodes)
					{
						if (Node) ExistingGuids.Add(Node->NodeGuid);
					}
					// Remove stale GUIDs
					State.AddedNodeGuids.RemoveAll([&ExistingGuids](const FGuid& G) { return !ExistingGuids.Contains(G); });
					if (State.AddedNodeGuids.Num() == 0)
					{
						State.bHasPendingDiff = false;
					}
				}
			}
		}

		DiffStates.Add(SessId, MoveTemp(State));
	}

	KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Loaded diff states for %d sessions"), DiffStates.Num());
}

void FKismetAIModule::RecoverPendingCheckpoints()
{
	// After a crash or restart the in-memory diff baseline is gone, but a run's
	// checkpoints are on disk. Every session whose newest run still has a
	// pending checkpoint of a changed asset gets its Reject back.
	int32 Recovered = 0;
	TSet<FString> Seen;
	for (const FKismetAICheckpointInfo& Info : Checkpoints->ListPending())
	{
		if (Info.SessionId.IsEmpty() || Seen.Contains(Info.SessionId) || Info.Status != TEXT("pending"))
		{
			continue;
		}
		Seen.Add(Info.SessionId);
		const FString RunId = Checkpoints->FindLatestPendingRun(Info.SessionId);
		bool bChanged = false;
		FString FirstAsset;
		for (const FKismetAICheckpointInfo& Asset : Checkpoints->ListRun(Info.SessionId, RunId))
		{
			if (Asset.Status == TEXT("pending") && Asset.bChangedByRun)
			{
				bChanged = true;
				if (FirstAsset.IsEmpty()) { FirstAsset = Asset.AssetPath; }
			}
		}
		if (!bChanged)
		{
			continue;
		}
		FDiffState& State = GetOrCreateDiffState(Info.SessionId);
		if (State.bHasPendingDiff && State.RunId == RunId)
		{
			continue;
		}
		State.bHasPendingDiff = true;
		State.bCanReapply = false;
		State.RunId = RunId;
		if (State.BlueprintPath.IsEmpty())
		{
			State.BlueprintPath = FirstAsset;
		}
		++Recovered;
	}
	if (Recovered > 0)
	{
		KISMETAI_LOG(LogKismetAI, Log, TEXT("Recovered %d session(s) with restorable run checkpoints"), Recovered);
		SaveDiffStates();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// UI registration
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIModule::RegisterTabSpawner()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		KismetAIPanelTabId,
		FOnSpawnTab::CreateRaw(this, &FKismetAIModule::SpawnKismetAITab))
		.SetDisplayName(NSLOCTEXT("KismetAI", "TabTitle", "Kismet"))
		.SetTooltipText(NSLOCTEXT("KismetAI", "TabTooltip", "AI-driven Blueprint editing"))
		.SetIcon(FSlateIcon(FKismetAIStyle::GetStyleSetName(), "KismetAI.ToolbarIcon"))
		.SetMenuType(ETabSpawnerMenuType::Enabled);
}

void FKismetAIModule::UnregisterTabSpawner()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(KismetAIPanelTabId);
}

TSharedRef<SDockTab> FKismetAIModule::SpawnKismetAITab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(NomadTab)
		[
			SNew(SKismetAIPanel)
		];
}

// ─────────────────────────────────────────────────────────────────────────────
// Manual format handler (toolbar button + hotkey)
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIModule::FormatFocusedGraph(UEdGraph* Graph)
{
	if (!Graph)
	{
		UE_LOG(LogKismetAI, Warning, TEXT("FormatFocusedGraph: no focused graph"));
		return;
	}

	const FScopedTransaction Tx(NSLOCTEXT("KismetAI", "FormatGraphTx", "Format Graph"));
	Graph->Modify();
	for (UEdGraphNode* N : Graph->Nodes)
	{
		if (N)
		{
			N->Modify();
		}
	}
	FKismetAILayout::FormatNodes(Graph, {});
	Graph->NotifyGraphChanged();

	if (UBlueprint* BP = FBlueprintEditorUtils::FindBlueprintForGraph(Graph))
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
	}
}

void FKismetAIModule::RegisterToolbarButton()
{
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateLambda([]()
	{
		// Level Editor toolbar — opens global nomad tab
		if (UToolMenu* LevelEditorMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar"))
		{
			FToolMenuSection& Section = LevelEditorMenu->FindOrAddSection("KismetAI");
			Section.AddEntry(FToolMenuEntry::InitToolBarButton(
				"OpenKismetAI",
				FUIAction(FExecuteAction::CreateLambda([]()
				{
					FGlobalTabmanager::Get()->TryInvokeTab(FKismetAIModule::KismetAIPanelTabId);
				})),
				NSLOCTEXT("KismetAI", "ToolbarButton", "Kismet"),
				NSLOCTEXT("KismetAI", "ToolbarButtonTooltip", "Open Kismet Panel"),
				FSlateIcon(FKismetAIStyle::GetStyleSetName(), "KismetAI.ToolbarIcon")
			));
		}

		// Blueprint/Asset Editor toolbar — opens docked tab in the editor's local tab manager
		if (UToolMenu* AssetEditorMenu = UToolMenus::Get()->ExtendMenu("AssetEditorToolbar.CommonActions"))
		{
			FToolMenuSection& Section = AssetEditorMenu->FindOrAddSection("KismetAI");
			Section.AddEntry(FToolMenuEntry::InitToolBarButton(
				"OpenKismetAI",
				FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
				{
					if (UAssetEditorToolkitMenuContext* EditorContext = Context.FindContext<UAssetEditorToolkitMenuContext>())
					{
						if (EditorContext->Toolkit.IsValid())
						{
							EditorContext->Toolkit.Pin()->GetTabManager()->TryInvokeTab(FKismetAIModule::KismetAIPanelTabId);
							return;
						}
					}
					FGlobalTabmanager::Get()->TryInvokeTab(FKismetAIModule::KismetAIPanelTabId);
				}),
				NSLOCTEXT("KismetAI", "ToolbarButton", "Kismet"),
				NSLOCTEXT("KismetAI", "ToolbarButtonTooltip", "Open Kismet Panel"),
				FSlateIcon(FKismetAIStyle::GetStyleSetName(), "KismetAI.ToolbarIcon")
			));

			// Format Graph button — auto-layouts the focused Blueprint graph
			Section.AddEntry(FToolMenuEntry::InitToolBarButton(
				"KismetFormatGraph",
				FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& Context)
				{
					if (UAssetEditorToolkitMenuContext* Ctx = Context.FindContext<UAssetEditorToolkitMenuContext>())
					{
						if (Ctx->Toolkit.IsValid())
						{
							// CommonActions shows in EVERY asset editor (Material, DataTable, …),
							// so guard the downcast — only Blueprint editors have a graph.
							TSharedPtr<IToolkit> Toolkit = Ctx->Toolkit.Pin();
							if (Toolkit.IsValid() && Toolkit->IsBlueprintEditor())
							{
								FBlueprintEditor* BPEd = static_cast<FBlueprintEditor*>(Toolkit.Get());
								FKismetAIModule::FormatFocusedGraph(BPEd->GetFocusedGraph());
							}
						}
					}
				}),
				NSLOCTEXT("KismetAI", "FormatGraphButton", "Format Graph"),
				NSLOCTEXT("KismetAI", "FormatGraphButtonTooltip", "Auto-layout the current Blueprint graph (Ctrl+Shift+L)"),
				// Built-in graph "straighten connections" glyph — reads as "tidy the
				// graph" and is distinct from the Kismet panel button's icon.
				FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.StraightenConnections")
			));
		}
	}));
}

void FKismetAIModule::RegisterBlueprintEditorExtension()
{
	FBlueprintEditorModule& BlueprintEditorModule = FModuleManager::LoadModuleChecked<FBlueprintEditorModule>("Kismet");

	BlueprintEditorTabsHandle = BlueprintEditorModule.OnRegisterTabsForEditor().AddRaw(
		this, &FKismetAIModule::HandleRegisterBlueprintEditorTabs);

	BlueprintEditorLayoutHandle = BlueprintEditorModule.OnRegisterLayoutExtensions().AddRaw(
		this, &FKismetAIModule::HandleRegisterBlueprintEditorLayout);
}

void FKismetAIModule::HandleRegisterBlueprintEditorTabs(
	FWorkflowAllowedTabSet& TabSet, FName ModeName, TSharedPtr<FBlueprintEditor> BlueprintEditor)
{
	TabSet.RegisterFactory(MakeShared<FKismetAIBlueprintTabFactory>(StaticCastSharedPtr<FAssetEditorToolkit>(BlueprintEditor)));

	// Map the Format Graph command into this editor's scoped command list so that
	// Ctrl/Cmd+Shift+L works when the Blueprint editor is focused.
	if (BlueprintEditor.IsValid() &&
		!BlueprintEditor->GetToolkitCommands()->IsActionMapped(FKismetAILayoutCommands::Get().FormatGraph))
	{
		BlueprintEditor->GetToolkitCommands()->MapAction(
			FKismetAILayoutCommands::Get().FormatGraph,
			FExecuteAction::CreateLambda([BPWeak = TWeakPtr<FBlueprintEditor>(BlueprintEditor)]()
			{
				if (TSharedPtr<FBlueprintEditor> E = BPWeak.Pin())
				{
					FKismetAIModule::FormatFocusedGraph(E->GetFocusedGraph());
				}
			}));
	}
}

void FKismetAIModule::HandleRegisterBlueprintEditorLayout(FLayoutExtender& Extender)
{
	Extender.ExtendLayout(
		FBlueprintEditorTabs::DetailsID,
		ELayoutExtensionPosition::After,
		FTabManager::FTab(FKismetAIModule::KismetAIPanelTabId, ETabState::ClosedTab));
}

// ─────────────────────────────────────────────────────────────────────────────
// HTTP server management
// ─────────────────────────────────────────────────────────────────────────────

static TOptional<FToolExecutionResult> GetEditorAccessError()
{
	// A game-thread hop alone does not permit reentrant object lookup or
	// mutation while serialization/GC owns the object graph.
	if (!IsInGameThread())
	{
		return FToolExecutionResult::Error(503, TEXT("EDITOR_BUSY"),
			TEXT("Editor object work must run on the game thread. Retry the request."));
	}
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 8)
	const bool bSaving = UE::IsSavingPackage();
#else
	const bool bSaving = GIsSavingPackage;
#endif
	// Validation opens its slow-task dialog on the tick after saving. The
	// renderer can pump tools before that modal window finishes construction.
	if (bSaving || IsGarbageCollecting() || GIsSlowTask)
	{
		return FToolExecutionResult::Error(503, TEXT("EDITOR_BUSY"),
			TEXT("The editor is saving, collecting garbage, or running a progress task. No tool work was performed."),
			{ TEXT("Retry after the editor finishes its current operation; do not disable autosave.") });
	}
	return {};
}

TOptional<FToolExecutionResult> FKismetAIModule::GetEditorObjectAccessError()
{
	return GetEditorAccessError();
}

void FKismetAIModule::RunWhenObjectAccessAllowed(TFunction<void()> Work)
{
	check(IsInGameThread());
	if (!GetEditorAccessError().IsSet())
	{
		Work();
		return;
	}
	// Not re-queued as another AsyncTask: inside a save the task queue is being
	// drained until idle, so a task that re-posts itself would spin there. The
	// core ticker runs from the engine loop, after the save has returned.
	const uint64 Id = ++NextDeferredObjectWorkId;
	const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([this, Id](float) -> bool
		{
			if (GetEditorAccessError().IsSet())
			{
				return true;
			}
			TPair<FTSTicker::FDelegateHandle, TFunction<void()>> Entry;
			if (DeferredObjectWork.RemoveAndCopyValue(Id, Entry))
			{
				Entry.Value();
			}
			return false;
		}));
	DeferredObjectWork.Add(Id, TPair<FTSTicker::FDelegateHandle, TFunction<void()>>(Handle, MoveTemp(Work)));
}

void FKismetAIModule::StartHttpServer()
{
	if (!SourceJobs.IsValid()) SourceJobs = MakeShared<FKismetAISourceJobs>();
	if (bIsRunning)
	{
		return;
	}

	if (!HttpStartupRetryHandle.IsValid())
	{
		// Allow overriding the listen port via KISMETAI_PORT so multiple editor
		// instances (e.g. parallel worktrees) can each run their own HTTP server.
		const FString PortEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_PORT"));
		if (!PortEnv.IsEmpty())
		{
			const int32 ParsedPort = FCString::Atoi(*PortEnv);
			if (ParsedPort > 0 && ParsedPort <= 65535)
			{
				ServerPort = static_cast<uint32>(ParsedPort);
				KISMETAI_LOG(LogKismetAI, Log, TEXT("HTTP port overridden via KISMETAI_PORT=%d"), ServerPort);
			}
			else
			{
				KISMETAI_LOG(LogKismetAI, Warning, TEXT("Ignoring invalid KISMETAI_PORT='%s' — using default %d"), *PortEnv, ServerPort);
			}
		}

	}

	HttpRouter = MakeShared<FKismetAIHttpTransport>();
	if (!HttpRouter->Start(ServerPort, TEXT("127.0.0.1")))
	{
		if (!HttpStartupRetryHandle.IsValid())
		{
			KISMETAI_LOG(LogKismetAI, Warning, TEXT("HTTP port %d is unavailable. Waiting up to 120 seconds for it to become free."), ServerPort);
			HttpStartupRetryDeadline = FPlatformTime::Seconds() + 120.0;
			HttpStartupRetryHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([this](float)
				{
					if (FPlatformTime::Seconds() >= HttpStartupRetryDeadline)
					{
						KISMETAI_LOG(LogKismetAI, Error, TEXT("HTTP server could not bind port %d after 120 seconds. Close the editor or application using this port, or set KISMETAI_PORT to a free port and restart."), ServerPort);
						HttpStartupRetryHandle.Reset();
						return false;
					}
					StartHttpServer();
					if (bIsRunning)
					{
						HttpStartupRetryHandle.Reset();
						return false;
					}
					return true;
				}), 2.0f);
		}
		return;
	}

	// Wrap every handler with the X-Kismet-Token + Origin gate. The bare
	// FHttpRequestHandler::CreateRaw form skips it; do not use it for new
	// routes — every route here has to be unreachable from a browser tab.
	// 1 MB body cap stops a single POST from forcing the editor to allocate
	// hundreds of MB inside ResolvePlaceholders / ImportNodesFromText.
	constexpr int32 MaxRequestBodyBytes = 1 * 1024 * 1024;
	auto WithAuth = [this, MaxRequestBodyBytes](bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&)) -> FHttpRequestHandler
	{
		return FHttpRequestHandler::CreateLambda(
			[this, Handler, MaxRequestBodyBytes](const FHttpServerRequest& Req, const FHttpResultCallback& Cb) -> bool
			{
				if (!KismetAIServerAuth::CheckRequest(Req, Cb)) return true;
				if (Req.Body.Num() > MaxRequestBodyBytes)
				{
					Cb(FHttpServerResponse::Error(
						EHttpServerResponseCodes::RequestTooLarge,
						TEXT("body_too_large"),
						TEXT("Request body exceeds 1 MiB cap")));
					return true;
				}
				if (const TOptional<FToolExecutionResult> Error = GetEditorAccessError())
				{
					SendError(Cb, Error->StatusCode, Error->ErrorCode, Error->Message, Error->Suggestions);
					return true;
				}
				return (this->*Handler)(Req, Cb);
			});
	};

	// Run inspection and cancel read and write only in-memory run
	// records (and cancel a run token), never a UObject, so they skip the
	// editor-state check: a client must be able to watch or stop a run while
	// the editor is saving or collecting garbage.
	auto WithTokenOnly = [this, MaxRequestBodyBytes](bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&)) -> FHttpRequestHandler
	{
		return FHttpRequestHandler::CreateLambda(
			[this, Handler, MaxRequestBodyBytes](const FHttpServerRequest& Req, const FHttpResultCallback& Cb) -> bool
			{
				if (!KismetAIServerAuth::CheckRequest(Req, Cb)) return true;
				if (Req.Body.Num() > MaxRequestBodyBytes)
				{
					Cb(FHttpServerResponse::Error(
						EHttpServerResponseCodes::RequestTooLarge,
						TEXT("body_too_large"),
						TEXT("Request body exceeds 1 MiB cap")));
					return true;
				}
				return (this->*Handler)(Req, Cb);
			});
	};

	// Tool routes. Same transport gate as WithAuth — token, body cap,
	// early editor-state check — and then the shared execution service rather
	// than a Do* handler: ServeToolRoute runs the request through
	// FKismetAIToolExecutor, so REST gets the registry validation, policy,
	// cancellation and game-thread admission the agent and MCP get. The
	// X-Kismet-Token check having passed is what the caller context records.
	auto ToolRoute = [this, MaxRequestBodyBytes](const TCHAR* ToolName, const TCHAR* Mode) -> FHttpRequestHandler
	{
		return FHttpRequestHandler::CreateLambda(
			[this, ToolName, Mode, MaxRequestBodyBytes](const FHttpServerRequest& Req, const FHttpResultCallback& Cb) -> bool
			{
				if (!KismetAIServerAuth::CheckRequest(Req, Cb)) return true;
				if (Req.Body.Num() > MaxRequestBodyBytes)
				{
					Cb(FHttpServerResponse::Error(
						EHttpServerResponseCodes::RequestTooLarge,
						TEXT("body_too_large"),
						TEXT("Request body exceeds 1 MiB cap")));
					return true;
				}
				if (const TOptional<FToolExecutionResult> Error = GetEditorAccessError())
				{
					SendError(Cb, Error->StatusCode, Error->ErrorCode, Error->Message, Error->Suggestions);
					return true;
				}
				return ServeToolRoute(ToolName, Mode,
					FKismetAICallerContext::Verified(EKismetAIEntryPoint::Rest, TEXT("x-kismet-token")), Req, Cb);
			});
	};

	RouteHandles.Add(HttpRouter->BindRoute(FHttpPath(TEXT("/api/v1/read_source/cancel")),
		EHttpServerRequestVerbs::VERB_POST, WithTokenOnly(&FKismetAIModule::HandleCancelSource)));

	// Register routes
	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/skills")), EHttpServerRequestVerbs::VERB_POST,
		WithAuth(&FKismetAIModule::HandleSkills)));
	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/status")),
		EHttpServerRequestVerbs::VERB_GET,
		WithAuth(&FKismetAIModule::HandleStatus)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/export_t3d")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_blueprint"), TEXT("t3d"))));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/inject_t3d")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("write_blueprint"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/compile")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("compile_blueprint"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/describe_graph")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_blueprint"), TEXT("describe"))));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/describe_material")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_material"), TEXT("describe"))));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/material_overview")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_material"), TEXT("overview"))));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/compile_material")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("compile_material"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/create_material_expression")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("create_material_expression"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/edit_material")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("edit_material"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/create_material_instance")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("create_material_instance"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/set_material_instance_parameter")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("set_material_instance_parameter"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/manage_material")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("manage_material"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/describe_widget")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_widget"), TEXT("tree"))));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/widget_overview")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_widget"), TEXT("overview"))));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/edit_widget")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("edit_widget"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/edit_graph")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("edit_blueprint"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/layout_graph")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("layout_graph"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/manage_blueprint")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("manage_blueprint"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/manage_components")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("manage_components"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/manage_state_machine")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("manage_state_machine"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/search_assets")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("search_assets"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/get_references")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("get_references"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/inspect_class")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("inspect_class"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/create_asset")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("create_asset"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/manage_asset")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("manage_asset"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/manage_animation")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("manage_animation"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/search_nodes")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("search_nodes"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/insert_node")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("insert_node"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/read_source")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_source"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/read_logs")),
		EHttpServerRequestVerbs::VERB_POST,
		ToolRoute(TEXT("read_logs"), nullptr)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/agent/run")),
		EHttpServerRequestVerbs::VERB_POST,
		WithAuth(&FKismetAIModule::HandleAgentRun)));

	// External runs.
	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/agent/runs")),
		EHttpServerRequestVerbs::VERB_POST,
		WithAuth(&FKismetAIModule::HandleAgentRunSubmit)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/agent/runs/status")),
		EHttpServerRequestVerbs::VERB_GET,
		WithTokenOnly(&FKismetAIModule::HandleAgentRunStatus)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/agent/runs/events")),
		EHttpServerRequestVerbs::VERB_GET,
		WithTokenOnly(&FKismetAIModule::HandleAgentRunEvents)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/agent/runs/cancel")),
		EHttpServerRequestVerbs::VERB_POST,
		WithTokenOnly(&FKismetAIModule::HandleAgentRunCancel)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/agent/answer")),
		EHttpServerRequestVerbs::VERB_POST,
		WithAuth(&FKismetAIModule::HandleAgentAnswer)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/authority")),
		EHttpServerRequestVerbs::VERB_GET,
		WithAuth(&FKismetAIModule::HandleAuthorityList)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/authority/grants")),
		EHttpServerRequestVerbs::VERB_POST,
		WithAuth(&FKismetAIModule::HandleAuthorityGrant)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/authority/revoke")),
		EHttpServerRequestVerbs::VERB_POST,
		WithAuth(&FKismetAIModule::HandleAuthorityRevoke)));

	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/agent/session")),
		EHttpServerRequestVerbs::VERB_GET,
		WithAuth(&FKismetAIModule::HandleGetSession)));

	// Serve clipboard images from a dedicated subdir under Saved/KismetAI/
	// for the chat UI. The previous shape sat at the Saved/KismetAI/ root,
	// where the agent also writes request_body.json / response_sse.txt — any
	// client that could fingerprint a filename could exfiltrate the in-flight
	// Anthropic POST body, including the system prompt and tool results.
	RouteHandles.Add(HttpRouter->BindRoute(
		FHttpPath(TEXT("/api/v1/image")),
		EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			if (!KismetAIServerAuth::CheckRequest(Request, OnComplete)) return true;

			FString Filename;
			if (Request.QueryParams.Contains(TEXT("f")))
			{
				Filename = Request.QueryParams[TEXT("f")];
			}

			auto IsAllowedExt = [](const FString& Name) {
				return Name.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase)
					|| Name.EndsWith(TEXT(".jpg"), ESearchCase::IgnoreCase)
					|| Name.EndsWith(TEXT(".jpeg"), ESearchCase::IgnoreCase);
			};

			if (Filename.IsEmpty()
				|| Filename.Contains(TEXT(".."))
				|| Filename.Contains(TEXT("/"))
				|| Filename.Contains(TEXT("\\"))
				|| Filename.Contains(TEXT(":"))
				|| Filename.StartsWith(TEXT("."))
				|| !IsAllowedExt(Filename))
			{
				OnComplete(FHttpServerResponse::Create(FString(TEXT("Not found")), FString(TEXT("text/plain"))));
				return true;
			}

			const FString ContentType = Filename.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase)
				? TEXT("image/png") : TEXT("image/jpeg");

			FString FilePath = FPaths::ProjectSavedDir() / TEXT("KismetAI") / TEXT("clipboard") / Filename;
			TArray<uint8> FileData;
			if (FFileHelper::LoadFileToArray(FileData, *FilePath))
			{
				OnComplete(FHttpServerResponse::Create(MoveTemp(FileData), ContentType));
			}
			else
			{
				OnComplete(FHttpServerResponse::Create(FString(TEXT("Not found")), FString(TEXT("text/plain"))));
			}
			return true;
		})));

	// MCP endpoint for external agents (Claude Code, Codex CLI, Cursor). Its own
	// auth — loopback peer + Origin guard + Authorization: Bearer from
	// ~/.kismet/mcp_token — not the X-Kismet-Token gate above. KismetAIMcpRoute.h.
	KismetAIMcpRoute::Bind(*this, HttpRouter, RouteHandles);

	bIsRunning = true;

	KISMETAI_LOG(LogKismetAI, Log, TEXT("Kismet HTTP server started on port %d"), ServerPort);
}

void FKismetAIModule::StopHttpServer()
{
	if (SourceJobs.IsValid()) { SourceJobs->Close(); SourceJobs.Reset(); }
	// A pending bind has no running listener, but must not outlive this module.
	FTSTicker::RemoveTicker(HttpStartupRetryHandle);
	HttpStartupRetryHandle.Reset();
	HttpStartupRetryDeadline = 0.0;
	if (!bIsRunning)
	{
		return;
	}

	if (HttpRouter)
	{
		for (const FHttpRouteHandle& Handle : RouteHandles)
		{
			HttpRouter->UnbindRoute(Handle);
		}
		RouteHandles.Empty();
	}

	if (HttpRouter)
	{
		HttpRouter->Stop();
	}

	HttpRouter.Reset();
	bIsRunning = false;

	KISMETAI_LOG(LogKismetAI, Log, TEXT("Kismet HTTP server stopped"));
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON helpers
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIModule::SendSuccess(const FHttpResultCallback& OnComplete, TSharedPtr<FJsonObject> Data)
{
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetBoolField(TEXT("success"), true);
	if (Data)
	{
		Envelope->SetObjectField(TEXT("data"), Data);
	}

	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);

	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
	OnComplete(MoveTemp(Response));
}

void FKismetAIModule::SendError(const FHttpResultCallback& OnComplete, int32 StatusCode, const FString& ErrorCode, const FString& Message, const TArray<FString>& Suggestions)
{
	TSharedPtr<FJsonObject> ErrorObj = MakeShared<FJsonObject>();
	ErrorObj->SetStringField(TEXT("error_code"), ErrorCode);
	ErrorObj->SetStringField(TEXT("message"), Message);

	// Add suggestions
	TArray<TSharedPtr<FJsonValue>> SuggestionsArr;
	if (Suggestions.Num() > 0)
	{
		for (const FString& S : Suggestions)
		{
			SuggestionsArr.Add(MakeShared<FJsonValueString>(S));
		}
	}
	else
	{
		// Auto-generate suggestions based on error code
		if (ErrorCode == TEXT("BLUEPRINT_NOT_FOUND"))
		{
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Check the asset path is correct (e.g. /Game/Blueprints/BP_MyActor)")));
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Use search_assets with class_filter=\"Blueprint\" to find the correct path by name")));
		}
		else if (ErrorCode == TEXT("GRAPH_NOT_FOUND"))
		{
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Common graph names: EventGraph, or function/macro names")));
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Read the blueprint with describe mode to see available graphs")));
		}
		else if (ErrorCode == TEXT("MISSING_FIELD"))
		{
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Check the required fields in the tool schema")));
		}
		else if (ErrorCode == TEXT("IMPORT_FAILED"))
		{
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Verify Begin Object / End Object blocks are well-formed")));
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Use LINK_* for PinId and GUID_* for NodeGuid placeholders")));
		}
		else if (ErrorCode == TEXT("VALIDATION_ERROR"))
		{
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Review the tool's input_schema for required fields")));
		}
	}
	ErrorObj->SetArrayField(TEXT("suggestions"), SuggestionsArr);

	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetBoolField(TEXT("success"), false);
	Envelope->SetObjectField(TEXT("error"), ErrorObj);

	FString ResponseStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResponseStr);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);

	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(ResponseStr, TEXT("application/json"));
	Response->Code = static_cast<EHttpServerResponseCodes>(StatusCode);
	OnComplete(MoveTemp(Response));
}

TSharedPtr<FJsonObject> FKismetAIModule::ParseRequestBody(const FHttpServerRequest& Request)
{
	if (Request.Body.Num() == 0)
	{
		return nullptr;
	}

	FString BodyStr;
	FFileHelper::BufferToString(BodyStr, Request.Body.GetData(), Request.Body.Num());

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);
	if (!FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		return nullptr;
	}

	return JsonObject;
}

// Run a typed tool handler on the game thread and return its result.
//
// Same tab-focus restore as ExecuteHandlerDirect — Blueprint editor refreshes
// triggered by a handler foreground the Details tab and steal focus from the
// KismetAI panel — but without fabricating an HTTP request or serializing
// through a response body. No cancellation: a token nobody can cancel.
FToolExecutionResult FKismetAIModule::RunOnGameThread(TFunction<FToolExecutionResult()> Work,
	TFunction<TOptional<FToolExecutionResult>()> Admit)
{
	return DispatchToGameThread(MoveTemp(Work), MoveTemp(Admit), FKismetAICancellationToken(),
		FToolExecutionResult::Error(409, TEXT("CANCELLED"), TEXT("The call was cancelled before it ran. Nothing was changed."))).Result;
}

// The worker's wait is owned by a dispatcher job, not by references to
// this stack frame: Work, Admit, the token and the cancelled result are copied
// into the body, and the body captures the module only for the game-thread
// side, which the dispatcher runs only while the job is Running (never after
// Close). See KismetAIGameThreadDispatcher.h for the state machine.
FKismetAIDispatchOutcome FKismetAIModule::DispatchToGameThread(TFunction<FToolExecutionResult()> Work,
	TFunction<TOptional<FToolExecutionResult>()> Admit, const FKismetAICancellationToken& Token,
	const FToolExecutionResult& CancelledBeforeStart)
{
	return GameThreadDispatcher->Dispatch(
		MakeAdmittedWorkBody(*this, MoveTemp(Work), MoveTemp(Admit), Token, CancelledBeforeStart),
		Token, CancelledBeforeStart);
}

FKismetAIGameThreadDispatcher::FBody FKismetAIModule::MakeAdmittedWorkBody(FKismetAIModule& Module,
	TFunction<FToolExecutionResult()> Work, TFunction<TOptional<FToolExecutionResult>()> Admit,
	const FKismetAICancellationToken& Token, const FToolExecutionResult& CancelledBeforeStart)
{
	FKismetAIModule* ModulePtr = &Module;
	return [ModulePtr, Work = MoveTemp(Work), Admit = MoveTemp(Admit), Token, CancelledBeforeStart]()
	{
		return ModulePtr->RunAdmittedWork(Work, Admit, Token, CancelledBeforeStart);
	};
}

FKismetAIDispatchOutcome FKismetAIModule::RunAdmittedWork(const TFunction<FToolExecutionResult()>& Work,
	const TFunction<TOptional<FToolExecutionResult>()>& Admit, const FKismetAICancellationToken& Token,
	const FToolExecutionResult& CancelledBeforeStart)
{
	check(IsInGameThread());
	FKismetAIDispatchOutcome Out;

	// The game-thread admission seam (KismetAIToolExecution.h). Everything
	// below runs on the game thread with nothing between it and Work():
	//   0. callback entry — a call cancelled while it was queued stops here,
	//      before the editor guard could turn it into a "busy" failure;
	//   1. editor-state guard — no object work while saving, collecting garbage
	//      or inside a slow task;
	//   2. Admit — the executor's cancellation re-check and the policy's
	//      EvaluateAdmission (authority);
	//   3. Work. Not preemptible: from here a Stop is only recorded.
	if (Token.IsCancelled())
	{
		Out.Result = CancelledBeforeStart;
		Out.Result.bHandlerNotStarted = true;
		Out.bStoppedBeforeStart = true;
		return Out;
	}
	if (const TOptional<FToolExecutionResult> Error = GetEditorAccessError())
	{
		Out.Result = Error.GetValue();
		Out.Result.bHandlerNotStarted = true;
		Out.bEditorBusyBeforeAdmission = true;
		return Out;
	}
	if (Admit)
	{
		if (const TOptional<FToolExecutionResult> Refused = Admit())
		{
			Out.Result = Refused.GetValue();
			Out.Result.bHandlerNotStarted = true;
			return Out;
		}
	}

	TSharedPtr<SDockTab> KismetTab = BlueprintEditorKismetTab.Pin();
	const bool bWasTabForeground = KismetTab.IsValid() && KismetTab->IsForeground();

	Out.Result = Work();
	// Stop that arrived while the native operation ran (only possible if the
	// handler pumped the game thread, e.g. a slow task ticking Slate) or
	// between admission and here. The result above is still the real one.
	Out.bCancelRequestedWhileRunning = Token.IsCancelled();

	if (bWasTabForeground && KismetTab.IsValid())
	{
		KismetTab->ActivateInParent(ETabActivationCause::SetDirectly);
	}
	return Out;
}

void FKismetAIModule::SetGameThreadSchedulerForTests(FKismetAIGameThreadDispatcher::FScheduler InScheduler)
{
	GameThreadDispatcher->SetSchedulerForTests(MoveTemp(InScheduler));
}

FKismetAIGameThreadDispatcher::FStats FKismetAIModule::GetGameThreadDispatchStatsForTests() const
{
	return GameThreadDispatcher->GetStatsForTests();
}

FString FKismetAIModule::ExecuteHandlerDirect(
	bool (FKismetAIModule::*Handler)(const FHttpServerRequest&, const FHttpResultCallback&),
	TSharedPtr<FJsonObject> Body)
{
	// If called from a background thread, bounce to game thread and block until done.
	// Handlers access UObjects which must only be touched on the game thread.
	if (!IsInGameThread())
	{
		FString Result;
		FEvent* DoneEvent = FPlatformProcess::GetSynchEventFromPool();
		AsyncTask(ENamedThreads::GameThread, [this, Handler, Body, &Result, DoneEvent]()
		{
			Result = ExecuteHandlerDirect(Handler, Body);
			DoneEvent->Trigger();
		});
		DoneEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
		return Result;
	}

	if (const TOptional<FToolExecutionResult> Error = GetEditorAccessError())
	{
		return Error->ToModelString();
	}

	// Check if the KismetAI tab is currently foreground before executing the handler,
	// so we can restore it afterwards (Blueprint editor auto-foregrounds Details on changes)
	TSharedPtr<SDockTab> KismetTab = BlueprintEditorKismetTab.Pin();
	bool bWasTabForeground = KismetTab.IsValid() && KismetTab->IsForeground();

	// Serialize the JSON body to a string
	FString BodyStr;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyStr);
	FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);

	// Build a fake FHttpServerRequest with the body bytes
	FHttpServerRequest FakeRequest;
	FTCHARToUTF8 Converter(*BodyStr);
	FakeRequest.Body.Append((const uint8*)Converter.Get(), Converter.Length());

	// Capture the response string from the callback
	FString CapturedResponse;
	FHttpResultCallback CaptureCallback = [&CapturedResponse](TUniquePtr<FHttpServerResponse>&& Response)
	{
		if (Response.IsValid())
		{
			// Extract body from response
			// FHttpServerResponse stores body as TArray<uint8>
			const TArray<uint8>& ResponseBody = Response->Body;
			FUTF8ToTCHAR ResponseConverter((const ANSICHAR*)ResponseBody.GetData(), ResponseBody.Num());
			CapturedResponse = FString(ResponseConverter.Length(), ResponseConverter.Get());
		}
	};

	// Call the handler directly
	(this->*Handler)(FakeRequest, CaptureCallback);

	// Restore KismetAI tab focus if it was foreground before the handler ran.
	// Handler operations (inject T3D, compile, edit graph) trigger Blueprint editor
	// refreshes that foreground the Details tab, stealing focus from KismetAI.
	if (bWasTabForeground && KismetTab.IsValid())
	{
		KismetTab->ActivateInParent(ETabActivationCause::SetDirectly);
	}

	// When called from the agent (not HTTP), wrap errors in <tool_use_error> tags
	if (CapturedResponse.Contains(TEXT("\"success\":false")) || CapturedResponse.Contains(TEXT("\"success\": false")))
	{
		CapturedResponse = FString::Printf(TEXT("<tool_use_error>%s</tool_use_error>"), *CapturedResponse);
	}

	return CapturedResponse;
}

// REST tool-route adapter. The transport's part: a body that is not a
// JSON object is refused here exactly as the handlers used to (400
// INVALID_JSON) and never reaches the service. Everything else — normalization,
// registry lookup and validation, policy, cancellation, the game-thread
// admission seam and dispatch — is FKismetAIToolExecutor's, the same object the
// agent and MCP use. The typed result is rendered as the envelope REST callers
// already parse; it is not truncated (truncation is a model-context concern).
bool FKismetAIModule::ServeToolRoute(const TCHAR* ToolName, const TCHAR* Mode, const FKismetAICallerContext& Caller,
	const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedPtr<FJsonObject> Body = ParseRequestBody(Request);
	if (!Body.IsValid())
	{
		SendError(OnComplete, 400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
		return true;
	}
	FKismetAIToolExecutor::NormalizeArguments(Body);

	// A mode alias (/export_t3d is read_blueprint in t3d mode) fixes the mode
	// the service validates and dispatches on, whatever the body says. The
	// handler still reads the caller's body.
	TSharedPtr<FJsonObject> Arguments = Body;
	if (Mode)
	{
		Arguments = MakeShared<FJsonObject>(*Body);
		Arguments->SetStringField(TEXT("mode"), Mode);
	}

	FToolCallContext Ctx;
	Ctx.ToolName = ToolName;
	Ctx.Caller = Caller;
	Ctx.Arguments = Arguments;
	Ctx.RequestBody = Body;
	Ctx.bPlanModeActive = false;      // plan mode is agent-run state
	Ctx.bInteractiveAllowed = false;  // nobody can answer an Ask on a REST call
	Ctx.DefaultMaxResultChars = AgentConfig.MaxToolResultChars;
	// REST has no cancellation source yet; the token is still the one
	// the admission seam checks.

	if (Ctx.ToolName == TEXT("read_source") && Caller.EntryPoint == EKismetAIEntryPoint::Rest)
	{
		FString RequestId;
		if (Body->HasField(TEXT("request_id")) && (!Body->TryGetStringField(TEXT("request_id"), RequestId) || RequestId.Len() > 128))
		{
			SendError(OnComplete, 400, TEXT("VALIDATION_ERROR"), TEXT("request_id must be a string of at most 128 characters."));
			return true;
		}
		if (RequestId.IsEmpty()) RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Json;
		const auto Writer = TJsonWriterFactory<>::Create(&Json);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		const auto Invoker = MakeShared<FKismetAIModuleToolInvoker, ESPMode::ThreadSafe>(*this);
		const int32 MaxResultChars = Ctx.DefaultMaxResultChars;
		const auto Submitted = SourceJobs->Submit(TEXT("rest:") + RequestId,
			[Invoker, Json, Caller, MaxResultChars](const FKismetAICancellationToken& Token)
			{
				FKismetAISourceJobs::FResult Result;
				TSharedPtr<FJsonObject> OwnedBody;
				const auto Reader = TJsonReaderFactory<>::Create(Json);
				if (!FJsonSerializer::Deserialize(Reader, OwnedBody))
				{ Result.Tool = FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Invalid source request.")); return Result; }
				FToolCallContext Context;
				Context.ToolName = TEXT("read_source"); Context.Caller = Caller;
				Context.Arguments = OwnedBody; Context.RequestBody = OwnedBody;
				Context.CancelToken = Token; Context.bInteractiveAllowed = false;
				Context.DefaultMaxResultChars = MaxResultChars;
				FKismetAIToolExecutor Executor(*Invoker);
				Result.Tool = Executor.Execute(Context).Typed;
				return Result;
			}, [OnComplete](FKismetAISourceJobs::FResult&& Result)
			{
				if (Result.bCancelled) Result.Tool = FToolExecutionResult::Error(409, TEXT("CANCELLED"), TEXT("Source request cancelled."));
				auto Response = FHttpServerResponse::Create(Result.Tool.ToEnvelopeString(), TEXT("application/json"));
				Response->Code = static_cast<EHttpServerResponseCodes>(Result.Tool.StatusCode);
				OnComplete(MoveTemp(Response));
			});
		if (Submitted != FKismetAISourceJobs::ESubmit::Started)
			SendError(OnComplete, Submitted == FKismetAISourceJobs::ESubmit::Duplicate ? 409 : 429,
				TEXT("SOURCE_BUSY"), TEXT("Source job capacity is exhausted, the request id is active, or the server is closing."));
		return true;
	}

	FKismetAIModuleToolInvoker Invoker(*this);
	FKismetAIToolExecutor Executor(Invoker);
	const FToolCallOutcome Outcome = Executor.Execute(Ctx);

	if (Outcome.Typed.bSuccess)
	{
		SendSuccess(OnComplete, Outcome.Typed.Data);
	}
	else
	{
		SendError(OnComplete, Outcome.Typed.StatusCode, Outcome.Typed.ErrorCode, Outcome.Typed.Message, Outcome.Typed.Suggestions);
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint helpers (moved to KismetAIUtils.h / KismetAIUtils.cpp)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// T3D processing
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAIModule::StripT3D(const FString& RawT3D)
{
	// --- Aggressive T3D stripping for LLM consumption ---
	// Goal: reduce ~2000 chars/node to ~150 chars/node while preserving
	// everything needed to understand and reproduce the graph.

	// Patterns to strip from CustomProperties Pin lines (order matters: prefixed first)
	static const TArray<FString> PinStripPatterns = {
		// PinType sub-fields (prefixed — must be first)
		TEXT("PinType.bIsReference=False"),
		TEXT("PinType.bIsConst=False"),
		TEXT("PinType.bIsWeakPointer=False"),
		TEXT("PinType.bIsUObjectWrapper=False"),
		TEXT("PinType.bSerializeAsSinglePrecisionFloat=False"),
		TEXT("PinType.ContainerType=None"),
		TEXT("PinType.PinSubCategory=\"\""),
		TEXT("PinType.PinValueType=()"),
		TEXT("PinType.PinSubCategoryObject=None"),
		TEXT("PinType.PinSubCategoryMemberReference=()"),
		// Top-level pin flags
		TEXT("bNotConnectable=False"),
		TEXT("bDefaultValueIsReadOnly=False"),
		TEXT("bDefaultValueIsIgnored=False"),
		TEXT("bAdvancedView=False"),
		TEXT("bOrphanedPin=False"),
		TEXT("bOverridePinName=False"),
		TEXT("bHidden=False"),
		// Zero GUIDs
		TEXT("PersistentGuid=00000000000000000000000000000000"),
		// Empty defaults
		TEXT("DefaultObject=None"),
	};

	// UE-generated node comments (not user comments)
	static const FString UEGeneratedComment = TEXT("This node is disabled and will not be called.");

	FString Result;
	Result.Reserve(RawT3D.Len() / 3); // Expect ~3x reduction

	TArray<FString> Lines;
	RawT3D.ParseIntoArrayLines(Lines);

	for (int32 LineIdx = 0; LineIdx < Lines.Num(); LineIdx++)
	{
		FString TrimmedLine = Lines[LineIdx];
		TrimmedLine.TrimStartAndEndInline();

		if (TrimmedLine.IsEmpty())
		{
			continue;
		}

		// --- Skip entire node-level lines that are pure noise ---

		// Skip ExportPath (long asset path, not needed)
		if (TrimmedLine.Contains(TEXT("ExportPath=")))
		{
			// ExportPath is on the Begin Object line — strip it but keep the rest
			int32 ExportIdx = TrimmedLine.Find(TEXT("ExportPath="));
			if (ExportIdx != INDEX_NONE)
			{
				TrimmedLine = TrimmedLine.Left(ExportIdx).TrimEnd();
			}
		}

		// Skip editor UI state
		if (TrimmedLine.StartsWith(TEXT("bCommentBubblePinned=")) ||
			TrimmedLine.StartsWith(TEXT("bCommentBubbleVisible=")))
		{
			continue;
		}

		// Handle NodeComment: keep user comments, strip UE-generated ones
		if (TrimmedLine.StartsWith(TEXT("NodeComment=")))
		{
			if (TrimmedLine.Contains(UEGeneratedComment))
			{
				continue; // Skip UE-generated "disabled node" comment
			}
			// Keep user comments — fall through to output
		}

		// --- Process CustomProperties Pin lines ---

		if (TrimmedLine.StartsWith(TEXT("CustomProperties Pin")))
		{
			// Skip hidden pins entirely (self, WorldContextObject, etc.)
			if (TrimmedLine.Contains(TEXT("bHidden=True")))
			{
				continue;
			}

			// Helper: extract a field value from the pin line by key prefix.
			// Handles both quoted ("value") and unquoted (value,) formats.
			auto ExtractField = [](const FString& Line, const FString& FieldKey) -> FString
			{
				// Search for the field key preceded by comma or opening paren (to avoid substring matches)
				// e.g. ",DefaultValue=" or "(DefaultValue=" but NOT "AutogeneratedDefaultValue="
				int32 Idx = INDEX_NONE;
				for (const TCHAR* Prefix : { TEXT(","), TEXT("(") })
				{
					FString Search = FString(Prefix) + FieldKey;
					int32 Found = Line.Find(Search);
					if (Found != INDEX_NONE)
					{
						Idx = Found + FCString::Strlen(Prefix);
						break;
					}
				}
				if (Idx == INDEX_NONE) return FString();

				int32 ValStart = Idx + FieldKey.Len();
				if (ValStart >= Line.Len()) return FString();

				if (Line[ValStart] == TEXT('"'))
				{
					// Quoted value — find closing quote (skip escaped quotes)
					int32 SearchFrom = ValStart + 1;
					while (SearchFrom < Line.Len())
					{
						int32 Q = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
						if (Q == INDEX_NONE) break;
						if (Q > 0 && Line[Q - 1] == TEXT('\\'))
						{
							SearchFrom = Q + 1;
							continue;
						}
						return Line.Mid(ValStart + 1, Q - ValStart - 1);
					}
				}
				else
				{
					// Unquoted value — read until comma or closing paren
					int32 End = ValStart;
					while (End < Line.Len() && Line[End] != TEXT(',') && Line[End] != TEXT(')'))
					{
						End++;
					}
					return Line.Mid(ValStart, End - ValStart);
				}
				return FString();
			};

			// Helper: remove an entire field (key + value) from the pin line
			auto RemoveField = [](FString& Line, const FString& FieldKey)
			{
				for (const TCHAR* Prefix : { TEXT(","), TEXT("(") })
				{
					FString Search = FString(Prefix) + FieldKey;
					int32 Idx = Line.Find(Search);
					if (Idx == INDEX_NONE) continue;

					int32 FieldStart = Idx + FCString::Strlen(Prefix); // keep the prefix char
					int32 ValStart = FieldStart + FieldKey.Len();
					int32 FieldEnd = ValStart;

					if (ValStart < Line.Len() && Line[ValStart] == TEXT('"'))
					{
						// Quoted — find closing quote
						int32 SearchFrom = ValStart + 1;
						while (SearchFrom < Line.Len())
						{
							int32 Q = Line.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
							if (Q == INDEX_NONE) { FieldEnd = Line.Len(); break; }
							if (Q > 0 && Line[Q - 1] == TEXT('\\')) { SearchFrom = Q + 1; continue; }
							FieldEnd = Q + 1;
							break;
						}
					}
					else
					{
						// Unquoted — read until comma or closing paren
						while (FieldEnd < Line.Len() && Line[FieldEnd] != TEXT(',') && Line[FieldEnd] != TEXT(')'))
						{
							FieldEnd++;
						}
					}

					// Remove the field including preceding comma
					FString ToRemove = Line.Mid(Idx, FieldEnd - Idx);
					// But if preceded by '(' don't remove the paren, just remove from after it
					if (*Prefix == TEXT('('))
					{
						ToRemove = Line.Mid(FieldStart, FieldEnd - FieldStart);
						// Also remove trailing comma if present
						if (FieldEnd < Line.Len() && Line[FieldEnd] == TEXT(','))
						{
							ToRemove = Line.Mid(FieldStart, FieldEnd - FieldStart + 1);
						}
					}
					Line = Line.Replace(*ToRemove, TEXT(""));
					return;
				}
			};

			// Skip advanced-view pins with unchanged default values
			if (TrimmedLine.Contains(TEXT("bAdvancedView=True")))
			{
				FString DefaultValue = ExtractField(TrimmedLine, TEXT("DefaultValue="));
				FString AutoDefault = ExtractField(TrimmedLine, TEXT("AutogeneratedDefaultValue="));
				bool bHasLinkedTo = TrimmedLine.Contains(TEXT("LinkedTo="));

				if (!bHasLinkedTo &&
					(DefaultValue == AutoDefault || (DefaultValue.IsEmpty() && AutoDefault.IsEmpty())))
				{
					continue;
				}
			}

			FString CleanedLine = TrimmedLine;

			// Remove fields that are pure noise
			RemoveField(CleanedLine, TEXT("PinToolTip="));
			RemoveField(CleanedLine, TEXT("PinFriendlyName="));

			// Remove AutogeneratedDefaultValue when it equals DefaultValue
			{
				FString DV = ExtractField(CleanedLine, TEXT("DefaultValue="));
				FString AD = ExtractField(CleanedLine, TEXT("AutogeneratedDefaultValue="));
				if (!DV.IsEmpty() && DV == AD)
				{
					RemoveField(CleanedLine, TEXT("AutogeneratedDefaultValue="));
				}
			}

			// Apply pattern stripping (bIsReference=False, PinType defaults, etc.)
			for (const FString& Pattern : PinStripPatterns)
			{
				CleanedLine = CleanedLine.Replace(*(TEXT(",") + Pattern), TEXT(""));
				CleanedLine = CleanedLine.Replace(*(Pattern + TEXT(",")), TEXT(""));
				CleanedLine = CleanedLine.Replace(*Pattern, TEXT(""));
			}

			// Clean up artifacts
			while (CleanedLine.Contains(TEXT(",,"))) CleanedLine = CleanedLine.Replace(TEXT(",,"), TEXT(","));
			while (CleanedLine.Contains(TEXT("  "))) CleanedLine = CleanedLine.Replace(TEXT("  "), TEXT(" "));
			CleanedLine = CleanedLine.Replace(TEXT(",)"), TEXT(")"));
			CleanedLine = CleanedLine.Replace(TEXT("( "), TEXT("("));

			Result += CleanedLine + TEXT("\n");
		}
		else
		{
			// Non-pin lines: strip AdvancedPinDisplay, keep everything else
			if (TrimmedLine.StartsWith(TEXT("AdvancedPinDisplay=")))
			{
				continue;
			}

			Result += TEXT("   ") + TrimmedLine + TEXT("\n");
		}
	}

	return Result;
}

FString FKismetAIModule::ResolvePlaceholders(const FString& T3DWithPlaceholders)
{
	// Match LINK_*, GUID_*, NODEREF_*, ID_* tokens
	// Map each unique token to a real GUID
	TMap<FString, FString> PlaceholderMap;

	// Regex: (LINK_|GUID_|NODEREF_|ID_)[A-Za-z0-9_]+
	const FRegexPattern Pattern(TEXT("(LINK_|GUID_|NODEREF_|ID_)[A-Za-z0-9_]+"));

	// First pass: assign a unique real GUID to each distinct placeholder token.
	{
		FRegexMatcher Matcher(Pattern, T3DWithPlaceholders);
		while (Matcher.FindNext())
		{
			FString Token = Matcher.GetCaptureGroup(0);
			if (!PlaceholderMap.Contains(Token))
			{
				// T3D format: 32 uppercase hex chars, no hyphens
				PlaceholderMap.Add(Token, FGuid::NewGuid().ToString(EGuidFormats::Digits));
			}
		}
	}

	if (PlaceholderMap.Num() == 0)
	{
		return T3DWithPlaceholders;
	}

	// Second pass: rebuild the string by substituting exactly the matched ranges.
	// A naive String::Replace per token corrupts prefix-colliding tokens — e.g.
	// replacing LINK_A first also rewrites the "LINK_A" inside LINK_AB, producing
	// an invalid PinId and the same SIGSEGV the validator guards against. Replacing
	// only the regex-matched spans (longest valid token at each position) is safe
	// regardless of token ordering or shared prefixes.
	FString Result;
	Result.Reserve(T3DWithPlaceholders.Len() + 16);
	FRegexMatcher Matcher(Pattern, T3DWithPlaceholders);
	int32 LastEnd = 0;
	while (Matcher.FindNext())
	{
		const int32 MatchBegin = Matcher.GetMatchBeginning();
		const int32 MatchEnd = Matcher.GetMatchEnding();
		Result.Append(T3DWithPlaceholders.Mid(LastEnd, MatchBegin - LastEnd));
		Result.Append(PlaceholderMap[Matcher.GetCaptureGroup(0)]);
		LastEnd = MatchEnd;
	}
	Result.Append(T3DWithPlaceholders.Mid(LastEnd));

	return Result;
}

bool FKismetAIModule::ValidateT3DForImport(const FString& ResolvedT3D, UEdGraph* Graph, FString& OutError)
{
	// 0. No residual placeholders may survive ResolvePlaceholders — a leftover
	//    LINK_/GUID_/NODEREF_/ID_ token would land as an invalid PinId/NodeGuid and
	//    crash the importer.
	{
		const FRegexPattern Pattern(TEXT("(LINK_|GUID_|NODEREF_|ID_)[A-Za-z0-9_]+"));
		FRegexMatcher Matcher(Pattern, ResolvedT3D);
		if (Matcher.FindNext())
		{
			OutError = FString::Printf(
				TEXT("T3D contains an unresolved placeholder '%s'. Every LINK_/GUID_/NODEREF_/ID_ token must be defined in this write call; a forward reference to an existing node must use edit_blueprint instead."),
				*Matcher.GetCaptureGroup(0));
			return false;
		}
	}

	// Helper: extract a field value starting at "Key" up to the next ',' or ')'
	// (used for unquoted values like PinId).
	auto ExtractUnquoted = [](const FString& Line, const TCHAR* Key) -> FString
	{
		int32 Start = Line.Find(Key);
		if (Start == INDEX_NONE) return FString();
		Start += FCString::Strlen(Key);
		int32 End = Start;
		while (End < Line.Len())
		{
			const TCHAR C = Line[End];
			if (C == TEXT(',') || C == TEXT(')')) break;
			++End;
		}
		return Line.Mid(Start, End - Start).TrimStartAndEnd();
	};

	// Collect every PinId defined in this buffer + validate each pin record.
	TSet<FString> DefinedPinIds;
	TArray<FString> LinkedToTargets;

	TArray<FString> Lines;
	ResolvedT3D.ParseIntoArrayLines(Lines);

	for (const FString& RawLine : Lines)
	{
		FString Line = RawLine;
		Line.TrimStartAndEndInline();
		if (!Line.StartsWith(TEXT("CustomProperties Pin")))
		{
			continue;
		}

		// a) Balanced parentheses (quote-aware so a paren inside a string default
		//    value does not trip the counter).
		{
			int32 Depth = 0;
			bool bAnyOpen = false;
			bool bInQuote = false;
			TCHAR Prev = TEXT('\0');
			for (const TCHAR C : Line)
			{
				if (C == TEXT('"') && Prev != TEXT('\\'))
				{
					bInQuote = !bInQuote;
				}
				else if (!bInQuote)
				{
					if (C == TEXT('(')) { ++Depth; bAnyOpen = true; }
					else if (C == TEXT(')')) { --Depth; }
				}
				Prev = C;
			}
			if (!bAnyOpen || Depth != 0)
			{
				OutError = FString::Printf(TEXT("Unbalanced parentheses in pin record: %s"), *Line.Left(160));
				return false;
			}
		}

		// b) Valid GUID PinId.
		const FString PinIdStr = ExtractUnquoted(Line, TEXT("PinId="));
		FGuid PinGuid;
		if (PinIdStr.IsEmpty() || !FGuid::Parse(PinIdStr, PinGuid) || !PinGuid.IsValid())
		{
			OutError = FString::Printf(TEXT("Pin record has a missing or invalid PinId ('%s'): %s"), *PinIdStr, *Line.Left(160));
			return false;
		}
		DefinedPinIds.Add(PinGuid.ToString(EGuidFormats::Digits));

		// c) Non-empty PinName.
		{
			const TCHAR* NameKey = TEXT("PinName=\"");
			int32 NameStart = Line.Find(NameKey);
			FString PinName;
			if (NameStart != INDEX_NONE)
			{
				NameStart += FCString::Strlen(NameKey);
				int32 NameEnd = Line.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, NameStart);
				if (NameEnd != INDEX_NONE)
				{
					PinName = Line.Mid(NameStart, NameEnd - NameStart);
				}
			}
			else
			{
				// Tolerate an unquoted PinName=Foo (legal in hand-authored T3D) so a
				// valid record isn't rejected as "missing PinName".
				PinName = ExtractUnquoted(Line, TEXT("PinName="));
			}
			if (PinName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Pin record is missing a non-empty PinName=\"...\": %s"), *Line.Left(160));
				return false;
			}
		}

		// d) Collect LinkedTo targets: LinkedTo=(NodeName PinIdHex,NodeName2 PinIdHex2,)
		{
			const TCHAR* LTKey = TEXT("LinkedTo=(");
			int32 LTStart = Line.Find(LTKey);
			if (LTStart != INDEX_NONE)
			{
				LTStart += FCString::Strlen(LTKey);
				int32 LTEnd = Line.Find(TEXT(")"), ESearchCase::IgnoreCase, ESearchDir::FromStart, LTStart);
				if (LTEnd != INDEX_NONE)
				{
					TArray<FString> Pairs;
					Line.Mid(LTStart, LTEnd - LTStart).ParseIntoArray(Pairs, TEXT(","), true);
					for (const FString& Pair : Pairs)
					{
						const FString TrimmedPair = Pair.TrimStartAndEnd();
						int32 SpaceIdx;
						if (TrimmedPair.FindChar(TEXT(' '), SpaceIdx))
						{
							const FString TargetPinId = TrimmedPair.Mid(SpaceIdx + 1).TrimStartAndEnd();
							FGuid TargetGuid;
							if (FGuid::Parse(TargetPinId, TargetGuid) && TargetGuid.IsValid())
							{
								LinkedToTargets.Add(TargetGuid.ToString(EGuidFormats::Digits));
							}
						}
					}
				}
			}
		}
	}

	// e) Every LinkedTo target must resolve to a pin defined in this buffer or one
	//    already present in the live graph; a dangling target is exactly what makes
	//    ResolveAllPinReferences write through a freed pin pointer.
	if (LinkedToTargets.Num() > 0)
	{
		TSet<FString> GraphPinIds;
		if (Graph)
		{
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (!Node) continue;
				for (UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin && Pin->PinId.IsValid())
					{
						GraphPinIds.Add(Pin->PinId.ToString(EGuidFormats::Digits));
					}
				}
			}
		}
		for (const FString& Target : LinkedToTargets)
		{
			if (!DefinedPinIds.Contains(Target) && !GraphPinIds.Contains(Target))
			{
				OutError = FString::Printf(
					TEXT("LinkedTo references PinId '%s' which is not defined in this write call and does not exist in the graph. Only link between NEW nodes here; use edit_blueprint to wire to existing nodes."),
					*Target);
				return false;
			}
		}
	}

	return true;
}

int32 FKismetAIModule::CompleteMacroReferences(FString& InOutT3D)
{
	if (!InOutT3D.Contains(TEXT("K2Node_MacroInstance")) || !InOutT3D.Contains(TEXT("MacroGraphReference=(")))
	{
		return 0;
	}

	// Index every macro graph in the standard libraries by (lowercased) name.
	struct FMacroEntry { UBlueprint* Blueprint; UEdGraph* Graph; };
	static const TCHAR* MacroLibPaths[] = {
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
		TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros"),
		TEXT("/Engine/EditorBlueprintResources/ActorComponentMacros.ActorComponentMacros"),
	};
	TMap<FString, FMacroEntry> MacroByName;
	for (const TCHAR* LibPath : MacroLibPaths)
	{
		UBlueprint* Lib = LoadObject<UBlueprint>(nullptr, LibPath);
		if (!Lib) continue;
		for (UEdGraph* MacroGraph : Lib->MacroGraphs)
		{
			if (MacroGraph)
			{
				MacroByName.Add(MacroGraph->GetName().ToLower(), FMacroEntry{ Lib, MacroGraph });
			}
		}
	}
	if (MacroByName.Num() == 0)
	{
		return 0;
	}

	// Quote-aware matching-paren finder.
	auto FindMatchingParen = [](const FString& S, int32 OpenIdx) -> int32
	{
		int32 Depth = 0;
		bool bInQuote = false;
		TCHAR Prev = TEXT('\0');
		for (int32 i = OpenIdx; i < S.Len(); ++i)
		{
			const TCHAR C = S[i];
			if (C == TEXT('"') && Prev != TEXT('\\')) { bInQuote = !bInQuote; }
			else if (!bInQuote)
			{
				if (C == TEXT('(')) { ++Depth; }
				else if (C == TEXT(')')) { if (--Depth == 0) return i; }
			}
			Prev = C;
		}
		return INDEX_NONE;
	};

	// Pull the macro graph's leaf name out of a MacroGraphReference body.
	auto ExtractMacroName = [](const FString& Inner) -> FString
	{
		int32 MGStart = Inner.Find(TEXT("MacroGraph="));
		if (MGStart == INDEX_NONE) return FString();
		MGStart += FCString::Strlen(TEXT("MacroGraph="));
		FString Val;
		if (Inner.IsValidIndex(MGStart) && Inner[MGStart] == TEXT('"'))
		{
			const int32 Q1 = MGStart + 1;
			const int32 Q2 = Inner.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, Q1);
			if (Q2 == INDEX_NONE) return FString();
			Val = Inner.Mid(Q1, Q2 - Q1);
		}
		else
		{
			const int32 Comma = Inner.Find(TEXT(","), ESearchCase::IgnoreCase, ESearchDir::FromStart, MGStart);
			Val = (Comma == INDEX_NONE) ? Inner.Mid(MGStart) : Inner.Mid(MGStart, Comma - MGStart);
		}
		Val = Val.TrimStartAndEnd();
		Val.RemoveFromEnd(TEXT("'"));
		int32 Idx;
		if (Val.FindLastChar(TEXT('\''), Idx)) { Val = Val.Mid(Idx + 1); }
		if (Val.FindLastChar(TEXT(':'), Idx)) { Val = Val.Mid(Idx + 1); }
		else if (Val.FindLastChar(TEXT('.'), Idx)) { Val = Val.Mid(Idx + 1); }
		return Val.TrimStartAndEnd();
	};

	// Collect replacements first, apply last-to-first so indices stay valid.
	struct FReplacement { int32 Start; int32 Len; FString Text; };
	TArray<FReplacement> Replacements;

	const FString Marker = TEXT("MacroGraphReference=(");
	int32 SearchStart = 0;
	while (true)
	{
		const int32 RefStart = InOutT3D.Find(Marker, ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchStart);
		if (RefStart == INDEX_NONE) break;
		const int32 OpenParen = RefStart + Marker.Len() - 1;
		const int32 Close = FindMatchingParen(InOutT3D, OpenParen);
		if (Close == INDEX_NONE) break;
		SearchStart = Close + 1;

		const FString Inner = InOutT3D.Mid(OpenParen + 1, Close - OpenParen - 1);

		// Already complete? A valid GraphGuid + a non-null GraphBlueprint means the
		// agent copied a real reference — leave it untouched.
		bool bHasGuid = false;
		{
			const int32 Gi = Inner.Find(TEXT("GraphGuid="));
			if (Gi != INDEX_NONE)
			{
				int32 ValStart = Gi + FCString::Strlen(TEXT("GraphGuid="));
				int32 ValEnd = ValStart;
				while (ValEnd < Inner.Len() && Inner[ValEnd] != TEXT(',') && Inner[ValEnd] != TEXT(')')) { ++ValEnd; }
				FGuid Tmp;
				bHasGuid = FGuid::Parse(Inner.Mid(ValStart, ValEnd - ValStart).TrimStartAndEnd(), Tmp) && Tmp.IsValid();
			}
		}
		const bool bHasBlueprint =
			Inner.Contains(TEXT("GraphBlueprint=")) &&
			!Inner.Contains(TEXT("GraphBlueprint=None")) &&
			!Inner.Contains(TEXT("GraphBlueprint=\"\""));
		if (bHasGuid && bHasBlueprint)
		{
			continue;
		}

		const FString MacroName = ExtractMacroName(Inner);
		if (MacroName.IsEmpty()) continue;
		const FMacroEntry* Entry = MacroByName.Find(MacroName.ToLower());
		if (!Entry || !Entry->Graph || !Entry->Blueprint) continue;

		const FString Canonical = FString::Printf(
			TEXT("MacroGraphReference=(MacroGraph=\"%s'%s'\",GraphBlueprint=\"%s'%s'\",GraphGuid=%s)"),
			*Entry->Graph->GetClass()->GetPathName(), *Entry->Graph->GetPathName(),
			*Entry->Blueprint->GetClass()->GetPathName(), *Entry->Blueprint->GetPathName(),
			*Entry->Graph->GraphGuid.ToString(EGuidFormats::Digits));

		Replacements.Add(FReplacement{ RefStart, (Close - RefStart) + 1, Canonical });
	}

	for (int32 i = Replacements.Num() - 1; i >= 0; --i)
	{
		const FReplacement& R = Replacements[i];
		InOutT3D = InOutT3D.Left(R.Start) + R.Text + InOutT3D.Mid(R.Start + R.Len);
	}

	if (Replacements.Num() > 0)
	{
		KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("CompleteMacroReferences: completed %d macro reference(s)"), Replacements.Num());
	}
	return Replacements.Num();
}

// Detect orphaned nodes in a graph — nodes with no connections to any other node.
// Returns warnings as strings for inclusion in tool results.
static TArray<FString> DetectOrphanedNodes(UEdGraph* Graph)
{
	TArray<FString> Warnings;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		// Skip comment nodes
		if (Node->GetClass()->GetName().Contains(TEXT("Comment"))) continue;

		bool bHasAnyConnection = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && !Pin->bHidden && Pin->LinkedTo.Num() > 0)
			{
				bHasAnyConnection = true;
				break;
			}
		}

		if (!bHasAnyConnection)
		{
			FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			Warnings.Add(FString::Printf(TEXT("ORPHANED NODE: '%s' (%s) has no connections — consider deleting it with edit_blueprint delete_node"),
				*Node->GetName(), *Title));
		}
	}
	return Warnings;
}

// Detect impure (exec-flow) nodes that can never execute: wired among
// themselves but with no exec path from any entry point (event, function
// entry, tunnel input). The Blueprint compiler does not warn about this — a
// Sequence pin left unwired silently strands its whole downstream subgraph.
static TArray<FString> DetectUnreachableExecNodes(UEdGraph* Graph)
{
	TArray<FString> Warnings;

	auto IsExecPin = [](const UEdGraphPin* Pin)
	{
		return Pin && !Pin->bOrphanedPin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec;
	};

	// Classify nodes participating in exec flow. A node with exec outputs but
	// no exec inputs is an entry point (events, function entries, tunnel inputs).
	TSet<UEdGraphNode*> ExecNodes;
	TArray<UEdGraphNode*> Roots;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node || Node->GetDesiredEnabledState() == ENodeEnabledState::Disabled) continue;
		bool bHasExecIn = false;
		bool bHasExecOut = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsExecPin(Pin)) continue;
			if (Pin->Direction == EGPD_Input) bHasExecIn = true;
			else bHasExecOut = true;
		}
		if (!bHasExecIn && !bHasExecOut) continue; // pure node or comment
		ExecNodes.Add(Node);
		if (!bHasExecIn)
		{
			Roots.Add(Node);
		}
	}

	// BFS along exec links from the entry points.
	TSet<UEdGraphNode*> Reachable(Roots);
	TArray<UEdGraphNode*> Queue = Roots;
	while (Queue.Num() > 0)
	{
		UEdGraphNode* Node = Queue.Pop();
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!IsExecPin(Pin) || Pin->Direction != EGPD_Output) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				UEdGraphNode* Target = Linked ? Linked->GetOwningNode() : nullptr;
				if (Target && ExecNodes.Contains(Target) && !Reachable.Contains(Target))
				{
					Reachable.Add(Target);
					Queue.Add(Target);
				}
			}
		}
	}

	TSet<UEdGraphNode*> Unreachable;
	for (UEdGraphNode* Node : ExecNodes)
	{
		if (Reachable.Contains(Node)) continue;
		// Fully disconnected nodes are DetectOrphanedNodes' territory.
		bool bHasAnyLink = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->LinkedTo.Num() > 0)
			{
				bHasAnyLink = true;
				break;
			}
		}
		if (bHasAnyLink)
		{
			Unreachable.Add(Node);
		}
	}
	if (Unreachable.Num() == 0)
	{
		return Warnings;
	}

	// Report chain heads (no incoming exec link at all) with their downstream
	// size; a headless cycle falls back to reporting every node in it.
	TArray<UEdGraphNode*> Heads;
	for (UEdGraphNode* Node : Unreachable)
	{
		bool bHasIncoming = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (IsExecPin(Pin) && Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
			{
				bHasIncoming = true;
				break;
			}
		}
		if (!bHasIncoming)
		{
			Heads.Add(Node);
		}
	}
	if (Heads.Num() == 0)
	{
		Heads = Unreachable.Array();
	}

	for (UEdGraphNode* Head : Heads)
	{
		TSet<UEdGraphNode*> Visited;
		Visited.Add(Head);
		TArray<UEdGraphNode*> Walk;
		Walk.Add(Head);
		while (Walk.Num() > 0)
		{
			UEdGraphNode* Node = Walk.Pop();
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!IsExecPin(Pin) || Pin->Direction != EGPD_Output) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					UEdGraphNode* Target = Linked ? Linked->GetOwningNode() : nullptr;
					if (Target && Unreachable.Contains(Target) && !Visited.Contains(Target))
					{
						Visited.Add(Target);
						Walk.Add(Target);
					}
				}
			}
		}

		FString EntryPinName;
		for (UEdGraphPin* Pin : Head->Pins)
		{
			if (IsExecPin(Pin) && Pin->Direction == EGPD_Input)
			{
				EntryPinName = Pin->PinName.ToString();
				break;
			}
		}

		const FString Title = Head->GetNodeTitle(ENodeTitleType::ListView).ToString();
		const int32 Downstream = Visited.Num() - 1;
		const FString DownstreamPart = Downstream > 0
			? FString::Printf(TEXT(" and %d downstream node(s)"), Downstream)
			: FString();
		Warnings.Add(FString::Printf(
			TEXT("UNREACHABLE EXEC PATH in '%s': nothing connects to exec input '%s' of '%s' (%s) — it%s can never execute. Wire an exec output into it (edit_blueprint connect) or delete the dead path."),
			*Graph->GetName(), *EntryPinName, *Head->GetName(), *Title, *DownstreamPart));
	}

	return Warnings;
}

void FKismetAIModule::SanitizeImportedNodes(const TSet<UEdGraphNode*>& ImportedNodes)
{
	// Build a map of all PinIds in the entire graph (not just imported nodes).
	// Skip pins with an invalid (zero) PinId: a malformed import can leave several
	// pins sharing FGuid() and they would collapse to one map entry, making the
	// orphan-link check below unreliable.
	TMap<FGuid, UEdGraphPin*> AllPinsInGraph;
	const UEdGraphSchema* Schema = nullptr;
	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (!Node || !Node->GetGraph()) continue;
		Schema = Node->GetGraph()->GetSchema();
		for (UEdGraphNode* GraphNode : Node->GetGraph()->Nodes)
		{
			if (!GraphNode) continue;
			for (UEdGraphPin* Pin : GraphNode->Pins)
			{
				if (Pin && Pin->PinId.IsValid())
				{
					AllPinsInGraph.Add(Pin->PinId, Pin);
				}
			}
		}
		break; // Same graph for all imported nodes
	}

	// Pass 1: Remove orphaned LinkedTo references (pointing to non-existent pins)
	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (!Node) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; --i)
			{
				UEdGraphPin* LinkedPin = Pin->LinkedTo[i];
				if (!LinkedPin || !AllPinsInGraph.Contains(LinkedPin->PinId))
				{
					Pin->LinkedTo.RemoveAt(i);
				}
			}
		}
	}

	// Pass 2: Fix bidirectional connections to existing (non-imported) nodes.
	// If an imported pin has LinkedTo pointing to an existing pin, complete the
	// link — but ONLY if the schema actually permits the connection. Forcing a
	// reverse link without CanCreateConnection silently produces a type-incompatible
	// or rule-violating edge that corrupts the compiled bytecode. When the schema
	// disallows it, drop the bogus forward link instead of cementing it.
	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (!Node) continue;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; --i)
			{
				UEdGraphPin* LinkedPin = Pin->LinkedTo[i];
				if (!LinkedPin)
				{
					Pin->LinkedTo.RemoveAt(i);
					continue;
				}

				// Only act on links to existing (non-imported) nodes.
				UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
				if (!LinkedNode || ImportedNodes.Contains(LinkedNode))
				{
					continue;
				}

				// Already bidirectional — nothing to do.
				if (LinkedPin->LinkedTo.Contains(Pin))
				{
					continue;
				}

				// Validate before completing the link. CanCreateConnection wants
				// (output, input) order; try both since LinkedTo does not record direction.
				bool bAllowed = false;
				if (Schema)
				{
					const bool bForward =
						Schema->CanCreateConnection(Pin, LinkedPin).Response != CONNECT_RESPONSE_DISALLOW;
					const bool bReverse =
						Schema->CanCreateConnection(LinkedPin, Pin).Response != CONNECT_RESPONSE_DISALLOW;
					bAllowed = bForward || bReverse;
				}

				if (bAllowed)
				{
					LinkedPin->Modify();
					LinkedPin->LinkedTo.Add(Pin);
				}
				else
				{
					// Disallowed: remove the dangling one-way link so we don't ship
					// a half-wired, schema-invalid connection.
					Pin->Modify();
					Pin->LinkedTo.RemoveAt(i);
				}
			}
		}
	}
}

void FKismetAIModule::WireExistingNodeConnections(UEdGraph* Graph, const TSet<UEdGraphNode*>& ImportedNodes, const FString& ResolvedT3D)
{
	if (!Graph) return;

	// Build lookup maps
	TMap<FString, UEdGraphNode*> NodesByName;
	TMap<FString, UEdGraphPin*> PinsByGuid;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		NodesByName.Add(Node->GetName(), Node);
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin)
			{
				PinsByGuid.Add(Pin->PinId.ToString(EGuidFormats::Digits), Pin);
			}
		}
	}

	// Parse the resolved T3D for LinkedTo references.
	// Format within a CustomProperties Pin line: LinkedTo=(NodeName PinIdHex,NodeName2 PinIdHex2,)
	// We need to find: which imported node's pin wants to connect to which existing node's pin.

	// Track which node name we're currently inside
	FString CurrentNodeName;

	TArray<FString> Lines;
	ResolvedT3D.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line;
		Trimmed.TrimStartAndEndInline();

		// Track current node: Begin Object ... Name="K2Node_Foo"
		if (Trimmed.StartsWith(TEXT("Begin Object")))
		{
			// Extract Name="..."
			FString NameKey = TEXT("Name=\"");
			int32 NameStart = Trimmed.Find(NameKey);
			if (NameStart != INDEX_NONE)
			{
				NameStart += NameKey.Len();
				int32 NameEnd = Trimmed.Find(TEXT("\""), ESearchCase::IgnoreCase, ESearchDir::FromStart, NameStart);
				if (NameEnd != INDEX_NONE)
				{
					CurrentNodeName = Trimmed.Mid(NameStart, NameEnd - NameStart);
				}
			}
			continue;
		}

		if (Trimmed.StartsWith(TEXT("End Object")))
		{
			CurrentNodeName.Empty();
			continue;
		}

		// Only process CustomProperties Pin lines with LinkedTo
		if (!Trimmed.StartsWith(TEXT("CustomProperties Pin")) || !Trimmed.Contains(TEXT("LinkedTo=")))
		{
			continue;
		}

		// This pin belongs to an imported node — check if CurrentNodeName is in imported set
		UEdGraphNode** SourceNodePtr = NodesByName.Find(CurrentNodeName);
		if (!SourceNodePtr || !ImportedNodes.Contains(*SourceNodePtr))
		{
			continue;
		}

		// Extract the PinId of this pin
		FString PinIdStr;
		{
			FString PinIdKey = TEXT("PinId=");
			int32 PinIdStart = Trimmed.Find(PinIdKey);
			if (PinIdStart != INDEX_NONE)
			{
				PinIdStart += PinIdKey.Len();
				int32 PinIdEnd = Trimmed.Find(TEXT(","), ESearchCase::IgnoreCase, ESearchDir::FromStart, PinIdStart);
				if (PinIdEnd != INDEX_NONE)
				{
					PinIdStr = Trimmed.Mid(PinIdStart, PinIdEnd - PinIdStart);
				}
			}
		}

		UEdGraphPin** SourcePinPtr = PinsByGuid.Find(PinIdStr);
		if (!SourcePinPtr) continue;
		UEdGraphPin* SourcePin = *SourcePinPtr;

		// Extract LinkedTo=(NodeName PinId, ...)
		FString LinkedToKey = TEXT("LinkedTo=(");
		int32 LTStart = Trimmed.Find(LinkedToKey);
		if (LTStart == INDEX_NONE) continue;
		LTStart += LinkedToKey.Len();

		int32 LTEnd = Trimmed.Find(TEXT(")"), ESearchCase::IgnoreCase, ESearchDir::FromStart, LTStart);
		if (LTEnd == INDEX_NONE) continue;

		FString LinkedToContent = Trimmed.Mid(LTStart, LTEnd - LTStart);

		// Parse pairs: "NodeName PinIdHex,NodeName2 PinIdHex2,"
		TArray<FString> Pairs;
		LinkedToContent.ParseIntoArray(Pairs, TEXT(","), true);

		for (const FString& Pair : Pairs)
		{
			FString TrimmedPair = Pair.TrimStartAndEnd();
			if (TrimmedPair.IsEmpty()) continue;

			// Split "NodeName PinIdHex"
			int32 SpaceIdx;
			if (!TrimmedPair.FindChar(TEXT(' '), SpaceIdx)) continue;

			FString TargetNodeName = TrimmedPair.Left(SpaceIdx);
			FString TargetPinId = TrimmedPair.Mid(SpaceIdx + 1);

			// Only wire to EXISTING (non-imported) nodes
			UEdGraphNode** TargetNodePtr = NodesByName.Find(TargetNodeName);
			if (!TargetNodePtr) continue;
			if (ImportedNodes.Contains(*TargetNodePtr)) continue; // Already handled by ImportNodesFromText

			// Find the target pin
			UEdGraphPin** TargetPinPtr = PinsByGuid.Find(TargetPinId);
			if (!TargetPinPtr) continue;
			UEdGraphPin* TargetPin = *TargetPinPtr;

			// Create bidirectional connection
			if (!SourcePin->LinkedTo.Contains(TargetPin))
			{
				SourcePin->Modify();
				SourcePin->LinkedTo.Add(TargetPin);
			}
			if (!TargetPin->LinkedTo.Contains(SourcePin))
			{
				TargetPin->Modify();
				TargetPin->LinkedTo.Add(SourcePin);
			}

			KISMETAI_DEV_LOG(LogKismetAI, Log, TEXT("Wired %s.%s <-> %s.%s"),
				*CurrentNodeName, *SourcePin->PinName.ToString(),
				*TargetNodeName, *TargetPin->PinName.ToString());
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Route: GET /api/v1/status
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIModule::HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("running"));
	Data->SetStringField(TEXT("version"), KismetAISkills::PluginVersion());
	Data->SetNumberField(TEXT("port"), ServerPort);

	SendSuccess(OnComplete, Data);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/export_t3d
// Request: {"blueprint": "/Game/BP_Player", "graph": "EventGraph", "strip": true}
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoExportT3D(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	FString GraphName;
	Body->TryGetStringField(TEXT("graph"), GraphName);

	bool bStrip = true;
	if (Body->HasField(TEXT("strip")))
	{
		bStrip = Body->GetBoolField(TEXT("strip"));
	}

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<FString> GraphCandidates;
	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName, &GraphCandidates);
	if (!Graph)
	{
		TArray<FString> Sugg;
		if (GraphCandidates.Num() > 0)
		{
			Sugg.Add(FString::Printf(TEXT("Ambiguous name — address one by path: %s"), *FString::Join(GraphCandidates, TEXT(", "))));
		}
		return FToolExecutionResult::Error(404, TEXT("GRAPH_NOT_FOUND"), FString::Printf(TEXT("Graph not found: %s"), *GraphName), Sugg);
	}

	// Collect nodes to export — filter by name if "nodes" array is provided
	TSet<FString> NodeFilter;
	const TArray<TSharedPtr<FJsonValue>>* NodesArrayPtr;
	if (Body->TryGetArrayField(TEXT("nodes"), NodesArrayPtr))
	{
		for (const TSharedPtr<FJsonValue>& Val : *NodesArrayPtr)
		{
			NodeFilter.Add(Val->AsString());
		}
	}
	bool bIncludeLinked = false;
	Body->TryGetBoolField(TEXT("include_linked"), bIncludeLinked);

	TSet<UObject*> NodesToExport;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		if (NodeFilter.Num() > 0 && !NodeFilter.Contains(Node->GetName()))
		{
			continue;
		}
		NodesToExport.Add(Node);

		// Pull in direct neighbors so one call answers "this node and what it wires to".
		if (bIncludeLinked && NodeFilter.Num() > 0)
		{
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
					{
						NodesToExport.Add(Linked->GetOwningNode());
					}
				}
			}
		}
	}

	FString ExportedText;
	FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);

	if (bStrip)
	{
		ExportedText = StripT3D(ExportedText);
	}

	// Build response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint"), AssetPath);
	Data->SetStringField(TEXT("graph"), Graph->GetName());

	// Apply pagination if offset/limit specified
	int32 PaginationOffset = -1;
	int32 PaginationLimit = -1;
	if (Body->HasField(TEXT("offset")))
	{
		PaginationOffset = Body->GetIntegerField(TEXT("offset"));
	}
	if (Body->HasField(TEXT("limit")))
	{
		PaginationLimit = Body->GetIntegerField(TEXT("limit"));
	}

	if (PaginationOffset >= 0 || PaginationLimit >= 0)
	{
		int32 ActualOffset = FMath::Max(0, PaginationOffset);
		int32 ActualLimit = (PaginationLimit > 0) ? PaginationLimit : 50;
		FTruncationResult Paginated = FKismetAITruncation::PaginateT3DNodes(
			ExportedText, ActualOffset, ActualLimit);
		ExportedText = Paginated.TruncatedText;

		Data->SetNumberField(TEXT("total_nodes"), Paginated.TotalItems);
		Data->SetNumberField(TEXT("offset"), ActualOffset);
		Data->SetNumberField(TEXT("limit"), ActualLimit);
		Data->SetNumberField(TEXT("returned_nodes"), Paginated.IncludedItems);
	}

	Data->SetStringField(TEXT("t3d"), ExportedText);
	Data->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());

	// Also include a list of node names and types for quick reference
	TArray<TSharedPtr<FJsonValue>> NodeArray;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("name"), Node->GetName());
		NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
		NodeObj->SetNumberField(TEXT("x"), Node->NodePosX);
		NodeObj->SetNumberField(TEXT("y"), Node->NodePosY);
		NodeArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	Data->SetArrayField(TEXT("nodes"), NodeArray);

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoExportT3D. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleExportT3D(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_blueprint"), TEXT("t3d"), FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Atomic batch rollback. See KismetAIBatchRecovery.h for why
// FScopedTransaction::Cancel() alone is not a rollback and how this one is
// scoped to the batch's own transaction record.
// ─────────────────────────────────────────────────────────────────────────────

struct FAtomicRestoreReport
{
	EKismetAIRecoveryStatus Status = EKismetAIRecoveryStatus::NotTracked;
	// Differences between the pre-batch snapshot and the state after the
	// restore attempt. Empty when Status == Restored.
	TArray<FString> Differences;
	// The revert did not run, so the batch's transaction was kept as an
	// ordinary undo entry the user can still undo.
	bool bUndoEntryKept = false;
};

// Revert the failed batch, then verify against the pre-batch snapshot. Only an
// equal snapshot counts as restored; then the package's dirty flag is put back
// to what it was before the batch (Modify() dirtied it).
static FAtomicRestoreReport RestoreFailedAtomicBatch(FKismetAIAtomicBatchScope& Scope,
	const FKismetAIStateSnapshot& Baseline, TFunctionRef<FKismetAIStateSnapshot()> Capture,
	UPackage* Package, bool bPackageWasDirtyBefore)
{
	FAtomicRestoreReport Report;
	const bool bReverted = Scope.RevertAndDiscard();
	Report.bUndoEntryKept = Scope.IsUndoEntryKept();
	const bool bEqual = FKismetAIStateSnapshot::Compare(Baseline, Capture(), Report.Differences);
	if (bEqual)
	{
		Report.Status = EKismetAIRecoveryStatus::Restored;
		if (Package)
		{
			Package->SetDirtyFlag(bPackageWasDirtyBefore);
		}
	}
	else
	{
		Report.Status = bReverted ? EKismetAIRecoveryStatus::PartiallyRestored : EKismetAIRecoveryStatus::RestoreFailed;
		UE_LOG(LogKismetAI, Warning, TEXT("Atomic batch rollback incomplete (%s), %d difference(s) from the pre-batch state"),
			FKismetAIMutationOutcome::RecoveryStatusToString(Report.Status), Report.Differences.Num());
		for (const FString& Diff : Report.Differences)
		{
			UE_LOG(LogKismetAI, Warning, TEXT("  %s"), *Diff);
		}
	}
	return Report;
}

// Outcome shared by edit_blueprint and edit_material when an atomic batch
// failed. Nothing is compiled or saved in either case: a restored asset has
// nothing to save, and an unverified one must not overwrite the file on disk,
// which still holds the pre-batch state.
static FKismetAIMutationOutcome MakeAtomicRollbackOutcome(const FAtomicRestoreReport& Report,
	const FString& PackageName, bool bPackageWasDirtyBefore, const TCHAR* SavePolicy)
{
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.PackageName = PackageName;
	Outcome.RecoveryStatus = Report.Status;
	const bool bRestored = Report.Status == EKismetAIRecoveryStatus::Restored;
	Outcome.bAppliedInMemory = !bRestored;
	Outcome.SaveSkipReason = bRestored ? KismetAISaveSkipReason::NothingApplied : KismetAISaveSkipReason::RestoreIncomplete;
	Outcome.SavePolicy = SavePolicy;
	return Outcome;
}

static void AddAtomicRollbackFields(const TSharedPtr<FJsonObject>& Data, const FAtomicRestoreReport& Report)
{
	const bool bRestored = Report.Status == EKismetAIRecoveryStatus::Restored;
	Data->SetBoolField(TEXT("rolled_back"), bRestored);
	Data->SetStringField(TEXT("recovery_status"), FKismetAIMutationOutcome::RecoveryStatusToString(Report.Status));
	Data->SetBoolField(TEXT("retry_safe"), bRestored);
	if (!bRestored)
	{
		Data->SetBoolField(TEXT("undo_entry_kept"), Report.bUndoEntryKept);
		TArray<TSharedPtr<FJsonValue>> DiffArray;
		for (const FString& Diff : Report.Differences)
		{
			DiffArray.Add(MakeShared<FJsonValueString>(Diff));
		}
		Data->SetArrayField(TEXT("restore_differences"), DiffArray);
	}
}

static FString DescribeAtomicRollback(const FAtomicRestoreReport& Report, int32 FailedCount, const TCHAR* AssetNoun)
{
	switch (Report.Status)
	{
	case EKismetAIRecoveryStatus::Restored:
		return FString::Printf(
			TEXT("atomic mode: %d operation(s) failed; every change in the batch was reverted and the %s was verified equal to its pre-batch state. Fix the failing operations and resend the full batch."),
			FailedCount, AssetNoun);
	case EKismetAIRecoveryStatus::PartiallyRestored:
		return FString::Printf(
			TEXT("atomic mode: %d operation(s) failed and the rollback is INCOMPLETE: the %s still differs from its pre-batch state in %d place(s) (see restore_differences). Do NOT resend the batch: re-read the %s, then fix only what is needed, or tell the user."),
			FailedCount, AssetNoun, Report.Differences.Num(), AssetNoun);
	case EKismetAIRecoveryStatus::RestoreFailed:
	default:
		return FString::Printf(
			TEXT("atomic mode: %d operation(s) failed and the rollback could NOT run: the operations that succeeded are still applied to the %s (the batch remains in the editor's undo history). Do NOT resend the batch: re-read the %s, then fix only what is needed, or tell the user."),
			FailedCount, AssetNoun, AssetNoun);
	}
}

// An atomic batch needs the outermost transaction; inside someone else's open
// transaction it could only be undone together with that transaction.
static FToolExecutionResult AtomicBatchUnavailableError()
{
	return FToolExecutionResult::Error(409, TEXT("TRANSACTION_ACTIVE"),
		TEXT("atomic=true needs its own undo transaction, but another editor transaction is open (a drag or another edit in progress). Nothing was changed. Retry once the editor is idle, or send the batch with atomic=false."));
}

// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/inject_t3d
// Request: {"blueprint": "/Game/BP_Player", "graph": "EventGraph", "t3d": "Begin Object..."}
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoInjectT3D(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	FString GraphName;
	Body->TryGetStringField(TEXT("graph"), GraphName);

	FString T3DText;
	if (!Body->TryGetStringField(TEXT("t3d"), T3DText) || T3DText.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'t3d' field is required and must be non-empty"));
	}

	// Opt-in proactive reflow (default off): after importing + placing the new
	// nodes, reformat the connected component(s) they joined.
	bool bAutoLayout = false;
	Body->TryGetBoolField(TEXT("auto_layout"), bAutoLayout);

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FToolExecutionResult::Error(404, TEXT("GRAPH_NOT_FOUND"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	// --- T3D Injection Pipeline ---

	// 1. Resolve GUID placeholders
	FString ResolvedT3D = ResolvePlaceholders(T3DText);

	// 1.2. Complete incomplete macro instance references (GraphBlueprint + GraphGuid)
	//      so ForEachLoop/DoOnce/etc. import instead of being silently dropped.
	CompleteMacroReferences(ResolvedT3D);

	// 1.3. Validate the buffer BEFORE it reaches the importer. A malformed pin
	//      record (bad PinId/PinName, unbalanced parens, dangling LinkedTo, or a
	//      leftover placeholder) crashes the editor uncatchably on macOS, so reject
	//      it here with an actionable error instead of importing it.
	{
		FString ValidationError;
		if (!ValidateT3DForImport(ResolvedT3D, Graph, ValidationError))
		{
			TArray<FString> Suggestions;
			Suggestions.Add(TEXT("Every pin needs a valid PinId (use LINK_* placeholders) and a non-empty PinName."));
			Suggestions.Add(TEXT("Only use LinkedTo between NEW nodes in this call; wire to existing nodes with edit_blueprint."));
			Suggestions.Add(TEXT("Read a similar existing node with read_blueprint(mode='t3d') to copy the exact pin format."));
			return FToolExecutionResult::Error(400, TEXT("INVALID_T3D"), ValidationError, Suggestions);
		}
	}

	// 1.5. Count requested node blocks for import validation
	int32 RequestedNodeCount = 0;
	TArray<FString> RequestedNodeNames;
	{
		const TCHAR* Cursor = *ResolvedT3D;
		while (Cursor && *Cursor)
		{
			const TCHAR* Found = FCString::Stristr(Cursor, TEXT("Begin Object"));
			if (!Found) break;
			RequestedNodeCount++;
			// Extract Name="..." from the Begin Object line
			const TCHAR* NameStart = FCString::Stristr(Found, TEXT("Name=\""));
			if (NameStart)
			{
				NameStart += 6; // skip Name="
				const TCHAR* NameEnd = FCString::Strchr(NameStart, '"');
				if (NameEnd)
				{
					RequestedNodeNames.Add(FString(NameEnd - NameStart, NameStart));
				}
			}
			Cursor = Found + 12; // skip past "Begin Object"
		}
	}

	// Captured before any Modify() so the outcome can disclose a save that also
	// writes edits the user had not saved yet.
	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);

	// 2. Import nodes within a transaction (for undo support)
	TSet<UEdGraphNode*> ImportedNodes;
	TArray<FString> SchemaRejectedNames; // nodes the schema does not allow in this graph
	{
		FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "InjectT3D", "Kismet: Inject T3D Nodes"));

		Graph->Modify();
		Blueprint->Modify();

		FEdGraphUtilities::ImportNodesFromText(Graph, ResolvedT3D, ImportedNodes);

		// Schema gate: FEdGraphUtilities::ImportNodesFromText does NOT consult
		// CanCreateUnderSpecifiedSchema, and that check is asymmetric anyway (a K2 node
		// returns true under EVERY schema, so it would not block a K2 node from an anim
		// graph). Enforce the engine's actual rule instead:
		//   * An anim (pose) graph legally hosts UAnimGraphNode_* AND pure data K2 nodes
		//     (variable getters, pure function calls, knots, conversions, struct break) —
		//     the only illegal K2 nodes are IMPURE (exec) ones. UAnimationGraphSchema is a
		//     GT_Animation graph, and UEdGraphSchema_K2::DoesGraphSupportImpureFunctions
		//     returns false for GT_Animation, so the proxy is: a non-anim node is rejected
		//     iff it has any exec pin. Knots and comments are always fine.
		//   * A K2 graph rejects UAnimGraphNode_* (pose nodes never belong there).
		// Rejected nodes are removed and reported so the caller gets a clear schema error
		// rather than a node that silently fails to compile.
		const bool bTargetIsAnim = AnimSpawnerHelpers::IsAnimNodeGraph(Graph);
		{
			TArray<UEdGraphNode*> ToReject;
			for (UEdGraphNode* Node : ImportedNodes)
			{
				if (!Node) continue;
				if (Node->IsA<UEdGraphNode_Comment>()) continue; // comments are legal anywhere
				if (Node->IsA<UK2Node_Knot>()) continue;         // reroute/knot nodes are legal anywhere
				const bool bIsAnimNode = Node->IsA<UAnimGraphNode_Base>();
				bool bRejected;
				if (bTargetIsAnim)
				{
					// Pure (no-exec) K2 data nodes are legal in an anim graph; impure ones are not.
					bRejected = !bIsAnimNode && AnimSpawnerHelpers::NodeHasExecPin(Node);
				}
				else
				{
					// An anim pose node has no place in a K2 graph.
					bRejected = bIsAnimNode;
				}
				if (bRejected)
				{
					ToReject.Add(Node);
				}
			}
			for (UEdGraphNode* Node : ToReject)
			{
				SchemaRejectedNames.Add(FString::Printf(TEXT("%s (%s)"), *Node->GetName(), *Node->GetClass()->GetName()));
				ImportedNodes.Remove(Node);
				FBlueprintEditorUtils::RemoveNode(Blueprint, Node, /*bDontRecompile*/ true);
			}
		}
	}

	// All requested nodes were schema-rejected → hard error (nothing usable imported).
	if (ImportedNodes.Num() == 0 && SchemaRejectedNames.Num() > 0)
	{
		const bool bTargetIsAnimGraph = AnimSpawnerHelpers::IsAnimNodeGraph(Graph);
		return FToolExecutionResult::Error(400, TEXT("SCHEMA_MISMATCH"),
			FString::Printf(TEXT("Node(s) rejected by the target graph's schema and not added: %s."), *FString::Join(SchemaRejectedNames, TEXT(", "))),
			bTargetIsAnimGraph
				? TArray<FString>{TEXT("This is an animation graph: impure (exec) nodes are not legal here — pure data nodes (variable getters, pure functions, struct breaks, knots) ARE. For pose nodes use insert_node with anim node menu names (e.g. \"Two Way Blend\", \"Play 'Asset'\").")}
				: TArray<FString>{TEXT("Animation pose nodes only belong in an Animation Blueprint's anim graph, not in a K2 (event/function) graph.")});
	}

	if (ImportedNodes.Num() == 0)
	{
		FString ErrorMsg = FString::Printf(TEXT("ImportNodesFromText produced no nodes from %d Begin Object block(s). Check T3D format."), RequestedNodeCount);
		if (RequestedNodeNames.Num() > 0)
		{
			ErrorMsg += TEXT(" Requested nodes: ") + FString::Join(RequestedNodeNames, TEXT(", "));
		}
		TArray<FString> Suggestions;
		Suggestions.Add(TEXT("Verify the FunctionReference path is correct. For math: MemberParent=Class'/Script/Engine.KismetMathLibrary'"));
		Suggestions.Add(TEXT("Use read_blueprint(mode='t3d') on a similar existing node to see the correct format"));
		Suggestions.Add(TEXT("UE5 uses DoubleDouble not FloatFloat: Multiply_DoubleDouble, Add_DoubleDouble"));
		return FToolExecutionResult::Error(400, TEXT("IMPORT_FAILED"), ErrorMsg, Suggestions);
	}

	// 2.5. Check for partial import failure (some nodes silently rejected)
	int32 RejectedCount = RequestedNodeCount - ImportedNodes.Num();
	TArray<FString> ImportWarnings;

	// Surface schema rejections (a partial batch where some nodes were illegal here).
	if (SchemaRejectedNames.Num() > 0)
	{
		const bool bTargetIsAnimGraph = AnimSpawnerHelpers::IsAnimNodeGraph(Graph);
		ImportWarnings.Add(FString::Printf(
			TEXT("WARNING: %d node(s) were rejected by the target graph's schema and NOT added: %s. %s"),
			SchemaRejectedNames.Num(), *FString::Join(SchemaRejectedNames, TEXT(", ")),
			bTargetIsAnimGraph
				? TEXT("In an animation graph, impure (exec) nodes are not legal — pure data nodes (getters, pure functions, struct breaks) are.")
				: TEXT("Animation pose nodes only belong in an anim graph.")));
	}

	if (RejectedCount > 0)
	{
		// Find which names were NOT imported
		TSet<FString> ImportedNames;
		for (UEdGraphNode* Node : ImportedNodes)
		{
			if (Node) ImportedNames.Add(Node->GetName());
		}
		TArray<FString> MissingNames;
		for (const FString& ReqName : RequestedNodeNames)
		{
			if (!ImportedNames.Contains(ReqName))
			{
				MissingNames.Add(ReqName);
			}
		}
		FString Warning = FString::Printf(TEXT("WARNING: %d of %d nodes failed to import (silently rejected by UE)."),
			RejectedCount, RequestedNodeCount);
		if (MissingNames.Num() > 0)
		{
			Warning += TEXT(" Missing: ") + FString::Join(MissingNames, TEXT(", "));
		}

		// Schema-mismatch diagnosis: ImportNodesFromText silently drops a node whose class
		// fails CanCreateUnderSpecifiedSchema for the target graph (a K2 node into an anim
		// graph, or an anim node into a K2 graph). Scan the T3D's "Begin Object Class=..."
		// lines for the missing names and report the real reason instead of a generic hint.
		const bool bTargetIsAnim = AnimSpawnerHelpers::IsAnimNodeGraph(Graph);
		TArray<FString> SchemaRejected;
		{
			const TCHAR* Cursor = *ResolvedT3D;
			while (Cursor && *Cursor)
			{
				const TCHAR* Begin = FCString::Stristr(Cursor, TEXT("Begin Object"));
				if (!Begin) break;
				const TCHAR* LineEnd = FCString::Strchr(Begin, '\n');
				const FString Line = LineEnd ? FString(LineEnd - Begin, Begin) : FString(Begin);
				Cursor = LineEnd ? LineEnd + 1 : nullptr;

				// Pull Name="..." and Class=... off this header line.
				FString ThisName;
				if (const TCHAR* N = FCString::Stristr(*Line, TEXT("Name=\"")))
				{
					const TCHAR* NS = N + 6;
					if (const TCHAR* NE = FCString::Strchr(NS, '"')) ThisName = FString(NE - NS, NS);
				}
				if (ThisName.IsEmpty() || !MissingNames.Contains(ThisName)) continue;

				FString ClassPath;
				if (const TCHAR* C = FCString::Stristr(*Line, TEXT("Class=")))
				{
					const TCHAR* CS = C + 6;
					int32 Len = 0;
					while (CS[Len] && CS[Len] != ' ' && CS[Len] != '\r' && CS[Len] != '\t') ++Len;
					ClassPath = FString(Len, CS);
				}
				if (UClass* NodeClass = UClass::TryFindTypeSlow<UClass>(ClassPath))
				{
					const bool bIsAnimClass = NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass());
					if (bTargetIsAnim && !bIsAnimClass)
					{
						SchemaRejected.Add(FString::Printf(TEXT("%s (%s is a K2 node, not legal in an animation graph)"), *ThisName, *NodeClass->GetName()));
					}
					else if (!bTargetIsAnim && bIsAnimClass)
					{
						SchemaRejected.Add(FString::Printf(TEXT("%s (%s is an animation node, not legal in a K2 graph)"), *ThisName, *NodeClass->GetName()));
					}
				}
			}
		}

		if (SchemaRejected.Num() > 0)
		{
			Warning += FString::Printf(TEXT(". Schema rejection: %s. %s"),
				*FString::Join(SchemaRejected, TEXT("; ")),
				bTargetIsAnim
					? TEXT("Use insert_node with anim node menu names (e.g. \"Two Way Blend\", \"Play 'Asset'\") for animation graphs.")
					: TEXT("Animation nodes only belong in an Animation Blueprint's anim graph."));
		}
		else
		{
			Warning += TEXT(". Check FunctionReference paths. For math: use MemberParent=Class'/Script/Engine.KismetMathLibrary', MemberName=\"Multiply_DoubleDouble\"");
		}
		ImportWarnings.Add(Warning);
	}

	// 2.6. Check for unresolved function references (imported but broken)
	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (UK2Node_CallFunction* FuncNode = Cast<UK2Node_CallFunction>(Node))
		{
			if (FuncNode->GetTargetFunction() == nullptr)
			{
				FString FuncName = FuncNode->FunctionReference.GetMemberName().ToString();
				ImportWarnings.Add(FString::Printf(
					TEXT("WARNING: Node '%s' has unresolved function '%s'. This will cause compilation errors."),
					*Node->GetName(), *FuncName));
			}
		}
	}

	// 3. Post-import sanitization: remove orphaned LinkedTo references
	SanitizeImportedNodes(ImportedNodes);

	// 3.5. Wire connections from imported nodes to existing nodes in the graph
	WireExistingNodeConnections(Graph, ImportedNodes, ResolvedT3D);

	// 3.6. Place the imported nodes beside their connection anchor. Only the
	// new nodes move — pre-existing nodes and comment boxes are obstacles.
	// Island nodes (no link to an existing node) are tracked so a later
	// edit_blueprint connect can pull them next to the node they end up feeding.
	{
		TSet<UEdGraphNode*> IslandNodes;
		FKismetAILayout::PlaceNewNodes(Graph, ImportedNodes, &IslandNodes);
		RecordUnanchoredSpawns(AssetPath, IslandNodes);

		// Opt-in: reflow the component(s) the imported nodes joined (overwrites
		// authored positions in those components — only on explicit request).
		if (bAutoLayout && ImportedNodes.Num() > 0)
		{
			FKismetAILayout::FormatNodes(Graph, ImportedNodes);
		}
	}

	// 4. Mark as structurally modified and compile
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

	// 4b. Record the compile, then persist (policy: saved even if the compile
	// failed — the outcome says so) and record what the save did.
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.bAppliedInMemory = ImportedNodes.Num() > 0;
	RecordCompileOutcome(Outcome, Blueprint, Results);
	AutoSaveAfterMutation(Blueprint, Outcome);

	// 5. Collect compile errors/warnings
	TArray<TSharedPtr<FJsonValue>> ErrorsArray;
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();

		EMessageSeverity::Type Severity = Message->GetSeverity();
		FString SeverityStr;
		switch (Severity)
		{
		case EMessageSeverity::Error: SeverityStr = TEXT("error"); break;
		case EMessageSeverity::Warning: SeverityStr = TEXT("warning"); break;
		case EMessageSeverity::Info: SeverityStr = TEXT("info"); break;
		default: SeverityStr = TEXT("note"); break;
		}

		MsgObj->SetStringField(TEXT("severity"), SeverityStr);
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());
		ErrorsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	bool bCompileSuccess = (Blueprint->Status != BS_Error);

	// 6. Readback: export the imported nodes as T3D for verification
	TSet<UObject*> NodesToReadback;
	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (Node)
		{
			NodesToReadback.Add(Node);
		}
	}

	FString ReadbackT3D;
	FEdGraphUtilities::ExportNodesToText(NodesToReadback, ReadbackT3D);

	// 7. Build response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetNumberField(TEXT("nodes_requested"), RequestedNodeCount);
	Data->SetNumberField(TEXT("nodes_imported"), ImportedNodes.Num());
	if (RejectedCount > 0)
	{
		Data->SetNumberField(TEXT("nodes_rejected"), RejectedCount);
	}
	Data->SetBoolField(TEXT("compile_success"), bCompileSuccess);
	Data->SetArrayField(TEXT("compile_messages"), ErrorsArray);

	// Include import warnings (partial failures, unresolved references)
	if (ImportWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> WarningsArray;
		for (const FString& Warning : ImportWarnings)
		{
			WarningsArray.Add(MakeShared<FJsonValueString>(Warning));
		}
		Data->SetArrayField(TEXT("import_warnings"), WarningsArray);
	}

	// Imported node summary
	TArray<TSharedPtr<FJsonValue>> ImportedArray;
	for (UEdGraphNode* Node : ImportedNodes)
	{
		if (!Node) continue;
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("name"), Node->GetName());
		NodeObj->SetStringField(TEXT("class"), Node->GetClass()->GetName());
		NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

		// Pin connections summary
		TArray<TSharedPtr<FJsonValue>> PinsArray;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
			PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
			PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
			PinObj->SetStringField(TEXT("type"), Pin->PinType.PinCategory.ToString());

			if (!Pin->DefaultValue.IsEmpty())
			{
				PinObj->SetStringField(TEXT("default"), Pin->DefaultValue);
			}

			if (Pin->LinkedTo.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> LinksArray;
				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						FString LinkStr = FString::Printf(TEXT("%s.%s"),
							*LinkedPin->GetOwningNode()->GetName(),
							*LinkedPin->PinName.ToString());
						LinksArray.Add(MakeShared<FJsonValueString>(LinkStr));
					}
				}
				PinObj->SetArrayField(TEXT("linked_to"), LinksArray);
			}

			PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
		}
		NodeObj->SetArrayField(TEXT("pins"), PinsArray);
		ImportedArray.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	Data->SetArrayField(TEXT("imported_nodes"), ImportedArray);
	Data->SetStringField(TEXT("readback_t3d"), StripT3D(ReadbackT3D));

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoInjectT3D. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleInjectT3D(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("write_blueprint"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/compile
// Request: {"blueprint": "/Game/BP_Player"}
// ─────────────────────────────────────────────────────────────────────────────

// compile_blueprint, inverted: the work returns a typed result and the
// HTTP route below is a thin adapter over it. The agent calls this directly
// rather than fabricating an FHttpServerRequest and string-matching the response
// for "success":false.
FToolExecutionResult FKismetAIModule::DoCompileBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	if (!Args.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Args->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"),
			TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"),
			FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);

	TArray<TSharedPtr<FJsonValue>> MessagesArray;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();

		EMessageSeverity::Type Severity = Message->GetSeverity();
		FString SeverityStr;
		switch (Severity)
		{
		case EMessageSeverity::Error:
			SeverityStr = TEXT("error");
			ErrorCount++;
			break;
		case EMessageSeverity::Warning:
			SeverityStr = TEXT("warning");
			WarningCount++;
			break;
		default:
			SeverityStr = TEXT("info");
			break;
		}

		MsgObj->SetStringField(TEXT("severity"), SeverityStr);
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());
		MessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	bool bSuccess = (Blueprint->Status != BS_Error);

	// Compile-only: nothing applied, nothing saved. The outcome still carries
	// the compile state so the run ledger reflects the latest compile.
	FKismetAIMutationOutcome Outcome;
	Outcome.PackageName = Blueprint->GetOutermost()->GetName();
	Outcome.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
	RecordCompileOutcome(Outcome, Blueprint, Results);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetBoolField(TEXT("success"), bSuccess);
	Data->SetStringField(TEXT("status"), bSuccess ? TEXT("compiled") : TEXT("error"));
	Data->SetNumberField(TEXT("errors"), ErrorCount);
	Data->SetNumberField(TEXT("warnings"), WarningCount);
	Data->SetArrayField(TEXT("messages"), MessagesArray);

	// Reachability lint: the compiler stays silent about exec subgraphs that
	// are wired internally but never connected to an entry point.
	TArray<TSharedPtr<FJsonValue>> UnreachableArray;
	TArray<UEdGraph*> AllGraphs;
	Blueprint->GetAllGraphs(AllGraphs);
	for (UEdGraph* BPGraph : AllGraphs)
	{
		if (!BPGraph) continue;
		for (const FString& W : DetectUnreachableExecNodes(BPGraph))
		{
			UnreachableArray.Add(MakeShared<FJsonValueString>(W));
		}
	}
	if (UnreachableArray.Num() > 0)
	{
		Data->SetArrayField(TEXT("unreachable_node_warnings"), UnreachableArray);
	}

	return FToolExecutionResult::Success(Data, bSuccess);
}

// HTTP adapter. Parses the body, runs the same function the agent runs, and
// serializes the result. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleCompile(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("compile_blueprint"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}

// ─────────────────────────────────────────────────────────────────────────────
// Materials — shared helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	/** Friendly node-class name (strip the "MaterialExpression" prefix). */
	static FString MaterialExprFriendlyClass(const UMaterialExpression* Expr)
	{
		FString N = Expr->GetClass()->GetName();
		N.RemoveFromStart(TEXT("MaterialExpression"), ESearchCase::IgnoreCase);
		return N;
	}

	/** First caption line for a node (e.g. "Multiply", "Constant3Vector"). */
	static FString MaterialExprTitle(UMaterialExpression* Expr)
	{
		TArray<FString> Captions;
		Expr->GetCaption(Captions);
		return Captions.Num() > 0 ? Captions[0] : MaterialExprFriendlyClass(Expr);
	}

	/** Stringify a reflected enum value, falling back to the integer if reflection is missing. */
	template<typename TEnum>
	static FString EnumToString(TEnum Value)
	{
		if (const UEnum* E = StaticEnum<TEnum>())
		{
			return E->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}

	/** Parse a reflected enum from a string, accepting either the bare enumerator ("Opaque") or the
	 *  fully-prefixed name ("BLEND_Opaque"). Returns false if unmatched. */
	template<typename TEnum>
	static bool EnumFromString(const FString& Name, const TCHAR* Prefix, TEnum& OutValue)
	{
		const UEnum* E = StaticEnum<TEnum>();
		if (!E) return false;
		int64 Val = E->GetValueByNameString(Name);
		if (Val == INDEX_NONE && Prefix)
		{
			Val = E->GetValueByNameString(FString(Prefix) + Name);
		}
		if (Val == INDEX_NONE) return false;
		OutValue = static_cast<TEnum>(Val);
		return true;
	}

	/** The material root properties we surface, with stable display names. */
	struct FRootPropertyEntry { EMaterialProperty Property; const TCHAR* Name; };
	static const FRootPropertyEntry GMaterialRootProperties[] = {
		{ MP_BaseColor,           TEXT("BaseColor") },
		{ MP_Metallic,            TEXT("Metallic") },
		{ MP_Specular,            TEXT("Specular") },
		{ MP_Roughness,           TEXT("Roughness") },
		{ MP_Anisotropy,          TEXT("Anisotropy") },
		{ MP_EmissiveColor,       TEXT("EmissiveColor") },
		{ MP_Opacity,             TEXT("Opacity") },
		{ MP_OpacityMask,         TEXT("OpacityMask") },
		{ MP_Normal,              TEXT("Normal") },
		{ MP_Tangent,             TEXT("Tangent") },
		{ MP_WorldPositionOffset, TEXT("WorldPositionOffset") },
		{ MP_SubsurfaceColor,     TEXT("SubsurfaceColor") },
		{ MP_AmbientOcclusion,    TEXT("AmbientOcclusion") },
		{ MP_Refraction,          TEXT("Refraction") },
		{ MP_PixelDepthOffset,    TEXT("PixelDepthOffset") },
		{ MP_Displacement,        TEXT("Displacement") },
		{ MP_FrontMaterial,       TEXT("FrontMaterial") },
		{ MP_MaterialAttributes,  TEXT("MaterialAttributes") },
	};

	/** Serialize one expression node: id, class, title, position, parameter name, and inputs
	 *  with their upstream connections (node id + output index). */
	static TSharedPtr<FJsonObject> SerializeMaterialExpression(UMaterialExpression* Expr)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Expr->GetName());
		Obj->SetStringField(TEXT("class"), MaterialExprFriendlyClass(Expr));
		Obj->SetStringField(TEXT("title"), MaterialExprTitle(Expr));
		Obj->SetNumberField(TEXT("pos_x"), Expr->MaterialExpressionEditorX);
		Obj->SetNumberField(TEXT("pos_y"), Expr->MaterialExpressionEditorY);

		if (const UMaterialExpressionParameter* Param = Cast<UMaterialExpressionParameter>(Expr))
		{
			Obj->SetStringField(TEXT("parameter_name"), Param->ParameterName.ToString());
		}

		// A function call node is opaque here — surface the called function's asset path so the
		// graph inside it can be read with read_material on that path.
		if (const UMaterialExpressionMaterialFunctionCall* FuncCall = Cast<UMaterialExpressionMaterialFunctionCall>(Expr))
		{
			if (FuncCall->MaterialFunction)
			{
				Obj->SetStringField(TEXT("function"), FuncCall->MaterialFunction->GetPathName());
			}
		}

		TArray<TSharedPtr<FJsonValue>> Inputs;
		const int32 NumInputs = Expr->CountInputs();
		for (int32 i = 0; i < NumInputs; ++i)
		{
			FExpressionInput* In = Expr->GetInput(i);
			if (!In) continue;
			TSharedPtr<FJsonObject> InObj = MakeShared<FJsonObject>();
			InObj->SetNumberField(TEXT("index"), i);
			// Empty name = an unnamed input (single-input math nodes); address it with to_input:"".
			InObj->SetStringField(TEXT("name"), Expr->GetInputName(i).ToString());
			const bool bConnected = (In->Expression != nullptr);
			InObj->SetBoolField(TEXT("connected"), bConnected);
			if (bConnected)
			{
				InObj->SetStringField(TEXT("from"), In->Expression->GetName());
				InObj->SetNumberField(TEXT("from_output"), In->OutputIndex);
			}
			Inputs.Add(MakeShared<FJsonValueObject>(InObj));
		}
		Obj->SetArrayField(TEXT("inputs"), Inputs);

		// Outputs — names are often empty (the single default output); index is what matters.
		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (int32 i = 0; i < Expr->Outputs.Num(); ++i)
		{
			TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
			OutObj->SetNumberField(TEXT("index"), i);
			OutObj->SetStringField(TEXT("name"), Expr->Outputs[i].OutputName.ToString());
			Outputs.Add(MakeShared<FJsonValueObject>(OutObj));
		}
		Obj->SetArrayField(TEXT("outputs"), Outputs);
		return Obj;
	}

	/** Serialize a Material Function's graph: its exposed inputs/outputs (the pins a function
	 *  call node shows) plus every inner expression node. Same node format as describe on a
	 *  base material, so the agent reads nested functions with the exact same mental model. */
	static TSharedPtr<FJsonObject> SerializeMaterialFunction(UMaterialFunction* Func)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("material"), Func->GetPathName());
		Data->SetStringField(TEXT("type"), TEXT("material_function"));
		if (!Func->Description.IsEmpty())
		{
			Data->SetStringField(TEXT("description"), Func->Description);
		}

		// The function's interface — FunctionInput/FunctionOutput expressions, sorted by
		// priority. These are what appear as pins on a function call node in a parent material.
		TArray<FFunctionExpressionInput> FuncInputs;
		TArray<FFunctionExpressionOutput> FuncOutputs;
		Func->GetInputsAndOutputs(FuncInputs, FuncOutputs);

		TArray<TSharedPtr<FJsonValue>> InputsArr;
		for (const FFunctionExpressionInput& In : FuncInputs)
		{
			if (!In.ExpressionInput) continue;
			TSharedPtr<FJsonObject> InObj = MakeShared<FJsonObject>();
			InObj->SetStringField(TEXT("name"), In.ExpressionInput->InputName.ToString());
			InObj->SetStringField(TEXT("id"), In.ExpressionInput->GetName());
			InObj->SetStringField(TEXT("input_type"), EnumToString<EFunctionInputType>(In.ExpressionInput->InputType.GetValue()));
			if (!In.ExpressionInput->Description.IsEmpty())
			{
				InObj->SetStringField(TEXT("description"), In.ExpressionInput->Description);
			}
			InputsArr.Add(MakeShared<FJsonValueObject>(InObj));
		}
		Data->SetArrayField(TEXT("function_inputs"), InputsArr);

		TArray<TSharedPtr<FJsonValue>> OutputsArr;
		for (const FFunctionExpressionOutput& Out : FuncOutputs)
		{
			if (!Out.ExpressionOutput) continue;
			TSharedPtr<FJsonObject> OutObj = MakeShared<FJsonObject>();
			OutObj->SetStringField(TEXT("name"), Out.ExpressionOutput->OutputName.ToString());
			OutObj->SetStringField(TEXT("id"), Out.ExpressionOutput->GetName());
			if (!Out.ExpressionOutput->Description.IsEmpty())
			{
				OutObj->SetStringField(TEXT("description"), Out.ExpressionOutput->Description);
			}
			OutputsArr.Add(MakeShared<FJsonValueObject>(OutObj));
		}
		Data->SetArrayField(TEXT("function_outputs"), OutputsArr);

		// Every inner expression node. FunctionInput/FunctionOutput expressions appear here too —
		// their connections show how the interface wires into the body.
		TArray<TSharedPtr<FJsonValue>> Exprs;
		for (UMaterialExpression* Expr : Func->GetExpressions())
		{
			if (!Expr) continue;
			Exprs.Add(MakeShared<FJsonValueObject>(SerializeMaterialExpression(Expr)));
		}
		Data->SetArrayField(TEXT("expressions"), Exprs);
		Data->SetNumberField(TEXT("expression_count"), Exprs.Num());
		return Data;
	}

	/** Find an expression in a material by its object name (the "id" describe returns). */
	static UMaterialExpression* FindExpressionByName(UMaterial* Material, const FString& Name)
	{
		for (UMaterialExpression* Expr : Material->GetExpressions())
		{
			if (Expr && Expr->GetName() == Name)
			{
				return Expr;
			}
		}
		return nullptr;
	}

	/** Find an input on an expression by its (case-insensitive) pin name. Returns the FExpressionInput
	 *  and its index, or nullptr. Empty Name selects the first input. */
	static FExpressionInput* FindExpressionInputByName(UMaterialExpression* Expr, const FString& Name, int32& OutIndex)
	{
		const int32 NumInputs = Expr->CountInputs();
		for (int32 i = 0; i < NumInputs; ++i)
		{
			FExpressionInput* In = Expr->GetInput(i);
			if (!In) continue;
			if (Name.IsEmpty() || Expr->GetInputName(i).ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				OutIndex = i;
				return In;
			}
		}
		OutIndex = INDEX_NONE;
		return nullptr;
	}

	/** Map a root-property display name (e.g. "BaseColor") to its EMaterialProperty. */
	static bool MaterialPropertyFromName(const FString& Name, EMaterialProperty& OutProp)
	{
		for (const FRootPropertyEntry& Entry : GMaterialRootProperties)
		{
			if (Name.Equals(Entry.Name, ESearchCase::IgnoreCase))
			{
				OutProp = Entry.Property;
				return true;
			}
		}
		return false;
	}

	/** Human-readable list of an expression's addressable inputs, for connect error messages.
	 *  Single-input math nodes (Sine, Abs, Floor…) have an UNNAMED input — its pin name is empty,
	 *  so it must be addressed with to_input:"" . This is the #1 cause of silent connect failures. */
	static FString DescribeExpressionInputs(UMaterialExpression* Expr)
	{
		if (!Expr) return TEXT("(none)");
		TArray<FString> Parts;
		const int32 NumInputs = Expr->CountInputs();
		for (int32 i = 0; i < NumInputs; ++i)
		{
			if (!Expr->GetInput(i)) continue;
			const FString Name = Expr->GetInputName(i).ToString();
			Parts.Add(Name.IsEmpty()
				? FString::Printf(TEXT("[%d] \"\" (unnamed — pass to_input:\"\")"), i)
				: FString::Printf(TEXT("[%d] \"%s\""), i, *Name));
		}
		return Parts.Num() ? FString::Join(Parts, TEXT(", ")) : TEXT("(no inputs)");
	}

	/** If a base material is open in a Material Editor, return that editor. The editor works on a
	 *  transient PREVIEW duplicate of the asset (FMaterialEditor::Material), not the asset itself,
	 *  so our tools must target that preview to show up live and to avoid being clobbered when the
	 *  user saves. Returns nullptr when the material isn't open. */
	static IMaterialEditor* FindOpenMaterialEditor(UObject* MaterialAsset)
	{
		if (!GEditor || !MaterialAsset) return nullptr;
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!Subsystem) return nullptr;
		IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(MaterialAsset, /*bFocusIfOpen=*/false);
		// GetEditorName() == GetToolkitFName() == "MaterialEditor" guarantees the static_cast is valid.
		if (!Instance || Instance->GetEditorName() != FName(TEXT("MaterialEditor"))) return nullptr;
		return static_cast<IMaterialEditor*>(Instance);
	}

	/** Resolve the base UMaterial to read/edit for Original. When Original is open in a Material
	 *  Editor, returns the editor's live preview material and sets OutEditor (edits then show
	 *  instantly and the user's Ctrl+S commits preview→asset); otherwise returns Original with
	 *  OutEditor = nullptr. Node ids (object names) are preserved across the duplicate, so the
	 *  agent's ids stay valid either way. */
	static UMaterial* ResolveMaterialEditTarget(UMaterial* Original, IMaterialEditor*& OutEditor)
	{
		OutEditor = nullptr;
		if (!Original) return nullptr;
		if (IMaterialEditor* Editor = FindOpenMaterialEditor(Original))
		{
			if (UMaterial* Preview = Cast<UMaterial>(Editor->GetMaterialInterface()))
			{
				OutEditor = Editor;
				return Preview;
			}
		}
		return Original;
	}

	/** Push externally-applied expression/setting edits into an open Material Editor: rebuild its
	 *  graph from the (just-edited) preview material, refresh node previews, and mark it dirty so the
	 *  change shows live and Ctrl+S still saves. No-op when Editor is null (material not open). */
	static void SyncOpenMaterialEditor(IMaterialEditor* Editor, UMaterial* PreviewMaterial)
	{
		if (!Editor || !PreviewMaterial) return;
		if (UMaterialGraph* Graph = PreviewMaterial->MaterialGraph)
		{
			Graph->RebuildGraph();
		}
		Editor->ForceRefreshExpressionPreviews();
		Editor->MarkMaterialDirty();
		Editor->UpdateDetailView();
	}

	static const FMaterialResource* GetCurrentMaterialResource(UMaterialInterface* Material)
	{
		if (!Material) return nullptr;

#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
		return Material->GetMaterialResource(GMaxRHIShaderPlatform);
#else
		return Material->GetMaterialResource(GMaxRHIFeatureLevel);
#endif
	}

	/** Set a UProperty on an expression from a JSON value. Handles float/int/bool/string/name,
	 *  object references (asset path), and FLinearColor/FVector structs (JSON array or {r,g,b,a}).
	 *  Returns false with OutError on failure. */
	static bool SetExpressionProperty(UMaterialExpression* Expr, const FString& PropName,
		const TSharedPtr<FJsonValue>& Value, FString& OutError)
	{
		// Case-insensitive property lookup.
		FProperty* Prop = Expr->GetClass()->FindPropertyByName(FName(*PropName));
		if (!Prop)
		{
			for (TFieldIterator<FProperty> It(Expr->GetClass()); It; ++It)
			{
				if (It->GetName().Equals(PropName, ESearchCase::IgnoreCase)) { Prop = *It; break; }
			}
		}
		if (!Prop)
		{
			OutError = FString::Printf(TEXT("'%s' has no property '%s'"), *Expr->GetClass()->GetName(), *PropName);
			return false;
		}

		void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Expr);

		auto ParseColor = [&Value](FLinearColor& Out) -> bool
		{
			const TArray<TSharedPtr<FJsonValue>>* Arr;
			if (Value->TryGetArray(Arr))
			{
				if (Arr->Num() >= 3)
				{
					Out.R = (*Arr)[0]->AsNumber(); Out.G = (*Arr)[1]->AsNumber(); Out.B = (*Arr)[2]->AsNumber();
					Out.A = Arr->Num() >= 4 ? (*Arr)[3]->AsNumber() : 1.0f;
					return true;
				}
			}
			const TSharedPtr<FJsonObject>* O;
			if (Value->TryGetObject(O))
			{
				(*O)->TryGetNumberField(TEXT("r"), Out.R); (*O)->TryGetNumberField(TEXT("g"), Out.G);
				(*O)->TryGetNumberField(TEXT("b"), Out.B);
				if (!(*O)->TryGetNumberField(TEXT("a"), Out.A)) Out.A = 1.0f;
				return true;
			}
			return false;
		};

		if (const FFloatProperty* FP = CastField<FFloatProperty>(Prop))
		{
			FP->SetPropertyValue(ValuePtr, Value->AsNumber());
		}
		else if (const FDoubleProperty* DP = CastField<FDoubleProperty>(Prop))
		{
			DP->SetPropertyValue(ValuePtr, Value->AsNumber());
		}
		else if (const FIntProperty* IP = CastField<FIntProperty>(Prop))
		{
			IP->SetPropertyValue(ValuePtr, static_cast<int32>(Value->AsNumber()));
		}
		else if (const FBoolProperty* BP = CastField<FBoolProperty>(Prop))
		{
			BP->SetPropertyValue(ValuePtr, Value->AsBool());
		}
		else if (const FStrProperty* SP = CastField<FStrProperty>(Prop))
		{
			SP->SetPropertyValue(ValuePtr, Value->AsString());
		}
		else if (const FNameProperty* NP = CastField<FNameProperty>(Prop))
		{
			NP->SetPropertyValue(ValuePtr, FName(*Value->AsString()));
		}
		else if (const FObjectProperty* OP = CastField<FObjectProperty>(Prop))
		{
			const FString Path = Value->AsString();
			UObject* Asset = Path.IsEmpty() ? nullptr : StaticLoadObject(OP->PropertyClass, nullptr, *Path);
			if (!Path.IsEmpty() && !Asset)
			{
				OutError = FString::Printf(TEXT("Could not load asset '%s' for property '%s'"), *Path, *PropName);
				return false;
			}
			OP->SetObjectPropertyValue(ValuePtr, Asset);
		}
		else if (const FStructProperty* StP = CastField<FStructProperty>(Prop))
		{
			if (StP->Struct == TBaseStructure<FLinearColor>::Get())
			{
				FLinearColor Color;
				if (!ParseColor(Color)) { OutError = TEXT("Expected [r,g,b,a] or {r,g,b,a} for color property"); return false; }
				*static_cast<FLinearColor*>(ValuePtr) = Color;
			}
			else if (StP->Struct == TBaseStructure<FVector>::Get())
			{
				FLinearColor C; if (!ParseColor(C)) { OutError = TEXT("Expected [x,y,z] for vector property"); return false; }
				*static_cast<FVector*>(ValuePtr) = FVector(C.R, C.G, C.B);
			}
			else
			{
				OutError = FString::Printf(TEXT("Unsupported struct property type '%s'"), *StP->Struct->GetName());
				return false;
			}
		}
		else
		{
			OutError = FString::Printf(TEXT("Unsupported property type for '%s'"), *PropName);
			return false;
		}

		Expr->MarkPackageDirty();
		return true;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/describe_material
// Request: {"material": "/Game/Materials/M_Foo"}
// Data-flow view: root property connections + every expression node and its inputs.
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoDescribeMaterial(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("material"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'material' field is required and must be a non-empty string"));
	}

	UMaterialInterface* MatInterface = KismetAIUtils::FindMaterialInterface(AssetPath);
	if (!MatInterface)
	{
		// Not a material — maybe a Material Function (the asset behind a MaterialFunctionCall
		// node). Describe its inner graph so nested function logic isn't a black box.
		if (UMaterialFunction* Func = KismetAIUtils::FindMaterialFunction(AssetPath))
		{
			return FToolExecutionResult::Success(SerializeMaterialFunction(Func));
		}
		return FToolExecutionResult::Error(404, TEXT("MATERIAL_NOT_FOUND"), FString::Printf(TEXT("Material not found: %s"), *AssetPath),
			{ TEXT("Use search_assets with class_filter='Material' to find the correct path") });
	}

	UMaterial* Material = Cast<UMaterial>(MatInterface);
	if (!Material)
	{
		// Instances have no editable expression graph — point the agent at overview.
		return FToolExecutionResult::Error(422, TEXT("NOT_A_BASE_MATERIAL"),
			FString::Printf(TEXT("'%s' is a Material Instance, which has no node graph"), *AssetPath),
			{ TEXT("Use read_material mode='overview' to inspect instance parameters and parent") });
	}

	// If the material is open in a Material Editor, read its live preview so the agent sees the
	// same state it's been editing (the editor works on a transient duplicate of the asset).
	IMaterialEditor* OpenEditor = nullptr;
	UMaterial* Source = ResolveMaterialEditTarget(Material, OpenEditor);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material"), Material->GetPathName());
	Data->SetStringField(TEXT("type"), TEXT("material"));
	Data->SetBoolField(TEXT("editor_open"), OpenEditor != nullptr);

	// Root property connections (what feeds BaseColor, Roughness, etc.)
	TArray<TSharedPtr<FJsonValue>> RootConns;
	for (const FRootPropertyEntry& Entry : GMaterialRootProperties)
	{
		UMaterialExpression* FromNode = UMaterialEditingLibrary::GetMaterialPropertyInputNode(Source, Entry.Property);
		if (!FromNode) continue;
		TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
		Conn->SetStringField(TEXT("property"), Entry.Name);
		Conn->SetStringField(TEXT("from"), FromNode->GetName());
		Conn->SetStringField(TEXT("from_output"), UMaterialEditingLibrary::GetMaterialPropertyInputNodeOutputName(Source, Entry.Property));
		RootConns.Add(MakeShared<FJsonValueObject>(Conn));
	}
	Data->SetArrayField(TEXT("root_connections"), RootConns);

	// Every expression node with its inputs/connections.
	TArray<TSharedPtr<FJsonValue>> Exprs;
	for (UMaterialExpression* Expr : Source->GetExpressions())
	{
		if (!Expr) continue;
		Exprs.Add(MakeShared<FJsonValueObject>(SerializeMaterialExpression(Expr)));
	}
	Data->SetArrayField(TEXT("expressions"), Exprs);
	Data->SetNumberField(TEXT("expression_count"), Exprs.Num());

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoDescribeMaterial. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleDescribeMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_material"), TEXT("describe"), FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/material_overview
// Request: {"material": "/Game/Materials/M_Foo"}
// Settings + parameter inventory (works for base materials and instances).
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoMaterialOverview(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("material"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'material' field is required and must be a non-empty string"));
	}

	UMaterialInterface* MatInterface = KismetAIUtils::FindMaterialInterface(AssetPath);
	if (!MatInterface)
	{
		// Material Function: report its interface (inputs/outputs) without the full graph.
		if (UMaterialFunction* Func = KismetAIUtils::FindMaterialFunction(AssetPath))
		{
			TSharedPtr<FJsonObject> FullDesc = SerializeMaterialFunction(Func);
			TSharedPtr<FJsonObject> Summary = MakeShared<FJsonObject>();
			Summary->SetStringField(TEXT("material"), Func->GetPathName());
			Summary->SetStringField(TEXT("type"), TEXT("material_function"));
			FString Desc;
			if (FullDesc->TryGetStringField(TEXT("description"), Desc))
			{
				Summary->SetStringField(TEXT("description"), Desc);
			}
			Summary->SetArrayField(TEXT("function_inputs"), FullDesc->GetArrayField(TEXT("function_inputs")));
			Summary->SetArrayField(TEXT("function_outputs"), FullDesc->GetArrayField(TEXT("function_outputs")));
			Summary->SetNumberField(TEXT("expression_count"), FullDesc->GetNumberField(TEXT("expression_count")));
			return FToolExecutionResult::Success(Summary);
		}
		return FToolExecutionResult::Error(404, TEXT("MATERIAL_NOT_FOUND"), FString::Printf(TEXT("Material not found: %s"), *AssetPath),
			{ TEXT("Use search_assets with class_filter='Material' or 'MaterialInstanceConstant'") });
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material"), MatInterface->GetPathName());

	UMaterial* BaseMaterial = Cast<UMaterial>(MatInterface);
	UMaterialInstance* Instance = Cast<UMaterialInstance>(MatInterface);

	if (BaseMaterial)
	{
		// Reflect the open editor's live preview state when the material is being edited.
		IMaterialEditor* OpenEditor = nullptr;
		UMaterial* Source = ResolveMaterialEditTarget(BaseMaterial, OpenEditor);

		Data->SetStringField(TEXT("type"), TEXT("material"));
		Data->SetBoolField(TEXT("editor_open"), OpenEditor != nullptr);
		Data->SetStringField(TEXT("domain"), EnumToString<EMaterialDomain>(Source->MaterialDomain.GetValue()));
		Data->SetStringField(TEXT("blend_mode"), EnumToString<EBlendMode>(Source->GetBlendMode()));
		const FMaterialShadingModelField ShadingModels = Source->GetShadingModels();
		Data->SetStringField(TEXT("shading_model"), ShadingModels.IsValid()
			? EnumToString<EMaterialShadingModel>(ShadingModels.GetFirstShadingModel())
			: TEXT("None"));
		Data->SetBoolField(TEXT("two_sided"), Source->IsTwoSided());
		Data->SetBoolField(TEXT("shading_model_from_expression"), Source->IsShadingModelFromMaterialExpression());
		// Heuristic: a connected FrontMaterial root input means this is a Substrate material.
		Data->SetBoolField(TEXT("is_substrate"),
			UMaterialEditingLibrary::GetMaterialPropertyInputNode(Source, MP_FrontMaterial) != nullptr);
		Data->SetNumberField(TEXT("expression_count"), UMaterialEditingLibrary::GetNumMaterialExpressions(Source));
	}
	else if (Instance)
	{
		Data->SetStringField(TEXT("type"), TEXT("material_instance"));
		if (UMaterialInterface* Parent = Instance->Parent)
		{
			Data->SetStringField(TEXT("parent"), Parent->GetPathName());
		}
	}

	// Parameter inventory with current values (works on materials and instances alike).
	// Instances additionally report per-parameter override state and the parent's value,
	// so the agent can inspect current state before/after set_material_instance_parameter.
	UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(MatInterface);
	UMaterial* ParentMaterial = nullptr;
	UMaterialInstanceConstant* ParentMIC = nullptr;
	if (Instance && Instance->Parent)
	{
		ParentMaterial = Cast<UMaterial>(Instance->Parent);
		ParentMIC = Cast<UMaterialInstanceConstant>(Instance->Parent);
	}

	enum class EParamKind { Scalar, Vector, Texture, Switch };
	auto AddParams = [&](const TCHAR* Field, EParamKind Kind, void(*Getter)(UMaterialInterface*, TArray<FName>&))
	{
		TArray<FName> Names;
		Getter(MatInterface, Names);
		TArray<TSharedPtr<FJsonValue>> Arr;
		for (const FName& N : Names)
		{
			TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("name"), N.ToString());

			// Reads the effective value from a MIC or a base material's expression defaults.
			auto SetValueField = [&P, Kind, &N](const TCHAR* ValueField, UMaterialInstanceConstant* AsMIC, UMaterial* AsMaterial)
			{
				switch (Kind)
				{
				case EParamKind::Scalar:
					P->SetNumberField(ValueField, AsMIC
						? UMaterialEditingLibrary::GetMaterialInstanceScalarParameterValue(AsMIC, N)
						: UMaterialEditingLibrary::GetMaterialDefaultScalarParameterValue(AsMaterial, N));
					break;
				case EParamKind::Vector:
				{
					const FLinearColor C = AsMIC
						? UMaterialEditingLibrary::GetMaterialInstanceVectorParameterValue(AsMIC, N)
						: UMaterialEditingLibrary::GetMaterialDefaultVectorParameterValue(AsMaterial, N);
					TArray<TSharedPtr<FJsonValue>> Rgba;
					Rgba.Add(MakeShared<FJsonValueNumber>(C.R));
					Rgba.Add(MakeShared<FJsonValueNumber>(C.G));
					Rgba.Add(MakeShared<FJsonValueNumber>(C.B));
					Rgba.Add(MakeShared<FJsonValueNumber>(C.A));
					P->SetArrayField(ValueField, Rgba);
					break;
				}
				case EParamKind::Texture:
				{
					UTexture* Tex = AsMIC
						? UMaterialEditingLibrary::GetMaterialInstanceTextureParameterValue(AsMIC, N)
						: UMaterialEditingLibrary::GetMaterialDefaultTextureParameterValue(AsMaterial, N);
					if (Tex)
					{
						P->SetStringField(ValueField, Tex->GetPathName());
					}
					else
					{
						P->SetField(ValueField, MakeShared<FJsonValueNull>());
					}
					break;
				}
				case EParamKind::Switch:
					P->SetBoolField(ValueField, AsMIC
						? UMaterialEditingLibrary::GetMaterialInstanceStaticSwitchParameterValue(AsMIC, N)
						: UMaterialEditingLibrary::GetMaterialDefaultStaticSwitchParameterValue(AsMaterial, N));
					break;
				}
			};

			if (MIC)
			{
				SetValueField(TEXT("value"), MIC, nullptr);
			}
			else if (BaseMaterial)
			{
				SetValueField(TEXT("value"), nullptr, BaseMaterial);
			}

			if (Instance)
			{
				bool bOverridden = false;
				switch (Kind)
				{
				case EParamKind::Scalar:
					bOverridden = Instance->ScalarParameterValues.ContainsByPredicate(
						[&N](const FScalarParameterValue& V) { return V.ParameterInfo.Name == N; });
					break;
				case EParamKind::Vector:
					bOverridden = Instance->VectorParameterValues.ContainsByPredicate(
						[&N](const FVectorParameterValue& V) { return V.ParameterInfo.Name == N; });
					break;
				case EParamKind::Texture:
					bOverridden = Instance->TextureParameterValues.ContainsByPredicate(
						[&N](const FTextureParameterValue& V) { return V.ParameterInfo.Name == N; });
					break;
				case EParamKind::Switch:
				{
					const FStaticParameterSet StaticParams = Instance->GetStaticParameters();
					bOverridden = StaticParams.StaticSwitchParameters.ContainsByPredicate(
						[&N](const FStaticSwitchParameter& V) { return V.ParameterInfo.Name == N && V.bOverride; });
					break;
				}
				}
				P->SetBoolField(TEXT("overridden"), bOverridden);

				if (ParentMIC)
				{
					SetValueField(TEXT("parent_value"), ParentMIC, nullptr);
				}
				else if (ParentMaterial)
				{
					SetValueField(TEXT("parent_value"), nullptr, ParentMaterial);
				}
			}

			Arr.Add(MakeShared<FJsonValueObject>(P));
		}
		Data->SetArrayField(Field, Arr);
	};
	AddParams(TEXT("scalar_parameters"), EParamKind::Scalar, &UMaterialEditingLibrary::GetScalarParameterNames);
	AddParams(TEXT("vector_parameters"), EParamKind::Vector, &UMaterialEditingLibrary::GetVectorParameterNames);
	AddParams(TEXT("texture_parameters"), EParamKind::Texture, &UMaterialEditingLibrary::GetTextureParameterNames);
	AddParams(TEXT("static_switch_parameters"), EParamKind::Switch, &UMaterialEditingLibrary::GetStaticSwitchParameterNames);

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoMaterialOverview. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleMaterialOverview(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_material"), TEXT("overview"), FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/compile_material
// Request: {"material": "/Game/Materials/M_Foo"}
// Recompiles the material's shaders and reports errors + instruction/sampler stats.
// This is the verify step after authoring/editing a material.
// ─────────────────────────────────────────────────────────────────────────────

static FKismetAIMutationOutcome MaterialMutationEvidence(UMaterial* Original, UMaterial* Target,
	bool bDirtyBefore, bool bApplied, bool bCompileRequested)
{
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bDirtyBefore);
	Outcome.PackageName = Original->GetOutermost()->GetName();
	Outcome.bAppliedInMemory = bApplied;
	Outcome.SavePolicy = KismetAISavePolicy::NoAutosave;
	Outcome.SaveSkipReason = bApplied ? KismetAISaveSkipReason::NotAutosaved : KismetAISaveSkipReason::NothingApplied;
	if (bCompileRequested)
	{
		if (const FMaterialResource* Resource = GetCurrentMaterialResource(Target))
		{
			if (Resource->IsCompilationFinished())
			{
				Outcome.CompileErrorCount = Resource->GetCompileErrors().Num();
				if (Outcome.CompileErrorCount > 0)
					Outcome.CompileStatus = EKismetAICompileStatus::Failed;
				else if (Resource->GetGameThreadShaderMap())
					Outcome.CompileStatus = EKismetAICompileStatus::Succeeded;
			}
		}
	}
	return Outcome;
}

FToolExecutionResult FKismetAIModule::DoCompileMaterial(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("material"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'material' field is required and must be a non-empty string"));
	}

	UMaterialInterface* MatInterface = KismetAIUtils::FindMaterialInterface(AssetPath);
	if (!MatInterface)
	{
		return FToolExecutionResult::Error(404, TEXT("MATERIAL_NOT_FOUND"), FString::Printf(TEXT("Material not found: %s"), *AssetPath));
	}

	UMaterial* Material = Cast<UMaterial>(MatInterface);
	if (!Material)
	{
		return FToolExecutionResult::Error(422, TEXT("NOT_A_BASE_MATERIAL"),
			FString::Printf(TEXT("'%s' is a Material Instance; it has no shaders of its own to compile"), *AssetPath),
			{ TEXT("Instances inherit their parent's shaders — edit parameters instead") });
	}

	// Target the open editor's live preview when present, so compiling reflects (and refreshes)
	// what the agent has been editing.
	IMaterialEditor* OpenEditor = nullptr;
	UMaterial* Target = ResolveMaterialEditTarget(Material, OpenEditor);
	if (!IsValid(Target))
	{
		// Recompiling a garbage material crashes the editor. The finder already refuses one;
		// this covers an open editor whose preview copy is being torn down.
		return FToolExecutionResult::Error(404, TEXT("MATERIAL_NOT_FOUND"),
			FString::Printf(TEXT("Material not found: %s (it is being deleted)"), *AssetPath));
	}

	const bool bDirtyBefore = KismetAIPersistence::IsPackageDirty(Material);
	// Recompile shaders (PreEditChange + PostEditChange under a FMaterialUpdateContext).
	UMaterialEditingLibrary::RecompileMaterial(Target);
	SyncOpenMaterialEditor(OpenEditor, Target);

	// GetStatistics finishes the shader jobs; sample evidence only after that existing wait.
	const FMaterialStatistics Stats = UMaterialEditingLibrary::GetStatistics(Target);
	const FKismetAIMutationOutcome Outcome = MaterialMutationEvidence(Material, Target, bDirtyBefore, false, true);

	// Collect shader compile errors from the material resource for the running platform.
	TArray<TSharedPtr<FJsonValue>> ErrorsArray;
	if (const FMaterialResource* Resource = GetCurrentMaterialResource(Target))
	{
		for (const FString& Err : Resource->GetCompileErrors())
		{
			ErrorsArray.Add(MakeShared<FJsonValueString>(Err));
		}
	}

	const bool bSuccess = Outcome.CompileStatus == EKismetAICompileStatus::Succeeded;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material"), Material->GetPathName());
	Data->SetBoolField(TEXT("success"), bSuccess);
	Data->SetStringField(TEXT("status"), bSuccess ? TEXT("compiled") : Outcome.CompileStatus == EKismetAICompileStatus::Failed ? TEXT("error") : TEXT("unverified"));
	Data->SetNumberField(TEXT("errors"), ErrorsArray.Num());
	Data->SetArrayField(TEXT("messages"), ErrorsArray);
	AttachMutationOutcome(Data, Outcome);


	// Instruction / sampler statistics. These require compiled shaders, so they may read 0
	// in a headless (-NullRHI) run — surface stats_available so the agent doesn't misread that.
	const bool bStatsAvailable = (Stats.NumPixelShaderInstructions > 0) || (Stats.NumSamplers > 0);
	TSharedPtr<FJsonObject> StatsObj = MakeShared<FJsonObject>();
	StatsObj->SetBoolField(TEXT("available"), bStatsAvailable);
	StatsObj->SetNumberField(TEXT("vertex_shader_instructions"), Stats.NumVertexShaderInstructions);
	StatsObj->SetNumberField(TEXT("pixel_shader_instructions"), Stats.NumPixelShaderInstructions);
	StatsObj->SetNumberField(TEXT("samplers"), Stats.NumSamplers);
	StatsObj->SetNumberField(TEXT("texture_samples_pixel"), Stats.NumPixelTextureSamples);
	StatsObj->SetNumberField(TEXT("texture_samples_vertex"), Stats.NumVertexTextureSamples);
	StatsObj->SetNumberField(TEXT("virtual_texture_samples"), Stats.NumVirtualTextureSamples);
	Data->SetObjectField(TEXT("statistics"), StatsObj);

	// Budget hint: the hard sampler limit is 16.
	if (bStatsAvailable && Stats.NumSamplers > 16)
	{
		Data->SetStringField(TEXT("warning"),
			FString::Printf(TEXT("Uses %d samplers (limit is 16) — share samplers (Shared:Wrap) or pack textures"), Stats.NumSamplers));
	}

	return FToolExecutionResult::Success(Data, bSuccess);
}

// HTTP adapter over DoCompileMaterial. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleCompileMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("compile_material"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/create_material_expression
// Request: {"material": "...", "node": "Multiply", "pos_x": -300, "pos_y": 0,
//           "parameter_name": "Tint"}
// Spawns one UMaterialExpression and returns its id + real pins.
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoCreateMaterialExpression(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath, NodeName;
	if (!Body->TryGetStringField(TEXT("material"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'material' is required"));
	}
	if (!Body->TryGetStringField(TEXT("node"), NodeName) || NodeName.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'node' (expression type, e.g. \"Multiply\") is required"));
	}

	UMaterial* Material = Cast<UMaterial>(KismetAIUtils::FindMaterialInterface(AssetPath));
	if (!Material)
	{
		return FToolExecutionResult::Error(404, TEXT("MATERIAL_NOT_FOUND"),
			FString::Printf(TEXT("Base material not found (instances cannot hold expressions): %s"), *AssetPath));
	}

	TArray<FString> Candidates;
	UClass* ExprClass = KismetAIUtils::ResolveMaterialExpressionClass(NodeName, &Candidates);
	if (!ExprClass)
	{
		TArray<FString> Suggestions;
		if (Candidates.Num() > 0)
		{
			Suggestions.Add(FString::Printf(TEXT("Did you mean: %s"), *FString::Join(Candidates, TEXT(", "))));
		}
		Suggestions.Add(TEXT("Use the friendly node name, e.g. Multiply, Add, Lerp, TextureSample, ScalarParameter, Constant3Vector"));
		return FToolExecutionResult::Error(422, TEXT("UNKNOWN_NODE_TYPE"),
			FString::Printf(TEXT("Could not resolve expression type '%s'"), *NodeName), Suggestions);
	}

	int32 PosX = 0, PosY = 0;
	Body->TryGetNumberField(TEXT("pos_x"), PosX);
	Body->TryGetNumberField(TEXT("pos_y"), PosY);

	// When the material is open in an editor, create the node on its live preview so it appears
	// immediately; otherwise create on the asset itself.
	UMaterial* Original = Material;
	const bool bDirtyBefore = KismetAIPersistence::IsPackageDirty(Original);
	IMaterialEditor* OpenEditor = nullptr;
	Material = ResolveMaterialEditTarget(Material, OpenEditor);

	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "CreateMaterialExpression", "Create Material Expression"));
	Material->Modify();

	UMaterialExpression* Expr = UMaterialEditingLibrary::CreateMaterialExpression(Material, ExprClass, PosX, PosY);
	if (!Expr)
	{
		Transaction.Cancel();
		return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"), FString::Printf(TEXT("Failed to create '%s'"), *NodeName));
	}

	// Optional parameter name for parameter expressions.
	FString ParamName;
	if (Body->TryGetStringField(TEXT("parameter_name"), ParamName) && !ParamName.IsEmpty())
	{
		if (UMaterialExpressionParameter* Param = Cast<UMaterialExpressionParameter>(Expr))
		{
			Param->ParameterName = FName(*ParamName);
			Param->MarkPackageDirty();
		}
	}

	SyncOpenMaterialEditor(OpenEditor, Material);

	TSharedPtr<FJsonObject> Data = SerializeMaterialExpression(Expr);
	AttachMutationOutcome(Data, MaterialMutationEvidence(Original, Material, bDirtyBefore, true, false));
	Data->SetStringField(TEXT("note"), TEXT("Node created. Use edit_material to set property values and wire connections by this node's id, then compile_material to verify."));
	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoCreateMaterialExpression. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleCreateMaterialExpression(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("create_material_expression"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/edit_material
// Request: {"material": "...", "atomic": false, "operations": [
//   {"action":"set_property","node":"...","property":"DefaultValue","value":0.5},
//   {"action":"connect","from_node":"A","from_output":"","to_node":"B","to_input":"A"},
//   {"action":"connect","from_node":"B","to_property":"BaseColor"},
//   {"action":"disconnect","to_node":"B","to_input":"A"},
//   {"action":"delete_node","node":"..."} ]}
// Batches graph edits, then recompiles to verify.
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoEditMaterial(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("material"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'material' is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Operations;
	if (!Body->TryGetArrayField(TEXT("operations"), Operations) || Operations->Num() == 0)
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'operations' must be a non-empty array"));
	}

	UMaterial* Original = Cast<UMaterial>(KismetAIUtils::FindMaterialInterface(AssetPath));
	if (!Original)
	{
		return FToolExecutionResult::Error(404, TEXT("MATERIAL_NOT_FOUND"),
			FString::Printf(TEXT("Base material not found (instances are edited via parameters): %s"), *AssetPath));
	}

	// When the material is open in a Material Editor, edit its live preview duplicate so changes
	// show instantly and aren't clobbered on save; otherwise edit the asset directly.
	IMaterialEditor* OpenEditor = nullptr;
	UMaterial* Material = ResolveMaterialEditTarget(Original, OpenEditor);

	const bool bAtomic = Body->HasField(TEXT("atomic")) && Body->GetBoolField(TEXT("atomic"));

	// Atomic mode: snapshot for verification, then record the material, its
	// editor-only data (expression collection + property inputs) and every
	// expression before the first operation. UMaterialEditingLibrary's
	// connect/disconnect/delete and the raw property writes below never call
	// Modify() on the expressions they change, so without this the transaction
	// record would not hold their pre-batch state.
	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Material);
	const bool bEvidenceDirtyBefore = bPackageWasDirtyBefore || KismetAIPersistence::IsPackageDirty(Original);
	FKismetAIStateSnapshot AtomicBaseline;
	if (bAtomic)
	{
		AtomicBaseline = KismetAIBatchRecovery::CaptureMaterial(Material);
	}
	FKismetAIAtomicBatchScope Transaction(NSLOCTEXT("KismetAI", "EditMaterial", "Edit Material"));
	if (bAtomic && !Transaction.CanRestore())
	{
		return AtomicBatchUnavailableError();
	}
	Material->Modify();
	if (bAtomic)
	{
		Transaction.Track(Material);
		Transaction.Track(Material->GetEditorOnlyData());
		for (UMaterialExpression* Expression : Material->GetExpressions())
		{
			Transaction.Track(Expression);
		}
	}

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 FailureCount = 0;

	for (int32 OpIndex = 0; OpIndex < Operations->Num(); ++OpIndex)
	{
		const TSharedPtr<FJsonObject> Op = (*Operations)[OpIndex]->AsObject();
		TSharedPtr<FJsonObject> Res = MakeShared<FJsonObject>();
		Res->SetNumberField(TEXT("index"), OpIndex);

		auto Fail = [&](const FString& Msg)
		{
			Res->SetBoolField(TEXT("success"), false);
			Res->SetStringField(TEXT("error"), Msg);
			++FailureCount;
		};

		if (!Op.IsValid()) { Fail(TEXT("operation is not an object")); Results.Add(MakeShared<FJsonValueObject>(Res)); continue; }

		const FString Action = Op->GetStringField(TEXT("action"));
		Res->SetStringField(TEXT("action"), Action);

		if (Action == TEXT("connect"))
		{
			const FString FromNode = Op->GetStringField(TEXT("from_node"));
			const FString FromOutput = Op->HasField(TEXT("from_output")) ? Op->GetStringField(TEXT("from_output")) : FString();
			UMaterialExpression* FromExpr = FindExpressionByName(Material, FromNode);
			if (!FromExpr) { Fail(FString::Printf(TEXT("from_node '%s' not found"), *FromNode)); }
			else if (Op->HasField(TEXT("to_property")))
			{
				const FString PropName = Op->GetStringField(TEXT("to_property"));
				EMaterialProperty Prop;
				if (!MaterialPropertyFromName(PropName, Prop)) { Fail(FString::Printf(TEXT("unknown material property '%s'"), *PropName)); }
				else if (!UMaterialEditingLibrary::ConnectMaterialProperty(FromExpr, FromOutput, Prop))
				{
					Fail(FString::Printf(TEXT("could not connect to property '%s' (check from_output name)"), *PropName));
				}
				else { Res->SetBoolField(TEXT("success"), true); }
			}
			else
			{
				const FString ToNode = Op->GetStringField(TEXT("to_node"));
				const FString ToInput = Op->HasField(TEXT("to_input")) ? Op->GetStringField(TEXT("to_input")) : FString();
				UMaterialExpression* ToExpr = FindExpressionByName(Material, ToNode);
				if (!ToExpr) { Fail(FString::Printf(TEXT("to_node '%s' not found"), *ToNode)); }
				else if (!UMaterialEditingLibrary::ConnectMaterialExpressions(FromExpr, FromOutput, ToExpr, ToInput))
				{
					Fail(FString::Printf(
						TEXT("connection rejected (to_input='%s'). Valid inputs on '%s': %s. Verify from_output and value types too."),
						*ToInput, *ToNode, *DescribeExpressionInputs(ToExpr)));
				}
				else { Res->SetBoolField(TEXT("success"), true); }
			}
		}
		else if (Action == TEXT("disconnect"))
		{
			if (Op->HasField(TEXT("to_property")))
			{
				const FString PropName = Op->GetStringField(TEXT("to_property"));
				EMaterialProperty Prop;
				if (!MaterialPropertyFromName(PropName, Prop)) { Fail(FString::Printf(TEXT("unknown material property '%s'"), *PropName)); }
				else if (FExpressionInput* In = Material->GetExpressionInputForProperty(Prop))
				{
					In->Expression = nullptr; In->OutputIndex = 0;
					Res->SetBoolField(TEXT("success"), true);
				}
				else { Fail(FString::Printf(TEXT("property '%s' has no input on this material"), *PropName)); }
			}
			else
			{
				const FString ToNode = Op->GetStringField(TEXT("to_node"));
				const FString ToInput = Op->HasField(TEXT("to_input")) ? Op->GetStringField(TEXT("to_input")) : FString();
				UMaterialExpression* ToExpr = FindExpressionByName(Material, ToNode);
				if (!ToExpr) { Fail(FString::Printf(TEXT("to_node '%s' not found"), *ToNode)); }
				else
				{
					int32 Idx; FExpressionInput* In = FindExpressionInputByName(ToExpr, ToInput, Idx);
					if (!In) { Fail(FString::Printf(TEXT("input '%s' not found on '%s'. Valid inputs: %s"), *ToInput, *ToNode, *DescribeExpressionInputs(ToExpr))); }
					else { In->Expression = nullptr; In->OutputIndex = 0; Res->SetBoolField(TEXT("success"), true); }
				}
			}
		}
		else if (Action == TEXT("set_property"))
		{
			const FString NodeName = Op->GetStringField(TEXT("node"));
			const FString PropName = Op->GetStringField(TEXT("property"));
			UMaterialExpression* Expr = FindExpressionByName(Material, NodeName);
			if (!Expr) { Fail(FString::Printf(TEXT("node '%s' not found"), *NodeName)); }
			else if (!Op->HasField(TEXT("value"))) { Fail(TEXT("'value' is required for set_property")); }
			else
			{
				FString Err;
				if (SetExpressionProperty(Expr, PropName, Op->Values.FindRef(TEXT("value")), Err))
				{
					Res->SetBoolField(TEXT("success"), true);
				}
				else { Fail(Err); }
			}
		}
		else if (Action == TEXT("delete_node"))
		{
			const FString NodeName = Op->GetStringField(TEXT("node"));
			UMaterialExpression* Expr = FindExpressionByName(Material, NodeName);
			if (!Expr) { Fail(FString::Printf(TEXT("node '%s' not found"), *NodeName)); }
			else { UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expr); Res->SetBoolField(TEXT("success"), true); }
		}
		else
		{
			Fail(FString::Printf(TEXT("unknown action '%s'"), *Action));
		}

		Results.Add(MakeShared<FJsonValueObject>(Res));
	}

	if (bAtomic && FailureCount > 0)
	{
		// Revert this batch's own transaction record and verify the material is
		// back at its pre-batch snapshot (see KismetAIBatchRecovery.h).
		const FAtomicRestoreReport Restore = RestoreFailedAtomicBatch(Transaction, AtomicBaseline,
			[Material]() { return KismetAIBatchRecovery::CaptureMaterial(Material); },
			Material->GetOutermost(), bPackageWasDirtyBefore);
		const bool bRestored = Restore.Status == EKismetAIRecoveryStatus::Restored;
		if (OpenEditor)
		{
			// The open editor's graph mirrors the preview material; rebuild it from
			// whatever state the material is in now. Not marked dirty: nothing changed.
			if (UMaterialGraph* MaterialGraph = Material->MaterialGraph)
			{
				MaterialGraph->RebuildGraph();
			}
			OpenEditor->ForceRefreshExpressionPreviews();
			OpenEditor->UpdateDetailView();
		}

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		AttachMutationOutcome(Data, MakeAtomicRollbackOutcome(Restore, Original->GetOutermost()->GetName(),
			bPackageWasDirtyBefore, KismetAISavePolicy::NoAutosave));
		Data->SetStringField(TEXT("material"), Original->GetPathName());
		Data->SetBoolField(TEXT("applied"), !bRestored);
		Data->SetStringField(TEXT("status"), bRestored ? TEXT("rolled_back") : TEXT("rollback_incomplete"));
		Data->SetNumberField(TEXT("failures"), FailureCount);
		AddAtomicRollbackFields(Data, Restore);
		Data->SetArrayField(TEXT("operations"), Results);
		Data->SetStringField(TEXT("message"), DescribeAtomicRollback(Restore, FailureCount, TEXT("material")));
		return FToolExecutionResult::Success(Data, false);
	}

	// Recompile to apply + verify the edits, then push them into the open editor's view (if any).
	UMaterialEditingLibrary::RecompileMaterial(Material);
	SyncOpenMaterialEditor(OpenEditor, Material);

	TArray<TSharedPtr<FJsonValue>> CompileErrors;
	if (const FMaterialResource* Resource = GetCurrentMaterialResource(Material))
	{
		for (const FString& Err : Resource->GetCompileErrors())
		{
			CompileErrors.Add(MakeShared<FJsonValueString>(Err));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material"), Original->GetPathName());
	const FKismetAIMutationOutcome Outcome = MaterialMutationEvidence(Original, Material, bEvidenceDirtyBefore, FailureCount < Operations->Num(), true);
	AttachMutationOutcome(Data, Outcome);
	Data->SetBoolField(TEXT("applied"), Outcome.bAppliedInMemory);
	Data->SetNumberField(TEXT("failures"), FailureCount);
	Data->SetArrayField(TEXT("operations"), Results);
	Data->SetBoolField(TEXT("compiled"), Outcome.CompileStatus == EKismetAICompileStatus::Succeeded);
	Data->SetArrayField(TEXT("compile_errors"), CompileErrors);
	return FToolExecutionResult::Success(Data, FailureCount == 0);
}

// HTTP adapter over DoEditMaterial. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleEditMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("edit_material"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/create_material_instance
// Request: {"name": "MI_Rock_Red", "path": "/Game/Materials", "parent": "/Game/Materials/M_Rock"}
// Creates a Material Instance Constant — the preferred way to make a material variant.
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoCreateMaterialInstance(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString Name, Path, ParentPath;
	if (!Body->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'name' is required"));
	}
	if (!Body->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'path' (destination folder, e.g. /Game/Materials) is required"));
	}
	if (!Body->TryGetStringField(TEXT("parent"), ParentPath) || ParentPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'parent' (the material/instance to inherit from) is required"));
	}

	if (!Path.StartsWith(TEXT("/Game/")) && Path != TEXT("/Game"))
	{
		return FToolExecutionResult::Error(403, TEXT("OUT_OF_SCOPE"),
			FString::Printf(TEXT("path must be inside /Game/ (got '%s')"), *Path),
			{ TEXT("New assets can only be created under /Game/") });
	}
	if (Path.EndsWith(TEXT("/"))) Path = Path.LeftChop(1);

	UMaterialInterface* Parent = KismetAIUtils::FindMaterialInterface(ParentPath);
	if (!Parent)
	{
		return FToolExecutionResult::Error(404, TEXT("PARENT_NOT_FOUND"),
			FString::Printf(TEXT("Parent material not found: %s"), *ParentPath),
			{ TEXT("Use search_assets to find the parent material's path") });
	}

	const FString FullAssetPath = Path / Name;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> ExistingAssets;
	AssetRegistry.GetAssetsByPackageName(FName(*FullAssetPath), ExistingAssets, true);
	if (ExistingAssets.Num() > 0)
	{
		return FToolExecutionResult::Error(409, TEXT("ASSET_ALREADY_EXISTS"),
			FString::Printf(TEXT("Asset already exists at '%s'"), *FullAssetPath),
			{ TEXT("Use a different name or path") });
	}

	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "CreateMaterialInstance", "Create Material Instance"));

	UMaterialInstanceConstantFactoryNew* Factory = NewObject<UMaterialInstanceConstantFactoryNew>();
	Factory->InitialParent = Parent;

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* Created = AssetTools.CreateAsset(Name, Path, UMaterialInstanceConstant::StaticClass(), Factory);
	if (!Created)
	{
		Transaction.Cancel();
		return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"), TEXT("Failed to create Material Instance"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Created->GetPathName());
	Data->SetStringField(TEXT("type"), TEXT("material_instance"));
	Data->SetStringField(TEXT("parent"), Parent->GetPathName());
	Data->SetStringField(TEXT("message"),
		TEXT("Created Material Instance. Use set_material_instance_parameter to override its parameters (read_material mode='overview' on the parent lists available parameter names)."));
	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoCreateMaterialInstance. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleCreateMaterialInstance(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("create_material_instance"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/set_material_instance_parameter
// Request: {"instance": "...", "parameters": [
//   {"type":"scalar","name":"Roughness","value":0.3},
//   {"type":"vector","name":"Tint","value":[1,0,0,1]},
//   {"type":"texture","name":"BaseTex","value":"/Game/Textures/T_Rock"},
//   {"type":"switch","name":"UseDetail","value":true} ]}
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoSetMaterialInstanceParameter(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString InstancePath;
	if (!Body->TryGetStringField(TEXT("instance"), InstancePath) || InstancePath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'instance' is required"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Params;
	if (!Body->TryGetArrayField(TEXT("parameters"), Params) || Params->Num() == 0)
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'parameters' must be a non-empty array"));
	}

	UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(KismetAIUtils::FindMaterialInterface(InstancePath));
	if (!MIC)
	{
		return FToolExecutionResult::Error(404, TEXT("INSTANCE_NOT_FOUND"),
			FString::Printf(TEXT("Material Instance Constant not found: %s"), *InstancePath),
			{ TEXT("Only Material Instance Constants (.uasset instances) can be edited this way") });
	}

	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "SetMaterialInstanceParameter", "Set Material Instance Parameter"));
	MIC->Modify();

	auto ParseColor = [](const TSharedPtr<FJsonValue>& Value, FLinearColor& Out) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr;
		if (Value.IsValid() && Value->TryGetArray(Arr) && Arr->Num() >= 3)
		{
			Out.R = (*Arr)[0]->AsNumber(); Out.G = (*Arr)[1]->AsNumber(); Out.B = (*Arr)[2]->AsNumber();
			Out.A = Arr->Num() >= 4 ? (*Arr)[3]->AsNumber() : 1.0f;
			return true;
		}
		return false;
	};

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 FailureCount = 0;

	// Echo the received entry back in error messages so a malformed call is fixable in one retry.
	auto EntryToString = [](const TSharedPtr<FJsonValue>& Entry) -> FString
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Entry, FString(), Writer);
		return Out;
	};
	const TCHAR* ExpectedShape = TEXT("each parameters[] entry must be a flat object {\"type\":\"scalar|vector|texture|switch\",\"name\":\"<param>\",\"value\":<v>}");

	for (int32 i = 0; i < Params->Num(); ++i)
	{
		const TSharedPtr<FJsonObject> P = (*Params)[i]->AsObject();
		TSharedPtr<FJsonObject> Res = MakeShared<FJsonObject>();
		Res->SetNumberField(TEXT("index"), i);

		auto Fail = [&](const FString& Msg) { Res->SetBoolField(TEXT("success"), false); Res->SetStringField(TEXT("error"), Msg); ++FailureCount; };

		if (!P.IsValid())
		{
			Fail(FString::Printf(TEXT("entry is not an object — received %s; %s"), *EntryToString((*Params)[i]), ExpectedShape));
			Results.Add(MakeShared<FJsonValueObject>(Res));
			continue;
		}

		const FString Type = P->GetStringField(TEXT("type"));
		const FString ParamName = P->GetStringField(TEXT("name"));
		Res->SetStringField(TEXT("name"), ParamName);
		const TSharedPtr<FJsonValue> Value = P->Values.FindRef(TEXT("value"));

		if (ParamName.IsEmpty() || !Value.IsValid())
		{
			// Common failure: the real {type,name,value} object wrapped one level too deep.
			FString WrapperKey;
			for (const auto& Pair : P->Values)
			{
				const TSharedPtr<FJsonObject>* Inner;
				if (Pair.Value.IsValid() && Pair.Value->TryGetObject(Inner)
					&& ((*Inner)->HasField(TEXT("name")) || (*Inner)->HasField(TEXT("value"))))
				{
					WrapperKey = KismetJsonKey(Pair.Key);
					break;
				}
			}
			if (!WrapperKey.IsEmpty())
			{
				Fail(FString::Printf(TEXT("entry nests the parameter under '%s' — remove the wrapper; received %s; %s"),
					*WrapperKey, *EntryToString((*Params)[i]), ExpectedShape));
			}
			else if (ParamName.IsEmpty())
			{
				Fail(FString::Printf(TEXT("'name' is required — received %s; %s"), *EntryToString((*Params)[i]), ExpectedShape));
			}
			else
			{
				Fail(FString::Printf(TEXT("'value' is required — received %s; %s"), *EntryToString((*Params)[i]), ExpectedShape));
			}
		}
		else if (Type == TEXT("scalar"))
		{
			UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, *ParamName, Value->AsNumber());
			Res->SetBoolField(TEXT("success"), true);
		}
		else if (Type == TEXT("vector"))
		{
			FLinearColor Color;
			if (!ParseColor(Value, Color)) { Fail(TEXT("vector value must be [r,g,b] or [r,g,b,a]")); }
			else { UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, *ParamName, Color); Res->SetBoolField(TEXT("success"), true); }
		}
		else if (Type == TEXT("texture"))
		{
			const FString TexPath = Value->AsString();
			UTexture* Tex = TexPath.IsEmpty() ? nullptr : Cast<UTexture>(StaticLoadObject(UTexture::StaticClass(), nullptr, *TexPath));
			if (!Tex) { Fail(FString::Printf(TEXT("could not load texture '%s'"), *TexPath)); }
			else { UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, *ParamName, Tex); Res->SetBoolField(TEXT("success"), true); }
		}
		else if (Type == TEXT("switch"))
		{
			UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MIC, *ParamName, Value->AsBool());
			Res->SetBoolField(TEXT("success"), true);
		}
		else
		{
			Fail(FString::Printf(TEXT("unknown parameter type '%s' (expected scalar, vector, texture, switch)"), *Type));
		}

		Results.Add(MakeShared<FJsonValueObject>(Res));
	}

	UMaterialEditingLibrary::UpdateMaterialInstance(MIC);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("instance"), MIC->GetPathName());
	Data->SetNumberField(TEXT("failures"), FailureCount);
	Data->SetArrayField(TEXT("parameters"), Results);
	return FToolExecutionResult::Success(Data, FailureCount == 0);
}

// HTTP adapter over DoSetMaterialInstanceParameter. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleSetMaterialInstanceParameter(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("set_material_instance_parameter"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/manage_material
// Request: {"material": "...", "blend_mode": "Translucent", "shading_model": "Unlit",
//           "domain": "Surface", "two_sided": true}
// Edits a base material's shading settings, then recompiles. Only provided fields change.
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoManageMaterial(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("material"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'material' is required"));
	}

	UMaterial* Original = Cast<UMaterial>(KismetAIUtils::FindMaterialInterface(AssetPath));
	if (!Original)
	{
		return FToolExecutionResult::Error(404, TEXT("MATERIAL_NOT_FOUND"),
			FString::Printf(TEXT("Base material not found (instances inherit settings): %s"), *AssetPath));
	}

	// Apply to the open editor's live preview when present, so settings changes show instantly.
	IMaterialEditor* OpenEditor = nullptr;
	UMaterial* Material = ResolveMaterialEditTarget(Original, OpenEditor);

	const bool bDirtyBefore = KismetAIPersistence::IsPackageDirty(Original) || KismetAIPersistence::IsPackageDirty(Material);
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "ManageMaterial", "Edit Material Settings"));
	Material->Modify();

	TArray<TSharedPtr<FJsonValue>> Applied;
	TArray<TSharedPtr<FJsonValue>> Rejected;
	auto NoteApplied = [&](const FString& Field, const FString& Val)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("field"), Field); O->SetStringField(TEXT("value"), Val);
		Applied.Add(MakeShared<FJsonValueObject>(O));
	};
	auto NoteRejected = [&](const FString& Field, const FString& Msg)
	{
		TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("field"), Field); O->SetStringField(TEXT("error"), Msg);
		Rejected.Add(MakeShared<FJsonValueObject>(O));
	};

	FString StrVal;
	if (Body->TryGetStringField(TEXT("blend_mode"), StrVal))
	{
		EBlendMode Mode;
		if (EnumFromString<EBlendMode>(StrVal, TEXT("BLEND_"), Mode)) { Material->BlendMode = Mode; NoteApplied(TEXT("blend_mode"), StrVal); }
		else NoteRejected(TEXT("blend_mode"), FString::Printf(TEXT("unknown blend mode '%s' (e.g. Opaque, Masked, Translucent, Additive, Modulate)"), *StrVal));
	}
	if (Body->TryGetStringField(TEXT("shading_model"), StrVal))
	{
		EMaterialShadingModel Model;
		if (EnumFromString<EMaterialShadingModel>(StrVal, TEXT("MSM_"), Model)) { Material->SetShadingModel(Model); NoteApplied(TEXT("shading_model"), StrVal); }
		else NoteRejected(TEXT("shading_model"), FString::Printf(TEXT("unknown shading model '%s' (e.g. DefaultLit, Unlit, Subsurface, ClearCoat)"), *StrVal));
	}
	if (Body->TryGetStringField(TEXT("domain"), StrVal))
	{
		EMaterialDomain Domain;
		if (EnumFromString<EMaterialDomain>(StrVal, TEXT("MD_"), Domain)) { Material->MaterialDomain = Domain; NoteApplied(TEXT("domain"), StrVal); }
		else NoteRejected(TEXT("domain"), FString::Printf(TEXT("unknown domain '%s' (e.g. Surface, DeferredDecal, LightFunction, PostProcess, UI)"), *StrVal));
	}
	bool bTwoSided;
	if (Body->TryGetBoolField(TEXT("two_sided"), bTwoSided))
	{
		Material->TwoSided = bTwoSided ? 1 : 0;
		NoteApplied(TEXT("two_sided"), bTwoSided ? TEXT("true") : TEXT("false"));
	}

	// Apply + recompile so the new settings take effect, then refresh the open editor's view.
	UMaterialEditingLibrary::RecompileMaterial(Material);
	SyncOpenMaterialEditor(OpenEditor, Material);

	const FMaterialShadingModelField ShadingModels = Material->GetShadingModels();
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("material"), Original->GetPathName());
	AttachMutationOutcome(Data, MaterialMutationEvidence(Original, Material, bDirtyBefore, Applied.Num() > 0, true));
	Data->SetArrayField(TEXT("applied"), Applied);
	Data->SetArrayField(TEXT("rejected"), Rejected);
	Data->SetStringField(TEXT("blend_mode"), EnumToString<EBlendMode>(Material->GetBlendMode()));
	Data->SetStringField(TEXT("shading_model"), ShadingModels.IsValid()
		? EnumToString<EMaterialShadingModel>(ShadingModels.GetFirstShadingModel()) : TEXT("None"));
	Data->SetStringField(TEXT("domain"), EnumToString<EMaterialDomain>(Material->MaterialDomain.GetValue()));
	Data->SetBoolField(TEXT("two_sided"), Material->IsTwoSided());
	return FToolExecutionResult::Success(Data, Rejected.Num() == 0);
}

// HTTP adapter over DoManageMaterial. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleManageMaterial(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("manage_material"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Animation Blueprint describe / overview helpers
// ─────────────────────────────────────────────────────────────────────────────

namespace AnimReadHelpers
{
	static FString Indent(int32 Depth)
	{
		FString S;
		for (int32 i = 0; i < Depth; ++i) S += TEXT("  ");
		return S;
	}

	// A short node title with the trailing "\n..." stripped to a single line.
	static FString NodeTitle(const UEdGraphNode* Node)
	{
		if (!Node) return TEXT("(null)");
		FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		Title.ReplaceInline(TEXT("\n"), TEXT(" "));
		Title.ReplaceInline(TEXT("\r"), TEXT(""));
		Title.TrimStartAndEndInline();
		if (Title.IsEmpty()) Title = Node->GetName();
		return Title;
	}

	// The pose input pins of an anim node, in declaration order.
	static void GetPoseInputPins(UEdGraphNode* Node, TArray<UEdGraphPin*>& Out)
	{
		if (!Node) return;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			if (UAnimationGraphSchema::IsPosePin(Pin->PinType))
			{
				Out.Add(Pin);
			}
		}
	}

	// Compact summary of a node's non-pose data inputs that are SET (default differs from
	// autogenerated) or LINKED. Keeps describe output quiet for untouched nodes.
	static FString DataInputSummary(UEdGraphNode* Node)
	{
		TArray<FString> Parts;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Input) continue;
			if (Pin->bHidden) continue;
			if (UAnimationGraphSchema::IsPosePin(Pin->PinType)) continue; // pose links rendered as tree children
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;

			if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0] && Pin->LinkedTo[0]->GetOwningNode())
			{
				Parts.Add(FString::Printf(TEXT("%s<-%s"), *Pin->PinName.ToString(),
					*NodeTitle(Pin->LinkedTo[0]->GetOwningNode())));
			}
			else if (!Pin->DefaultValue.IsEmpty() && Pin->DefaultValue != Pin->AutogeneratedDefaultValue)
			{
				Parts.Add(FString::Printf(TEXT("%s=%s"), *Pin->PinName.ToString(), *Pin->DefaultValue));
			}
			else if (Pin->DefaultObject)
			{
				Parts.Add(FString::Printf(TEXT("%s=%s"), *Pin->PinName.ToString(), *Pin->DefaultObject->GetName()));
			}
		}
		return Parts.Num() > 0 ? FString::Printf(TEXT(" (%s)"), *FString::Join(Parts, TEXT(", "))) : FString();
	}

	// The bound animation asset for an asset-player node (single sequence, blendspace, etc.), or null.
	static UAnimationAsset* GetBoundAsset(UEdGraphNode* Node)
	{
		if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
		{
			return AnimNode->GetAnimationAsset();
		}
		return nullptr;
	}

	// Sink node of a data-flow anim graph (Root / StateResult / TransitionResult).
	static UEdGraphNode* FindSinkNode(UEdGraph* Graph)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(Node))
			{
				if (AnimNode->IsSinkNode())
				{
					return Node;
				}
			}
		}
		return nullptr;
	}

	// Walk the pose tree backwards from a node through its pose-input links, rendering an indented
	// tree. CacheUsers cross-references SaveCachedPose/UseCachedPose pairs by CacheName.
	static void WalkPoseTree(UEdGraphNode* Node, int32 Depth, FString& Out, TSet<UEdGraphNode*>& Visited)
	{
		if (!Node) return;
		if (Visited.Contains(Node))
		{
			Out += FString::Printf(TEXT("%s%s (already shown)\n"), *Indent(Depth), *NodeTitle(Node));
			return;
		}
		Visited.Add(Node);

		FString Line = Indent(Depth) + NodeTitle(Node);
		if (UAnimationAsset* Asset = GetBoundAsset(Node))
		{
			Line += FString::Printf(TEXT(" [%s]"), *Asset->GetName());
		}
		Line += DataInputSummary(Node);
		Out += Line + TEXT("\n");

		// Follow pose inputs (the data-flow tree).
		TArray<UEdGraphPin*> PosePins;
		GetPoseInputPins(Node, PosePins);
		for (UEdGraphPin* Pin : PosePins)
		{
			if (Pin->LinkedTo.Num() == 0)
			{
				continue; // unconnected pose input — quiet
			}
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode())
				{
					// Label the pin when a node has multiple pose inputs (blends).
					if (PosePins.Num() > 1)
					{
						Out += FString::Printf(TEXT("%s[%s]\n"), *Indent(Depth + 1), *Pin->PinName.ToString());
						WalkPoseTree(Linked->GetOwningNode(), Depth + 2, Out, Visited);
					}
					else
					{
						WalkPoseTree(Linked->GetOwningNode(), Depth + 1, Out, Visited);
					}
				}
			}
		}
	}

	// One-line content summary for a state: its single asset player's asset, else the sink node's
	// pose source title, else "(empty)".
	static FString StateContentSummary(UEdGraph* StateGraph)
	{
		if (!StateGraph) return TEXT("(empty)");

		UEdGraphNode* Sink = FindSinkNode(StateGraph);
		if (!Sink) return TEXT("(no result)");

		// Trace the single pose link from the result.
		TArray<UEdGraphPin*> PosePins;
		GetPoseInputPins(Sink, PosePins);
		if (PosePins.Num() == 1 && PosePins[0]->LinkedTo.Num() == 1)
		{
			UEdGraphNode* Source = PosePins[0]->LinkedTo[0]->GetOwningNode();
			if (UAnimationAsset* Asset = GetBoundAsset(Source))
			{
				return FString::Printf(TEXT("%s [%s]"), *NodeTitle(Source), *Asset->GetName());
			}
			return NodeTitle(Source);
		}
		if (PosePins.Num() >= 1 && PosePins[0]->LinkedTo.Num() == 0)
		{
			return TEXT("(empty)");
		}
		return FString::Printf(TEXT("%d nodes"), StateGraph->Nodes.Num());
	}

	// One-line summary of a transition rule graph: the bool result source, or "automatic rule".
	static FString TransitionRuleSummary(UAnimStateTransitionNode* Trans)
	{
		if (!Trans) return FString();
		if (Trans->bAutomaticRuleBasedOnSequencePlayerInState)
		{
			return TEXT("automatic rule");
		}
		UEdGraph* RuleGraph = Trans->BoundGraph;
		if (!RuleGraph) return TEXT("(no rule)");

		// Find the TransitionResult sink; report its bool source.
		UEdGraphNode* Sink = FindSinkNode(RuleGraph);
		if (Sink)
		{
			for (UEdGraphPin* Pin : Sink->Pins)
			{
				if (Pin && Pin->Direction == EGPD_Input && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
				{
					if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0] && Pin->LinkedTo[0]->GetOwningNode())
					{
						return NodeTitle(Pin->LinkedTo[0]->GetOwningNode());
					}
					if (!Pin->DefaultValue.IsEmpty())
					{
						return FString::Printf(TEXT("bCanEnterTransition=%s"), *Pin->DefaultValue);
					}
				}
			}
		}
		return FString::Printf(TEXT("%d rule nodes"), RuleGraph->Nodes.Num());
	}

	// Properties of a transition worth surfacing, compact.
	static FString TransitionProps(UAnimStateTransitionNode* Trans)
	{
		if (!Trans) return FString();
		TArray<FString> Parts;
		Parts.Add(FString::Printf(TEXT("crossfade=%.2fs"), Trans->CrossfadeDuration));
		if (Trans->PriorityOrder != 0) Parts.Add(FString::Printf(TEXT("priority=%d"), Trans->PriorityOrder));
		if (Trans->Bidirectional) Parts.Add(TEXT("bidirectional"));
		if (Trans->bDisabled) Parts.Add(TEXT("disabled"));
		if (Trans->bAutomaticRuleBasedOnSequencePlayerInState) Parts.Add(TEXT("auto-rule"));
		return FString::Join(Parts, TEXT(", "));
	}

	// Endpoint-form address of a transition for display: "<SMPath>/From->To" with a "#N"
	// disambiguator (1-based, creation order) when the From->To pair is duplicated. SMPath may
	// be empty (then just "From->To[#N]"). AllTransitions is the SM's transitions in node order.
	static FString TransitionEndpointAddress(const FString& SMPath, UAnimStateTransitionNode* Trans,
		const TArray<UAnimStateTransitionNode*>& AllTransitions)
	{
		if (!Trans) return FString();
		UAnimStateNodeBase* From = Trans->GetPreviousState();
		UAnimStateNodeBase* To = Trans->GetNextState();
		const FString FromName = From ? From->GetStateName() : TEXT("?");
		const FString ToName = To ? To->GetStateName() : TEXT("?");

		// Ordinal among transitions sharing this exact From->To pair (1-based, creation order).
		int32 Ordinal = 0;
		int32 SameCount = 0;
		for (UAnimStateTransitionNode* T : AllTransitions)
		{
			if (!T) continue;
			UAnimStateNodeBase* P = T->GetPreviousState();
			UAnimStateNodeBase* N = T->GetNextState();
			const FString PName = P ? P->GetStateName() : TEXT("?");
			const FString NName = N ? N->GetStateName() : TEXT("?");
			if (PName == FromName && NName == ToName)
			{
				SameCount++;
				if (T == Trans) Ordinal = SameCount;
			}
		}
		FString Addr = FString::Printf(TEXT("%s->%s"), *FromName, *ToName);
		if (SameCount > 1)
		{
			Addr += FString::Printf(TEXT("#%d"), Ordinal);
		}
		return SMPath.IsEmpty() ? Addr : (SMPath + TEXT("/") + Addr);
	}

	// Render a state-machine graph: entry -> initial state, states, transitions, conduits, aliases.
	// SMPath (e.g. "AnimGraph/Locomotion") is prepended to the addressable rule-graph paths so the
	// agent can address transitions/states directly; empty when unknown.
	static void DescribeStateMachine(UAnimationStateMachineGraph* SM, FString& Out, const FString& SMPath = FString())
	{
		// Entry → initial state.
		FString InitialState = TEXT("(none)");
		if (SM->EntryNode)
		{
			if (UEdGraphNode* InitNode = SM->EntryNode->GetOutputNode())
			{
				InitialState = NodeTitle(InitNode);
			}
		}
		Out += FString::Printf(TEXT("Entry -> %s\n\n"), *InitialState);

		TArray<UAnimStateNode*> States;
		TArray<UAnimStateConduitNode*> Conduits;
		TArray<UAnimStateAliasNode*> Aliases;
		TArray<UAnimStateTransitionNode*> Transitions;
		for (UEdGraphNode* Node : SM->Nodes)
		{
			if (UAnimStateNode* S = Cast<UAnimStateNode>(Node)) States.Add(S);
			else if (UAnimStateConduitNode* C = Cast<UAnimStateConduitNode>(Node)) Conduits.Add(C);
			else if (UAnimStateAliasNode* A = Cast<UAnimStateAliasNode>(Node)) Aliases.Add(A);
			else if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node)) Transitions.Add(T);
		}

		Out += FString::Printf(TEXT("States (%d):\n"), States.Num());
		for (UAnimStateNode* S : States)
		{
			Out += FString::Printf(TEXT("- %s: %s"), *S->GetStateName(), *StateContentSummary(S->GetBoundGraph()));
			// When the bound graph's object name differs from the displayed state name
			// (editor-authored assets have opaque graph names), print the addressable path so
			// the agent can address the state's pose graph directly.
			if (UEdGraph* BG = S->GetBoundGraph())
			{
				if (BG->GetName() != S->GetStateName())
				{
					const FString Addr = SMPath.IsEmpty() ? S->GetStateName() : (SMPath + TEXT("/") + S->GetStateName());
					Out += FString::Printf(TEXT("  [address: %s]"), *Addr);
				}
			}
			Out += TEXT("\n");
		}
		Out += TEXT("\n");

		if (Conduits.Num() > 0)
		{
			Out += FString::Printf(TEXT("Conduits (%d):\n"), Conduits.Num());
			for (UAnimStateConduitNode* C : Conduits)
			{
				Out += FString::Printf(TEXT("- %s\n"), *C->GetStateName());
			}
			Out += TEXT("\n");
		}

		if (Aliases.Num() > 0)
		{
			Out += FString::Printf(TEXT("Aliases (%d):\n"), Aliases.Num());
			for (UAnimStateAliasNode* A : Aliases)
			{
				TArray<FString> AliasedNames;
				for (const TWeakObjectPtr<UAnimStateNodeBase>& W : A->GetAliasedStates())
				{
					if (UAnimStateNodeBase* St = W.Get())
					{
						AliasedNames.Add(St->GetStateName());
					}
				}
				const FString Target = A->bGlobalAlias ? TEXT("<all states>")
					: (AliasedNames.Num() > 0 ? FString::Join(AliasedNames, TEXT(", ")) : TEXT("(none)"));
				Out += FString::Printf(TEXT("- %s -> %s\n"), *A->GetStateName(), *Target);
			}
			Out += TEXT("\n");
		}

		Out += FString::Printf(TEXT("Transitions (%d):\n"), Transitions.Num());
		for (UAnimStateTransitionNode* T : Transitions)
		{
			UAnimStateNodeBase* From = T->GetPreviousState();
			UAnimStateNodeBase* To = T->GetNextState();
			const FString FromName = From ? From->GetStateName() : TEXT("?");
			const FString ToName = To ? To->GetStateName() : TEXT("?");
			const FString Arrow = T->Bidirectional ? TEXT("<->") : TEXT("->");
			FString Line = FString::Printf(TEXT("- %s %s %s"), *FromName, *Arrow, *ToName);
			const FString Props = TransitionProps(T);
			if (!Props.IsEmpty()) Line += FString::Printf(TEXT(" [%s]"), *Props);
			Line += FString::Printf(TEXT(" rule: %s"), *TransitionRuleSummary(T));
			// Addressable rule-graph path in endpoint form (object names are opaque in
			// editor-authored assets, so endpoint form is the stable address).
			const FString Addr = TransitionEndpointAddress(SMPath, T, Transitions);
			if (!Addr.IsEmpty()) Line += FString::Printf(TEXT(" [rule: %s]"), *Addr);
			if (T->CustomTransitionGraph)
			{
				Line += FString::Printf(TEXT(" [blend: %s@blend]"), *Addr);
			}
			Out += Line + TEXT("\n");
		}
	}
}

bool FKismetAIModule::DescribeAnimGraph(UBlueprint* Blueprint, UEdGraph* Graph, FString& OutDescription)
{
	using namespace AnimReadHelpers;
	if (!Graph) return false;

	// State machine graph → state/transition listing.
	if (UAnimationStateMachineGraph* SM = Cast<UAnimationStateMachineGraph>(Graph))
	{
		OutDescription += FString::Printf(TEXT("## %s / %s (state machine)\n\n"), *Blueprint->GetName(), *Graph->GetName());
		// Build the SM's qualified path so transitions/states render addressable paths.
		FString SMPath = Graph->GetName();
		if (UEdGraph* AnimRoot = KismetAIUtils::FindAnimGraph(Blueprint))
		{
			TMap<UEdGraph*, FString> All;
			KismetAIUtils::CollectGraphsRecursive(AnimRoot, FString(), All);
			if (const FString* P = All.Find(Graph)) SMPath = *P;
		}
		DescribeStateMachine(SM, OutDescription, SMPath);
		return true;
	}

	// Pose-tree graphs: the main anim graph, a state's bound graph, a transition's custom blend graph,
	// or a transition rule graph (bool sink). All are walked backwards from their sink node.
	const bool bIsPoseGraph =
		Cast<UAnimationGraph>(Graph) != nullptr ||
		Cast<UAnimationStateGraph>(Graph) != nullptr ||
		Cast<UAnimationTransitionGraph>(Graph) != nullptr;
	if (!bIsPoseGraph)
	{
		return false;
	}

	OutDescription += FString::Printf(TEXT("## %s / %s\n\n"), *Blueprint->GetName(), *Graph->GetName());

	UEdGraphNode* Sink = FindSinkNode(Graph);
	if (!Sink)
	{
		OutDescription += TEXT("(no result/sink node)\n");
	}
	else
	{
		OutDescription += TEXT("Pose tree (sink first, inputs indented):\n\n");
		TSet<UEdGraphNode*> Visited;
		WalkPoseTree(Sink, 0, OutDescription, Visited);
	}

	OutDescription += FString::Printf(TEXT("\nNodes (%d total)\n"), Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		OutDescription += FString::Printf(TEXT("%s: %s\n"), *Node->GetName(), *NodeTitle(Node));
	}
	return true;
}

void FKismetAIModule::AppendAnimOverview(UBlueprint* Blueprint, FString& Description)
{
	using namespace AnimReadHelpers;

	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Blueprint);
	if (!AnimBP)
	{
		return;
	}

	Description += TEXT("### Animation\n");
	if (AnimBP->TargetSkeleton)
	{
		Description += FString::Printf(TEXT("- Target Skeleton: %s\n"), *AnimBP->TargetSkeleton->GetPathName());
	}
	else
	{
		Description += TEXT("- Target Skeleton: None (template)\n");
	}

	UEdGraph* AnimGraph = KismetAIUtils::FindAnimGraph(Blueprint);
	if (!AnimGraph)
	{
		Description += TEXT("\n");
		return;
	}

	// State machines in the anim graph.
	TArray<UAnimGraphNode_StateMachineBase*> StateMachines;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (UAnimGraphNode_StateMachineBase* SM = Cast<UAnimGraphNode_StateMachineBase>(Node))
		{
			StateMachines.Add(SM);
		}
	}

	Description += FString::Printf(TEXT("- Anim Graph: %s (%d nodes, %d state machine(s))\n"),
		*AnimGraph->GetName(), AnimGraph->Nodes.Num(), StateMachines.Num());

	for (UAnimGraphNode_StateMachineBase* SMNode : StateMachines)
	{
		UAnimationStateMachineGraph* SM = SMNode->EditorStateMachineGraph;
		if (!SM) continue;

		const FString SMPath = FString::Printf(TEXT("%s/%s"), *AnimGraph->GetName(), *SM->GetName());
		Description += FString::Printf(TEXT("\n#### State Machine: %s (path \"%s\")\n"),
			*SM->GetName(), *SMPath);
		DescribeStateMachine(SM, Description, SMPath);
	}
	Description += TEXT("\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/describe_graph
// Request: {"blueprint": "/Game/BP_Player", "graph": "EventGraph"}
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoDescribeGraph(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	FString GraphName;
	Body->TryGetStringField(TEXT("graph"), GraphName);

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	TArray<FString> GraphCandidates;
	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName, &GraphCandidates);
	if (!Graph)
	{
		TArray<FString> Sugg;
		if (GraphCandidates.Num() > 0)
		{
			Sugg.Add(FString::Printf(TEXT("Ambiguous name — address one by path: %s"), *FString::Join(GraphCandidates, TEXT(", "))));
		}
		return FToolExecutionResult::Error(404, TEXT("GRAPH_NOT_FOUND"), FString::Printf(TEXT("Graph not found: %s"), *GraphName), Sugg);
	}

	// Animation Blueprint graphs are data-flow pose trees / state machines, not exec chains. Route
	// them to dedicated renderers (returns true when handled).
	{
		FString AnimDescription;
		if (DescribeAnimGraph(Blueprint, Graph, AnimDescription))
		{
			TSharedPtr<FJsonObject> AnimData = MakeShared<FJsonObject>();
			AnimData->SetStringField(TEXT("description"), AnimDescription);
			AnimData->SetStringField(TEXT("blueprint"), AssetPath);
			AnimData->SetStringField(TEXT("graph"), Graph->GetName());
			AnimData->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
			return FToolExecutionResult::Success(AnimData);
		}
	}

	// Walk execution chains from entry points to produce pseudo-code
	FString Description;
	Description += FString::Printf(TEXT("## %s / %s\n\n"), *Blueprint->GetName(), *Graph->GetName());

	// Find entry points (event nodes — nodes with exec output but no exec input, or K2Node_Event)
	TArray<UEdGraphNode*> EntryPoints;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;

		bool bIsEntryPoint = false;

		// Check if it's an event node (has exec output, no exec input connection)
		bool bHasExecOutput = false;
		bool bHasExecInput = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				if (Pin->Direction == EGPD_Output) bHasExecOutput = true;
				if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0) bHasExecInput = true;
			}
		}

		// Entry point: has exec output but no incoming exec connection
		if (bHasExecOutput && !bHasExecInput)
		{
			bIsEntryPoint = true;
		}

		if (bIsEntryPoint)
		{
			EntryPoints.Add(Node);
		}
	}

	// Walk each execution chain
	Description += TEXT("### Execution Flow\n\n");

	for (UEdGraphNode* EntryNode : EntryPoints)
	{
		// Recursive walk
		TSet<UEdGraphNode*> Visited;

		TFunction<void(UEdGraphNode*, int32)> WalkExecChain = [&](UEdGraphNode* CurrentNode, int32 Indent)
		{
			if (!CurrentNode || Visited.Contains(CurrentNode))
			{
				return;
			}
			Visited.Add(CurrentNode);

			// Indentation
			FString IndentStr;
			for (int32 i = 0; i < Indent; i++)
			{
				IndentStr += TEXT("  ");
			}

			// Node description
			FString NodeTitle = CurrentNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
			if (NodeTitle.IsEmpty())
			{
				NodeTitle = CurrentNode->GetName();
			}

			// Collect data pin info (non-exec inputs with default values or connections)
			FString DataInfo;
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin) continue;
				if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->Direction != EGPD_Input) continue;
				if (Pin->bHidden) continue;

				FString PinValue;
				if (Pin->LinkedTo.Num() > 0 && Pin->LinkedTo[0] && Pin->LinkedTo[0]->GetOwningNode())
				{
					PinValue = Pin->LinkedTo[0]->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString();
				}
				else if (!Pin->DefaultValue.IsEmpty())
				{
					PinValue = FString::Printf(TEXT("\"%s\""), *Pin->DefaultValue);
				}

				if (!PinValue.IsEmpty())
				{
					if (!DataInfo.IsEmpty()) DataInfo += TEXT(", ");
					DataInfo += FString::Printf(TEXT("%s=%s"), *Pin->PinName.ToString(), *PinValue);
				}
			}

			if (!DataInfo.IsEmpty())
			{
				Description += FString::Printf(TEXT("%s%s(%s)\n"), *IndentStr, *NodeTitle, *DataInfo);
			}
			else
			{
				Description += FString::Printf(TEXT("%s%s\n"), *IndentStr, *NodeTitle);
			}

			// Follow exec output pins
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin) continue;
				if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) continue;
				if (Pin->Direction != EGPD_Output) continue;

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						// Label branching pins
						if (Pin->PinName != TEXT("then") && Pin->PinName != TEXT("execute") && !Pin->PinName.IsNone())
						{
							Description += FString::Printf(TEXT("%s  [%s]:\n"), *IndentStr, *Pin->PinName.ToString());
							WalkExecChain(LinkedPin->GetOwningNode(), Indent + 2);
						}
						else
						{
							WalkExecChain(LinkedPin->GetOwningNode(), Indent + 1);
						}
					}
				}
			}
		};

		WalkExecChain(EntryNode, 0);
		Description += TEXT("\n");
	}

	// Node summary table — include first user-set pin value to disambiguate identical titles
	Description += FString::Printf(TEXT("### Nodes (%d total)\n\n"), Graph->Nodes.Num());
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		FString Title = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();

		FString Disambiguator;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) continue;
			if (Pin->Direction != EGPD_Input) continue;
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) continue;
			if (Pin->bHidden) continue;
			if (Pin->DefaultValue.IsEmpty()) continue;
			if (Pin->DefaultValue == Pin->AutogeneratedDefaultValue) continue;
			Disambiguator = FString::Printf(TEXT(" (%s=\"%s\")"), *Pin->PinName.ToString(), *Pin->DefaultValue);
			break;
		}

		Description += FString::Printf(TEXT("%s: %s%s\n"), *Node->GetName(), *Title, *Disambiguator);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("description"), Description);
	Data->SetStringField(TEXT("blueprint"), AssetPath);
	Data->SetStringField(TEXT("graph"), Graph->GetName());
	Data->SetNumberField(TEXT("node_count"), Graph->Nodes.Num());
	Data->SetNumberField(TEXT("entry_points"), EntryPoints.Num());

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoDescribeGraph. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleDescribeGraph(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_blueprint"), TEXT("describe"), FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/edit_graph
// Request: {"blueprint": "/Game/BP_Test.BP_Test", "operations": [
//   {"action": "connect", "source_node": "K2Node_Event_0", "source_pin": "then", "target_node": "K2Node_Delay_NEW", "target_pin": "execute"},
//   {"action": "disconnect", "source_node": "K2Node_Event_0", "source_pin": "then", "target_node": "K2Node_CallFunction_0", "target_pin": "execute"},
//   {"action": "delete_node", "node": "K2Node_Foo"}
// ]}
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoEditGraph(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	FString GraphName;
	Body->TryGetStringField(TEXT("graph"), GraphName);

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FToolExecutionResult::Error(404, TEXT("GRAPH_NOT_FOUND"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsArray;
	if (!Body->TryGetArrayField(TEXT("operations"), OpsArray))
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'operations' array is required"));
	}

	// Atomic mode: roll the whole batch back if any operation fails (default off,
	// preserving best-effort behavior).
	bool bAtomic = false;
	Body->TryGetBoolField(TEXT("atomic"), bAtomic);

	// Build node/pin lookup
	TMap<FString, UEdGraphNode*> NodesByName;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			NodesByName.Add(Node->GetName(), Node);
		}
	}

	auto FindPin = [](UEdGraphNode* Node, const FString& PinName, EEdGraphPinDirection Direction = EGPD_MAX) -> UEdGraphPin*
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString() == PinName)
			{
				if (Direction == EGPD_MAX || Pin->Direction == Direction)
				{
					return Pin;
				}
			}
		}
		return nullptr;
	};

	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);

	// Atomic mode: the pre-batch snapshot is what a rollback is verified
	// against, and every node is recorded in the batch's transaction up front —
	// an operation that skips Modify() (or a side effect such as
	// reposition-on-first-wire) must still be revertible.
	FKismetAIStateSnapshot AtomicBaseline;
	if (bAtomic)
	{
		AtomicBaseline = KismetAIBatchRecovery::CaptureGraph(Graph);
	}
	FKismetAIAtomicBatchScope Transaction(NSLOCTEXT("KismetAI", "EditGraph", "Kismet: Edit Graph"));
	if (bAtomic && !Transaction.CanRestore())
	{
		return AtomicBatchUnavailableError();
	}
	Graph->Modify();
	Blueprint->Modify();
	if (bAtomic)
	{
		Transaction.Track(Blueprint);
		Transaction.Track(Graph);
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			Transaction.Track(Node);
		}
	}

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 ErrorCount = 0;
	TSet<UEdGraphNode*> DirtyNodes;

	for (const TSharedPtr<FJsonValue>& OpVal : *OpsArray)
	{
		TSharedPtr<FJsonObject> Op = OpVal->AsObject();
		if (!Op) continue;

		FString Action;
		if (!Op->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
		{
			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
			ResultObj->SetStringField(TEXT("error"), TEXT("'action' field is required"));
			ErrorCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}
		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("action"), Action);

		if (Action == TEXT("connect") || Action == TEXT("disconnect"))
		{
			FString SourceNodeName = Op->GetStringField(TEXT("source_node"));
			FString SourcePinName = Op->GetStringField(TEXT("source_pin"));
			FString TargetNodeName = Op->GetStringField(TEXT("target_node"));
			FString TargetPinName = Op->GetStringField(TEXT("target_pin"));

			UEdGraphNode** SourceNodePtr = NodesByName.Find(SourceNodeName);
			UEdGraphNode** TargetNodePtr = NodesByName.Find(TargetNodeName);

			if (!SourceNodePtr || !*SourceNodePtr)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NODE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Source node not found: %s"), *SourceNodeName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"describe\") to see available node names")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			if (!TargetNodePtr || !*TargetNodePtr)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NODE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Target node not found: %s"), *TargetNodeName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"describe\") to see available node names")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UEdGraphPin* SourcePin = FindPin(*SourceNodePtr, SourcePinName, EGPD_Output);
			UEdGraphPin* TargetPin = FindPin(*TargetNodePtr, TargetPinName, EGPD_Input);

			if (!SourcePin)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PIN_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Source pin not found: %s.%s (output)"), *SourceNodeName, *SourcePinName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"t3d\") to see exact pin names")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			if (!TargetPin)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PIN_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Target pin not found: %s.%s (input)"), *TargetNodeName, *TargetPinName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"t3d\") to see exact pin names")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			if (Action == TEXT("connect"))
			{
				// Ask the schema WHY before/while connecting so we can surface the real
				// reason (type mismatch, direction, single-link replacement, …) instead
				// of a generic "pins may be incompatible".
				const UEdGraphSchema* Schema = Graph->GetSchema();
				const FPinConnectionResponse Resp = Schema
					? Schema->CanCreateConnection(SourcePin, TargetPin)
					: FPinConnectionResponse(CONNECT_RESPONSE_DISALLOW, NSLOCTEXT("KismetAI", "EditGraphNoSchema", "Graph has no schema"));
				if (Schema && Resp.Response != CONNECT_RESPONSE_DISALLOW && Schema->TryCreateConnection(SourcePin, TargetPin))
				{
					ResultObj->SetBoolField(TEXT("success"), true);
					SuccessCount++;
					UEdGraphNode* SrcNode = SourcePin->GetOwningNode();
					UEdGraphNode* TgtNode = TargetPin->GetOwningNode();
					DirtyNodes.Add(SrcNode);
					DirtyNodes.Add(TgtNode);

					// Reposition-on-first-wire: if either endpoint is an agent-created
					// island still parked at its unanchored spawn spot, pull it next to
					// its new peer. One-shot, and never touches user/pre-session nodes.
					MaybeRepositionOnFirstWire(Graph, AssetPath, SrcNode, TgtNode);
					MaybeRepositionOnFirstWire(Graph, AssetPath, TgtNode, SrcNode);
				}
				else
				{
					FString Reason = Resp.Message.ToString();
					if (Reason.IsEmpty()) Reason = TEXT("pins may be incompatible");
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("CONNECTION_FAILED"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Connection failed: %s"), *Reason));
					TArray<TSharedPtr<FJsonValue>> Sugg;
					Sugg.Add(MakeShared<FJsonValueString>(TEXT("Check pin types/direction. A conversion node may be needed (e.g. Conv_DoubleToString).")));
					ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
					ErrorCount++;
				}
			}
			else // disconnect
			{
				SourcePin->Modify();
				TargetPin->Modify();
				SourcePin->BreakLinkTo(TargetPin);
				ResultObj->SetBoolField(TEXT("success"), true);
				SuccessCount++;
				DirtyNodes.Add(SourcePin->GetOwningNode());
				DirtyNodes.Add(TargetPin->GetOwningNode());
			}
		}
		else if (Action == TEXT("set_pin_default"))
		{
			FString NodeName = Op->GetStringField(TEXT("node"));
			FString PinName = Op->GetStringField(TEXT("pin"));
			FString Value = Op->GetStringField(TEXT("value"));

			UEdGraphNode** NodePtr = NodesByName.Find(NodeName);
			if (!NodePtr || !*NodePtr)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NODE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Node not found: %s"), *NodeName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"describe\") to see available node names")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UEdGraphPin* TargetPin = FindPin(*NodePtr, PinName, EGPD_Input);
			if (!TargetPin)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PIN_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Pin not found: %s.%s (input)"), *NodeName, *PinName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"t3d\") to see exact pin names")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Route the write through the schema: a raw DefaultValue assignment
			// accepts malformed values (bad struct literals, out-of-range enums)
			// that only surface as compile errors much later, and silently does
			// the wrong thing for text/object pins which store their default in
			// DefaultTextValue/DefaultObject instead.
			const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(Graph->GetSchema());
			const FName PinCategory = TargetPin->PinType.PinCategory;
			const bool bIsObjectPin =
				PinCategory == UEdGraphSchema_K2::PC_Object ||
				PinCategory == UEdGraphSchema_K2::PC_Class ||
				PinCategory == UEdGraphSchema_K2::PC_Interface;

			FString ValidationError;
			UObject* NewDefaultObject = nullptr;
			if (bIsObjectPin && !Value.IsEmpty() && Value != TEXT("None"))
			{
				NewDefaultObject = LoadObject<UObject>(nullptr, *Value);
				if (!NewDefaultObject)
				{
					ValidationError = FString::Printf(TEXT("Object not found: '%s' (expected a full object path, or 'None' to clear)"), *Value);
				}
			}

			if (ValidationError.IsEmpty() && K2Schema)
			{
				if (PinCategory == UEdGraphSchema_K2::PC_Text)
				{
					ValidationError = K2Schema->IsPinDefaultValid(TargetPin, FString(), nullptr, FText::FromString(Value));
				}
				else if (bIsObjectPin)
				{
					ValidationError = K2Schema->IsPinDefaultValid(TargetPin, FString(), NewDefaultObject, FText());
				}
				else
				{
					ValidationError = K2Schema->IsPinDefaultValid(TargetPin, Value, nullptr, FText());
				}
			}

			if (!ValidationError.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_DEFAULT"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Invalid default for %s.%s: %s"), *NodeName, *PinName, *ValidationError));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("Pin type is '%s' — format the value to match. Struct pins use parenthesized literals, e.g. LinearColor: (R=1.000000,G=0.000000,B=0.000000,A=1.000000)"),
					*PinCategory.ToString())));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			TargetPin->Modify();
			FString PreviousValue = TargetPin->GetDefaultAsString();
			if (K2Schema)
			{
				if (PinCategory == UEdGraphSchema_K2::PC_Text)
				{
					K2Schema->TrySetDefaultText(*TargetPin, FText::FromString(Value));
				}
				else if (bIsObjectPin)
				{
					K2Schema->TrySetDefaultObject(*TargetPin, NewDefaultObject);
				}
				else
				{
					K2Schema->TrySetDefaultValue(*TargetPin, Value);
				}
			}
			else
			{
				TargetPin->DefaultValue = Value;
			}
			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("previous_value"), PreviousValue);
			ResultObj->SetStringField(TEXT("new_value"), TargetPin->GetDefaultAsString());
			SuccessCount++;
		}
		else if (Action == TEXT("delete_node"))
		{
			FString NodeName = Op->GetStringField(TEXT("node"));
			UEdGraphNode** NodePtr = NodesByName.Find(NodeName);
			if (NodePtr && *NodePtr)
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, *NodePtr);
				NodesByName.Remove(NodeName);
				ResultObj->SetBoolField(TEXT("success"), true);
				SuccessCount++;
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NODE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Node not found: %s"), *NodeName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"describe\") to see available node names")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
			}
		}
		else if (Action == TEXT("set_node_property"))
		{
			// Set a UPROPERTY directly on a node (not a pin). Primary use is anim nodes
			// whose settings live on the node or its runtime struct rather than on a pin:
			// e.g. CacheName on SaveCachedPose (top-level), or "Node.GroupName" /
			// "BlendNode.AlphaScaleBias.Bias" (a field inside the runtime struct — the
			// struct member name varies per node: "Node" on most, "BlendNode" on
			// TwoWayBlend). Dotted paths walk struct sub-properties; value is parsed via
			// FProperty::ImportText so it accepts the same literals UE serializes.
			const FString NodeName = Op->GetStringField(TEXT("node"));
			const FString PropertyPath = Op->GetStringField(TEXT("property"));
			const FString Value = Op->GetStringField(TEXT("value"));

			UEdGraphNode** NodePtr = NodesByName.Find(NodeName);
			if (!NodePtr || !*NodePtr)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NODE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Node not found: %s"), *NodeName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			if (PropertyPath.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'property' is required for set_node_property"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Walk the (possibly dotted) property path to the leaf FProperty + container ptr.
			UEdGraphNode* TargetNode = *NodePtr;
			TArray<FString> Segments;
			PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
			UStruct* OwnerStruct = TargetNode->GetClass();
			void* Container = TargetNode;
			FProperty* LeafProp = nullptr;
			FString PathError;
			for (int32 SegIdx = 0; SegIdx < Segments.Num(); ++SegIdx)
			{
				FProperty* Prop = OwnerStruct ? OwnerStruct->FindPropertyByName(FName(*Segments[SegIdx])) : nullptr;
				if (!Prop)
				{
					PathError = FString::Printf(TEXT("Property '%s' not found on %s"), *Segments[SegIdx],
						SegIdx == 0 ? *TargetNode->GetClass()->GetName() : *Segments[SegIdx - 1]);
					break;
				}
				if (SegIdx == Segments.Num() - 1)
				{
					LeafProp = Prop;
					break;
				}
				// Intermediate segment must be a struct we can descend into.
				FStructProperty* StructProp = CastField<FStructProperty>(Prop);
				if (!StructProp)
				{
					PathError = FString::Printf(TEXT("Property '%s' is not a struct; cannot descend into '%s'"), *Segments[SegIdx], *Segments[SegIdx + 1]);
					break;
				}
				Container = StructProp->ContainerPtrToValuePtr<void>(Container);
				OwnerStruct = StructProp->Struct;
			}

			if (!PathError.IsEmpty() || !LeafProp)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PROPERTY_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), PathError.IsEmpty() ? FString::Printf(TEXT("Property path '%s' did not resolve"), *PropertyPath) : PathError);
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use read_blueprint(mode=\"t3d\") to see the node's serialized property names. The runtime struct is usually named 'Node' (or 'BlendNode' on TwoWayBlend); address fields inside it as 'Node.FieldName'.")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			TargetNode->Modify();
			void* LeafPtr = LeafProp->ContainerPtrToValuePtr<void>(Container);
			FString PreviousValue;
			LeafProp->ExportTextItem_Direct(PreviousValue, LeafPtr, LeafPtr, TargetNode, PPF_None);

			FStringOutputDevice ImportErrors;
			const TCHAR* Result = LeafProp->ImportText_Direct(*Value, LeafPtr, TargetNode, PPF_None, &ImportErrors);
			if (Result == nullptr || !ImportErrors.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_PROPERTY_VALUE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not set %s.%s = '%s': %s"),
					*NodeName, *PropertyPath, *Value, ImportErrors.IsEmpty() ? TEXT("value did not parse for this property type") : *ImportErrors));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Property changes can affect pins (optional-pin defaults, exposed values);
			// reconstruct so the editor view + pins stay consistent.
			TargetNode->ReconstructNode();
			DirtyNodes.Add(TargetNode);

			FString NewValue;
			LeafProp->ExportTextItem_Direct(NewValue, LeafPtr, LeafPtr, TargetNode, PPF_None);
			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("property"), PropertyPath);
			ResultObj->SetStringField(TEXT("previous_value"), PreviousValue);
			ResultObj->SetStringField(TEXT("new_value"), NewValue);
			SuccessCount++;
		}
		else if (Action == TEXT("set_pin_visibility"))
		{
			// Toggle an anim node's "property as pin" exposure: each entry in
			// ShowPinForProperties maps a runtime-struct property to a pin. Flipping bShowPin
			// + ReconstructNode() adds/removes the pin (so the agent can then wire/set it, or
			// hide it back to a default). Anim nodes only.
			const FString NodeName = Op->GetStringField(TEXT("node"));
			const FString PropertyName = Op->GetStringField(TEXT("property"));
			bool bVisible = true;
			Op->TryGetBoolField(TEXT("visible"), bVisible);

			UEdGraphNode** NodePtr = NodesByName.Find(NodeName);
			if (!NodePtr || !*NodePtr)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NODE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Node not found: %s"), *NodeName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			UAnimGraphNode_Base* AnimNode = Cast<UAnimGraphNode_Base>(*NodePtr);
			if (!AnimNode)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_AN_ANIM_NODE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("set_pin_visibility only applies to animation nodes; '%s' is a %s"), *NodeName, *(*NodePtr)->GetClass()->GetName()));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			if (PropertyName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'property' is required for set_pin_visibility"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			bool bFound = false;
			TArray<FString> Available;
			for (FOptionalPinFromProperty& Opt : AnimNode->ShowPinForProperties)
			{
				Available.Add(Opt.PropertyName.ToString());
				if (Opt.PropertyName.ToString() == PropertyName)
				{
					if (!Opt.bCanToggleVisibility && Opt.bShowPin != bVisible)
					{
						ResultObj->SetBoolField(TEXT("success"), false);
						ResultObj->SetStringField(TEXT("error_code"), TEXT("PIN_NOT_TOGGLEABLE"));
						ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Pin '%s' on '%s' cannot toggle visibility"), *PropertyName, *NodeName));
						ErrorCount++;
						bFound = true;
					}
					else
					{
						AnimNode->Modify();
						Opt.bShowPin = bVisible;
						AnimNode->ReconstructNode();
						DirtyNodes.Add(AnimNode);
						ResultObj->SetBoolField(TEXT("success"), true);
						ResultObj->SetStringField(TEXT("property"), PropertyName);
						ResultObj->SetBoolField(TEXT("visible"), bVisible);
						SuccessCount++;
						bFound = true;
					}
					break;
				}
			}
			if (!bFound)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PROPERTY_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("No toggleable pin property '%s' on '%s'. Available: %s"),
					*PropertyName, *NodeName, Available.Num() ? *FString::Join(Available, TEXT(", ")) : TEXT("(none)")));
				ErrorCount++;
			}
		}
		else
		{
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("UNKNOWN_ACTION"));
			ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown action: %s"), *Action));
			TArray<TSharedPtr<FJsonValue>> Sugg;
			Sugg.Add(MakeShared<FJsonValueString>(TEXT("Valid actions: connect, disconnect, set_pin_default, set_node_property, set_pin_visibility, delete_node")));
			ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
			ErrorCount++;
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
	}

	// Atomic mode: if any operation failed, undo the whole batch so the graph is
	// never left half-wired (a half-applied batch otherwise drives the agent into
	// re-read/retry loops). The batch's own transaction record is applied and the
	// result verified against the pre-batch snapshot; rolled_back is true only
	// when the graph is provably back at baseline.
	if (bAtomic && ErrorCount > 0)
	{
		const FAtomicRestoreReport Restore = RestoreFailedAtomicBatch(Transaction, AtomicBaseline,
			[Graph]() { return KismetAIBatchRecovery::CaptureGraph(Graph); },
			Blueprint->GetOutermost(), bPackageWasDirtyBefore);
		if (Restore.Status == EKismetAIRecoveryStatus::Restored)
		{
			Graph->NotifyGraphChanged();
		}
		const bool bRestored = Restore.Status == EKismetAIRecoveryStatus::Restored;

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		AttachMutationOutcome(Data, MakeAtomicRollbackOutcome(Restore, Blueprint->GetOutermost()->GetName(),
			bPackageWasDirtyBefore, KismetAISavePolicy::AutosaveEvenIfCompileFailed));
		// After an incomplete restore the operations that succeeded may still be
		// applied, so they are reported rather than zeroed.
		Data->SetNumberField(TEXT("operations_succeeded"), bRestored ? 0 : SuccessCount);
		Data->SetNumberField(TEXT("operations_failed"), ErrorCount);
		AddAtomicRollbackFields(Data, Restore);
		Data->SetArrayField(TEXT("results"), ResultsArray);
		Data->SetStringField(TEXT("message"), DescribeAtomicRollback(Restore, ErrorCount, TEXT("graph")));
		return FToolExecutionResult::Success(Data, false);
	}

	// Deliberately no auto-layout here: edit_graph only rewires/retypes
	// existing nodes, and repositioning nodes the agent didn't create destroys
	// human-authored layout (and pulls nodes out of their comment frames).

	// Compile
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);

	// Compile result first, then persist and record the save — the compile flag
	// used to be computed only after the save had already happened.
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.bAppliedInMemory = SuccessCount > 0;
	RecordCompileOutcome(Outcome, Blueprint, CompileResults);
	AutoSaveAfterMutation(Blueprint, Outcome);

	bool bCompileSuccess = (Blueprint->Status != BS_Error);

	// Collect compile errors/warnings
	TArray<TSharedPtr<FJsonValue>> CompileMessagesArray;
	for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();

		EMessageSeverity::Type Severity = Message->GetSeverity();
		FString SeverityStr;
		switch (Severity)
		{
		case EMessageSeverity::Error: SeverityStr = TEXT("error"); break;
		case EMessageSeverity::Warning: SeverityStr = TEXT("warning"); break;
		case EMessageSeverity::Info: SeverityStr = TEXT("info"); break;
		default: SeverityStr = TEXT("note"); break;
		}

		MsgObj->SetStringField(TEXT("severity"), SeverityStr);
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());
		CompileMessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetNumberField(TEXT("operations_succeeded"), SuccessCount);
	Data->SetNumberField(TEXT("operations_failed"), ErrorCount);
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetBoolField(TEXT("compile_success"), bCompileSuccess);
	Data->SetArrayField(TEXT("compile_messages"), CompileMessagesArray);

	// Check for orphaned nodes
	TArray<FString> OrphanWarnings = DetectOrphanedNodes(Graph);
	if (OrphanWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> OrphanArray;
		for (const FString& W : OrphanWarnings)
		{
			OrphanArray.Add(MakeShared<FJsonValueString>(W));
		}
		Data->SetArrayField(TEXT("orphaned_node_warnings"), OrphanArray);
	}

	// Check for exec subgraphs left unreachable by this batch (e.g. a Sequence
	// output disconnected without rewiring its downstream chain).
	TArray<FString> UnreachableWarnings = DetectUnreachableExecNodes(Graph);
	if (UnreachableWarnings.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> UnreachableArray;
		for (const FString& W : UnreachableWarnings)
		{
			UnreachableArray.Add(MakeShared<FJsonValueString>(W));
		}
		Data->SetArrayField(TEXT("unreachable_node_warnings"), UnreachableArray);
	}

	return FToolExecutionResult::Success(Data, ErrorCount == 0);
}

// HTTP adapter over DoEditGraph. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleEditGraph(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("edit_blueprint"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/layout_graph
// Request: {"blueprint": "/Game/BP_Test", "graph": "EventGraph", "nodes": ["K2Node_..."]}
// Positions only — wiring is never touched, so no compile is needed.
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoLayoutGraph(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	FString GraphName;
	Body->TryGetStringField(TEXT("graph"), GraphName);

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FToolExecutionResult::Error(404, TEXT("GRAPH_NOT_FOUND"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	// Optional seeds: each seed's whole connected component gets reformatted.
	TSet<UEdGraphNode*> Seeds;
	const TArray<TSharedPtr<FJsonValue>>* NodeNames = nullptr;
	if (Body->TryGetArrayField(TEXT("nodes"), NodeNames))
	{
		TMap<FString, UEdGraphNode*> NodesByName;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				NodesByName.Add(Node->GetName(), Node);
			}
		}
		for (const TSharedPtr<FJsonValue>& Val : *NodeNames)
		{
			const FString Name = Val->AsString();
			UEdGraphNode** Found = NodesByName.Find(Name);
			if (!Found || !*Found)
			{
				return FToolExecutionResult::Error(404, TEXT("NODE_NOT_FOUND"), FString::Printf(TEXT("Node not found: %s"), *Name),
					{TEXT("Use read_blueprint(mode=\"describe\") to see available node names")});
			}
			Seeds.Add(*Found);
		}
	}

	// Optional: insert reroute knots for long wires (only when requested).
	bool bReroute = false;
	Body->TryGetBoolField(TEXT("reroute"), bReroute);

	// Optional: wrap each event subtree in a keyword-colored comment box.
	bool bGroup = false;
	Body->TryGetBoolField(TEXT("group"), bGroup);

	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "LayoutGraph", "Kismet: Layout Graph"));
	Graph->Modify();
	TMap<UEdGraphNode*, TPair<int32, int32>> Before;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			Node->Modify();
			Before.Add(Node, TPair<int32, int32>(Node->NodePosX, Node->NodePosY));
		}
	}

	FKismetAILayout::FormatNodes(Graph, Seeds, bReroute, bGroup);

	int32 MovedCount = 0;
	for (const auto& Pair : Before)
	{
		if (Pair.Key->NodePosX != Pair.Value.Key || Pair.Key->NodePosY != Pair.Value.Value)
		{
			MovedCount++;
		}
	}

	// Positions are not structural — no recompile, just persist. compile_status
	// stays not_run: moving nodes neither fixes nor breaks a compile.
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.bAppliedInMemory = true; // positions were (re)written, even if none moved
	AutoSaveAfterMutation(Blueprint, Outcome);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetStringField(TEXT("graph"), Graph->GetName());
	Data->SetNumberField(TEXT("nodes_moved"), MovedCount);
	Data->SetNumberField(TEXT("nodes_total"), Before.Num());
	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoLayoutGraph. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleLayoutGraph(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("layout_graph"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Interactive ask_user: deliver the user's answer to the suspended agent
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIModule::SubmitAgentAnswer(const FString& QuestionId, const FString& AnswersJson, EKismetAIEntryPoint AnsweredVia)
{
	// Only the lease holder can take an answer, and only while it is running
	// and not cancelled; its agent refuses any id it is not waiting on.
	return Session->SubmitAnswer(QuestionId, AnswersJson, AnsweredVia);
}

// POST /api/v1/agent/answer — { "question_id": "...", "answers": {..}|"..." }
bool FKismetAIModule::HandleAgentAnswer(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	TSharedPtr<FJsonObject> Body = ParseRequestBody(Request);
	if (!Body.IsValid())
	{
		SendError(OnComplete, 400, TEXT("INVALID_BODY"),
			TEXT("Request body must be a JSON object with 'question_id' and 'answers'."), {});
		return true;
	}

	FString QuestionId;
	Body->TryGetStringField(TEXT("question_id"), QuestionId);
	if (QuestionId.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("MISSING_FIELD"), TEXT("'question_id' is required."), {});
		return true;
	}

	// 'answers' may be an object map {question: selection} or a plain string.
	FString AnswersJson;
	const TSharedPtr<FJsonObject>* AnswersObj = nullptr;
	if (Body->TryGetObjectField(TEXT("answers"), AnswersObj) && AnswersObj)
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&AnswersJson);
		FJsonSerializer::Serialize((*AnswersObj).ToSharedRef(), Writer);
	}
	else
	{
		Body->TryGetStringField(TEXT("answers"), AnswersJson);
	}

	// A REST answer can settle ask_user questions but not approvals.
	if (!SubmitAgentAnswer(QuestionId, AnswersJson, EKismetAIEntryPoint::Rest))
	{
		SendError(OnComplete, 409, TEXT("NO_PENDING_QUESTION"),
			TEXT("No agent is currently waiting for an answer to this question_id."), {});
		return true;
	}

	SendSuccess(OnComplete, nullptr);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// External agent runs
//
//   POST /api/v1/agent/runs           submit; 202 with the run id at once
//   GET  /api/v1/agent/runs/status    state, phase, progress, terminal outcome
//   GET  /api/v1/agent/runs/events    events after a cursor; JSON page or SSE
//   POST /api/v1/agent/runs/cancel    idempotent cancel, reports the phase
//   POST /api/v1/agent/run            compatibility: submit + live SSE stream
//
// The run is model orchestration on a session-controller worker; only editor
// operations go to the game thread (through the dispatcher, as for the panel).
// Previously, /agent/run ran the loop inline on the game thread and sent all
// events in one body at the end.
// ─────────────────────────────────────────────────────────────────────────────
// Provider routing (IsOpenAIModel) lives in KismetAILLMProvider.h — single source.

namespace KismetAIExternalRunHttp
{
	constexpr int32 MaxClientRequestIdLen = 128;
	constexpr int32 DefaultEventPage = 500;
	constexpr int32 MaxEventPage = 2000;
	// Stream pump cadence and the idle interval after which a comment line is
	// written: a write is how a dropped client is noticed (the connection is
	// destroyed on the failed send and the pump then detaches).
	constexpr float PumpIntervalSeconds = 0.05f;
	constexpr double KeepAliveSeconds = 5.0;

	static void SendJson(const FHttpResultCallback& OnComplete, int32 Code, const TSharedRef<FJsonObject>& Envelope)
	{
		FString Text;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Envelope, Writer);
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Text, TEXT("application/json"));
		Response->Code = static_cast<EHttpServerResponseCodes>(Code);
		OnComplete(MoveTemp(Response));
	}

	static void SendData(const FHttpResultCallback& OnComplete, int32 Code, const TSharedRef<FJsonObject>& Data)
	{
		TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetBoolField(TEXT("success"), true);
		Envelope->SetObjectField(TEXT("data"), Data);
		SendJson(OnComplete, Code, Envelope);
	}

	static void SendErrorWithData(const FHttpResultCallback& OnComplete, int32 Code, const FString& ErrorCode,
		const FString& Message, const TSharedRef<FJsonObject>& Data)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("error_code"), ErrorCode);
		Error->SetStringField(TEXT("message"), Message);
		TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
		Envelope->SetBoolField(TEXT("success"), false);
		Envelope->SetObjectField(TEXT("error"), Error);
		Envelope->SetObjectField(TEXT("data"), Data);
		SendJson(OnComplete, Code, Envelope);
	}

	static FString QueryParam(const FHttpServerRequest& Request, const TCHAR* Name)
	{
		const FString* Value = Request.QueryParams.Find(Name);
		return Value ? *Value : FString();
	}

	static FString HeaderValue(const FHttpServerRequest& Request, const FString& Name)
	{
		for (const TPair<FString, TArray<FString>>& Header : Request.Headers)
		{
			if (Header.Key.Equals(Name, ESearchCase::IgnoreCase) && Header.Value.Num() > 0)
			{
				return Header.Value[0];
			}
		}
		return FString();
	}

	static TSharedRef<FJsonObject> Links(const FString& RunId)
	{
		TSharedRef<FJsonObject> L = MakeShared<FJsonObject>();
		L->SetStringField(TEXT("status"), FString::Printf(TEXT("/api/v1/agent/runs/status?run_id=%s"), *RunId));
		L->SetStringField(TEXT("events"), FString::Printf(TEXT("/api/v1/agent/runs/events?run_id=%s&after=0"), *RunId));
		L->SetStringField(TEXT("stream"), FString::Printf(TEXT("/api/v1/agent/runs/events?run_id=%s&after=0&stream=1"), *RunId));
		L->SetStringField(TEXT("cancel"), TEXT("/api/v1/agent/runs/cancel"));
		return L;
	}

	static TSharedRef<FJsonObject> StatusOf(FKismetAISessionController& Session, const FKismetAIExternalRunRecord& Record)
	{
		TOptional<FKismetAIRunStatus> Worker;
		if (const FKismetAIRunPtr Run = Record.GetControllerRun())
		{
			Worker = Session.GetRunStatus(Run.ToSharedRef());
		}
		return Record.BuildStatusJson(Worker);
	}

	static TSharedRef<FJsonObject> ExpiredData(const FKismetAIExternalRunRecord& Record, int64 After, const FKismetAIRunEventPage& Page)
	{
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("run_id"), Record.GetRunId());
		D->SetNumberField(TEXT("after"), After);
		D->SetNumberField(TEXT("first_seq"), Page.FirstRetainedSeq);
		D->SetNumberField(TEXT("last_seq"), Page.LastSeq);
		D->SetNumberField(TEXT("resume_after"), Page.FirstRetainedSeq - 1);
		D->SetBoolField(TEXT("finished"), Page.bFinished);
		return D;
	}

	using FByteQueue = TQueue<TArray<uint8>, EQueueMode::Spsc>;

	// Moves one stream's events from the run record into its HTTP response.
	// Producer side of the SPSC body queue: runs only on the game thread (the
	// first fill in the handler, then the core ticker), where the connection
	// also consumes it. Holds the queue weakly: once the connection is
	// destroyed (client gone, server stopped) the pin fails and the pump
	// detaches. Detaching never cancels the run.
	struct FStreamPump
	{
		FKismetAIExternalRunRef Record;
		TWeakPtr<FByteQueue> Queue;
		TSharedPtr<TAtomic<bool>> Complete;
		TWeakPtr<uint8> WeakLife;
		int64 Cursor = 0;
		double LastWrite = 0.0;

		FStreamPump(const FKismetAIExternalRunRef& InRecord) : Record(InRecord) {}

		// False once the stream is over: all events through the terminal
		// outcome are queued, or there is no one left to send them to.
		bool PumpOnce()
		{
			TSharedPtr<FByteQueue> Pinned = Queue.Pin();
			if (!Pinned.IsValid() || !WeakLife.IsValid())
			{
				return false;
			}
			FString Chunk;
			FKismetAIRunEventPage Page = Record->Read(Cursor, 256);
			if (Page.bExpired)
			{
				// Fell behind the retained window mid-stream: say so, then go on
				// from the oldest retained event.
				TSharedRef<FJsonObject> Gap = MakeShared<FJsonObject>();
				Gap->SetStringField(TEXT("type"), TEXT("gap"));
				Gap->SetStringField(TEXT("run_id"), Record->GetRunId());
				Gap->SetNumberField(TEXT("after"), Cursor);
				Gap->SetNumberField(TEXT("first_seq"), Page.FirstRetainedSeq);
				FString GapJson;
				TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&GapJson);
				FJsonSerializer::Serialize(Gap, Writer);
				Chunk += FString::Printf(TEXT("event: gap\ndata: %s\n\n"), *GapJson);
				Cursor = Page.FirstRetainedSeq - 1;
				Page = Record->Read(Cursor, 256);
			}
			for (const FKismetAIRunEvent& Event : Page.Events)
			{
				Chunk += KismetAIExternalRuns::FormatSseFrame(Event.Seq, Event.Name, Event.Data);
			}
			Cursor = FMath::Max(Cursor, Page.NextAfter);

			const double Now = FPlatformTime::Seconds();
			if (Chunk.IsEmpty() && Now - LastWrite >= KeepAliveSeconds)
			{
				Chunk = TEXT(": keepalive\n\n");
			}
			if (!Chunk.IsEmpty())
			{
				const FTCHARToUTF8 Utf8(*Chunk);
				TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
				Pinned->Enqueue(MoveTemp(Bytes));
				LastWrite = Now;
			}
			if (Page.bFinished && !Page.bMore && Cursor >= Page.LastSeq)
			{
				Complete->Store(true);
				return false;
			}
			return true;
		}
	};
}

void FKismetAIModule::StreamExternalRunEvents(const FKismetAIExternalRunRef& Record, int64 AfterSeq,
	const FHttpResultCallback& OnComplete)
{
	using namespace KismetAIExternalRunHttp;

	TSharedPtr<FByteQueue> Queue = MakeShared<FByteQueue>();
	TSharedPtr<FStreamPump> Pump = MakeShared<FStreamPump>(Record);
	Pump->Queue = Queue;
	Pump->Complete = MakeShared<TAtomic<bool>>(false);
	Pump->WeakLife = Lifeline;
	Pump->Cursor = FMath::Max<int64>(0, AfterSeq);
	Pump->LastWrite = FPlatformTime::Seconds();

	TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(FString(), TEXT("text/event-stream"));
	Response->Code = EHttpServerResponseCodes::Ok;
	Response->Headers.Add(TEXT("Cache-Control"), { TEXT("no-cache") });
	Response->Headers.Add(TEXT("X-Kismet-Run-Id"), { Record->GetRunId() });
	KismetAIHttpCompat::AttachStream(*Response, Queue, Pump->Complete);

	// The first chunk is queued before the response is handed over: the
	// connection writes no headers until a chunk arrives and would time the
	// write out after 5 s without one. An SSE comment, ignored by parsers.
	{
		const FString Attached = FString::Printf(TEXT(": attached %s\n\n"), *Record->GetRunId());
		const FTCHARToUTF8 Utf8(*Attached);
		Queue->Enqueue(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	}
	const bool bMore = Pump->PumpOnce();
	OnComplete(MoveTemp(Response));
	Queue.Reset();   // the response owns it now; the pump only observes

	if (bMore)
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Pump](float)
		{
			return Pump->PumpOnce();
		}), PumpIntervalSeconds);
	}
}

FKismetAIModule::FExternalSubmit FKismetAIModule::SubmitExternalRun(const FHttpServerRequest& Request)
{
	FExternalSubmit Out;
	auto Refuse = [&Out](int32 Status, const TCHAR* Code, const FString& Message) -> FExternalSubmit
	{
		Out.ErrorStatus = Status;
		Out.ErrorCode = Code;
		Out.ErrorMessage = Message;
		return Out;
	};

	TSharedPtr<FJsonObject> Body = ParseRequestBody(Request);
	if (!Body.IsValid())
	{
		return Refuse(400, TEXT("invalid_json"), TEXT("Failed to parse request body"));
	}

	FString Input;
	if (!Body->TryGetStringField(TEXT("input"), Input) || Input.IsEmpty())
	{
		return Refuse(400, TEXT("missing_input"), TEXT("Request must include 'input' field"));
	}

	// Optional client-chosen id for this submission. Repeating it never starts
	// a second run: the same request gets the run it already started, a
	// different request under the same id is a conflict.
	FString ClientRequestId;
	Body->TryGetStringField(TEXT("client_request_id"), ClientRequestId);
	if (ClientRequestId.Len() > KismetAIExternalRunHttp::MaxClientRequestIdLen)
	{
		return Refuse(400, TEXT("invalid_client_request_id"), FString::Printf(
			TEXT("'client_request_id' is at most %d characters."), KismetAIExternalRunHttp::MaxClientRequestIdLen));
	}

	// Override max_steps if provided. Clamp to a safe range so a
	// caller-supplied value can't burn arbitrary tokens or freeze the
	// editor in a runaway loop.
	FKismetAIAgentConfig RunConfig = AgentConfig;
	if (!RunConfig.RunBudgetError.IsEmpty()
		|| !FKismetAIRunLimits::ReadSettings(*Body, RunConfig.RunLimits, RunConfig.RunBudgetError))
		return Refuse(400, TEXT("invalid_run_budget"), RunConfig.RunBudgetError);
	if (Body->HasField(TEXT("max_steps")))
	{
		RunConfig.MaxSteps = FMath::Clamp(Body->GetIntegerField(TEXT("max_steps")), 1, 128);
	}

	// The request may carry an optional session_id to continue a session and a
	// 'blueprint' hint naming the target asset (the eval framework sends
	// target_blueprint here).
	FString RequestedSessionId;
	Body->TryGetStringField(TEXT("session_id"), RequestedSessionId);
	FString TargetBlueprint;
	Body->TryGetStringField(TEXT("blueprint"), TargetBlueprint);
	const FString Fingerprint = KismetAIExternalRuns::FingerprintRequest(Input, RequestedSessionId, TargetBlueprint, RunConfig.MaxSteps)
		+ TEXT("|") + CondenseJsonObject(RunConfig.RunLimits.ToJson());

	if (!ClientRequestId.IsEmpty())
	{
		if (const FKismetAIExternalRunPtr Existing = ExternalRuns->FindByClientRequestId(ClientRequestId))
		{
			if (Existing->GetFingerprint() != Fingerprint)
			{
				return Refuse(409, TEXT("request_id_conflict"), FString::Printf(
					TEXT("client_request_id '%s' already names run %s with a different request. Nothing was started."),
					*ClientRequestId, *Existing->GetRunId()));
			}
			Out.Record = Existing;
			Out.bExisting = true;
			return Out;
		}
	}

	// Lease: one run at a time, panel or agent/run. Refuse before any
	// side effect (diff baseline, session id, grant). TryAdmit below re-checks
	// atomically; this early answer just does not depend on key or config state.
	if (Session->IsClosed())
	{
		return Refuse(503, TEXT("shutting_down"), TEXT("The editor is shutting down; no new agent run is accepted."));
	}
	if (const TOptional<FKismetAIRunStatus> Active = Session->GetActiveRun())
	{
		return Refuse(409, TEXT("agent_busy"), FString::Printf(
			TEXT("Another agent run is active (%s run %s). Nothing was started; retry when it finishes or stop it."),
			LexToString(Active->Origin), *Active->RunId));
	}

	FString SetupError;
	if (!KismetAIRunSetup::Prepare(RunConfig, SetupError))
	{
		return Refuse(400, TEXT("run_setup_failed"), SetupError);
	}

	if (!AgentFactoryForTests)
	{
		// The key for the run's own profile, never another provider's.
		const FKismetAIProviderProfile& Profile = KismetAIProviders::ResolveProfile(AgentConfig);
		if (!KismetAIProviders::HasUsableKey(AgentConfig, Profile))
		{
			const FString EnvHint = Profile.KeyEnvVar.IsEmpty()
				? FString()
				: FString::Printf(TEXT(" (or set the %s environment variable)"), *Profile.KeyEnvVar);
			return Refuse(500, TEXT("no_api_key"), FString::Printf(
				TEXT("No %s API key configured. Open Settings in the Kismet panel%s."), *Profile.DisplayName, *EnvHint));
		}
	}

	// An HTTP agent run is unattended: nobody can approve anything mid-run, so
	// it may only change what the caller grants it explicitly. The
	// optional 'authority' object — {"operations": [...], "scope": [...]} —
	// becomes a grant bound to this run's session and revoked when the run
	// ends. Resuming a session never brings back an earlier run's authority.
	FKismetAIAuthorityGrant RunGrant;
	bool bHasRunGrant = false;
	{
		const TSharedPtr<FJsonObject>* AuthorityObj = nullptr;
		if (Body->TryGetObjectField(TEXT("authority"), AuthorityObj) && AuthorityObj && AuthorityObj->IsValid())
		{
			TSharedRef<FJsonObject> Spec = MakeShared<FJsonObject>(**AuthorityObj);
			Spec->RemoveField(TEXT("entry_points"));
			Spec->RemoveField(TEXT("lifetime"));
			Spec->RemoveField(TEXT("session_id"));
			Spec->SetArrayField(TEXT("entry_points"), { MakeShared<FJsonValueString>(LexToString(EKismetAIEntryPoint::HttpAgentRun)) });
			FString GrantError;
			if (!FKismetAIAuthorityGrant::FromJson(*Spec, RunGrant, GrantError))
			{
				return Refuse(400, TEXT("invalid_authority"), GrantError);
			}
			bHasRunGrant = true;
		}
	}

	// Create the agent and take the lease before the first side effect.
	TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe> Agent = AgentFactoryForTests
		? AgentFactoryForTests(RunConfig)
		: MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(MakeUnique<FKismetAIModuleToolInvoker>(*this), RunConfig);
	// Reached only through WithAuth: the X-Kismet-Token check passed.
	Agent->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::HttpAgentRun, TEXT("x-kismet-token")));
	const FKismetAIAdmission Admission = Session->TryAdmit(EKismetAIRunOrigin::HttpAgentRun, Agent);
	if (!Admission.IsAdmitted())
	{
		if (Admission.Refusal == EKismetAIAdmissionRefusal::Closed)
		{
			return Refuse(503, TEXT("shutting_down"), TEXT("The editor is shutting down; no new agent run is accepted."));
		}
		return Refuse(409, TEXT("agent_busy"), FString::Printf(
			TEXT("Another agent run is active (%s run %s). Nothing was started; retry when it finishes or stop it."),
			LexToString(Admission.ActiveOrigin), *Admission.ActiveRunId));
	}
	const FKismetAIRunRef Run = Admission.Run.ToSharedRef();

	// Arm the per-session diff baseline so run-scoped tracking — "nodes the agent
	// created this run" and reposition-on-first-wire — works for HTTP-driven runs.
	const bool bStartNewSession = RequestedSessionId.IsEmpty();
	const FString RunSessionId = ArmDiffSessionForRun(RequestedSessionId, TargetBlueprint);

	FString RunGrantId;
	if (bHasRunGrant)
	{
		RunGrant.SessionId = RunSessionId;
		RunGrant.Lifetime = EKismetAIGrantLifetime::Session;
		RunGrant.CreatedBy = TEXT("x-kismet-token (agent/run)");
		if (RunGrant.Label.IsEmpty())
		{
			RunGrant.Label = TEXT("agent-run");
		}
		FString GrantError;
		RunGrantId = GetAuthorityStore().AddGrant(RunGrant, GrantError);
		if (RunGrantId.IsEmpty())
		{
			Session->Release(Run);
			LaunchNextQueuedMessage();
			return Refuse(400, TEXT("invalid_authority"), GrantError);
		}
	}

	// One id for the run everywhere: controller, journal, events
	// (run_id) and this HTTP resource.
	const FString RunId = Run->GetRunId();
	Agent->SetRunIdForNextRun(RunId);
	FKismetAIExternalRunRef Record = MakeShared<FKismetAIExternalRunRecord, ESPMode::ThreadSafe>(
		RunId, ClientRequestId, Fingerprint, RunSessionId, FKismetAIExternalRunRecord::FLimits());
	Record->SetControllerRun(Run);
	ExternalRuns->Add(Record);

	// The run's game-thread end: diff, grant, lease, then the one terminal
	// outcome. Also the path for a worker the controller refused.
	TWeakPtr<uint8> WeakLife = Lifeline;
	auto Finalize = [this, WeakLife, Run, Record, RunSessionId, RunGrantId](const FString& ResultSessionId, const FString& FallbackStatus)
	{
		check(IsInGameThread());
		const FString FinalSessionId = ResultSessionId.IsEmpty() ? RunSessionId : ResultSessionId;
		Record->SetSessionId(FinalSessionId);
		if (WeakLife.IsValid())
		{
			// Keep the module session id pinned to the one this run used, then
			// finalize the diff, as the panel path does after its run.
			Session->SetSessionId(FinalSessionId);
			ComputeDiffAndMarkNodes();
			if (!RunGrantId.IsEmpty())
			{
				GetAuthorityStore().Revoke(RunGrantId, TEXT("agent-run-ended"));
			}
			Session->Release(Run);
		}
		Record->Finish(FallbackStatus);
		if (WeakLife.IsValid())
		{
			// A panel message that queued behind this run starts now.
			LaunchNextQueuedMessage();
		}
	};

	const bool bStarted = Session->StartWorker(Run, [this, WeakLife, Agent, Record, Input, RunSessionId, bStartNewSession, Finalize]()
	{
		FOnAgentStep StepCallback;
		StepCallback.BindLambda([Record](const FString& EventType, const FString& EventJson)
		{
			Record->Append(EventType, EventJson);
		});
		const FString FinalText = Agent->RunTask(Input, StepCallback, TEXT(""), RunSessionId, bStartNewSession);
		Record->SetFinalText(FinalText);
		const FString ResultSessionId = Agent->GetSessionId();
		// Finalize on the game thread (UObject diff work). Finalize touches the
		// module only behind WeakLife; so does the wait for the editor to leave
		// a save or slow task that pumps this task.
		AsyncTask(ENamedThreads::GameThread, [this, WeakLife, Finalize, ResultSessionId]()
		{
			auto FinalizeNow = [Finalize, ResultSessionId]() { Finalize(ResultSessionId, TEXT("no_terminal_event")); };
			if (WeakLife.IsValid())
			{
				RunWhenObjectAccessAllowed(MoveTemp(FinalizeNow));
			}
			else
			{
				FinalizeNow();
			}
		});
	});
	if (!bStarted)
	{
		// Shutdown closed admission in between; the body never runs.
		Finalize(FString(), TEXT("not_started"));
		return Refuse(503, TEXT("shutting_down"), TEXT("The editor is shutting down; the run was not started."));
	}

	Out.Record = Record;
	return Out;
}

bool FKismetAIModule::HandleAgentRun(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const FExternalSubmit Submit = SubmitExternalRun(Request);
	if (Submit.ErrorStatus != 0)
	{
		SendError(OnComplete, Submit.ErrorStatus, Submit.ErrorCode, Submit.ErrorMessage);
		return true;
	}
	const FKismetAIExternalRunRef Record = Submit.Record.ToSharedRef();
	if (Submit.bExisting)
	{
		// A repeated submission attaches to the run it already started. If its
		// early events have been dropped, say so rather than replay a partial
		// history as if it were whole.
		const FKismetAIRunEventPage Probe = Record->Read(0, 1);
		if (Probe.bExpired)
		{
			KismetAIExternalRunHttp::SendErrorWithData(OnComplete, 410, TEXT("cursor_expired"),
				TEXT("This run's early events are no longer retained. Read its status, or its events from resume_after."),
				KismetAIExternalRunHttp::ExpiredData(*Record, 0, Probe));
			return true;
		}
	}
	StreamExternalRunEvents(Record, 0, OnComplete);
	return true;
}

bool FKismetAIModule::HandleAgentRunSubmit(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	using namespace KismetAIExternalRunHttp;
	const FString Rest = Request.RelativePath.GetPath();
	if (!Rest.IsEmpty() && Rest != TEXT("/"))
	{
		// The router hands unknown sub-paths to the nearest parent route; a
		// typo must not start a run.
		SendError(OnComplete, 404, TEXT("not_found"), FString::Printf(TEXT("No route for agent/runs%s"), *Rest));
		return true;
	}
	const FExternalSubmit Submit = SubmitExternalRun(Request);
	if (Submit.ErrorStatus != 0)
	{
		SendError(OnComplete, Submit.ErrorStatus, Submit.ErrorCode, Submit.ErrorMessage);
		return true;
	}
	TSharedRef<FJsonObject> Data = StatusOf(*Session, *Submit.Record);
	Data->SetBoolField(TEXT("existing"), Submit.bExisting);
	Data->SetObjectField(TEXT("links"), Links(Submit.Record->GetRunId()));
	SendData(OnComplete, Submit.bExisting ? 200 : 202, Data);
	return true;
}

bool FKismetAIModule::HandleAgentRunStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	using namespace KismetAIExternalRunHttp;
	const FString RunId = QueryParam(Request, TEXT("run_id"));
	const FString ClientRequestId = QueryParam(Request, TEXT("client_request_id"));
	if (RunId.IsEmpty() && ClientRequestId.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("missing_run_id"), TEXT("Pass 'run_id' (or 'client_request_id') as a query parameter."));
		return true;
	}
	ExternalRuns->Prune();
	const FKismetAIExternalRunPtr Record = RunId.IsEmpty()
		? ExternalRuns->FindByClientRequestId(ClientRequestId)
		: ExternalRuns->Find(RunId);
	if (!Record.IsValid())
	{
		SendError(OnComplete, 404, TEXT("run_not_found"),
			TEXT("No external run with that id is known: it never existed, was started from the panel, or finished long enough ago to be evicted."));
		return true;
	}
	SendData(OnComplete, 200, StatusOf(*Session, *Record));
	return true;
}

bool FKismetAIModule::HandleAgentRunEvents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	using namespace KismetAIExternalRunHttp;
	const FString RunId = QueryParam(Request, TEXT("run_id"));
	if (RunId.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("missing_run_id"), TEXT("Pass 'run_id' as a query parameter."));
		return true;
	}
	ExternalRuns->Prune();
	const FKismetAIExternalRunPtr Record = ExternalRuns->Find(RunId);
	if (!Record.IsValid())
	{
		SendError(OnComplete, 404, TEXT("run_not_found"),
			TEXT("No external run with that id is known: it never existed, was started from the panel, or finished long enough ago to be evicted."));
		return true;
	}

	// The cursor: the last seq the client has. SSE reconnects send it as
	// Last-Event-ID.
	FString AfterText = QueryParam(Request, TEXT("after"));
	if (AfterText.IsEmpty())
	{
		AfterText = HeaderValue(Request, TEXT("Last-Event-ID"));
	}
	int64 After = 0;
	if (!AfterText.IsEmpty() && (!AfterText.IsNumeric() || (After = FCString::Atoi64(*AfterText)) < 0))
	{
		SendError(OnComplete, 400, TEXT("invalid_cursor"), TEXT("'after' must be a non-negative event sequence number."));
		return true;
	}
	int32 Limit = DefaultEventPage;
	const FString LimitText = QueryParam(Request, TEXT("limit"));
	if (!LimitText.IsEmpty())
	{
		Limit = FMath::Clamp(FCString::Atoi(*LimitText), 1, MaxEventPage);
	}

	const FKismetAIRunEventPage Page = Record->Read(After, Limit);
	if (Page.bExpired)
	{
		SendErrorWithData(OnComplete, 410, TEXT("cursor_expired"),
			TEXT("Events after this cursor are no longer retained; the history from here is incomplete. Read status for the outcome, or continue from resume_after."),
			ExpiredData(*Record, After, Page));
		return true;
	}

	const FString StreamText = QueryParam(Request, TEXT("stream"));
	if (StreamText == TEXT("1") || StreamText.Equals(TEXT("true"), ESearchCase::IgnoreCase))
	{
		StreamExternalRunEvents(Record.ToSharedRef(), After, OnComplete);
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> EventsJson;
	for (const FKismetAIRunEvent& Event : Page.Events)
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetNumberField(TEXT("seq"), Event.Seq);
		E->SetStringField(TEXT("event"), Event.Name);
		E->SetStringField(TEXT("type"), Event.Type);
		E->SetStringField(TEXT("at"), Event.At.ToIso8601());
		TSharedPtr<FJsonObject> Data;
		if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Event.Data), Data) && Data.IsValid())
		{
			E->SetObjectField(TEXT("data"), Data);
		}
		else
		{
			E->SetStringField(TEXT("data"), Event.Data);
		}
		EventsJson.Add(MakeShared<FJsonValueObject>(E));
	}
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("run_id"), Record->GetRunId());
	Data->SetArrayField(TEXT("events"), EventsJson);
	Data->SetNumberField(TEXT("next_after"), Page.NextAfter);
	Data->SetNumberField(TEXT("first_seq"), Page.FirstRetainedSeq);
	Data->SetNumberField(TEXT("last_seq"), Page.LastSeq);
	Data->SetBoolField(TEXT("more"), Page.bMore);
	Data->SetBoolField(TEXT("finished"), Page.bFinished);
	SendData(OnComplete, 200, Data);
	return true;
}

bool FKismetAIModule::HandleAgentRunCancel(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	using namespace KismetAIExternalRunHttp;
	TSharedPtr<FJsonObject> Body = ParseRequestBody(Request);
	FString RunId;
	if (!Body.IsValid() || !Body->TryGetStringField(TEXT("run_id"), RunId) || RunId.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("missing_run_id"), TEXT("Request body must be a JSON object with 'run_id'."));
		return true;
	}
	const FKismetAIExternalRunPtr Record = ExternalRuns->Find(RunId);
	if (!Record.IsValid())
	{
		SendError(OnComplete, 404, TEXT("run_not_found"),
			TEXT("No external run with that id is known: it never existed, was started from the panel, or finished long enough ago to be evicted."));
		return true;
	}

	const FKismetAIExternalRunRecord::FCancelResult Result = Record->NoteCancel();
	const TCHAR* Ack = TEXT("requested");
	switch (Result.Ack)
	{
	case FKismetAIExternalRunRecord::ECancelAck::Requested:
		// Non-blocking: kills a model call, wakes a wait, cancels the
		// run token so no further tool call is admitted. A tool call already
		// running on the game thread finishes; nothing applied is undone.
		if (const FKismetAIRunPtr Run = Record->GetControllerRun())
		{
			Session->Cancel(Run.ToSharedRef());
		}
		break;
	case FKismetAIExternalRunRecord::ECancelAck::AlreadyRequested:
		Ack = TEXT("already_requested");
		break;
	case FKismetAIExternalRunRecord::ECancelAck::AlreadyFinished:
		Ack = TEXT("already_finished");
		break;
	}

	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("run_id"), Record->GetRunId());
	Data->SetStringField(TEXT("cancel"), Ack);
	Data->SetStringField(TEXT("phase_at_request"), Result.PhaseAtRequest);
	Data->SetStringField(TEXT("limits"),
		TEXT("Cancel stops the run between operations. A model request is aborted; a tool call already running in the editor completes and its result stands; nothing already applied is undone. Poll status for the terminal outcome."));
	Data->SetObjectField(TEXT("status"), StatusOf(*Session, *Record));
	SendData(OnComplete, Result.Ack == FKismetAIExternalRunRecord::ECancelAck::Requested ? 202 : 200, Data);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Session retrieval — GET /api/v1/agent/session?id=<session_id>
// Returns the full canonical JSONL record (every entry: session_start,
// user/assistant/tool_result messages, token_stats, error, session_meta) so a
// caller can restore the whole conversation. Pure file IO (no UObject access),
// so it runs on the HTTP worker thread without a game-thread bounce.
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIModule::HandleGetSession(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FString SessionId;
	if (Request.QueryParams.Contains(TEXT("id")))
	{
		SessionId = Request.QueryParams[TEXT("id")];
	}
	if (SessionId.IsEmpty())
	{
		SendError(OnComplete, 400, TEXT("missing_id"), TEXT("Request must include an 'id' query parameter"));
		return true;
	}

	TArray<TSharedPtr<FJsonObject>> Entries = FKismetAISessionStorage::LoadAllEntries(SessionId);

	TArray<TSharedPtr<FJsonValue>> EntriesJson;
	EntriesJson.Reserve(Entries.Num());
	for (const TSharedPtr<FJsonObject>& Entry : Entries)
	{
		EntriesJson.Add(MakeShared<FJsonValueObject>(Entry));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetNumberField(TEXT("count"), EntriesJson.Num());
	Data->SetArrayField(TEXT("entries"), EntriesJson);

	SendSuccess(OnComplete, Data);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Blueprint Overview (read_blueprint mode="overview")
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoBlueprintOverview(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const auto* Schema = GetDefault<UEdGraphSchema_K2>();
	FString Description;

	// ── Section 1: Parent Class ──────────────────────────────────────────────
	Description += FString::Printf(TEXT("## %s Overview\n\n"), *Blueprint->GetName());
	Description += TEXT("### Parent Class\n");
	if (Blueprint->ParentClass)
	{
		Description += FString::Printf(TEXT("%s (%s)\n"), *Blueprint->ParentClass->GetName(), *Blueprint->ParentClass->GetPathName());
	}
	else
	{
		Description += TEXT("None\n");
	}
	Description += TEXT("\n");

	// ── Animation Blueprint section (skeleton + graph hierarchy) ─────────────
	// No-op for non-anim Blueprints.
	AppendAnimOverview(Blueprint, Description);

	// ── Section 2: Variables ─────────────────────────────────────────────────
	int32 VarCount = 0;
	TArray<FString> VarLines;
	TArray<FString> DispatcherLines;

	for (const auto& VarDesc : Blueprint->NewVariables)
	{
		// Event dispatchers go to a separate section
		if (VarDesc.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
		{
			// Build dispatcher entry with params from delegate signature graph
			FString DispatcherLine = TEXT("- ") + VarDesc.VarName.ToString();

			FString ParamStr;
			for (UEdGraph* SigGraph : Blueprint->DelegateSignatureGraphs)
			{
				if (!SigGraph) continue;
				if (SigGraph->GetFName() != VarDesc.VarName) continue;

				// Find the entry node carrying the delegate params. A signature graph
				// built via CreateFunctionGraphTerminators uses a UK2Node_FunctionEntry;
				// older/tunnel-based graphs use a UK2Node_Tunnel with outputs.
				for (UEdGraphNode* Node : SigGraph->Nodes)
				{
					const UK2Node_EditablePinBase* EntryNode = Cast<UK2Node_FunctionEntry>(Node);
					if (!EntryNode)
					{
						const UK2Node_Tunnel* TunnelNode = Cast<UK2Node_Tunnel>(Node);
						if (!TunnelNode || !TunnelNode->bCanHaveOutputs) continue;
						EntryNode = TunnelNode;
					}

					for (const auto& PinInfo : EntryNode->UserDefinedPins)
					{
						if (!PinInfo.IsValid()) continue;
						FString TypeText = UEdGraphSchema_K2::TypeToText(PinInfo->PinType).ToString();
						if (!ParamStr.IsEmpty()) ParamStr += TEXT(", ");
						ParamStr += PinInfo->PinName.ToString() + TEXT(": ") + TypeText;
					}
					break;
				}
				break;
			}

			DispatcherLine += TEXT("(") + ParamStr + TEXT(")");
			DispatcherLines.Add(DispatcherLine);
			continue;
		}

		VarCount++;
		FString TypeText = UEdGraphSchema_K2::TypeToText(VarDesc.VarType).ToString();
		FString Line = TEXT("- ") + VarDesc.VarName.ToString() + TEXT(": ") + TypeText;

		if (!VarDesc.DefaultValue.IsEmpty())
		{
			Line += TEXT(" = ") + VarDesc.DefaultValue;
		}

		// Collect flags
		TArray<FString> Flags;
		if (VarDesc.PropertyFlags & CPF_Net)
		{
			Flags.Add(TEXT("Replicated"));
		}
		if (VarDesc.PropertyFlags & CPF_RepNotify)
		{
			Flags.Add(FString::Printf(TEXT("RepNotify=%s"), *VarDesc.RepNotifyFunc.ToString()));
		}
		if (VarDesc.PropertyFlags & CPF_ExposeOnSpawn)
		{
			Flags.Add(TEXT("ExposeOnSpawn"));
		}
		if (VarDesc.PropertyFlags & CPF_SaveGame)
		{
			Flags.Add(TEXT("SaveGame"));
		}
		if (VarDesc.PropertyFlags & CPF_BlueprintReadOnly)
		{
			Flags.Add(TEXT("BlueprintReadOnly"));
		}

		if (Flags.Num() > 0)
		{
			Line += TEXT(" [") + FString::Join(Flags, TEXT(", ")) + TEXT("]");
		}

		if (!VarDesc.Category.IsEmpty())
		{
			Line += FString::Printf(TEXT(" Category=\"%s\""), *VarDesc.Category.ToString());
		}

		VarLines.Add(Line);
	}

	Description += FString::Printf(TEXT("### Variables (%d)\n"), VarCount);
	for (const auto& Line : VarLines)
	{
		Description += Line + TEXT("\n");
	}
	Description += TEXT("\n");

	// ── Section 3: Functions ─────────────────────────────────────────────────
	int32 FuncCount = 0;
	TArray<FString> FuncLines;

	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph) continue;

		FString GraphName = Graph->GetName();
		if (GraphName.StartsWith(TEXT("__")) || GraphName.Contains(TEXT("ConstructionScript")))
		{
			continue;
		}

		FuncCount++;

		// Find entry and result nodes
		UK2Node_FunctionEntry* EntryNode = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (!EntryNode)
			{
				EntryNode = Cast<UK2Node_FunctionEntry>(Node);
			}
			if (!ResultNode)
			{
				ResultNode = Cast<UK2Node_FunctionResult>(Node);
			}
			if (EntryNode && ResultNode) break;
		}

		// Input params from entry node
		FString InputStr;
		if (EntryNode)
		{
			for (const auto& PinInfo : EntryNode->UserDefinedPins)
			{
				if (!PinInfo.IsValid()) continue;
				FString TypeText = UEdGraphSchema_K2::TypeToText(PinInfo->PinType).ToString();
				if (!InputStr.IsEmpty()) InputStr += TEXT(", ");
				InputStr += PinInfo->PinName.ToString() + TEXT(": ") + TypeText;
			}
		}

		// Output params from result node
		FString OutputStr;
		if (ResultNode)
		{
			for (const auto& PinInfo : ResultNode->UserDefinedPins)
			{
				if (!PinInfo.IsValid()) continue;
				FString TypeText = UEdGraphSchema_K2::TypeToText(PinInfo->PinType).ToString();
				if (!OutputStr.IsEmpty()) OutputStr += TEXT(", ");
				OutputStr += PinInfo->PinName.ToString() + TEXT(": ") + TypeText;
			}
		}

		// Build function signature
		FString FuncLine = TEXT("- ") + GraphName + TEXT("(") + InputStr + TEXT(")");
		if (!OutputStr.IsEmpty())
		{
			FuncLine += TEXT(" -> (") + OutputStr + TEXT(")");
		}

		// Function flags
		TArray<FString> FuncFlags;
		if (EntryNode)
		{
			int32 ExtraFlags = EntryNode->GetExtraFlags();
			if (ExtraFlags & FUNC_Protected)
			{
				FuncFlags.Add(TEXT("Protected"));
			}
			else if (ExtraFlags & FUNC_Private)
			{
				FuncFlags.Add(TEXT("Private"));
			}
			else
			{
				FuncFlags.Add(TEXT("Public"));
			}
			if (ExtraFlags & FUNC_BlueprintPure)
			{
				FuncFlags.Add(TEXT("Pure"));
			}
			if (ExtraFlags & FUNC_Const)
			{
				FuncFlags.Add(TEXT("Const"));
			}
		}

		if (FuncFlags.Num() > 0)
		{
			FuncLine += TEXT(" [") + FString::Join(FuncFlags, TEXT(", ")) + TEXT("]");
		}

		FuncLines.Add(FuncLine);
	}

	Description += FString::Printf(TEXT("### Functions (%d)\n"), FuncCount);
	for (const auto& Line : FuncLines)
	{
		Description += Line + TEXT("\n");
	}
	Description += TEXT("\n");

	// ── Section 4: Macros ────────────────────────────────────────────────────
	int32 MacroCount = Blueprint->MacroGraphs.Num();
	Description += FString::Printf(TEXT("### Macros (%d)\n"), MacroCount);

	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (!Graph) continue;

		// Find entry tunnel (bCanHaveInputs) and exit tunnel (bCanHaveOutputs)
		const UK2Node_Tunnel* EntryTunnel = nullptr;
		const UK2Node_Tunnel* ExitTunnel = nullptr;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			const auto* TunnelNode = Cast<UK2Node_Tunnel>(Node);
			if (!TunnelNode) continue;
			// Skip composite nodes — they are a subclass of UK2Node_Tunnel
			if (TunnelNode->IsA<UK2Node_Composite>()) continue;
			if (TunnelNode->bCanHaveInputs && !EntryTunnel)
			{
				EntryTunnel = TunnelNode;
			}
			if (TunnelNode->bCanHaveOutputs && !ExitTunnel)
			{
				ExitTunnel = TunnelNode;
			}
			if (EntryTunnel && ExitTunnel) break;
		}

		FString InputStr;
		if (EntryTunnel)
		{
			for (const auto& PinInfo : EntryTunnel->UserDefinedPins)
			{
				if (!PinInfo.IsValid()) continue;
				FString TypeText = UEdGraphSchema_K2::TypeToText(PinInfo->PinType).ToString();
				if (!InputStr.IsEmpty()) InputStr += TEXT(", ");
				InputStr += PinInfo->PinName.ToString() + TEXT(": ") + TypeText;
			}
		}

		FString OutputStr;
		if (ExitTunnel)
		{
			for (const auto& PinInfo : ExitTunnel->UserDefinedPins)
			{
				if (!PinInfo.IsValid()) continue;
				FString TypeText = UEdGraphSchema_K2::TypeToText(PinInfo->PinType).ToString();
				if (!OutputStr.IsEmpty()) OutputStr += TEXT(", ");
				OutputStr += PinInfo->PinName.ToString() + TEXT(": ") + TypeText;
			}
		}

		FString MacroLine = TEXT("- ") + Graph->GetName() + TEXT("(") + InputStr + TEXT(")");
		if (!OutputStr.IsEmpty())
		{
			MacroLine += TEXT(" -> (") + OutputStr + TEXT(")");
		}
		Description += MacroLine + TEXT("\n");
	}
	Description += TEXT("\n");

	// ── Section 5: Event Dispatchers ─────────────────────────────────────────
	int32 DispatcherCount = DispatcherLines.Num();
	Description += FString::Printf(TEXT("### Event Dispatchers (%d)\n"), DispatcherCount);
	for (const auto& Line : DispatcherLines)
	{
		Description += Line + TEXT("\n");
	}
	Description += TEXT("\n");

	// ── Section 6: Interfaces ────────────────────────────────────────────────
	int32 InterfaceCount = Blueprint->ImplementedInterfaces.Num();
	Description += FString::Printf(TEXT("### Interfaces (%d)\n"), InterfaceCount);
	for (const auto& InterfaceDesc : Blueprint->ImplementedInterfaces)
	{
		if (!InterfaceDesc.Interface)
		{
			continue;
		}
		FString InterfaceName = InterfaceDesc.Interface->GetName();
		FString InterfacePath = InterfaceDesc.Interface->GetPathName();
		Description += FString::Printf(TEXT("- %s (%s)\n"), *InterfaceName, *InterfacePath);
	}
	Description += TEXT("\n");

	// ── Section 7: Components ────────────────────────────────────────────────
	int32 ComponentCount = 0;
	TSet<FString> OwnComponentNames;
	Description += TEXT("### Components\n");

	// Compact " (anim: ABP=/Game/.../ABP_Hero, mode=AnimationBlueprint)" annotation for a
	// skeletal mesh component template — empty for non-skeletal components.
	auto DescribeComponentAnim = [](UActorComponent* Template) -> FString
	{
		USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Template);
		if (!SkelComp)
		{
			return FString();
		}
		TArray<FString> Parts;
		if (UClass* AnimClass = SkelComp->AnimClass)
		{
			const FString ABPPath = AnimBlueprintPathFromGeneratedClass(AnimClass);
			Parts.Add(FString::Printf(TEXT("ABP=%s"), ABPPath.IsEmpty() ? *AnimClass->GetName() : *ABPPath));
		}
		Parts.Add(FString::Printf(TEXT("mode=%s"), *AnimationModeToString(SkelComp->GetAnimationMode())));
		if (UAnimationAsset* AnimToPlay = SkelComp->AnimationData.AnimToPlay)
		{
			Parts.Add(FString::Printf(TEXT("anim=%s"), *AnimToPlay->GetPathName()));
		}
		if (USkinnedAsset* MeshAsset = SkelComp->GetSkinnedAsset())
		{
			Parts.Add(FString::Printf(TEXT("mesh=%s"), *MeshAsset->GetName()));
		}
		return FString::Printf(TEXT(" (anim: %s)"), *FString::Join(Parts, TEXT(", ")));
	};
	if (Blueprint->SimpleConstructionScript)
	{
		const TArray<USCS_Node*>& RootNodes = Blueprint->SimpleConstructionScript->GetRootNodes();
		bool bFirstRoot = true;

		TFunction<void(USCS_Node*, int32)> WalkSCS = [&](USCS_Node* Node, int32 Indent)
		{
			if (!Node || !Node->ComponentClass) return;

			ComponentCount++;
			OwnComponentNames.Add(Node->GetVariableName().ToString());
			FString Line = FString::ChrN(Indent, TEXT(' ')) + TEXT("- ");
			Line += Node->GetVariableName().ToString() + TEXT(": ");

			FString ClassName = Node->ComponentClass->GetName();
			if (ClassName.StartsWith(TEXT("U")))
			{
				ClassName = ClassName.RightChop(1);
			}
			Line += ClassName;

			if (Indent == 0 && bFirstRoot)
			{
				Line += TEXT(" [Root]");
				bFirstRoot = false;
			}

			// Skeletal mesh components: append a compact animation summary so the agent
			// can answer "which Animation Blueprint is set?" straight from the overview.
			Line += DescribeComponentAnim(Node->ComponentTemplate);

			Description += Line + TEXT("\n");

			for (USCS_Node* Child : Node->GetChildNodes())
			{
				WalkSCS(Child, Indent + 2);
			}
		};

		for (USCS_Node* RootNode : RootNodes)
		{
			WalkSCS(RootNode, 0);
		}
	}

	// Inherited components (from a C++ parent class or a parent Blueprint) are
	// NOT in this Blueprint's SimpleConstructionScript — they live as UObject
	// properties on the parent class. The editor shows them in the Components
	// panel and lets you drag them into a graph to make a getter; surface them
	// here too (independent of BlueprintReadWrite/visibility) so the agent knows
	// they exist and can reference one with insert_node "Get <Name>" instead of
	// resorting to GetComponentByClass + Cast.
	if (UClass* ParentClass = Blueprint->ParentClass)
	{
		for (TFieldIterator<FObjectProperty> It(ParentClass); It; ++It)
		{
			FObjectProperty* Prop = *It;
			if (!Prop || !Prop->PropertyClass) continue;
			if (!Prop->PropertyClass->IsChildOf(UActorComponent::StaticClass())) continue;

			const FString VarName = Prop->GetName();
			if (OwnComponentNames.Contains(VarName)) continue;

			ComponentCount++;
			FString ClassName = Prop->PropertyClass->GetName();
			if (ClassName.StartsWith(TEXT("U")) || ClassName.StartsWith(TEXT("A")))
			{
				ClassName = ClassName.RightChop(1);
			}

			// Resolve the inherited component's effective default template (own override
			// or native CDO subobject) so the anim summary reflects what this Blueprint
			// actually uses, not the class default.
			FString InhSource;
			UActorComponent* InhTemplate = ResolveComponentTemplate(Blueprint, VarName, InhSource, /*bForWrite=*/false);
			Description += FString::Printf(TEXT("- %s: %s [Inherited]%s\n"), *VarName, *ClassName, *DescribeComponentAnim(InhTemplate));
		}
	}
	Description += TEXT("\n");

	// ── Section 8: Graphs summary ────────────────────────────────────────────
	Description += TEXT("### Graphs\n");

	// EventGraph(s)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (!Graph) continue;

		TArray<FString> EventNames;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (const auto* CustomEvent = Cast<UK2Node_CustomEvent>(Node))
			{
				EventNames.Add(TEXT("Custom Event ") + CustomEvent->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
			else if (const auto* EventNode = Cast<UK2Node_Event>(Node))
			{
				EventNames.Add(TEXT("Event ") + EventNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
			}
		}

		FString GraphLine = TEXT("- ") + Graph->GetName() + TEXT(": ");
		if (EventNames.Num() > 0)
		{
			GraphLine += FString::Join(EventNames, TEXT(", "));
		}
		else
		{
			GraphLine += TEXT("(empty)");
		}
		Description += GraphLine + TEXT("\n");
	}

	// Functions
	{
		TArray<FString> FuncNames;
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (!Graph) continue;
			FString GraphName = Graph->GetName();
			if (GraphName.StartsWith(TEXT("__")) || GraphName.Contains(TEXT("ConstructionScript")))
			{
				continue;
			}
			FuncNames.Add(GraphName);
		}
		if (FuncNames.Num() > 0)
		{
			Description += TEXT("- Functions: ") + FString::Join(FuncNames, TEXT(", ")) + TEXT("\n");
		}
	}

	// Macros
	{
		TArray<FString> MacroNames;
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (!Graph) continue;
			MacroNames.Add(Graph->GetName());
		}
		if (MacroNames.Num() > 0)
		{
			Description += TEXT("- Macros: ") + FString::Join(MacroNames, TEXT(", ")) + TEXT("\n");
		}
	}

	// ── Build response ───────────────────────────────────────────────────────
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("description"), Description);
	Data->SetStringField(TEXT("blueprint"), AssetPath);
	Data->SetNumberField(TEXT("variable_count"), VarCount);
	Data->SetNumberField(TEXT("function_count"), FuncCount);
	Data->SetNumberField(TEXT("macro_count"), MacroCount);
	Data->SetNumberField(TEXT("dispatcher_count"), DispatcherCount);
	Data->SetNumberField(TEXT("interface_count"), InterfaceCount);
	Data->SetNumberField(TEXT("component_count"), ComponentCount);

	return FToolExecutionResult::Success(Data);
}


// ─────────────────────────────────────────────────────────────────────────────
// Manage Blueprint (structural operations)
// ─────────────────────────────────────────────────────────────────────────────

static FString BlueprintObjectPathFromPackagePath(const FString& AssetPath)
{
	if (!AssetPath.StartsWith(TEXT("/Game/")) || AssetPath.Contains(TEXT(".")))
	{
		return AssetPath;
	}

	FString AssetName;
	int32 SlashIndex = INDEX_NONE;
	if (AssetPath.FindLastChar(TEXT('/'), SlashIndex))
	{
		AssetName = AssetPath.Mid(SlashIndex + 1);
	}
	return AssetName.IsEmpty() ? AssetPath : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
}

static UClass* ResolveInterfaceClassForManageBlueprint(const FString& InterfacePath)
{
	UClass* InterfaceClass = FindObject<UClass>(nullptr, *InterfacePath);
	if (InterfaceClass)
	{
		return InterfaceClass;
	}

	FString ObjectPath = BlueprintObjectPathFromPackagePath(InterfacePath);
	FString ClassPath = ObjectPath.EndsWith(TEXT("_C")) ? ObjectPath : ObjectPath + TEXT("_C");
	InterfaceClass = StaticLoadClass(UInterface::StaticClass(), nullptr, *ClassPath);
	if (InterfaceClass)
	{
		return InterfaceClass;
	}

	InterfaceClass = StaticLoadClass(UInterface::StaticClass(), nullptr, *InterfacePath);
	if (InterfaceClass)
	{
		return InterfaceClass;
	}

	UObject* LoadedObject = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *InterfacePath);
	if (!LoadedObject && ObjectPath != InterfacePath)
	{
		LoadedObject = StaticLoadObject(UBlueprint::StaticClass(), nullptr, *ObjectPath);
	}

	if (UBlueprint* InterfaceBlueprint = Cast<UBlueprint>(LoadedObject))
	{
		return InterfaceBlueprint->GeneratedClass;
	}

	return nullptr;
}

// Ensure a variable's RepNotify handler exists. Real UE auto-creates an
// OnRep_<Var> function when a variable is flipped to RepNotify; the plain
// SetBlueprintVariableRepNotifyFunc only records the name. Creates an empty
// function graph if one with the target name does not already exist. Returns the
// function name actually bound.
static FString EnsureRepNotifyFunction(UBlueprint* Blueprint, const FString& VarName, const FString& RequestedFunc)
{
	FString FuncName = RequestedFunc;
	if (FuncName.IsEmpty())
	{
		FuncName = FString::Printf(TEXT("OnRep_%s"), *VarName);
	}

	bool bExists = false;
	for (UEdGraph* G : Blueprint->FunctionGraphs)
	{
		if (G && G->GetName() == FuncName) { bExists = true; break; }
	}
	if (!bExists)
	{
		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!FuncGraph)
		{
			// Couldn't create the graph — don't bind a RepNotify handler that has no body.
			return FuncName;
		}
		FBlueprintEditorUtils::AddFunctionGraph(Blueprint, FuncGraph, /*bIsUserCreated=*/true, (UClass*)nullptr);
	}
	FBlueprintEditorUtils::SetBlueprintVariableRepNotifyFunc(Blueprint, FName(*VarName), FName(*FuncName));
	return FuncName;
}

// Resolve a class value for a class-typed class-default property (e.g. a Game
// Mode's DefaultPawnClass). Accepts a full class path (/Script/Engine.DefaultPawn
// or /Game/BP_Pawn.BP_Pawn_C), a Blueprint asset path/name (/Game/BP_Pawn or
// BP_Pawn → its generated class). Returns nullptr for empty (a valid clear) or
// when nothing resolves; the caller distinguishes the two.
static UClass* ResolveClassDefaultValue(const FString& Value)
{
	if (Value.IsEmpty()) return nullptr;
	if (UClass* C = FindObject<UClass>(nullptr, *Value)) return C;
	if (UClass* C = StaticLoadClass(UObject::StaticClass(), nullptr, *Value)) return C;

	// Bare short name (e.g. "PlayerController", "SpectatorPawn"): try the Engine
	// module — where the common GameMode classes live — then a global type search.
	if (!Value.Contains(TEXT(".")) && !Value.Contains(TEXT("/")))
	{
		const FString EnginePath = FString(TEXT("/Script/Engine.")) + Value;
		if (UClass* C = FindObject<UClass>(nullptr, *EnginePath)) return C;
		if (UClass* C = StaticLoadClass(UObject::StaticClass(), nullptr, *EnginePath)) return C;
		if (UClass* C = UClass::TryFindTypeSlow<UClass>(Value)) return C;
	}

	if (UBlueprint* BP = KismetAIUtils::FindBlueprint(Value))
	{
		if (BP->GeneratedClass) return BP->GeneratedClass;
	}
	if (!Value.EndsWith(TEXT("_C")))
	{
		if (UClass* C = StaticLoadClass(UObject::StaticClass(), nullptr, *(Value + TEXT("_C")))) return C;
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Widget Blueprint (UMG) read handlers — Designer hierarchy, not the event graph.
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIModule::HandleDescribeWidget(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_widget"), TEXT("tree"), FKismetAICallerContext::InProcess(), Request, OnComplete);
}


bool FKismetAIModule::HandleWidgetOverview(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_widget"), TEXT("overview"), FKismetAICallerContext::InProcess(), Request, OnComplete);
}


bool FKismetAIModule::HandleEditWidget(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("edit_widget"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


FToolExecutionResult FKismetAIModule::DoManageBlueprint(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsArray;
	if (!Body->TryGetArrayField(TEXT("operations"), OpsArray))
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'operations' array is required"));
	}

	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "ManageBlueprint", "Kismet: Manage Blueprint"));
	Blueprint->Modify();

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 ErrorCount = 0;
	bool bAnyMutation = false;

	for (const TSharedPtr<FJsonValue>& OpVal : *OpsArray)
	{
		TSharedPtr<FJsonObject> Op = OpVal->AsObject();
		if (!Op) continue;

		FString Action;
		if (!Op->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
		{
			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
			ResultObj->SetStringField(TEXT("error"), TEXT("'action' field is required"));
			ErrorCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("action"), Action);

		bAnyMutation |= (Action != TEXT("get_class_default"));

		// ── add_variable ──────────────────────────────────────────────
		if (Action == TEXT("add_variable"))
		{
			FString VarName = Op->GetStringField(TEXT("name"));
			FString TypeStr = Op->GetStringField(TEXT("type"));

			if (VarName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for add_variable"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			FString ParseError;
			FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TypeStr, &ParseError);
			if (!ParseError.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_TYPE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to parse type '%s': %s"), *TypeStr, *ParseError));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			FString DefaultValue;
			Op->TryGetStringField(TEXT("default_value"), DefaultValue);

			bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*VarName), PinType, DefaultValue);
			if (!bAdded)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("VARIABLE_ALREADY_EXISTS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable '%s' already exists or could not be added"), *VarName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Apply optional category
			FString Category;
			if (Op->TryGetStringField(TEXT("category"), Category))
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*VarName), nullptr, FText::FromString(Category));
			}

			// Apply replication flags
			FString Replication;
			Op->TryGetStringField(TEXT("replication"), Replication);
			if (Replication == TEXT("Replicated") || Replication == TEXT("RepNotify"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags) *Flags |= CPF_Net;
				if (Replication == TEXT("RepNotify"))
				{
					if (Flags) *Flags |= CPF_RepNotify;
					FString RepFunc;
					Op->TryGetStringField(TEXT("rep_notify_func"), RepFunc);
					// Auto-create the OnRep_<Var> handler graph like the editor does.
					EnsureRepNotifyFunction(Blueprint, VarName, RepFunc);
				}
			}

			// Apply expose_on_spawn
			bool bExposeOnSpawn = false;
			if (Op->TryGetBoolField(TEXT("expose_on_spawn"), bExposeOnSpawn) && bExposeOnSpawn)
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags) *Flags |= CPF_ExposeOnSpawn;
			}

			// Apply save_game
			bool bSaveGame = false;
			if (Op->TryGetBoolField(TEXT("save_game"), bSaveGame) && bSaveGame)
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags) *Flags |= CPF_SaveGame;
			}

			// Apply blueprint_read_only
			bool bReadOnly = false;
			if (Op->TryGetBoolField(TEXT("blueprint_read_only"), bReadOnly) && bReadOnly)
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags) *Flags |= CPF_BlueprintReadOnly;
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("variable"), VarName);
			SuccessCount++;
		}
		// ── remove_variable ───────────────────────────────────────────
		else if (Action == TEXT("remove_variable"))
		{
			FString VarName = Op->GetStringField(TEXT("name"));
			if (VarName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for remove_variable"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*VarName));
			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("variable"), VarName);
			SuccessCount++;
		}
		// ── set_variable_property ─────────────────────────────────────
		else if (Action == TEXT("set_variable_property"))
		{
			FString VarName = Op->GetStringField(TEXT("name"));
			FString Property = Op->GetStringField(TEXT("property"));
			FString Value = Op->GetStringField(TEXT("value"));

			if (VarName.IsEmpty() || Property.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' and 'property' fields are required for set_variable_property"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			if (Property == TEXT("category"))
			{
				FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, FName(*VarName), nullptr, FText::FromString(Value));
			}
			else if (Property == TEXT("replication"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags)
				{
					// Clear existing replication flags first
					*Flags &= ~(CPF_Net | CPF_RepNotify);
					if (Value == TEXT("Replicated") || Value == TEXT("RepNotify"))
					{
						*Flags |= CPF_Net;
						if (Value == TEXT("RepNotify"))
						{
							*Flags |= CPF_RepNotify;
							// Auto-create the OnRep_<Var> handler graph like the editor does.
							FString RepFunc;
							Op->TryGetStringField(TEXT("rep_notify_func"), RepFunc);
							EnsureRepNotifyFunction(Blueprint, VarName, RepFunc);
						}
					}
				}
			}
			else if (Property == TEXT("expose_on_spawn"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags)
				{
					if (Value == TEXT("true") || Value == TEXT("1"))
						*Flags |= CPF_ExposeOnSpawn;
					else
						*Flags &= ~CPF_ExposeOnSpawn;
				}
			}
			else if (Property == TEXT("save_game"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags)
				{
					if (Value == TEXT("true") || Value == TEXT("1"))
						*Flags |= CPF_SaveGame;
					else
						*Flags &= ~CPF_SaveGame;
				}
			}
			else if (Property == TEXT("blueprint_read_only"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags)
				{
					if (Value == TEXT("true") || Value == TEXT("1"))
						*Flags |= CPF_BlueprintReadOnly;
					else
						*Flags &= ~CPF_BlueprintReadOnly;
				}
			}
			else if (Property == TEXT("instance_editable"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags)
				{
					// "Instance Editable" = editable and not locked per-instance.
					if (Value == TEXT("true") || Value == TEXT("1"))
					{
						*Flags |= CPF_Edit;
						*Flags &= ~CPF_DisableEditOnInstance;
					}
					else
					{
						*Flags |= CPF_DisableEditOnInstance;
					}
				}
			}
			else if (Property == TEXT("transient"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags)
				{
					if (Value == TEXT("true") || Value == TEXT("1"))
						*Flags |= CPF_Transient;
					else
						*Flags &= ~CPF_Transient;
				}
			}
			else if (Property == TEXT("config"))
			{
				uint64* Flags = FBlueprintEditorUtils::GetBlueprintVariablePropertyFlags(Blueprint, FName(*VarName));
				if (Flags)
				{
					if (Value == TEXT("true") || Value == TEXT("1"))
						*Flags |= CPF_Config;
					else
						*Flags &= ~CPF_Config;
				}
			}
			else if (Property == TEXT("container_type"))
			{
				// Change the variable's container kind (none/array/set) in place,
				// preserving its element type. For map, use add_variable with the
				// map<key,value> type syntax (a single value can't carry both types).
				const FString V = Value.ToLower();
				EPinContainerType NewContainer = EPinContainerType::None;
				bool bValid = true;
				if (V == TEXT("array")) NewContainer = EPinContainerType::Array;
				else if (V == TEXT("set")) NewContainer = EPinContainerType::Set;
				else if (V == TEXT("none") || V.IsEmpty()) NewContainer = EPinContainerType::None;
				else bValid = false;

				if (!bValid)
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("container_type must be none/array/set (got '%s'); for a map, recreate the variable with type map<key,value>"), *Value));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}

				FEdGraphPinType CurrentType;
				bool bFoundVar = false;
				for (const FBPVariableDescription& Var : Blueprint->NewVariables)
				{
					if (Var.VarName == FName(*VarName)) { CurrentType = Var.VarType; bFoundVar = true; break; }
				}
				if (!bFoundVar)
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("VARIABLE_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Variable not found: %s"), *VarName));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
				CurrentType.ContainerType = NewContainer;
				FBlueprintEditorUtils::ChangeMemberVariableType(Blueprint, FName(*VarName), CurrentType);
			}
			else if (Property == TEXT("replication_condition"))
			{
				// ELifetimeCondition (COND_*) lives on the variable description, not in CPF flags.
				const int64 CondVal = StaticEnum<ELifetimeCondition>()->GetValueByNameString(Value);
				if (CondVal == INDEX_NONE)
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown replication condition '%s' (expected e.g. COND_OwnerOnly)"), *Value));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
				for (FBPVariableDescription& Var : Blueprint->NewVariables)
				{
					if (Var.VarName == FName(*VarName))
					{
						Var.ReplicationCondition = static_cast<ELifetimeCondition>(CondVal);
						break;
					}
				}
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("UNKNOWN_PROPERTY"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown variable property: %s"), *Property));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Valid properties: category, replication, replication_condition, expose_on_spawn, save_game, blueprint_read_only, instance_editable, transient, config")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("variable"), VarName);
			ResultObj->SetStringField(TEXT("property"), Property);
			SuccessCount++;
		}
		// ── add_function ──────────────────────────────────────────────
		else if (Action == TEXT("add_function"))
		{
			FString FuncName = Op->GetStringField(TEXT("name"));
			if (FuncName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for add_function"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Check if function already exists
			bool bExists = false;
			for (UEdGraph* ExistingGraph : Blueprint->FunctionGraphs)
			{
				if (ExistingGraph && ExistingGraph->GetName() == FuncName)
				{
					bExists = true;
					break;
				}
			}
			if (bExists)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("FUNCTION_ALREADY_EXISTS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Function '%s' already exists"), *FuncName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!FuncGraph)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("GRAPH_CREATE_FAILED"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not create function graph '%s'"), *FuncName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			FBlueprintEditorUtils::AddFunctionGraph(Blueprint, FuncGraph, /*bIsUserCreated=*/true, (UClass*)nullptr);

			// Find entry/result nodes
			UK2Node_FunctionEntry* EntryNode = nullptr;
			UK2Node_FunctionResult* FuncResultNode = nullptr;
			for (UEdGraphNode* Node : FuncGraph->Nodes)
			{
				if (!EntryNode) EntryNode = Cast<UK2Node_FunctionEntry>(Node);
				if (!FuncResultNode) FuncResultNode = Cast<UK2Node_FunctionResult>(Node);
			}

			// Wire entry → result exec pins
			if (EntryNode && FuncResultNode)
			{
				UEdGraphPin* EntryThenPin = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
				UEdGraphPin* ResultExecPin = FuncResultNode->FindPin(UEdGraphSchema_K2::PN_Execute);
				if (EntryThenPin && ResultExecPin)
				{
					EntryThenPin->MakeLinkTo(ResultExecPin);
				}
			}

			// Add input parameters (on entry node as outputs — they flow OUT from entry)
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (Op->TryGetArrayField(TEXT("inputs"), Inputs))
			{
				for (const auto& InputVal : *Inputs)
				{
					auto InputObj = InputVal->AsObject();
					if (!InputObj) continue;
					FString ParamName = InputObj->GetStringField(TEXT("name"));
					FString ParamType = InputObj->GetStringField(TEXT("type"));
					FString ParamError;
					FEdGraphPinType ParamPinType = KismetAIUtils::ParsePinTypeString(ParamType, &ParamError);
					if (ParamError.IsEmpty() && EntryNode)
					{
						EntryNode->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Output);
					}
				}
			}

			// Add output parameters (on result node as inputs — they flow IN to result)
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if (Op->TryGetArrayField(TEXT("outputs"), Outputs))
			{
				// Create a result node if one doesn't exist yet and we have outputs
				if (!FuncResultNode && Outputs->Num() > 0)
				{
					FGraphNodeCreator<UK2Node_FunctionResult> NodeCreator(*FuncGraph);
					FuncResultNode = NodeCreator.CreateNode();
					FuncResultNode->NodePosX = 400;
					FuncResultNode->NodePosY = 0;
					NodeCreator.Finalize();

					// Wire entry → newly created result node
					if (EntryNode)
					{
						UEdGraphPin* EntryThenPin = EntryNode->FindPin(UEdGraphSchema_K2::PN_Then);
						UEdGraphPin* ResultExecPin = FuncResultNode->FindPin(UEdGraphSchema_K2::PN_Execute);
						if (EntryThenPin && ResultExecPin)
						{
							EntryThenPin->MakeLinkTo(ResultExecPin);
						}
					}
				}

				for (const auto& OutputVal : *Outputs)
				{
					auto OutputObj = OutputVal->AsObject();
					if (!OutputObj) continue;
					FString ParamName = OutputObj->GetStringField(TEXT("name"));
					FString ParamType = OutputObj->GetStringField(TEXT("type"));
					FString ParamError;
					FEdGraphPinType ParamPinType = KismetAIUtils::ParsePinTypeString(ParamType, &ParamError);
					if (ParamError.IsEmpty() && FuncResultNode)
					{
						FuncResultNode->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Input);
					}
				}
			}

			// Set function flags
			bool bPure = false;
			if (Op->TryGetBoolField(TEXT("pure"), bPure) && bPure && EntryNode)
			{
				EntryNode->AddExtraFlags(FUNC_BlueprintPure);
			}
			bool bConst = false;
			if (Op->TryGetBoolField(TEXT("const"), bConst) && bConst && EntryNode)
			{
				EntryNode->AddExtraFlags(FUNC_Const);
			}

			// Set access specifier
			FString Access;
			Op->TryGetStringField(TEXT("access"), Access);
			if (EntryNode)
			{
				if (Access == TEXT("Protected"))
				{
					EntryNode->MetaData.SetMetaData(FName(TEXT("BlueprintProtected")), FStringView(TEXT("true")));
				}
				else if (Access == TEXT("Private"))
				{
					EntryNode->MetaData.SetMetaData(FName(TEXT("BlueprintPrivate")), FStringView(TEXT("true")));
				}
			}

			// Set category
			FString Category;
			if (Op->TryGetStringField(TEXT("category"), Category) && EntryNode)
			{
				EntryNode->MetaData.Category = FText::FromString(Category);
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("function"), FuncName);
			ResultObj->SetStringField(TEXT("graph"), FuncGraph->GetName());
			SuccessCount++;
		}
		// ── remove_function ───────────────────────────────────────────
		else if (Action == TEXT("remove_function"))
		{
			FString FuncName = Op->GetStringField(TEXT("name"));
			if (FuncName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for remove_function"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UEdGraph* FuncGraph = nullptr;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == FuncName)
				{
					FuncGraph = Graph;
					break;
				}
			}

			if (FuncGraph)
			{
				FBlueprintEditorUtils::RemoveGraph(Blueprint, FuncGraph);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("function"), FuncName);
				SuccessCount++;
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("FUNCTION_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Function graph not found: %s"), *FuncName));
				ErrorCount++;
			}
		}
		// ── add_macro ─────────────────────────────────────────────────
		else if (Action == TEXT("add_macro"))
		{
			FString MacroName = Op->GetStringField(TEXT("name"));
			if (MacroName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for add_macro"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Check if macro already exists
			bool bExists = false;
			for (UEdGraph* ExistingGraph : Blueprint->MacroGraphs)
			{
				if (ExistingGraph && ExistingGraph->GetName() == MacroName)
				{
					bExists = true;
					break;
				}
			}
			if (bExists)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MACRO_ALREADY_EXISTS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Macro '%s' already exists"), *MacroName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UEdGraph* MacroGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*MacroName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (!MacroGraph)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("GRAPH_CREATE_FAILED"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not create macro graph '%s'"), *MacroName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			FBlueprintEditorUtils::AddMacroGraph(Blueprint, MacroGraph, /*bIsUserCreated=*/true, (UClass*)nullptr);

			// Find tunnel entry/exit nodes
			UK2Node_Tunnel* EntryTunnel = nullptr;
			UK2Node_Tunnel* ExitTunnel = nullptr;
			for (UEdGraphNode* Node : MacroGraph->Nodes)
			{
				if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node))
				{
					if (Tunnel->bCanHaveInputs && !EntryTunnel)
						EntryTunnel = Tunnel;
					else if (Tunnel->bCanHaveOutputs && !ExitTunnel)
						ExitTunnel = Tunnel;
				}
			}

			// Add input pins to entry tunnel (outputs from entry = inputs to the macro)
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (Op->TryGetArrayField(TEXT("inputs"), Inputs) && EntryTunnel)
			{
				for (const auto& InputVal : *Inputs)
				{
					auto InputObj = InputVal->AsObject();
					if (!InputObj) continue;
					FString ParamName = InputObj->GetStringField(TEXT("name"));
					FString ParamType = InputObj->GetStringField(TEXT("type"));
					FString ParamError;
					FEdGraphPinType ParamPinType = KismetAIUtils::ParsePinTypeString(ParamType, &ParamError);
					if (ParamError.IsEmpty())
					{
						EntryTunnel->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Output);
					}
				}
			}

			// Add output pins to exit tunnel (inputs to exit = outputs from the macro)
			const TArray<TSharedPtr<FJsonValue>>* Outputs = nullptr;
			if (Op->TryGetArrayField(TEXT("outputs"), Outputs) && ExitTunnel)
			{
				for (const auto& OutputVal : *Outputs)
				{
					auto OutputObj = OutputVal->AsObject();
					if (!OutputObj) continue;
					FString ParamName = OutputObj->GetStringField(TEXT("name"));
					FString ParamType = OutputObj->GetStringField(TEXT("type"));
					FString ParamError;
					FEdGraphPinType ParamPinType = KismetAIUtils::ParsePinTypeString(ParamType, &ParamError);
					if (ParamError.IsEmpty())
					{
						ExitTunnel->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Input);
					}
				}
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("macro"), MacroName);
			ResultObj->SetStringField(TEXT("graph"), MacroGraph->GetName());
			SuccessCount++;
		}
		// ── remove_macro ──────────────────────────────────────────────
		else if (Action == TEXT("remove_macro"))
		{
			FString MacroName = Op->GetStringField(TEXT("name"));
			if (MacroName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for remove_macro"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UEdGraph* MacroGraph = nullptr;
			for (UEdGraph* Graph : Blueprint->MacroGraphs)
			{
				if (Graph && Graph->GetName() == MacroName)
				{
					MacroGraph = Graph;
					break;
				}
			}

			if (MacroGraph)
			{
				FBlueprintEditorUtils::RemoveGraph(Blueprint, MacroGraph);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("macro"), MacroName);
				SuccessCount++;
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MACRO_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Macro graph not found: %s"), *MacroName));
				ErrorCount++;
			}
		}
		// ── add_custom_event ──────────────────────────────────────────
		else if (Action == TEXT("add_custom_event"))
		{
			FString EventName = Op->GetStringField(TEXT("name"));
			if (EventName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for add_custom_event"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			FString GraphName;
			Op->TryGetStringField(TEXT("graph"), GraphName);
			if (GraphName.IsEmpty()) GraphName = TEXT("EventGraph");

			UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName);
			if (!Graph)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("GRAPH_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
			EventNode->CustomFunctionName = FName(*EventName);
			EventNode->SetFlags(RF_Transactional);
			EventNode->CreateNewGuid();
			EventNode->AllocateDefaultPins();
			Graph->AddNode(EventNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			EventNode->PostPlacedNewNode();

			// Add parameters
			const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
			if (Op->TryGetArrayField(TEXT("params"), Params))
			{
				for (const auto& ParamVal : *Params)
				{
					auto ParamObj = ParamVal->AsObject();
					if (!ParamObj) continue;
					FString ParamName = ParamObj->GetStringField(TEXT("name"));
					FString ParamType = ParamObj->GetStringField(TEXT("type"));
					FString ParamError;
					FEdGraphPinType ParamPinType = KismetAIUtils::ParsePinTypeString(ParamType, &ParamError);
					if (ParamError.IsEmpty())
					{
						EventNode->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Output);
					}
				}
			}

			// Set replication
			FString Replication;
			Op->TryGetStringField(TEXT("replication"), Replication);
			if (Replication == TEXT("Server"))
			{
				EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetServer);
			}
			else if (Replication == TEXT("Client"))
			{
				EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetClient);
			}
			else if (Replication == TEXT("NetMulticast"))
			{
				EventNode->FunctionFlags |= (FUNC_Net | FUNC_NetMulticast);
			}

			bool bReliable = false;
			if (Op->TryGetBoolField(TEXT("reliable"), bReliable) && bReliable)
			{
				EventNode->FunctionFlags |= FUNC_NetReliable;
			}

			bool bWithValidation = false;
			if (Op->TryGetBoolField(TEXT("with_validation"), bWithValidation) && bWithValidation)
			{
				EventNode->FunctionFlags |= FUNC_NetValidate;
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("event"), EventName);
			ResultObj->SetStringField(TEXT("node"), EventNode->GetName());
			SuccessCount++;
		}
		// ── add_function_param ────────────────────────────────────────
		else if (Action == TEXT("add_function_param"))
		{
			FString FuncName = Op->GetStringField(TEXT("function"));
			FString ParamName = Op->GetStringField(TEXT("param_name"));
			FString TypeStr = Op->GetStringField(TEXT("type"));
			FString Direction; Op->TryGetStringField(TEXT("direction"), Direction);
			if (Direction.IsEmpty()) Direction = TEXT("input");

			// Find function graph
			UEdGraph* FuncGraph = nullptr;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == FuncName) { FuncGraph = Graph; break; }
			}
			if (!FuncGraph)
			{
				// Also check macros
				for (UEdGraph* Graph : Blueprint->MacroGraphs)
				{
					if (Graph && Graph->GetName() == FuncName) { FuncGraph = Graph; break; }
				}
			}
			if (!FuncGraph)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("FUNCTION_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Function/macro '%s' not found"), *FuncName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			FString TypeError;
			FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(TypeStr, &TypeError);
			if (!TypeError.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_TYPE"));
				ResultObj->SetStringField(TEXT("error"), TypeError);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			bool bIsOutput = (Direction.ToLower() == TEXT("output") || Direction.ToLower() == TEXT("out"));

			if (bIsOutput)
			{
				// Find result node
				UK2Node_FunctionResult* ResultNode = nullptr;
				for (UEdGraphNode* Node : FuncGraph->Nodes)
				{
					ResultNode = Cast<UK2Node_FunctionResult>(Node);
					if (ResultNode) break;
				}
				if (!ResultNode)
				{
					// Create a result node if none exists
					FGraphNodeCreator<UK2Node_FunctionResult> NodeCreator(*FuncGraph);
					ResultNode = NodeCreator.CreateNode();
					ResultNode->NodePosX = 400;
					ResultNode->NodePosY = 0;
					NodeCreator.Finalize();
				}
				ResultNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Input);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("direction"), TEXT("output"));
			}
			else
			{
				// Find entry node
				UK2Node_FunctionEntry* EntryNode = nullptr;
				for (UEdGraphNode* Node : FuncGraph->Nodes)
				{
					EntryNode = Cast<UK2Node_FunctionEntry>(Node);
					if (EntryNode) break;
				}
				if (!EntryNode)
				{
					// Try tunnel entry for macros
					UK2Node_Tunnel* TunnelEntry = nullptr;
					for (UEdGraphNode* Node : FuncGraph->Nodes)
					{
						UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
						if (Tunnel && Tunnel->bCanHaveInputs && !Cast<UK2Node_Composite>(Tunnel))
						{
							TunnelEntry = Tunnel;
							break;
						}
					}
					if (TunnelEntry)
					{
						TunnelEntry->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);
						ResultObj->SetBoolField(TEXT("success"), true);
						ResultObj->SetStringField(TEXT("direction"), TEXT("input"));
						ResultObj->SetStringField(TEXT("param"), ParamName);
						SuccessCount++;
						ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
						continue;
					}
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("ENTRY_NODE_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), TEXT("No function entry node found"));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
				EntryNode->CreateUserDefinedPin(FName(*ParamName), PinType, EGPD_Output);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("direction"), TEXT("input"));
			}
			ResultObj->SetStringField(TEXT("param"), ParamName);
			ResultObj->SetStringField(TEXT("function"), FuncName);
			SuccessCount++;
		}
		// ── remove_function_param ─────────────────────────────────────
		else if (Action == TEXT("remove_function_param"))
		{
			FString FuncName = Op->GetStringField(TEXT("function"));
			FString ParamName = Op->GetStringField(TEXT("param_name"));

			UEdGraph* FuncGraph = nullptr;
			for (UEdGraph* Graph : Blueprint->FunctionGraphs)
			{
				if (Graph && Graph->GetName() == FuncName) { FuncGraph = Graph; break; }
			}
			if (!FuncGraph)
			{
				for (UEdGraph* Graph : Blueprint->MacroGraphs)
				{
					if (Graph && Graph->GetName() == FuncName) { FuncGraph = Graph; break; }
				}
			}
			if (!FuncGraph)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("FUNCTION_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Function/macro '%s' not found"), *FuncName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Search entry and result nodes for the pin
			bool bRemoved = false;
			for (UEdGraphNode* Node : FuncGraph->Nodes)
			{
				UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(Node);
				if (!EditableNode) continue;
				for (const TSharedPtr<FUserPinInfo>& PinInfo : EditableNode->UserDefinedPins)
				{
					if (PinInfo && PinInfo->PinName == FName(*ParamName))
					{
						EditableNode->RemoveUserDefinedPinByName(FName(*ParamName));
						bRemoved = true;
						break;
					}
				}
				if (bRemoved) break;
			}

			if (bRemoved)
			{
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("param"), ParamName);
				ResultObj->SetStringField(TEXT("function"), FuncName);
				SuccessCount++;
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PARAM_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Parameter '%s' not found in '%s'"), *ParamName, *FuncName));
				ErrorCount++;
			}
		}
		// ── add_event_dispatcher ──────────────────────────────────────
		else if (Action == TEXT("add_event_dispatcher"))
		{
			FString DispatcherName = Op->GetStringField(TEXT("name"));
			if (DispatcherName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for add_event_dispatcher"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			FEdGraphPinType DelegateType;
			DelegateType.PinCategory = UEdGraphSchema_K2::PC_MCDelegate;
			bool bAdded = FBlueprintEditorUtils::AddMemberVariable(Blueprint, FName(*DispatcherName), DelegateType);

			if (bAdded)
			{
				// A bare MCDelegate variable has no signature graph, so any params
				// the caller asked for would be silently dropped. Build the delegate
				// signature graph the way the editor does, then add the params to its
				// entry tunnel so they actually take effect.
				const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
				UEdGraph* SigGraph = FBlueprintEditorUtils::CreateNewGraph(
					Blueprint, FName(*DispatcherName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
				if (SigGraph)
				{
					SigGraph->bEditable = false;
					K2Schema->CreateDefaultNodesForGraph(*SigGraph);
					K2Schema->CreateFunctionGraphTerminators(*SigGraph, (UClass*)nullptr);
					K2Schema->AddExtraFunctionFlags(SigGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
					K2Schema->MarkFunctionEntryAsEditable(SigGraph, true);
					Blueprint->DelegateSignatureGraphs.Add(SigGraph);

					// Apply params to the entry tunnel (output pins = delegate args).
					int32 ParamsAdded = 0;
					const TArray<TSharedPtr<FJsonValue>>* Params = nullptr;
					if (Op->TryGetArrayField(TEXT("params"), Params))
					{
						// CreateFunctionGraphTerminators makes a UK2Node_FunctionEntry; the
						// delegate params live on it as output pins.
						UK2Node_EditablePinBase* EntryNode = nullptr;
						for (UEdGraphNode* Node : SigGraph->Nodes)
						{
							if (UK2Node_FunctionEntry* FE = Cast<UK2Node_FunctionEntry>(Node)) { EntryNode = FE; break; }
						}
						if (!EntryNode)
						{
							for (UEdGraphNode* Node : SigGraph->Nodes)
							{
								UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node);
								if (Tunnel && Tunnel->bCanHaveOutputs) { EntryNode = Tunnel; break; }
							}
						}
						if (EntryNode)
						{
							for (const auto& ParamVal : *Params)
							{
								auto ParamObj = ParamVal->AsObject();
								if (!ParamObj) continue;
								FString ParamName = ParamObj->GetStringField(TEXT("name"));
								FString ParamType = ParamObj->GetStringField(TEXT("type"));
								FString ParamError;
								FEdGraphPinType ParamPinType = KismetAIUtils::ParsePinTypeString(ParamType, &ParamError);
								if (ParamError.IsEmpty() && !ParamName.IsEmpty())
								{
									EntryNode->CreateUserDefinedPin(FName(*ParamName), ParamPinType, EGPD_Output);
									ParamsAdded++;
								}
							}
						}
					}
					ResultObj->SetNumberField(TEXT("params_added"), ParamsAdded);
				}

				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("dispatcher"), DispatcherName);
				SuccessCount++;
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("DISPATCHER_ALREADY_EXISTS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Event dispatcher '%s' already exists or could not be added"), *DispatcherName));
				ErrorCount++;
			}
		}
		// ── remove_event_dispatcher ───────────────────────────────────
		else if (Action == TEXT("remove_event_dispatcher"))
		{
			FString DispatcherName = Op->GetStringField(TEXT("name"));
			if (DispatcherName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for remove_event_dispatcher"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Remove the signature graph (if any) then the delegate member variable.
			if (UEdGraph* SigGraph = FBlueprintEditorUtils::GetDelegateSignatureGraphByName(Blueprint, FName(*DispatcherName)))
			{
				Blueprint->DelegateSignatureGraphs.Remove(SigGraph);
				FBlueprintEditorUtils::RemoveGraph(Blueprint, SigGraph);
			}
			FBlueprintEditorUtils::RemoveMemberVariable(Blueprint, FName(*DispatcherName));
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("dispatcher"), DispatcherName);
			SuccessCount++;
		}
		// ── add_interface ─────────────────────────────────────────────
		else if (Action == TEXT("add_interface"))
		{
			FString InterfacePath;
			Op->TryGetStringField(TEXT("interface"), InterfacePath);
			if (InterfacePath.IsEmpty())
			{
				Op->TryGetStringField(TEXT("interface_path"), InterfacePath);
			}
			if (InterfacePath.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'interface_path' field is required for add_interface"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Try to find the interface class — either as a native class or a Blueprint-generated class
			UClass* InterfaceClass = ResolveInterfaceClassForManageBlueprint(InterfacePath);

			if (InterfaceClass)
			{
				FTopLevelAssetPath ClassAssetPath(InterfaceClass);
				bool bSuccess = FBlueprintEditorUtils::ImplementNewInterface(Blueprint, ClassAssetPath);
				if (bSuccess)
				{
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultObj->SetStringField(TEXT("interface"), InterfacePath);
					SuccessCount++;
				}
				else
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("INTERFACE_FAILED"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to implement interface '%s' (class found but implementation failed — may already be implemented)"), *InterfacePath));
					ErrorCount++;
				}
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INTERFACE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Interface class not found: %s"), *InterfacePath));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use full class path like '/Script/Engine.UInterface' or Blueprint path like '/Game/BPI_Damageable'")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
			}
		}
		// ── remove_interface ──────────────────────────────────────────
		else if (Action == TEXT("remove_interface"))
		{
			FString InterfacePath;
			Op->TryGetStringField(TEXT("interface"), InterfacePath);
			if (InterfacePath.IsEmpty())
			{
				Op->TryGetStringField(TEXT("interface_path"), InterfacePath);
			}
			if (InterfacePath.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'interface_path' field is required for remove_interface"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Try to find the interface class to get a proper FTopLevelAssetPath
			UClass* InterfaceClass = ResolveInterfaceClassForManageBlueprint(InterfacePath);
			if (!InterfaceClass)
			{
				InterfaceClass = StaticLoadClass(UInterface::StaticClass(), nullptr, *InterfacePath);
			}

			if (InterfaceClass)
			{
				FBlueprintEditorUtils::RemoveInterface(Blueprint, FTopLevelAssetPath(InterfaceClass));
			}
			else
			{
				// Fallback: try matching by name against implemented interfaces
				bool bFound = false;
				for (int32 i = Blueprint->ImplementedInterfaces.Num() - 1; i >= 0; --i)
				{
					if (Blueprint->ImplementedInterfaces[i].Interface &&
						(Blueprint->ImplementedInterfaces[i].Interface->GetName() == InterfacePath ||
						 Blueprint->ImplementedInterfaces[i].Interface->GetPathName() == InterfacePath))
					{
						FBlueprintEditorUtils::RemoveInterface(Blueprint, FTopLevelAssetPath(Blueprint->ImplementedInterfaces[i].Interface));
						bFound = true;
						break;
					}
				}
				if (!bFound)
				{
					KISMETAI_DEV_LOG(LogKismetAI, Warning, TEXT("RemoveInterface: Could not find interface class for '%s', removal may not have effect"), *InterfacePath);
				}
			}
			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("interface"), InterfacePath);
			SuccessCount++;
		}
		// ── set_parent_class ──────────────────────────────────────────
		else if (Action == TEXT("set_parent_class"))
		{
			FString ParentClassPath = Op->GetStringField(TEXT("parent_class_path"));
			if (ParentClassPath.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'parent_class_path' field is required for set_parent_class"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UClass* NewParent = FindObject<UClass>(nullptr, *ParentClassPath);
			if (!NewParent)
			{
				NewParent = StaticLoadClass(UObject::StaticClass(), nullptr, *ParentClassPath);
			}

			if (NewParent)
			{
				Blueprint->ParentClass = NewParent;
				FBlueprintEditorUtils::RefreshAllNodes(Blueprint);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("parent_class"), NewParent->GetPathName());
				SuccessCount++;
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("CLASS_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Parent class not found: %s"), *ParentClassPath));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use full path like '/Script/Engine.Character' or '/Script/Engine.Pawn'")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
			}
		}
		// ── set_class_default ─────────────────────────────────────────
		// Set an inherited "Class Defaults" property on the Blueprint's CDO —
		// e.g. a Game Mode's DefaultPawnClass / PlayerControllerClass / HUDClass /
		// GameStateClass, or any inherited UPROPERTY shown in the Class Defaults
		// panel. These are NOT Blueprint variables (use set_variable_property for
		// declared variables). The edit is made on the current CDO; the recompile
		// below preserves it (reinstancing copies old CDO -> new CDO).
		else if (Action == TEXT("set_class_default"))
		{
			const FString PropName = Op->GetStringField(TEXT("property"));
			FString PropValue;
			Op->TryGetStringField(TEXT("value"), PropValue);

			UClass* GenClass = Blueprint->GeneratedClass;
			UObject* CDO = GenClass ? GenClass->GetDefaultObject() : nullptr;

			if (PropName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'property' field is required for set_class_default"));
				ErrorCount++;
			}
			else if (!CDO)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NO_CDO"));
				ResultObj->SetStringField(TEXT("error"), TEXT("Blueprint has no generated class yet — compile it first"));
				ErrorCount++;
			}
			else if (FProperty* Prop = GenClass->FindPropertyByName(FName(*PropName)))
			{
				CDO->Modify();
				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(CDO);

				bool bSet = false;
				FString FailDetail;
				if (Prop->IsA<FClassProperty>() || Prop->IsA<FSoftClassProperty>())
				{
					// Class-reference (TSubclassOf): resolve a class name/path.
					UClass* Resolved = ResolveClassDefaultValue(PropValue);
					UClass* MetaClass = Prop->IsA<FClassProperty>()
						? CastField<FClassProperty>(Prop)->MetaClass
						: CastField<FSoftClassProperty>(Prop)->MetaClass;
					if (!PropValue.IsEmpty() && !Resolved)
					{
						FailDetail = FString::Printf(TEXT("could not resolve class '%s'"), *PropValue);
					}
					else if (Resolved && MetaClass && !Resolved->IsChildOf(MetaClass))
					{
						FailDetail = FString::Printf(TEXT("'%s' is not a %s"), *Resolved->GetName(), *MetaClass->GetName());
					}
					else
					{
						CastFieldChecked<FObjectPropertyBase>(Prop)->SetObjectPropertyValue(ValuePtr, Resolved);
						bSet = true;
					}
				}
				else
				{
					const TCHAR* Res = Prop->ImportText_Direct(*PropValue, ValuePtr, CDO, PPF_None);
					bSet = (Res != nullptr);
					if (!bSet) FailDetail = FString::Printf(TEXT("could not parse value '%s'"), *PropValue);
				}

				if (bSet)
				{
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultObj->SetStringField(TEXT("property"), PropName);
					ResultObj->SetStringField(TEXT("value"), PropValue);
					SuccessCount++;
				}
				else
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("set_class_default '%s': %s"), *PropName, *FailDetail));
					ErrorCount++;
				}
			}
			else
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PROPERTY_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Class-default property '%s' not found on '%s'"), *PropName, *GenClass->GetName()));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use the C++ property name (e.g. DefaultPawnClass, PlayerControllerClass, HUDClass). inspect_class lists available properties.")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
			}
		}
		// ── get_class_default ─────────────────────────────────────────
		// Read counterpart to set_class_default. With 'property', exports that
		// one CDO value; without it, lists the editable defaults that differ
		// from the parent class — i.e. this Blueprint's customizations,
		// including its own variables' defaults.
		else if (Action == TEXT("get_class_default"))
		{
			FString PropName;
			Op->TryGetStringField(TEXT("property"), PropName);

			UClass* GenClass = Blueprint->GeneratedClass;
			UObject* CDO = GenClass ? GenClass->GetDefaultObject() : nullptr;

			auto ExportPropValue = [CDO](FProperty* Prop) -> FString
			{
				FString ValueText;
				Prop->ExportTextItem_Direct(ValueText, Prop->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);
				return ValueText;
			};
			auto PropCppType = [](FProperty* Prop) -> FString
			{
				FString Extended;
				FString CppType = Prop->GetCPPType(&Extended);
				return CppType + Extended;
			};

			if (!CDO)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NO_CDO"));
				ResultObj->SetStringField(TEXT("error"), TEXT("Blueprint has no generated class yet — compile it first"));
				ErrorCount++;
			}
			else if (!PropName.IsEmpty())
			{
				if (FProperty* Prop = GenClass->FindPropertyByName(FName(*PropName)))
				{
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultObj->SetStringField(TEXT("property"), PropName);
					ResultObj->SetStringField(TEXT("type"), PropCppType(Prop));
					ResultObj->SetStringField(TEXT("value"), ExportPropValue(Prop));
					SuccessCount++;
				}
				else
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("PROPERTY_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Class-default property '%s' not found on '%s'"), *PropName, *GenClass->GetName()));
					TArray<TSharedPtr<FJsonValue>> Sugg;
					Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use the C++ property name. inspect_class lists available properties; get_class_default without 'property' lists this Blueprint's modified defaults.")));
					ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
					ErrorCount++;
				}
			}
			else
			{
				UClass* ParentClass = GenClass->GetSuperClass();
				UObject* ParentCDO = ParentClass ? ParentClass->GetDefaultObject() : nullptr;
				TArray<TSharedPtr<FJsonValue>> PropsArr;
				int32 EditableCount = 0;
				for (TFieldIterator<FProperty> It(GenClass); It; ++It)
				{
					FProperty* Prop = *It;
					if (!Prop->HasAnyPropertyFlags(CPF_Edit))
					{
						continue;
					}
					EditableCount++;
					// A property the parent also has is only worth listing when this
					// class changed its value; properties declared on this Blueprint
					// (its variables) are always listed.
					UClass* OwnerClass = Prop->GetOwnerClass();
					const bool bOwnedByParent = ParentCDO && OwnerClass && ParentClass->IsChildOf(OwnerClass);
					if (bOwnedByParent && Prop->Identical_InContainer(CDO, ParentCDO))
					{
						continue;
					}
					TSharedPtr<FJsonObject> PJ = MakeShared<FJsonObject>();
					PJ->SetStringField(TEXT("property"), Prop->GetName());
					PJ->SetStringField(TEXT("type"), PropCppType(Prop));
					PJ->SetStringField(TEXT("value"), ExportPropValue(Prop));
					PJ->SetBoolField(TEXT("declared_here"), !bOwnedByParent);
					PropsArr.Add(MakeShared<FJsonValueObject>(PJ));
				}
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetArrayField(TEXT("modified_defaults"), PropsArr);
				ResultObj->SetNumberField(TEXT("editable_property_count"), EditableCount);
				ResultObj->SetStringField(TEXT("note"), TEXT("Only defaults differing from the parent class are listed; pass 'property' to read any specific one (inspect_class lists all names)."));
				SuccessCount++;
			}
		}
		// ── unknown action ────────────────────────────────────────────
		else
		{
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("UNKNOWN_ACTION"));
			ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown action: %s"), *Action));
			TArray<TSharedPtr<FJsonValue>> Sugg;
			Sugg.Add(MakeShared<FJsonValueString>(TEXT("Valid actions: add_variable, remove_variable, set_variable_property, add_function, remove_function, add_macro, remove_macro, add_custom_event, add_event_dispatcher, add_interface, remove_interface, set_parent_class, set_class_default, get_class_default")));
			ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
			ErrorCount++;
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
	}

	// Compile — skipped when every operation was a read (get_class_default mutates nothing)
	FCompilerResultsLog CompileResults;
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.PackageName = Blueprint->GetOutermost()->GetName();
	Outcome.bAppliedInMemory = bAnyMutation;
	if (bAnyMutation)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);
		RecordCompileOutcome(Outcome, Blueprint, CompileResults);

		// Persist the change so it survives an editor close without manual save
		AutoSaveAfterMutation(Blueprint, Outcome);
	}
	else
	{
		Outcome.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
		Outcome.SavePolicy = KismetAISavePolicy::AutosaveEvenIfCompileFailed;
	}

	bool bCompileSuccess = (Blueprint->Status != BS_Error);

	// Collect compile errors/warnings
	TArray<TSharedPtr<FJsonValue>> CompileErrors;
	TArray<TSharedPtr<FJsonValue>> CompileWarnings;
	for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());

		EMessageSeverity::Type Severity = Message->GetSeverity();
		if (Severity == EMessageSeverity::Error)
		{
			MsgObj->SetStringField(TEXT("severity"), TEXT("error"));
			CompileErrors.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
		else if (Severity == EMessageSeverity::Warning)
		{
			MsgObj->SetStringField(TEXT("severity"), TEXT("warning"));
			CompileWarnings.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
	}

	TSharedPtr<FJsonObject> CompilationObj = MakeShared<FJsonObject>();
	CompilationObj->SetBoolField(TEXT("success"), bCompileSuccess);
	CompilationObj->SetArrayField(TEXT("errors"), CompileErrors);
	CompilationObj->SetArrayField(TEXT("warnings"), CompileWarnings);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetBoolField(TEXT("success"), ErrorCount == 0);
	Data->SetStringField(TEXT("blueprint"), AssetPath);
	Data->SetNumberField(TEXT("operations_count"), OpsArray->Num());
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetObjectField(TEXT("compilation"), CompilationObj);

	return FToolExecutionResult::Success(Data, ErrorCount == 0 && bCompileSuccess);
}

// HTTP adapter over DoManageBlueprint. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleManageBlueprint(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("manage_blueprint"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Manage Components (SCS operations)
// ─────────────────────────────────────────────────────────────────────────────

// Find a component class by short name (without U prefix)
static UClass* FindComponentClass(const FString& ShortName)
{
	// Try /Script/Engine.U{ShortName}
	FString FullPath = FString::Printf(TEXT("/Script/Engine.U%s"), *ShortName);
	UClass* Found = FindObject<UClass>(nullptr, *FullPath);
	if (Found) return Found;

	// Try /Script/Engine.{ShortName} (without U)
	FullPath = FString::Printf(TEXT("/Script/Engine.%s"), *ShortName);
	Found = FindObject<UClass>(nullptr, *FullPath);
	if (Found) return Found;

	// Try other common modules
	static const TCHAR* OtherModules[] = {TEXT("UMG"), TEXT("Niagara"), TEXT("Paper2D"), TEXT("AIModule")};
	for (const TCHAR* Module : OtherModules)
	{
		FullPath = FString::Printf(TEXT("/Script/%s.U%s"), Module, *ShortName);
		Found = FindObject<UClass>(nullptr, *FullPath);
		if (Found) return Found;
	}

	// Try as full path if it contains /
	if (ShortName.Contains(TEXT("/")))
	{
		Found = FindObject<UClass>(nullptr, *ShortName);
		if (Found) return Found;
	}

	return nullptr;
}

// Find SCS node by variable name
static USCS_Node* FindSCSNodeByName(USimpleConstructionScript* SCS, const FString& Name)
{
	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName().ToString() == Name)
		{
			return Node;
		}
	}
	return nullptr;
}

// Resolve the editable default-template for a named component of a Blueprint. Components come
// from three places and each stores its defaults on a DIFFERENT object (mirrors what the
// Blueprint editor's details panel edits):
//   - this Blueprint's SCS           → USCS_Node::ComponentTemplate
//   - an ancestor Blueprint's SCS    → override template in this class's InheritableComponentHandler
//   - a native C++ default subobject → the component instance on this class's CDO
// OutSource reports which ("scs" / "inherited_blueprint" / "native") so results stay explainable.
// Returns nullptr when the component doesn't exist anywhere in the hierarchy.
static UActorComponent* ResolveComponentTemplate(UBlueprint* Blueprint, const FString& Name, FString& OutSource, bool bForWrite = true)
{
	OutSource.Reset();

	if (Blueprint->SimpleConstructionScript)
	{
		if (USCS_Node* Node = FindSCSNodeByName(Blueprint->SimpleConstructionScript, Name))
		{
			OutSource = TEXT("scs");
			return Node->ComponentTemplate;
		}
	}

	// Ancestor Blueprint SCS: the child Blueprint has no SCS node for it — per-child default
	// overrides live in the child class's InheritableComponentHandler, keyed by the ancestor's
	// SCS node. Create the override template on first edit (the editor does the same).
	for (UClass* C = Blueprint->ParentClass; C; C = C->GetSuperClass())
	{
		UBlueprintGeneratedClass* AncestorBPGC = Cast<UBlueprintGeneratedClass>(C);
		if (!AncestorBPGC)
		{
			break; // ran out of Blueprint ancestors — fall through to the native CDO path
		}
		UBlueprint* AncestorBP = Cast<UBlueprint>(AncestorBPGC->ClassGeneratedBy);
		USCS_Node* AncestorNode = (AncestorBP && AncestorBP->SimpleConstructionScript)
			? FindSCSNodeByName(AncestorBP->SimpleConstructionScript, Name)
			: nullptr;
		if (!AncestorNode)
		{
			continue;
		}

		// The canonical ICH lives on the UBlueprint — the compiler copies it onto the
		// generated class on every compile (KismetCompiler CleanAndSanitizeClass), so an
		// ICH created directly on the BPGC would be wiped by the recompile this handler
		// runs afterwards.
		UInheritableComponentHandler* ICH = Blueprint->GetInheritableComponentHandler(/*bCreateIfNecessary=*/bForWrite);
		UActorComponent* Template = nullptr;
		const FComponentKey Key(AncestorNode);
		if (ICH)
		{
			Template = ICH->GetOverridenComponentTemplate(Key);
		}
		if (!Template && bForWrite)
		{
			if (!ICH)
			{
				return nullptr;
			}
			ICH->Modify();
			Template = ICH->CreateOverridenComponentTemplate(Key);
		}
		if (!Template)
		{
			// Read-only path with no override yet: the effective defaults are the
			// ancestor's template — read from it without creating an override.
			Template = AncestorNode->ComponentTemplate;
		}
		OutSource = TEXT("inherited_blueprint");
		return Template;
	}

	// Native C++ component: defaults live on the generated class's CDO subobject. The agent
	// addresses it by the C++ property name (what read_blueprint's overview lists as
	// [Inherited]); the subobject's own name can differ (Character's "Mesh" property is the
	// "CharacterMesh0" subobject), so try both.
	UObject* CDO = Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject() : nullptr;
	if (CDO)
	{
		if (UActorComponent* Sub = Cast<UActorComponent>(CDO->GetDefaultSubobjectByName(FName(*Name))))
		{
			OutSource = TEXT("native");
			return Sub;
		}
		for (TFieldIterator<FObjectProperty> It(Blueprint->GeneratedClass); It; ++It)
		{
			if (!It->PropertyClass || !It->PropertyClass->IsChildOf(UActorComponent::StaticClass())) continue;
			if (!It->GetName().Equals(Name, ESearchCase::IgnoreCase)) continue;
			if (UActorComponent* Comp = Cast<UActorComponent>(It->GetObjectPropertyValue_InContainer(CDO)))
			{
				OutSource = TEXT("native");
				return Comp;
			}
		}
	}
	return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// Skeletal-mesh-component animation config (read + write helpers)
//
// A USkeletalMeshComponent stores its animation setup on the component template:
//   AnimationMode (TEnumAsByte<EAnimationMode::Type>) — Blueprint vs single-node vs custom
//   AnimClass     (TSubclassOf<UAnimInstance>)        — the AnimBP generated class
//   AnimationData.AnimToPlay (UAnimationAsset*)       — single-node-mode asset
// These same fields work on both SCS and inherited templates, so the read/write
// helpers operate on any UActorComponent that is a USkinnedMeshComponent.
// ─────────────────────────────────────────────────────────────────────────────

// Map the EAnimationMode enum to the stable string the tool exposes.
static FString AnimationModeToString(EAnimationMode::Type Mode)
{
	switch (Mode)
	{
		case EAnimationMode::AnimationBlueprint: return TEXT("AnimationBlueprint");
		case EAnimationMode::AnimationSingleNode: return TEXT("AnimationSingleNode");
		case EAnimationMode::AnimationCustomMode: return TEXT("AnimationCustomMode");
		default: return TEXT("AnimationBlueprint");
	}
}

// Parse the tool's animation_mode string back to the enum. Returns false on an
// unknown value (so the caller can emit a clear error).
static bool ParseAnimationMode(const FString& In, EAnimationMode::Type& Out)
{
	if (In == TEXT("AnimationBlueprint")) { Out = EAnimationMode::AnimationBlueprint; return true; }
	if (In == TEXT("AnimationSingleNode")) { Out = EAnimationMode::AnimationSingleNode; return true; }
	if (In == TEXT("AnimationCustomMode")) { Out = EAnimationMode::AnimationCustomMode; return true; }
	return false;
}

// Set AnimationMode on a skeletal mesh component TEMPLATE. The member is protected and
// SetAnimationMode() would try to (re)initialize a runtime anim instance — wrong for an
// unregistered template — so write the value through UE reflection, exactly as the
// details panel does, so it serializes with the Blueprint.
static void SetAnimationModeOnTemplate(USkeletalMeshComponent* SkelComp, EAnimationMode::Type Mode)
{
	if (!SkelComp) return;
	if (FProperty* Prop = SkelComp->GetClass()->FindPropertyByName(TEXT("AnimationMode")))
	{
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop))
		{
			ByteProp->SetPropertyValue_InContainer(SkelComp, static_cast<uint8>(Mode));
		}
	}
}

// Given a TSubclassOf<UAnimInstance> value (a UAnimBlueprintGeneratedClass at edit
// time), resolve the SOURCE UAnimBlueprint asset path ("/Game/.../ABP_Hero"). The
// generated class name has a trailing "_C"; the editor authors against the ABP, so
// surface that path. Returns empty when the class isn't BP-generated.
static FString AnimBlueprintPathFromGeneratedClass(UClass* AnimClass)
{
	if (!AnimClass) return FString();
	if (UAnimBlueprintGeneratedClass* BPGC = Cast<UAnimBlueprintGeneratedClass>(AnimClass))
	{
		if (UObject* GeneratedBy = BPGC->ClassGeneratedBy)
		{
			return GeneratedBy->GetPathName();
		}
	}
	return FString();
}

// Resolve an "anim_class" tool value to a UAnimBlueprintGeneratedClass. Accepts
// either the source ABP asset path ("/Game/.../ABP_Hero[.ABP_Hero]") or a generated
// class path ("/Game/.../ABP_Hero.ABP_Hero_C"). Loads the asset. Returns nullptr if
// the value resolves to nothing animation-related; OutAnimBP carries the source ABP
// (for skeleton checks) when available.
static UClass* ResolveAnimInstanceClass(const FString& Value, UAnimBlueprint*& OutAnimBP)
{
	OutAnimBP = nullptr;
	if (Value.IsEmpty()) return nullptr;

	// Try as a UAnimBlueprint asset first (the path the agent should pass).
	if (UAnimBlueprint* ABP = LoadObject<UAnimBlueprint>(nullptr, *Value))
	{
		OutAnimBP = ABP;
		return ABP->GeneratedClass;
	}
	// Package path without object suffix ("/Game/.../ABP_Hero").
	if (!Value.Contains(TEXT(".")))
	{
		FString ObjName;
		Value.Split(TEXT("/"), nullptr, &ObjName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		const FString Full = Value + TEXT(".") + ObjName;
		if (UAnimBlueprint* ABP = LoadObject<UAnimBlueprint>(nullptr, *Full))
		{
			OutAnimBP = ABP;
			return ABP->GeneratedClass;
		}
	}
	// Already a generated class path ("....ABP_Hero_C").
	if (UClass* Cls = LoadObject<UClass>(nullptr, *Value))
	{
		if (Cls->IsChildOf(UAnimInstance::StaticClass()))
		{
			OutAnimBP = Cast<UAnimBlueprint>(Cls->ClassGeneratedBy);
			return Cls;
		}
	}
	return nullptr;
}

// Fill OutObj with the animation configuration of a skinned/skeletal mesh component
// template. No-op (returns false) for non-skinned components. Read-only.
static bool EmitSkinnedMeshAnimInfo(UActorComponent* Template, const TSharedPtr<FJsonObject>& OutObj)
{
	USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Template);
	if (!Skinned || !OutObj.IsValid())
	{
		return false;
	}

	// SkeletalMesh + its skeleton (drives skeleton-compat reasoning). GetSkinnedAsset
	// covers both USkeletalMeshComponent and the base USkinnedMeshComponent on 5.6/5.7.
	USkinnedAsset* MeshAsset = Skinned->GetSkinnedAsset();
	if (MeshAsset)
	{
		OutObj->SetStringField(TEXT("skeletal_mesh"), MeshAsset->GetPathName());
		if (USkeleton* Skel = MeshAsset->GetSkeleton())
		{
			OutObj->SetStringField(TEXT("skeleton"), Skel->GetPathName());
		}
	}
	else
	{
		OutObj->SetStringField(TEXT("skeletal_mesh"), FString());
	}

	// AnimationMode + AnimClass + single-node asset live only on USkeletalMeshComponent.
	USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Template);
	if (SkelComp)
	{
		OutObj->SetStringField(TEXT("animation_mode"), AnimationModeToString(SkelComp->GetAnimationMode()));

		UClass* AnimClass = SkelComp->AnimClass;
		if (AnimClass)
		{
			OutObj->SetStringField(TEXT("anim_class_raw"), AnimClass->GetName());
			const FString ABPPath = AnimBlueprintPathFromGeneratedClass(AnimClass);
			OutObj->SetStringField(TEXT("anim_class"), ABPPath.IsEmpty() ? AnimClass->GetPathName() : ABPPath);
		}
		else
		{
			OutObj->SetStringField(TEXT("anim_class"), FString());
		}

		if (UAnimationAsset* AnimToPlay = SkelComp->AnimationData.AnimToPlay)
		{
			OutObj->SetStringField(TEXT("anim_to_play"), AnimToPlay->GetPathName());
		}
	}
	return true;
}

// Resolve a material asset value to a UMaterialInterface. Accepts a full object
// path ("/Game/M/M_Foo.M_Foo") or a package path ("/Game/M/M_Foo" → ".M_Foo").
static UMaterialInterface* ResolveMaterialAsset(const FString& Value)
{
	if (Value.IsEmpty()) return nullptr;
	if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Value)) return M;
	if (!Value.Contains(TEXT(".")))
	{
		FString Leaf;
		Value.Split(TEXT("/"), nullptr, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (!Leaf.IsEmpty())
		{
			if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *(Value + TEXT(".") + Leaf))) return M;
		}
	}
	return nullptr;
}

// Locate the FPostProcessSettings struct on a component template via reflection. Covers
// UPostProcessComponent (property "Settings"), UCameraComponent and USceneCaptureComponent2D
// (property "PostProcessSettings") without linking against each component class.
static FPostProcessSettings* FindPostProcessSettings(UActorComponent* Template, FString& OutPropName)
{
	if (!Template) return nullptr;
	for (TFieldIterator<FStructProperty> It(Template->GetClass()); It; ++It)
	{
		if (It->Struct == FPostProcessSettings::StaticStruct())
		{
			OutPropName = It->GetName();
			return It->ContainerPtrToValuePtr<FPostProcessSettings>(Template);
		}
	}
	return nullptr;
}

static bool ParseCollisionEnabled(const FString& In, ECollisionEnabled::Type& Out)
{
	FString S = In; S.RemoveFromStart(TEXT("ECollisionEnabled::"));
	if (S.Equals(TEXT("NoCollision"), ESearchCase::IgnoreCase))     { Out = ECollisionEnabled::NoCollision; return true; }
	if (S.Equals(TEXT("QueryOnly"), ESearchCase::IgnoreCase))       { Out = ECollisionEnabled::QueryOnly; return true; }
	if (S.Equals(TEXT("PhysicsOnly"), ESearchCase::IgnoreCase))     { Out = ECollisionEnabled::PhysicsOnly; return true; }
	if (S.Equals(TEXT("QueryAndPhysics"), ESearchCase::IgnoreCase)) { Out = ECollisionEnabled::QueryAndPhysics; return true; }
	return false;
}

static bool ParseCollisionResponse(const FString& In, ECollisionResponse& Out)
{
	FString S = In; S.RemoveFromStart(TEXT("ECR_"));
	if (S.Equals(TEXT("Block"), ESearchCase::IgnoreCase))   { Out = ECR_Block; return true; }
	if (S.Equals(TEXT("Overlap"), ESearchCase::IgnoreCase)) { Out = ECR_Overlap; return true; }
	if (S.Equals(TEXT("Ignore"), ESearchCase::IgnoreCase))  { Out = ECR_Ignore; return true; }
	return false;
}

// Standard built-in collision channels by name (e.g. "WorldStatic", "Pawn").
// Custom channels are best configured via a collision profile.
static bool ParseCollisionChannel(const FString& In, ECollisionChannel& Out)
{
	FString S = In; S.RemoveFromStart(TEXT("ECC_"));
	S = S.ToLower();
	if (S == TEXT("worldstatic"))  { Out = ECC_WorldStatic; return true; }
	if (S == TEXT("worlddynamic")) { Out = ECC_WorldDynamic; return true; }
	if (S == TEXT("pawn"))         { Out = ECC_Pawn; return true; }
	if (S == TEXT("visibility"))   { Out = ECC_Visibility; return true; }
	if (S == TEXT("camera"))       { Out = ECC_Camera; return true; }
	if (S == TEXT("physicsbody"))  { Out = ECC_PhysicsBody; return true; }
	if (S == TEXT("vehicle"))      { Out = ECC_Vehicle; return true; }
	if (S == TEXT("destructible")) { Out = ECC_Destructible; return true; }
	return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Route: POST /api/v1/manage_state_machine
// Author Animation Blueprint state machines: add a state machine to the anim graph, then add
// states / conduits / aliases / transitions inside it, set transition properties, set the entry
// state, rename and delete states/transitions. Anim BPs only. Batch-op, graph-addressed —
// mirrors HandleEditGraph's response shape.
// ─────────────────────────────────────────────────────────────────────────────

namespace StateMachineHelpers
{
	// All state-like nodes (states, conduits, aliases) keyed by GetStateName().
	static UAnimStateNodeBase* FindStateByName(UAnimationStateMachineGraph* SM, const FString& Name)
	{
		for (UEdGraphNode* Node : SM->Nodes)
		{
			if (UAnimStateNodeBase* S = Cast<UAnimStateNodeBase>(Node))
			{
				if (S->GetStateName() == Name) return S;
			}
		}
		return nullptr;
	}

	static bool StateNameExists(UAnimationStateMachineGraph* SM, const FString& Name)
	{
		return FindStateByName(SM, Name) != nullptr;
	}

	// Transitions matching a from->to pair, in node (creation) order. Aliases/conduits are valid
	// endpoints too (all derive from UAnimStateNodeBase).
	static TArray<UAnimStateTransitionNode*> FindTransitions(UAnimationStateMachineGraph* SM, const FString& From, const FString& To)
	{
		TArray<UAnimStateTransitionNode*> Out;
		for (UEdGraphNode* Node : SM->Nodes)
		{
			UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node);
			if (!T) continue;
			UAnimStateNodeBase* P = T->GetPreviousState();
			UAnimStateNodeBase* N = T->GetNextState();
			const FString PName = P ? P->GetStateName() : FString();
			const FString NName = N ? N->GetStateName() : FString();
			if (PName == From && NName == To) Out.Add(T);
		}
		return Out;
	}

	// Qualified bound-graph address for a state/conduit/alias node: "<SMPath>/<StateGraphName>".
	static FString BoundGraphAddress(const FString& SMPath, UAnimStateNodeBase* State)
	{
		UEdGraph* BG = State ? State->GetBoundGraph() : nullptr;
		if (!BG) return FString();
		return FString::Printf(TEXT("%s/%s"), *SMPath, *BG->GetName());
	}

	// Apply the optional transition properties present on an op JSON object to a transition node.
	// Returns the list of applied property descriptions (for the response).
	static TArray<FString> ApplyTransitionProps(UAnimStateTransitionNode* T, const TSharedPtr<FJsonObject>& Op)
	{
		TArray<FString> Applied;
		double Num = 0.0;
		bool bFlag = false;
		if (Op->TryGetNumberField(TEXT("crossfade_duration"), Num))
		{
			T->CrossfadeDuration = (float)Num;
			Applied.Add(FString::Printf(TEXT("crossfade_duration=%.3f"), T->CrossfadeDuration));
		}
		int32 Prio = 0;
		if (Op->TryGetNumberField(TEXT("priority_order"), Num))
		{
			T->PriorityOrder = (int32)Num;
			Applied.Add(FString::Printf(TEXT("priority_order=%d"), T->PriorityOrder));
		}
		FString BlendModeStr;
		if (Op->TryGetStringField(TEXT("blend_mode"), BlendModeStr) && !BlendModeStr.IsEmpty())
		{
			if (UEnum* EnumPtr = StaticEnum<EAlphaBlendOption>())
			{
				int64 Val = EnumPtr->GetValueByNameString(BlendModeStr);
				if (Val == INDEX_NONE) Val = EnumPtr->GetValueByNameString(FString(TEXT("EAlphaBlendOption::")) + BlendModeStr);
				if (Val != INDEX_NONE)
				{
					T->BlendMode = (EAlphaBlendOption)Val;
					Applied.Add(FString::Printf(TEXT("blend_mode=%s"), *BlendModeStr));
				}
			}
		}
		if (Op->TryGetBoolField(TEXT("automatic_rule"), bFlag))
		{
			T->bAutomaticRuleBasedOnSequencePlayerInState = bFlag;
			Applied.Add(FString::Printf(TEXT("automatic_rule=%s"), bFlag ? TEXT("true") : TEXT("false")));
		}
		if (Op->TryGetNumberField(TEXT("automatic_rule_trigger_time"), Num))
		{
			T->AutomaticRuleTriggerTime = (float)Num;
			Applied.Add(FString::Printf(TEXT("automatic_rule_trigger_time=%.3f"), T->AutomaticRuleTriggerTime));
		}
		if (Op->TryGetBoolField(TEXT("bidirectional"), bFlag))
		{
			T->Bidirectional = bFlag;
			Applied.Add(FString::Printf(TEXT("bidirectional=%s"), bFlag ? TEXT("true") : TEXT("false")));
		}
		if (Op->TryGetBoolField(TEXT("disabled"), bFlag))
		{
			T->bDisabled = bFlag;
			Applied.Add(FString::Printf(TEXT("disabled=%s"), bFlag ? TEXT("true") : TEXT("false")));
		}
		if (Op->TryGetNumberField(TEXT("min_time_before_reentry"), Num))
		{
#if ENGINE_MAJOR_VERSION > 5 || (ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7)
			T->MinTimeBeforeReentry = (float)Num;
			Applied.Add(FString::Printf(TEXT("min_time_before_reentry=%.3f"), T->MinTimeBeforeReentry));
#else
			// UAnimStateTransitionNode::MinTimeBeforeReentry was added in UE 5.7; unavailable on 5.6.
			Applied.Add(TEXT("min_time_before_reentry=unsupported (requires UE 5.7+)"));
#endif
		}
		return Applied;
	}
}

FToolExecutionResult FKismetAIModule::DoManageStateMachine(const TSharedPtr<FJsonObject>& Args)
{
	using namespace StateMachineHelpers;

	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(Blueprint);
	if (!AnimBP)
	{
		return FToolExecutionResult::Error(400, TEXT("ANIM_BP_REQUIRED"),
			FString::Printf(TEXT("manage_state_machine only applies to Animation Blueprints; '%s' is a %s"), *AssetPath, *Blueprint->GetClass()->GetName()),
			{ TEXT("Use this tool on a UAnimBlueprint asset (an Animation Blueprint).") });
	}

	// 'state_machine' (alias 'graph') addresses the SM graph for most ops, or the host anim graph for
	// add_state_machine. Empty defaults to the AnimGraph.
	FString SMAddress;
	if (!Body->TryGetStringField(TEXT("state_machine"), SMAddress))
	{
		Body->TryGetStringField(TEXT("graph"), SMAddress);
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsArray = nullptr;
	if (!Body->TryGetArrayField(TEXT("operations"), OpsArray) || !OpsArray || OpsArray->Num() == 0)
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'operations' must be a non-empty array"));
	}

	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);
	int32 ReadOnlySuccessCount = 0; // successful "get" ops: they count as success but change nothing
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "ManageStateMachine", "Kismet: Manage State Machine"));
	Blueprint->Modify();

	// Lazily-resolved SM graph: add_state_machine sets it; other ops resolve it from SMAddress.
	UAnimationStateMachineGraph* SMGraph = nullptr;
	FString SMPath;          // graph address of the SM graph (for building child addresses)
	bool bSMResolved = false;

	// Resolve the SM graph for the non-creating ops. Returns false + writes an error result on failure.
	auto ResolveSM = [&](TSharedPtr<FJsonObject>& ResultObj) -> bool
	{
		if (bSMResolved) return SMGraph != nullptr;
		bSMResolved = true;
		TArray<FString> Candidates;
		UEdGraph* G = KismetAIUtils::FindGraph(Blueprint, SMAddress, &Candidates);
		if (!G)
		{
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("GRAPH_NOT_FOUND"));
			ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("State machine graph not found: '%s'"), *SMAddress));
			TArray<TSharedPtr<FJsonValue>> Sugg;
			if (Candidates.Num() > 0)
				Sugg.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Ambiguous — address one by path: %s"), *FString::Join(Candidates, TEXT(", ")))));
			Sugg.Add(MakeShared<FJsonValueString>(TEXT("Address the state machine graph, e.g. \"AnimGraph/Locomotion\". Create one first with add_state_machine.")));
			ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
			return false;
		}
		SMGraph = Cast<UAnimationStateMachineGraph>(G);
		if (!SMGraph)
		{
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_A_STATE_MACHINE"));
			ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Graph '%s' is a %s, not a state machine graph"), *G->GetName(), *G->GetClass()->GetName()));
			return false;
		}
		// Build the SM path. Use FindGraph's own qualified path via CollectGraphsRecursive on the anim graph.
		SMPath = SMAddress;
		if (UEdGraph* AnimRoot = KismetAIUtils::FindAnimGraph(Blueprint))
		{
			TMap<UEdGraph*, FString> All;
			KismetAIUtils::CollectGraphsRecursive(AnimRoot, FString(), All);
			if (const FString* P = All.Find(SMGraph)) SMPath = *P;
		}
		return true;
	};

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 ErrorCount = 0;

	for (const TSharedPtr<FJsonValue>& OpVal : *OpsArray)
	{
		TSharedPtr<FJsonObject> Op = OpVal->AsObject();
		if (!Op) continue;

		FString Action;
		if (!Op->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
		{
			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
			ResultObj->SetStringField(TEXT("error"), TEXT("'action' field is required"));
			ErrorCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("action"), Action);

		// ── add_state_machine ─────────────────────────────────────────
		if (Action == TEXT("add_state_machine"))
		{
			FString Name;
			if (!Op->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' is required for add_state_machine"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Host anim graph: SMAddress (or per-op 'state_machine') addresses it; default AnimGraph.
			FString HostAddr = SMAddress;
			Op->TryGetStringField(TEXT("state_machine"), HostAddr);
			UEdGraph* HostGraph = HostAddr.IsEmpty()
				? KismetAIUtils::FindAnimGraph(Blueprint)
				: KismetAIUtils::FindGraph(Blueprint, HostAddr);
			if (!HostGraph || !Cast<UAnimationGraph>(HostGraph))
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("GRAPH_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Host anim graph not found or not an anim graph: '%s'"), *HostAddr));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			HostGraph->Modify();
			UAnimGraphNode_StateMachine* SMNode = NewObject<UAnimGraphNode_StateMachine>(HostGraph);
			HostGraph->AddNode(SMNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
			SMNode->CreateNewGuid();
			SMNode->PostPlacedNewNode();      // creates EditorStateMachineGraph + EntryNode, adds to SubGraphs
			SMNode->AllocateDefaultPins();

			UAnimationStateMachineGraph* NewSM = SMNode->EditorStateMachineGraph;
			if (!NewSM)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("CREATE_FAILED"));
				ResultObj->SetStringField(TEXT("error"), TEXT("State machine graph was not created"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			// Name the SM: rename its bound graph (keeps the node title in sync via OnRenameNode path).
			FBlueprintEditorUtils::RenameGraph(NewSM, Name);

			const FString NewSMPath = FString::Printf(TEXT("%s/%s"), *HostGraph->GetName(), *NewSM->GetName());
			// Adopt this SM as the working SM for subsequent ops that don't re-specify state_machine.
			SMGraph = NewSM;
			SMPath = NewSMPath;
			bSMResolved = true;

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("state_machine"), NewSM->GetName());
			ResultObj->SetStringField(TEXT("graph_address"), NewSMPath);
			SuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// All remaining ops operate inside an existing SM graph.
		if (!ResolveSM(ResultObj))
		{
			ErrorCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// ── get ───────────────────────────────────────────────────────
		// Return the SM structure as JSON: entry state, states (name + bound-graph address),
		// transitions (from/to, index, endpoint-form rule_graph address, custom-blend address,
		// and the full property set). Requires nothing beyond the addressed SM.
		if (Action == TEXT("get"))
		{
			using namespace AnimReadHelpers;

			// Entry → initial state.
			FString EntryState;
			if (SMGraph->EntryNode)
			{
				if (UEdGraphNode* InitNode = SMGraph->EntryNode->GetOutputNode())
				{
					if (UAnimStateNodeBase* InitState = Cast<UAnimStateNodeBase>(InitNode))
					{
						EntryState = InitState->GetStateName();
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> StatesArr;
			TArray<UAnimStateTransitionNode*> AllTransitions;
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node))
				{
					AllTransitions.Add(T);
					continue;
				}
				if (UAnimStateNodeBase* S = Cast<UAnimStateNodeBase>(Node))
				{
					TSharedPtr<FJsonObject> StateObj = MakeShared<FJsonObject>();
					StateObj->SetStringField(TEXT("name"), S->GetStateName());
					FString Kind = TEXT("state");
					if (S->IsA<UAnimStateConduitNode>()) Kind = TEXT("conduit");
					else if (S->IsA<UAnimStateAliasNode>()) Kind = TEXT("alias");
					StateObj->SetStringField(TEXT("kind"), Kind);
					if (UEdGraph* BG = S->GetBoundGraph())
					{
						StateObj->SetStringField(TEXT("bound_graph"), FString::Printf(TEXT("%s/%s"), *SMPath, *S->GetStateName()));
					}
					StatesArr.Add(MakeShared<FJsonValueObject>(StateObj));
				}
			}

			TArray<TSharedPtr<FJsonValue>> TransArr;
			for (UAnimStateTransitionNode* T : AllTransitions)
			{
				UAnimStateNodeBase* From = T->GetPreviousState();
				UAnimStateNodeBase* To = T->GetNextState();
				TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
				TObj->SetStringField(TEXT("from"), From ? From->GetStateName() : TEXT("?"));
				TObj->SetStringField(TEXT("to"), To ? To->GetStateName() : TEXT("?"));
				TObj->SetStringField(TEXT("node"), T->GetName());
				TObj->SetStringField(TEXT("rule_graph"), TransitionEndpointAddress(SMPath, T, AllTransitions));
				TObj->SetBoolField(TEXT("has_custom_blend"), T->CustomTransitionGraph != nullptr);
				if (T->CustomTransitionGraph)
				{
					TObj->SetStringField(TEXT("custom_blend_graph"), TransitionEndpointAddress(SMPath, T, AllTransitions) + TEXT("@blend"));
				}
				TObj->SetNumberField(TEXT("priority_order"), T->PriorityOrder);
				TObj->SetNumberField(TEXT("crossfade_duration"), T->CrossfadeDuration);
				if (UEnum* EnumPtr = StaticEnum<EAlphaBlendOption>())
				{
					TObj->SetStringField(TEXT("blend_mode"), EnumPtr->GetNameStringByValue((int64)T->BlendMode));
				}
				TObj->SetBoolField(TEXT("automatic_rule"), T->bAutomaticRuleBasedOnSequencePlayerInState);
				TObj->SetBoolField(TEXT("bidirectional"), T->Bidirectional);
				TObj->SetBoolField(TEXT("disabled"), T->bDisabled);
				TransArr.Add(MakeShared<FJsonValueObject>(TObj));
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("state_machine"), SMPath);
			if (!EntryState.IsEmpty()) ResultObj->SetStringField(TEXT("entry_state"), EntryState);
			ResultObj->SetArrayField(TEXT("states"), StatesArr);
			ResultObj->SetArrayField(TEXT("transitions"), TransArr);
			SuccessCount++;
			ReadOnlySuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// ── add_state / add_conduit / add_state_alias ─────────────────
		if (Action == TEXT("add_state") || Action == TEXT("add_conduit") || Action == TEXT("add_state_alias"))
		{
			FString Name;
			if (!Op->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("'name' is required for %s"), *Action));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			if (StateNameExists(SMGraph, Name))
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("STATE_EXISTS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("A state/conduit/alias named '%s' already exists in this state machine"), *Name));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			SMGraph->Modify();
			UAnimStateNodeBase* NewState = nullptr;
			if (Action == TEXT("add_state"))
			{
				NewState = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
					SMGraph, NewObject<UAnimStateNode>(SMGraph), FVector2f::ZeroVector, false);
			}
			else if (Action == TEXT("add_conduit"))
			{
				NewState = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateConduitNode>(
					SMGraph, NewObject<UAnimStateConduitNode>(SMGraph), FVector2f::ZeroVector, false);
			}
			else // add_state_alias
			{
				NewState = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateAliasNode>(
					SMGraph, NewObject<UAnimStateAliasNode>(SMGraph), FVector2f::ZeroVector, false);
			}
			if (!NewState)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("CREATE_FAILED"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to create %s"), *Action));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			// Name it via OnRenameNode (renames the node AND its bound graph so GetStateName() matches).
			NewState->OnRenameNode(Name);

			// Configure an alias's targets.
			if (UAnimStateAliasNode* Alias = Cast<UAnimStateAliasNode>(NewState))
			{
				bool bGlobal = false;
				Op->TryGetBoolField(TEXT("global"), bGlobal);
				Alias->bGlobalAlias = bGlobal;
				const TArray<TSharedPtr<FJsonValue>>* AliasedArr = nullptr;
				if (!bGlobal && Op->TryGetArrayField(TEXT("aliased_states"), AliasedArr))
				{
					TSet<TWeakObjectPtr<UAnimStateNodeBase>>& Targets = Alias->GetAliasedStates();
					Targets.Empty();
					for (const TSharedPtr<FJsonValue>& AV : *AliasedArr)
					{
						const FString AName = AV->AsString();
						if (UAnimStateNodeBase* T = FindStateByName(SMGraph, AName))
						{
							Targets.Add(T);
						}
					}
				}
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("name"), NewState->GetStateName());
			ResultObj->SetStringField(TEXT("node"), NewState->GetName());
			const FString Addr = BoundGraphAddress(SMPath, NewState);
			if (!Addr.IsEmpty()) ResultObj->SetStringField(TEXT("graph_address"), Addr);
			SuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// ── add_transition ────────────────────────────────────────────
		if (Action == TEXT("add_transition"))
		{
			FString From, To;
			Op->TryGetStringField(TEXT("from"), From);
			Op->TryGetStringField(TEXT("to"), To);
			if (From.IsEmpty() || To.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'from' and 'to' are required for add_transition"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			UAnimStateNodeBase* FromState = FindStateByName(SMGraph, From);
			UAnimStateNodeBase* ToState = FindStateByName(SMGraph, To);
			if (!FromState || !ToState)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("STATE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Transition endpoint(s) not found: %s%s"),
					FromState ? TEXT("") : *FString::Printf(TEXT("from '%s' "), *From),
					ToState ? TEXT("") : *FString::Printf(TEXT("to '%s'"), *To)));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			SMGraph->Modify();
			UAnimStateTransitionNode* Trans = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
				SMGraph, NewObject<UAnimStateTransitionNode>(SMGraph), FVector2f::ZeroVector, false);
			if (!Trans)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("CREATE_FAILED"));
				ResultObj->SetStringField(TEXT("error"), TEXT("Failed to create transition"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			Trans->CreateConnections(FromState, ToState);
			TArray<FString> Applied = ApplyTransitionProps(Trans, Op);

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("from"), From);
			ResultObj->SetStringField(TEXT("to"), To);
			ResultObj->SetStringField(TEXT("transition"), Trans->GetName());
			if (Trans->BoundGraph)
				ResultObj->SetStringField(TEXT("graph_address"), FString::Printf(TEXT("%s/%s"), *SMPath, *Trans->BoundGraph->GetName()));
			// Endpoint-form address (stable across opaque graph renames): "<SMPath>/From->To".
			{
				TArray<UAnimStateTransitionNode*> AllT;
				for (UEdGraphNode* N : SMGraph->Nodes)
					if (UAnimStateTransitionNode* TT = Cast<UAnimStateTransitionNode>(N)) AllT.Add(TT);
				const FString EndpointAddr = AnimReadHelpers::TransitionEndpointAddress(SMPath, Trans, AllT);
				if (!EndpointAddr.IsEmpty())
					ResultObj->SetStringField(TEXT("rule_graph_address"), EndpointAddr);
			}
			if (Applied.Num() > 0)
				ResultObj->SetStringField(TEXT("properties_applied"), FString::Join(Applied, TEXT(", ")));
			SuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// ── set_transition_properties / delete_transition (share transition lookup) ──
		if (Action == TEXT("set_transition_properties") || Action == TEXT("delete_transition"))
		{
			// Locate the transition either by exact node name or by from/to[+index].
			UAnimStateTransitionNode* Trans = nullptr;
			FString TransName;
			if (Op->TryGetStringField(TEXT("transition"), TransName) && !TransName.IsEmpty())
			{
				for (UEdGraphNode* Node : SMGraph->Nodes)
				{
					if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node))
					{
						if (T->GetName() == TransName) { Trans = T; break; }
					}
				}
				if (!Trans)
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("TRANSITION_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("No transition node named '%s'"), *TransName));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
			}
			else
			{
				FString From, To;
				Op->TryGetStringField(TEXT("from"), From);
				Op->TryGetStringField(TEXT("to"), To);
				if (From.IsEmpty() || To.IsEmpty())
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("%s needs either 'transition' or both 'from' and 'to'"), *Action));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
				TArray<UAnimStateTransitionNode*> Matches = FindTransitions(SMGraph, From, To);
				int32 Index = 0;
				double IdxNum = 0.0;
				if (Op->TryGetNumberField(TEXT("index"), IdxNum)) Index = (int32)IdxNum;
				if (!Matches.IsValidIndex(Index))
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("TRANSITION_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("No transition %s -> %s at index %d (found %d)"), *From, *To, Index, Matches.Num()));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
				Trans = Matches[Index];
			}

			if (Action == TEXT("delete_transition"))
			{
				const FString Removed = Trans->GetName();
				FBlueprintEditorUtils::RemoveNode(Blueprint, Trans, /*bDontRecompile=*/true);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("deleted_transition"), Removed);
				SuccessCount++;
			}
			else // set_transition_properties
			{
				Trans->Modify();
				TArray<FString> Applied = ApplyTransitionProps(Trans, Op);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("transition"), Trans->GetName());
				if (Trans->BoundGraph)
					ResultObj->SetStringField(TEXT("graph_address"), FString::Printf(TEXT("%s/%s"), *SMPath, *Trans->BoundGraph->GetName()));
				ResultObj->SetStringField(TEXT("properties_applied"), Applied.Num() > 0 ? FString::Join(Applied, TEXT(", ")) : TEXT("(none)"));
				SuccessCount++;
			}
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// ── set_entry_state ───────────────────────────────────────────
		if (Action == TEXT("set_entry_state"))
		{
			FString StateName;
			Op->TryGetStringField(TEXT("state"), StateName);
			if (StateName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'state' is required for set_entry_state"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			UAnimStateNodeBase* State = FindStateByName(SMGraph, StateName);
			if (!State)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("STATE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("State not found: '%s'"), *StateName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			if (!SMGraph->EntryNode)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NO_ENTRY_NODE"));
				ResultObj->SetStringField(TEXT("error"), TEXT("State machine has no entry node"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			UEdGraphPin* EntryOut = KismetAIUtils::GetStateEntryOutputPin(SMGraph->EntryNode);
			UEdGraphPin* StateIn = State->GetInputPin();
			if (!EntryOut || !StateIn)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PIN_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), TEXT("Entry output or state input pin missing"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			SMGraph->EntryNode->Modify();
			State->Modify();
			// Entry output is single-link by schema; break any existing link first.
			EntryOut->BreakAllPinLinks();
			EntryOut->MakeLinkTo(StateIn);

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("entry_state"), State->GetStateName());
			SuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// ── rename_state ──────────────────────────────────────────────
		if (Action == TEXT("rename_state"))
		{
			FString StateName, NewName;
			Op->TryGetStringField(TEXT("state"), StateName);
			Op->TryGetStringField(TEXT("new_name"), NewName);
			if (StateName.IsEmpty() || NewName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'state' and 'new_name' are required for rename_state"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			UAnimStateNodeBase* State = FindStateByName(SMGraph, StateName);
			if (!State)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("STATE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("State not found: '%s'"), *StateName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			if (StateName != NewName && StateNameExists(SMGraph, NewName))
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("STATE_EXISTS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("A state named '%s' already exists"), *NewName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			State->Modify();
			// OnRenameNode renames the node AND its bound graph (keeps GetStateName() + path addressing in sync).
			State->OnRenameNode(NewName);

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("old_name"), StateName);
			ResultObj->SetStringField(TEXT("new_name"), State->GetStateName());
			const FString Addr = BoundGraphAddress(SMPath, State);
			if (!Addr.IsEmpty()) ResultObj->SetStringField(TEXT("graph_address"), Addr);
			SuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// ── delete_state ──────────────────────────────────────────────
		if (Action == TEXT("delete_state"))
		{
			FString StateName;
			Op->TryGetStringField(TEXT("state"), StateName);
			if (StateName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'state' is required for delete_state"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}
			UAnimStateNodeBase* State = FindStateByName(SMGraph, StateName);
			if (!State)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("STATE_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("State not found: '%s'"), *StateName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Collect transitions touching this state so we can ensure none survive half-connected.
			TArray<UAnimStateTransitionNode*> Dangling;
			for (UEdGraphNode* Node : SMGraph->Nodes)
			{
				if (UAnimStateTransitionNode* T = Cast<UAnimStateTransitionNode>(Node))
				{
					if (T->GetPreviousState() == State || T->GetNextState() == State)
					{
						Dangling.Add(T);
					}
				}
			}
			int32 RemovedTransitions = 0;
			for (UAnimStateTransitionNode* T : Dangling)
			{
				FBlueprintEditorUtils::RemoveNode(Blueprint, T, /*bDontRecompile=*/true);
				RemovedTransitions++;
			}
			const FString RemovedName = State->GetStateName();
			FBlueprintEditorUtils::RemoveNode(Blueprint, State, /*bDontRecompile=*/true);

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("deleted_state"), RemovedName);
			ResultObj->SetNumberField(TEXT("removed_transitions"), RemovedTransitions);
			SuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		// Unknown action.
		ResultObj->SetBoolField(TEXT("success"), false);
		ResultObj->SetStringField(TEXT("error_code"), TEXT("UNKNOWN_ACTION"));
		ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown action: %s"), *Action));
		{
			TArray<TSharedPtr<FJsonValue>> Sugg;
			Sugg.Add(MakeShared<FJsonValueString>(TEXT("Valid actions: get, add_state_machine, add_state, add_conduit, add_state_alias, add_transition, set_transition_properties, set_entry_state, rename_state, delete_state, delete_transition")));
			ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
		}
		ErrorCount++;
		ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
	}

	// Structural change → mark + compile so the agent gets immediate compile feedback.
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);
	const bool bCompileSuccess = (Blueprint->Status != BS_Error);
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.bAppliedInMemory = (SuccessCount - ReadOnlySuccessCount) > 0;
	RecordCompileOutcome(Outcome, Blueprint, CompileResults);
	AutoSaveAfterMutation(Blueprint, Outcome);
	TArray<TSharedPtr<FJsonValue>> CompileMessagesArray;
	for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		EMessageSeverity::Type Severity = Message->GetSeverity();
		FString SeverityStr;
		switch (Severity)
		{
		case EMessageSeverity::Error: SeverityStr = TEXT("error"); break;
		case EMessageSeverity::Warning: SeverityStr = TEXT("warning"); break;
		case EMessageSeverity::Info: SeverityStr = TEXT("info"); break;
		default: SeverityStr = TEXT("note"); break;
		}
		MsgObj->SetStringField(TEXT("severity"), SeverityStr);
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());
		CompileMessagesArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetNumberField(TEXT("operations_succeeded"), SuccessCount);
	Data->SetNumberField(TEXT("operations_failed"), ErrorCount);
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetBoolField(TEXT("compile_success"), bCompileSuccess);
	Data->SetArrayField(TEXT("compile_messages"), CompileMessagesArray);
	if (!SMPath.IsEmpty()) Data->SetStringField(TEXT("state_machine"), SMPath);

	return FToolExecutionResult::Success(Data, ErrorCount == 0);
}

// HTTP adapter over DoManageStateMachine. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleManageStateMachine(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("manage_state_machine"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


FToolExecutionResult FKismetAIModule::DoManageComponents(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required and must be a non-empty string"));
	}

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath));
	}

	if (!Blueprint->SimpleConstructionScript)
	{
		return FToolExecutionResult::Error(400, TEXT("NO_SCS"), TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor-based Blueprint)"));
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsArray;
	if (!Body->TryGetArrayField(TEXT("operations"), OpsArray))
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'operations' array is required"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;

	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "ManageComponents", "Kismet: Manage Components"));
	Blueprint->Modify();

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 ErrorCount = 0;
	bool bAnyMutation = false;

	for (const TSharedPtr<FJsonValue>& OpVal : *OpsArray)
	{
		TSharedPtr<FJsonObject> Op = OpVal->AsObject();
		if (!Op) continue;

		FString Action;
		if (!Op->TryGetStringField(TEXT("action"), Action) || Action.IsEmpty())
		{
			TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
			ResultObj->SetStringField(TEXT("error"), TEXT("'action' field is required"));
			ErrorCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
			continue;
		}

		TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
		ResultObj->SetStringField(TEXT("action"), Action);

		bAnyMutation |= (Action != TEXT("get_component_property") && Action != TEXT("get_animation"));

		// ── add_component ─────────────────────────────────────────────
		if (Action == TEXT("add_component"))
		{
			FString ClassName = Op->GetStringField(TEXT("class"));
			FString CompName = Op->GetStringField(TEXT("name"));
			FString ParentName;
			Op->TryGetStringField(TEXT("parent"), ParentName);

			if (ClassName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'class' field is required for add_component"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			UClass* CompClass = FindComponentClass(ClassName);
			if (!CompClass)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_CLASS_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component class not found: %s"), *ClassName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use short names like 'StaticMeshComponent', 'BoxCollision', 'PointLightComponent', or full path '/Script/Engine.UStaticMeshComponent'")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			if (!CompClass->IsChildOf(UActorComponent::StaticClass()))
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_A_COMPONENT_CLASS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Class '%s' is not a component class (must inherit from UActorComponent)"), *ClassName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			USCS_Node* NewNode = SCS->CreateNode(CompClass, CompName.IsEmpty() ? NAME_None : FName(*CompName));
			if (!NewNode)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("CREATE_FAILED"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to create SCS node for class '%s'"), *ClassName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			if (ParentName.IsEmpty())
			{
				SCS->AddNode(NewNode);
			}
			else
			{
				USCS_Node* ParentNode = FindSCSNodeByName(SCS, ParentName);
				if (!ParentNode)
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Parent component not found: %s"), *ParentName));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
				ParentNode->AddChildNode(NewNode);
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("component"), NewNode->GetVariableName().ToString());
			ResultObj->SetStringField(TEXT("class"), CompClass->GetName());
			SuccessCount++;
		}
		// ── remove_component ──────────────────────────────────────────
		else if (Action == TEXT("remove_component"))
		{
			FString CompName = Op->GetStringField(TEXT("name"));
			if (CompName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for remove_component"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			USCS_Node* Node = FindSCSNodeByName(SCS, CompName);
			if (!Node)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *CompName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			SCS->RemoveNodeAndPromoteChildren(Node);
			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("component"), CompName);
			SuccessCount++;
		}
		// ── set_component_property ────────────────────────────────────
		else if (Action == TEXT("set_component_property"))
		{
			FString CompName = Op->GetStringField(TEXT("name"));
			FString PropName = Op->GetStringField(TEXT("property"));
			FString PropValue = Op->GetStringField(TEXT("value"));

			if (CompName.IsEmpty() || PropName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' and 'property' fields are required for set_component_property"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			FString TemplateSource;
			UActorComponent* Template = ResolveComponentTemplate(Blueprint, CompName, TemplateSource);
			if (!Template)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s (searched this Blueprint's components, parent Blueprint components, and native C++ components)"), *CompName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Indexed access like "OverrideMaterials[0]" never matches a property name —
			// catch it up front and point at the action that does the job, instead of a
			// bare PROPERTY_NOT_FOUND that reads as "impossible".
			FString BasePropName = PropName;
			FString IndexPart;
			const bool bIndexedAccess = PropName.Split(TEXT("["), &BasePropName, &IndexPart);

			FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropName));
			FProperty* BaseProp = bIndexedAccess ? Template->GetClass()->FindPropertyByName(FName(*BasePropName)) : Prop;

			if (bIndexedAccess && BaseProp)
			{
				const int32 SlotIndex = FCString::Atoi(*IndexPart);
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("ARRAY_INDEX_UNSUPPORTED"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Reflection can't target one element of '%s' — indexed property access is not supported"), *BasePropName));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				if (BasePropName == TEXT("OverrideMaterials"))
				{
					Sugg.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Use the set_material action for material slots: {\"action\":\"set_material\",\"name\":\"%s\",\"material\":\"/Game/...\",\"index\":%d}"), *CompName, SlotIndex)));
				}
				else
				{
					Sugg.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Set the whole array instead: property='%s', value='(elem0,elem1,...)' in UE text format"), *BasePropName)));
				}
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			if (Prop && PropName == TEXT("OverrideMaterials"))
			{
				// Whole-array import "works" syntactically but bypasses SetMaterial's
				// slot bookkeeping — the dedicated action exists because of that.
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("USE_SET_MATERIAL"));
				ResultObj->SetStringField(TEXT("error"), TEXT("OverrideMaterials can't be set via generic reflection"));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("Use the set_material action once per slot: {\"action\":\"set_material\",\"name\":\"%s\",\"material\":\"/Game/...\",\"index\":0}"), *CompName)));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			if (!Prop)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("PROPERTY_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Property '%s' not found on component '%s' (class: %s)"), *PropName, *CompName, *Template->GetClass()->GetName()));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			Template->Modify();
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Template);
			const TCHAR* ImportResult = Prop->ImportText_Direct(*PropValue, ValuePtr, Template, PPF_None);
			if (!ImportResult)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Failed to parse value '%s' for property '%s'"), *PropValue, *PropName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("component"), CompName);
			ResultObj->SetStringField(TEXT("property"), PropName);
			ResultObj->SetStringField(TEXT("template_source"), TemplateSource);
			SuccessCount++;
		}
		// ── reparent_component ────────────────────────────────────────
		else if (Action == TEXT("reparent_component"))
		{
			FString CompName = Op->GetStringField(TEXT("name"));
			FString NewParentName;
			Op->TryGetStringField(TEXT("new_parent"), NewParentName);

			if (CompName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for reparent_component"));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			USCS_Node* Node = FindSCSNodeByName(SCS, CompName);
			if (!Node)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *CompName));
				ErrorCount++;
				ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
				continue;
			}

			// Remove from current location
			USCS_Node* CurrentParent = SCS->FindParentNode(Node);
			if (CurrentParent)
			{
				CurrentParent->RemoveChildNode(Node, /*bRemoveFromAllNodes=*/false);
			}
			else
			{
				// It's a root node, remove from root set
				SCS->RemoveNode(Node, /*bValidateSceneRootNodes=*/false);
			}

			// Add to new parent
			if (NewParentName.IsEmpty())
			{
				SCS->AddNode(Node);
			}
			else
			{
				USCS_Node* NewParent = FindSCSNodeByName(SCS, NewParentName);
				if (!NewParent)
				{
					// Couldn't find new parent — put it back as root to avoid orphaning
					SCS->AddNode(Node);
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("New parent component not found: %s (node re-added as root)"), *NewParentName));
					ErrorCount++;
					ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
					continue;
				}
				NewParent->AddChildNode(Node, /*bAddToAllNodes=*/false);
			}

			ResultObj->SetBoolField(TEXT("success"), true);
			ResultObj->SetStringField(TEXT("component"), CompName);
			ResultObj->SetStringField(TEXT("new_parent"), NewParentName.IsEmpty() ? TEXT("(root)") : *NewParentName);
			SuccessCount++;
		}
		// ── set_collision ─────────────────────────────────────────────
		// Collision lives in the BodyInstance struct; set it through the proper
		// APIs (a generic set_component_property on BodyInstance can't load a
		// profile's channel responses). At least one of profile /
		// collision_enabled / object_type / responses is required.
		else if (Action == TEXT("set_collision"))
		{
			const FString CompName = Op->GetStringField(TEXT("name"));
			FString TemplateSource;
			UActorComponent* Template = CompName.IsEmpty() ? nullptr : ResolveComponentTemplate(Blueprint, CompName, TemplateSource);
			UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Template);

			if (CompName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' is required for set_collision"));
				ErrorCount++;
			}
			else if (!Template)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *CompName));
				ErrorCount++;
			}
			else if (!Prim)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_PRIMITIVE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Collision only applies to primitive components (mesh/shape), not '%s'"), *CompName));
				ErrorCount++;
			}
			else
			{
				Prim->Modify();
				TArray<FString> Applied;
				FString FailDetail;

				FString Profile;
				if (Op->TryGetStringField(TEXT("profile"), Profile) && !Profile.IsEmpty())
				{
					Prim->SetCollisionProfileName(FName(*Profile));
					Applied.Add(FString::Printf(TEXT("profile=%s"), *Profile));
				}
				FString Enabled;
				if (Op->TryGetStringField(TEXT("collision_enabled"), Enabled) && !Enabled.IsEmpty())
				{
					ECollisionEnabled::Type E;
					if (ParseCollisionEnabled(Enabled, E)) { Prim->SetCollisionEnabled(E); Applied.Add(TEXT("collision_enabled")); }
					else FailDetail += FString::Printf(TEXT("bad collision_enabled '%s'; "), *Enabled);
				}
				FString ObjType;
				if (Op->TryGetStringField(TEXT("object_type"), ObjType) && !ObjType.IsEmpty())
				{
					ECollisionChannel C;
					if (ParseCollisionChannel(ObjType, C)) { Prim->SetCollisionObjectType(C); Applied.Add(TEXT("object_type")); }
					else FailDetail += FString::Printf(TEXT("bad object_type '%s'; "), *ObjType);
				}
				const TSharedPtr<FJsonObject>* Responses = nullptr;
				if (Op->TryGetObjectField(TEXT("responses"), Responses) && Responses && Responses->IsValid())
				{
					for (const auto& Pair : (*Responses)->Values)
					{
						const FString Key = KismetJsonKey(Pair.Key);
						ECollisionChannel C; ECollisionResponse R;
						const FString RespStr = Pair.Value.IsValid() ? Pair.Value->AsString() : FString();
						if (ParseCollisionChannel(Key, C) && ParseCollisionResponse(RespStr, R))
						{ Prim->SetCollisionResponseToChannel(C, R); Applied.Add(FString::Printf(TEXT("%s=%s"), *Key, *RespStr)); }
						else FailDetail += FString::Printf(TEXT("bad response %s=%s; "), *Key, *RespStr);
					}
				}

				if (Applied.Num() == 0)
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
					ResultObj->SetStringField(TEXT("error"), FailDetail.IsEmpty()
						? TEXT("set_collision needs at least one of: profile, collision_enabled, object_type, responses")
						: FailDetail);
					ErrorCount++;
				}
				else
				{
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultObj->SetStringField(TEXT("component"), CompName);
					ResultObj->SetStringField(TEXT("applied"), FString::Join(Applied, TEXT(", ")));
					if (!FailDetail.IsEmpty()) ResultObj->SetStringField(TEXT("warning"), FailDetail);
					SuccessCount++;
				}
			}
		}
		// ── set_material ──────────────────────────────────────────────
		// Set/override a material slot on a primitive component. A generic
		// set_component_property on the OverrideMaterials array can't target a
		// single slot; SetMaterial(index, material) does.
		else if (Action == TEXT("set_material"))
		{
			const FString CompName = Op->GetStringField(TEXT("name"));
			FString MatPath;
			Op->TryGetStringField(TEXT("material"), MatPath);
			int32 Index = Op->HasField(TEXT("index")) ? static_cast<int32>(Op->GetNumberField(TEXT("index"))) : 0;
			if (Index < 0) Index = 0;

			FString TemplateSource;
			UActorComponent* Template = CompName.IsEmpty() ? nullptr : ResolveComponentTemplate(Blueprint, CompName, TemplateSource);
			UPrimitiveComponent* Prim = Cast<UPrimitiveComponent>(Template);
			UMaterialInterface* Mat = ResolveMaterialAsset(MatPath);

			if (CompName.IsEmpty() || MatPath.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' and 'material' are required for set_material"));
				ErrorCount++;
			}
			else if (!Template)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *CompName));
				ErrorCount++;
			}
			else if (!Prim)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_PRIMITIVE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Materials only apply to primitive components, not '%s'"), *CompName));
				ErrorCount++;
			}
			else if (!Mat)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not load material '%s'"), *MatPath));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use a full asset path, e.g. /Game/Materials/M_Foo or /Engine/EngineMaterials/DefaultMaterial")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
			}
			else
			{
				Prim->Modify();
				Prim->SetMaterial(Index, Mat);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("component"), CompName);
				ResultObj->SetNumberField(TEXT("index"), Index);
				ResultObj->SetStringField(TEXT("material"), Mat->GetPathName());
				SuccessCount++;
			}
		}
		// ── set_post_process_material ─────────────────────────────────
		// Post-process materials (blendables) live in Settings.WeightedBlendables.Array —
		// an array of structs nested two levels inside FPostProcessSettings, out of reach
		// of a generic set_component_property. Use the engine's AddBlendable/RemoveBlendable.
		else if (Action == TEXT("set_post_process_material"))
		{
			const FString CompName = Op->GetStringField(TEXT("name"));
			FString MatPath;
			Op->TryGetStringField(TEXT("material"), MatPath);
			double Weight = 1.0;
			Op->TryGetNumberField(TEXT("weight"), Weight);
			bool bRemove = false;
			Op->TryGetBoolField(TEXT("remove"), bRemove);

			FString TemplateSource;
			UActorComponent* Template = CompName.IsEmpty() ? nullptr : ResolveComponentTemplate(Blueprint, CompName, TemplateSource);
			FString SettingsPropName;
			FPostProcessSettings* Settings = FindPostProcessSettings(Template, SettingsPropName);
			UMaterialInterface* Mat = ResolveMaterialAsset(MatPath);

			if (CompName.IsEmpty() || MatPath.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' and 'material' are required for set_post_process_material"));
				ErrorCount++;
			}
			else if (!Template)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s"), *CompName));
				ErrorCount++;
			}
			else if (!Settings)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NO_POST_PROCESS_SETTINGS"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component '%s' (%s) has no PostProcessSettings — use a PostProcessComponent, CameraComponent, or SceneCaptureComponent2D"), *CompName, *Template->GetClass()->GetName()));
				ErrorCount++;
			}
			else if (!Mat)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not load material '%s'"), *MatPath));
				ErrorCount++;
			}
			else if (!bRemove && Mat->GetMaterial() && Mat->GetMaterial()->MaterialDomain != MD_PostProcess)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_POST_PROCESS_DOMAIN"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("'%s' has domain %s — a blendable must use the PostProcess material domain or it silently does nothing"), *MatPath, *EnumToString<EMaterialDomain>(Mat->GetMaterial()->MaterialDomain.GetValue())));
				TArray<TSharedPtr<FJsonValue>> Sugg;
				Sugg.Add(MakeShared<FJsonValueString>(TEXT("Set the domain first: manage_material with domain='PostProcess'")));
				ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
				ErrorCount++;
			}
			else
			{
				Template->Modify();
				if (bRemove)
				{
					Settings->RemoveBlendable(Mat);
				}
				else
				{
					Settings->AddBlendable(Mat, static_cast<float>(Weight));
				}
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("component"), CompName);
				ResultObj->SetStringField(TEXT("material"), Mat->GetPathName());
				ResultObj->SetStringField(TEXT("settings_property"), SettingsPropName);
				ResultObj->SetStringField(TEXT("template_source"), TemplateSource);
				if (bRemove)
				{
					ResultObj->SetBoolField(TEXT("removed"), true);
				}
				else
				{
					ResultObj->SetNumberField(TEXT("weight"), Weight);
				}
				ResultObj->SetNumberField(TEXT("blendable_count"), Settings->WeightedBlendables.Array.Num());
				SuccessCount++;
			}
		}
		// ── get_component_property ────────────────────────────────────
		// Read counterpart to set_component_property: exports template property
		// values via reflection. With 'property', returns that one value; without
		// it, lists the properties that differ from the component class defaults
		// (the template's customizations). Reads from SCS templates, inherited-BP
		// override templates (without creating one), and native CDO subobjects.
		else if (Action == TEXT("get_component_property"))
		{
			const FString CompName = Op->GetStringField(TEXT("name"));
			FString PropName;
			Op->TryGetStringField(TEXT("property"), PropName);

			FString TemplateSource;
			UActorComponent* Template = CompName.IsEmpty() ? nullptr
				: ResolveComponentTemplate(Blueprint, CompName, TemplateSource, /*bForWrite=*/false);

			auto ExportPropValue = [](FProperty* Prop, UActorComponent* Container) -> FString
			{
				FString ValueText;
				Prop->ExportTextItem_Direct(ValueText, Prop->ContainerPtrToValuePtr<void>(Container), nullptr, Container, PPF_None);
				return ValueText;
			};
			auto PropCppType = [](FProperty* Prop) -> FString
			{
				FString Extended;
				FString CppType = Prop->GetCPPType(&Extended);
				return CppType + Extended;
			};

			if (CompName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for get_component_property"));
				ErrorCount++;
			}
			else if (!Template)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s (searched this Blueprint's components, parent Blueprint components, and native C++ components)"), *CompName));
				ErrorCount++;
			}
			else if (!PropName.IsEmpty())
			{
				if (FProperty* Prop = Template->GetClass()->FindPropertyByName(FName(*PropName)))
				{
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultObj->SetStringField(TEXT("component"), CompName);
					ResultObj->SetStringField(TEXT("property"), PropName);
					ResultObj->SetStringField(TEXT("type"), PropCppType(Prop));
					ResultObj->SetStringField(TEXT("value"), ExportPropValue(Prop, Template));
					ResultObj->SetStringField(TEXT("template_source"), TemplateSource);
					SuccessCount++;
				}
				else
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("PROPERTY_NOT_FOUND"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Property '%s' not found on component '%s' (class: %s)"), *PropName, *CompName, *Template->GetClass()->GetName()));
					TArray<TSharedPtr<FJsonValue>> Sugg;
					Sugg.Add(MakeShared<FJsonValueString>(TEXT("Use the C++ property name (e.g. StaticMesh, OverrideMaterials, RelativeLocation, bVisible). get_component_property without 'property' lists the template's modified values.")));
					ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
					ErrorCount++;
				}
			}
			else
			{
				// Diff against the template's archetype: the class CDO for SCS
				// components, or the ancestor template for inherited overrides.
				UObject* Archetype = Template->GetArchetype();
				TArray<TSharedPtr<FJsonValue>> PropsArr;
				for (TFieldIterator<FProperty> It(Template->GetClass()); It; ++It)
				{
					FProperty* Prop = *It;
					if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated))
					{
						continue;
					}
					if (Archetype && Archetype->GetClass() == Template->GetClass()
						&& Prop->Identical_InContainer(Template, Archetype))
					{
						continue;
					}
					TSharedPtr<FJsonObject> PJ = MakeShared<FJsonObject>();
					PJ->SetStringField(TEXT("property"), Prop->GetName());
					PJ->SetStringField(TEXT("type"), PropCppType(Prop));
					PJ->SetStringField(TEXT("value"), ExportPropValue(Prop, Template));
					PropsArr.Add(MakeShared<FJsonValueObject>(PJ));
				}
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("component"), CompName);
				ResultObj->SetStringField(TEXT("class"), Template->GetClass()->GetName());
				ResultObj->SetStringField(TEXT("template_source"), TemplateSource);
				ResultObj->SetArrayField(TEXT("modified_properties"), PropsArr);
				// Skeletal mesh components: surface the resolved animation config (mode,
				// ABP asset path, single-node asset, mesh + skeleton) here too, so a plain
				// inspection answers "which Animation Blueprint is set?" in one call.
				if (Cast<USkinnedMeshComponent>(Template))
				{
					TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
					EmitSkinnedMeshAnimInfo(Template, AnimObj);
					ResultObj->SetObjectField(TEXT("animation"), AnimObj);
				}
				ResultObj->SetStringField(TEXT("note"), TEXT("Only values differing from the component class defaults are listed; pass 'property' to read any specific one (inspect_class on the component class lists all names). For skeletal mesh components, the 'animation' object holds mode/anim_class/skeleton."));
				SuccessCount++;
			}
		}
		// ── get_animation ─────────────────────────────────────────────
		// READ the animation configuration of a skeletal mesh component: mode,
		// anim class (resolved to the source ABP asset path), single-node asset,
		// and the SkeletalMesh + its skeleton. Works on SCS, inherited-Blueprint
		// and native C++ components. Read-only (no compile).
		else if (Action == TEXT("get_animation"))
		{
			const FString CompName = Op->GetStringField(TEXT("name"));
			FString TemplateSource;
			UActorComponent* Template = CompName.IsEmpty() ? nullptr
				: ResolveComponentTemplate(Blueprint, CompName, TemplateSource, /*bForWrite=*/false);

			if (CompName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for get_animation"));
				ErrorCount++;
			}
			else if (!Template)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s (searched this Blueprint's components, parent Blueprint components, and native C++ components)"), *CompName));
				ErrorCount++;
			}
			else if (!Cast<USkinnedMeshComponent>(Template))
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_SKELETAL_MESH"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component '%s' is a %s, not a SkeletalMeshComponent — it has no animation configuration"), *CompName, *Template->GetClass()->GetName()));
				ErrorCount++;
			}
			else
			{
				TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
				EmitSkinnedMeshAnimInfo(Template, AnimObj);
				ResultObj->SetBoolField(TEXT("success"), true);
				ResultObj->SetStringField(TEXT("component"), CompName);
				ResultObj->SetStringField(TEXT("template_source"), TemplateSource);
				ResultObj->SetObjectField(TEXT("animation"), AnimObj);
				SuccessCount++;
			}
		}
		// ── set_animation ─────────────────────────────────────────────
		// ASSIGN animation configuration on a skeletal mesh component. Accepts
		// anim_class (ABP asset OR generated-class path), animation_mode, and
		// anim_to_play. Sets AnimationMode automatically when assigning a class
		// (→ AnimationBlueprint) or an asset (→ AnimationSingleNode) unless the
		// caller passes animation_mode explicitly. Validates skeleton compat.
		else if (Action == TEXT("set_animation"))
		{
			const FString CompName = Op->GetStringField(TEXT("name"));
			FString AnimClassValue, AnimModeValue, AnimToPlayValue;
			const bool bHasAnimClass = Op->TryGetStringField(TEXT("anim_class"), AnimClassValue);
			const bool bHasMode = Op->TryGetStringField(TEXT("animation_mode"), AnimModeValue);
			const bool bHasAnimToPlay = Op->TryGetStringField(TEXT("anim_to_play"), AnimToPlayValue);

			FString TemplateSource;
			UActorComponent* Template = CompName.IsEmpty() ? nullptr
				: ResolveComponentTemplate(Blueprint, CompName, TemplateSource);
			USkeletalMeshComponent* SkelComp = Cast<USkeletalMeshComponent>(Template);

			if (CompName.IsEmpty())
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("'name' field is required for set_animation"));
				ErrorCount++;
			}
			else if (!Template)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("COMPONENT_NOT_FOUND"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component not found: %s (searched this Blueprint's components, parent Blueprint components, and native C++ components)"), *CompName));
				ErrorCount++;
			}
			else if (!SkelComp)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("NOT_SKELETAL_MESH"));
				ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Component '%s' is a %s, not a SkeletalMeshComponent — it has no animation configuration to set"), *CompName, *Template->GetClass()->GetName()));
				ErrorCount++;
			}
			else if (!bHasAnimClass && !bHasMode && !bHasAnimToPlay)
			{
				ResultObj->SetBoolField(TEXT("success"), false);
				ResultObj->SetStringField(TEXT("error_code"), TEXT("MISSING_FIELD"));
				ResultObj->SetStringField(TEXT("error"), TEXT("set_animation needs at least one of: anim_class, animation_mode, anim_to_play"));
				ErrorCount++;
			}
			else
			{
				bool bFailed = false;
				TArray<TSharedPtr<FJsonValue>> Warnings;

				// The skeleton the component is built around (for compat checks).
				USkinnedAsset* MeshAsset = SkelComp->GetSkinnedAsset();
				USkeleton* CompSkeleton = MeshAsset ? MeshAsset->GetSkeleton() : nullptr;

				// Resolve + assign the anim class.
				UClass* ResolvedAnimClass = nullptr;
				bool bClearAnimClass = false;
				if (bHasAnimClass)
				{
					if (AnimClassValue.IsEmpty() || AnimClassValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
					{
						bClearAnimClass = true;
					}
					else
					{
						UAnimBlueprint* SourceABP = nullptr;
						ResolvedAnimClass = ResolveAnimInstanceClass(AnimClassValue, SourceABP);
						if (!ResolvedAnimClass)
						{
							ResultObj->SetBoolField(TEXT("success"), false);
							ResultObj->SetStringField(TEXT("error_code"), TEXT("ANIM_CLASS_NOT_FOUND"));
							ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not resolve anim_class '%s' to an Animation Blueprint (pass an ABP asset path like /Game/.../ABP_Hero)"), *AnimClassValue));
							ErrorCount++;
							bFailed = true;
						}
						else if (CompSkeleton)
						{
							// Skeleton-compat guard, mirroring insert_node's anim asset spawner.
							USkeleton* ABPSkeleton = SourceABP ? SourceABP->TargetSkeleton : nullptr;
							if (ABPSkeleton && !CompSkeleton->IsCompatibleForEditor(ABPSkeleton))
							{
								ResultObj->SetBoolField(TEXT("success"), false);
								ResultObj->SetStringField(TEXT("error_code"), TEXT("SKELETON_MISMATCH"));
								ResultObj->SetStringField(TEXT("error"), FString::Printf(
									TEXT("Skeleton mismatch: Animation Blueprint '%s' targets skeleton '%s' but component '%s' uses skeleton '%s'. Pick an Animation Blueprint built for the compatible skeleton."),
									*AnimClassValue, *ABPSkeleton->GetName(), *CompName, *CompSkeleton->GetName()));
								ErrorCount++;
								bFailed = true;
							}
						}
					}
				}

				// Resolve the single-node asset.
				UAnimationAsset* ResolvedAnimToPlay = nullptr;
				bool bClearAnimToPlay = false;
				if (!bFailed && bHasAnimToPlay)
				{
					if (AnimToPlayValue.IsEmpty() || AnimToPlayValue.Equals(TEXT("None"), ESearchCase::IgnoreCase))
					{
						bClearAnimToPlay = true;
					}
					else
					{
						ResolvedAnimToPlay = LoadObject<UAnimationAsset>(nullptr, *AnimToPlayValue);
						if (!ResolvedAnimToPlay)
						{
							ResultObj->SetBoolField(TEXT("success"), false);
							ResultObj->SetStringField(TEXT("error_code"), TEXT("ANIM_ASSET_NOT_FOUND"));
							ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Could not load anim_to_play asset '%s'"), *AnimToPlayValue));
							ErrorCount++;
							bFailed = true;
						}
						else if (CompSkeleton && ResolvedAnimToPlay->GetSkeleton()
							&& !CompSkeleton->IsCompatibleForEditor(ResolvedAnimToPlay->GetSkeleton()))
						{
							ResultObj->SetBoolField(TEXT("success"), false);
							ResultObj->SetStringField(TEXT("error_code"), TEXT("SKELETON_MISMATCH"));
							ResultObj->SetStringField(TEXT("error"), FString::Printf(
								TEXT("Skeleton mismatch: asset '%s' uses skeleton '%s' but component '%s' uses skeleton '%s'. Pick an animation built for the compatible skeleton."),
								*AnimToPlayValue, *ResolvedAnimToPlay->GetSkeleton()->GetName(), *CompName, *CompSkeleton->GetName()));
							ErrorCount++;
							bFailed = true;
						}
					}
				}

				// Validate an explicit animation_mode value.
				EAnimationMode::Type ExplicitMode = EAnimationMode::AnimationBlueprint;
				if (!bFailed && bHasMode && !ParseAnimationMode(AnimModeValue, ExplicitMode))
				{
					ResultObj->SetBoolField(TEXT("success"), false);
					ResultObj->SetStringField(TEXT("error_code"), TEXT("INVALID_VALUE"));
					ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown animation_mode '%s' (expected AnimationBlueprint, AnimationSingleNode, or AnimationCustomMode)"), *AnimModeValue));
					ErrorCount++;
					bFailed = true;
				}

				if (!bFailed)
				{
					if (!MeshAsset)
					{
						Warnings.Add(MakeShared<FJsonValueString>(FString::Printf(
							TEXT("Component '%s' has no SkeletalMesh set, so skeleton compatibility could not be verified. The animation assignment was applied anyway; set the mesh to confirm it matches."), *CompName)));
					}

					SkelComp->Modify();

					if (bClearAnimClass)
					{
						SkelComp->AnimClass = nullptr;
					}
					else if (ResolvedAnimClass)
					{
						SkelComp->AnimClass = ResolvedAnimClass;
					}

					if (bClearAnimToPlay)
					{
						SkelComp->AnimationData.AnimToPlay = nullptr;
					}
					else if (ResolvedAnimToPlay)
					{
						SkelComp->AnimationData.AnimToPlay = ResolvedAnimToPlay;
					}

					// Mode: explicit caller value wins; otherwise infer from what was set
					// (assigning a class → AnimationBlueprint, an asset → AnimationSingleNode).
					if (bHasMode)
					{
						SetAnimationModeOnTemplate(SkelComp, ExplicitMode);
					}
					else if (ResolvedAnimClass)
					{
						SetAnimationModeOnTemplate(SkelComp, EAnimationMode::AnimationBlueprint);
					}
					else if (ResolvedAnimToPlay)
					{
						SetAnimationModeOnTemplate(SkelComp, EAnimationMode::AnimationSingleNode);
					}

					TSharedPtr<FJsonObject> AnimObj = MakeShared<FJsonObject>();
					EmitSkinnedMeshAnimInfo(Template, AnimObj);
					ResultObj->SetBoolField(TEXT("success"), true);
					ResultObj->SetStringField(TEXT("component"), CompName);
					ResultObj->SetStringField(TEXT("template_source"), TemplateSource);
					ResultObj->SetObjectField(TEXT("animation"), AnimObj);
					if (Warnings.Num() > 0)
					{
						ResultObj->SetArrayField(TEXT("warnings"), Warnings);
					}
					SuccessCount++;
				}
			}
		}
		// ── unknown action ────────────────────────────────────────────
		else
		{
			ResultObj->SetBoolField(TEXT("success"), false);
			ResultObj->SetStringField(TEXT("error_code"), TEXT("UNKNOWN_ACTION"));
			ResultObj->SetStringField(TEXT("error"), FString::Printf(TEXT("Unknown action: %s"), *Action));
			TArray<TSharedPtr<FJsonValue>> Sugg;
			Sugg.Add(MakeShared<FJsonValueString>(TEXT("Valid actions: add_component, remove_component, set_component_property, get_component_property, get_animation, set_animation, reparent_component, set_collision, set_material, set_post_process_material")));
			ResultObj->SetArrayField(TEXT("suggestions"), Sugg);
			ErrorCount++;
		}

		ResultsArray.Add(MakeShared<FJsonValueObject>(ResultObj));
	}

	// Compile — skipped when every operation was a read (get_component_property mutates nothing)
	FCompilerResultsLog CompileResults;
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.PackageName = Blueprint->GetOutermost()->GetName();
	Outcome.bAppliedInMemory = bAnyMutation;
	if (bAnyMutation)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &CompileResults);
		RecordCompileOutcome(Outcome, Blueprint, CompileResults);

		// Persist the change so it survives an editor close without manual save
		AutoSaveAfterMutation(Blueprint, Outcome);
	}
	else
	{
		Outcome.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
		Outcome.SavePolicy = KismetAISavePolicy::AutosaveEvenIfCompileFailed;
	}

	bool bCompileSuccess = (Blueprint->Status != BS_Error);

	// Collect compile errors/warnings
	TArray<TSharedPtr<FJsonValue>> CompileErrors;
	TArray<TSharedPtr<FJsonValue>> CompileWarnings;
	for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
	{
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());

		EMessageSeverity::Type Severity = Message->GetSeverity();
		if (Severity == EMessageSeverity::Error)
		{
			MsgObj->SetStringField(TEXT("severity"), TEXT("error"));
			CompileErrors.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
		else if (Severity == EMessageSeverity::Warning)
		{
			MsgObj->SetStringField(TEXT("severity"), TEXT("warning"));
			CompileWarnings.Add(MakeShared<FJsonValueObject>(MsgObj));
		}
	}

	TSharedPtr<FJsonObject> CompilationObj = MakeShared<FJsonObject>();
	CompilationObj->SetBoolField(TEXT("success"), bCompileSuccess);
	CompilationObj->SetArrayField(TEXT("errors"), CompileErrors);
	CompilationObj->SetArrayField(TEXT("warnings"), CompileWarnings);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetBoolField(TEXT("success"), ErrorCount == 0);
	Data->SetStringField(TEXT("blueprint"), AssetPath);
	Data->SetNumberField(TEXT("operations_count"), OpsArray->Num());
	Data->SetArrayField(TEXT("results"), ResultsArray);
	Data->SetObjectField(TEXT("compilation"), CompilationObj);

	return FToolExecutionResult::Success(Data, ErrorCount == 0 && bCompileSuccess);
}

// HTTP adapter over DoManageComponents. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleManageComponents(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("manage_components"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// QueryAssets — shared core used by HandleSearchAssets (HTTP) and
//               UKismetAIBridge::SearchAssets (JS bridge).
//
// Must run on the game thread. Does NOT load assets.
// Unknown TypeFilter → Out remains empty (no silent fallback to "all").
// ─────────────────────────────────────────────────────────────────────────────

// Canonical map from friendly type names → FTopLevelAssetPath.
// Defined once here so both callers stay in sync (no duplication).
static const TMap<FString, FTopLevelAssetPath>& GetAssetClassMap()
{
	static const TMap<FString, FTopLevelAssetPath> ClassMap = {
		{TEXT("Blueprint"),               FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("Blueprint"))},
		{TEXT("WidgetBlueprint"),          FTopLevelAssetPath(TEXT("/Script/UMGEditor"),TEXT("WidgetBlueprint"))},
		{TEXT("AnimBlueprint"),            FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("AnimBlueprint"))},
		{TEXT("UserDefinedStruct"),        FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("UserDefinedStruct"))},
		{TEXT("UserDefinedEnum"),          FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("UserDefinedEnum"))},
		{TEXT("DataTable"),               FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("DataTable"))},
		{TEXT("Material"),                FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("Material"))},
		{TEXT("MaterialInstance"),         FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("MaterialInstanceConstant"))},
		{TEXT("MaterialInstanceConstant"), FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("MaterialInstanceConstant"))},
		{TEXT("Texture2D"),               FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("Texture2D"))},
		{TEXT("StaticMesh"),              FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("StaticMesh"))},
		{TEXT("SkeletalMesh"),            FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("SkeletalMesh"))},
		{TEXT("SoundWave"),               FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("SoundWave"))},
		{TEXT("World"),                   FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("World"))},
		{TEXT("ParticleSystem"),           FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("ParticleSystem"))},
		{TEXT("NiagaraSystem"),            FTopLevelAssetPath(TEXT("/Script/Niagara"),   TEXT("NiagaraSystem"))},
		{TEXT("NiagaraEmitter"),           FTopLevelAssetPath(TEXT("/Script/Niagara"),   TEXT("NiagaraEmitter"))},
		// Animation asset types
		{TEXT("AnimSequence"),             FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("AnimSequence"))},
		{TEXT("AnimMontage"),              FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("AnimMontage"))},
		{TEXT("AnimComposite"),            FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("AnimComposite"))},
		{TEXT("BlendSpace"),               FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("BlendSpace"))},
		{TEXT("BlendSpace1D"),             FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("BlendSpace1D"))},
		{TEXT("AimOffsetBlendSpace"),      FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("AimOffsetBlendSpace"))},
		{TEXT("AimOffsetBlendSpace1D"),    FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("AimOffsetBlendSpace1D"))},
		{TEXT("Skeleton"),                 FTopLevelAssetPath(TEXT("/Script/Engine"),    TEXT("Skeleton"))},
	};
	return ClassMap;
}

void FKismetAIModule::QueryAssets(const FString& Query, const FString& TypeFilter, int32 Max, TArray<FAssetData>& Out)
{
	Out.Reset();

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	FARFilter Filter;
	Filter.bRecursivePaths   = true;
	Filter.bRecursiveClasses = true;

	if (!TypeFilter.IsEmpty())
	{
		const TMap<FString, FTopLevelAssetPath>& ClassMap = GetAssetClassMap();
		const FTopLevelAssetPath* Found = ClassMap.Find(TypeFilter);
		if (!Found)
		{
			// Unknown TypeFilter — return empty result (fail loud, no silent fallback).
			return;
		}
		Filter.ClassPaths.Add(*Found);
	}

	// FARFilter requires at least one criterion; fall back to scanning /Game.
	if (Filter.ClassPaths.Num() == 0 && Filter.PackagePaths.Num() == 0)
	{
		Filter.PackagePaths.Add(FName(TEXT("/Game")));
	}

	// Ensure registry scan is complete before querying.
	if (AssetRegistry.IsLoadingAssets())
	{
		AssetRegistry.WaitForCompletion();
	}

	TArray<FAssetData> AllAssets;
	AssetRegistry.GetAssets(Filter, AllAssets);

	// Post-filter: /Game only + name substring.
	Out.Reserve(AllAssets.Num());
	for (const FAssetData& Asset : AllAssets)
	{
		if (!Asset.PackageName.ToString().StartsWith(TEXT("/Game")))
		{
			continue;
		}
		if (!Query.IsEmpty())
		{
			if (!Asset.AssetName.ToString().Contains(Query, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}
		Out.Add(Asset);
	}

	// Sort by name, then cap.
	Out.Sort([](const FAssetData& A, const FAssetData& B)
	{
		return A.AssetName.LexicalLess(B.AssetName);
	});

	if (Out.Num() > Max)
	{
		Out.SetNum(Max);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// @-reference injection pipeline
// FormatReferencesBlock — pure, ResolveRefs — game-thread
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAIModule::FormatReferencesBlock(const TArray<FAssetRef>& Refs)
{
	if (Refs.IsEmpty())
	{
		return FString();
	}

	// Deduplicate by lowercase path (case-insensitive, keep first occurrence).
	TSet<FString> SeenPaths;
	TArray<const FAssetRef*> Unique;
	for (const FAssetRef& Ref : Refs)
	{
		FString Lower = Ref.Path.ToLower();
		if (!SeenPaths.Contains(Lower))
		{
			SeenPaths.Add(Lower);
			Unique.Add(&Ref);
		}
	}

	FString Body;
	for (const FAssetRef* Ref : Unique)
	{
		if (Ref->bMissing)
		{
			// Missing refs surfaced explicitly, never silently dropped
			Body += FString::Printf(TEXT("- (missing) %s\n"), *Ref->Path);
		}
		else
		{
			FString ClassLower = Ref->Class.ToLower();
			FString Line = FString::Printf(TEXT("- %s %s"), *ClassLower, *Ref->Path);
			if (Ref->bFocused)
			{
				Line += TEXT(" (focused)");
			}
			if (!Ref->Summary.IsEmpty())
			{
				Line += FString::Printf(TEXT(" (%s)"), *Ref->Summary);
			}
			Body += Line + TEXT("\n");
		}
	}

	return FString::Printf(TEXT("<references>\n%s</references>\n\n"), *Body);
}

void FKismetAIModule::ResolveRefs(const FString& RefsJson, TArray<FAssetRef>& Out)
{
	Out.Reset();

	if (RefsJson.IsEmpty())
	{
		return;
	}

	// Parse the JSON array [{path, name, class, focused}, ...]
	TArray<TSharedPtr<FJsonValue>> JsonArray;
	{
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RefsJson);
		if (!FJsonSerializer::Deserialize(Reader, JsonArray))
		{
			// Malformed JSON — treat as empty (no crash, no silent fabrication)
			KISMETAI_DEV_LOG(LogKismetAI, Warning, TEXT("ResolveRefs: failed to parse RefsJson — treating as empty refs"));
			return;
		}
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	for (const TSharedPtr<FJsonValue>& Val : JsonArray)
	{
		TSharedPtr<FJsonObject> Obj = Val.IsValid() ? Val->AsObject() : nullptr;
		if (!Obj.IsValid())
		{
			continue;
		}

		FAssetRef Ref;
		Ref.Path  = Obj->HasField(TEXT("path"))  ? Obj->GetStringField(TEXT("path"))  : FString();
		Ref.Name  = Obj->HasField(TEXT("name"))  ? Obj->GetStringField(TEXT("name"))  : FString();
		Ref.Class = Obj->HasField(TEXT("class")) ? Obj->GetStringField(TEXT("class")) : FString();
		Ref.bFocused = Obj->HasField(TEXT("focused")) ? Obj->GetBoolField(TEXT("focused")) : false;

		if (Ref.Path.IsEmpty())
		{
			continue;
		}

		// Look up in Asset Registry (no load)
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Ref.Path));
		if (!AssetData.IsValid())
		{
			// Try package name (path without the ".AssetName" suffix)
			FString PackageName = FPackageName::ObjectPathToPackageName(Ref.Path);
			TArray<FAssetData> AssetsInPackage;
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), AssetsInPackage, true);
			if (AssetsInPackage.Num() > 0)
			{
				AssetData = AssetsInPackage[0];
			}
		}

		if (!AssetData.IsValid())
		{
			// Mark missing, never silently drop
			Ref.bMissing = true;
			Out.Add(MoveTemp(Ref));
			continue;
		}

		// Fill Summary from cheap FAssetData tag reads (no load).
		// Each branch probes whether the tag actually exists; absent tag → empty Summary
		// (no fabrication). Tags verified against engine source:
		//   Blueprint/AnimBlueprint/WidgetBlueprint → "ParentClass" tag (FObjectPropertyBase path)
		//   DataTable                               → "RowStructure" tag (FTopLevelAssetPath string)
		//   Texture2D                               → "Dimensions" tag (e.g. "1024x512")
		// Other types: Summary remains empty (no guess).

		const FString ClassName = Ref.Class;

		if (ClassName == TEXT("Blueprint") || ClassName == TEXT("AnimBlueprint") || ClassName == TEXT("WidgetBlueprint"))
		{
			FAssetDataTagMapSharedView::FFindTagResult Tag = AssetData.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
			if (Tag.IsSet())
			{
				FString ParentRaw = Tag.GetValue();
				// Raw value is a full path like "/Script/Engine.Actor'" — extract short name after last '.'
				int32 DotIdx;
				if (ParentRaw.FindLastChar(TEXT('.'), DotIdx))
				{
					ParentRaw = ParentRaw.Mid(DotIdx + 1);
				}
				ParentRaw.RemoveFromEnd(TEXT("'"));
				if (!ParentRaw.IsEmpty())
				{
					Ref.Summary = FString::Printf(TEXT("parent: %s"), *ParentRaw);
				}
			}
		}
		else if (ClassName == TEXT("DataTable"))
		{
			FAssetDataTagMapSharedView::FFindTagResult Tag = AssetData.TagsAndValues.FindTag(FName(TEXT("RowStructure")));
			if (Tag.IsSet())
			{
				FString RowStructRaw = Tag.GetValue();
				// Value is a FTopLevelAssetPath string "/Script/Foo.Bar" — extract short name after last '.'
				int32 DotIdx;
				if (RowStructRaw.FindLastChar(TEXT('.'), DotIdx))
				{
					RowStructRaw = RowStructRaw.Mid(DotIdx + 1);
				}
				if (!RowStructRaw.IsEmpty())
				{
					Ref.Summary = FString::Printf(TEXT("row: %s"), *RowStructRaw);
				}
			}
		}
		else if (ClassName == TEXT("Texture2D"))
		{
			FAssetDataTagMapSharedView::FFindTagResult Tag = AssetData.TagsAndValues.FindTag(FName(TEXT("Dimensions")));
			if (Tag.IsSet())
			{
				FString Dims = Tag.GetValue();
				if (!Dims.IsEmpty())
				{
					Ref.Summary = Dims;  // Already formatted e.g. "1024x512"
				}
			}
		}
		// Material, MaterialInstance, StaticMesh, SkeletalMesh: tag inspection
		// shows no cheap reliable summary available without loading → Summary stays empty.

		Out.Add(MoveTemp(Ref));
	}
}

FString FKismetAIModule::BuildReferencesBlock(const FString& RefsJson, TArray<FString>& OutBlueprintPaths)
{
	OutBlueprintPaths.Reset();

	TArray<FAssetRef> Refs;
	ResolveRefs(RefsJson, Refs);

	// Collect Blueprint-class paths for snapshotting
	for (const FAssetRef& Ref : Refs)
	{
		if (!Ref.bMissing && (Ref.Class == TEXT("Blueprint") || Ref.Class == TEXT("AnimBlueprint") || Ref.Class == TEXT("WidgetBlueprint")))
		{
			OutBlueprintPaths.AddUnique(Ref.Path);
		}
	}

	return FormatReferencesBlock(Refs);
}

// ────────────────────────────────────────────────────────────────────────────
// search_assets handler
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoSearchAssets(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	// --- Read parameters ---
	FString Query           = Body->HasField(TEXT("query"))        ? Body->GetStringField(TEXT("query"))        : TEXT("");
	FString ClassFilter     = Body->HasField(TEXT("class_filter")) ? Body->GetStringField(TEXT("class_filter")) : TEXT("");
	FString Path            = Body->HasField(TEXT("path"))         ? Body->GetStringField(TEXT("path"))         : TEXT("");
	FString ParentClassFilter = Body->HasField(TEXT("parent_class")) ? Body->GetStringField(TEXT("parent_class")) : TEXT("");
	int32   Limit           = Body->HasField(TEXT("limit"))        ? static_cast<int32>(Body->GetNumberField(TEXT("limit"))) : 50;
	// 0 (the value OpenAI/Codex models fill optional ints with) means "use default".
	if (Limit <= 0)
	{
		Limit = 50;
	}
	Limit = FMath::Clamp(Limit, 1, 200);

	// --- Validate class filter against the canonical map ---
	if (!ClassFilter.IsEmpty() && !GetAssetClassMap().Contains(ClassFilter))
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_CLASS_FILTER"),
			FString::Printf(TEXT("Unknown class_filter: '%s'"), *ClassFilter),
			{TEXT("Use: Blueprint, WidgetBlueprint, AnimBlueprint, UserDefinedStruct, UserDefinedEnum, DataTable, Material, Texture2D, StaticMesh, SkeletalMesh, SoundWave, World, AnimSequence, AnimMontage, AnimComposite, BlendSpace, BlendSpace1D, AimOffsetBlendSpace, AimOffsetBlendSpace1D, Skeleton")});
	}

	// --- Core query via shared helper (Query + TypeFilter + /Game filter + sort) ---
	// Pass a generous cap so we can apply the HTTP-specific path/parent_class
	// post-filters before capping to Limit.
	TArray<FAssetData> Assets;
	QueryAssets(Query, ClassFilter, 10000, Assets);

	// --- HTTP-specific post-filters ---

	// Path filter (restrict to a sub-folder; not available to the bridge).
	if (!Path.IsEmpty())
	{
		Assets.RemoveAll([&Path](const FAssetData& A)
		{
			return !A.PackagePath.ToString().StartsWith(Path);
		});
	}

	// Parent class filter (Blueprints only).
	if (!ParentClassFilter.IsEmpty())
	{
		Assets.RemoveAll([&ParentClassFilter](const FAssetData& Asset) -> bool
		{
			FAssetDataTagMapSharedView::FFindTagResult ParentTag = Asset.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
			if (!ParentTag.IsSet())
			{
				return true; // exclude
			}
			FString ParentClassName = ParentTag.GetValue();
			int32 DotIdx;
			if (ParentClassName.FindLastChar(TEXT('.'), DotIdx))
			{
				ParentClassName = ParentClassName.Mid(DotIdx + 1);
			}
			ParentClassName.RemoveFromEnd(TEXT("'"));
			return ParentClassName.IsEmpty() || !ParentClassName.Contains(ParentClassFilter, ESearchCase::IgnoreCase);
		});
	}

	// QueryAssets already sorted by name; just cap and track truncation.
	int32 TotalCount = Assets.Num();
	bool bTruncated = TotalCount > Limit;
	if (bTruncated)
	{
		Assets.SetNum(Limit);
	}

	// --- Build response ---
	TArray<TSharedPtr<FJsonValue>> AssetsArray;
	AssetsArray.Reserve(Assets.Num());

	for (const FAssetData& Asset : Assets)
	{
		TSharedPtr<FJsonObject> AssetObj = MakeShared<FJsonObject>();
		AssetObj->SetStringField(TEXT("asset_path"),   Asset.GetObjectPathString());
		AssetObj->SetStringField(TEXT("asset_name"),   Asset.AssetName.ToString());
		AssetObj->SetStringField(TEXT("asset_type"),   Asset.AssetClassPath.GetAssetName().ToString());
		AssetObj->SetStringField(TEXT("package_path"), Asset.PackagePath.ToString());

		// Include parent class for Blueprints (tag read — no load).
		FAssetDataTagMapSharedView::FFindTagResult ParentTag = Asset.TagsAndValues.FindTag(FName(TEXT("ParentClass")));
		if (ParentTag.IsSet())
		{
			FString ParentClassName = ParentTag.GetValue();
			int32 DotIdx;
			if (ParentClassName.FindLastChar(TEXT('.'), DotIdx))
			{
				ParentClassName = ParentClassName.Mid(DotIdx + 1);
			}
			ParentClassName.RemoveFromEnd(TEXT("'"));
			AssetObj->SetStringField(TEXT("parent_class"), ParentClassName);
		}

		AssetsArray.Add(MakeShared<FJsonValueObject>(AssetObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), Assets.Num());
	Data->SetNumberField(TEXT("total"), TotalCount);
	Data->SetArrayField(TEXT("assets"), AssetsArray);
	if (bTruncated)
	{
		Data->SetBoolField(TEXT("truncated"), true);
	}

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoSearchAssets. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleSearchAssets(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("search_assets"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// get_references handler
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoGetReferences(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	// --- Read parameters ---
	FString AssetPath = Body->GetStringField(TEXT("asset_path"));
	FString Direction = Body->HasField(TEXT("direction")) ? Body->GetStringField(TEXT("direction")) : TEXT("referencers");
	bool bRecursive = Body->HasField(TEXT("recursive")) ? Body->GetBoolField(TEXT("recursive")) : false;
	bool bFilterEngine = Body->HasField(TEXT("filter_engine")) ? Body->GetBoolField(TEXT("filter_engine")) : true;
	int32 Limit = Body->HasField(TEXT("limit")) ? static_cast<int32>(Body->GetNumberField(TEXT("limit"))) : 50;
	Limit = FMath::Clamp(Limit, 1, 200);

	// Convert asset path to package name
	FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Verify asset exists
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(AssetPath));
	if (!AssetData.IsValid())
	{
		// Try with package name directly
		TArray<FAssetData> AssetsInPackage;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), AssetsInPackage, true);
		if (AssetsInPackage.Num() == 0)
		{
			return FToolExecutionResult::Error(404, TEXT("ASSET_NOT_FOUND"),
				FString::Printf(TEXT("Asset not found: '%s'"), *AssetPath),
				{TEXT("Use search_assets to find the correct asset path")});
		}
	}

	// --- Helper lambda to collect references in one direction ---
	auto CollectReferences = [&](bool bGetDependencies) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<FName> Results;
		TSet<FName> Visited;
		TArray<FName> Queue;
		Queue.Add(FName(*PackageName));
		Visited.Add(FName(*PackageName));

		int32 MaxDepth = bRecursive ? 10 : 1;
		int32 CurrentDepth = 0;

		while (Queue.Num() > 0 && CurrentDepth < MaxDepth)
		{
			TArray<FName> NextQueue;
			for (const FName& Current : Queue)
			{
				TArray<FName> Refs;
				if (bGetDependencies)
				{
					AssetRegistry.GetDependencies(Current, Refs);
				}
				else
				{
					AssetRegistry.GetReferencers(Current, Refs);
				}

				for (const FName& Ref : Refs)
				{
					if (Visited.Contains(Ref))
					{
						continue;
					}
					Visited.Add(Ref);

					FString RefStr = Ref.ToString();

					// Filter engine content
					if (bFilterEngine && !RefStr.StartsWith(TEXT("/Game")))
					{
						continue;
					}

					// Skip script/memory packages
					if (RefStr.StartsWith(TEXT("/Script/")) || RefStr.StartsWith(TEXT("/Memory/")))
					{
						continue;
					}

					Results.Add(Ref);

					if (bRecursive)
					{
						NextQueue.Add(Ref);
					}
				}
			}
			Queue = MoveTemp(NextQueue);
			CurrentDepth++;
		}

		// Build JSON array from results, capped at limit
		int32 Count = FMath::Min(Results.Num(), Limit);
		TArray<TSharedPtr<FJsonValue>> JsonArr;
		JsonArr.Reserve(Count);

		for (int32 i = 0; i < Count; i++)
		{
			TSharedPtr<FJsonObject> RefObj = MakeShared<FJsonObject>();
			FString RefPath = Results[i].ToString();
			RefObj->SetStringField(TEXT("path"), RefPath);

			// Look up class info
			TArray<FAssetData> RefAssets;
			AssetRegistry.GetAssetsByPackageName(Results[i], RefAssets, true);
			if (RefAssets.Num() > 0)
			{
				RefObj->SetStringField(TEXT("class"), RefAssets[0].AssetClassPath.GetAssetName().ToString());
				RefObj->SetStringField(TEXT("name"), RefAssets[0].AssetName.ToString());
			}

			JsonArr.Add(MakeShared<FJsonValueObject>(RefObj));
		}

		return JsonArr;
	};

	// --- Collect results based on direction ---
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	if (Direction == TEXT("dependencies") || Direction == TEXT("both"))
	{
		TArray<TSharedPtr<FJsonValue>> Deps = CollectReferences(true);
		Data->SetArrayField(TEXT("dependencies"), Deps);
		Data->SetNumberField(TEXT("dependencies_count"), Deps.Num());
	}

	if (Direction == TEXT("referencers") || Direction == TEXT("both"))
	{
		TArray<TSharedPtr<FJsonValue>> Refs = CollectReferences(false);
		Data->SetArrayField(TEXT("referencers"), Refs);
		Data->SetNumberField(TEXT("referencers_count"), Refs.Num());
	}

	if (bRecursive)
	{
		Data->SetBoolField(TEXT("recursive"), true);
	}

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoGetReferences. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleGetReferences(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("get_references"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// inspect_class handler
// ─────────────────────────────────────────────────────────────────────────────

namespace InspectClassHelpers
{
	/** Map an FProperty to a pin-compatible type string. */
	FString PropertyToPinType(const FProperty* Prop)
	{
		if (CastField<FBoolProperty>(Prop))      return TEXT("bool");
		if (CastField<FByteProperty>(Prop))       return TEXT("byte");
		if (CastField<FIntProperty>(Prop))        return TEXT("int");
		if (CastField<FInt64Property>(Prop))      return TEXT("int64");
		if (const FFloatProperty* F = CastField<FFloatProperty>(Prop))  return TEXT("float");
		if (const FDoubleProperty* D = CastField<FDoubleProperty>(Prop)) return TEXT("double");
		if (CastField<FStrProperty>(Prop))        return TEXT("string");
		if (CastField<FNameProperty>(Prop))       return TEXT("name");
		if (CastField<FTextProperty>(Prop))       return TEXT("text");

		if (const FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			if (ObjProp->PropertyClass)
				return FString::Printf(TEXT("object:%s"), *ObjProp->PropertyClass->GetName());
			return TEXT("object");
		}
		if (const FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Prop))
		{
			if (SoftProp->PropertyClass)
				return FString::Printf(TEXT("softobject:%s"), *SoftProp->PropertyClass->GetName());
			return TEXT("softobject");
		}
		if (const FClassProperty* ClsProp = CastField<FClassProperty>(Prop))
		{
			if (ClsProp->MetaClass)
				return FString::Printf(TEXT("class:%s"), *ClsProp->MetaClass->GetName());
			return TEXT("class");
		}
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			if (StructProp->Struct)
				return FString::Printf(TEXT("struct:%s"), *StructProp->Struct->GetName());
			return TEXT("struct");
		}
		if (const FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop))
		{
			if (EnumProp->GetEnum())
				return FString::Printf(TEXT("enum:%s"), *EnumProp->GetEnum()->GetName());
			return TEXT("enum");
		}
		if (const FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
		{
			return FString::Printf(TEXT("array<%s>"), *PropertyToPinType(ArrProp->Inner));
		}
		if (const FSetProperty* SetProp = CastField<FSetProperty>(Prop))
		{
			return FString::Printf(TEXT("set<%s>"), *PropertyToPinType(SetProp->ElementProp));
		}
		if (const FMapProperty* MapProp = CastField<FMapProperty>(Prop))
		{
			return FString::Printf(TEXT("map<%s, %s>"),
				*PropertyToPinType(MapProp->KeyProp),
				*PropertyToPinType(MapProp->ValueProp));
		}
		if (CastField<FDelegateProperty>(Prop))         return TEXT("delegate");
		if (CastField<FMulticastDelegateProperty>(Prop)) return TEXT("multicast_delegate");
		if (const FInterfaceProperty* IntProp = CastField<FInterfaceProperty>(Prop))
		{
			if (IntProp->InterfaceClass)
				return FString::Printf(TEXT("interface:%s"), *IntProp->InterfaceClass->GetName());
			return TEXT("interface");
		}

		return TEXT("unknown");
	}

	/** Try to resolve a class name to a UClass*. */
	UClass* ResolveClass(const FString& ClassName)
	{
		UClass* Result = nullptr;

		// 1. Try exact FindObject with common script prefixes
		static const TArray<FString> Prefixes = {
			TEXT("/Script/Engine."),
			TEXT("/Script/CoreUObject."),
			TEXT("/Script/UMG."),
			TEXT("/Script/AIModule."),
			TEXT("/Script/NavigationSystem."),
			TEXT("/Script/PhysicsCore."),
			TEXT("/Script/GameplayAbilities."),
			TEXT("/Script/EnhancedInput."),
			TEXT("/Script/Niagara."),
			TEXT(""),  // try as-is (for full paths)
		};

		// If it looks like a full path already
		if (ClassName.StartsWith(TEXT("/Script/")))
		{
			Result = FindObject<UClass>(nullptr, *ClassName);
			if (Result) return Result;
		}

		// Try short name with prefixes
		for (const FString& Prefix : Prefixes)
		{
			FString FullPath = Prefix + ClassName;
			Result = FindObject<UClass>(nullptr, *FullPath);
			if (Result) return Result;
		}

		// 2. Try StaticLoadClass for any remaining paths
		Result = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassName, nullptr, LOAD_None, nullptr);
		if (Result) return Result;

		// 3. Try as Blueprint path — load Blueprint and get GeneratedClass
		if (ClassName.StartsWith(TEXT("/Game/")))
		{
			FString BPPath = ClassName;
			if (!BPPath.EndsWith(TEXT("_C")))
			{
				// Try loading as Blueprint asset
				UBlueprint* BP = Cast<UBlueprint>(StaticLoadObject(UBlueprint::StaticClass(), nullptr, *BPPath));
				if (BP && BP->GeneratedClass)
				{
					return BP->GeneratedClass;
				}

				// Try with _C suffix (generated class path)
				FString GeneratedPath = BPPath + TEXT("_C");
				Result = StaticLoadClass(UObject::StaticClass(), nullptr, *GeneratedPath, nullptr, LOAD_None, nullptr);
				if (Result) return Result;
			}
			else
			{
				Result = StaticLoadClass(UObject::StaticClass(), nullptr, *BPPath, nullptr, LOAD_None, nullptr);
				if (Result) return Result;
			}
		}

		// 4. Try AnyPackage search as last resort
		Result = FindFirstObject<UClass>(*ClassName, EFindFirstObjectOptions::NativeFirst);

		return Result;
	}
}

FToolExecutionResult FKismetAIModule::DoInspectClass(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	// --- Read parameters ---
	FString ClassName = Body->GetStringField(TEXT("class_name"));
	FString Filter = Body->HasField(TEXT("filter")) ? Body->GetStringField(TEXT("filter")) : TEXT("all");
	bool bIncludeInherited = Body->HasField(TEXT("include_inherited")) ? Body->GetBoolField(TEXT("include_inherited")) : false;
	FString CategoryFilter = Body->HasField(TEXT("category")) ? Body->GetStringField(TEXT("category")) : TEXT("");
	FString Query = Body->HasField(TEXT("query")) ? Body->GetStringField(TEXT("query")) : TEXT("");
	int32 Limit = Body->HasField(TEXT("limit")) ? static_cast<int32>(Body->GetNumberField(TEXT("limit"))) : 30;
	Limit = FMath::Clamp(Limit, 1, 100);

	// --- Resolve class ---
	UClass* Class = InspectClassHelpers::ResolveClass(ClassName);
	if (!Class)
	{
		return FToolExecutionResult::Error(404, TEXT("CLASS_NOT_FOUND"),
			FString::Printf(TEXT("Could not resolve class: '%s'"), *ClassName),
			{TEXT("Try the full path format: /Script/Engine.ClassName"),
			 TEXT("For Blueprint classes, use the asset path: /Game/BP_MyClass"),
			 TEXT("Use search_assets to find the asset first")});
	}

	// --- Build response ---
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_name"), Class->GetName());
	Data->SetStringField(TEXT("full_path"), Class->GetPathName());

	// Parent class
	if (UClass* Super = Class->GetSuperClass())
	{
		Data->SetStringField(TEXT("parent_class"), Super->GetName());
	}

	// Interfaces
	TArray<TSharedPtr<FJsonValue>> InterfacesArr;
	for (const FImplementedInterface& Iface : Class->Interfaces)
	{
		if (Iface.Class)
		{
			InterfacesArr.Add(MakeShared<FJsonValueString>(Iface.Class->GetName()));
		}
	}
	if (InterfacesArr.Num() > 0)
	{
		Data->SetArrayField(TEXT("interfaces"), InterfacesArr);
	}

	// Iterator flags
	EFieldIterationFlags IterFlags = bIncludeInherited
		? EFieldIterationFlags::IncludeSuper
		: EFieldIterationFlags::None;

	// --- Properties ---
	if (Filter == TEXT("all") || Filter == TEXT("properties"))
	{
		TArray<TSharedPtr<FJsonValue>> PropsArr;
		int32 PropCount = 0;
		int32 TotalProps = 0;

		for (TFieldIterator<FProperty> It(Class, IterFlags); It; ++It)
		{
			FProperty* Prop = *It;

			// Only Blueprint-visible properties
			if (!Prop->HasAnyPropertyFlags(CPF_BlueprintVisible))
			{
				continue;
			}

			TotalProps++;

			FString PropName = Prop->GetName();

			// Apply query filter
			if (!Query.IsEmpty() && !PropName.Contains(Query, ESearchCase::IgnoreCase))
			{
				continue;
			}

			// Apply category filter
			if (!CategoryFilter.IsEmpty())
			{
				FString PropCategory = Prop->GetMetaData(TEXT("Category"));
				if (!PropCategory.Contains(CategoryFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			if (PropCount >= Limit)
			{
				continue; // Keep counting TotalProps but stop adding
			}

			TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
			PropObj->SetStringField(TEXT("name"), PropName);
			PropObj->SetStringField(TEXT("type"), InspectClassHelpers::PropertyToPinType(Prop));

			// Category
			FString Category = Prop->GetMetaData(TEXT("Category"));
			if (!Category.IsEmpty())
			{
				PropObj->SetStringField(TEXT("category"), Category);
			}

			// Flags
			TArray<TSharedPtr<FJsonValue>> Flags;
			if (Prop->HasAnyPropertyFlags(CPF_BlueprintReadOnly))
				Flags.Add(MakeShared<FJsonValueString>(TEXT("BlueprintReadOnly")));
			if (Prop->HasAnyPropertyFlags(CPF_Edit))
				Flags.Add(MakeShared<FJsonValueString>(TEXT("EditAnywhere")));
			if (Prop->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
				Flags.Add(MakeShared<FJsonValueString>(TEXT("EditDefaultsOnly")));
			if (Prop->HasAnyPropertyFlags(CPF_Net))
				Flags.Add(MakeShared<FJsonValueString>(TEXT("Replicated")));
			if (Flags.Num() > 0)
			{
				PropObj->SetArrayField(TEXT("flags"), Flags);
			}

			// Tooltip
			FString Tooltip = Prop->GetMetaData(TEXT("ToolTip"));
			if (!Tooltip.IsEmpty() && Tooltip.Len() <= 200)
			{
				PropObj->SetStringField(TEXT("tooltip"), Tooltip);
			}

			PropsArr.Add(MakeShared<FJsonValueObject>(PropObj));
			PropCount++;
		}

		Data->SetArrayField(TEXT("properties"), PropsArr);
		Data->SetNumberField(TEXT("properties_count"), PropCount);
		if (TotalProps > PropCount)
		{
			Data->SetNumberField(TEXT("properties_total"), TotalProps);
			Data->SetBoolField(TEXT("properties_truncated"), true);
		}
	}

	// --- Functions ---
	if (Filter == TEXT("all") || Filter == TEXT("functions"))
	{
		TArray<TSharedPtr<FJsonValue>> FuncsArr;
		int32 FuncCount = 0;
		int32 TotalFuncs = 0;

		for (TFieldIterator<UFunction> It(Class, IterFlags); It; ++It)
		{
			UFunction* Func = *It;

			// Only Blueprint-callable functions
			if (!Func->HasAnyFunctionFlags(FUNC_BlueprintCallable | FUNC_BlueprintEvent))
			{
				continue;
			}

			// Skip internal/hidden functions
			if (Func->HasMetaData(TEXT("BlueprintInternalUseOnly")))
			{
				continue;
			}

			TotalFuncs++;

			FString FuncName = Func->GetName();

			// Apply query filter
			if (!Query.IsEmpty() && !FuncName.Contains(Query, ESearchCase::IgnoreCase))
			{
				continue;
			}

			// Apply category filter
			if (!CategoryFilter.IsEmpty())
			{
				FString FuncCategory = Func->GetMetaData(TEXT("Category"));
				if (!FuncCategory.Contains(CategoryFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			if (FuncCount >= Limit)
			{
				continue;
			}

			TSharedPtr<FJsonObject> FuncObj = MakeShared<FJsonObject>();
			FuncObj->SetStringField(TEXT("name"), FuncName);

			// Category
			FString Category = Func->GetMetaData(TEXT("Category"));
			if (!Category.IsEmpty())
			{
				FuncObj->SetStringField(TEXT("category"), Category);
			}

			// Pure / Event flags
			bool bIsPure = Func->HasAnyFunctionFlags(FUNC_BlueprintPure);
			FuncObj->SetBoolField(TEXT("is_pure"), bIsPure);

			if (Func->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				FuncObj->SetBoolField(TEXT("is_event"), true);
			}

			if (Func->HasAnyFunctionFlags(FUNC_Static))
			{
				FuncObj->SetBoolField(TEXT("is_static"), true);
			}

			// Parameters
			TArray<TSharedPtr<FJsonValue>> ParamsArr;

			// Add exec pins for impure functions
			if (!bIsPure)
			{
				TSharedPtr<FJsonObject> ExecIn = MakeShared<FJsonObject>();
				ExecIn->SetStringField(TEXT("name"), TEXT("execute"));
				ExecIn->SetStringField(TEXT("direction"), TEXT("input"));
				ExecIn->SetStringField(TEXT("type"), TEXT("exec"));
				ParamsArr.Add(MakeShared<FJsonValueObject>(ExecIn));

				TSharedPtr<FJsonObject> ExecOut = MakeShared<FJsonObject>();
				ExecOut->SetStringField(TEXT("name"), TEXT("then"));
				ExecOut->SetStringField(TEXT("direction"), TEXT("output"));
				ExecOut->SetStringField(TEXT("type"), TEXT("exec"));
				ParamsArr.Add(MakeShared<FJsonValueObject>(ExecOut));
			}

			// Iterate function parameters
			for (TFieldIterator<FProperty> ParamIt(Func); ParamIt; ++ParamIt)
			{
				FProperty* Param = *ParamIt;

				TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
				FString ParamName = Param->GetName();

				// Return value
				if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					ParamObj->SetStringField(TEXT("name"), TEXT("ReturnValue"));
					ParamObj->SetStringField(TEXT("direction"), TEXT("output"));
				}
				else if (Param->HasAnyPropertyFlags(CPF_OutParm))
				{
					ParamObj->SetStringField(TEXT("name"), ParamName);
					ParamObj->SetStringField(TEXT("direction"), TEXT("output"));
				}
				else
				{
					ParamObj->SetStringField(TEXT("name"), ParamName);
					ParamObj->SetStringField(TEXT("direction"), TEXT("input"));
				}

				ParamObj->SetStringField(TEXT("type"), InspectClassHelpers::PropertyToPinType(Param));
				ParamsArr.Add(MakeShared<FJsonValueObject>(ParamObj));
			}

			FuncObj->SetArrayField(TEXT("parameters"), ParamsArr);

			// Tooltip
			FString Tooltip = Func->GetMetaData(TEXT("ToolTip"));
			if (!Tooltip.IsEmpty() && Tooltip.Len() <= 200)
			{
				FuncObj->SetStringField(TEXT("tooltip"), Tooltip);
			}

			FuncsArr.Add(MakeShared<FJsonValueObject>(FuncObj));
			FuncCount++;
		}

		Data->SetArrayField(TEXT("functions"), FuncsArr);
		Data->SetNumberField(TEXT("functions_count"), FuncCount);
		if (TotalFuncs > FuncCount)
		{
			Data->SetNumberField(TEXT("functions_total"), TotalFuncs);
			Data->SetBoolField(TEXT("functions_truncated"), true);
		}
	}

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoInspectClass. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleInspectClass(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("inspect_class"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// create_asset handler
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoCreateAsset(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	FString AssetType = Body->GetStringField(TEXT("asset_type"));
	FString Name = Body->GetStringField(TEXT("name"));
	FString Path = Body->GetStringField(TEXT("path"));

	// Refuse to create assets outside /Game/ (engine/plugin content).
	if (!Path.StartsWith(TEXT("/Game/")) && !(Path == TEXT("/Game")))
	{
		return FToolExecutionResult::Error(403, TEXT("OUT_OF_SCOPE"),
			FString::Printf(TEXT("path must be inside /Game/ (got '%s')"), *Path),
			{TEXT("New assets can only be created under /Game/")});
	}

	// Normalize path — remove trailing slash
	if (Path.EndsWith(TEXT("/")))
	{
		Path = Path.LeftChop(1);
	}

	// Check if asset already exists
	FString FullAssetPath = Path / Name;
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> ExistingAssets;
	AssetRegistry.GetAssetsByPackageName(FName(*FullAssetPath), ExistingAssets, true);
	if (ExistingAssets.Num() > 0)
	{
		return FToolExecutionResult::Error(409, TEXT("ASSET_ALREADY_EXISTS"),
			FString::Printf(TEXT("Asset already exists at '%s'"), *FullAssetPath),
			{TEXT("Use a different name or path"),
			 TEXT("Use search_assets to check what exists at this location")});
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	if (AssetType == TEXT("Blueprint"))
	{
		// --- Blueprint creation ---
		FString ParentClassName = Body->HasField(TEXT("parent_class")) ? Body->GetStringField(TEXT("parent_class")) : TEXT("Actor");
		FString BlueprintType = Body->HasField(TEXT("blueprint_type")) ? Body->GetStringField(TEXT("blueprint_type")) : TEXT("normal");

		// Resolve parent class
		UClass* ParentClass = nullptr;
		if (BlueprintType == TEXT("function_library"))
		{
			ParentClass = UBlueprintFunctionLibrary::StaticClass();
		}
		else if (BlueprintType != TEXT("interface"))
		{
			// Reuse the same resolution logic as inspect_class
			ParentClass = InspectClassHelpers::ResolveClass(ParentClassName);
			if (!ParentClass)
			{
				return FToolExecutionResult::Error(404, TEXT("PARENT_CLASS_NOT_FOUND"),
					FString::Printf(TEXT("Could not resolve parent class: '%s'"), *ParentClassName),
					{TEXT("Use inspect_class to verify the class exists"),
					 TEXT("Common classes: Actor, Character, Pawn, ActorComponent, PlayerController, GameModeBase")});
			}
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Create Blueprint")));

		UBlueprintFactory* Factory = NewObject<UBlueprintFactory>();
		Factory->ParentClass = ParentClass ? ParentClass : AActor::StaticClass();

		if (BlueprintType == TEXT("interface"))
		{
			Factory->BlueprintType = BPTYPE_Interface;
			Factory->ParentClass = UInterface::StaticClass();
		}
		else if (BlueprintType == TEXT("macro_library"))
		{
			Factory->BlueprintType = BPTYPE_MacroLibrary;
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* CreatedAsset = AssetTools.CreateAsset(Name, Path, UBlueprint::StaticClass(), Factory);

		if (!CreatedAsset)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"),
				TEXT("Failed to create Blueprint asset"),
				{TEXT("Check the output log for details")});
		}

		UBlueprint* Blueprint = Cast<UBlueprint>(CreatedAsset);
		Data->SetStringField(TEXT("asset_path"), Blueprint->GetPathName());
		Data->SetStringField(TEXT("asset_type"), TEXT("Blueprint"));
		Data->SetStringField(TEXT("blueprint_type"), BlueprintType);
		if (ParentClass)
		{
			Data->SetStringField(TEXT("parent_class"), ParentClass->GetName());
		}
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Created Blueprint '%s'. Use read_blueprint to inspect it, then write_blueprint to add nodes."), *Name));
	}
	else if (AssetType == TEXT("WidgetBlueprint"))
	{
		// --- Widget Blueprint creation ---
		FString ParentClassName = Body->HasField(TEXT("parent_class")) ? Body->GetStringField(TEXT("parent_class")) : TEXT("UserWidget");
		UClass* ParentClass = InspectClassHelpers::ResolveClass(ParentClassName);
		if (!ParentClass || !ParentClass->IsChildOf(UUserWidget::StaticClass()))
		{
			return FToolExecutionResult::Error(404, TEXT("PARENT_CLASS_NOT_FOUND"),
				FString::Printf(TEXT("Could not resolve '%s' as a UserWidget subclass"), *ParentClassName),
				{TEXT("Widget Blueprint parents must derive from UserWidget"),
				 TEXT("Omit parent_class to use UserWidget")});
		}

		// Root panel seeded after creation; "none" leaves the tree empty.
		FString RootWidgetName = Body->HasField(TEXT("root_widget")) ? Body->GetStringField(TEXT("root_widget")) : TEXT("CanvasPanel");
		UClass* RootClass = nullptr;
		if (RootWidgetName != TEXT("none"))
		{
			TArray<FString> Candidates;
			RootClass = KismetAIUtils::ResolveWidgetClass(RootWidgetName, &Candidates);
			if (!RootClass || !RootClass->IsChildOf(UPanelWidget::StaticClass()))
			{
				TArray<FString> Suggestions = {TEXT("root_widget must be a panel widget class (CanvasPanel, VerticalBox, HorizontalBox, Overlay, GridPanel...) or \"none\"")};
				for (const FString& Candidate : Candidates)
				{
					Suggestions.Add(FString::Printf(TEXT("Did you mean '%s'?"), *Candidate));
				}
				return FToolExecutionResult::Error(400, TEXT("INVALID_ROOT_WIDGET"),
					FString::Printf(TEXT("Could not resolve '%s' as a panel widget class"), *RootWidgetName),
					Suggestions);
			}
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Create Widget Blueprint")));

		UWidgetBlueprintFactory* Factory = NewObject<UWidgetBlueprintFactory>();
		Factory->ParentClass = ParentClass;

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* CreatedAsset = AssetTools.CreateAsset(Name, Path, UWidgetBlueprint::StaticClass(), Factory);

		UWidgetBlueprint* WidgetBP = Cast<UWidgetBlueprint>(CreatedAsset);
		if (!WidgetBP)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"),
				TEXT("Failed to create Widget Blueprint asset"),
				{TEXT("Check the output log for details")});
		}

		FString RootResult = TEXT("none");
		if (RootClass && WidgetBP->WidgetTree && !WidgetBP->WidgetTree->RootWidget)
		{
			WidgetBP->WidgetTree->Modify();
			UWidget* Root = WidgetBP->WidgetTree->ConstructWidget<UWidget>(RootClass);
			if (Root)
			{
				WidgetBP->WidgetTree->RootWidget = Root;
				// Register in the widget→guid map so later rename/replace/delete stay
				// consistent (the engine's add paths do this; ConstructWidget alone does not).
				WidgetBP->OnVariableAdded(Root->GetFName());
				RootResult = Root->GetClass()->GetName();
			}
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBP);
			FKismetEditorUtilities::CompileBlueprint(WidgetBP);
		}
		else if (WidgetBP->WidgetTree && WidgetBP->WidgetTree->RootWidget)
		{
			RootResult = WidgetBP->WidgetTree->RootWidget->GetClass()->GetName();
		}

		Data->SetStringField(TEXT("asset_path"), WidgetBP->GetPathName());
		Data->SetStringField(TEXT("asset_type"), TEXT("WidgetBlueprint"));
		Data->SetStringField(TEXT("parent_class"), ParentClass->GetName());
		Data->SetStringField(TEXT("root_widget"), RootResult);
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Created Widget Blueprint '%s'. Use read_widget to inspect the tree, edit_widget to add widgets, and the standard graph tools for its event graph."), *Name));
	}
	else if (AssetType == TEXT("UserDefinedStruct"))
	{
		// --- Struct creation ---
		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Create Struct")));

		FString PackagePath = Path / Name;
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"),
				TEXT("Failed to create package for struct"), {});
		}

		UUserDefinedStruct* Struct = FStructureEditorUtils::CreateUserDefinedStruct(Package, FName(*Name), RF_Public | RF_Standalone);
		if (!Struct)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"),
				TEXT("Failed to create User Defined Struct"), {});
		}

		// Add fields
		TArray<TSharedPtr<FJsonValue>> FieldsResult;
		const TArray<TSharedPtr<FJsonValue>>* FieldsArr = nullptr;
		if (Body->TryGetArrayField(TEXT("fields"), FieldsArr) && FieldsArr)
		{
			for (const auto& FieldVal : *FieldsArr)
			{
				auto FieldObj = FieldVal->AsObject();
				if (!FieldObj) continue;

				FString FieldName = FieldObj->GetStringField(TEXT("name"));
				FString FieldTypeStr = FieldObj->GetStringField(TEXT("type"));

				// Parse the type string to FEdGraphPinType
				FString TypeError;
				FEdGraphPinType PinType = KismetAIUtils::ParsePinTypeString(FieldTypeStr, &TypeError);
				if (!TypeError.IsEmpty())
				{
					// Skip field with bad type but report it
					TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
					FieldResult->SetStringField(TEXT("name"), FieldName);
					FieldResult->SetStringField(TEXT("error"), FString::Printf(TEXT("Invalid type '%s': %s"), *FieldTypeStr, *TypeError));
					FieldsResult.Add(MakeShared<FJsonValueObject>(FieldResult));
					continue;
				}

				// Add variable to struct
				bool bAdded = FStructureEditorUtils::AddVariable(Struct, PinType);
				if (bAdded)
				{
					// Get the GUID of the newly added variable (last in the array)
					const TArray<FStructVariableDescription>& VarDescs = FStructureEditorUtils::GetVarDesc(Struct);
					if (VarDescs.Num() > 0)
					{
						FGuid NewVarGuid = VarDescs.Last().VarGuid;

						// Rename to the desired name
						FStructureEditorUtils::RenameVariable(Struct, NewVarGuid, FieldName);

						// Set default value if provided
						if (FieldObj->HasField(TEXT("default_value")))
						{
							FString DefaultValue = FieldObj->GetStringField(TEXT("default_value"));
							FStructureEditorUtils::ChangeVariableDefaultValue(Struct, NewVarGuid, DefaultValue);
						}

						TSharedPtr<FJsonObject> FieldResult = MakeShared<FJsonObject>();
						FieldResult->SetStringField(TEXT("name"), FieldName);
						FieldResult->SetStringField(TEXT("type"), FieldTypeStr);
						FieldsResult.Add(MakeShared<FJsonValueObject>(FieldResult));
					}
				}
			}

			// Compile the struct after all fields are added
			FStructureEditorUtils::CompileStructure(Struct);
		}

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(Struct);
		Struct->MarkPackageDirty();

		Data->SetStringField(TEXT("asset_path"), Struct->GetPathName());
		Data->SetStringField(TEXT("asset_type"), TEXT("UserDefinedStruct"));
		Data->SetArrayField(TEXT("fields"), FieldsResult);
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Created struct '%s' with %d fields. Compiled successfully."), *Name, FieldsResult.Num()));
	}
	else if (AssetType == TEXT("UserDefinedEnum"))
	{
		// --- Enum creation ---
		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Create Enum")));

		FString PackagePath = Path / Name;
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"),
				TEXT("Failed to create package for enum"), {});
		}

		UUserDefinedEnum* Enum = Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package, FName(*Name), RF_Public | RF_Standalone));
		if (!Enum)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"),
				TEXT("Failed to create User Defined Enum"), {});
		}

		// Add entries
		TArray<TSharedPtr<FJsonValue>> EntriesResult;
		const TArray<TSharedPtr<FJsonValue>>* EntriesArr = nullptr;
		if (Body->TryGetArrayField(TEXT("entries"), EntriesArr) && EntriesArr)
		{
			for (int32 i = 0; i < EntriesArr->Num(); i++)
			{
				FString EntryName = (*EntriesArr)[i]->AsString();
				if (EntryName.IsEmpty()) continue;

				// UE creates one default entry — reuse it for the first, add new ones for the rest
				if (i > 0)
				{
					FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
				}

				// Set the display name (index maps to enumerator index, skip the hidden MAX entry)
				bool bRenamed = FEnumEditorUtils::SetEnumeratorDisplayName(Enum, i, FText::FromString(EntryName));

				TSharedPtr<FJsonObject> EntryResult = MakeShared<FJsonObject>();
				EntryResult->SetStringField(TEXT("name"), EntryName);
				EntryResult->SetNumberField(TEXT("index"), i);
				EntriesResult.Add(MakeShared<FJsonValueObject>(EntryResult));
			}
		}

		// Bitflags
		if (Body->HasField(TEXT("is_bitflags")) && Body->GetBoolField(TEXT("is_bitflags")))
		{
			FEnumEditorUtils::SetEnumeratorBitflagsTypeState(Enum, true);
		}

		// Notify asset registry
		FAssetRegistryModule::AssetCreated(Enum);
		Enum->MarkPackageDirty();

		Data->SetStringField(TEXT("asset_path"), Enum->GetPathName());
		Data->SetStringField(TEXT("asset_type"), TEXT("UserDefinedEnum"));
		Data->SetArrayField(TEXT("entries"), EntriesResult);
		if (Body->HasField(TEXT("is_bitflags")) && Body->GetBoolField(TEXT("is_bitflags")))
		{
			Data->SetBoolField(TEXT("is_bitflags"), true);
		}
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Created enum '%s' with %d entries."), *Name, EntriesResult.Num()));
	}
	else if (AssetType == TEXT("Material"))
	{
		// --- Base Material creation ---
		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Create Material")));

		UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Created = AssetTools.CreateAsset(Name, Path, UMaterial::StaticClass(), Factory);
		if (!Created)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"), TEXT("Failed to create Material asset"),
				{TEXT("Check the output log for details")});
		}

		Created->MarkPackageDirty();
		Data->SetStringField(TEXT("asset_path"), Created->GetPathName());
		Data->SetStringField(TEXT("asset_type"), TEXT("Material"));
		Data->SetStringField(TEXT("message"),
			TEXT("Created Material. Build the graph with create_material_expression + edit_material (connect to BaseColor/Roughness/Normal/…), then compile_material. Use manage_material to set shading model (e.g. Unlit) or blend mode."));
	}
	else if (AssetType == TEXT("MaterialFunction"))
	{
		// --- Material Function creation ---
		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Create Material Function")));

		UMaterialFunctionFactoryNew* Factory = NewObject<UMaterialFunctionFactoryNew>();
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Created = AssetTools.CreateAsset(Name, Path, UMaterialFunction::StaticClass(), Factory);
		if (!Created)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"), TEXT("Failed to create Material Function asset"),
				{TEXT("Check the output log for details")});
		}

		Created->MarkPackageDirty();
		Data->SetStringField(TEXT("asset_path"), Created->GetPathName());
		Data->SetStringField(TEXT("asset_type"), TEXT("MaterialFunction"));
		Data->SetStringField(TEXT("message"),
			TEXT("Created Material Function. Add FunctionInput/FunctionOutput + logic via create_material_expression (in-material-function authoring), then call it from a material."));
	}
	else if (AssetType == TEXT("AnimBlueprint") || AssetType == TEXT("AnimMontage") || AssetType == TEXT("AnimComposite")
		|| AssetType == TEXT("BlendSpace") || AssetType == TEXT("BlendSpace1D")
		|| AssetType == TEXT("AimOffsetBlendSpace") || AssetType == TEXT("AimOffsetBlendSpace1D")
		|| AssetType == TEXT("AnimSequence"))
	{
		// ── Animation assets ──────────────────────────────────────────
		// Resolve the target skeleton (required for everything except a template Anim Blueprint).
		const bool bTemplate = AssetType == TEXT("AnimBlueprint") && Body->HasField(TEXT("template")) && Body->GetBoolField(TEXT("template"));
		USkeleton* Skeleton = nullptr;
		if (Body->HasField(TEXT("skeleton")) && !Body->GetStringField(TEXT("skeleton")).IsEmpty())
		{
			const FString SkeletonPath = Body->GetStringField(TEXT("skeleton"));
			Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
			if (!Skeleton)
			{
				return FToolExecutionResult::Error(404, TEXT("SKELETON_NOT_FOUND"),
					FString::Printf(TEXT("Could not load skeleton: '%s'"), *SkeletonPath),
					{ TEXT("Pass 'skeleton' as the full path to a USkeleton asset, e.g. /Game/Anim/SK_Hero_Skeleton"),
					  TEXT("Use search_assets with class_filter=\"Skeleton\" to find one") });
			}
		}
		if (!Skeleton && !bTemplate)
		{
			return FToolExecutionResult::Error(400, TEXT("SKELETON_REQUIRED"),
				FString::Printf(TEXT("'skeleton' is required for %s"), *AssetType),
				{ TEXT("Pass 'skeleton' as the path to a USkeleton asset"),
				  AssetType == TEXT("AnimBlueprint") ? TEXT("Or set template:true for a skeleton-agnostic Anim Blueprint") : TEXT("All animation assets bind to a skeleton") });
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Create Animation Asset")));
		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

		UObject* Created = nullptr;
		UClass* AssetClass = nullptr;
		FString FollowUp;

		if (AssetType == TEXT("AnimBlueprint"))
		{
			FString ParentClassName = Body->HasField(TEXT("parent_class")) ? Body->GetStringField(TEXT("parent_class")) : TEXT("AnimInstance");
			UClass* ParentClass = InspectClassHelpers::ResolveClass(ParentClassName);
			if (!ParentClass)
			{
				ParentClass = UAnimInstance::StaticClass();
			}
			if (!ParentClass->IsChildOf(UAnimInstance::StaticClass()))
			{
				return FToolExecutionResult::Error(400, TEXT("PARENT_CLASS_INVALID"),
					FString::Printf(TEXT("Anim Blueprint parent '%s' must derive from AnimInstance"), *ParentClassName),
					{ TEXT("Omit parent_class to use AnimInstance") });
			}
			UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
			Factory->ParentClass = ParentClass;
			Factory->TargetSkeleton = Skeleton;     // may be null when bTemplate
			Factory->bTemplate = bTemplate;
			Factory->BlueprintType = BPTYPE_Normal;
			AssetClass = UAnimBlueprint::StaticClass();
			Created = AssetTools.CreateAsset(Name, Path, AssetClass, Factory);
			FollowUp = TEXT("Author its AnimGraph with read_blueprint/write_blueprint/insert_node and manage_state_machine (discover_tools \"blueprint_tools\").");
		}
		else if (AssetType == TEXT("AnimMontage") || AssetType == TEXT("AnimComposite"))
		{
			UAnimSequence* SourceAnim = nullptr;
			if (Body->HasField(TEXT("source_animation")) && !Body->GetStringField(TEXT("source_animation")).IsEmpty())
			{
				const FString SrcPath = Body->GetStringField(TEXT("source_animation"));
				SourceAnim = LoadObject<UAnimSequence>(nullptr, *SrcPath);
				if (!SourceAnim)
				{
					return FToolExecutionResult::Error(404, TEXT("SOURCE_ANIM_NOT_FOUND"),
						FString::Printf(TEXT("Could not load source_animation: '%s'"), *SrcPath), {});
				}
				if (SourceAnim->GetSkeleton() && !Skeleton->IsCompatibleForEditor(SourceAnim->GetSkeleton()))
				{
					return FToolExecutionResult::Error(400, TEXT("SKELETON_MISMATCH"),
						FString::Printf(TEXT("source_animation '%s' uses an incompatible skeleton"), *SrcPath), {});
				}
			}
			UFactory* Factory = nullptr;
			if (AssetType == TEXT("AnimMontage"))
			{
				UAnimMontageFactory* F = NewObject<UAnimMontageFactory>();
				F->TargetSkeleton = Skeleton;
				F->SourceAnimation = SourceAnim;
				Factory = F;
				AssetClass = UAnimMontage::StaticClass();
				FollowUp = TEXT("Edit with manage_animation: add_slot_track, add_segment, add_section, set_sync_group, add_notify.");
			}
			else
			{
				UAnimCompositeFactory* F = NewObject<UAnimCompositeFactory>();
				F->TargetSkeleton = Skeleton;
				F->SourceAnimation = SourceAnim;
				Factory = F;
				AssetClass = UAnimComposite::StaticClass();
				FollowUp = TEXT("Edit with manage_animation: add_segment, add_notify.");
			}
			Created = AssetTools.CreateAsset(Name, Path, AssetClass, Factory);
		}
		else if (AssetType == TEXT("BlendSpace") || AssetType == TEXT("BlendSpace1D")
			|| AssetType == TEXT("AimOffsetBlendSpace") || AssetType == TEXT("AimOffsetBlendSpace1D"))
		{
			UFactory* Factory = nullptr;
			if (AssetType == TEXT("BlendSpace"))
			{
				UBlendSpaceFactoryNew* F = NewObject<UBlendSpaceFactoryNew>(); F->TargetSkeleton = Skeleton; Factory = F; AssetClass = UBlendSpace::StaticClass();
			}
			else if (AssetType == TEXT("BlendSpace1D"))
			{
				UBlendSpaceFactory1D* F = NewObject<UBlendSpaceFactory1D>(); F->TargetSkeleton = Skeleton; Factory = F; AssetClass = UBlendSpace1D::StaticClass();
			}
			else if (AssetType == TEXT("AimOffsetBlendSpace"))
			{
				UAimOffsetBlendSpaceFactoryNew* F = NewObject<UAimOffsetBlendSpaceFactoryNew>(); F->TargetSkeleton = Skeleton; Factory = F; AssetClass = UAimOffsetBlendSpace::StaticClass();
			}
			else
			{
				UAimOffsetBlendSpaceFactory1D* F = NewObject<UAimOffsetBlendSpaceFactory1D>(); F->TargetSkeleton = Skeleton; Factory = F; AssetClass = UAimOffsetBlendSpace1D::StaticClass();
			}
			Created = AssetTools.CreateAsset(Name, Path, AssetClass, Factory);
			FollowUp = TEXT("Configure axes (set_axis) then place samples (add_sample) with manage_animation.");
		}
		else // AnimSequence
		{
			UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
			Factory->TargetSkeleton = Skeleton;
			AssetClass = UAnimSequence::StaticClass();
			Created = AssetTools.CreateAsset(Name, Path, AssetClass, Factory);
			FollowUp = TEXT("Empty 1-frame sequence. Add notifies with manage_animation (add_notify).");
		}

		if (!Created)
		{
			return FToolExecutionResult::Error(500, TEXT("CREATE_FAILED"),
				FString::Printf(TEXT("Failed to create %s asset"), *AssetType),
				{ TEXT("Check the output log for details") });
		}

		Created->MarkPackageDirty();
		{
			// A brand-new package cannot hold earlier human edits.
			FKismetAIMutationOutcome Outcome = BeginMutationOutcome(/*bPackageWasDirtyBefore*/ false);
			Outcome.bAppliedInMemory = true;
			AutoSaveAfterMutation(Created, Outcome);
			AttachMutationOutcome(Data, Outcome);
		}

		Data->SetStringField(TEXT("asset_path"), Created->GetPathName());
		Data->SetStringField(TEXT("asset_type"), AssetType);
		if (Skeleton)
		{
			Data->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
		}
		if (bTemplate)
		{
			Data->SetBoolField(TEXT("template"), true);
		}
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Created %s '%s'. %s"), *AssetType, *Name, *FollowUp));
	}

	// Branches that do not autosave (Blueprints, widgets, structs, enums,
	// materials) still say so: the new asset exists only in memory until saved.
	if (!Data->HasField(TEXT("outcome")))
	{
		FString CreatedPath;
		if (Data->TryGetStringField(TEXT("asset_path"), CreatedPath) && !CreatedPath.IsEmpty())
		{
			FKismetAIMutationOutcome Outcome = BeginMutationOutcome(/*bPackageWasDirtyBefore*/ false);
			Outcome.bAppliedInMemory = true;
			Outcome.PackageName = FPackageName::ObjectPathToPackageName(CreatedPath);
			Outcome.SaveSkipReason = KismetAISaveSkipReason::NotAutosaved;
			Outcome.SavePolicy = KismetAISavePolicy::NoAutosave;
			AttachMutationOutcome(Data, Outcome);
		}
	}

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoCreateAsset. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleCreateAsset(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("create_asset"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// manage_asset handler
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoManageAsset(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	FString Action = Body->GetStringField(TEXT("action"));
	FString AssetPath = Body->GetStringField(TEXT("asset_path"));
	FString NewName = Body->HasField(TEXT("new_name")) ? Body->GetStringField(TEXT("new_name")) : TEXT("");
	FString NewPath = Body->HasField(TEXT("new_path")) ? Body->GetStringField(TEXT("new_path")) : TEXT("");

	// Refuse to operate on engine/plugin content. The /Game/-only referencer
	// check on delete looked at outgoing references but not at the asset path
	// itself — an asset referenced only from /Engine/ would report zero /Game/
	// referencers and proceed to ObjectTools::DeleteAssets. Hard-gate the path
	// up-front so no action (delete/rename/move/duplicate/save) can target
	// anything outside /Game/.
	if (!AssetPath.StartsWith(TEXT("/Game/")))
	{
		return FToolExecutionResult::Error(403, TEXT("OUT_OF_SCOPE"),
			FString::Printf(TEXT("Only /Game/ assets can be managed (got '%s')"), *AssetPath),
			{TEXT("Engine, plugin, and script content is read-only via this tool")});
	}
	if (!NewPath.IsEmpty() && !NewPath.StartsWith(TEXT("/Game/")))
	{
		return FToolExecutionResult::Error(403, TEXT("OUT_OF_SCOPE"),
			FString::Printf(TEXT("new_path must stay inside /Game/ (got '%s')"), *NewPath),
			{TEXT("Pick a destination under /Game/")});
	}

	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();

	// Load the asset
	FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);

	// If not found by object path, try loading by package name
	if (!Asset)
	{
		TArray<FAssetData> AssetsInPackage;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), AssetsInPackage, true);
		if (AssetsInPackage.Num() > 0)
		{
			Asset = AssetsInPackage[0].GetAsset();
		}
	}

	if (!Asset)
	{
		return FToolExecutionResult::Error(404, TEXT("ASSET_NOT_FOUND"),
			FString::Printf(TEXT("Asset not found: '%s'"), *AssetPath),
			{TEXT("Use search_assets to find the correct asset path")});
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	if (Action == TEXT("duplicate"))
	{
		FString DestPath = NewPath.IsEmpty() ? FPackageName::GetLongPackagePath(Asset->GetPathName()) : NewPath;
		if (DestPath.EndsWith(TEXT("/")))
		{
			DestPath = DestPath.LeftChop(1);
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Duplicate Asset")));

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		UObject* Duplicate = AssetTools.DuplicateAsset(NewName, DestPath, Asset);

		if (!Duplicate)
		{
			return FToolExecutionResult::Error(500, TEXT("DUPLICATE_FAILED"),
				TEXT("Failed to duplicate asset"),
				{TEXT("Check the output log for details")});
		}

		Data->SetStringField(TEXT("action"), TEXT("duplicate"));
		Data->SetStringField(TEXT("source"), AssetPath);
		Data->SetStringField(TEXT("result_path"), Duplicate->GetPathName());
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Duplicated '%s' to '%s'"), *AssetPath, *Duplicate->GetPathName()));
	}
	else if (Action == TEXT("rename") || Action == TEXT("move"))
	{
		FString OldPackagePath = FPackageName::GetLongPackagePath(Asset->GetOutermost()->GetPathName());
		FString TargetName = NewName.IsEmpty() ? Asset->GetName() : NewName;
		FString TargetPath = NewPath.IsEmpty() ? OldPackagePath : NewPath;
		if (TargetPath.EndsWith(TEXT("/")))
		{
			TargetPath = TargetPath.LeftChop(1);
		}

		FScopedTransaction Transaction(FText::FromString(
			Action == TEXT("rename") ? TEXT("Kismet: Rename Asset") : TEXT("Kismet: Move Asset")));

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();

		TArray<FAssetRenameData> RenameData;
		RenameData.Emplace(Asset, TargetPath, TargetName);

		bool bSuccess = AssetTools.RenameAssets(RenameData);
		if (!bSuccess)
		{
			return FToolExecutionResult::Error(500, TEXT("RENAME_FAILED"),
				FString::Printf(TEXT("Failed to %s asset"), *Action),
				{TEXT("A name collision may exist at the target path"),
				 TEXT("Use search_assets to check what exists there")});
		}

		Data->SetStringField(TEXT("action"), Action);
		Data->SetStringField(TEXT("source"), AssetPath);
		Data->SetStringField(TEXT("result_path"), TargetPath / TargetName);
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("%s '%s' to '%s'"),
				Action == TEXT("rename") ? TEXT("Renamed") : TEXT("Moved"),
				*AssetPath, *(TargetPath / TargetName)));
	}
	else if (Action == TEXT("delete"))
	{
		// Check for referencers first — refuse if any exist
		TArray<FName> Referencers;
		AssetRegistry.GetReferencers(FName(*PackageName), Referencers);

		// Filter to only /Game/ referencers
		TArray<FString> GameReferencers;
		for (const FName& Ref : Referencers)
		{
			FString RefStr = Ref.ToString();
			if (RefStr.StartsWith(TEXT("/Game")) && RefStr != PackageName)
			{
				GameReferencers.Add(RefStr);
			}
		}

		if (GameReferencers.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> RefsArr;
			for (const FString& Ref : GameReferencers)
			{
				RefsArr.Add(MakeShared<FJsonValueString>(Ref));
			}

			TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
			ErrorData->SetStringField(TEXT("error_code"), TEXT("HAS_REFERENCERS"));
			ErrorData->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Cannot delete '%s': %d asset(s) reference it"), *AssetPath, GameReferencers.Num()));
			ErrorData->SetArrayField(TEXT("referencers"), RefsArr);

			TArray<FString> Suggestions;
			Suggestions.Add(TEXT("Use get_references to understand the full dependency graph"));
			Suggestions.Add(TEXT("Remove references before deleting"));
			ErrorData->SetArrayField(TEXT("suggestions"), MakeShared<FJsonObject>()->GetArrayField(TEXT("_unused_")));

			// Build error response manually
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetBoolField(TEXT("success"), false);
			Root->SetObjectField(TEXT("error"), ErrorData);

			TArray<TSharedPtr<FJsonValue>> SuggestionsArr;
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Use get_references to understand the full dependency graph")));
			SuggestionsArr.Add(MakeShared<FJsonValueString>(TEXT("Remove references before deleting")));
			ErrorData->SetArrayField(TEXT("suggestions"), SuggestionsArr);

			return FToolExecutionResult::Error(409, TEXT("HAS_REFERENCERS"),
				FString::Printf(TEXT("Cannot delete '%s': %d asset(s) reference it. Referencers: %s"),
					*AssetPath, GameReferencers.Num(), *FString::Join(GameReferencers, TEXT(", "))),
				{TEXT("Use get_references to understand the full dependency graph"),
				 TEXT("Remove references before deleting")});
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("Kismet: Delete Asset")));

		TArray<FAssetData> AssetsToDelete;
		FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(Asset->GetPathName()));
		if (AssetData.IsValid())
		{
			AssetsToDelete.Add(AssetData);
		}
		else
		{
			// Fallback: get by package name
			AssetRegistry.GetAssetsByPackageName(FName(*PackageName), AssetsToDelete, true);
		}

		int32 Deleted = ObjectTools::DeleteAssets(AssetsToDelete, false);

		Data->SetStringField(TEXT("action"), TEXT("delete"));
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetNumberField(TEXT("deleted_count"), Deleted);
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Deleted '%s'"), *AssetPath));
	}
	else if (Action == TEXT("save"))
	{
		UPackage* Package = Asset->GetOutermost();
		if (!Package)
		{
			return FToolExecutionResult::Error(500, TEXT("SAVE_FAILED"),
				TEXT("Could not get package for asset"), {});
		}

		// Same save boundary as autosave (and the same failure injection seam).
		// An explicit save writes whatever is dirty in the package, so the
		// pre-existing-changes flag here means "the package had unsaved edits".
		FKismetAIMutationOutcome Outcome = BeginMutationOutcome(Package->IsDirty());
		KismetAIPersistence::SaveExplicitly(Asset, Outcome);

		if (Outcome.SaveStatus != EKismetAISaveStatus::Saved)
		{
			const FString Reason = Outcome.SaveStatus == EKismetAISaveStatus::Failed
				? Outcome.SaveError
				: FString(TEXT("the asset has no file on disk"));
			return FToolExecutionResult::Error(500, TEXT("SAVE_FAILED"),
				FString::Printf(TEXT("Failed to save '%s': %s. Unsaved changes remain in the open editor only."), *AssetPath, *Reason),
				{TEXT("The file may be read-only or locked by source control")});
		}

		AttachMutationOutcome(Data, Outcome);
		Data->SetStringField(TEXT("action"), TEXT("save"));
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("file_path"), Outcome.PackageFilename);
		Data->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Saved '%s' to disk"), *AssetPath));
	}

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoManageAsset. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleManageAsset(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("manage_asset"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// Animation node-spawner helpers — shared by search_nodes and insert_node so the
// two stay consistent about which spawners belong in an anim graph.
//
// Why a dedicated layer: a UAnimGraphNode_* only reports CanCreateUnderSpecifiedSchema
// == true under a UAnimationGraphSchema (AnimGraphNode_Base.cpp), while plain K2 nodes
// return true under EVERY schema. So the schema check alone is asymmetric — it keeps
// anim nodes OUT of K2 graphs but does NOT keep K2 nodes OUT of anim graphs. We pair
// it with a node-class test (child of UAnimGraphNode_Base) to enforce both directions.
// ─────────────────────────────────────────────────────────────────────────────

namespace AnimSpawnerHelpers
{
	// A graph that accepts anim (pose) nodes: its schema is a UAnimationGraphSchema.
	// Covers the main AnimGraph, state bound graphs, transition graphs — anywhere a
	// UAnimGraphNode_* is legal.
	static bool IsAnimNodeGraph(const UEdGraph* Graph)
	{
		if (!Graph) return false;
		const UEdGraphSchema* Schema = Graph->GetSchema();
		return Schema && Schema->IsA<UAnimationGraphSchema>();
	}

	// The spawner would produce a UAnimGraphNode_* (an animation pose node). Read off
	// NodeClass so we never have to invoke the spawner just to classify it.
	static bool IsAnimNodeSpawner(const UBlueprintNodeSpawner* Spawner)
	{
		return Spawner && Spawner->NodeClass && Spawner->NodeClass->IsChildOf(UAnimGraphNode_Base::StaticClass());
	}

	// Does this node carry any exec pin? Anim graphs are GT_Animation and do not support
	// impure (exec) functions, so an exec pin is exactly what makes a K2 node illegal in an
	// anim graph. Pure data nodes (variable getters, pure functions, knots, struct breaks)
	// carry no exec pin and are legal pose-graph data sources.
	static bool NodeHasExecPin(const UEdGraphNode* Node)
	{
		if (!Node) return false;
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return true;
			}
		}
		return false;
	}

	// A node spawner that produces a PURE (no exec) K2 node, hence legal in an anim graph:
	// variable getters (UBlueprintVariableNodeSpawner produce a getter; setters are impure
	// but only getters match the "Get X" query path) and BlueprintPure function spawners.
	// Other K2 spawners (impure function calls, flow control, casts) are excluded. NodeClass
	// is read without invoking the spawner.
	static bool IsPureK2Spawner(const UBlueprintNodeSpawner* Spawner)
	{
		if (!Spawner) return false;
		if (const UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
		{
			const UFunction* Func = FuncSpawner->GetFunction();
			return Func && Func->HasAnyFunctionFlags(FUNC_BlueprintPure);
		}
		// Variable GETTERS are pure; the getter node class is UK2Node_VariableGet.
		if (Spawner->NodeClass && Spawner->NodeClass->IsChildOf(UK2Node_VariableGet::StaticClass()))
		{
			return true;
		}
		return false;
	}

	// If this is a per-asset spawner ("Play 'Run_Fwd'") whose asset is an animation
	// asset, return its FAssetData (WITHOUT loading the asset — loading mid action-DB
	// iteration could refresh the database we're walking). bOutIsAnimAssetSpawner reports
	// whether it was an asset spawner targeting an animation asset.
	static FAssetData GetAnimAssetDataFromSpawner(const UBlueprintNodeSpawner* Spawner, bool& bOutIsAnimAssetSpawner)
	{
		bOutIsAnimAssetSpawner = false;
		const UBlueprintAssetNodeSpawner* AssetSpawner = Cast<UBlueprintAssetNodeSpawner>(Spawner);
		if (!AssetSpawner) return FAssetData();
		const FAssetData& AssetData = AssetSpawner->GetAssetData();
		if (!AssetData.IsValid()) return FAssetData();
		const UClass* AssetClass = AssetData.GetClass();
		if (!AssetClass || !AssetClass->IsChildOf(UAnimationAsset::StaticClass())) return FAssetData();
		bOutIsAnimAssetSpawner = true;
		return AssetData;
	}

	// Reject an asset whose skeleton is incompatible with the anim Blueprint's
	// TargetSkeleton — the same rule the editor's action menu applies. Uses the
	// FAssetData "Skeleton" tag so no asset load is forced. Returns true if compatible;
	// fills OutError naming the skeletons if not.
	static bool CheckSkeletonCompatible(UAnimBlueprint* AnimBP, const FAssetData& AssetData, FString& OutError)
	{
		if (!AssetData.IsValid()) return true;
		USkeleton* BPSkeleton = AnimBP ? AnimBP->TargetSkeleton : nullptr;
		if (BPSkeleton == nullptr)
		{
			// No target skeleton (e.g. a template anim BP): the editor would still allow
			// the action, so we do too rather than block authoring.
			return true;
		}
		// Tag-based compatibility check — does not load the asset.
		if (!BPSkeleton->IsCompatibleForEditor(AssetData))
		{
			FString AssetSkeletonStr;
			AssetData.GetTagValue(TEXT("Skeleton"), AssetSkeletonStr);
			OutError = FString::Printf(
				TEXT("Skeleton mismatch: asset '%s' uses skeleton '%s' but this Animation Blueprint targets '%s'. Pick an asset built for the compatible skeleton."),
				*AssetData.AssetName.ToString(),
				AssetSkeletonStr.IsEmpty() ? TEXT("(unknown)") : *AssetSkeletonStr,
				*BPSkeleton->GetName());
			return false;
		}
		return true;
	}

	// "local" / "component" / "" for a pin's pose space (empty when not a pose pin).
	static FString PoseSpaceString(const FEdGraphPinType& PinType)
	{
		if (UAnimationGraphSchema::IsComponentSpacePosePin(PinType)) return TEXT("component");
		if (UAnimationGraphSchema::IsLocalSpacePosePin(PinType)) return TEXT("local");
		if (UAnimationGraphSchema::IsPosePin(PinType)) return TEXT("local");
		return FString();
	}

	// First pose INPUT pin on a node (declaration order), or null if it has none.
	static UEdGraphPin* GetFirstPoseInputPin(UEdGraphNode* Node)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input && UAnimationGraphSchema::IsPosePin(Pin->PinType))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	// First pose OUTPUT pin on a node, or null.
	static UEdGraphPin* GetFirstPoseOutputPin(UEdGraphNode* Node)
	{
		if (!Node) return nullptr;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && UAnimationGraphSchema::IsPosePin(Pin->PinType))
			{
				return Pin;
			}
		}
		return nullptr;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// manage_animation handler — edit/read animation ASSETS (montage/composite/sequence/blendspace)
// ─────────────────────────────────────────────────────────────────────────────

namespace AnimAssetHelpers
{
	// Resolve a slot track by name; nullptr if absent.
	static FSlotAnimationTrack* FindSlot(UAnimMontage* Montage, const FName& SlotName)
	{
		for (FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
		{
			if (Track.SlotName == SlotName) return &Track;
		}
		return nullptr;
	}

	// Find a montage section index by name; INDEX_NONE if absent.
	static int32 FindSectionIndex(UAnimMontage* Montage, const FName& Name)
	{
		for (int32 i = 0; i < Montage->CompositeSections.Num(); ++i)
		{
			if (Montage->CompositeSections[i].SectionName == Name) return i;
		}
		return INDEX_NONE;
	}

	// Resolve a UAnimNotify / UAnimNotifyState subclass by name or path.
	static UClass* ResolveNotifyClass(const FString& NameOrPath)
	{
		UClass* Found = InspectClassHelpers::ResolveClass(NameOrPath);
		if (!Found)
		{
			Found = LoadObject<UClass>(nullptr, *NameOrPath);
		}
		if (Found && (Found->IsChildOf(UAnimNotify::StaticClass()) || Found->IsChildOf(UAnimNotifyState::StaticClass())))
		{
			return Found;
		}
		return nullptr;
	}

	// Set one blend-space axis via reflection (BlendParameters[3] is protected). Returns false on bad index.
	static bool SetBlendAxis(UBlendSpace* BS, int32 Axis, const FString& DisplayName, float Min, float Max,
		int32 GridDivisions, bool bSnap, bool bWrap, const TSet<FString>& Provided)
	{
		if (Axis < 0 || Axis > 2) return false;
		// BlendParameters is protected and a fixed C-array (ArrayDim==3) of FBlendParameter.
		// Reach it via reflection and index by element size.
		FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters"));
		if (!Prop) return false;
		// Fixed-size array: ArrayDim == 3, element is a struct.
		FStructProperty* StructProp = CastField<FStructProperty>(Prop);
		if (!StructProp || Prop->ArrayDim < (Axis + 1)) return false;
		void* Base = Prop->ContainerPtrToValuePtr<void>(BS);
		FBlendParameter* Param = reinterpret_cast<FBlendParameter*>(static_cast<uint8*>(Base) + Axis * StructProp->GetElementSize());
		Param->DisplayName = DisplayName;
		Param->Min = Min;
		Param->Max = Max;
		if (Provided.Contains(TEXT("grid_divisions"))) Param->GridNum = FMath::Max(1, GridDivisions);
		if (Provided.Contains(TEXT("snap"))) Param->bSnapToGrid = bSnap;
		if (Provided.Contains(TEXT("wrap"))) Param->bWrapInput = bWrap;
		return true;
	}

	// Compact JSON summary of any animation asset (the 'get' action output).
	static TSharedPtr<FJsonObject> DescribeAsset(UObject* Asset)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("asset"), Asset->GetPathName());
		Out->SetStringField(TEXT("class"), Asset->GetClass()->GetName());

		if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
		{
			Out->SetStringField(TEXT("kind"), TEXT("montage"));
			Out->SetNumberField(TEXT("length"), Montage->GetPlayLength());
			if (USkeleton* Sk = Montage->GetSkeleton()) Out->SetStringField(TEXT("skeleton"), Sk->GetPathName());
			Out->SetStringField(TEXT("sync_group"), Montage->SyncGroup.ToString());
			Out->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);

			TArray<TSharedPtr<FJsonValue>> Slots;
			for (const FSlotAnimationTrack& Track : Montage->SlotAnimTracks)
			{
				TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
				S->SetStringField(TEXT("slot"), Track.SlotName.ToString());
				TArray<TSharedPtr<FJsonValue>> Segs;
				for (int32 i = 0; i < Track.AnimTrack.AnimSegments.Num(); ++i)
				{
					const FAnimSegment& Seg = Track.AnimTrack.AnimSegments[i];
					TSharedPtr<FJsonObject> SG = MakeShared<FJsonObject>();
					SG->SetNumberField(TEXT("index"), i);
					SG->SetStringField(TEXT("animation"), Seg.GetAnimReference() ? Seg.GetAnimReference()->GetPathName() : TEXT("(none)"));
					SG->SetNumberField(TEXT("start_pos"), Seg.StartPos);
					SG->SetNumberField(TEXT("anim_start"), Seg.AnimStartTime);
					SG->SetNumberField(TEXT("anim_end"), Seg.AnimEndTime);
					SG->SetNumberField(TEXT("play_rate"), Seg.AnimPlayRate);
					SG->SetNumberField(TEXT("loop_count"), Seg.LoopingCount);
					Segs.Add(MakeShared<FJsonValueObject>(SG));
				}
				S->SetArrayField(TEXT("segments"), Segs);
				Slots.Add(MakeShared<FJsonValueObject>(S));
			}
			Out->SetArrayField(TEXT("slots"), Slots);

			TArray<TSharedPtr<FJsonValue>> Sections;
			for (const FCompositeSection& Sec : Montage->CompositeSections)
			{
				TSharedPtr<FJsonObject> SecObj = MakeShared<FJsonObject>();
				SecObj->SetStringField(TEXT("name"), Sec.SectionName.ToString());
				SecObj->SetNumberField(TEXT("start_time"), Sec.GetTime());
				SecObj->SetStringField(TEXT("next_section"), Sec.NextSectionName.IsNone() ? TEXT("") : Sec.NextSectionName.ToString());
				Sections.Add(MakeShared<FJsonValueObject>(SecObj));
			}
			Out->SetArrayField(TEXT("sections"), Sections);
		}
		else if (UAnimComposite* Composite = Cast<UAnimComposite>(Asset))
		{
			Out->SetStringField(TEXT("kind"), TEXT("composite"));
			Out->SetNumberField(TEXT("length"), Composite->GetPlayLength());
			if (USkeleton* Sk = Composite->GetSkeleton()) Out->SetStringField(TEXT("skeleton"), Sk->GetPathName());
			TArray<TSharedPtr<FJsonValue>> Segs;
			for (int32 i = 0; i < Composite->AnimationTrack.AnimSegments.Num(); ++i)
			{
				const FAnimSegment& Seg = Composite->AnimationTrack.AnimSegments[i];
				TSharedPtr<FJsonObject> SG = MakeShared<FJsonObject>();
				SG->SetNumberField(TEXT("index"), i);
				SG->SetStringField(TEXT("animation"), Seg.GetAnimReference() ? Seg.GetAnimReference()->GetPathName() : TEXT("(none)"));
				SG->SetNumberField(TEXT("start_pos"), Seg.StartPos);
				Segs.Add(MakeShared<FJsonValueObject>(SG));
			}
			Out->SetArrayField(TEXT("segments"), Segs);
		}
		else if (UBlendSpace* BS = Cast<UBlendSpace>(Asset))
		{
			Out->SetStringField(TEXT("kind"), TEXT("blendspace"));
			if (USkeleton* Sk = BS->GetSkeleton()) Out->SetStringField(TEXT("skeleton"), Sk->GetPathName());
			const bool b1D = BS->IsA<UBlendSpace1D>();
			Out->SetNumberField(TEXT("dimensions"), b1D ? 1 : 2);
			TArray<TSharedPtr<FJsonValue>> Axes;
			for (int32 a = 0; a < (b1D ? 1 : 2); ++a)
			{
				const FBlendParameter& P = BS->GetBlendParameter(a);
				TSharedPtr<FJsonObject> Ax = MakeShared<FJsonObject>();
				Ax->SetStringField(TEXT("axis"), a == 0 ? TEXT("x") : TEXT("y"));
				Ax->SetStringField(TEXT("name"), P.DisplayName);
				Ax->SetNumberField(TEXT("min"), P.Min);
				Ax->SetNumberField(TEXT("max"), P.Max);
				Ax->SetNumberField(TEXT("grid_divisions"), P.GridNum);
				Axes.Add(MakeShared<FJsonValueObject>(Ax));
			}
			Out->SetArrayField(TEXT("axes"), Axes);
			TArray<TSharedPtr<FJsonValue>> Samples;
			for (int32 i = 0; i < BS->GetNumberOfBlendSamples(); ++i)
			{
				const FBlendSample& Sample = BS->GetBlendSample(i);
				TSharedPtr<FJsonObject> SObj = MakeShared<FJsonObject>();
				SObj->SetNumberField(TEXT("index"), i);
				SObj->SetStringField(TEXT("animation"), Sample.Animation ? Sample.Animation->GetPathName() : TEXT("(none)"));
				SObj->SetNumberField(TEXT("x"), Sample.SampleValue.X);
				if (!b1D) SObj->SetNumberField(TEXT("y"), Sample.SampleValue.Y);
				Samples.Add(MakeShared<FJsonValueObject>(SObj));
			}
			Out->SetArrayField(TEXT("samples"), Samples);
		}
		else if (UAnimSequence* Seq = Cast<UAnimSequence>(Asset))
		{
			Out->SetStringField(TEXT("kind"), TEXT("sequence"));
			Out->SetNumberField(TEXT("length"), Seq->GetPlayLength());
			if (USkeleton* Sk = Seq->GetSkeleton()) Out->SetStringField(TEXT("skeleton"), Sk->GetPathName());
			if (const IAnimationDataModel* Model = Seq->GetDataModel())
			{
				Out->SetNumberField(TEXT("frames"), Model->GetNumberOfFrames());
				Out->SetNumberField(TEXT("keys"), Model->GetNumberOfKeys());
				Out->SetNumberField(TEXT("bone_tracks"), Model->GetNumBoneTracks());
				Out->SetNumberField(TEXT("float_curves"), Model->GetNumberOfFloatCurves());
			}
		}

		// Notifies (sequence + montage share UAnimSequenceBase::Notifies).
		if (UAnimSequenceBase* Base = Cast<UAnimSequenceBase>(Asset))
		{
			TArray<TSharedPtr<FJsonValue>> Notifies;
			for (int32 i = 0; i < Base->Notifies.Num(); ++i)
			{
				const FAnimNotifyEvent& Ev = Base->Notifies[i];
				TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
				N->SetNumberField(TEXT("index"), i);
				N->SetNumberField(TEXT("time"), Ev.GetTriggerTime());
				N->SetNumberField(TEXT("duration"), Ev.GetDuration());
				N->SetNumberField(TEXT("track"), Ev.TrackIndex);
				if (Ev.Notify) N->SetStringField(TEXT("notify_class"), Ev.Notify->GetClass()->GetName());
				else if (Ev.NotifyStateClass) N->SetStringField(TEXT("notify_state_class"), Ev.NotifyStateClass->GetClass()->GetName());
				else N->SetStringField(TEXT("name"), Ev.NotifyName.ToString());
				Notifies.Add(MakeShared<FJsonValueObject>(N));
			}
			Out->SetArrayField(TEXT("notifies"), Notifies);
		}

		return Out;
	}
}

FToolExecutionResult FKismetAIModule::DoManageAnimation(const TSharedPtr<FJsonObject>& Args)
{
	using namespace AnimAssetHelpers;

	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("asset"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'asset' field is required"));
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *AssetPath);
	if (!Asset)
	{
		return FToolExecutionResult::Error(404, TEXT("ASSET_NOT_FOUND"), FString::Printf(TEXT("Animation asset not found: %s"), *AssetPath),
			{ TEXT("Pass the full path to a montage, composite, anim sequence, or blend space."),
			  TEXT("Create it first with create_asset.") });
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(Asset);
	UAnimComposite* Composite = Cast<UAnimComposite>(Asset);
	UBlendSpace* BlendSpace = Cast<UBlendSpace>(Asset);
	UAnimSequenceBase* SeqBase = Cast<UAnimSequenceBase>(Asset);
	USkeleton* Skeleton = nullptr;
	if (UAnimationAsset* AnimAsset = Cast<UAnimationAsset>(Asset)) Skeleton = AnimAsset->GetSkeleton();

	if (!Montage && !Composite && !BlendSpace && !SeqBase)
	{
		return FToolExecutionResult::Error(400, TEXT("UNSUPPORTED_ASSET"),
			FString::Printf(TEXT("manage_animation does not support '%s' (a %s)"), *AssetPath, *Asset->GetClass()->GetName()),
			{ TEXT("Supported: AnimMontage, AnimComposite, AnimSequence, BlendSpace (and 1D / aim offset variants).") });
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsArray = nullptr;
	if (!Body->TryGetArrayField(TEXT("operations"), OpsArray) || !OpsArray || OpsArray->Num() == 0)
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'operations' must be a non-empty array"));
	}

	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Asset);
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "ManageAnimation", "Kismet: Manage Animation"));
	Asset->Modify();

	TArray<TSharedPtr<FJsonValue>> ResultsArray;
	int32 SuccessCount = 0;
	int32 ErrorCount = 0;
	bool bMutated = false;
	bool bNotifiesChanged = false;

	auto Fail = [&](TSharedPtr<FJsonObject>& R, const TCHAR* Code, const FString& Msg)
	{
		R->SetBoolField(TEXT("success"), false);
		R->SetStringField(TEXT("error_code"), Code);
		R->SetStringField(TEXT("error"), Msg);
		ErrorCount++;
	};

	for (const TSharedPtr<FJsonValue>& OpVal : *OpsArray)
	{
		TSharedPtr<FJsonObject> Op = OpVal->AsObject();
		if (!Op) continue;

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		FString Action;
		Op->TryGetStringField(TEXT("action"), Action);
		R->SetStringField(TEXT("action"), Action);

		// ── get ────────────────────────────────────────────────────
		if (Action == TEXT("get"))
		{
			R->SetBoolField(TEXT("success"), true);
			R->SetObjectField(TEXT("structure"), DescribeAsset(Asset));
			SuccessCount++;
			ResultsArray.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}

		// ── Montage structural ops ──────────────────────────────────
		if (Action == TEXT("add_slot_track") || Action == TEXT("add_segment") || Action == TEXT("remove_segment")
			|| Action == TEXT("add_section") || Action == TEXT("set_section_next") || Action == TEXT("remove_section")
			|| Action == TEXT("set_sync_group"))
		{
			const bool bCompositeSegment = (Action == TEXT("add_segment") || Action == TEXT("remove_segment")) && Composite;
			if (!Montage && !bCompositeSegment)
			{
				Fail(R, TEXT("WRONG_ASSET_TYPE"), FString::Printf(TEXT("'%s' applies to a montage (this asset is a %s)"), *Action, *Asset->GetClass()->GetName()));
				ResultsArray.Add(MakeShared<FJsonValueObject>(R));
				continue;
			}

			if (Action == TEXT("add_slot_track"))
			{
				const FName SlotName(*Op->GetStringField(TEXT("slot_name")));
				if (FindSlot(Montage, SlotName))
				{
					Fail(R, TEXT("SLOT_EXISTS"), FString::Printf(TEXT("Slot '%s' already exists"), *SlotName.ToString()));
				}
				else
				{
					if (Skeleton) { Skeleton->Modify(); Skeleton->RegisterSlotNode(SlotName); }
					Montage->AddSlot(SlotName);
					bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetStringField(TEXT("slot"), SlotName.ToString());
					SuccessCount++;
				}
			}
			else if (Action == TEXT("add_segment"))
			{
				const FString AnimPath = Op->GetStringField(TEXT("animation"));
				UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *AnimPath);
				if (!Seq)
				{
					Fail(R, TEXT("ANIM_NOT_FOUND"), FString::Printf(TEXT("animation not found: '%s'"), *AnimPath));
				}
				else if (Skeleton && Seq->GetSkeleton() && !Skeleton->IsCompatibleForEditor(Seq->GetSkeleton()))
				{
					Fail(R, TEXT("SKELETON_MISMATCH"), FString::Printf(TEXT("'%s' uses an incompatible skeleton"), *AnimPath));
				}
				else
				{
					FAnimTrack* Track = nullptr;
					FName SlotName;
					if (Composite)
					{
						Track = &Composite->AnimationTrack;
					}
					else
					{
						SlotName = Op->HasField(TEXT("slot")) ? FName(*Op->GetStringField(TEXT("slot"))) : NAME_None;
						FSlotAnimationTrack* Slot = SlotName.IsNone()
							? (Montage->SlotAnimTracks.Num() > 0 ? &Montage->SlotAnimTracks[0] : nullptr)
							: FindSlot(Montage, SlotName);
						if (!Slot && SlotName.IsNone() && Montage->SlotAnimTracks.Num() == 0)
						{
							// Auto-create a DefaultSlot so add_segment works on a fresh montage.
							SlotName = FName(TEXT("DefaultSlot"));
							if (Skeleton) { Skeleton->Modify(); Skeleton->RegisterSlotNode(SlotName); }
							Slot = &Montage->AddSlot(SlotName);
						}
						if (Slot) { SlotName = Slot->SlotName; Track = &Slot->AnimTrack; }
					}
					if (!Track)
					{
						Fail(R, TEXT("SLOT_NOT_FOUND"), FString::Printf(TEXT("slot '%s' not found"), *SlotName.ToString()));
					}
					else
					{
						FAnimSegment Segment;
						Segment.SetAnimReference(Seq, true);
						double V;
						if (Op->TryGetNumberField(TEXT("start_time"), V)) Segment.AnimStartTime = (float)V;
						Segment.AnimEndTime = Op->TryGetNumberField(TEXT("end_time"), V) ? (float)V : Seq->GetPlayLength();
						if (Op->TryGetNumberField(TEXT("play_rate"), V)) Segment.AnimPlayRate = (float)V;
						int32 LoopCount;
						if (Op->TryGetNumberField(TEXT("loop_count"), LoopCount)) Segment.LoopingCount = FMath::Max(1, LoopCount);

						int32 InsertAt = Track->AnimSegments.Num();
						int32 Requested;
						if (Op->TryGetNumberField(TEXT("insert_at"), Requested)) InsertAt = FMath::Clamp(Requested, 0, Track->AnimSegments.Num());
						Track->AnimSegments.Insert(Segment, InsertAt);
						// Re-chain StartPos so the timeline stays contiguous.
						float Cursor = 0.f;
						for (FAnimSegment& S : Track->AnimSegments) { S.StartPos = Cursor; Cursor += S.GetLength(); }

						bMutated = true;
						R->SetBoolField(TEXT("success"), true);
						R->SetNumberField(TEXT("index"), InsertAt);
						if (!SlotName.IsNone()) R->SetStringField(TEXT("slot"), SlotName.ToString());
						R->SetNumberField(TEXT("start_pos"), Track->AnimSegments[InsertAt].StartPos);
						SuccessCount++;
					}
				}
			}
			else if (Action == TEXT("remove_segment"))
			{
				FAnimTrack* Track = Composite ? &Composite->AnimationTrack : nullptr;
				FName SlotName;
				if (!Composite)
				{
					SlotName = FName(*Op->GetStringField(TEXT("slot")));
					FSlotAnimationTrack* Slot = FindSlot(Montage, SlotName);
					if (Slot) Track = &Slot->AnimTrack;
				}
				int32 Index = -1;
				Op->TryGetNumberField(TEXT("index"), Index);
				if (!Track)
				{
					Fail(R, TEXT("SLOT_NOT_FOUND"), FString::Printf(TEXT("slot '%s' not found"), *SlotName.ToString()));
				}
				else if (!Track->AnimSegments.IsValidIndex(Index))
				{
					Fail(R, TEXT("INDEX_OUT_OF_RANGE"), FString::Printf(TEXT("segment index %d out of range (0..%d)"), Index, Track->AnimSegments.Num() - 1));
				}
				else
				{
					Track->AnimSegments.RemoveAt(Index);
					float Cursor = 0.f;
					for (FAnimSegment& S : Track->AnimSegments) { S.StartPos = Cursor; Cursor += S.GetLength(); }
					bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetNumberField(TEXT("removed_index"), Index);
					SuccessCount++;
				}
			}
			else if (Action == TEXT("add_section"))
			{
				const FName SecName(*Op->GetStringField(TEXT("name")));
				double StartTime = 0.0;
				Op->TryGetNumberField(TEXT("start_time"), StartTime);
				if (FindSectionIndex(Montage, SecName) != INDEX_NONE)
				{
					Fail(R, TEXT("SECTION_EXISTS"), FString::Printf(TEXT("A section named '%s' already exists"), *SecName.ToString()));
				}
				else
				{
					const int32 SecIdx = Montage->AddAnimCompositeSection(SecName, (float)StartTime);
					FString NextSection;
					if (Op->TryGetStringField(TEXT("next_section"), NextSection) && !NextSection.IsEmpty())
					{
						Montage->CompositeSections[SecIdx].NextSectionName = FName(*NextSection);
					}
					bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetStringField(TEXT("section"), SecName.ToString());
					R->SetNumberField(TEXT("index"), SecIdx);
					SuccessCount++;
				}
			}
			else if (Action == TEXT("set_section_next"))
			{
				const FName SecName(*Op->GetStringField(TEXT("section")));
				const int32 SecIdx = FindSectionIndex(Montage, SecName);
				if (SecIdx == INDEX_NONE)
				{
					Fail(R, TEXT("SECTION_NOT_FOUND"), FString::Printf(TEXT("Section not found: '%s'"), *SecName.ToString()));
				}
				else
				{
					FString NextSection;
					Op->TryGetStringField(TEXT("next_section"), NextSection); // empty/absent clears the loop/branch
					Montage->CompositeSections[SecIdx].NextSectionName = NextSection.IsEmpty() ? NAME_None : FName(*NextSection);
					bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetStringField(TEXT("section"), SecName.ToString());
					R->SetStringField(TEXT("next_section"), NextSection);
					SuccessCount++;
				}
			}
			else if (Action == TEXT("remove_section"))
			{
				const FName SecName(*Op->GetStringField(TEXT("name")));
				const int32 SecIdx = FindSectionIndex(Montage, SecName);
				if (SecIdx == INDEX_NONE)
				{
					Fail(R, TEXT("SECTION_NOT_FOUND"), FString::Printf(TEXT("Section not found: '%s'"), *SecName.ToString()));
				}
				else
				{
					Montage->CompositeSections.RemoveAt(SecIdx);
					bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetStringField(TEXT("removed_section"), SecName.ToString());
					SuccessCount++;
				}
			}
			else if (Action == TEXT("set_sync_group"))
			{
				Montage->SyncGroup = FName(*Op->GetStringField(TEXT("group")));
				int32 SlotIndex;
				if (Op->TryGetNumberField(TEXT("slot_index"), SlotIndex)) Montage->SyncSlotIndex = SlotIndex;
				bMutated = true;
				R->SetBoolField(TEXT("success"), true);
				R->SetStringField(TEXT("sync_group"), Montage->SyncGroup.ToString());
				R->SetNumberField(TEXT("sync_slot_index"), Montage->SyncSlotIndex);
				SuccessCount++;
			}

			ResultsArray.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}

		// ── Notify ops (sequence + montage) ─────────────────────────
		if (Action == TEXT("add_notify") || Action == TEXT("remove_notify"))
		{
			if (!SeqBase)
			{
				Fail(R, TEXT("WRONG_ASSET_TYPE"), TEXT("notify ops apply to anim sequences and montages"));
				ResultsArray.Add(MakeShared<FJsonValueObject>(R));
				continue;
			}

			if (Action == TEXT("add_notify"))
			{
				double Time = 0.0;
				Op->TryGetNumberField(TEXT("time"), Time);
				FName NotifyName(*Op->GetStringField(TEXT("name")));
				FString NotifyClassName;
				Op->TryGetStringField(TEXT("notify_class"), NotifyClassName);
				double Duration = 0.0;
				Op->TryGetNumberField(TEXT("duration"), Duration);
				int32 TrackIndex = 0;
				Op->TryGetNumberField(TEXT("track"), TrackIndex);

				TrackIndex = FMath::Max(0, TrackIndex);
				// Ensure a notify track exists at TrackIndex (RefreshCacheData errors on dangling indices).
				if (SeqBase->AnimNotifyTracks.Num() == 0)
				{
					SeqBase->InitializeNotifyTrack();
				}
				while (SeqBase->AnimNotifyTracks.Num() <= TrackIndex)
				{
					SeqBase->AnimNotifyTracks.Add(FAnimNotifyTrack(
						FName(*FString::Printf(TEXT("%d"), SeqBase->AnimNotifyTracks.Num() + 1)), FLinearColor::White));
				}

				FAnimNotifyEvent Ev;
				Ev.Link(SeqBase, (float)Time);
				Ev.SetTime((float)Time);
				Ev.TrackIndex = TrackIndex;

				bool bOk = true;
				if (!NotifyClassName.IsEmpty())
				{
					UClass* NotifyClass = ResolveNotifyClass(NotifyClassName);
					if (!NotifyClass)
					{
						Fail(R, TEXT("NOTIFY_CLASS_NOT_FOUND"), FString::Printf(TEXT("Could not resolve notify class '%s' (must derive from AnimNotify or AnimNotifyState)"), *NotifyClassName));
						bOk = false;
					}
					else if (NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()) || Duration > 0.0)
					{
						// Notify STATE — needs a duration and an instance under the sequence.
						UClass* StateClass = NotifyClass->IsChildOf(UAnimNotifyState::StaticClass()) ? NotifyClass : nullptr;
						if (!StateClass)
						{
							Fail(R, TEXT("NOTIFY_CLASS_MISMATCH"), FString::Printf(TEXT("duration>0 requires a UAnimNotifyState class; '%s' is a UAnimNotify"), *NotifyClassName));
							bOk = false;
						}
						else
						{
							Ev.NotifyStateClass = NewObject<UAnimNotifyState>(SeqBase, StateClass, NAME_None, RF_Transactional);
							Ev.NotifyName = FName(*StateClass->GetName().Replace(TEXT("AnimNotifyState_"), TEXT("")));
							Ev.SetDuration(Duration > 0.0 ? (float)Duration : 0.1f);
							Ev.EndLink.Link(SeqBase, Ev.GetTriggerTime() + Ev.GetDuration());
						}
					}
					else
					{
						Ev.Notify = NewObject<UAnimNotify>(SeqBase, NotifyClass, NAME_None, RF_Transactional);
						Ev.NotifyName = FName(*NotifyClass->GetName().Replace(TEXT("AnimNotify_"), TEXT("")));
					}
				}
				else
				{
					// Named (skeleton) notify — register on the skeleton so it is valid.
					if (Skeleton) { Skeleton->Modify(); Skeleton->AddNewAnimationNotify(NotifyName); }
					Ev.NotifyName = NotifyName;
				}

				if (bOk)
				{
					SeqBase->Notifies.Add(Ev);
					bNotifiesChanged = bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetStringField(TEXT("name"), Ev.NotifyName.ToString());
					R->SetNumberField(TEXT("time"), Ev.GetTriggerTime());
					if (Ev.NotifyStateClass) R->SetNumberField(TEXT("duration"), Ev.GetDuration());
					SuccessCount++;
				}
			}
			else // remove_notify
			{
				int32 Index = -1;
				const bool bHasIndex = Op->TryGetNumberField(TEXT("index"), Index);
				if (bHasIndex)
				{
					if (!SeqBase->Notifies.IsValidIndex(Index))
					{
						Fail(R, TEXT("INDEX_OUT_OF_RANGE"), FString::Printf(TEXT("notify index %d out of range"), Index));
					}
					else
					{
						SeqBase->Notifies.RemoveAt(Index);
						bNotifiesChanged = bMutated = true;
						R->SetBoolField(TEXT("success"), true);
						R->SetNumberField(TEXT("removed_index"), Index);
						SuccessCount++;
					}
				}
				else
				{
					const FName Name(*Op->GetStringField(TEXT("name")));
					double Time;
					const bool bHasTime = Op->TryGetNumberField(TEXT("time"), Time);
					int32 Removed = SeqBase->Notifies.RemoveAll([&](const FAnimNotifyEvent& Ev)
					{
						return Ev.NotifyName == Name && (!bHasTime || FMath::IsNearlyEqual(Ev.GetTriggerTime(), (float)Time, 0.01f));
					});
					if (Removed == 0)
					{
						Fail(R, TEXT("NOTIFY_NOT_FOUND"), FString::Printf(TEXT("No notify named '%s'%s"), *Name.ToString(), bHasTime ? TEXT(" at that time") : TEXT("")));
					}
					else
					{
						bNotifiesChanged = bMutated = true;
						R->SetBoolField(TEXT("success"), true);
						R->SetNumberField(TEXT("removed"), Removed);
						SuccessCount++;
					}
				}
			}

			ResultsArray.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}

		// ── Blend space ops ─────────────────────────────────────────
		if (Action == TEXT("set_axis") || Action == TEXT("add_sample") || Action == TEXT("edit_sample") || Action == TEXT("remove_sample"))
		{
			if (!BlendSpace)
			{
				Fail(R, TEXT("WRONG_ASSET_TYPE"), FString::Printf(TEXT("'%s' applies to a blend space (this asset is a %s)"), *Action, *Asset->GetClass()->GetName()));
				ResultsArray.Add(MakeShared<FJsonValueObject>(R));
				continue;
			}
			const bool b1D = BlendSpace->IsA<UBlendSpace1D>();

			if (Action == TEXT("set_axis"))
			{
				const FString AxisStr = Op->GetStringField(TEXT("axis"));
				const int32 AxisIdx = (AxisStr == TEXT("y")) ? 1 : 0;
				if (AxisIdx == 1 && b1D)
				{
					Fail(R, TEXT("INVALID_AXIS"), TEXT("1D blend spaces have only an x axis"));
				}
				else
				{
					double Min = 0, Max = 100; int32 Grid = 4; bool bSnap = false, bWrap = false;
					Op->TryGetNumberField(TEXT("min"), Min);
					Op->TryGetNumberField(TEXT("max"), Max);
					Op->TryGetNumberField(TEXT("grid_divisions"), Grid);
					Op->TryGetBoolField(TEXT("snap"), bSnap);
					Op->TryGetBoolField(TEXT("wrap"), bWrap);
					TSet<FString> Provided;
					if (Op->HasField(TEXT("grid_divisions"))) Provided.Add(TEXT("grid_divisions"));
					if (Op->HasField(TEXT("snap"))) Provided.Add(TEXT("snap"));
					if (Op->HasField(TEXT("wrap"))) Provided.Add(TEXT("wrap"));
					if (SetBlendAxis(BlendSpace, AxisIdx, Op->GetStringField(TEXT("name")), (float)Min, (float)Max, Grid, bSnap, bWrap, Provided))
					{
						BlendSpace->ValidateSampleData();
						bMutated = true;
						R->SetBoolField(TEXT("success"), true);
						R->SetStringField(TEXT("axis"), AxisStr);
						SuccessCount++;
					}
					else
					{
						Fail(R, TEXT("AXIS_SET_FAILED"), TEXT("Failed to set blend-space axis"));
					}
				}
			}
			else if (Action == TEXT("add_sample"))
			{
				const FString AnimPath = Op->GetStringField(TEXT("animation"));
				UAnimSequence* Seq = LoadObject<UAnimSequence>(nullptr, *AnimPath);
				double X = 0, Y = 0;
				Op->TryGetNumberField(TEXT("x"), X);
				Op->TryGetNumberField(TEXT("y"), Y);
				if (!Seq)
				{
					Fail(R, TEXT("ANIM_NOT_FOUND"), FString::Printf(TEXT("animation not found: '%s'"), *AnimPath));
				}
				else if (Skeleton && Seq->GetSkeleton() && !Skeleton->IsCompatibleForEditor(Seq->GetSkeleton()))
				{
					Fail(R, TEXT("SKELETON_MISMATCH"), FString::Printf(TEXT("'%s' uses an incompatible skeleton"), *AnimPath));
				}
				else
				{
					const int32 Idx = BlendSpace->AddSample(Seq, FVector((float)X, b1D ? 0.f : (float)Y, 0.f));
					if (Idx == INDEX_NONE)
					{
						Fail(R, TEXT("SAMPLE_REJECTED"), TEXT("Blend space rejected the sample (out of axis range, or duplicate position)"));
					}
					else
					{
						BlendSpace->ValidateSampleData();
						bMutated = true;
						R->SetBoolField(TEXT("success"), true);
						R->SetNumberField(TEXT("index"), Idx);
						SuccessCount++;
					}
				}
			}
			else if (Action == TEXT("edit_sample"))
			{
				int32 Index = -1;
				Op->TryGetNumberField(TEXT("index"), Index);
				double X = 0, Y = 0;
				Op->TryGetNumberField(TEXT("x"), X);
				Op->TryGetNumberField(TEXT("y"), Y);
				if (!BlendSpace->IsValidBlendSampleIndex(Index))
				{
					Fail(R, TEXT("INDEX_OUT_OF_RANGE"), FString::Printf(TEXT("sample index %d out of range"), Index));
				}
				else if (!BlendSpace->EditSampleValue(Index, FVector((float)X, b1D ? 0.f : (float)Y, 0.f)))
				{
					Fail(R, TEXT("EDIT_REJECTED"), TEXT("Blend space rejected the new sample position"));
				}
				else
				{
					BlendSpace->ValidateSampleData();
					bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetNumberField(TEXT("index"), Index);
					SuccessCount++;
				}
			}
			else // remove_sample
			{
				int32 Index = -1;
				Op->TryGetNumberField(TEXT("index"), Index);
				if (!BlendSpace->IsValidBlendSampleIndex(Index))
				{
					Fail(R, TEXT("INDEX_OUT_OF_RANGE"), FString::Printf(TEXT("sample index %d out of range"), Index));
				}
				else if (!BlendSpace->DeleteSample(Index))
				{
					Fail(R, TEXT("DELETE_FAILED"), TEXT("Failed to delete sample"));
				}
				else
				{
					BlendSpace->ValidateSampleData();
					bMutated = true;
					R->SetBoolField(TEXT("success"), true);
					R->SetNumberField(TEXT("removed_index"), Index);
					SuccessCount++;
				}
			}

			ResultsArray.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}

		// Unknown action.
		Fail(R, TEXT("UNKNOWN_ACTION"), FString::Printf(TEXT("Unknown action: '%s'"), *Action));
		ResultsArray.Add(MakeShared<FJsonValueObject>(R));
	}

	// Post-edit fixups.
	if (bNotifiesChanged && SeqBase)
	{
		SeqBase->SortNotifies();
		SeqBase->RefreshCacheData();
	}
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.PackageName = Asset->GetOutermost()->GetName();
	Outcome.bAppliedInMemory = bMutated;
	if (bMutated)
	{
		Asset->PostEditChange();
		Asset->MarkPackageDirty();
		AutoSaveAfterMutation(Asset, Outcome);
	}
	else
	{
		Outcome.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
		Outcome.SavePolicy = KismetAISavePolicy::AutosaveEvenIfCompileFailed;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetStringField(TEXT("asset"), Asset->GetPathName());
	Data->SetNumberField(TEXT("operations_succeeded"), SuccessCount);
	Data->SetNumberField(TEXT("operations_failed"), ErrorCount);
	Data->SetArrayField(TEXT("results"), ResultsArray);

	return FToolExecutionResult::Success(Data, ErrorCount == 0);
}

// HTTP adapter over DoManageAnimation. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleManageAnimation(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("manage_animation"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// search_nodes handler
// ─────────────────────────────────────────────────────────────────────────────

FToolExecutionResult FKismetAIModule::DoSearchNodes(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	FString Query = Body->GetStringField(TEXT("query"));
	FString CategoryFilter = Body->HasField(TEXT("category")) ? Body->GetStringField(TEXT("category")) : TEXT("");
	FString ClassFilter = Body->HasField(TEXT("class")) ? Body->GetStringField(TEXT("class")) : TEXT("");
	bool bIncludePins = Body->HasField(TEXT("include_pins")) ? Body->GetBoolField(TEXT("include_pins")) : false;
	int32 Limit = Body->HasField(TEXT("limit")) ? static_cast<int32>(Body->GetNumberField(TEXT("limit"))) : 20;
	Limit = FMath::Clamp(Limit, 1, 100);

	// Optional anim scoping: when blueprint+graph resolve to an animation graph, surface
	// the anim node spawners (UAnimGraphNode_* class spawners + per-asset "Play 'X'"
	// spawners) and skeleton-filter the asset ones. Otherwise anim spawners are excluded
	// (they carry no useful menu text for a K2 graph and would just be noise).
	bool bAnimGraphScope = false;
	UAnimBlueprint* AnimBPForSearch = nullptr;
	{
		FString SearchBPPath, SearchGraphName;
		Body->TryGetStringField(TEXT("blueprint"), SearchBPPath);
		Body->TryGetStringField(TEXT("graph"), SearchGraphName);
		if (!SearchBPPath.IsEmpty())
		{
			if (UBlueprint* SearchBP = KismetAIUtils::FindBlueprint(SearchBPPath))
			{
				if (UEdGraph* SearchGraph = KismetAIUtils::FindGraph(SearchBP, SearchGraphName))
				{
					if (AnimSpawnerHelpers::IsAnimNodeGraph(SearchGraph))
					{
						bAnimGraphScope = true;
						AnimBPForSearch = Cast<UAnimBlueprint>(SearchBP);
					}
				}
			}
		}
	}

	// --- Scoring struct for ranked results ---
	struct FNodeMatch
	{
		int32 Score;
		FString MenuName;
		FString NodeClassName;
		FString Category;
		FString Keywords;
		FString Tooltip;
		UBlueprintNodeSpawner* Spawner;
	};

	TArray<FNodeMatch> Matches;

	// --- Iterate the Blueprint action database ---
	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();
	const FBlueprintActionDatabase::FActionRegistry& AllActions = ActionDB.GetAllActions();

	for (const auto& Pair : AllActions)
	{
		for (UBlueprintNodeSpawner* Spawner : Pair.Value)
		{
			if (!Spawner) continue;

			// Anim/K2 scoping: in an anim graph keep only anim node spawners; everywhere
			// else exclude anim node spawners. Asset spawners ("Play 'X'") are additionally
			// skeleton-filtered below.
			const bool bIsAnimSpawner = AnimSpawnerHelpers::IsAnimNodeSpawner(Spawner);
			if (bAnimGraphScope)
			{
				// In an anim graph keep anim pose spawners AND pure K2 spawners (getters,
				// pure functions — legal pose-graph data sources); drop impure K2 spawners.
				if (!bIsAnimSpawner && !AnimSpawnerHelpers::IsPureK2Spawner(Spawner)) continue;
				bool bIsAnimAssetSpawner = false;
				const FAssetData AssetData = AnimSpawnerHelpers::GetAnimAssetDataFromSpawner(Spawner, bIsAnimAssetSpawner);
				if (bIsAnimAssetSpawner)
				{
					// Drop asset spawners whose asset is skeleton-incompatible.
					FString SkelErr;
					if (!AnimSpawnerHelpers::CheckSkeletonCompatible(AnimBPForSearch, AssetData, SkelErr))
					{
						continue;
					}
				}
			}
			else if (bIsAnimSpawner)
			{
				continue;
			}

			// Get UI spec (cached after first call)
			const FBlueprintActionUiSpec& UiSpec = Spawner->PrimeDefaultUiSpec();

			FString MenuName = UiSpec.MenuName.ToString();
			FString Category = UiSpec.Category.ToString();
			FString Keywords = UiSpec.Keywords.ToString();

			// Skip empty/unnamed nodes
			if (MenuName.IsEmpty()) continue;

			// Category filter
			if (!CategoryFilter.IsEmpty())
			{
				if (!Category.Contains(CategoryFilter, ESearchCase::IgnoreCase))
				{
					continue;
				}
			}

			// Class filter — check if this spawner is associated with the target class
			if (!ClassFilter.IsEmpty())
			{
				bool bClassMatch = false;

				// Check function spawners
				if (UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
				{
					if (const UFunction* Func = FuncSpawner->GetFunction())
					{
						UClass* OwnerClass = Func->GetOwnerClass();
						if (OwnerClass)
						{
							FString ClassName = OwnerClass->GetName();
							if (ClassName.Contains(ClassFilter, ESearchCase::IgnoreCase))
							{
								bClassMatch = true;
							}
						}
					}
				}

				if (!bClassMatch)
				{
					continue;
				}
			}

			// Score the match against the query
			int32 Score = 0;
			FString QueryLower = Query.ToLower();
			FString MenuNameLower = MenuName.ToLower();
			FString KeywordsLower = Keywords.ToLower();

			// Exact name match (highest)
			if (MenuNameLower == QueryLower)
			{
				Score = 100;
			}
			// Name starts with query
			else if (MenuNameLower.StartsWith(QueryLower))
			{
				Score = 50;
			}
			// Name contains query
			else if (MenuNameLower.Contains(QueryLower))
			{
				Score = 30;
			}
			// Keywords match
			else if (KeywordsLower.Contains(QueryLower))
			{
				Score = 20;
			}
			else
			{
				// No match
				continue;
			}

			// Build tooltip (lazy — only if we have a match)
			FString Tooltip;
			if (UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
			{
				if (const UFunction* Func = FuncSpawner->GetFunction())
				{
					Tooltip = Func->GetMetaData(TEXT("ToolTip"));
					if (Tooltip.Len() > 200)
					{
						Tooltip = Tooltip.Left(200) + TEXT("...");
					}
				}
			}

			Matches.Add({Score, MenuName, Spawner->NodeClass ? Spawner->NodeClass->GetName() : TEXT(""), Category, Keywords, Tooltip, Spawner});
		}
	}

	// --- Sort by score descending, then by name ---
	Matches.Sort([](const FNodeMatch& A, const FNodeMatch& B)
	{
		if (A.Score != B.Score) return A.Score > B.Score;
		return A.MenuName < B.MenuName;
	});

	int32 TotalMatches = Matches.Num();
	if (Matches.Num() > Limit)
	{
		Matches.SetNum(Limit);
	}

	// --- Build response ---
	TArray<TSharedPtr<FJsonValue>> ResultsArr;
	ResultsArr.Reserve(Matches.Num());

	for (const FNodeMatch& Match : Matches)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		NodeObj->SetStringField(TEXT("name"), Match.MenuName);

		if (!Match.NodeClassName.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("node_class"), Match.NodeClassName);
		}

		if (!Match.Category.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("category"), Match.Category);
		}

		if (!Match.Keywords.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("keywords"), Match.Keywords);
		}

		// Function reference (T3D-compatible format)
		UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Match.Spawner);
		if (FuncSpawner)
		{
			if (const UFunction* Func = FuncSpawner->GetFunction())
			{
				TSharedPtr<FJsonObject> FuncRef = MakeShared<FJsonObject>();
				UClass* OwnerClass = Func->GetOwnerClass();
				if (OwnerClass)
				{
					FuncRef->SetStringField(TEXT("member_parent"), OwnerClass->GetPathName());
				}
				FuncRef->SetStringField(TEXT("member_name"), Func->GetName());
				NodeObj->SetObjectField(TEXT("function_reference"), FuncRef);

				// Pin signatures
				if (bIncludePins)
				{
					TArray<TSharedPtr<FJsonValue>> PinsArr;
					bool bIsPure = Func->HasAnyFunctionFlags(FUNC_BlueprintPure);

					// Exec pins for impure functions
					if (!bIsPure)
					{
						TSharedPtr<FJsonObject> ExecIn = MakeShared<FJsonObject>();
						ExecIn->SetStringField(TEXT("name"), TEXT("execute"));
						ExecIn->SetStringField(TEXT("direction"), TEXT("input"));
						ExecIn->SetStringField(TEXT("category"), TEXT("exec"));
						PinsArr.Add(MakeShared<FJsonValueObject>(ExecIn));

						TSharedPtr<FJsonObject> ExecOut = MakeShared<FJsonObject>();
						ExecOut->SetStringField(TEXT("name"), TEXT("then"));
						ExecOut->SetStringField(TEXT("direction"), TEXT("output"));
						ExecOut->SetStringField(TEXT("category"), TEXT("exec"));
						PinsArr.Add(MakeShared<FJsonValueObject>(ExecOut));
					}

					// Function parameters
					for (TFieldIterator<FProperty> ParamIt(Func); ParamIt; ++ParamIt)
					{
						FProperty* Param = *ParamIt;
						TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();

						if (Param->HasAnyPropertyFlags(CPF_ReturnParm))
						{
							PinObj->SetStringField(TEXT("name"), TEXT("ReturnValue"));
							PinObj->SetStringField(TEXT("direction"), TEXT("output"));
						}
						else if (Param->HasAnyPropertyFlags(CPF_OutParm))
						{
							PinObj->SetStringField(TEXT("name"), Param->GetName());
							PinObj->SetStringField(TEXT("direction"), TEXT("output"));
						}
						else
						{
							PinObj->SetStringField(TEXT("name"), Param->GetName());
							PinObj->SetStringField(TEXT("direction"), TEXT("input"));
						}

						PinObj->SetStringField(TEXT("type"), InspectClassHelpers::PropertyToPinType(Param));

						// Default value from metadata
						FString DefaultVal = Func->GetMetaData(*FString::Printf(TEXT("CPP_Default_%s"), *Param->GetName()));
						if (!DefaultVal.IsEmpty())
						{
							PinObj->SetStringField(TEXT("default"), DefaultVal);
						}

						PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
					}

					NodeObj->SetArrayField(TEXT("pins"), PinsArr);
				}
			}
		}

		if (!Match.Tooltip.IsEmpty())
		{
			NodeObj->SetStringField(TEXT("tooltip"), Match.Tooltip);
		}

		ResultsArr.Add(MakeShared<FJsonValueObject>(NodeObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("count"), ResultsArr.Num());
	Data->SetNumberField(TEXT("total_matches"), TotalMatches);
	Data->SetArrayField(TEXT("results"), ResultsArr);

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoSearchNodes. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleSearchNodes(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("search_nodes"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// insert_node handler — spawn a node by its menu name (macros, casts, switches,
// function calls, …) via the Blueprint action database, instead of hand-authoring
// T3D. Returns the created node's real pins so the agent can wire it with
// edit_blueprint. Optionally wires an exec input from an existing node.
// ─────────────────────────────────────────────────────────────────────────────
// Resolve a component-getter request ("Get CameraSpringArm" or "CameraSpringArm")
// to the component's variable name on this Blueprint's class. Covers components
// inherited from a C++ parent or a parent Blueprint — which the action database
// does NOT offer as a "Get X" menu action when the underlying UPROPERTY isn't
// BlueprintReadWrite/ReadOnly (e.g. EditDefaultsOnly), even though the editor's
// Components-panel drag-drop still creates a getter for them. Returns "" if the
// query doesn't name a component. This is the headless equivalent of that
// drag-drop, so the agent stops resorting to GetComponentByClass + Cast.
static FString ResolveComponentGetterVariable(UBlueprint* Blueprint, const FString& NodeQuery)
{
	if (!Blueprint) return FString();
	UClass* Scope = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
	if (!Scope) return FString();

	FString Q = NodeQuery;
	Q.TrimStartAndEndInline();
	if (Q.StartsWith(TEXT("get "), ESearchCase::IgnoreCase))
	{
		Q = Q.RightChop(4);
	}
	const FString QNorm = Q.ToLower().Replace(TEXT(" "), TEXT(""));
	if (QNorm.IsEmpty()) return FString();

	for (TFieldIterator<FObjectProperty> It(Scope); It; ++It)
	{
		FObjectProperty* Prop = *It;
		if (!Prop || !Prop->PropertyClass) continue;
		if (!Prop->PropertyClass->IsChildOf(UActorComponent::StaticClass())) continue;
		if (Prop->GetName().ToLower().Replace(TEXT(" "), TEXT("")) == QNorm)
		{
			return Prop->GetName();
		}
	}
	return FString();
}

// Resolve a "Get <var>" (or bare "<var>") request to a plain member-variable name on this
// Blueprint's class, so insert_node can spawn a getter even when the action database did not
// surface the variable as a menu action. A getter is a PURE node, so this fallback applies in
// anim graphs too (the real-world case: a bool member variable driving a Blend's bool pin).
// Skips component properties (those go through ResolveComponentGetterVariable). Returns "" if
// the query does not name a member variable.
static FString ResolveMemberVariableGetter(UBlueprint* Blueprint, const FString& NodeQuery)
{
	if (!Blueprint) return FString();
	UClass* Scope = Blueprint->SkeletonGeneratedClass ? Blueprint->SkeletonGeneratedClass : Blueprint->GeneratedClass;
	if (!Scope) return FString();

	FString Q = NodeQuery;
	Q.TrimStartAndEndInline();
	if (Q.StartsWith(TEXT("get "), ESearchCase::IgnoreCase))
	{
		Q = Q.RightChop(4);
	}
	const FString QNorm = Q.ToLower().Replace(TEXT(" "), TEXT(""));
	if (QNorm.IsEmpty()) return FString();

	for (TFieldIterator<FProperty> It(Scope); It; ++It)
	{
		FProperty* Prop = *It;
		if (!Prop) continue;
		// Components are handled by ResolveComponentGetterVariable; skip them here.
		if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
		{
			if (ObjProp->PropertyClass && ObjProp->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
			{
				continue;
			}
		}
		if (Prop->GetName().ToLower().Replace(TEXT(" "), TEXT("")) == QNorm)
		{
			return Prop->GetName();
		}
	}
	return FString();
}

// Resolve an insert_node query to a type-promotable operator FName (Add/Subtract/
// Multiply/Divide/Greater/GreaterEqual/Less/LessEqual/EqualEqual/NotEqual), or NAME_None.
// Operators have many same-named CallFunction overloads in the action DB (FrameNumber,
// FrameRate, Timespan, ...) and the menu matcher grabs an exotic one first; routing these
// queries to UE's promotable-operator spawner (FTypePromotion) yields the canonical
// wildcard node with the clean operator title the user means. Precise on purpose: a
// function name that merely CONTAINS an operator word (e.g. "AddActorWorldOffset") must
// NOT resolve — word aliases require an exact match or a non-letter boundary.
static FName ResolvePromotableOpName(const FString& Query)
{
	FString Q = Query.ToLower();
	Q.ReplaceInline(TEXT(" "), TEXT(""));
	if (Q.IsEmpty()) return NAME_None;

	struct FOpAlias { const TCHAR* Word; const TCHAR* Op; };
	static const FOpAlias Aliases[] = {
		{TEXT("greaterequal"), TEXT("GreaterEqual")}, {TEXT("greaterthanorequal"), TEXT("GreaterEqual")},
		{TEXT("lessequal"), TEXT("LessEqual")},       {TEXT("lessthanorequal"), TEXT("LessEqual")},
		{TEXT("notequal"), TEXT("NotEqual")},
		{TEXT("equalequal"), TEXT("EqualEqual")}, {TEXT("equals"), TEXT("EqualEqual")}, {TEXT("equal"), TEXT("EqualEqual")},
		{TEXT("greaterthan"), TEXT("Greater")}, {TEXT("greater"), TEXT("Greater")},
		{TEXT("lessthan"), TEXT("Less")},       {TEXT("less"), TEXT("Less")},
		{TEXT("divide"), TEXT("Divide")},       {TEXT("divided"), TEXT("Divide")},
		{TEXT("multiply"), TEXT("Multiply")},   {TEXT("multiplied"), TEXT("Multiply")}, {TEXT("times"), TEXT("Multiply")},
		{TEXT("subtract"), TEXT("Subtract")},   {TEXT("minus"), TEXT("Subtract")},
		{TEXT("add"), TEXT("Add")},             {TEXT("plus"), TEXT("Add")},
	};
	auto IsLetter = [](TCHAR C) { return C >= TEXT('a') && C <= TEXT('z'); };
	for (const FOpAlias& A : Aliases)
	{
		const FString W = A.Word;
		// Exact, or word followed by a non-letter boundary ("greater(>)", "divide(a,b)").
		if (Q == W || (Q.StartsWith(W) && (Q.Len() == W.Len() || !IsLetter(Q[W.Len()]))))
		{
			return FName(A.Op);
		}
	}

	// Symbol-shaped queries ("float > float", "a>=b", ">"). Strip operand type words, then
	// match a bare symbol. Check the two-char symbols before the one-char ones.
	FString S = Q;
	for (const TCHAR* T : { TEXT("float"), TEXT("double"), TEXT("int32"), TEXT("int64"), TEXT("integer"),
		TEXT("int"), TEXT("real"), TEXT("number"), TEXT("byte"), TEXT("vector"), TEXT("("), TEXT(")"), TEXT(","), TEXT("vs") })
	{
		S.ReplaceInline(T, TEXT(""));
	}
	if (S == TEXT(">=")) return FName(TEXT("GreaterEqual"));
	if (S == TEXT("<=")) return FName(TEXT("LessEqual"));
	if (S == TEXT("!=") || S == TEXT("=/=")) return FName(TEXT("NotEqual"));
	if (S == TEXT("==")) return FName(TEXT("EqualEqual"));
	if (S == TEXT(">")) return FName(TEXT("Greater"));
	if (S == TEXT("<")) return FName(TEXT("Less"));
	if (S == TEXT("/")) return FName(TEXT("Divide"));
	if (S == TEXT("*")) return FName(TEXT("Multiply"));
	if (S == TEXT("+")) return FName(TEXT("Add"));
	return NAME_None;
}

FToolExecutionResult FKismetAIModule::DoInsertNode(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("blueprint"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'blueprint' field is required"));
	}
	FString NodeQuery;
	if (!Body->TryGetStringField(TEXT("node"), NodeQuery) || NodeQuery.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'node' field is required (the node's menu name, e.g. \"ForEachLoop\", \"Branch\", \"Print String\")"));
	}
	FString GraphName;
	Body->TryGetStringField(TEXT("graph"), GraphName);
	const FString ClassFilter = Body->HasField(TEXT("class")) ? Body->GetStringField(TEXT("class")) : TEXT("");
	// Optional node-CLASS filter, e.g. "K2Node_InputAxisEvent" (distinct from 'class',
	// which filters a function's owning class). Menu names are not unique: an input axis
	// exposes BOTH a value getter (K2Node_GetInputAxisValue) and an event
	// (K2Node_InputAxisEvent) under the same name, and whichever the action database
	// yields first wins the exact-match tie below — leaving the other unreachable by any
	// query string. This narrows the search to the node type actually wanted. The
	// "K2Node_" prefix is optional, so "InputAxisEvent" works too.
	const FString NodeClassFilter = Body->HasField(TEXT("node_class")) ? Body->GetStringField(TEXT("node_class")) : TEXT("");
	auto NodeClassAllows = [&NodeClassFilter](const TCHAR* CandidateClass) -> bool
	{
		if (NodeClassFilter.IsEmpty()) return true;
		const FString Want(CandidateClass);
		return Want.Equals(NodeClassFilter, ESearchCase::IgnoreCase)
			|| Want.Equals(FString(TEXT("K2Node_")) + NodeClassFilter, ESearchCase::IgnoreCase);
	};
	// Opt-in proactive reflow: after placing the new node, reformat the connected
	// component it joined. Off by default (normal placement preserves authored
	// positions); the agent sets it to tidy the graph as part of the edit.
	bool bAutoLayout = false;
	Body->TryGetBoolField(TEXT("auto_layout"), bAutoLayout);

	UBlueprint* Blueprint = KismetAIUtils::FindBlueprint(AssetPath);
	if (!Blueprint)
	{
		return FToolExecutionResult::Error(404, TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("Blueprint not found: %s"), *AssetPath),
			{TEXT("Use search_assets to find the correct asset path")});
	}
	UEdGraph* Graph = KismetAIUtils::FindGraph(Blueprint, GraphName);
	if (!Graph)
	{
		return FToolExecutionResult::Error(404, TEXT("GRAPH_NOT_FOUND"), FString::Printf(TEXT("Graph not found: %s"), *GraphName));
	}

	// Anim-graph scoping: in a UAnimationGraphSchema graph we match anim node spawners
	// (UAnimGraphNode_* class spawners + per-asset "Play 'X'" spawners) AND pure data K2
	// spawners (variable getters + BlueprintPure function calls — legal pose-graph data
	// sources); we reject IMPURE K2 spawners (those produce exec nodes, illegal here). In a
	// K2 graph we reject anim spawners. Skeleton-incompatible asset spawners are tracked so
	// we can return a precise error if the agent named one. bImpureNameMatch records whether
	// the query DID match an impure K2 spawner, so a not-found maps to the right message.
	const bool bAnimGraph = AnimSpawnerHelpers::IsAnimNodeGraph(Graph);
	UAnimBlueprint* AnimBP = bAnimGraph ? Cast<UAnimBlueprint>(Blueprint) : nullptr;
	FString SkeletonRejectError; // set if a name-matched asset spawner was skeleton-incompatible
	bool bImpureNameMatch = false; // an impure K2 spawner name-matched the query in an anim graph

	// --- Find the best-matching spawner by menu name ---
	// Space-insensitive so "ForEachLoop" and "For Each Loop" both match.
	const FString QueryLower = NodeQuery.ToLower();
	const FString QueryDespaced = QueryLower.Replace(TEXT(" "), TEXT(""));
	UBlueprintNodeSpawner* BestSpawner = nullptr;
	FString BestMenuName;
	int32 BestScore = 0;

	// Operator routing (math/comparison): send these straight to UE's type-promotable
	// operator spawner instead of the action-DB menu match, which otherwise grabs an
	// exotic same-named overload (FrameNumber/FrameRate/Timespan). K2-only; skip in anim.
	if (!bAnimGraph)
	{
		const FName OpName = ResolvePromotableOpName(NodeQuery);
		if (OpName != NAME_None)
		{
			if (UBlueprintFunctionNodeSpawner* OpSpawner = FTypePromotion::GetOperatorSpawner(OpName))
			{
				const UClass* SpawnClass = OpSpawner->NodeClass.Get();
				if (NodeClassFilter.IsEmpty() || (SpawnClass && NodeClassAllows(*SpawnClass->GetName())))
				{
					BestSpawner = OpSpawner;
					BestMenuName = FTypePromotion::GetUserFacingOperatorName(OpName).ToString();
					BestScore = 100;
				}
			}
		}
	}

	FBlueprintActionDatabase& ActionDB = FBlueprintActionDatabase::Get();
	const FBlueprintActionDatabase::FActionRegistry& AllActions = ActionDB.GetAllActions();
	if (!BestSpawner)
	for (const auto& Pair : AllActions)
	{
		for (UBlueprintNodeSpawner* Spawner : Pair.Value)
		{
			if (!Spawner) continue;

			// Node-class filter first: a cheap class-name check that also skips the
			// comparatively expensive PrimeDefaultUiSpec() below for non-candidates.
			if (!NodeClassFilter.IsEmpty())
			{
				const UClass* SpawnClass = Spawner->NodeClass.Get();
				if (!SpawnClass || !NodeClassAllows(*SpawnClass->GetName())) continue;
			}

			// Anim/K2 gating before any scoring.
			const bool bIsAnimSpawner = AnimSpawnerHelpers::IsAnimNodeSpawner(Spawner);
			if (bAnimGraph)
			{
				// Anim pose spawners are always fine. Otherwise only PURE K2 spawners are
				// legal (getters, pure functions); impure K2 spawners produce exec nodes
				// which the anim schema rejects.
				if (!bIsAnimSpawner && !AnimSpawnerHelpers::IsPureK2Spawner(Spawner))
				{
					// Record whether the rejected impure spawner name-matched the query, so a
					// later "not found" can explain the exec rule rather than a generic miss.
					const FBlueprintActionUiSpec& RejSpec = Spawner->PrimeDefaultUiSpec();
					const FString RejMenu = RejSpec.MenuName.ToString().ToLower();
					if (!RejMenu.IsEmpty() && (RejMenu == QueryLower || RejMenu.Replace(TEXT(" "), TEXT("")) == QueryDespaced))
					{
						bImpureNameMatch = true;
					}
					continue;
				}
			}
			else if (bIsAnimSpawner)
			{
				continue; // anim node — not legal in a K2 graph
			}

			const FBlueprintActionUiSpec& UiSpec = Spawner->PrimeDefaultUiSpec();
			const FString MenuName = UiSpec.MenuName.ToString();
			if (MenuName.IsEmpty()) continue;

			// Skeleton compatibility for anim asset spawners: if a name-matched asset
			// spawner is incompatible, remember it (so the error names the skeletons)
			// but keep searching for a compatible alternative.
			if (bAnimGraph)
			{
				bool bIsAnimAssetSpawner = false;
				const FAssetData AssetData = AnimSpawnerHelpers::GetAnimAssetDataFromSpawner(Spawner, bIsAnimAssetSpawner);
				if (bIsAnimAssetSpawner)
				{
					const FString MN = MenuName.ToLower();
					const bool bNameMatches = (MN == QueryLower) || (MN.Replace(TEXT(" "), TEXT("")) == QueryDespaced)
						|| MN.StartsWith(QueryLower) || MN.Contains(QueryLower);
					FString SkelErr;
					if (!AnimSpawnerHelpers::CheckSkeletonCompatible(AnimBP, AssetData, SkelErr))
					{
						if (bNameMatches && SkeletonRejectError.IsEmpty())
						{
							SkeletonRejectError = SkelErr;
						}
						continue;
					}
				}
			}

			// Optional class filter for function spawners.
			if (!ClassFilter.IsEmpty())
			{
				bool bClassMatch = false;
				if (UBlueprintFunctionNodeSpawner* FuncSpawner = Cast<UBlueprintFunctionNodeSpawner>(Spawner))
				{
					if (const UFunction* Func = FuncSpawner->GetFunction())
					{
						if (UClass* OwnerClass = Func->GetOwnerClass())
						{
							bClassMatch = OwnerClass->GetName().Contains(ClassFilter, ESearchCase::IgnoreCase);
						}
					}
				}
				if (!bClassMatch) continue;
			}

			const FString MenuLower = MenuName.ToLower();
			const FString MenuDespaced = MenuLower.Replace(TEXT(" "), TEXT(""));
			const FString KeywordsLower = UiSpec.Keywords.ToString().ToLower();

			// Precision over recall: an exact / prefix / whole-word match only. Plain
			// substring matching is deliberately excluded — it spawns the wrong node
			// (e.g. "Gate" matching the "Get Propa-GATE Curves" variable getter).
			int32 Score = 0;
			if (MenuLower == QueryLower || MenuDespaced == QueryDespaced)
			{
				Score = 100;
			}
			else if (MenuLower.StartsWith(QueryLower) || MenuDespaced.StartsWith(QueryDespaced))
			{
				Score = 60;
			}
			else
			{
				// Whole-word match against the spaced menu name.
				TArray<FString> Words;
				MenuLower.ParseIntoArray(Words, TEXT(" "), true);
				if (Words.Contains(QueryLower))
				{
					Score = 40;
				}
				else
				{
					// Whole-word match against keywords (pipe/space separated).
					TArray<FString> KwWords;
					KeywordsLower.ParseIntoArray(KwWords, TEXT(" "), true);
					if (KwWords.Contains(QueryLower))
					{
						Score = 25;
					}
				}
			}
			if (Score == 0) continue;

			if (Score > BestScore)
			{
				BestScore = Score;
				BestSpawner = Spawner;
				BestMenuName = MenuName;
				if (Score == 100) break; // exact — good enough
			}
		}
		if (BestScore == 100) break;
	}

	// --- Also search the standard macro libraries by graph name ---
	// The high-value flow-control macros (ForEachLoop array variant, DoOnce, Gate,
	// FlipFlop) are NOT in the global action database, so match them directly.
	UEdGraph* BestMacroGraph = nullptr;
	FString BestMacroName;
	int32 BestMacroScore = 0;
	// Fallbacks must respect the requested node class as well as operator routing.
	if (!bAnimGraph && !BestSpawner && NodeClassAllows(TEXT("K2Node_MacroInstance")))
	{
		static const TCHAR* MacroLibPaths[] = {
			TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
			TEXT("/Engine/EditorBlueprintResources/ActorMacros.ActorMacros"),
			TEXT("/Engine/EditorBlueprintResources/ActorComponentMacros.ActorComponentMacros"),
		};
		for (const TCHAR* LibPath : MacroLibPaths)
		{
			UBlueprint* Lib = LoadObject<UBlueprint>(nullptr, LibPath);
			if (!Lib) continue;
			for (UEdGraph* MacroGraph : Lib->MacroGraphs)
			{
				if (!MacroGraph) continue;
				const FString NameLower = MacroGraph->GetName().ToLower();
				const FString NameDespaced = NameLower.Replace(TEXT(" "), TEXT(""));
				int32 Score = 0;
				if (NameLower == QueryLower || NameDespaced == QueryDespaced) Score = 100;
				else if (NameLower.StartsWith(QueryLower) || NameDespaced.StartsWith(QueryDespaced)) Score = 60;
				if (Score > BestMacroScore)
				{
					BestMacroScore = Score;
					BestMacroGraph = MacroGraph;
					BestMacroName = MacroGraph->GetName();
				}
			}
		}
	}

	const bool bUseMacro = BestMacroGraph && BestMacroScore >= BestScore;

	// Fallback: a component reference the editor would let you drag in from the
	// Components panel, but which has no "Get X" action in the menu database
	// (inherited and not BlueprintReadWrite/ReadOnly). Spawn the getter directly.
	// Allowed in anim graphs too — a variable getter is a pure data node.
	FString ComponentGetterVar;
	if (!BestSpawner && !bUseMacro && NodeClassAllows(TEXT("K2Node_VariableGet")))
	{
		ComponentGetterVar = ResolveComponentGetterVariable(Blueprint, NodeQuery);
	}

	// Fallback: a "Get <var>" for a plain member variable that the action DB did not
	// surface as a menu action. This is the headless equivalent of dragging a variable
	// out of the My Blueprint panel. A getter is pure, so it is legal in an anim graph
	// (the real-world case: a bool member driving a Blend's Active Value).
	FString MemberGetterVar;
	if (!BestSpawner && !bUseMacro && ComponentGetterVar.IsEmpty() && NodeClassAllows(TEXT("K2Node_VariableGet")))
	{
		MemberGetterVar = ResolveMemberVariableGetter(Blueprint, NodeQuery);
	}

	if (!BestSpawner && !bUseMacro && ComponentGetterVar.IsEmpty() && MemberGetterVar.IsEmpty())
	{
		// A name-matched asset spawner that was rejected purely on skeleton grounds gets
		// a precise error naming both skeletons instead of a generic "not found".
		if (!SkeletonRejectError.IsEmpty())
		{
			return FToolExecutionResult::Error(400, TEXT("SKELETON_MISMATCH"), SkeletonRejectError,
				{TEXT("Use search_assets to find an animation asset built for the compatible skeleton."),
				 TEXT("Check the Animation Blueprint's target skeleton in read_blueprint(mode=\"overview\").")});
		}
		// An impure (exec) K2 node DID match the query, but exec nodes are illegal in an
		// anim graph — explain the actual rule rather than a generic miss.
		if (bAnimGraph && bImpureNameMatch)
		{
			return FToolExecutionResult::Error(400, TEXT("SCHEMA_MISMATCH"),
				FString::Printf(TEXT("'%s' is an impure (exec) node, which is not legal in an animation graph — pure getters/functions are."), *NodeQuery),
				{TEXT("Anim graphs are pose-flow data graphs: they accept pure data nodes (variable getters, pure functions, struct breaks) and anim pose nodes, but not exec nodes."),
				 TEXT("If you need a value here, use a pure node (e.g. a variable Get, or a pure KismetMathLibrary function like \"NOT Boolean\").")});
		}
		FToolExecutionResult Result = FToolExecutionResult::Error(404, TEXT("NODE_NOT_FOUND"),
			FString::Printf(TEXT("No spawnable node matches '%s'%s%s%s."), *NodeQuery,
				ClassFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (class filter '%s')"), *ClassFilter),
				NodeClassFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (node_class filter '%s')"), *NodeClassFilter),
				bAnimGraph ? TEXT(" in this animation graph") : TEXT("")),
			bAnimGraph
				? TArray<FString>{TEXT("This is an animation graph. Use anim node menu names like \"Blend\", \"Two Way Blend\", \"Layered blend per bone\", \"State Machine\", \"Slot 'DefaultSlot'\", or \"Play 'AssetName'\" for an asset player."),
				                  TEXT("Pure data nodes are also legal here (a variable Get, a pure function like \"NOT Boolean\"); impure (exec) nodes are not."),
				                  TEXT("Run search_nodes with this blueprint+graph to list the nodes available here.")}
				: TArray<FString>{TEXT("Try search_nodes to find the exact menu name first."),
				                  TEXT("For a class member, pass the class filter, e.g. {\"node\":\"GetActorLocation\",\"class\":\"Actor\"}."),
				                  TEXT("If a name is ambiguous, pass node_class to pick the node type, e.g. {\"node\":\"MyAxis\",\"node_class\":\"K2Node_InputAxisEvent\"} — the response's node_class field shows which type you got.")});
		// Lookup ended before the transaction or any graph mutation. Preserve the
		// error while giving the agent journal definite no-effect evidence.
		FKismetAIMutationOutcome Outcome;
		Outcome.PackageName = Blueprint->GetOutermost()->GetName();
		Outcome.bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);
		Outcome.SavePolicy = KismetAISavePolicy::NoAutosave;
		Outcome.SaveSkipReason = KismetAISaveSkipReason::NothingApplied;
		Result.Data = MakeShared<FJsonObject>();
		AttachMutationOutcome(Result.Data, Outcome);
		return Result;
	}

	// --- Spawn + optional wire + layout under ONE transaction, so connect_from's
	//     reverse link on the existing node is undoable together with the spawned
	//     node (a separate transaction per branch would commit the spawn before the
	//     wire, leaving a non-undoable dangling link on undo). ---
	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(Blueprint);
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "InsertNode", "Kismet: Insert Node"));
	Graph->Modify();
	Blueprint->Modify();

	UEdGraphNode* NewNode = nullptr;
	if (!ComponentGetterVar.IsEmpty())
	{
		// Self-context getter bound to the component by name — exactly what the
		// Components-panel drag-drop produces. Binds by name in self context, so
		// it resolves the correctly-typed component pin even for an inherited C++
		// property that isn't BlueprintReadWrite.
		UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
		Graph->AddNode(GetNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		GetNode->VariableReference.SetSelfMember(FName(*ComponentGetterVar));
		GetNode->CreateNewGuid();
		GetNode->PostPlacedNewNode();
		GetNode->AllocateDefaultPins();
		NewNode = GetNode;
		BestMenuName = FString::Printf(TEXT("Get %s"), *ComponentGetterVar);
	}
	else if (!MemberGetterVar.IsEmpty())
	{
		// Plain member-variable getter ("Get <var>"). Pure node — valid in K2 and anim graphs.
		UK2Node_VariableGet* GetNode = NewObject<UK2Node_VariableGet>(Graph);
		Graph->AddNode(GetNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		GetNode->VariableReference.SetSelfMember(FName(*MemberGetterVar));
		GetNode->CreateNewGuid();
		GetNode->PostPlacedNewNode();
		GetNode->AllocateDefaultPins();
		NewNode = GetNode;
		BestMenuName = FString::Printf(TEXT("Get %s"), *MemberGetterVar);
	}
	else if (bUseMacro)
	{
		UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Graph);
		MacroNode->SetMacroGraph(BestMacroGraph);
		Graph->AddNode(MacroNode, /*bFromUI*/ false, /*bSelectNewNode*/ false);
		MacroNode->CreateNewGuid();
		MacroNode->PostPlacedNewNode();
		MacroNode->AllocateDefaultPins();
		NewNode = MacroNode;
		BestMenuName = BestMacroName;
	}
	else
	{
		IBlueprintNodeBinder::FBindingSet EmptyBindings;
		NewNode = BestSpawner->Invoke(Graph, EmptyBindings, FVector2D(0.0f, 0.0f));
	}

	if (!NewNode)
	{
		return FToolExecutionResult::Error(500, TEXT("SPAWN_FAILED"),
			FString::Printf(TEXT("Matched '%s' but the node spawner produced no node."), *BestMenuName),
			{TEXT("This node may require a context that isn't available in this graph.")});
	}

	// --- Optional: wire an exec input from an existing node ---
	FString WireResult;
	const TSharedPtr<FJsonObject>* ConnectFromObj = nullptr;
	if (Body->TryGetObjectField(TEXT("connect_from"), ConnectFromObj) && ConnectFromObj && ConnectFromObj->IsValid())
	{
		// In an anim graph "exec" is replaced by pose flow: the source node's pose OUTPUT
		// feeds the new node's first pose INPUT. Pose inputs are single-link and the schema
		// auto-breaks any prior link, so there is no exec-style splice — a re-link just
		// re-parents the pose. Default source pin is "Pose"/the node's pose output.
		const FString FromNodeName = (*ConnectFromObj)->HasField(TEXT("node")) ? (*ConnectFromObj)->GetStringField(TEXT("node")) : FString();
		const FString DefaultFromPin = bAnimGraph ? TEXT("") : TEXT("then");
		const FString FromPinName = (*ConnectFromObj)->HasField(TEXT("pin")) ? (*ConnectFromObj)->GetStringField(TEXT("pin")) : DefaultFromPin;

		UEdGraphNode* FromNode = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->GetName() == FromNodeName) { FromNode = N; break; }
		}
		if (!FromNode)
		{
			WireResult = FString::Printf(TEXT("connect_from skipped: node '%s' not found in graph"), *FromNodeName);
		}
		else if (bAnimGraph)
		{
			// Source pose output: named pin if given, else the node's first pose output.
			UEdGraphPin* FromPin = FromPinName.IsEmpty()
				? AnimSpawnerHelpers::GetFirstPoseOutputPin(FromNode)
				: FromNode->FindPin(FName(*FromPinName), EGPD_Output);
			UEdGraphPin* ToPin = AnimSpawnerHelpers::GetFirstPoseInputPin(NewNode);
			if (!FromPin)
			{
				WireResult = FString::Printf(TEXT("connect_from skipped: no pose output%s found on '%s'"),
					FromPinName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" '%s'"), *FromPinName), *FromNodeName);
			}
			else if (!UAnimationGraphSchema::IsPosePin(FromPin->PinType))
			{
				WireResult = FString::Printf(TEXT("connect_from skipped: pin '%s' on '%s' is not a pose pin"), *FromPin->PinName.ToString(), *FromNodeName);
			}
			else if (!ToPin)
			{
				// A pure data node (variable getter, pure function) has no pose input. Skip
				// pose wiring gracefully rather than failing the whole insert — the node is
				// placed and its pins returned; the agent wires its value pin with edit_blueprint.
				WireResult = FString::Printf(TEXT("connect_from skipped: inserted node '%s' has no pose input (it is a pure data node) — wire its value output with edit_blueprint"), *NewNode->GetName());
			}
			else
			{
				FromNode->Modify();
				const FPinConnectionResponse Resp = Graph->GetSchema()->CanCreateConnection(FromPin, ToPin);
				if (Resp.Response != CONNECT_RESPONSE_DISALLOW && Graph->GetSchema()->TryCreateConnection(FromPin, ToPin))
				{
					WireResult = FString::Printf(TEXT("wired pose %s.%s -> %s.%s"),
						*FromNodeName, *FromPin->PinName.ToString(), *NewNode->GetName(), *ToPin->PinName.ToString());
				}
				else
				{
					FString Reason = Resp.Message.ToString();
					if (Reason.IsEmpty()) Reason = TEXT("schema rejected the pose connection (check local vs component space)");
					WireResult = FString::Printf(TEXT("connect_from failed: %s"), *Reason);
				}
			}
		}
		else
		{
			UEdGraphPin* FromPin = FromNode->FindPin(FName(*FromPinName), EGPD_Output);
			// New node's exec input (first exec input pin).
			UEdGraphPin* ToPin = nullptr;
			for (UEdGraphPin* P : NewNode->Pins)
			{
				if (P && P->Direction == EGPD_Input && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					ToPin = P; break;
				}
			}
			if (!FromPin)
			{
				WireResult = FString::Printf(TEXT("connect_from skipped: output pin '%s' not found on '%s'"), *FromPinName, *FromNodeName);
			}
			else if (!ToPin)
			{
				WireResult = TEXT("connect_from skipped: inserted node has no exec input (it may be a pure node)");
			}
			else
			{
				bool bSplice = false;
				(*ConnectFromObj)->TryGetBoolField(TEXT("splice"), bSplice);

				// An exec output holds at most one link, so connecting it re-routes
				// any existing one. Capture the old target first: splice re-wires it
				// to the new node's exec output, keeping the chain intact.
				UEdGraphPin* OldTarget = (FromPin->LinkedTo.Num() > 0) ? FromPin->LinkedTo[0] : nullptr;

				// Record the existing node so its reverse link is captured by the
				// transaction (the spawned node is already transacted from AddNode).
				FromNode->Modify();
				if (Graph->GetSchema()->TryCreateConnection(FromPin, ToPin))
				{
					WireResult = FString::Printf(TEXT("wired %s.%s -> %s.execute"), *FromNodeName, *FromPinName, *NewNode->GetName());

					if (bSplice && OldTarget)
					{
						// Prefer the conventional "then" pin; fall back to the first exec output.
						UEdGraphPin* NewOut = NewNode->FindPin(TEXT("then"), EGPD_Output);
						if (!NewOut || NewOut->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						{
							NewOut = nullptr;
							for (UEdGraphPin* P : NewNode->Pins)
							{
								if (P && P->Direction == EGPD_Output && P->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
								{
									NewOut = P;
									break;
								}
							}
						}
						if (!NewOut)
						{
							WireResult += FString::Printf(TEXT("; splice skipped: inserted node has no exec output — %s.%s is now disconnected, rewire it with edit_blueprint"),
								*OldTarget->GetOwningNode()->GetName(), *OldTarget->PinName.ToString());
						}
						else
						{
							OldTarget->GetOwningNode()->Modify();
							if (Graph->GetSchema()->TryCreateConnection(NewOut, OldTarget))
							{
								WireResult += FString::Printf(TEXT("; spliced: %s.%s -> %s.%s (previous link re-routed through the new node)"),
									*NewNode->GetName(), *NewOut->PinName.ToString(),
									*OldTarget->GetOwningNode()->GetName(), *OldTarget->PinName.ToString());
							}
							else
							{
								WireResult += FString::Printf(TEXT("; splice failed: schema rejected %s.%s -> %s.%s — rewire it with edit_blueprint"),
									*NewNode->GetName(), *NewOut->PinName.ToString(),
									*OldTarget->GetOwningNode()->GetName(), *OldTarget->PinName.ToString());
							}
						}
					}
					else if (bSplice)
					{
						WireResult += TEXT("; splice: source pin had no existing link (plain append)");
					}
					else if (OldTarget)
					{
						WireResult += FString::Printf(TEXT("; note: %s.%s previously linked to %s.%s and is now disconnected (pass splice=true to re-route it through the new node)"),
							*FromNodeName, *FromPinName,
							*OldTarget->GetOwningNode()->GetName(), *OldTarget->PinName.ToString());
					}
				}
				else
				{
					WireResult = FString::Printf(TEXT("connect_from failed: schema rejected %s.%s -> exec input"), *FromNodeName, *FromPinName);
				}
			}
		}
	}

	// --- Optional: wire this NEW node's output into an existing node's input ---
	// connect_to is the mirror of connect_from for the dominant "feed a downstream
	// node" flow. Giving the new node a real link to its target at insert time gives
	// the layout a true anchor, so it lands adjacent (left of the input it feeds)
	// instead of stranded off to the right. Works for exec, pose, and data pins —
	// the side and compatibility are decided by the schema. If the target pin name is
	// omitted, the first schema-compatible input is chosen.
	FString WireToResult;
	const TSharedPtr<FJsonObject>* ConnectToObj = nullptr;
	if (Body->TryGetObjectField(TEXT("connect_to"), ConnectToObj) && ConnectToObj && ConnectToObj->IsValid())
	{
		const FString ToNodeName = (*ConnectToObj)->HasField(TEXT("node")) ? (*ConnectToObj)->GetStringField(TEXT("node")) : FString();
		const FString ToPinName = (*ConnectToObj)->HasField(TEXT("pin")) ? (*ConnectToObj)->GetStringField(TEXT("pin")) : FString();

		UEdGraphNode* ToNode = nullptr;
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N && N->GetName() == ToNodeName) { ToNode = N; break; }
		}
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (ToNodeName.IsEmpty())
		{
			WireToResult = TEXT("connect_to skipped: 'node' (target) is required");
		}
		else if (!ToNode)
		{
			WireToResult = FString::Printf(TEXT("connect_to skipped: node '%s' not found in graph — node still inserted, wire it with edit_blueprint"), *ToNodeName);
		}
		else if (!Schema)
		{
			WireToResult = TEXT("connect_to skipped: graph has no schema");
		}
		else
		{
			// Candidate target input pins: the named one, or every input pin (we let
			// the schema pick the first compatible).
			TArray<UEdGraphPin*> TargetCandidates;
			if (!ToPinName.IsEmpty())
			{
				if (UEdGraphPin* Named = ToNode->FindPin(FName(*ToPinName), EGPD_Input))
				{
					TargetCandidates.Add(Named);
				}
			}
			else
			{
				for (UEdGraphPin* P : ToNode->Pins)
				{
					if (P && P->Direction == EGPD_Input)
					{
						TargetCandidates.Add(P);
					}
				}
			}

			if (TargetCandidates.Num() == 0)
			{
				WireToResult = ToPinName.IsEmpty()
					? FString::Printf(TEXT("connect_to skipped: '%s' has no input pins — node still inserted, wire it with edit_blueprint"), *ToNodeName)
					: FString::Printf(TEXT("connect_to skipped: input pin '%s' not found on '%s' — node still inserted, wire it with edit_blueprint"), *ToPinName, *ToNodeName);
			}
			else
			{
				// First schema-compatible (new node OUTPUT -> target INPUT) pair wins.
				// Iterating target pins in the outer loop keeps a named pin deterministic
				// and, for the auto-pick, prefers the target's pin order.
				UEdGraphPin* ChosenOut = nullptr;
				UEdGraphPin* ChosenIn = nullptr;
				for (UEdGraphPin* TPin : TargetCandidates)
				{
					for (UEdGraphPin* OutPin : NewNode->Pins)
					{
						if (!OutPin || OutPin->Direction != EGPD_Output) continue;
						const FPinConnectionResponse Resp = Schema->CanCreateConnection(OutPin, TPin);
						if (Resp.Response != CONNECT_RESPONSE_DISALLOW)
						{
							ChosenOut = OutPin;
							ChosenIn = TPin;
							break;
						}
					}
					if (ChosenOut) break;
				}

				if (!ChosenOut || !ChosenIn)
				{
					WireToResult = ToPinName.IsEmpty()
						? FString::Printf(TEXT("connect_to skipped: no compatible output on the inserted node fits any input of '%s' — node still inserted, wire it with edit_blueprint"), *ToNodeName)
						: FString::Printf(TEXT("connect_to skipped: no compatible output on the inserted node fits '%s.%s' — node still inserted, wire it with edit_blueprint"), *ToNodeName, *ToPinName);
				}
				else
				{
					// Record the existing node so its reverse link is captured by the
					// transaction. TryCreateConnection auto-breaks any prior link on a
					// single-input pin (exec/pose), which is the desired re-parent.
					ToNode->Modify();
					if (Schema->TryCreateConnection(ChosenOut, ChosenIn))
					{
						WireToResult = FString::Printf(TEXT("wired %s.%s -> %s.%s"),
							*NewNode->GetName(), *ChosenOut->PinName.ToString(), *ToNodeName, *ChosenIn->PinName.ToString());
					}
					else
					{
						WireToResult = FString::Printf(TEXT("connect_to failed: schema rejected %s.%s -> %s.%s — node still inserted, wire it with edit_blueprint"),
							*NewNode->GetName(), *ChosenOut->PinName.ToString(), *ToNodeName, *ChosenIn->PinName.ToString());
					}
				}
			}
		}
	}

	// --- Placement + compile: only the new node gets a position ---
	// If the node ended up with NO link (no connect_from / connect_to wired it), the
	// layout parks it as an island; track that spot so a later edit_blueprint connect
	// can pull it next to its peer (reposition-on-first-wire).
	{
		TSet<UEdGraphNode*> NewNodes;
		NewNodes.Add(NewNode);
		TSet<UEdGraphNode*> IslandNodes;
		FKismetAILayout::PlaceNewNodes(Graph, NewNodes, &IslandNodes);
		RecordUnanchoredSpawns(AssetPath, IslandNodes);

		// Opt-in: reflow the component the new node joined. Seeds expand to the
		// whole connected component, so authored positions in that component are
		// overwritten — only done when the caller passed auto_layout=true.
		if (bAutoLayout)
		{
			FKismetAILayout::FormatNodes(Graph, NewNodes);
		}
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	FCompilerResultsLog Results;
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::None, &Results);
	const bool bCompileSuccess = (Blueprint->Status != BS_Error);

	// Record the compile, then persist and record the save.
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	Outcome.bAppliedInMemory = true; // the node was spawned
	RecordCompileOutcome(Outcome, Blueprint, Results);
	AutoSaveAfterMutation(Blueprint, Outcome);

	// --- Build response: real pins of the spawned node ---
	TArray<TSharedPtr<FJsonValue>> PinsArr;
	for (UEdGraphPin* Pin : NewNode->Pins)
	{
		if (!Pin) continue;
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("name"), Pin->PinName.ToString());
		PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
		PinObj->SetStringField(TEXT("category"), Pin->PinType.PinCategory.ToString());
		if (Pin->PinType.PinSubCategory != NAME_None)
		{
			PinObj->SetStringField(TEXT("subcategory"), Pin->PinType.PinSubCategory.ToString());
		}
		if (const UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get())
		{
			PinObj->SetStringField(TEXT("subcategory_object"), SubObj->GetName());
		}
		// Pose pins: report their space so the agent wires local→local / component→component.
		const FString PoseSpace = AnimSpawnerHelpers::PoseSpaceString(Pin->PinType);
		if (!PoseSpace.IsEmpty())
		{
			PinObj->SetBoolField(TEXT("pose"), true);
			PinObj->SetStringField(TEXT("pose_space"), PoseSpace);
		}
		if (!Pin->DefaultValue.IsEmpty())
		{
			PinObj->SetStringField(TEXT("default"), Pin->DefaultValue);
		}
		if (Pin->bHidden)
		{
			PinObj->SetBoolField(TEXT("hidden"), true);
		}
		PinsArr.Add(MakeShared<FJsonValueObject>(PinObj));
	}

	TArray<TSharedPtr<FJsonValue>> ErrorsArray;
	for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
	{
		if (Message->GetSeverity() != EMessageSeverity::Error) continue;
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("severity"), TEXT("error"));
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());
		ErrorsArray.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetStringField(TEXT("node_name"), NewNode->GetName());
	Data->SetStringField(TEXT("node_class"), NewNode->GetClass()->GetName());
	Data->SetStringField(TEXT("matched_menu_name"), BestMenuName);
	Data->SetArrayField(TEXT("pins"), PinsArr);
	Data->SetBoolField(TEXT("compile_success"), bCompileSuccess);
	if (ErrorsArray.Num() > 0)
	{
		Data->SetArrayField(TEXT("errors"), ErrorsArray);
	}
	if (!WireResult.IsEmpty())
	{
		Data->SetStringField(TEXT("connect_from_result"), WireResult);
	}
	if (!WireToResult.IsEmpty())
	{
		Data->SetStringField(TEXT("connect_to_result"), WireToResult);
	}
	Data->SetStringField(TEXT("note"), TEXT("Node spawned with real pins. Wire it to other nodes with edit_blueprint using the pin NAMES above. Do not re-create it with write_blueprint."));

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoInsertNode. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleInsertNode(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("insert_node"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


// ─────────────────────────────────────────────────────────────────────────────
// read_source handler
// ─────────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────────
// read_logs — grep the current session's UE log file (Saved/Logs/<Project>.log)
// ─────────────────────────────────────────────────────────────────────────────
FToolExecutionResult FKismetAIModule::DoReadLogs(const TSharedPtr<FJsonObject>& Args)
{
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		Body = MakeShared<FJsonObject>(); // every field is optional
	}

	FString Pattern;
	Body->TryGetStringField(TEXT("pattern"), Pattern);
	const FString Level = Body->HasField(TEXT("level")) ? Body->GetStringField(TEXT("level")).ToLower() : TEXT("warning");
	FString Category;
	Body->TryGetStringField(TEXT("category"), Category);
	int32 MaxLines = Body->HasField(TEXT("lines")) ? static_cast<int32>(Body->GetNumberField(TEXT("lines"))) : 100;
	MaxLines = FMath::Clamp(MaxLines, 1, 500);
	const bool bTail = Body->HasField(TEXT("tail")) ? Body->GetBoolField(TEXT("tail")) : true;

	// Flush so lines the agent just caused (compile, PIE, asset load) are on disk.
	if (GLog)
	{
		GLog->Flush();
	}

	const FString SessionLogPath = FPaths::ConvertRelativePathToFull(FPlatformOutputDevices::GetAbsoluteLogFilename());

	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*SessionLogPath));
	if (!Reader)
	{
		return FToolExecutionResult::Error(404, TEXT("LOG_NOT_FOUND"),
			FString::Printf(TEXT("Could not open the session log: %s"), *SessionLogPath), {});
	}

	// Bound the scan to the last few MB so a huge log can't blow the context window.
	constexpr int64 ScanCapBytes = 4 * 1024 * 1024;
	const int64 FileSize = Reader->TotalSize();
	const int64 ReadStart = FMath::Max<int64>(0, FileSize - ScanCapBytes);
	const bool bCappedRead = (ReadStart > 0);
	const int64 ToRead = FileSize - ReadStart;

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(ToRead) + 1);
	Reader->Seek(ReadStart);
	Reader->Serialize(Bytes.GetData(), ToRead);
	Reader->Close();
	Bytes[static_cast<int32>(ToRead)] = 0;

	// UE writes the log as UTF-8.
	const FString Content(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Bytes.GetData())));

	TArray<FString> AllLines;
	Content.ParseIntoArrayLines(AllLines, /*CullEmpty=*/false);
	// A capped read likely starts mid-line; drop the first (partial) line.
	if (bCappedRead && AllLines.Num() > 0)
	{
		AllLines.RemoveAt(0);
	}

	const bool bHasPattern = !Pattern.IsEmpty();
	FRegexPattern RegexPattern(Pattern);

	auto PassesLevel = [&Level](const FString& Line) -> bool
	{
		if (Level == TEXT("error"))   return Line.Contains(TEXT("Error:"));
		if (Level == TEXT("warning")) return Line.Contains(TEXT("Warning:")) || Line.Contains(TEXT("Error:"));
		return true; // display / all / verbose — no level filter
	};

	TArray<FString> Matched;
	for (const FString& Line : AllLines)
	{
		if (Line.IsEmpty()) continue;
		if (!PassesLevel(Line)) continue;
		if (!Category.IsEmpty() && !Line.Contains(Category, ESearchCase::IgnoreCase)) continue;
		if (bHasPattern)
		{
			FRegexMatcher M(RegexPattern, Line);
			if (!M.FindNext()) continue;
		}
		Matched.Add(Line);
	}

	const int32 TotalMatched = Matched.Num();
	TArray<TSharedPtr<FJsonValue>> OutLines;
	if (bTail)
	{
		for (int32 i = FMath::Max(0, TotalMatched - MaxLines); i < TotalMatched; ++i)
			OutLines.Add(MakeShared<FJsonValueString>(Matched[i]));
	}
	else
	{
		for (int32 i = 0; i < FMath::Min(MaxLines, TotalMatched); ++i)
			OutLines.Add(MakeShared<FJsonValueString>(Matched[i]));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("lines"), OutLines);
	Data->SetNumberField(TEXT("matched"), TotalMatched);
	Data->SetNumberField(TEXT("returned"), OutLines.Num());
	Data->SetBoolField(TEXT("truncated"), (OutLines.Num() < TotalMatched) || bCappedRead);
	Data->SetStringField(TEXT("log_path"), SessionLogPath);
	Data->SetStringField(TEXT("level"), Level);
	if (bCappedRead)
	{
		Data->SetStringField(TEXT("note"),
			FString::Printf(TEXT("Scanned the last %lldMB of the log (it is larger); pre-that history not included."), ScanCapBytes / (1024 * 1024)));
	}

	return FToolExecutionResult::Success(Data);
}

// HTTP adapter over DoReadLogs. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleReadLogs(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_logs"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


FToolExecutionResult FKismetAIModule::DoReadSource(const TSharedPtr<FJsonObject>& Args)
{
	return KismetAISourceService::Execute(Args, KismetAISourceService::Prepare(), FKismetAICancellationToken());
}

// HTTP adapter over DoReadSource. SendError is used for failures so the per-error-code
// default suggestions still apply on the HTTP path.
bool FKismetAIModule::HandleCancelSource(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const auto Body = ParseRequestBody(Request);
	FString Id;
	if (!Body.IsValid() || !Body->TryGetStringField(TEXT("request_id"), Id) || Id.IsEmpty() || Id.Len() > 128)
	{ SendError(OnComplete, 400, TEXT("INVALID_REQUEST"), TEXT("A source request_id is required.")); return true; }
	auto Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("cancel_requested"), SourceJobs.IsValid() && SourceJobs->Cancel(TEXT("rest:") + Id));
	SendSuccess(OnComplete, Data);
	return true;
}

bool FKismetAIModule::HandleReadSource(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	return ServeToolRoute(TEXT("read_source"), nullptr, FKismetAICallerContext::InProcess(), Request, OnComplete);
}


bool FKismetAIModule::HandleSkills(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	const TSharedPtr<FJsonObject> Body = ParseRequestBody(Request);
	FString Action, Payload;
	if (!Body.IsValid() || !Body->TryGetStringField(TEXT("action"), Action) || !Body->TryGetStringField(TEXT("payload"), Payload))
	{
		SendError(OnComplete, 400, TEXT("invalid_skill_command"), TEXT("Expected action and payload strings."));
		return true;
	}
	bool bRegistry = false;
	Body->TryGetBoolField(TEXT("registry"), bRegistry);
	auto Work = [Action, Payload, bRegistry, OnComplete]()
	{
		auto Respond = [OnComplete](const FString& Json)
		{
			OnComplete(FHttpServerResponse::Create(Json, TEXT("application/json")));
		};
		if (bRegistry) KismetAIPlatformCommands::Registry(Action, Payload, Respond);
		else Respond(KismetAIPlatformCommands::Local(Action, Payload));
	};
	if (IsInGameThread()) Work();
	else AsyncTask(ENamedThreads::GameThread, MoveTemp(Work));
	return true;
}
