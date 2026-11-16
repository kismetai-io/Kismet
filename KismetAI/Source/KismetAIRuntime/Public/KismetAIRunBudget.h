// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FModelCallOutcome;
struct FKismetAIAgentConfig;

// Zero disables a dimension. Existing turn/retry limits remain independent.
struct KISMETAIRUNTIME_API FKismetAIRunLimits
{
	double MaxElapsedSeconds = 0;
	int32 MaxModelRequests = 0;
	int32 MaxToolCalls = 0;
	int64 MaxOutputBytes = 0;
	double MaxEstimatedSpendUSD = 0;

	// Transactional optional settings/HTTP override; omitted fields inherit.
	static bool ReadSettings(const FJsonObject& Parent, FKismetAIRunLimits& InOut, FString& Error);
	FString Validate() const;
	TSharedPtr<FJsonObject> ToJson() const;
};

// Worker-owned accounting, shared with attempt callbacks without retaining the
// agent. Limits and start time cannot change after admission. Checks stop new
// actions; an in-flight action's actual outcome must still be committed.
class KISMETAIRUNTIME_API FKismetAIRunBudget
{
public:
	FKismetAIRunBudget(const FKismetAIAgentConfig& Config, const FString& Provider, double AdmittedAt);
	FString CheckModel() const;
	FString CheckTool() const;
	FString StartModel();
	FString StartTool();
	void ObserveModel(const FModelCallOutcome& Outcome);
	void ObserveLegacyModel(const FModelCallOutcome& Outcome);
	void ObserveToolOutput(const FString& Text);
	int32 GetModelRequests() const { return ModelRequests; }
	FString StopCode() const;
	TSharedPtr<FJsonObject> ToJson() const;

private:
	FString CheckCommon() const;
	const FKismetAIRunLimits Limits;
	const double StartedAt;
	const FString Model;
	const FString Provider;
	const bool bFastMode;
	const FString ConfigurationError;
	bool bPriceAvailable = false;
	bool bUsageComplete = true;
	bool bHasReportedUsage = false;
	bool bOutputComplete = true;
	int32 ModelRequests = 0;
	int32 ToolCalls = 0;
	int32 TransportRequests = 0;
	int64 OutputBytes = 0;
	double EstimatedSpendUSD = 0;
	FString LastFailedModelError;
	int32 LastFailedModelHttpStatus = 0;
};
