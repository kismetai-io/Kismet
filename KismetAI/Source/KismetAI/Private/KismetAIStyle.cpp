// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Styling/CoreStyle.h"
#include "Interfaces/IPluginManager.h"

TSharedPtr<FSlateStyleSet> FKismetAIStyle::StyleInstance = nullptr;

FName FKismetAIStyle::GetStyleSetName()
{
	static FName Name(TEXT("KismetAIStyle"));
	return Name;
}

void FKismetAIStyle::Initialize()
{
	if (StyleInstance.IsValid())
	{
		return;
	}

	StyleInstance = MakeShared<FSlateStyleSet>(GetStyleSetName());

	// Resolve content root to the plugin's Resources/ directory so we can load brand PNGs.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI"));
	if (Plugin.IsValid())
	{
		StyleInstance->SetContentRoot(Plugin->GetBaseDir() / TEXT("Resources"));
	}
	StyleInstance->SetCoreContentRoot(FPaths::EngineContentDir() / TEXT("Slate"));

	// ── Toolbar icons (used by KismetAIModule for tab spawner and toolbar button) ──
	const FVector2D Icon40x40(40.0f, 40.0f);
	const FVector2D Icon20x20(20.0f, 20.0f);
	StyleInstance->Set("KismetAI.ToolbarIcon",
		new FSlateImageBrush(StyleInstance->RootToContentDir(TEXT("ToolbarIcon40"), TEXT(".png")), Icon40x40));
	StyleInstance->Set("KismetAI.ToolbarIcon.Small",
		new FSlateImageBrush(StyleInstance->RootToContentDir(TEXT("ToolbarIcon20"), TEXT(".png")), Icon20x20));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
}

void FKismetAIStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		StyleInstance.Reset();
	}
}

const ISlateStyle& FKismetAIStyle::Get()
{
	check(StyleInstance.IsValid());
	return *StyleInstance;
}
