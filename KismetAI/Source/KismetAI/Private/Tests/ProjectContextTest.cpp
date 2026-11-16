// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIProjectContext.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/AutomationTest.h"

constexpr auto ProjectContextTestFlags =
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// A scoped temp project root under ProjectIntermediate/. Lets the scan/import
	// logic run against a controlled directory without touching the real project.
	struct FScopedTempProjectDir
	{
		FString Root;

		explicit FScopedTempProjectDir(const TCHAR* TestName)
		{
			Root = FPaths::ProjectIntermediateDir() /
				FString::Printf(TEXT("%s_%s"), TestName,
					*FGuid::NewGuid().ToString(EGuidFormats::Short));
			IFileManager::Get().MakeDirectory(*Root, /*Tree=*/true);
		}

		~FScopedTempProjectDir()
		{
			IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
		}

		void WriteFile(const FString& RelPath, const FString& Content) const
		{
			const FString Full = Root / RelPath;
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Full), /*Tree=*/true);
			FFileHelper::SaveStringToFile(Content, *Full,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
	};
}

// ─────────────────────────────────────────────────────────────────────────────
// Import path: a .cursorrules file is found and concatenated into AGENTS.md.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectContext_ImportsCursorRules,
	"KismetAI.ProjectContext.ImportsCursorRules",
	ProjectContextTestFlags)
