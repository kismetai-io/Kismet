// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Pure (no-UObject) helper for plan file serialization.
 * All functions are deterministic — no Date/Guid/random calls inside.
 */
class KISMETAIRUNTIME_API FKismetAIPlanFile
{
public:
	/**
	 * Lowercase the title, replace every run of non-alphanumeric characters
	 * with a single underscore, then trim leading/trailing underscores.
	 * Returns "plan" if the result would be empty.
	 */
	static FString Slugify(const FString& Title);

	/**
	 * Combine a slug and an 8-character hash into a plan filename.
	 * Format: <Slug>_<Hash8>.plan.md
	 * The Hash is passed in — not generated here — so the function is deterministic.
	 */
	static FString MakeFileName(const FString& Slug, const FString& Hash8);

	/**
	 * Render a Markdown document for the plan file.
	 *
	 * Structure:
	 *   ---
	 *   title: <Title>
	 *   slug: <Slug>
	 *   created: <CreatedIso>
	 *   ---
	 *
	 *   # <Title>
	 *
	 *   <Description>          (omitted if empty)
	 *
	 *   ## To-dos              (omitted if Todos is empty)
	 *   - [x] <content>        for completed entries
	 *   - [ ] <content>        for pending entries
	 *
	 *   ## Plan
	 *   <Body>
	 *
	 * Each Todo is a JSON object with string fields "content" and "status".
	 * Malformed entries (not an object, missing fields) are silently skipped.
	 * Status == "completed" renders as checked; anything else renders unchecked.
	 */
	static FString BuildMarkdown(
		const FString& Title,
		const FString& Description,
		const FString& Body,
		const TArray<TSharedPtr<FJsonValue>>& Todos,
		const FString& Slug,
		const FString& CreatedIso);

	/**
	 * Build the plan_ready SSE event payload.
	 *
	 * Returned JSON object fields:
	 *   type        (string) = "plan_ready"
	 *   plan        (string) = Body  — backwards-compatible: callers that read
	 *                                  only "plan" still get the human-readable text
	 *   title       (string)
	 *   description (string)
	 *   file_path   (string)
	 *   slug        (string)
	 *   todos       (array)  = the Todos array passed in, unchanged
	 *
	 * "created" is intentionally omitted from the event (it belongs in the file).
	 */
	static TSharedPtr<FJsonObject> BuildPlanReadyEvent(
		const FString& Title,
		const FString& Description,
		const FString& Body,
		const TArray<TSharedPtr<FJsonValue>>& Todos,
		const FString& FilePath,
		const FString& Slug);
};
