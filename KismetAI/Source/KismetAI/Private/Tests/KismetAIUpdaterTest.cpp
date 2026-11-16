// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIUpdater.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

constexpr auto KismetAIUpdaterTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	FString MakeScratchDir(const TCHAR* Tag)
	{
		const FString Base = FPaths::ProjectIntermediateDir() / TEXT("KismetAIUpdaterTest")
			/ FString::Printf(TEXT("%s-%lld"), Tag, FDateTime::UtcNow().GetTicks());
		IFileManager::Get().MakeDirectory(*Base, /*Tree=*/true);
		return Base;
	}

	void WriteFile(const FString& Path, const FString& Body)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		FFileHelper::SaveStringToFile(Body, *Path);
	}

	FString ReadFile(const FString& Path)
	{
		FString Out;
		FFileHelper::LoadFileToString(Out, *Path);
		return Out;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Happy path: staged files replace live files, originals preserved as .old
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIUpdaterApplyHappyPath,
	"KismetAI.Updater.Apply.HappyPath", KismetAIUpdaterTestFlags)

bool FKismetAIUpdaterApplyHappyPath::RunTest(const FString& Parameters)
{
	const FString Scratch = MakeScratchDir(TEXT("happy"));
	const FString TargetDir = Scratch / TEXT("plugin");
	const FString StagedDir = Scratch / TEXT("staged");

	// Original (live) files.
	WriteFile(TargetDir / TEXT("KismetAI.uplugin"),                 TEXT("OLD-UPLUGIN"));
	WriteFile(TargetDir / TEXT("Binaries/Mac/UnrealEditor-KismetAI.dylib"), TEXT("OLD-DYLIB"));

	// Staged files (mirror package.ps1 layout: contents wrapped in KismetAI/).
	WriteFile(StagedDir / TEXT("KismetAI/KismetAI.uplugin"),        TEXT("NEW-UPLUGIN"));
	WriteFile(StagedDir / TEXT("KismetAI/Binaries/Mac/UnrealEditor-KismetAI.dylib"), TEXT("NEW-DYLIB"));

	FString Err;
	const bool bOk = FKismetAIUpdater::ApplyStagedTreeForTest(StagedDir, TargetDir, Err);
	TestTrue(TEXT("Apply succeeded"), bOk);
	TestEqual(TEXT("No error message"), Err, FString());

	TestEqual(TEXT("Live uplugin replaced"),
		ReadFile(TargetDir / TEXT("KismetAI.uplugin")), FString(TEXT("NEW-UPLUGIN")));
	TestEqual(TEXT("Live dylib replaced"),
		ReadFile(TargetDir / TEXT("Binaries/Mac/UnrealEditor-KismetAI.dylib")), FString(TEXT("NEW-DYLIB")));

	TestEqual(TEXT(".old uplugin contains pre-apply contents"),
		ReadFile(TargetDir / TEXT("KismetAI.uplugin.old")), FString(TEXT("OLD-UPLUGIN")));
	TestEqual(TEXT(".old dylib contains pre-apply contents"),
		ReadFile(TargetDir / TEXT("Binaries/Mac/UnrealEditor-KismetAI.dylib.old")), FString(TEXT("OLD-DYLIB")));

	IFileManager::Get().DeleteDirectory(*Scratch, false, /*Tree=*/true);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// New-file case: staged file with no live counterpart still lands in place,
// no .old left behind.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIUpdaterApplyNewFile,
	"KismetAI.Updater.Apply.NewFile", KismetAIUpdaterTestFlags)

