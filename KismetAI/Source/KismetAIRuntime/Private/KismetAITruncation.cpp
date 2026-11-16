// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAITruncation.h"

TArray<FString> FKismetAITruncation::SplitT3DIntoNodeBlocks(const FString& T3DText)
{
	TArray<FString> Blocks;
	TArray<FString> Lines;
	T3DText.ParseIntoArrayLines(Lines);

	FString CurrentBlock;
	int32 Depth = 0;

	for (const FString& Line : Lines)
	{
		FString Trimmed = Line.TrimStartAndEnd();

		if (Trimmed.StartsWith(TEXT("Begin Object")))
		{
			if (Depth == 0 && !CurrentBlock.IsEmpty())
			{
				// Stray text before a node -- unlikely but handle gracefully
				CurrentBlock.Reset();
			}
			Depth++;
		}

		CurrentBlock += Line + TEXT("\n");

		if (Trimmed.StartsWith(TEXT("End Object")))
		{
			Depth--;
			if (Depth == 0)
			{
				Blocks.Add(CurrentBlock);
				CurrentBlock.Reset();
			}
		}
	}

	// Handle any trailing content
	if (!CurrentBlock.TrimStartAndEnd().IsEmpty())
	{
		Blocks.Add(CurrentBlock);
	}

	return Blocks;
}

FTruncationResult FKismetAITruncation::TruncateT3DByNodeBoundary(
	const FString& T3DText, int32 MaxChars)
{
	FTruncationResult Result;
	TArray<FString> Blocks = SplitT3DIntoNodeBlocks(T3DText);
	Result.TotalItems = Blocks.Num();

	if (T3DText.Len() <= MaxChars)
	{
		Result.TruncatedText = T3DText;
		Result.IncludedItems = Blocks.Num();
		Result.OmittedItems = 0;
		return Result;
	}

	FString Output;
	int32 IncludedCount = 0;
	// Reserve space for the truncation marker (estimate ~50 chars)
	int32 ReservedChars = 50;

	for (const FString& Block : Blocks)
	{
		if (Output.Len() + Block.Len() + ReservedChars > MaxChars && IncludedCount > 0)
		{
			break;
		}
		Output += Block;
		IncludedCount++;
	}

	int32 Omitted = Blocks.Num() - IncludedCount;
	if (Omitted > 0)
	{
		Output += FString::Printf(TEXT("\n[truncated: %d more nodes]\n"), Omitted);
	}

	Result.TruncatedText = Output;
	Result.IncludedItems = IncludedCount;
	Result.OmittedItems = Omitted;
	return Result;
}

FTruncationResult FKismetAITruncation::TruncateDescribeByChain(
	const FString& DescribeText, int32 MaxChars)
{
	FTruncationResult Result;

	if (DescribeText.Len() <= MaxChars)
	{
		Result.TruncatedText = DescribeText;
		Result.TotalItems = 1;
		Result.IncludedItems = 1;
		Result.OmittedItems = 0;
		return Result;
	}

	// Find the sections
	int32 ExecFlowIdx = DescribeText.Find(TEXT("### Execution Flow"));
	int32 NodesSectionIdx = DescribeText.Find(TEXT("### Nodes"));

	FString Header = (ExecFlowIdx != INDEX_NONE)
		? DescribeText.Left(ExecFlowIdx)
		: TEXT("");
	FString ExecSection = TEXT("");
	FString NodesSection = TEXT("");

	if (ExecFlowIdx != INDEX_NONE && NodesSectionIdx != INDEX_NONE)
	{
		ExecSection = DescribeText.Mid(ExecFlowIdx, NodesSectionIdx - ExecFlowIdx);
		NodesSection = DescribeText.Mid(NodesSectionIdx);
	}
	else if (ExecFlowIdx != INDEX_NONE)
	{
		ExecSection = DescribeText.Mid(ExecFlowIdx);
	}

	// Always keep Header + NodesSection
	// Truncate ExecSection chains if needed
	FString ExecHeader = TEXT("### Execution Flow\n\n");
	FString ExecBody = ExecSection.Mid(ExecHeader.Len());

	// Split chains by double-newline
	TArray<FString> Chains;
	ExecBody.ParseIntoArray(Chains, TEXT("\n\n"), true);
	// Filter out empty strings
	Chains.RemoveAll([](const FString& S) { return S.TrimStartAndEnd().IsEmpty(); });

	Result.TotalItems = Chains.Num();

	int32 AvailableForChains = MaxChars - Header.Len() - ExecHeader.Len() - NodesSection.Len() - 60; // margin for marker
	FString ChainOutput;
	int32 IncludedChains = 0;

	for (const FString& Chain : Chains)
	{
		if (ChainOutput.Len() + Chain.Len() + 2 > AvailableForChains && IncludedChains > 0)
		{
			break;
		}
		ChainOutput += Chain + TEXT("\n\n");
		IncludedChains++;
	}

	int32 OmittedChains = Chains.Num() - IncludedChains;
	if (OmittedChains > 0)
	{
		ChainOutput += FString::Printf(TEXT("[truncated: %d more execution chains]\n\n"), OmittedChains);
	}

	Result.TruncatedText = Header + ExecHeader + ChainOutput + NodesSection;
	Result.IncludedItems = IncludedChains;
	Result.OmittedItems = OmittedChains;
	return Result;
}

FTruncationResult FKismetAITruncation::PaginateT3DNodes(
	const FString& T3DText, int32 Offset, int32 Limit)
{
	FTruncationResult Result;
	TArray<FString> Blocks = SplitT3DIntoNodeBlocks(T3DText);
	Result.TotalItems = Blocks.Num();

	int32 StartIdx = FMath::Clamp(Offset, 0, Blocks.Num());
	int32 EndIdx = FMath::Clamp(Offset + Limit, 0, Blocks.Num());

	FString Output;
	for (int32 i = StartIdx; i < EndIdx; i++)
	{
		Output += Blocks[i];
	}

	Result.IncludedItems = EndIdx - StartIdx;
	Result.OmittedItems = Blocks.Num() - Result.IncludedItems;

	if (EndIdx < Blocks.Num())
	{
		Output += FString::Printf(
			TEXT("\n[showing nodes %d-%d of %d, use offset=%d to see more]\n"),
			StartIdx, EndIdx - 1, Blocks.Num(), EndIdx);
	}

	Result.TruncatedText = Output;
	return Result;
}
