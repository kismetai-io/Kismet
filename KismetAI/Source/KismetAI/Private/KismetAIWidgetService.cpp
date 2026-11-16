// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIWidgetService.h"
#include "KismetAIEditorMutation.h"
#include "KismetAIJsonCompat.h"
#include "KismetAIUtils.h"
#include "KismetAIPersistence.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/PanelSlot.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/GridSlot.h"
#include "Components/OverlaySlot.h"
#include "Components/BorderSlot.h"
#include "Components/TextBlock.h"
#include "Components/NamedSlot.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Kismet2/Kismet2NameValidators.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"
#include "K2Node_Variable.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ScopedTransaction.h"
#include "EdGraph/EdGraph.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Logging/TokenizedMessage.h"

using namespace KismetAIEditorMutation;

namespace WidgetReadHelpers
{
	/** Short class name for a widget (e.g. "TextBlock", "Button"). */
	static FString ShortClassName(const UObject* Obj)
	{
		return Obj && Obj->GetClass() ? Obj->GetClass()->GetName() : FString();
	}

	static TArray<TSharedPtr<FJsonValue>> Floats2(double A, double B)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(A));
		Arr.Add(MakeShared<FJsonValueNumber>(B));
		return Arr;
	}

	static TArray<TSharedPtr<FJsonValue>> Floats4(double A, double B, double C, double D)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Add(MakeShared<FJsonValueNumber>(A));
		Arr.Add(MakeShared<FJsonValueNumber>(B));
		Arr.Add(MakeShared<FJsonValueNumber>(C));
		Arr.Add(MakeShared<FJsonValueNumber>(D));
		return Arr;
	}

	static FString HAlignString(EHorizontalAlignment Align)
	{
		switch (Align)
		{
		case HAlign_Fill:   return TEXT("Fill");
		case HAlign_Left:   return TEXT("Left");
		case HAlign_Center: return TEXT("Center");
		case HAlign_Right:  return TEXT("Right");
		default:            return TEXT("Fill");
		}
	}

	static FString VAlignString(EVerticalAlignment Align)
	{
		switch (Align)
		{
		case VAlign_Fill:   return TEXT("Fill");
		case VAlign_Top:    return TEXT("Top");
		case VAlign_Center: return TEXT("Center");
		case VAlign_Bottom: return TEXT("Bottom");
		default:            return TEXT("Fill");
		}
	}

	/** Describe a widget's slot — the parent panel decides the slot type and its layout props. */
	static TSharedPtr<FJsonObject> SerializeSlot(UPanelSlot* Slot)
	{
		if (!Slot)
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("class"), ShortClassName(Slot));

		if (UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot))
		{
			const FAnchorData Layout = Canvas->GetLayout();
			S->SetArrayField(TEXT("position"), Floats2(Layout.Offsets.Left, Layout.Offsets.Top));
			S->SetArrayField(TEXT("size"), Floats2(Layout.Offsets.Right, Layout.Offsets.Bottom));
			S->SetArrayField(TEXT("anchors"), Floats4(
				Layout.Anchors.Minimum.X, Layout.Anchors.Minimum.Y,
				Layout.Anchors.Maximum.X, Layout.Anchors.Maximum.Y));
			S->SetArrayField(TEXT("alignment"), Floats2(Layout.Alignment.X, Layout.Alignment.Y));
			S->SetBoolField(TEXT("auto_size"), Canvas->GetAutoSize());
			S->SetNumberField(TEXT("z_order"), Canvas->GetZOrder());
		}
		else if (UHorizontalBoxSlot* HBox = Cast<UHorizontalBoxSlot>(Slot))
		{
			const FMargin P = HBox->GetPadding();
			S->SetArrayField(TEXT("padding"), Floats4(P.Left, P.Top, P.Right, P.Bottom));
			const FSlateChildSize Size = HBox->GetSize();
			S->SetStringField(TEXT("size_rule"), Size.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Auto"));
			S->SetNumberField(TEXT("fill"), Size.Value);
			S->SetStringField(TEXT("h_align"), HAlignString(HBox->GetHorizontalAlignment()));
			S->SetStringField(TEXT("v_align"), VAlignString(HBox->GetVerticalAlignment()));
		}
		else if (UVerticalBoxSlot* VBox = Cast<UVerticalBoxSlot>(Slot))
		{
			const FMargin P = VBox->GetPadding();
			S->SetArrayField(TEXT("padding"), Floats4(P.Left, P.Top, P.Right, P.Bottom));
			const FSlateChildSize Size = VBox->GetSize();
			S->SetStringField(TEXT("size_rule"), Size.SizeRule == ESlateSizeRule::Fill ? TEXT("Fill") : TEXT("Auto"));
			S->SetNumberField(TEXT("fill"), Size.Value);
			S->SetStringField(TEXT("h_align"), HAlignString(VBox->GetHorizontalAlignment()));
			S->SetStringField(TEXT("v_align"), VAlignString(VBox->GetVerticalAlignment()));
		}
		else if (UGridSlot* Grid = Cast<UGridSlot>(Slot))
		{
			S->SetNumberField(TEXT("row"), Grid->GetRow());
			S->SetNumberField(TEXT("column"), Grid->GetColumn());
			S->SetNumberField(TEXT("row_span"), Grid->GetRowSpan());
			S->SetNumberField(TEXT("column_span"), Grid->GetColumnSpan());
		}
		else if (UOverlaySlot* Overlay = Cast<UOverlaySlot>(Slot))
		{
			const FMargin P = Overlay->GetPadding();
			S->SetArrayField(TEXT("padding"), Floats4(P.Left, P.Top, P.Right, P.Bottom));
			S->SetStringField(TEXT("h_align"), HAlignString(Overlay->GetHorizontalAlignment()));
			S->SetStringField(TEXT("v_align"), VAlignString(Overlay->GetVerticalAlignment()));
		}
		else if (UBorderSlot* Border = Cast<UBorderSlot>(Slot))
		{
			const FMargin P = Border->GetPadding();
			S->SetArrayField(TEXT("padding"), Floats4(P.Left, P.Top, P.Right, P.Bottom));
			S->SetStringField(TEXT("h_align"), HAlignString(Border->GetHorizontalAlignment()));
			S->SetStringField(TEXT("v_align"), VAlignString(Border->GetVerticalAlignment()));
		}
		// Other slot classes: just the class name is reported.

		return S;
	}

	/** Recursively serialize a widget node: name, class, is_variable, slot, summary, children. */
	static TSharedPtr<FJsonObject> SerializeWidget(UWidget* Widget)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		if (!Widget)
		{
			return Node;
		}
		Node->SetStringField(TEXT("name"), Widget->GetName());
		Node->SetStringField(TEXT("class"), ShortClassName(Widget));
		Node->SetBoolField(TEXT("is_variable"), Widget->bIsVariable);

		if (TSharedPtr<FJsonObject> Slot = SerializeSlot(Widget->Slot))
		{
			Node->SetObjectField(TEXT("slot"), Slot);
		}

		// Cheap, widget-specific summary props (full property read is a later phase).
		if (UTextBlock* Text = Cast<UTextBlock>(Widget))
		{
			Node->SetStringField(TEXT("text"), Text->GetText().ToString());
		}

		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
			{
				if (UWidget* Child = Panel->GetChildAt(i))
				{
					Children.Add(MakeShared<FJsonValueObject>(SerializeWidget(Child)));
				}
			}
			Node->SetArrayField(TEXT("children"), Children);
		}

		return Node;
	}

	/** Collect every widget reachable from the root into a set, for the orphan check below. */
	static void CollectReachable(UWidget* Widget, TSet<UWidget*>& OutReachable)
	{
		if (!Widget || OutReachable.Contains(Widget))
		{
			return;
		}
		OutReachable.Add(Widget);
		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			for (int32 i = 0; i < Panel->GetChildrenCount(); ++i)
			{
				CollectReachable(Panel->GetChildAt(i), OutReachable);
			}
		}
	}

	/** Build the describe-tree payload (parent_class, root, named_slots, widget_count, orphaned)
	 *  for a Widget Blueprint. Shared by Describe and edit_widget's readback so the
	 *  agent verifies the same hierarchy shape after a mutation. */
	static TSharedPtr<FJsonObject> BuildWidgetTreeData(UWidgetBlueprint* WBP)
	{
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("widget"), WBP->GetPathName());
		Data->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : FString());

		UWidgetTree* Tree = WBP->WidgetTree;
		int32 WidgetCount = 0;
		TSet<UWidget*> Reachable;

		if (Tree && Tree->RootWidget)
		{
			Data->SetObjectField(TEXT("root"), SerializeWidget(Tree->RootWidget));
			CollectReachable(Tree->RootWidget, Reachable);
		}
		else
		{
			Data->SetField(TEXT("root"), MakeShared<FJsonValueNull>());
		}

		TSharedPtr<FJsonObject> NamedSlots = MakeShared<FJsonObject>();
		if (Tree)
		{
			Tree->ForEachWidget([&](UWidget* Widget)
			{
				if (!Widget) return;
				++WidgetCount;
				if (UNamedSlot* NamedSlot = Cast<UNamedSlot>(Widget))
				{
					UWidget* Content = NamedSlot->GetChildAt(0);
					NamedSlots->SetStringField(NamedSlot->GetName(), Content ? Content->GetName() : FString());
				}
			});
		}
		Data->SetObjectField(TEXT("named_slots"), NamedSlots);
		Data->SetNumberField(TEXT("widget_count"), WidgetCount);

		// Orphans: widgets present in the tree but not reachable from the root. Surfacing these
		// avoids silently hiding widgets the user added but never parented (common in hand edits).
		if (Tree)
		{
			TArray<TSharedPtr<FJsonValue>> Orphans;
			Tree->ForEachWidget([&](UWidget* Widget)
			{
				if (Widget && Widget != Tree->RootWidget && !Reachable.Contains(Widget))
				{
					Orphans.Add(MakeShared<FJsonValueObject>(SerializeWidget(Widget)));
				}
			});
			if (Orphans.Num() > 0)
			{
				Data->SetArrayField(TEXT("orphaned"), Orphans);
			}
		}

		return Data;
	}
}

