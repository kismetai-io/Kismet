// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "KismetAISettings.generated.h"

UENUM()
enum class EKismetAIBackend : uint8
{
	Production UMETA(DisplayName = "Production (kismetai.io)"),
	Development UMETA(DisplayName = "Development (dev.kismetai.io)"),
	Custom UMETA(DisplayName = "Custom")
};

UCLASS(config=KismetAI, defaultconfig, meta=(DisplayName="KismetAI"))
class KISMETAI_API UKismetAISettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	UPROPERTY(EditAnywhere, config, Category = "Backend")
	EKismetAIBackend Backend = EKismetAIBackend::Production;

	UPROPERTY(EditAnywhere, config, Category = "Backend", meta=(EditCondition="Backend == EKismetAIBackend::Custom"))
	FString CustomUrl;
};
