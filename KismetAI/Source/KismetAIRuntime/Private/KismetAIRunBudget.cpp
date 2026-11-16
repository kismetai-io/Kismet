// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAIRunBudget.h"
#include "KismetAIAgent.h"
#include "KismetAIModelCatalog.h"
#include "HAL/PlatformTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

bool FKismetAIRunLimits::ReadSettings(const FJsonObject& Parent, FKismetAIRunLimits& InOut, FString& Error)
{
	Error.Reset();
	if (!Parent.HasField(TEXT("run_budget"))) { Error = InOut.Validate(); return Error.IsEmpty(); }
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Parent.TryGetObjectField(TEXT("run_budget"), Object) || !Object || !Object->IsValid())
	{
		Error = TEXT("run_budget must be an object of finite nonnegative numeric limits.");
		return false;
	}
	FKismetAIRunLimits Candidate = InOut;
	for (const auto& Pair : (*Object)->Values)
	{
		const bool bSeconds = Pair.Key == TEXT("max_elapsed_seconds");
		const bool bModels = Pair.Key == TEXT("max_model_requests");
		const bool bTools = Pair.Key == TEXT("max_tool_calls");
		const bool bBytes = Pair.Key == TEXT("max_output_bytes");
		const bool bSpend = Pair.Key == TEXT("max_estimated_spend_usd");
		double Value = 0;
		if (!(bSeconds || bModels || bTools || bBytes || bSpend))
		{
			Error = TEXT("Unknown run_budget field: ") + FString(Pair.Key);
			return false;
		}
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number || !Pair.Value->TryGetNumber(Value)
			|| !FMath::IsFinite(Value) || Value < 0
			|| ((bModels || bTools || bBytes) && FMath::FloorToDouble(Value) != Value)
			|| ((bModels || bTools) && Value > MAX_int32)
			|| (bBytes && Value > 9007199254740991.0))
		{
			Error = TEXT("Invalid run_budget.") + FString(Pair.Key) + TEXT(": use a finite nonnegative number; counts/bytes must be exactly representable integers. Zero disables the limit.");
			return false;
		}
		if (bSeconds) Candidate.MaxElapsedSeconds = Value;
		else if (bModels) Candidate.MaxModelRequests = static_cast<int32>(Value);
		else if (bTools) Candidate.MaxToolCalls = static_cast<int32>(Value);
		else if (bBytes) Candidate.MaxOutputBytes = static_cast<int64>(Value);
		else Candidate.MaxEstimatedSpendUSD = Value;
	}
	Error = Candidate.Validate();
	if (!Error.IsEmpty()) return false;
	InOut = Candidate;
	return true;
}

FString FKismetAIRunLimits::Validate() const
{
	if (!FMath::IsFinite(MaxElapsedSeconds) || MaxElapsedSeconds < 0 || MaxModelRequests < 0 || MaxToolCalls < 0
		|| MaxOutputBytes < 0 || !FMath::IsFinite(MaxEstimatedSpendUSD) || MaxEstimatedSpendUSD < 0)
		return TEXT("Run limits must be finite nonnegative values; zero disables a dimension.");
	return FString();
}

TSharedPtr<FJsonObject> FKismetAIRunLimits::ToJson() const
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetNumberField(TEXT("max_elapsed_seconds"), FMath::IsFinite(MaxElapsedSeconds) ? MaxElapsedSeconds : 0);
	Out->SetNumberField(TEXT("max_model_requests"), MaxModelRequests);
	Out->SetNumberField(TEXT("max_tool_calls"), MaxToolCalls);
	Out->SetNumberField(TEXT("max_output_bytes"), MaxOutputBytes);
	Out->SetNumberField(TEXT("max_estimated_spend_usd"), FMath::IsFinite(MaxEstimatedSpendUSD) ? MaxEstimatedSpendUSD : 0);
	return Out;
}

FKismetAIRunBudget::FKismetAIRunBudget(const FKismetAIAgentConfig& Config, const FString& InProvider, double AdmittedAt)
	: Limits(Config.RunLimits), StartedAt(AdmittedAt > 0 ? AdmittedAt : FPlatformTime::Seconds()), Model(Config.Model)
	, Provider(InProvider), bFastMode(Config.bFastMode)
	, ConfigurationError(Config.RunBudgetError.IsEmpty() ? Limits.Validate() : Config.RunBudgetError)
{
	const FKismetAIModelEntry* Entry = FKismetAIModelCatalog::Get().Find(Model);
	bPriceAvailable = Entry && !Entry->bPatternOnly;
}

