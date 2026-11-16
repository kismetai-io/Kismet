// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIProjectContext.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	// The runtime loader's precedence chain (first match wins). Mirrors
	// KismetAIAgent.cpp — keep in sync. AGENTS.md is the write target.
	const TCHAR* const ChainFiles[] =
	{
		TEXT("KISMET.md"),
		TEXT("AGENTS.md"),
		TEXT("CLAUDE.md"),
		TEXT("GEMINI.md"),
		TEXT("KISMETAI.md"),
	};

	// Other-format single files from popular agent tools. The chain does NOT
	// read these natively, so /init offers to import them into AGENTS.md.
	struct FSingleSource
	{
		const TCHAR* RelPath;   // path relative to project root
		const TCHAR* ToolLabel; // friendly name for the import header
	};

	// Chain-format files written by OTHER agent tools. The runtime loader reads
	// these natively as fallbacks, but the import panel still offers them so the
	// user can consolidate everything into Kismet's own file. KISMET.md and
	// AGENTS.md are deliberately absent — those are Kismet's own targets.
	const FSingleSource ChainImportable[] =
	{
		{ TEXT("CLAUDE.md"),    TEXT("Claude") },
		{ TEXT("GEMINI.md"),    TEXT("Gemini") },
		{ TEXT("KISMETAI.md"),  TEXT("Kismet (legacy)") },
	};

	const FSingleSource SingleSources[] =
	{
		{ TEXT(".cursorrules"),                       TEXT("Cursor") },
		{ TEXT(".windsurfrules"),                     TEXT("Windsurf") },
		{ TEXT("CONVENTIONS.md"),                     TEXT("Aider") },
		{ TEXT(".clinerules"),                        TEXT("Cline") },
		{ TEXT(".roorules"),                          TEXT("Roo Code") },
		{ TEXT(".github/copilot-instructions.md"),    TEXT("GitHub Copilot") },
		{ TEXT(".idx/airules.md"),                    TEXT("Firebase Studio") },
		{ TEXT(".junie/guidelines.md"),               TEXT("JetBrains Junie") },
		{ TEXT("AI.md"),                              TEXT("AI") },
		{ TEXT("AI_INSTRUCTIONS.md"),                 TEXT("AI Instructions") },
		{ TEXT("README.ai.md"),                       TEXT("README (AI)") },
	};

	// Directory-based rule sources — concatenate every *.md / *.mdc inside.
	struct FDirSource
	{
		const TCHAR* RelDir;
		const TCHAR* ToolLabel;
	};

	const FDirSource DirSources[] =
	{
		{ TEXT(".cursor/rules"),    TEXT("Cursor") },
		{ TEXT(".windsurf/rules"),  TEXT("Windsurf") },
		{ TEXT(".clinerules"),      TEXT("Cline") },   // may be a directory in newer Cline
		{ TEXT(".roo/rules"),       TEXT("Roo Code") },
		{ TEXT(".continue/rules"),  TEXT("Continue") },
		{ TEXT(".amazonq/rules"),   TEXT("Amazon Q") },
	};
}

FKismetAIProjectContext::FScanResult FKismetAIProjectContext::ScanProjectRoot(const FString& ProjectRoot)
{
	FScanResult Result;
	IFileManager& FileMgr = IFileManager::Get();

	// 1. Existing chain file (first match wins).
	for (const TCHAR* ChainName : ChainFiles)
	{
		const FString Path = ProjectRoot / ChainName;
		if (FileMgr.FileExists(*Path))
		{
			Result.ExistingChainFile = ChainName;
			break;
		}
	}

	// 2. Importable single-file sources.
	for (const FSingleSource& Source : SingleSources)
	{
		const FString Path = ProjectRoot / Source.RelPath;
		// A directory-form .clinerules is handled by the dir scan below; only
		// treat it as a single source when it is actually a file.
		if (FileMgr.FileExists(*Path))
		{
			Result.Importable.Add(Source.RelPath);
		}
	}

	// 3. Importable directory-based sources (only if they contain rule files).
	for (const FDirSource& Source : DirSources)
	{
		const FString DirPath = ProjectRoot / Source.RelDir;
		if (!FileMgr.DirectoryExists(*DirPath))
		{
			continue;
		}
		TArray<FString> Found;
		FileMgr.FindFiles(Found, *(DirPath / TEXT("*.md")), /*Files=*/true, /*Dirs=*/false);
		FileMgr.FindFiles(Found, *(DirPath / TEXT("*.mdc")), /*Files=*/true, /*Dirs=*/false);
		if (Found.Num() > 0)
		{
			// Present as a trailing-slash relative path so it's clear it's a dir.
			Result.Importable.Add(FString(Source.RelDir) + TEXT("/"));
		}
	}

	return Result;
}