FToolExecutionResult FKismetAIWidgetService::Describe(const TSharedPtr<FJsonObject>& Args)
{
	check(IsInGameThread());
	using namespace WidgetReadHelpers;

	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("widget"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'widget' field is required and must be a non-empty string"));
	}

	UWidgetBlueprint* WBP = KismetAIUtils::FindWidgetBlueprint(AssetPath);
	if (!WBP)
	{
		// Distinguish a non-widget Blueprint (wrong asset type) from a genuinely missing asset.
		if (KismetAIUtils::FindBlueprint(AssetPath))
		{
			return FToolExecutionResult::Error(404, TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
				FString::Printf(TEXT("'%s' is a Blueprint but not a Widget Blueprint (UMG)"), *AssetPath),
				{ TEXT("read_widget only works on Widget Blueprints; use read_blueprint for ordinary Blueprints") });
		}
		return FToolExecutionResult::Error(404, TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
			FString::Printf(TEXT("Widget Blueprint not found: %s"), *AssetPath),
			{ TEXT("Use search_assets with class_filter='WidgetBlueprint' to find the correct path") });
	}

	return FToolExecutionResult::Success(BuildWidgetTreeData(WBP));
}



FToolExecutionResult FKismetAIWidgetService::Overview(const TSharedPtr<FJsonObject>& Args)
{
	check(IsInGameThread());
	using namespace WidgetReadHelpers;

	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("widget"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'widget' field is required and must be a non-empty string"));
	}

	UWidgetBlueprint* WBP = KismetAIUtils::FindWidgetBlueprint(AssetPath);
	if (!WBP)
	{
		if (KismetAIUtils::FindBlueprint(AssetPath))
		{
			return FToolExecutionResult::Error(404, TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
				FString::Printf(TEXT("'%s' is a Blueprint but not a Widget Blueprint (UMG)"), *AssetPath),
				{ TEXT("read_widget only works on Widget Blueprints; use read_blueprint for ordinary Blueprints") });
		}
		return FToolExecutionResult::Error(404, TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
			FString::Printf(TEXT("Widget Blueprint not found: %s"), *AssetPath),
			{ TEXT("Use search_assets with class_filter='WidgetBlueprint' to find the correct path") });
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("widget"), WBP->GetPathName());
	Data->SetStringField(TEXT("parent_class"), WBP->ParentClass ? WBP->ParentClass->GetName() : FString());

	UWidgetTree* Tree = WBP->WidgetTree;
	Data->SetStringField(TEXT("root_class"),
		(Tree && Tree->RootWidget) ? ShortClassName(Tree->RootWidget) : FString());

	int32 WidgetCount = 0;
	TArray<TSharedPtr<FJsonValue>> Variables;
	TArray<TSharedPtr<FJsonValue>> NamedSlots;
	if (Tree)
	{
		Tree->ForEachWidget([&](UWidget* Widget)
		{
			if (!Widget) return;
			++WidgetCount;
			if (Widget->bIsVariable)
			{
				TSharedPtr<FJsonObject> Var = MakeShared<FJsonObject>();
				Var->SetStringField(TEXT("name"), Widget->GetName());
				Var->SetStringField(TEXT("class"), ShortClassName(Widget));
				Variables.Add(MakeShared<FJsonValueObject>(Var));
			}
			if (Cast<UNamedSlot>(Widget))
			{
				NamedSlots.Add(MakeShared<FJsonValueString>(Widget->GetName()));
			}
		});
	}
	Data->SetNumberField(TEXT("widget_count"), WidgetCount);
	Data->SetArrayField(TEXT("variables"), Variables);
	Data->SetArrayField(TEXT("named_slots"), NamedSlots);

	// Animations.
	TArray<TSharedPtr<FJsonValue>> Animations;
	for (const UWidgetAnimation* Anim : WBP->Animations)
	{
		if (Anim)
		{
			Animations.Add(MakeShared<FJsonValueString>(Anim->GetName()));
		}
	}
	Data->SetArrayField(TEXT("animations"), Animations);

	// Property bindings (the Designer's "Bind" entries).
	TArray<TSharedPtr<FJsonValue>> Bindings;
	for (const FDelegateEditorBinding& Binding : WBP->Bindings)
	{
		TSharedPtr<FJsonObject> B = MakeShared<FJsonObject>();
		B->SetStringField(TEXT("widget"), Binding.ObjectName);
		B->SetStringField(TEXT("property"), Binding.PropertyName.ToString());
		B->SetStringField(TEXT("function"), Binding.FunctionName.ToString());
		B->SetStringField(TEXT("kind"), Binding.Kind == EBindingKind::Function ? TEXT("Function") : TEXT("Property"));
		Bindings.Add(MakeShared<FJsonValueObject>(B));
	}
	Data->SetArrayField(TEXT("bindings"), Bindings);

	// Graphs (event + function graph names).
	TArray<TSharedPtr<FJsonValue>> Graphs;
	auto AddGraphNames = [&Graphs](const TArray<TObjectPtr<UEdGraph>>& GraphArr)
	{
		for (const UEdGraph* Graph : GraphArr)
		{
			if (Graph)
			{
				Graphs.Add(MakeShared<FJsonValueString>(Graph->GetName()));
			}
		}
	};
	AddGraphNames(WBP->UbergraphPages);
	AddGraphNames(WBP->FunctionGraphs);
	Data->SetArrayField(TEXT("graphs"), Graphs);

	return FToolExecutionResult::Success(Data);
}



