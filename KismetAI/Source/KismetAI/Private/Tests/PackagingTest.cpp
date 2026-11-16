// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIStyle.h"
#include "KismetAIEmbeddedConfig.h"
#include "Interfaces/IPluginManager.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/SlateBrush.h"

constexpr auto KismetAIPackagingTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// Why: package.ps1 strips Resources/ from release zips, but
// FKismetAIStyle::Initialize() loads ToolbarIcon{40,20}.png lazily from
// Plugin->GetBaseDir()/Resources via FSlateImageBrush. Stripped resources
// produce a missing tab/toolbar icon at runtime with no startup error —
// visually obvious in the editor, invisible to a unit test against the dev
// tree (where Resources/ is on disk by definition). This test runs against
// whichever copy of the plugin the editor actually loaded, so when invoked
// via scripts/test-release.sh (zip → install-local.sh → headless editor) it
// fails as soon as a release zip ships incomplete asset coverage.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FKismetAIPackagingAssetsShippedTest,
	"KismetAI.Packaging.AssetsShipped",
	KismetAIPackagingTestFlags)

bool FKismetAIPackagingAssetsShippedTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI"));
	if (!TestNotNull(TEXT("Plugin 'KismetAI' is loaded"), Plugin.Get()))
	{
		return false;
	}
	const FString BaseDir = Plugin->GetBaseDir();

	// 1. Brand PNGs that Slate brushes load lazily must be on disk.
	const TArray<const TCHAR*> RequiredPngs = {
		TEXT("ToolbarIcon40.png"),
		TEXT("ToolbarIcon20.png"),
		TEXT("Icon128.png"),
	};
	for (const TCHAR* Asset : RequiredPngs)
	{
		const FString Path = BaseDir / TEXT("Resources") / Asset;
		TestTrue(
			FString::Printf(TEXT("Resources/%s present at %s"), Asset, *Path),
			IFileManager::Get().FileExists(*Path));
	}

	// 2. Style brush must be registered AND its resource path must point at a
	//    file that exists. Catches a future renames-out-of-sync regression
	//    where the PNG ships but KismetAIStyle.cpp references a different name.
	const FSlateBrush* ToolbarBrush = FKismetAIStyle::Get().GetBrush(
		FName(TEXT("KismetAI.ToolbarIcon")));
	if (TestNotNull(TEXT("Brush 'KismetAI.ToolbarIcon' registered"), ToolbarBrush))
	{
		const FString BrushFile = ToolbarBrush->GetResourceName().ToString();
		TestTrue(
			FString::Printf(TEXT("Brush resource readable: %s"), *BrushFile),
			!BrushFile.IsEmpty() && IFileManager::Get().FileExists(*BrushFile));
	}

	// 3. ui.html must be loadable via the unified accessor (embedded in release,
	//    on disk in dev). Catches the case where the embed step is silently
	//    disabled and Config/ wasn't shipped either — currently embedded so
	//    this passes on the broken 0.2.3 zip, but it pins the contract.
	const FString DiskUiPath = BaseDir / TEXT("Config/ui.html");
	FString UiContent;
	TestTrue(
		TEXT("ui.html loadable (embedded or disk)"),
		KismetAIEmbeddedConfig::LoadConfigFile(TEXT("ui.html"), DiskUiPath, UiContent)
			&& UiContent.Len() > 0);

	// 4. Self-updated Marketplace installs replace the installed .uplugin with the
	//    descriptor from the release zip. That descriptor must carry
	//    EnabledByDefault=true or the plugin disables itself after restart.
	const FString DescriptorPath = BaseDir / TEXT("KismetAI.uplugin");
	FString DescriptorJson;
	if (TestTrue(TEXT("KismetAI.uplugin readable"), FFileHelper::LoadFileToString(DescriptorJson, *DescriptorPath)))
	{
		TSharedPtr<FJsonObject> Descriptor;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(DescriptorJson);
		if (TestTrue(TEXT("KismetAI.uplugin parses as JSON"),
			FJsonSerializer::Deserialize(Reader, Descriptor) && Descriptor.IsValid()))
		{
			bool bEnabledByDefault = false;
			TestTrue(TEXT("KismetAI.uplugin has EnabledByDefault=true"),
				Descriptor->TryGetBoolField(TEXT("EnabledByDefault"), bEnabledByDefault) && bEnabledByDefault);
		}
	}

	return true;
}