bool FKismetAIProjectContext::TryImportSingleFile(const FString& ProjectRoot, const FString& RelPath,
	const FString& ToolLabel, FString& OutSection)
{
	FString Content;
	if (!FFileHelper::LoadFileToString(Content, *(ProjectRoot / RelPath)))
	{
		return false;
	}
	Content.TrimStartAndEndInline();
	if (Content.IsEmpty())
	{
		return false;
	}

	OutSection = FString::Printf(
		TEXT("<!-- Imported from %s -->\n## %s rules\n\n%s\n"),
		*RelPath, *ToolLabel, *Content);
	return true;
}

bool FKismetAIProjectContext::TryImportRulesDir(const FString& ProjectRoot, const FString& RelDir,
	const FString& ToolLabel, FString& OutSection)
{
	IFileManager& FileMgr = IFileManager::Get();
	const FString DirPath = ProjectRoot / RelDir;

	TArray<FString> Files;
	FileMgr.FindFiles(Files, *(DirPath / TEXT("*.md")), /*Files=*/true, /*Dirs=*/false);
	FileMgr.FindFiles(Files, *(DirPath / TEXT("*.mdc")), /*Files=*/true, /*Dirs=*/false);
	if (Files.Num() == 0)
	{
		return false;
	}
	// Stable order regardless of filesystem enumeration.
	Files.Sort();

	FString Combined;
	for (const FString& File : Files)
	{
		FString Content;
		if (!FFileHelper::LoadFileToString(Content, *(DirPath / File)))
		{
			continue;
		}
		Content.TrimStartAndEndInline();
		if (Content.IsEmpty())
		{
			continue;
		}
		if (!Combined.IsEmpty())
		{
			Combined += TEXT("\n\n");
		}
		Combined += Content;
	}

	if (Combined.IsEmpty())
	{
		return false;
	}

	OutSection = FString::Printf(
		TEXT("<!-- Imported from %s/ -->\n## %s rules\n\n%s\n"),
		*RelDir, *ToolLabel, *Combined);
	return true;
}

FString FKismetAIProjectContext::BuildTemplate()
{
	// ~25 lines of commented scaffolding for a UE5 Blueprint project. Concise,
	// with the three sections the spec asks for.
	return TEXT(
		"<!-- AGENTS.md — project instructions for AI agents.\n"
		"     Kismet reads this file (the cross-tool open standard) and injects it\n"
		"     into the agent's system prompt for every run in this project.\n"
		"     Edit freely; keep it short and high-signal. -->\n"
		"\n"
		"# Project Context\n"
		"\n"
		"<!-- One line on what this project is. -->\n"
		"Project: <your project name here>\n"
		"\n"
		"## Conventions\n"
		"\n"
		"<!-- Naming, folder layout, prefixes the agent should follow. -->\n"
		"- Blueprints live under /Game/Blueprints.\n"
		"- Prefix actor Blueprints with BP_ and widgets with WBP_.\n"
		"\n"
		"## Blueprints the agent should not touch\n"
		"\n"
		"<!-- List assets that are off-limits for edits. -->\n"
		"- (none yet)\n"
		"\n"
		"## Domain notes\n"
		"\n"
		"<!-- Gameplay systems, key actors, anything the agent should know. -->\n"
		"- (add project-specific notes here)\n");
}