// ─────────────────────────────────────────────────────────────────────────────
// Widget Designer mutations
// Request: {"widget": "/Game/UI/WBP_Foo", "operations": [...]}
// Batched Designer-tree mutations (add/remove/reparent/rename/replace widgets,
// set slot + widget properties), then recompile and return the resulting tree.
// ─────────────────────────────────────────────────────────────────────────────

namespace WidgetEditHelpers
{
	using namespace WidgetReadHelpers;

	static EHorizontalAlignment ParseHAlign(const FString& In, bool& bOk)
	{
		bOk = true;
		const FString S = In.ToLower();
		if (S == TEXT("fill")) return HAlign_Fill;
		if (S == TEXT("left")) return HAlign_Left;
		if (S == TEXT("center")) return HAlign_Center;
		if (S == TEXT("right")) return HAlign_Right;
		bOk = false;
		return HAlign_Fill;
	}

	static EVerticalAlignment ParseVAlign(const FString& In, bool& bOk)
	{
		bOk = true;
		const FString S = In.ToLower();
		if (S == TEXT("fill")) return VAlign_Fill;
		if (S == TEXT("top")) return VAlign_Top;
		if (S == TEXT("center")) return VAlign_Center;
		if (S == TEXT("bottom")) return VAlign_Bottom;
		bOk = false;
		return VAlign_Fill;
	}