bool FKismetAIUpdaterApplyNewFile::RunTest(const FString& Parameters)
{
	const FString Scratch = MakeScratchDir(TEXT("new"));
	const FString TargetDir = Scratch / TEXT("plugin");
	const FString StagedDir = Scratch / TEXT("staged");

	IFileManager::Get().MakeDirectory(*TargetDir, /*Tree=*/true);
	WriteFile(StagedDir / TEXT("KismetAI/Brand/New/File.txt"), TEXT("NEW"));

	FString Err;
	TestTrue(TEXT("Apply succeeded"),
		FKismetAIUpdater::ApplyStagedTreeForTest(StagedDir, TargetDir, Err));
	TestEqual(TEXT("Brand-new file landed"),
		ReadFile(TargetDir / TEXT("Brand/New/File.txt")), FString(TEXT("NEW")));
	TestFalse(TEXT("No spurious .old for brand-new file"),
		IFileManager::Get().FileExists(*(TargetDir / TEXT("Brand/New/File.txt.old"))));

	IFileManager::Get().DeleteDirectory(*Scratch, false, /*Tree=*/true);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rollback: when a later move fails (we inject failure by leaving an unsafe
// path in the staged tree), prior swaps are undone and live files are restored.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIUpdaterApplyRollback,
	"KismetAI.Updater.Apply.Rollback", KismetAIUpdaterTestFlags)

bool FKismetAIUpdaterApplyRollback::RunTest(const FString& Parameters)
{
	const FString Scratch = MakeScratchDir(TEXT("rollback"));
	const FString TargetDir = Scratch / TEXT("plugin");
	const FString StagedDir = Scratch / TEXT("staged");

	WriteFile(TargetDir / TEXT("KismetAI.uplugin"), TEXT("ORIGINAL"));

	// First entry: ordinary swap (will succeed). Goes into EarlyFiles bucket.
	WriteFile(StagedDir / TEXT("KismetAI/KismetAI.uplugin"), TEXT("NEW-UPLUGIN"));
	// Second entry: lives under Binaries/ so it goes into the BinaryFiles
	// bucket, applied AFTER EarlyFiles. The literal ".." sequence inside the
	// filename trips IsSafeRelativePath (Contains("..") returns true) and
	// forces the apply to abort, exercising rollback.
	WriteFile(StagedDir / TEXT("KismetAI/Binaries/Mac/foo..bar.dylib"), TEXT("BAD"));

	FString Err;
	const bool bOk = FKismetAIUpdater::ApplyStagedTreeForTest(StagedDir, TargetDir, Err);
	TestFalse(TEXT("Apply rejected (returned false)"), bOk);
	TestFalse(TEXT("Error message populated"), Err.IsEmpty());

	// Critical: the live file must be restored to its original content.
	TestEqual(TEXT("Live uplugin restored to original after rollback"),
		ReadFile(TargetDir / TEXT("KismetAI.uplugin")), FString(TEXT("ORIGINAL")));
	TestFalse(TEXT(".old left behind after rollback"),
		IFileManager::Get().FileExists(*(TargetDir / TEXT("KismetAI.uplugin.old"))));

	IFileManager::Get().DeleteDirectory(*Scratch, false, /*Tree=*/true);
	return true;
}

// Same-root fixture deliberately aliases source and target to inject a missing
// staged source after rename-aside. This is not a supported install layout.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIUpdaterApplyFailedMoveRestoresCurrent,
	"KismetAI.Updater.Apply.FailedMoveRestoresCurrent", KismetAIUpdaterTestFlags)

bool FKismetAIUpdaterApplyFailedMoveRestoresCurrent::RunTest(const FString& Parameters)
{
	const FString Scratch = MakeScratchDir(TEXT("failed-move"));
	const FString LivePath = Scratch / TEXT("KismetAI.uplugin");
	WriteFile(LivePath, TEXT("ORIGINAL"));
	AddExpectedError(TEXT("Error moving file"), EAutomationExpectedErrorFlags::Contains, 1);

	FString Err;
	TestFalse(TEXT("Injected staged move fails"),
		FKismetAIUpdater::ApplyStagedTreeForTest(Scratch, Scratch, Err));
	TestTrue(TEXT("Failure identifies staged move"), Err.Contains(TEXT("Could not move staged file into place")));
	const bool bLiveExists = IFileManager::Get().FileExists(*LivePath);
	TestTrue(TEXT("Current live file restored after its staged move failed"), bLiveExists);
	if (bLiveExists)
	{
		TestEqual(TEXT("Current live file retains original contents"), ReadFile(LivePath), FString(TEXT("ORIGINAL")));
	}
	TestFalse(TEXT("Successful restoration consumes current backup"),
		IFileManager::Get().FileExists(*(LivePath + TEXT(".old"))));
	IFileManager::Get().DeleteDirectory(*Scratch, false, /*Tree=*/true);
	return true;
}