FKismetAIProjectContext::FInitResult FKismetAIProjectContext::InitProjectContext(const FString& ProjectRoot)
{
	FInitResult Result;
	Result.TargetPath = FPaths::ConvertRelativePathToFull(ProjectRoot / GetTargetFileName());

	const FScanResult Scan = ScanProjectRoot(ProjectRoot);

	// 1. A chain file already exists — never overwrite.
	if (!Scan.ExistingChainFile.IsEmpty())
	{
		Result.Status = EInitStatus::Exists;
		Result.Message = FString::Printf(
			TEXT("Project context already exists (%s). Edit that file to update instructions."),
			*Scan.ExistingChainFile);
		return Result;
	}

	// 2. Import from other-format sources, if any.
	FString FileContents;
	TArray<FString> ImportedFrom;
	if (Scan.Importable.Num() > 0)
	{
		FString Body;
		// Single-file sources.
		for (const FSingleSource& Source : SingleSources)
		{
			const FString Path = ProjectRoot / Source.RelPath;
			if (!IFileManager::Get().FileExists(*Path))
			{
				continue;
			}
			FString Section;
			if (TryImportSingleFile(ProjectRoot, Source.RelPath, Source.ToolLabel, Section))
			{
				if (!Body.IsEmpty()) { Body += TEXT("\n"); }
				Body += Section;
				ImportedFrom.Add(Source.RelPath);
			}
		}
		// Directory-based sources.
		for (const FDirSource& Source : DirSources)
		{
			FString Section;
			if (TryImportRulesDir(ProjectRoot, Source.RelDir, Source.ToolLabel, Section))
			{
				if (!Body.IsEmpty()) { Body += TEXT("\n"); }
				Body += Section;
				ImportedFrom.Add(FString(Source.RelDir) + TEXT("/"));
			}
		}

		if (!Body.IsEmpty())
		{
			FileContents =
				TEXT("<!-- AGENTS.md — generated by Kismet /init from existing agent-tool rules.\n")
				TEXT("     Kismet reads this file and injects it into the agent's system prompt. -->\n\n")
				+ Body;
		}
	}

	if (!FileContents.IsEmpty())
	{
		if (!FFileHelper::SaveStringToFile(FileContents, *Result.TargetPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Result.Status = EInitStatus::Failed;
			Result.Message = TEXT("Failed to write AGENTS.md.");
			return Result;
		}
		Result.Status = EInitStatus::Imported;
		Result.ImportedFrom = ImportedFrom;
		Result.Message = FString::Printf(
			TEXT("Imported %d source(s) into AGENTS.md."), ImportedFrom.Num());
		return Result;
	}

	// 3. Nothing found — seed the template.
	const FString Template = BuildTemplate();
	if (!FFileHelper::SaveStringToFile(Template, *Result.TargetPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.Status = EInitStatus::Failed;
		Result.Message = TEXT("Failed to write AGENTS.md.");
		return Result;
	}
	Result.Status = EInitStatus::Created;
	Result.Message = TEXT("Created a starter AGENTS.md. Fill in the project-specific sections.");
	return Result;
}

FString FKismetAIProjectContext::InitResultToJson(const FInitResult& Result)
{
	const TCHAR* StatusStr = TEXT("failed");
	switch (Result.Status)
	{
	case EInitStatus::Exists:   StatusStr = TEXT("exists");   break;
	case EInitStatus::Imported: StatusStr = TEXT("imported"); break;
	case EInitStatus::Created:  StatusStr = TEXT("created");  break;
	case EInitStatus::Failed:   StatusStr = TEXT("failed");   break;
	}

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("status"), StatusStr);
	Obj->SetStringField(TEXT("target"), GetTargetFileName());
	Obj->SetStringField(TEXT("message"), Result.Message);

	TArray<TSharedPtr<FJsonValue>> Sources;
	for (const FString& Src : Result.ImportedFrom)
	{
		Sources.Add(MakeShared<FJsonValueString>(Src));
	}
	Obj->SetArrayField(TEXT("imported_from"), Sources);

	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj, Writer);
	return Output;
}

FString FKismetAIProjectContext::FindExistingTarget(const FString& ProjectRoot)
{
	// Kismet's own files, highest precedence first. The import panel writes to
	// whichever already exists (appending), or creates AGENTS.md when neither does.
	for (const TCHAR* Name : { TEXT("KISMET.md"), TEXT("AGENTS.md") })
	{
		if (IFileManager::Get().FileExists(*(ProjectRoot / Name)))
		{
			return Name;
		}
	}
	return FString();
}

TArray<FKismetAIProjectContext::FDetectedSource> FKismetAIProjectContext::DetectSources(const FString& ProjectRoot)
{
	IFileManager& FileMgr = IFileManager::Get();
	TArray<FDetectedSource> Out;

	// Single-file sources: chain-format other-tool files first (Claude/Gemini/…),
	// then the dedicated agent-tool rule files (Cursor/Windsurf/…).
	auto AddSingles = [&](const FSingleSource* Arr, int32 Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			if (FileMgr.FileExists(*(ProjectRoot / Arr[i].RelPath)))
			{
				Out.Add({ FString(Arr[i].RelPath), FString(Arr[i].ToolLabel), /*bIsDir=*/false });
			}
		}
	};
	AddSingles(SingleSources, UE_ARRAY_COUNT(SingleSources));
	AddSingles(ChainImportable, UE_ARRAY_COUNT(ChainImportable));

	// Directory-based sources — only when they contain at least one rule file.
	for (const FDirSource& Source : DirSources)
	{
		const FString DirPath = ProjectRoot / Source.RelDir;
		if (!FileMgr.DirectoryExists(*DirPath))
		{
			continue;
		}
		TArray<FString> Found;
		FileMgr.FindFiles(Found, *(DirPath / TEXT("*.md")), /*Files=*/true, /*Dirs=*/false);
		FileMgr.FindFiles(Found, *(DirPath / TEXT("*.mdc")), /*Files=*/true, /*Dirs=*/false);
		if (Found.Num() > 0)
		{
			Out.Add({ FString(Source.RelDir) + TEXT("/"), FString(Source.ToolLabel), /*bIsDir=*/true });
		}
	}

	return Out;
}