	/** Read a 2-element [x,y] number array from a json value; false if malformed. */
	static bool AsVector2(const TSharedPtr<FJsonValue>& V, FVector2D& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!V.IsValid() || !V->TryGetArray(Arr) || Arr->Num() < 2) return false;
		Out.X = (*Arr)[0]->AsNumber();
		Out.Y = (*Arr)[1]->AsNumber();
		return true;
	}

	/** Read a 4-element number array; false if malformed. */
	static bool AsVector4(const TSharedPtr<FJsonValue>& V, double& A, double& B, double& C, double& D)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!V.IsValid() || !V->TryGetArray(Arr) || Arr->Num() < 4) return false;
		A = (*Arr)[0]->AsNumber();
		B = (*Arr)[1]->AsNumber();
		C = (*Arr)[2]->AsNumber();
		D = (*Arr)[3]->AsNumber();
		return true;
	}

	/** Apply one slot property by the same names read_widget emits. Returns false + Err + valid
	 *  property list when the property name is unknown for this slot type. */
	static bool ApplySlotProperty(UPanelSlot* Slot, const FString& Key, const TSharedPtr<FJsonValue>& Value, FString& Err, FString& OutValidProps)
	{
		if (UCanvasPanelSlot* Canvas = Cast<UCanvasPanelSlot>(Slot))
		{
			OutValidProps = TEXT("position, size, anchors, alignment, auto_size, z_order");
			if (Key == TEXT("position")) { FVector2D P; if (!AsVector2(Value, P)) { Err = TEXT("'position' must be [x,y]"); return false; } Canvas->SetPosition(P); return true; }
			if (Key == TEXT("size")) { FVector2D P; if (!AsVector2(Value, P)) { Err = TEXT("'size' must be [w,h]"); return false; } Canvas->SetSize(P); return true; }
			if (Key == TEXT("anchors")) { double A,B,C,D; if (!AsVector4(Value, A,B,C,D)) { Err = TEXT("'anchors' must be [minX,minY,maxX,maxY]"); return false; } Canvas->SetAnchors(FAnchors(A,B,C,D)); return true; }
			if (Key == TEXT("alignment")) { FVector2D P; if (!AsVector2(Value, P)) { Err = TEXT("'alignment' must be [x,y]"); return false; } Canvas->SetAlignment(P); return true; }
			if (Key == TEXT("auto_size")) { Canvas->SetAutoSize(Value->AsBool()); return true; }
			if (Key == TEXT("z_order")) { Canvas->SetZOrder((int32)Value->AsNumber()); return true; }
		}
		else if (UHorizontalBoxSlot* HBox = Cast<UHorizontalBoxSlot>(Slot))
		{
			OutValidProps = TEXT("padding, size_rule, fill, h_align, v_align");
			if (Key == TEXT("padding")) { double A,B,C,D; if (!AsVector4(Value, A,B,C,D)) { Err = TEXT("'padding' must be [l,t,r,b]"); return false; } HBox->SetPadding(FMargin(A,B,C,D)); return true; }
			if (Key == TEXT("size_rule")) { FSlateChildSize Sz = HBox->GetSize(); Sz.SizeRule = Value->AsString().ToLower() == TEXT("fill") ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic; HBox->SetSize(Sz); return true; }
			if (Key == TEXT("fill")) { FSlateChildSize Sz = HBox->GetSize(); Sz.Value = Value->AsNumber(); HBox->SetSize(Sz); return true; }
			if (Key == TEXT("h_align")) { bool bOk; EHorizontalAlignment A = ParseHAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'h_align' must be Left|Center|Right|Fill"); return false; } HBox->SetHorizontalAlignment(A); return true; }
			if (Key == TEXT("v_align")) { bool bOk; EVerticalAlignment A = ParseVAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'v_align' must be Top|Center|Bottom|Fill"); return false; } HBox->SetVerticalAlignment(A); return true; }
		}
		else if (UVerticalBoxSlot* VBox = Cast<UVerticalBoxSlot>(Slot))
		{
			OutValidProps = TEXT("padding, size_rule, fill, h_align, v_align");
			if (Key == TEXT("padding")) { double A,B,C,D; if (!AsVector4(Value, A,B,C,D)) { Err = TEXT("'padding' must be [l,t,r,b]"); return false; } VBox->SetPadding(FMargin(A,B,C,D)); return true; }
			if (Key == TEXT("size_rule")) { FSlateChildSize Sz = VBox->GetSize(); Sz.SizeRule = Value->AsString().ToLower() == TEXT("fill") ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic; VBox->SetSize(Sz); return true; }
			if (Key == TEXT("fill")) { FSlateChildSize Sz = VBox->GetSize(); Sz.Value = Value->AsNumber(); VBox->SetSize(Sz); return true; }
			if (Key == TEXT("h_align")) { bool bOk; EHorizontalAlignment A = ParseHAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'h_align' must be Left|Center|Right|Fill"); return false; } VBox->SetHorizontalAlignment(A); return true; }
			if (Key == TEXT("v_align")) { bool bOk; EVerticalAlignment A = ParseVAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'v_align' must be Top|Center|Bottom|Fill"); return false; } VBox->SetVerticalAlignment(A); return true; }
		}
		else if (UGridSlot* Grid = Cast<UGridSlot>(Slot))
		{
			OutValidProps = TEXT("row, column, row_span, column_span");
			if (Key == TEXT("row")) { Grid->SetRow((int32)Value->AsNumber()); return true; }
			if (Key == TEXT("column")) { Grid->SetColumn((int32)Value->AsNumber()); return true; }
			if (Key == TEXT("row_span")) { Grid->SetRowSpan((int32)Value->AsNumber()); return true; }
			if (Key == TEXT("column_span")) { Grid->SetColumnSpan((int32)Value->AsNumber()); return true; }
		}
		else if (UOverlaySlot* Overlay = Cast<UOverlaySlot>(Slot))
		{
			OutValidProps = TEXT("padding, h_align, v_align");
			if (Key == TEXT("padding")) { double A,B,C,D; if (!AsVector4(Value, A,B,C,D)) { Err = TEXT("'padding' must be [l,t,r,b]"); return false; } Overlay->SetPadding(FMargin(A,B,C,D)); return true; }
			if (Key == TEXT("h_align")) { bool bOk; EHorizontalAlignment A = ParseHAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'h_align' must be Left|Center|Right|Fill"); return false; } Overlay->SetHorizontalAlignment(A); return true; }
			if (Key == TEXT("v_align")) { bool bOk; EVerticalAlignment A = ParseVAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'v_align' must be Top|Center|Bottom|Fill"); return false; } Overlay->SetVerticalAlignment(A); return true; }
		}
		else if (UBorderSlot* Border = Cast<UBorderSlot>(Slot))
		{
			OutValidProps = TEXT("padding, h_align, v_align");
			if (Key == TEXT("padding")) { double A,B,C,D; if (!AsVector4(Value, A,B,C,D)) { Err = TEXT("'padding' must be [l,t,r,b]"); return false; } Border->SetPadding(FMargin(A,B,C,D)); return true; }
			if (Key == TEXT("h_align")) { bool bOk; EHorizontalAlignment A = ParseHAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'h_align' must be Left|Center|Right|Fill"); return false; } Border->SetHorizontalAlignment(A); return true; }
			if (Key == TEXT("v_align")) { bool bOk; EVerticalAlignment A = ParseVAlign(Value->AsString(), bOk); if (!bOk) { Err = TEXT("'v_align' must be Top|Center|Bottom|Fill"); return false; } Border->SetVerticalAlignment(A); return true; }
		}
		else
		{
			OutValidProps = TEXT("(this slot type exposes no editable layout properties)");
			Err = FString::Printf(TEXT("slot type '%s' has no settable layout properties"), Slot ? *Slot->GetClass()->GetName() : TEXT("None"));
			return false;
		}

		Err = FString::Printf(TEXT("unknown slot property '%s'"), *Key);
		return false;
	}

	/** Names of variable-get/set graph nodes that reference WidgetName (so remove/rename can warn). */
	static TArray<FString> FindGraphReferences(UWidgetBlueprint* WBP, const FName WidgetName)
	{
		TArray<FString> Refs;
		TArray<UEdGraph*> AllGraphs;
		WBP->GetAllGraphs(AllGraphs);
		for (const UEdGraph* Graph : AllGraphs)
		{
			if (!Graph) continue;
			TArray<UK2Node_Variable*> VarNodes;
			Graph->GetNodesOfClass(VarNodes);
			for (UK2Node_Variable* Node : VarNodes)
			{
				if (Node && Node->VariableReference.GetMemberName() == WidgetName)
				{
					Refs.AddUnique(Graph->GetName() + TEXT(":") + Node->GetName());
				}
			}
		}
		return Refs;
	}
}

FToolExecutionResult FKismetAIWidgetService::Edit(const TSharedPtr<FJsonObject>& Args)
{
	check(IsInGameThread());
	using namespace WidgetReadHelpers;
	using namespace WidgetEditHelpers;

	TSharedPtr<FJsonObject> Body = Args;
	if (!Body)
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Request body must be valid JSON"));
	}

	FString AssetPath;
	if (!Body->TryGetStringField(TEXT("widget"), AssetPath) || AssetPath.IsEmpty())
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'widget' field is required and must be a non-empty string"));
	}

	const TArray<TSharedPtr<FJsonValue>>* Operations = nullptr;
	if (!Body->TryGetArrayField(TEXT("operations"), Operations) || Operations->Num() == 0)
	{
		return FToolExecutionResult::Error(400, TEXT("MISSING_FIELD"), TEXT("'operations' must be a non-empty array"));
	}

	UWidgetBlueprint* WBP = KismetAIUtils::FindWidgetBlueprint(AssetPath);
	if (!WBP || !WBP->WidgetTree)
	{
		if (KismetAIUtils::FindBlueprint(AssetPath))
		{
			return FToolExecutionResult::Error(404, TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
				FString::Printf(TEXT("'%s' is a Blueprint but not a Widget Blueprint (UMG)"), *AssetPath),
				{ TEXT("edit_widget only works on Widget Blueprints; use edit_blueprint/edit_graph for ordinary Blueprints") });
		}
		return FToolExecutionResult::Error(404, TEXT("WIDGET_BLUEPRINT_NOT_FOUND"),
			FString::Printf(TEXT("Widget Blueprint not found: %s"), *AssetPath),
			{ TEXT("Use search_assets with class_filter='WidgetBlueprint' to find the correct path") });
	}

	UWidgetTree* Tree = WBP->WidgetTree;

	// Report whether the asset is open in an editor; the structural recompile below refreshes
	// open Blueprint editors, so we just surface the flag (no live-preview duplicate like materials).
	bool bEditorOpen = false;
	if (GEditor)
	{
		if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			bEditorOpen = Subsystem->FindEditorForAsset(WBP, /*bFocusIfOpen=*/false) != nullptr;
		}
	}

	const bool bPackageWasDirtyBefore = KismetAIPersistence::IsPackageDirty(WBP);
	FScopedTransaction Transaction(NSLOCTEXT("KismetAI", "EditWidget", "Edit Widget"));
	WBP->Modify();
	Tree->Modify();

	TArray<TSharedPtr<FJsonValue>> Results;
	int32 SucceededCount = 0;
	int32 FailedCount = 0;
	bool bStructuralChange = false;
	bool bAppliedProperties = false;

	for (int32 OpIndex = 0; OpIndex < Operations->Num(); ++OpIndex)
	{
		const TSharedPtr<FJsonObject> Op = (*Operations)[OpIndex]->AsObject();
		TSharedPtr<FJsonObject> Res = MakeShared<FJsonObject>();
		Res->SetNumberField(TEXT("index"), OpIndex);

		auto Succeed = [&]()
		{
			Res->SetBoolField(TEXT("success"), true);
			++SucceededCount;
		};
		auto Fail = [&](const FString& Code, const FString& Msg)
		{
			Res->SetBoolField(TEXT("success"), false);
			Res->SetStringField(TEXT("error_code"), Code);
			Res->SetStringField(TEXT("message"), Msg);
			++FailedCount;
		};

		if (!Op.IsValid()) { Fail(TEXT("BAD_OPERATION"), TEXT("operation is not an object")); Results.Add(MakeShared<FJsonValueObject>(Res)); continue; }

		const FString Action = Op->GetStringField(TEXT("action"));
		Res->SetStringField(TEXT("action"), Action);

		if (Action == TEXT("add_widget"))
		{
			const FString ClassName = Op->GetStringField(TEXT("class"));
			TArray<FString> Candidates;
			UClass* WidgetClass = KismetAIUtils::ResolveWidgetClass(ClassName, &Candidates);
			if (!WidgetClass)
			{
				FString Msg = FString::Printf(TEXT("unknown widget class '%s'"), *ClassName);
				if (Candidates.Num() > 0) Msg += FString::Printf(TEXT(" (did you mean: %s)"), *FString::Join(Candidates, TEXT(", ")));
				Fail(TEXT("UNKNOWN_WIDGET_CLASS"), Msg);
			}
			else
			{
				FName DesiredName = NAME_None;
				if (Op->HasField(TEXT("name")) && !Op->GetStringField(TEXT("name")).IsEmpty())
				{
					DesiredName = FName(*Op->GetStringField(TEXT("name")));
				}
				UWidget* NewWidget = DesiredName.IsNone()
					? Tree->ConstructWidget<UWidget>(WidgetClass)
					: Tree->ConstructWidget<UWidget>(WidgetClass, DesiredName);
				if (!NewWidget)
				{
					Fail(TEXT("CONSTRUCT_FAILED"), FString::Printf(TEXT("could not construct '%s'"), *ClassName));
				}
				else
				{
					if (Op->HasField(TEXT("is_variable"))) NewWidget->bIsVariable = Op->GetBoolField(TEXT("is_variable"));

					const bool bHasParent = Op->HasField(TEXT("parent")) && !Op->GetStringField(TEXT("parent")).IsEmpty();
					if (!Tree->RootWidget && !bHasParent)
					{
						// First widget with no parent given becomes the root, like the Designer.
						Tree->RootWidget = NewWidget;
						// Register in the widget→guid map so later rename/replace/delete stay
						// consistent (the engine's add paths do this; ConstructWidget alone does not).
						WBP->OnVariableAdded(NewWidget->GetFName());
						bStructuralChange = true;
						Res->SetStringField(TEXT("name"), NewWidget->GetName());
						Res->SetStringField(TEXT("set_as_root"), TEXT("true"));
						Succeed();
					}
					else
					{
						const FString ParentName = Op->GetStringField(TEXT("parent"));
						UWidget* ParentWidget = Tree->FindWidget(FName(*ParentName));
						UPanelWidget* Parent = Cast<UPanelWidget>(ParentWidget);
						if (!ParentWidget) { Fail(TEXT("PARENT_NOT_FOUND"), FString::Printf(TEXT("parent '%s' not found"), *ParentName)); }
						else if (!Parent) { Fail(TEXT("PARENT_NOT_PANEL"), FString::Printf(TEXT("parent '%s' (%s) is not a panel and cannot hold children"), *ParentName, *ShortClassName(ParentWidget))); }
						else if (!Parent->CanAddMoreChildren()) { Fail(TEXT("PARENT_FULL"), FString::Printf(TEXT("parent '%s' (%s) cannot accept more children"), *ParentName, *ShortClassName(ParentWidget))); }
						else
						{
							Parent->Modify();
							UPanelSlot* Slot = nullptr;
							if (Op->HasField(TEXT("index")))
							{
								Slot = Parent->InsertChildAt((int32)Op->GetNumberField(TEXT("index")), NewWidget);
							}
							else
							{
								Slot = Parent->AddChild(NewWidget);
							}
							// Register in the widget→guid map (see root branch above).
							WBP->OnVariableAdded(NewWidget->GetFName());
							bStructuralChange = true;
							Res->SetStringField(TEXT("name"), NewWidget->GetName());
							if (Slot) Res->SetStringField(TEXT("slot_class"), ShortClassName(Slot));
							Succeed();
						}
					}
				}
			}
		}
		else if (Action == TEXT("remove_widget"))
		{
			const FString Name = Op->GetStringField(TEXT("name"));
			UWidget* Widget = Tree->FindWidget(FName(*Name));
			if (!Widget) { Fail(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("widget '%s' not found"), *Name)); }
			else
			{
				// Surface graph references BEFORE deleting so the agent knows getter/setter nodes
				// will be orphaned (DeleteSilently does not warn).
				TArray<FString> GraphRefs = FindGraphReferences(WBP, Widget->GetFName());
				if (GraphRefs.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> RefArr;
					for (const FString& R : GraphRefs) RefArr.Add(MakeShared<FJsonValueString>(R));
					Res->SetArrayField(TEXT("graph_references"), RefArr);
				}
				FWidgetBlueprintEditorUtils::DeleteWidgets(WBP, { Widget }, FWidgetBlueprintEditorUtils::EDeleteWidgetWarningType::DeleteSilently);
				bStructuralChange = true;
				Succeed();
			}
		}
		else if (Action == TEXT("reparent_widget"))
		{
			const FString Name = Op->GetStringField(TEXT("name"));
			const FString NewParentName = Op->GetStringField(TEXT("new_parent"));
			UWidget* Widget = Tree->FindWidget(FName(*Name));
			UWidget* NewParentWidget = Tree->FindWidget(FName(*NewParentName));
			UPanelWidget* NewParent = Cast<UPanelWidget>(NewParentWidget);
			if (!Widget) { Fail(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("widget '%s' not found"), *Name)); }
			else if (!NewParentWidget) { Fail(TEXT("PARENT_NOT_FOUND"), FString::Printf(TEXT("new_parent '%s' not found"), *NewParentName)); }
			else if (!NewParent) { Fail(TEXT("PARENT_NOT_PANEL"), FString::Printf(TEXT("new_parent '%s' (%s) is not a panel"), *NewParentName, *ShortClassName(NewParentWidget))); }
			else if (Widget == NewParentWidget) { Fail(TEXT("INVALID_REPARENT"), TEXT("a widget cannot be parented to itself")); }
			else if (!NewParent->CanAddMoreChildren()) { Fail(TEXT("PARENT_FULL"), FString::Printf(TEXT("new_parent '%s' cannot accept more children"), *NewParentName)); }
			else
			{
				Widget->Modify();
				if (UPanelWidget* OldParent = Widget->GetParent())
				{
					OldParent->Modify();
					OldParent->RemoveChild(Widget);
				}
				NewParent->Modify();
				UPanelSlot* Slot = Op->HasField(TEXT("index"))
					? NewParent->InsertChildAt((int32)Op->GetNumberField(TEXT("index")), Widget)
					: NewParent->AddChild(Widget);
				bStructuralChange = true;
				if (Slot) Res->SetStringField(TEXT("slot_class"), ShortClassName(Slot));
				Succeed();
			}
		}
		else if (Action == TEXT("set_slot_property"))
		{
			const FString Name = Op->GetStringField(TEXT("name"));
			UWidget* Widget = Tree->FindWidget(FName(*Name));
			const TSharedPtr<FJsonObject>* Props = nullptr;
			if (!Widget) { Fail(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("widget '%s' not found"), *Name)); }
			else if (!Widget->Slot) { Fail(TEXT("NO_SLOT"), FString::Printf(TEXT("widget '%s' has no slot (not parented)"), *Name)); }
			else if (!Op->TryGetObjectField(TEXT("properties"), Props) || !Props) { Fail(TEXT("MISSING_FIELD"), TEXT("'properties' object is required")); }
			else
			{
				Widget->Slot->Modify();
				TArray<FString> Applied, Failures;
				FString ValidProps;
				for (const auto& Pair : (*Props)->Values)
				{
					const FString Key = KismetJsonKey(Pair.Key);
					FString Err;
					if (ApplySlotProperty(Widget->Slot, Key, Pair.Value, Err, ValidProps))
					{
						Applied.Add(Key);
					}
					else
					{
						Failures.Add(FString::Printf(TEXT("%s (%s)"), *Key, *Err));
					}
				}
				bAppliedProperties |= Applied.Num() > 0;
				if (Failures.Num() > 0)
				{
					Fail(TEXT("INVALID_SLOT_PROPERTY"),
						FString::Printf(TEXT("failed: %s. Valid properties for %s: %s"),
							*FString::Join(Failures, TEXT("; ")), *ShortClassName(Widget->Slot), *ValidProps));
				}
				else
				{
					Succeed();
				}
			}
		}
		else if (Action == TEXT("set_widget_property"))
		{
			const FString Name = Op->GetStringField(TEXT("name"));
			UWidget* Widget = Tree->FindWidget(FName(*Name));
			const TSharedPtr<FJsonObject>* Props = nullptr;
			if (!Widget) { Fail(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("widget '%s' not found"), *Name)); }
			else if (!Op->TryGetObjectField(TEXT("properties"), Props) || !Props) { Fail(TEXT("MISSING_FIELD"), TEXT("'properties' object is required")); }
			else
			{
				Widget->Modify();
				// Pre-check which property names actually exist; ImportPropertiesFromText silently
				// ignores unknown ones, so report them instead of failing quietly.
				TArray<FString> Unknown;
				TMap<FName, FString> ToImport;
				for (const auto& Pair : (*Props)->Values)
				{
					const FString Key = KismetJsonKey(Pair.Key);
					const FName PropName(*Key);
					if (!Widget->GetClass()->FindPropertyByName(PropName))
					{
						Unknown.Add(Key);
						continue;
					}
					FString StrVal;
					if (!Pair.Value->TryGetString(StrVal))
					{
						// Coerce non-string json (numbers/bools) to their text form for import.
						StrVal = Pair.Value->AsString();
					}
					ToImport.Add(PropName, StrVal);
				}
				if (ToImport.Num() > 0)
				{
					FWidgetBlueprintEditorUtils::ImportPropertiesFromText(Widget, ToImport);
					Widget->SynchronizeProperties();
					bAppliedProperties = true;
				}
				if (Unknown.Num() > 0)
				{
					Fail(TEXT("UNKNOWN_PROPERTY"),
						FString::Printf(TEXT("unknown propert%s on %s: %s (applied %d valid)"),
							Unknown.Num() == 1 ? TEXT("y") : TEXT("ies"),
							*ShortClassName(Widget), *FString::Join(Unknown, TEXT(", ")), ToImport.Num()));
				}
				else
				{
					Succeed();
				}
			}
		}
		else if (Action == TEXT("rename_widget"))
		{
			const FString Name = Op->GetStringField(TEXT("name"));
			const FString NewName = Op->GetStringField(TEXT("new_name"));
			UWidget* Widget = Tree->FindWidget(FName(*Name));
			if (!Widget) { Fail(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("widget '%s' not found"), *Name)); }
			else
			{
				const FName OldFName = Widget->GetFName();
				const FName NewFName(*NewName);
				// Validate against existing member names (mirrors the engine's RenameWidget gate).
				TSharedPtr<INameValidatorInterface> Validator = MakeShareable(new FKismetNameValidator(WBP, OldFName));
				const EValidatorResult VR = Validator->IsValid(NewFName);
				if (NewName.IsEmpty()) { Fail(TEXT("INVALID_NAME"), TEXT("'new_name' must be non-empty")); }
				else if (VR != EValidatorResult::Ok) { Fail(TEXT("INVALID_NAME"), FString::Printf(TEXT("name '%s' is not valid/unique"), *NewName)); }
				else
				{
					Widget->Modify();
					// Asset-only port of FWidgetBlueprintEditorUtils::RenameWidget (no open-editor preview).
					// Only touch the variable→guid map when the old name is actually registered;
					// OnVariableRenamed ensures the entry exists, so guard against widgets that were
					// never registered (avoids a spurious engine ensure).
					if (WBP->WidgetVariableNameToGuidMap.Contains(OldFName))
					{
						WBP->OnVariableRenamed(OldFName, NewFName);
					}
					Widget->SetDisplayLabel(NewName);
					Widget->Rename(*NewName);

					// Update explicit property bindings.
					for (FDelegateEditorBinding& Binding : WBP->Bindings)
					{
						if (Binding.ObjectName == OldFName.ToString()) Binding.ObjectName = NewFName.ToString();
					}
					// Update animation bindings.
					for (UWidgetAnimation* Anim : WBP->Animations)
					{
						if (!Anim) continue;
						for (FWidgetAnimationBinding& AnimBinding : Anim->AnimationBindings)
						{
							if (AnimBinding.WidgetName == OldFName) AnimBinding.WidgetName = NewFName;
						}
					}
					// Follow graph variable references (getter/setter nodes) to the new name.
					FBlueprintEditorUtils::ReplaceVariableReferences(WBP, OldFName, NewFName);
					bStructuralChange = true;
					Res->SetStringField(TEXT("new_name"), NewFName.ToString());
					Succeed();
				}
			}
		}
		else if (Action == TEXT("replace_widget"))
		{
			const FString Name = Op->GetStringField(TEXT("name"));
			const FString ClassName = Op->GetStringField(TEXT("class"));
			UWidget* Widget = Tree->FindWidget(FName(*Name));
			TArray<FString> Candidates;
			UClass* NewClass = KismetAIUtils::ResolveWidgetClass(ClassName, &Candidates);
			if (!Widget) { Fail(TEXT("WIDGET_NOT_FOUND"), FString::Printf(TEXT("widget '%s' not found"), *Name)); }
			else if (!NewClass)
			{
				FString Msg = FString::Printf(TEXT("unknown widget class '%s'"), *ClassName);
				if (Candidates.Num() > 0) Msg += FString::Printf(TEXT(" (did you mean: %s)"), *FString::Join(Candidates, TEXT(", ")));
				Fail(TEXT("UNKNOWN_WIDGET_CLASS"), Msg);
			}
			else
			{
				// MaintainNameAndReferences keeps the old name (and graph refs) when classes are compatible.
				FWidgetBlueprintEditorUtils::ReplaceWidgets(WBP, { Widget }, NewClass,
					FWidgetBlueprintEditorUtils::EReplaceWidgetNamingMethod::MaintainNameAndReferences);
				bStructuralChange = true;
				Succeed();
			}
		}
		else
		{
			Fail(TEXT("UNKNOWN_ACTION"), FString::Printf(TEXT("unknown action '%s'"), *Action));
		}

		Results.Add(MakeShared<FJsonValueObject>(Res));
	}

	// Mark modified: structural changes (tree shape, variables) need the heavier rebuild;
	// property-only batches just dirty the asset.
	if (bStructuralChange)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(WBP);
	}

	// Compile to apply + verify, then collect errors/warnings.
	FCompilerResultsLog CompileResults;
	FKismetEditorUtilities::CompileBlueprint(WBP, EBlueprintCompileOptions::None, &CompileResults);
	const bool bCompileSuccess = (WBP->Status != BS_Error);
	FKismetAIMutationOutcome Outcome = BeginMutationOutcome(bPackageWasDirtyBefore);
	// A property operation can apply valid fields and still fail on unknown ones.
	Outcome.bAppliedInMemory = SucceededCount > 0 || bAppliedProperties;
	RecordCompileOutcome(Outcome, WBP, CompileResults);
	AutoSaveAfterMutation(WBP, Outcome);

	TArray<TSharedPtr<FJsonValue>> CompileMessages;
	for (const TSharedRef<FTokenizedMessage>& Message : CompileResults.Messages)
	{
		const EMessageSeverity::Type Severity = Message->GetSeverity();
		if (Severity != EMessageSeverity::Error && Severity != EMessageSeverity::Warning) continue;
		TSharedPtr<FJsonObject> MsgObj = MakeShared<FJsonObject>();
		MsgObj->SetStringField(TEXT("severity"), Severity == EMessageSeverity::Error ? TEXT("error") : TEXT("warning"));
		MsgObj->SetStringField(TEXT("message"), Message->ToText().ToString());
		CompileMessages.Add(MakeShared<FJsonValueObject>(MsgObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	AttachMutationOutcome(Data, Outcome);
	Data->SetStringField(TEXT("widget"), WBP->GetPathName());
	Data->SetNumberField(TEXT("operations_succeeded"), SucceededCount);
	Data->SetNumberField(TEXT("operations_failed"), FailedCount);
	Data->SetArrayField(TEXT("results"), Results);
	Data->SetBoolField(TEXT("compiled"), bCompileSuccess);
	Data->SetArrayField(TEXT("compile_messages"), CompileMessages);
	Data->SetBoolField(TEXT("editor_open"), bEditorOpen);
	// Readback: the resulting tree so the agent verifies the mutation without a second read_widget.
	Data->SetObjectField(TEXT("tree"), BuildWidgetTreeData(WBP));

	return FToolExecutionResult::Success(Data, FailedCount == 0);
}