FString FKismetAIRunBudget::CheckCommon() const
{
	if (!ConfigurationError.IsEmpty()) return ConfigurationError;
	if (Limits.MaxElapsedSeconds > 0 && FPlatformTime::Seconds() - StartedAt >= Limits.MaxElapsedSeconds)
		return TEXT("The admitted run's elapsed-time limit was reached.");
	if (Limits.MaxOutputBytes > 0 && !bOutputComplete) return TEXT("The configured output limit cannot be evaluated: earlier attempt output is unavailable.");
	if (Limits.MaxOutputBytes > 0 && OutputBytes >= Limits.MaxOutputBytes)
		return TEXT("The run's decoded output-byte limit was reached.");
	if (Limits.MaxEstimatedSpendUSD > 0)
	{
		if (!bPriceAvailable) return TEXT("The configured spend limit cannot be evaluated: this model has no catalog price. No further request will be sent.");
		if (EstimatedSpendUSD >= Limits.MaxEstimatedSpendUSD) return TEXT("The run's estimated-spend limit was reached. This is a catalog estimate, not a billing guarantee.");
		if (!bUsageComplete) return TEXT("The configured spend limit cannot be evaluated completely: an attempt has unavailable usage. No further request will be sent.");
	}
	return FString();
}

FString FKismetAIRunBudget::CheckModel() const
{
	const FString Common = CheckCommon();
	if (!Common.IsEmpty()) return Common;
	if (Limits.MaxModelRequests > 0 && ModelRequests >= Limits.MaxModelRequests) return TEXT("The run's model-request limit was reached (including retries and summaries).");
	if (Limits.MaxToolCalls > 0 && ToolCalls >= Limits.MaxToolCalls) return TEXT("The run's tool-call limit was reached.");
	return FString();
}

FString FKismetAIRunBudget::CheckTool() const
{
	const FString Common = CheckCommon();
	if (!Common.IsEmpty()) return Common;
	if (Limits.MaxToolCalls > 0 && ToolCalls >= Limits.MaxToolCalls) return TEXT("The run's tool-call limit was reached.");
	return FString();
}

FString FKismetAIRunBudget::StartModel()
{
	FString Reason = CheckModel();
	if (Reason.IsEmpty()) ++ModelRequests;
	return Reason;
}
FString FKismetAIRunBudget::StartTool()
{
	FString Reason = CheckTool();
	if (Reason.IsEmpty()) ++ToolCalls;
	return Reason;
}
void FKismetAIRunBudget::ObserveToolOutput(const FString& Text)
{
	OutputBytes += FTCHARToUTF8(*Text).Length();
}

void FKismetAIRunBudget::ObserveModel(const FModelCallOutcome& Outcome)
{
	TransportRequests += Outcome.RequestCount;
	if (Outcome.Result != EModelCallResult::Success)
	{
		LastFailedModelError = Outcome.ErrorType;
		LastFailedModelHttpStatus = Outcome.HttpStatusCode;
	}
	TArray<TSharedPtr<FJsonValue>> Parts = KismetAIAgentCodec::ToParts(Outcome.AssistantMessage.Parts);
	const TArray<TSharedPtr<FJsonValue>>* FailedParts = nullptr;
	if (Parts.IsEmpty() && Outcome.Response.IsValid() && Outcome.Response->TryGetArrayField(TEXT("content"), FailedParts)) Parts = *FailedParts;
	if (!Parts.IsEmpty())
	{
		FString Json;
		auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		FJsonSerializer::Serialize(Parts, Writer);
		ObserveToolOutput(Json);
	}
	TArray<TSharedPtr<FJsonObject>> Reports = Outcome.UsageReports;
	const TSharedPtr<FJsonObject>* Usage = nullptr;
	if (Reports.IsEmpty() && Outcome.Response.IsValid() && Outcome.Response->TryGetObjectField(TEXT("usage"), Usage)) Reports.Add(*Usage);
	bUsageComplete &= !Outcome.bUsageIncomplete;
	if (Reports.IsEmpty() && Outcome.RequestCount > 0 && Outcome.Result != EModelCallResult::NetworkUnreachable) bUsageComplete = false;
	const FKismetAIModelEntry* Entry = FKismetAIModelCatalog::Get().Find(Model);
	for (const auto& Report : Reports)
	{
		if (!Report.IsValid()) { bUsageComplete = false; continue; }
		int32 Input = 0, Output = 0, Read = 0, Write = 0;
		const bool bInputKnown = Report->TryGetNumberField(TEXT("input_tokens"), Input);
		const bool bOutputKnown = Report->TryGetNumberField(TEXT("output_tokens"), Output);
		bHasReportedUsage |= bInputKnown || bOutputKnown;
		bUsageComplete &= bInputKnown && bOutputKnown;
		Report->TryGetNumberField(TEXT("cache_read_input_tokens"), Read);
		Report->TryGetNumberField(TEXT("cache_creation_input_tokens"), Write);
		Input = FMath::Max(0, Input); Output = FMath::Max(0, Output); Read = FMath::Max(0, Read); Write = FMath::Max(0, Write);
		if (!bPriceAvailable) continue;
		FString Speed;
		const bool bFastApplied = bFastMode && !(Report->TryGetStringField(TEXT("speed"), Speed) && Speed == TEXT("standard"));
		const auto Price = FKismetAIModelCatalog::PriceFor(*Entry, bFastApplied, FKismetAIAgent::ComputeContextInputTokens(Provider, Input, Read, Write));
		const int32 PricedInput = Provider.Equals(TEXT("openai"), ESearchCase::IgnoreCase) ? FMath::Max(0, Input - Read) : Input;
		EstimatedSpendUSD += (PricedInput * Price.Input + Output * Price.Output + Read * Price.CacheRead + Write * Price.CacheWrite) / 1000000.0;
	}
}

