// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIRequestBudget.h"
#include "KismetAIModelCatalog.h"
#include "Math/UnrealMathUtility.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// About four characters per token; attachments at a flat cost, because
	// providers bill an image by its pixels, not by its base64 length.
	constexpr int64 CharsPerToken = 4;
	constexpr int64 AttachmentChars = FKismetAIRequestEstimate::AttachmentTokensEach * CharsPerToken;

	int32 CharsToTokens(int64 Chars)
	{
		return static_cast<int32>(FMath::Min<int64>((Chars + CharsPerToken - 1) / CharsPerToken, MAX_int32));
	}

	int64 SerializedChars(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return 0;
		}
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Out.Len();
	}

	int64 BlockChars(const TSharedPtr<FJsonValue>& BlockValue, int32& Attachments)
	{
		const TSharedPtr<FJsonObject> Block = BlockValue.IsValid() ? BlockValue->AsObject() : nullptr;
		if (!Block.IsValid())
		{
			return SerializedChars(BlockValue);
		}
		FString Type;
		Block->TryGetStringField(TEXT("type"), Type);
		FString Text;
		if (Type == TEXT("text") && Block->TryGetStringField(TEXT("text"), Text))
		{
			return Text.Len();
		}
		if (Type == TEXT("thinking") && Block->TryGetStringField(TEXT("thinking"), Text))
		{
			return Text.Len();
		}
		if (Type == TEXT("image") || Type == TEXT("document"))
		{
			++Attachments;
			return AttachmentChars;
		}
		if (Type == TEXT("tool_result"))
		{
			if (Block->TryGetStringField(TEXT("content"), Text))
			{
				return Text.Len() + 32;
			}
			const TArray<TSharedPtr<FJsonValue>>* Nested = nullptr;
			if (Block->TryGetArrayField(TEXT("content"), Nested))
			{
				int64 Sum = 32;
				for (const TSharedPtr<FJsonValue>& Inner : *Nested)
				{
					Sum += BlockChars(Inner, Attachments);
				}
				return Sum;
			}
		}
		return SerializedChars(BlockValue);
	}

	int64 MessagesChars(const TArray<TSharedPtr<FJsonValue>>& Messages, int32& OutCount, int32& OutAttachments)
	{
		int64 Chars = 0;
		for (const TSharedPtr<FJsonValue>& Message : Messages)
		{
			const TSharedPtr<FJsonObject> Obj = Message.IsValid() ? Message->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			++OutCount;
			Chars += 16;  // role and framing
			FString Text;
			const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
			if (Obj->TryGetStringField(TEXT("content"), Text))
			{
				Chars += Text.Len();
			}
			else if (Obj->TryGetArrayField(TEXT("content"), Blocks))
			{
				for (const TSharedPtr<FJsonValue>& Block : *Blocks)
				{
					Chars += BlockChars(Block, OutAttachments);
				}
			}
		}
		return Chars;
	}
}

const TCHAR* FKismetAIContextBudget::LimitSourceName(EKismetAIContextLimitSource Source)
{
	switch (Source)
	{
	case EKismetAIContextLimitSource::Catalog:             return TEXT("catalog");
	case EKismetAIContextLimitSource::ModelSuffix:         return TEXT("model_suffix");
	case EKismetAIContextLimitSource::AssumedUnknownModel: return TEXT("assumed_unknown_model");
	}
	return TEXT("unknown");
}

FKismetAIContextBudget FKismetAIContextBudget::Resolve(const FString& Model, int32 RequestedMaxTokens, int32 OperatingContextLimit)
{
	FKismetAIContextBudget B;
	const FKismetAIModelCatalog& Catalog = FKismetAIModelCatalog::Get();
	const int32 DefaultWindow = Catalog.GetDefaults().ContextWindow > 0 ? Catalog.GetDefaults().ContextWindow : 200000;
	if (Catalog.IsKnownContextSuffix(Model))
	{
		B.ContextWindow = 1000000;
		B.CatalogWindow = 1000000;
		B.LimitSource = EKismetAIContextLimitSource::ModelSuffix;
	}
	else if (const FKismetAIModelEntry* Entry = Catalog.Find(Model); Entry && Entry->ContextWindow > 0)
	{
		B.ContextWindow = Entry->ContextWindow;
		B.CatalogWindow = Entry->ContextWindow;
		B.LimitSource = EKismetAIContextLimitSource::Catalog;
	}
	else
	{
		// No figure for this id (or a catalog entry with no usable window):
		// assume a conservative window rather than the most generous default.
		B.CatalogWindow = DefaultWindow;
		B.ContextWindow = FMath::Min(DefaultWindow, UnknownModelWindow);
		B.LimitSource = EKismetAIContextLimitSource::AssumedUnknownModel;
	}
	B.UnrestrictedContextWindow = B.ContextWindow;
	B.OperatingContextLimit = FMath::Max(0, OperatingContextLimit);
	if (B.OperatingContextLimit > 0)
	{
		B.ContextWindow = FMath::Min(B.ContextWindow, B.OperatingContextLimit);
	}
	B.RequestedMaxTokens = FMath::Max(0, RequestedMaxTokens);
	B.OutputReserve = FMath::Min(B.RequestedMaxTokens, OutputReserveCap);
	B.EffectiveLimit = FMath::Max(1, B.ContextWindow - B.OutputReserve);
	return B;
}

