// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The budget of the request that will actually be sent.
//
// Context-pressure decisions used to be made from the input count the provider
// reported for the PREVIOUS request. That count describes a request that no
// longer exists: microcompaction may since have cleared most of it, and tool
// results, nudges and notes appended since were never in it. Deciding from it
// summarized conversations that clearing one old result had already fixed, and
// sent new large results without looking at them.
//
// Three separate quantities, kept apart in code and in diagnostics:
//
//   * measured — what the provider reported for the last request it accepted
//     (FKismetAIUsageCalibration). Exact, but for a request already sent.
//   * estimated — a local, provider-neutral estimate of the candidate request
//     (FKismetAIRequestEstimate): system prompt + every tool schema that will be
//     sent (discovered extended tools included) + the messages as they stand
//     after every projection change + a flat cost per image/document. The
//     output reservation is NOT part of the input estimate; it is subtracted from
//     the window instead (FKismetAIContextBudget).
//   * limit — the window this model is assumed to have and where that figure
//     came from (catalog, model-id suffix, or a conservative assumption for an
//     id the catalog does not know).
//
// When a measurement exists, the estimate is anchored to it: the provider's
// count for the last request plus the estimated CHANGE since (scaled by how far
// the local heuristic under-counted that request, within [1, 4]), never below
// the plain local estimate. So clearing a huge result lowers the figure by
// about that result, and appending one raises it, without pretending the local
// heuristic is a tokenizer.
//
// Not promised: exact provider token counts. The heuristic (about four
// characters per token) under-counts dense JSON/T3D and non-Latin text and
// over-counts plain English; the calibration narrows that gap once a request
// has been measured. The provider's prompt_too_long stays the final authority.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Where a context-window figure came from.
enum class EKismetAIContextLimitSource : uint8
{
	// The model catalog (Config/models.json) names this model.
	Catalog,
	// A named catalog model supports at least the requested -1m window.
	ModelSuffix,
	// The catalog does not know the id. The budget assumes the smaller of the
	// catalog default and UnknownModelWindow instead of treating the default as
	// a fact (an unknown id is most often a third-party or local model).
	AssumedUnknownModel,
};

// The usable input window for the next request.
struct KISMETAIRUNTIME_API FKismetAIContextBudget
{
	// Operating input + output window, restricted by an explicit configured limit.
	int32 ContextWindow = 0;
	// Catalog capacity or labelled assumption before applying an operating restriction.
	int32 UnrestrictedContextWindow = 0;
	// User-configured restriction; zero means unset. This is not model capability evidence.
	int32 OperatingContextLimit = 0;
	// What the catalog (or its defaults) says, for diagnostics. Equal to
	// UnrestrictedContextWindow unless the limit is an assumption.
	int32 CatalogWindow = 0;
	EKismetAIContextLimitSource LimitSource = EKismetAIContextLimitSource::Catalog;
	// max_tokens the next request will carry (reasoning/thinking tokens are
	// billed against it by every supported provider).
	int32 RequestedMaxTokens = 0;
	// The part of the window held back for output: RequestedMaxTokens capped at
	// OutputReserveCap. The model client shrinks max_tokens on an input+output
	// overflow (down to a floor of a few thousand tokens), so holding back an
	// unusually large max_tokens in full would compact conversations that fit.
	int32 OutputReserve = 0;
	// ContextWindow - OutputReserve; at least 1 (never "unknown = unlimited").
	int32 EffectiveLimit = 0;

	static constexpr int32 OutputReserveCap = 20000;
	// Assumed window for an id the catalog does not know. Conservative: the
	// smallest window among the long-context hosted models this plugin targets,
	// well under the 200k catalog default, which describes an unknown Claude id.
	static constexpr int32 UnknownModelWindow = 128000;

	static const TCHAR* LimitSourceName(EKismetAIContextLimitSource Source);

	// The budget for Model with RequestedMaxTokens of output.
	static FKismetAIContextBudget Resolve(const FString& Model, int32 RequestedMaxTokens, int32 OperatingContextLimit = 0);
};

// One provider measurement: the input count the provider reported for a
// request, beside the local estimate of that same request.
struct KISMETAIRUNTIME_API FKismetAIUsageCalibration
{
	bool bValid = false;
	int32 ReportedInputTokens = 0;
	int32 EstimatedInputTokens = 0;

	// How much the local heuristic under-counted the measured request, clamped
	// to [1, MaxRatio]. Below 1 the local estimate is used as is (conservative);
	// above MaxRatio the measurement is implausible for the heuristic (about one
	// character per token is the densest real text) and is not extrapolated.
	double Ratio() const;
	static constexpr double MaxRatio = 4.0;
};

// The estimated input of one candidate request, with its parts and provenance.
struct KISMETAIRUNTIME_API FKismetAIRequestEstimate
{
	// Local components (heuristic).
	int32 SystemTokens = 0;
	int32 ToolSchemaTokens = 0;
	int32 ToolCount = 0;
	int32 MessageTokens = 0;     // includes attachments
	int32 MessageCount = 0;
	int32 AttachmentCount = 0;   // image and document blocks, anywhere in the messages
	int32 LocalTokens = 0;       // system + tools + messages

	// The decision figure: LocalTokens when uncalibrated; otherwise
	// max(LocalTokens, Reported + Ratio * (LocalTokens - EstimatedAtReport)).
	int32 Tokens = 0;
	bool bCalibrated = false;
	double CalibrationRatio = 1.0;
	FKismetAIUsageCalibration Calibration;

	FKismetAIContextBudget Budget;

	// Flat per-attachment cost the estimate assumes (an image the provider
	// downsizes to its maximum is roughly this; a multi-page document can cost
	// more, which this estimate does not see).
	static constexpr int32 AttachmentTokensEach = 1600;

	bool ExceedsLimit() const { return Tokens > Budget.EffectiveLimit; }

	// "reported_usage_plus_estimated_delta" or "local_estimate".
	const TCHAR* BasisName() const;

	// Components and the decision figure (no budget fields).
	TSharedPtr<FJsonObject> ComponentsToJson() const;

	// Window, reservation, limit provenance and calibration — what a decision
	// was judged against.
	void WriteBudgetFields(FJsonObject& Out) const;

	// Everything: budget fields plus components.
	TSharedPtr<FJsonObject> ToJson() const;

	// Estimate a request carrying SystemPrompt, Tools and Messages.
	static FKismetAIRequestEstimate Estimate(const FString& SystemPrompt,
		const TArray<TSharedPtr<FJsonValue>>& Tools,
		const TArray<TSharedPtr<FJsonValue>>& Messages,
		const FKismetAIUsageCalibration& Calibration,
		const FKismetAIContextBudget& Budget);
};
