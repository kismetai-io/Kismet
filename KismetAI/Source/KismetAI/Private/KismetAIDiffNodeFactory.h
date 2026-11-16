// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "EdGraphUtilities.h"
#include "KismetNodes/SGraphNodeK2Default.h"
#include "K2Node.h"
#include "K2Node_Event.h"
#include "K2Node_Tunnel.h"
#include "K2Node_Composite.h"
#include "Styling/AppStyle.h"

// ─────────────────────────────────────────────────────────────────────────────
// Custom SGraphNode: original node colors + thick purple rounded border
// ─────────────────────────────────────────────────────────────────────────────

class SGraphNodeK2Diff : public SGraphNodeK2Default
{
public:
	SLATE_BEGIN_ARGS(SGraphNodeK2Diff) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UK2Node* InNode, const FLinearColor& InTint)
	{
		BorderColor = InTint;
		SGraphNodeK2Default::Construct(SGraphNodeK2Default::FArguments(), InNode);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements,
		int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override
	{
		const FVector2D NodeSize = AllottedGeometry.GetLocalSize();
		if (NodeSize.X > 1.0 && NodeSize.Y > 1.0)
		{
			const FSlateBrush* HighlightBrush = FAppStyle::GetBrush(TEXT("Graph.Node.DiffHighlight"));
			// #5A1CE7 — inflate proportional to node size
			const double Pad = FMath::Clamp(FMath::Min(NodeSize.X, NodeSize.Y) * 0.03 + 4.0, 6.0, 14.0);

			FGeometry BorderGeo = AllottedGeometry.MakeChild(
				NodeSize + FVector2D(Pad * 2.0, Pad * 2.0),
				FSlateLayoutTransform(FVector2D(-Pad, -Pad)));

			FSlateDrawElement::MakeBox(
				OutDrawElements,
				LayerId,
				BorderGeo.ToPaintGeometry(),
				HighlightBrush,
				ESlateDrawEffect::None,
				BorderColor);
		}

		// Paint the normal node on top (original colors, no tint)
		return SGraphNodeK2Default::OnPaint(Args, AllottedGeometry, MyCullingRect,
			OutDrawElements, LayerId + 1, InWidgetStyle, bParentEnabled);
	}

private:
	FLinearColor BorderColor = FLinearColor(0.353f, 0.110f, 0.906f);  // #5A1CE7
	float BorderThickness = 3.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// Node factory: wraps K2Nodes with diff border when in the marked set
// ─────────────────────────────────────────────────────────────────────────────

class FKismetAIDiffNodeFactory : public FGraphPanelNodeFactory
{
public:
	TSet<FGuid> AddedNodeGuids;     // Purple border — new nodes
	TSet<FGuid> ModifiedNodeGuids;  // Lighter purple border — modified existing nodes

	void Clear()
	{
		AddedNodeGuids.Empty();
		ModifiedNodeGuids.Empty();
	}

	virtual TSharedPtr<SGraphNode> CreateNode(UEdGraphNode* InNode) const override
	{
		if (!InNode || (AddedNodeGuids.Num() == 0 && ModifiedNodeGuids.Num() == 0)) return nullptr;

		UK2Node* K2Node = Cast<UK2Node>(InNode);
		if (!K2Node) return nullptr;

		// Skip node types that need specialized SGraphNode subclasses.
		// These get a comment-bubble fallback instead (set by ComputeDiffAndMarkNodes).
		if (K2Node->IsA<UK2Node_Event>() ||
			K2Node->IsA<UK2Node_Tunnel>() ||
			K2Node->IsA<UK2Node_Composite>())
		{
			return nullptr;
		}

		if (AddedNodeGuids.Contains(InNode->NodeGuid))
		{
			// #5A1CE7 — primary purple, new nodes
			return SNew(SGraphNodeK2Diff, K2Node, FLinearColor(0.353f, 0.110f, 0.906f));
		}

		if (ModifiedNodeGuids.Contains(InNode->NodeGuid))
		{
			// #B971FC — soft purple, modified nodes
			return SNew(SGraphNodeK2Diff, K2Node, FLinearColor(0.725f, 0.443f, 0.988f));
		}

		return nullptr;
	}
};