double FKismetAIUsageCalibration::Ratio() const
{
	if (!bValid || ReportedInputTokens <= 0 || EstimatedInputTokens <= 0)
	{
		return 1.0;
	}
	return FMath::Clamp(static_cast<double>(ReportedInputTokens) / EstimatedInputTokens, 1.0, MaxRatio);
}

const TCHAR* FKismetAIRequestEstimate::BasisName() const
{
	return bCalibrated ? TEXT("reported_usage_plus_estimated_delta") : TEXT("local_estimate");
}

TSharedPtr<FJsonObject> FKismetAIRequestEstimate::ComponentsToJson() const
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("tokens"), Tokens);
	Obj->SetNumberField(TEXT("local_tokens"), LocalTokens);
	Obj->SetNumberField(TEXT("system_tokens"), SystemTokens);
	Obj->SetNumberField(TEXT("tool_schema_tokens"), ToolSchemaTokens);
	Obj->SetNumberField(TEXT("tools"), ToolCount);
	Obj->SetNumberField(TEXT("message_tokens"), MessageTokens);
	Obj->SetNumberField(TEXT("messages"), MessageCount);
	Obj->SetNumberField(TEXT("attachments"), AttachmentCount);
	Obj->SetNumberField(TEXT("attachment_tokens_each"), AttachmentTokensEach);
	return Obj;
}

void FKismetAIRequestEstimate::WriteBudgetFields(FJsonObject& Out) const
{
	Out.SetStringField(TEXT("basis"), BasisName());
	Out.SetNumberField(TEXT("effective_limit"), Budget.EffectiveLimit);
	Out.SetNumberField(TEXT("context_window"), Budget.ContextWindow);
	Out.SetNumberField(TEXT("catalog_context_window"), Budget.CatalogWindow);
	Out.SetNumberField(TEXT("unrestricted_context_window"), Budget.UnrestrictedContextWindow);
	Out.SetNumberField(TEXT("operating_context_limit"), Budget.OperatingContextLimit);
	Out.SetStringField(TEXT("limit_source"), FKismetAIContextBudget::LimitSourceName(Budget.LimitSource));
	Out.SetNumberField(TEXT("requested_max_tokens"), Budget.RequestedMaxTokens);
	Out.SetNumberField(TEXT("output_reserve"), Budget.OutputReserve);
	TSharedPtr<FJsonObject> Cal = MakeShared<FJsonObject>();
	Cal->SetBoolField(TEXT("valid"), Calibration.bValid);
	if (Calibration.bValid)
	{
		Cal->SetNumberField(TEXT("reported_input_tokens"), Calibration.ReportedInputTokens);
		Cal->SetNumberField(TEXT("estimated_input_tokens"), Calibration.EstimatedInputTokens);
		Cal->SetNumberField(TEXT("ratio"), CalibrationRatio);
	}
	Out.SetObjectField(TEXT("calibration"), Cal);
}

TSharedPtr<FJsonObject> FKismetAIRequestEstimate::ToJson() const
{
	TSharedPtr<FJsonObject> Obj = ComponentsToJson();
	WriteBudgetFields(*Obj);
	return Obj;
}

FKismetAIRequestEstimate FKismetAIRequestEstimate::Estimate(const FString& SystemPrompt,
	const TArray<TSharedPtr<FJsonValue>>& Tools,
	const TArray<TSharedPtr<FJsonValue>>& Messages,
	const FKismetAIUsageCalibration& Calibration,
	const FKismetAIContextBudget& Budget)
{
	FKismetAIRequestEstimate E;
	E.Budget = Budget;
	E.Calibration = Calibration;

	E.SystemTokens = CharsToTokens(SystemPrompt.Len());
	int64 ToolChars = 0;
	for (const TSharedPtr<FJsonValue>& Tool : Tools)
	{
		ToolChars += SerializedChars(Tool);
	}
	E.ToolCount = Tools.Num();
	E.ToolSchemaTokens = CharsToTokens(ToolChars);
	E.MessageTokens = CharsToTokens(MessagesChars(Messages, E.MessageCount, E.AttachmentCount));
	E.LocalTokens = static_cast<int32>(FMath::Min<int64>(
		static_cast<int64>(E.SystemTokens) + E.ToolSchemaTokens + E.MessageTokens, MAX_int32));

	E.Tokens = E.LocalTokens;
	if (Calibration.bValid && Calibration.ReportedInputTokens > 0 && Calibration.EstimatedInputTokens > 0)
	{
		E.bCalibrated = true;
		E.CalibrationRatio = Calibration.Ratio();
		const double Delta = static_cast<double>(E.LocalTokens) - Calibration.EstimatedInputTokens;
		const double Anchored = Calibration.ReportedInputTokens + E.CalibrationRatio * Delta;
		E.Tokens = static_cast<int32>(FMath::Clamp<double>(FMath::Max<double>(E.LocalTokens, Anchored), 0.0, MAX_int32));
	}
	return E;
}
