// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Regression coverage for Windows users whose %USERPROFILE% contains
// Cyrillic, CJK, or other non-ASCII characters. UE's FPaths/FFileHelper/
// IFileManager wrap the wide-char Win32 APIs, so these operations are
// expected to work — but it's easy to accidentally narrow a path through
// TCHAR_TO_ANSI, fopen, or std::ifstream and silently break for those
// users. This test pins the behavior we rely on across the plugin.
//
// String literals below are raw UTF-8. UE's UBT passes /utf-8 to MSVC
// and clang/gcc default to UTF-8, so non-ASCII inside TEXT(...) decodes
// correctly to wide on all supported platforms.

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

constexpr auto NonAsciiPathTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// Cyrillic + CJK directory segment.
	//   U+042E U+0437 U+0435 U+0440 = "Yuzer" in Cyrillic
	//   underscore
	//   U+7528 U+6237 = "yonghu" in Simplified Chinese
	const TCHAR* const NonAsciiDirSegment =
		TEXT("Юзер_用户");

	// Cyrillic + CJK base filename.
	//   U+0444 U+0430 U+0439 U+043B = "fail" (file) in Cyrillic
	//   underscore
	//   U+6587 U+4EF6 = "wenjian" (document) in Simplified Chinese
	const TCHAR* const NonAsciiBaseName =
		TEXT("файл_文件");

	FString MakeScratchRoot()
	{
		const FString Root = FPaths::ProjectIntermediateDir()
			/ TEXT("KismetAINonAsciiPathTest")
			/ FString::Printf(TEXT("%s-%lld"), NonAsciiDirSegment, FDateTime::UtcNow().GetTicks());
		IFileManager::Get().MakeDirectory(*Root, /*Tree=*/true);
		return Root;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// FFileHelper round-trip through a path with Cyrillic + CJK segments.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNonAsciiPathRoundTripTest,
	"KismetAI.Paths.NonAscii.RoundTrip", NonAsciiPathTestFlags)

bool FNonAsciiPathRoundTripTest::RunTest(const FString& Parameters)
{
	const FString Scratch = MakeScratchRoot();
	// Nest non-ASCII inside a non-ASCII directory to exercise tree creation.
	const FString NestedDir = Scratch / NonAsciiDirSegment;
	const FString FilePath = NestedDir / (FString(NonAsciiBaseName) + TEXT(".json"));

	IFileManager& FileMgr = IFileManager::Get();
	TestTrue(TEXT("MakeDirectory creates non-ASCII tree"),
		FileMgr.MakeDirectory(*NestedDir, /*Tree=*/true));
	TestTrue(TEXT("Non-ASCII directory exists after MakeDirectory"),
		FileMgr.DirectoryExists(*NestedDir));

	const FString Payload = FString::Printf(
		TEXT("{\"who\":\"%s\",\"name\":\"%s\"}"),
		NonAsciiDirSegment, NonAsciiBaseName);

	TestTrue(TEXT("SaveStringToFile writes through non-ASCII path"),
		FFileHelper::SaveStringToFile(
			Payload, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));

	TestTrue(TEXT("FileExists reports the non-ASCII file"),
		FileMgr.FileExists(*FilePath));

	FString Roundtrip;
	TestTrue(TEXT("LoadFileToString reads back through non-ASCII path"),
		FFileHelper::LoadFileToString(Roundtrip, *FilePath));
	TestEqual(TEXT("Round-trip content is byte-identical"), Roundtrip, Payload);

	// Also exercise the byte-array path (used by curl response loading).
	TArray<uint8> RawBytes;
	TestTrue(TEXT("LoadFileToArray reads through non-ASCII path"),
		FFileHelper::LoadFileToArray(RawBytes, *FilePath));
	TestTrue(TEXT("LoadFileToArray returns non-empty bytes"), RawBytes.Num() > 0);

	TestTrue(TEXT("Delete removes non-ASCII file"),
		FileMgr.Delete(*FilePath, /*RequireExists=*/true, /*EvenReadOnly=*/false));
	TestFalse(TEXT("FileExists is false after Delete"),
		FileMgr.FileExists(*FilePath));

	FileMgr.DeleteDirectory(*Scratch, /*RequireExists=*/false, /*Tree=*/true);
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// FPaths string operations preserve non-ASCII segments without mojibake.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNonAsciiPathDecompositionTest,
	"KismetAI.Paths.NonAscii.Decomposition", NonAsciiPathTestFlags)

bool FNonAsciiPathDecompositionTest::RunTest(const FString& Parameters)
{
	const FString Filename = FString(NonAsciiBaseName) + TEXT(".json");
	const FString Full = FPaths::ProjectIntermediateDir()
		/ NonAsciiDirSegment / Filename;

	TestEqual(TEXT("GetCleanFilename preserves non-ASCII basename"),
		FPaths::GetCleanFilename(Full), Filename);
	TestEqual(TEXT("GetBaseFilename preserves non-ASCII basename"),
		FPaths::GetBaseFilename(Full), FString(NonAsciiBaseName));
	TestEqual(TEXT("GetExtension is plain ASCII 'json'"),
		FPaths::GetExtension(Full), FString(TEXT("json")));

	const FString Parent = FPaths::GetPath(Full);
	TestTrue(TEXT("GetPath ends with non-ASCII directory segment"),
		Parent.EndsWith(NonAsciiDirSegment));

	const FString Absolute = FPaths::ConvertRelativePathToFull(Full);
	TestTrue(TEXT("ConvertRelativePathToFull preserves non-ASCII directory segment"),
		Absolute.Contains(NonAsciiDirSegment));
	TestTrue(TEXT("ConvertRelativePathToFull preserves non-ASCII basename"),
		Absolute.Contains(NonAsciiBaseName));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// IFileManager::FindFiles surfaces non-ASCII filenames.
// ─────────────────────────────────────────────────────────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNonAsciiPathFindFilesTest,
	"KismetAI.Paths.NonAscii.FindFiles", NonAsciiPathTestFlags)

bool FNonAsciiPathFindFilesTest::RunTest(const FString& Parameters)
{
	const FString Scratch = MakeScratchRoot();
	const FString FilePath = Scratch / (FString(NonAsciiBaseName) + TEXT(".txt"));

	TestTrue(TEXT("Setup: write non-ASCII filename"),
		FFileHelper::SaveStringToFile(TEXT("ok"), *FilePath));

	IFileManager& FileMgr = IFileManager::Get();
	TArray<FString> Found;
	FileMgr.FindFiles(Found, *(Scratch / TEXT("*.txt")), /*Files=*/true, /*Dirs=*/false);

	TestEqual(TEXT("FindFiles returns exactly one match"), Found.Num(), 1);
	if (Found.Num() == 1)
	{
		TestEqual(TEXT("FindFiles result preserves non-ASCII basename"),
			Found[0], FString(NonAsciiBaseName) + TEXT(".txt"));
	}

	FileMgr.DeleteDirectory(*Scratch, /*RequireExists=*/false, /*Tree=*/true);
	return true;
}
