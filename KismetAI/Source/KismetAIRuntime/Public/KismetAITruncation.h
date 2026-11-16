// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "CoreMinimal.h"

struct FTruncationResult
{
	FString TruncatedText;
	int32 TotalItems;     // Total nodes or chains
	int32 IncludedItems;  // Items included
	int32 OmittedItems;   // Items truncated
};

class KISMETAIRUNTIME_API FKismetAITruncation
{
public:
	// Truncate T3D text by complete "Begin Object...End Object" block boundaries.
	// Keeps as many complete node blocks as fit within MaxChars.
	// Appends "[truncated: N more nodes]" marker if nodes were omitted.
	static FTruncationResult TruncateT3DByNodeBoundary(
		const FString& T3DText,
		int32 MaxChars);

	// Truncate describe-mode output by execution chain boundaries.
	// Keeps complete "### Execution Flow" chains that fit within MaxChars.
	// Preserves the "### Nodes" summary section always.
	// Appends "[truncated: N more execution chains]" marker if chains omitted.
	static FTruncationResult TruncateDescribeByChain(
		const FString& DescribeText,
		int32 MaxChars);

	// Apply pagination to T3D text: extract nodes [Offset..Offset+Limit).
	// Returns the T3D blocks for that window.
	static FTruncationResult PaginateT3DNodes(
		const FString& T3DText,
		int32 Offset,
		int32 Limit);

	// Parse T3D text into individual node blocks (Begin Object...End Object).
	// Returns an array of individual node strings.
	static TArray<FString> SplitT3DIntoNodeBlocks(const FString& T3DText);
};
