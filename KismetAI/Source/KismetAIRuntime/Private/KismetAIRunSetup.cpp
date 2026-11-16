// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIRunSetup.h"
#include "KismetAIAgent.h"
#include "KismetAISkill.h"
#include "KismetAIProviderProfile.h"
#include "KismetAIModelClientInterface.h"
#include "Math/UnrealMathUtility.h"
#include "Dom/JsonObject.h"

bool KismetAIRunSetup::ReadOperatingContextLimit(const FJsonObject& Settings, int32& OutLimit, FString& OutError)
{
	const TSharedPtr<FJsonValue> Value = Settings.TryGetField(TEXT("operating_context_limit"));
	if (!Value.IsValid())
	{
		OutLimit = 0;
		return true;
	}
	double Number = 0;
	if (Value->Type != EJson::Number || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number)
		|| Number < 0 || Number > MAX_int32 || FMath::FloorToDouble(Number) != Number)
	{
		OutError = TEXT("operating_context_limit must be zero (unset) or a positive whole number no greater than 2147483647. Correct settings before running.");
		return false;
	}
	OutLimit = static_cast<int32>(Number);
	return true;
}

bool KismetAIRunSetup::RequiresProviderConfirmation(const FJsonObject& Settings)
{
	for (const TCHAR* Key : {TEXT("hosted_route"), TEXT("provider_confirmation_required")})
	{
		bool bRequired = false;
		const TSharedPtr<FJsonValue> Value = Settings.TryGetField(Key);
		if (Value.IsValid() && (Value->Type != EJson::Boolean || !Value->TryGetBool(bRequired) || bRequired)) return true;
	}
	return false;
}

bool KismetAIRunSetup::Prepare(FKismetAIAgentConfig& Config, FString& Error)
{
	if (!FModelCallRequest::ValidatePolicy(FModelCallRequest::SnapshotConfig(Config), Error)) return false;
	Config.bSkillRestricted = false;
	Config.SkillInstructions.Empty();
	Config.SkillTools.Empty();
	if (Config.bProviderConfirmationRequired)
	{
		Error = TEXT("Kismet-hosted inference has been retired. Open Skills & marketplace, review Provider settings, then choose Use my provider to enable your own provider connection and its charges.");
		return false;
	}
	if (!Config.SelectedSkill.IsEmpty())
	{
		FKismetAISkill Skill;
		if (!KismetAISkills::Load(KismetAISkills::DefaultRoot(), Config.SelectedSkill, Skill, Error)) return false;
		Config.bSkillRestricted = true;
		Config.SkillTools = Skill.RequiredTools;
		Config.SkillInstructions = Skill.Instructions;
	}
	return true;
}
