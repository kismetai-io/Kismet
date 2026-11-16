// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIPlanFile.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ─────────────────────────────────────────────────────────────────────────────
// Slugify
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAIPlanFile::Slugify(const FString& Title)
{
	// Work character-by-character: lowercase alpha/digit passes through;
	// any other character contributes an underscore (runs are collapsed).
	FString Result;
	Result.Reserve(Title.Len());

	bool bPendingUnderscore = false;

	for (int32 i = 0; i < Title.Len(); ++i)
	{
		TCHAR Ch = Title[i];

		if (FChar::IsAlpha(Ch))
		{
			if (bPendingUnderscore && Result.Len() > 0)
			{
				Result.AppendChar(TEXT('_'));
			}
			bPendingUnderscore = false;
			Result.AppendChar(FChar::ToLower(Ch));
		}
		else if (FChar::IsDigit(Ch))
		{
			if (bPendingUnderscore && Result.Len() > 0)
			{
				Result.AppendChar(TEXT('_'));
			}
			bPendingUnderscore = false;
			Result.AppendChar(Ch);
		}
		else
		{
			// Non-alphanumeric: schedule an underscore (collapsed)
			bPendingUnderscore = true;
		}
	}
	// Trailing underscore is dropped by not flushing bPendingUnderscore.

	if (Result.IsEmpty())
	{
		return TEXT("plan");
	}
	return Result;
}

// ─────────────────────────────────────────────────────────────────────────────
// MakeFileName
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAIPlanFile::MakeFileName(const FString& Slug, const FString& Hash8)
{
	return FString::Printf(TEXT("%s_%s.plan.md"), *Slug, *Hash8);
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildMarkdown
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAIPlanFile::BuildMarkdown(
	const FString& Title,
	const FString& Description,
	const FString& Body,
	const TArray<TSharedPtr<FJsonValue>>& Todos,
	const FString& Slug,
	const FString& CreatedIso)
{
	TArray<FString> Lines;

	// ── YAML frontmatter ──
	Lines.Add(TEXT("---"));
	Lines.Add(FString::Printf(TEXT("title: %s"), *Title));
	Lines.Add(FString::Printf(TEXT("slug: %s"), *Slug));
	Lines.Add(FString::Printf(TEXT("created: %s"), *CreatedIso));
	Lines.Add(TEXT("---"));
	Lines.Add(TEXT(""));

	// ── H1 title ──
	Lines.Add(FString::Printf(TEXT("# %s"), *Title));

	// ── Optional description ──
	if (!Description.IsEmpty())
	{
		Lines.Add(TEXT(""));
		Lines.Add(Description);
	}

	// ── To-dos section (omitted when empty) ──
	if (Todos.Num() > 0)
	{
		Lines.Add(TEXT(""));
		Lines.Add(TEXT("## To-dos"));

		for (const TSharedPtr<FJsonValue>& TodoVal : Todos)
		{
			if (!TodoVal.IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>* TodoObjPtr = nullptr;
			if (!TodoVal->TryGetObject(TodoObjPtr) || !TodoObjPtr || !TodoObjPtr->IsValid())
			{
				continue;
			}

			const TSharedPtr<FJsonObject>& TodoObj = *TodoObjPtr;

			FString Content;
			if (!TodoObj->TryGetStringField(TEXT("content"), Content))
			{
				continue;
			}

			FString Status;
			TodoObj->TryGetStringField(TEXT("status"), Status);

			bool bCompleted = (Status == TEXT("completed"));
			FString Checkbox = bCompleted ? TEXT("- [x] ") : TEXT("- [ ] ");
			Lines.Add(Checkbox + Content);
		}
	}

	// ── Plan section ──
	Lines.Add(TEXT(""));
	Lines.Add(TEXT("## Plan"));
	Lines.Add(Body);

	return FString::Join(Lines, TEXT("\n"));
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildPlanReadyEvent
// ─────────────────────────────────────────────────────────────────────────────

TSharedPtr<FJsonObject> FKismetAIPlanFile::BuildPlanReadyEvent(
	const FString& Title,
	const FString& Description,
	const FString& Body,
	const TArray<TSharedPtr<FJsonValue>>& Todos,
	const FString& FilePath,
	const FString& Slug)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();

	Obj->SetStringField(TEXT("type"),        TEXT("plan_ready"));
	// Backwards-compatible: "plan" field holds the human-readable plan body text.
	Obj->SetStringField(TEXT("plan"),        Body);
	Obj->SetStringField(TEXT("title"),       Title);
	Obj->SetStringField(TEXT("description"), Description);
	Obj->SetStringField(TEXT("file_path"),   FilePath);
	Obj->SetStringField(TEXT("slug"),        Slug);
	Obj->SetArrayField(TEXT("todos"),        Todos);

	return Obj;
}
