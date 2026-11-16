// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

struct KISMETAIRUNTIME_API FKismetAISkill
{
	FString Publisher;
	FString Name;
	FString Version;
	FString Description;
	FString Instructions;
	FString Json;
	FString Sha256;
	TSet<FString> RequiredTools;
	FString Id() const { return Publisher + TEXT("/") + Name + TEXT("/") + Version; }
};

// JSON-only packages: resources remain data and are never extracted or executed.
namespace KismetAISkills
{
	KISMETAIRUNTIME_API bool Parse(const FString& Json, const FString& PluginVersion, const FString& EngineVersion, FKismetAISkill& Out, FString& Error);
	KISMETAIRUNTIME_API bool Install(const FString& Root, const FKismetAISkill& Skill, FString& Error);
	KISMETAIRUNTIME_API bool Remove(const FString& Root, const FString& Id, FString& Error);
	KISMETAIRUNTIME_API bool Load(const FString& Root, const FString& Id, FKismetAISkill& Out, FString& Error);
	KISMETAIRUNTIME_API TArray<FKismetAISkill> List(const FString& Root);
	KISMETAIRUNTIME_API FString DefaultRoot();
	KISMETAIRUNTIME_API FString PluginVersion();
	KISMETAIRUNTIME_API FString EngineVersion();
}