void FKismetAIRunBudget::ObserveLegacyModel(const FModelCallOutcome& Outcome)
{
	// Older injected clients return only an aggregate. The outer boundary still
	// accounts it once; production uses the per-attempt hooks instead.
	const int32 Attempts = FMath::Max(1, FMath::Max(Outcome.AttemptCount, Outcome.RequestCount));
	ModelRequests += Attempts;
	bOutputComplete &= Attempts == 1;
	ObserveModel(Outcome);
}

FString FKismetAIRunBudget::StopCode() const
{
	if (!ConfigurationError.IsEmpty()) return TEXT("invalid_run_budget");
	if (Limits.MaxOutputBytes > 0 && !bOutputComplete) return TEXT("run_budget_unavailable");
	if (Limits.MaxEstimatedSpendUSD > 0 && (!bPriceAvailable || (!bUsageComplete && EstimatedSpendUSD < Limits.MaxEstimatedSpendUSD))) return TEXT("run_budget_unavailable");
	return TEXT("run_budget_exhausted");
}

TSharedPtr<FJsonObject> FKismetAIRunBudget::ToJson() const
{
	auto Out = MakeShared<FJsonObject>();
	Out->SetObjectField(TEXT("limits"), Limits.ToJson());
	Out->SetStringField(TEXT("zero_limit"), TEXT("disabled; legacy turn and retry limits still apply"));
	Out->SetStringField(TEXT("enforcement"), TEXT("before new actions; in-flight outcomes and terminal records are always persisted"));
	Out->SetNumberField(TEXT("elapsed_seconds"), FMath::Max(0.0, FPlatformTime::Seconds() - StartedAt));
	Out->SetNumberField(TEXT("model_requests"), ModelRequests);
	Out->SetNumberField(TEXT("transport_requests"), TransportRequests);
	if (!LastFailedModelError.IsEmpty() || LastFailedModelHttpStatus > 0)
	{
		Out->SetStringField(TEXT("last_failed_model_error"), LastFailedModelError);
		Out->SetNumberField(TEXT("last_failed_model_http_status"), LastFailedModelHttpStatus);
	}
	Out->SetNumberField(TEXT("tool_calls"), ToolCalls);
	Out->SetNumberField(TEXT("output_bytes"), OutputBytes);
	Out->SetStringField(TEXT("output_bytes_basis"), TEXT("UTF-8 serialized decoded model content plus tool result text; includes failed attempts, excludes journal envelopes"));
	Out->SetBoolField(TEXT("output_complete"), bOutputComplete);
	Out->SetStringField(TEXT("usage_basis"), bHasReportedUsage ? TEXT("provider_reported") : TEXT("unavailable"));
	Out->SetBoolField(TEXT("usage_complete"), bUsageComplete);
	Out->SetStringField(TEXT("spend_basis"), bPriceAvailable && bHasReportedUsage ? TEXT("estimated") : TEXT("unavailable"));
	Out->SetNumberField(TEXT("estimated_spend_usd"), EstimatedSpendUSD);
	Out->SetStringField(TEXT("price_source"), bPriceAvailable ? TEXT("Config/models.json") : TEXT("unavailable"));
	Out->SetStringField(TEXT("price_as_of"), TEXT("unavailable: catalog has no structured rate date"));
	Out->SetStringField(TEXT("price_note"), TEXT("Local catalog estimate on reported tokens, not provider billing or a spending guarantee; unavailable usage is not zero cost."));
	if (!ConfigurationError.IsEmpty()) Out->SetStringField(TEXT("configuration_error"), ConfigurationError);
	return Out;
}