bool FProjectContext_ImportsCursorRules::RunTest(const FString& Parameters)
{
	FScopedTempProjectDir Dir(TEXT("PCImport"));
	const FString Marker = TEXT("ALWAYS use tabs not spaces in this repo.");
	Dir.WriteFile(TEXT(".cursorrules"), Marker);

	FKismetAIProjectContext::FInitResult Result =
		FKismetAIProjectContext::InitProjectContext(Dir.Root);

	TestEqual(TEXT("status is Imported"),
		(int32)Result.Status, (int32)FKismetAIProjectContext::EInitStatus::Imported);
	TestEqual(TEXT(".cursorrules listed in imported_from"),
		Result.ImportedFrom.Num(), 1);

	// AGENTS.md was written and contains the imported content under a header.
	FString Written;
	const bool bRead = FFileHelper::LoadFileToString(Written,
		*(Dir.Root / FKismetAIProjectContext::GetTargetFileName()));
	TestTrue(TEXT("AGENTS.md written"), bRead);
	TestTrue(TEXT("contains imported marker"), Written.Contains(Marker));
	TestTrue(TEXT("contains import header comment"),
		Written.Contains(TEXT("<!-- Imported from .cursorrules -->")));
	TestTrue(TEXT("contains Cursor rules label"),
		Written.Contains(TEXT("## Cursor rules")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Import path: a rules directory is concatenated (multiple .md/.mdc files).
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectContext_ImportsRulesDir,
	"KismetAI.ProjectContext.ImportsRulesDir",
	ProjectContextTestFlags)
bool FProjectContext_ImportsRulesDir::RunTest(const FString& Parameters)
{
	FScopedTempProjectDir Dir(TEXT("PCDir"));
	Dir.WriteFile(TEXT(".cursor/rules/a.mdc"), TEXT("Rule A content."));
	Dir.WriteFile(TEXT(".cursor/rules/b.md"), TEXT("Rule B content."));

	FKismetAIProjectContext::FInitResult Result =
		FKismetAIProjectContext::InitProjectContext(Dir.Root);

	TestEqual(TEXT("status is Imported"),
		(int32)Result.Status, (int32)FKismetAIProjectContext::EInitStatus::Imported);

	FString Written;
	FFileHelper::LoadFileToString(Written,
		*(Dir.Root / FKismetAIProjectContext::GetTargetFileName()));
	TestTrue(TEXT("contains rule A"), Written.Contains(TEXT("Rule A content.")));
	TestTrue(TEXT("contains rule B"), Written.Contains(TEXT("Rule B content.")));
	TestTrue(TEXT("dir source labeled"),
		Written.Contains(TEXT("<!-- Imported from .cursor/rules/ -->")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Template path: empty project root → seeded template written.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectContext_CreatesTemplate,
	"KismetAI.ProjectContext.CreatesTemplate",
	ProjectContextTestFlags)
bool FProjectContext_CreatesTemplate::RunTest(const FString& Parameters)
{
	FScopedTempProjectDir Dir(TEXT("PCTemplate"));

	FKismetAIProjectContext::FInitResult Result =
		FKismetAIProjectContext::InitProjectContext(Dir.Root);

	TestEqual(TEXT("status is Created"),
		(int32)Result.Status, (int32)FKismetAIProjectContext::EInitStatus::Created);
	TestEqual(TEXT("no imports for template path"), Result.ImportedFrom.Num(), 0);

	FString Written;
	const bool bRead = FFileHelper::LoadFileToString(Written,
		*(Dir.Root / FKismetAIProjectContext::GetTargetFileName()));
	TestTrue(TEXT("AGENTS.md written"), bRead);
	TestTrue(TEXT("template has Conventions section"),
		Written.Contains(TEXT("## Conventions")));
	TestTrue(TEXT("template has off-limits section"),
		Written.Contains(TEXT("## Blueprints the agent should not touch")));
	TestTrue(TEXT("template has Domain notes section"),
		Written.Contains(TEXT("## Domain notes")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Exists path: a chain file already present → no overwrite.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectContext_DoesNotOverwriteExisting,
	"KismetAI.ProjectContext.DoesNotOverwriteExisting",
	ProjectContextTestFlags)
bool FProjectContext_DoesNotOverwriteExisting::RunTest(const FString& Parameters)
{
	FScopedTempProjectDir Dir(TEXT("PCExists"));
	const FString Original = TEXT("# My existing context\nKeep me.");
	Dir.WriteFile(TEXT("AGENTS.md"), Original);
	// Also drop a .cursorrules to prove it is NOT imported when a chain file exists.
	Dir.WriteFile(TEXT(".cursorrules"), TEXT("should not be imported"));

	FKismetAIProjectContext::FInitResult Result =
		FKismetAIProjectContext::InitProjectContext(Dir.Root);

	TestEqual(TEXT("status is Exists"),
		(int32)Result.Status, (int32)FKismetAIProjectContext::EInitStatus::Exists);

	FString Written;
	FFileHelper::LoadFileToString(Written,
		*(Dir.Root / FKismetAIProjectContext::GetTargetFileName()));
	TestEqual(TEXT("existing AGENTS.md untouched"), Written, Original);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Status JSON reflects scan + dismissal flag.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectContext_StatusJson,
	"KismetAI.ProjectContext.StatusJson",
	ProjectContextTestFlags)
bool FProjectContext_StatusJson::RunTest(const FString& Parameters)
{
	FScopedTempProjectDir Dir(TEXT("PCStatus"));
	Dir.WriteFile(TEXT(".windsurfrules"), TEXT("some rules"));

	FKismetAIProjectContext::FScanResult Scan =
		FKismetAIProjectContext::ScanProjectRoot(Dir.Root);
	TestTrue(TEXT("no chain file"), Scan.ExistingChainFile.IsEmpty());
	TestEqual(TEXT("one importable"), Scan.Importable.Num(), 1);

	const FString Json = FKismetAIProjectContext::StatusToJson(Scan, /*bDismissed=*/true);
	TestTrue(TEXT("has_context false"), Json.Contains(TEXT("\"has_context\":false")));
	TestTrue(TEXT("importable lists windsurf"), Json.Contains(TEXT(".windsurfrules")));
	TestTrue(TEXT("dismissed true"), Json.Contains(TEXT("\"onboarding_dismissed\":true")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Import panel: DetectSources lists chain-format other-tool files (CLAUDE.md)
// alongside dedicated rule files, but never Kismet's own AGENTS.md/KISMET.md.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectContext_DetectsAllSources,
	"KismetAI.ProjectContext.DetectsAllSources",
	ProjectContextTestFlags)
bool FProjectContext_DetectsAllSources::RunTest(const FString& Parameters)
{
	FScopedTempProjectDir Dir(TEXT("PCDetect"));
	Dir.WriteFile(TEXT(".cursorrules"), TEXT("cursor"));
	Dir.WriteFile(TEXT("CLAUDE.md"), TEXT("claude"));
	Dir.WriteFile(TEXT(".cursor/rules/a.md"), TEXT("dir rule"));
	// Kismet's own files must be excluded from the source list.
	Dir.WriteFile(TEXT("AGENTS.md"), TEXT("kismet own"));

	const TArray<FKismetAIProjectContext::FDetectedSource> Sources =
		FKismetAIProjectContext::DetectSources(Dir.Root);

	auto HasId = [&Sources](const TCHAR* Id)
	{
		return Sources.ContainsByPredicate(
			[Id](const FKismetAIProjectContext::FDetectedSource& S) { return S.Id == Id; });
	};

	TestTrue(TEXT("detects .cursorrules"), HasId(TEXT(".cursorrules")));
	TestTrue(TEXT("detects CLAUDE.md"), HasId(TEXT("CLAUDE.md")));
	TestTrue(TEXT("detects .cursor/rules/ dir"), HasId(TEXT(".cursor/rules/")));
	TestFalse(TEXT("never lists AGENTS.md as a source"), HasId(TEXT("AGENTS.md")));

	// FindExistingTarget picks up Kismet's own file.
	TestEqual(TEXT("existing target is AGENTS.md"),
		FKismetAIProjectContext::FindExistingTarget(Dir.Root), FString(TEXT("AGENTS.md")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Import panel: ImportSelected imports ONLY the chosen ids and appends to an
// existing target rather than overwriting it.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectContext_ImportSelectedSubset,
	"KismetAI.ProjectContext.ImportSelectedSubset",
	ProjectContextTestFlags)
bool FProjectContext_ImportSelectedSubset::RunTest(const FString& Parameters)
{
	FScopedTempProjectDir Dir(TEXT("PCSelect"));
	Dir.WriteFile(TEXT(".cursorrules"), TEXT("CURSOR_MARKER"));
	Dir.WriteFile(TEXT("CLAUDE.md"), TEXT("CLAUDE_MARKER"));

	// Select only Claude; Cursor must be left out.
	TArray<FString> Selected = { TEXT("CLAUDE.md") };
	FKismetAIProjectContext::FInitResult Result =
		FKismetAIProjectContext::ImportSelected(Dir.Root, Selected);

	TestEqual(TEXT("status is Imported"),
		(int32)Result.Status, (int32)FKismetAIProjectContext::EInitStatus::Imported);
	TestEqual(TEXT("exactly one source imported"), Result.ImportedFrom.Num(), 1);

	FString Written;
	FFileHelper::LoadFileToString(Written,
		*(Dir.Root / FKismetAIProjectContext::GetTargetFileName()));
	TestTrue(TEXT("contains selected Claude content"), Written.Contains(TEXT("CLAUDE_MARKER")));
	TestFalse(TEXT("excludes unselected Cursor content"), Written.Contains(TEXT("CURSOR_MARKER")));

	// Re-import Cursor — should append, preserving the prior Claude content.
	TArray<FString> Selected2 = { TEXT(".cursorrules") };
	FKismetAIProjectContext::ImportSelected(Dir.Root, Selected2);
	FFileHelper::LoadFileToString(Written,
		*(Dir.Root / FKismetAIProjectContext::GetTargetFileName()));
	TestTrue(TEXT("append keeps Claude content"), Written.Contains(TEXT("CLAUDE_MARKER")));
	TestTrue(TEXT("append adds Cursor content"), Written.Contains(TEXT("CURSOR_MARKER")));

	return true;
}