FKismetAIProjectContext::FInitResult FKismetAIProjectContext::ImportSelected(
	const FString& ProjectRoot, const TArray<FString>& SelectedIds)
{
	const FString ExistingTarget = FindExistingTarget(ProjectRoot);
	const FString TargetRel = ExistingTarget.IsEmpty() ? FString(GetTargetFileName()) : ExistingTarget;

	FInitResult Result;
	Result.TargetPath = FPaths::ConvertRelativePathToFull(ProjectRoot / TargetRel);

	// Resolve each selected id back to a (label, kind) and import it. Iterate the
	// registries (not SelectedIds) so output order is stable regardless of UI order.
	FString Body;
	TArray<FString> ImportedFrom;
	auto WantsId = [&](const FString& Id) { return SelectedIds.Contains(Id) && Id != TargetRel; };

	auto ImportSingles = [&](const FSingleSource* Arr, int32 Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			const FString Id = Arr[i].RelPath;
			if (!WantsId(Id)) { continue; }
			FString Section;
			if (TryImportSingleFile(ProjectRoot, Id, Arr[i].ToolLabel, Section))
			{
				if (!Body.IsEmpty()) { Body += TEXT("\n"); }
				Body += Section;
				ImportedFrom.Add(Id);
			}
		}
	};
	ImportSingles(SingleSources, UE_ARRAY_COUNT(SingleSources));
	ImportSingles(ChainImportable, UE_ARRAY_COUNT(ChainImportable));

	for (const FDirSource& Source : DirSources)
	{
		const FString Id = FString(Source.RelDir) + TEXT("/");
		if (!WantsId(Id)) { continue; }
		FString Section;
		if (TryImportRulesDir(ProjectRoot, Source.RelDir, Source.ToolLabel, Section))
		{
			if (!Body.IsEmpty()) { Body += TEXT("\n"); }
			Body += Section;
			ImportedFrom.Add(Id);
		}
	}

	if (Body.IsEmpty())
	{
		Result.Status = EInitStatus::Failed;
		Result.Message = TEXT("Nothing was imported — no readable sources were selected.");
		return Result;
	}

	// Append to an existing Kismet file, or create a fresh AGENTS.md with a header.
	FString FileContents;
	if (!ExistingTarget.IsEmpty())
	{
		FFileHelper::LoadFileToString(FileContents, *Result.TargetPath);
		FileContents.TrimEndInline();
		if (!FileContents.IsEmpty()) { FileContents += TEXT("\n\n"); }
		FileContents += Body;
	}
	else
	{
		FileContents =
			TEXT("<!-- AGENTS.md — generated by Kismet /import from existing agent-tool rules.\n")
			TEXT("     Kismet reads this file and injects it into the agent's system prompt. -->\n\n")
			+ Body;
	}

	if (!FFileHelper::SaveStringToFile(FileContents, *Result.TargetPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		Result.Status = EInitStatus::Failed;
		Result.Message = FString::Printf(TEXT("Failed to write %s."), *TargetRel);
		return Result;
	}

	Result.Status = EInitStatus::Imported;
	Result.ImportedFrom = ImportedFrom;
	Result.Message = FString::Printf(
		TEXT("Imported %d source(s) into %s."), ImportedFrom.Num(), *TargetRel);
	return Result;
}

FString FKismetAIProjectContext::SourcesToJson(
	const TArray<FDetectedSource>& Sources, const FString& ExistingTarget, bool bOnboardingDismissed)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("target"), GetTargetFileName());
	Obj->SetStringField(TEXT("existing_target"), ExistingTarget);
	Obj->SetBoolField(TEXT("onboarding_dismissed"), bOnboardingDismissed);

	TArray<TSharedPtr<FJsonValue>> Arr;
	for (const FDetectedSource& Src : Sources)
	{
		TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("id"), Src.Id);
		S->SetStringField(TEXT("label"), Src.Label);
		S->SetBoolField(TEXT("is_dir"), Src.bIsDir);
		Arr.Add(MakeShared<FJsonValueObject>(S));
	}
	Obj->SetArrayField(TEXT("sources"), Arr);

	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj, Writer);
	return Output;
}

FString FKismetAIProjectContext::StatusToJson(const FScanResult& Scan, bool bOnboardingDismissed)
{
	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetBoolField(TEXT("has_context"), !Scan.ExistingChainFile.IsEmpty());
	Obj->SetStringField(TEXT("context_file"), Scan.ExistingChainFile);

	TArray<TSharedPtr<FJsonValue>> Importable;
	for (const FString& Src : Scan.Importable)
	{
		Importable.Add(MakeShared<FJsonValueString>(Src));
	}
	Obj->SetArrayField(TEXT("importable"), Importable);
	Obj->SetBoolField(TEXT("onboarding_dismissed"), bOnboardingDismissed);

	FString Output;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
	FJsonSerializer::Serialize(Obj, Writer);
	return Output;
}
