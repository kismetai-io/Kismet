// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

/**
 * Pure (no-UObject) helper for the /init project-context onboarding feature.
 *
 * The agent's runtime loader (KismetAIAgent.cpp) reads a precedence chain of
 * context files from the project root — first match wins:
 *   KISMET.md, AGENTS.md, CLAUDE.md, GEMINI.md, KISMETAI.md
 *
 * This helper lets the user create or import that file. It can:
 *   - detect which chain file (if any) already exists,
 *   - scan for OTHER-format files written by popular agent tools (Cursor,
 *     Windsurf, Aider, Cline, Roo, Copilot, …) that the chain does NOT read
 *     natively, and
 *   - build an AGENTS.md by importing those, or seed a fresh template.
 *
 * All functions take an explicit project-root directory so they are
 * deterministic and unit-testable (tests pass a temp dir; the bridge passes
 * FPaths::ProjectDir()). No engine globals are touched here.
 */
class KISMETAI_API FKismetAIProjectContext
{
public:
	/** Always the write target — the cross-tool open standard. */
	static const TCHAR* GetTargetFileName() { return TEXT("AGENTS.md"); }

	/** Result of scanning a project root for existing/importable context. */
	struct FScanResult
	{
		/** First chain file that exists at the root (e.g. "AGENTS.md"), or empty. */
		FString ExistingChainFile;

		/** Other-format files/dirs found that AGENTS.md could be imported from.
		 *  Each entry is the relative path as the user would recognise it
		 *  (e.g. ".cursorrules", ".cursor/rules/"). */
		TArray<FString> Importable;
	};

	/**
	 * Scan ProjectRoot for the existing chain file and importable other-format
	 * sources. Does not write anything. Shared by Init and Status.
	 */
	static FScanResult ScanProjectRoot(const FString& ProjectRoot);

	/** Outcome of InitProjectContext. */
	enum class EInitStatus : uint8
	{
		Exists,    // a chain file already existed — nothing written
		Imported,  // built AGENTS.md from other-format sources
		Created,   // wrote the seeded template (nothing else found)
		Failed,    // write failed
	};

	struct FInitResult
	{
		EInitStatus Status = EInitStatus::Failed;
		FString TargetPath;            // absolute path of AGENTS.md (write target)
		TArray<FString> ImportedFrom;  // sources imported (Imported status only)
		FString Message;               // human-readable summary
	};

	/**
	 * Create or import the AGENTS.md context file under ProjectRoot.
	 *
	 * 1. If any chain file already exists -> Exists (no write).
	 * 2. Else if other-format sources are found -> concatenate them into
	 *    AGENTS.md under labeled headers -> Imported.
	 * 3. Else -> write the seeded template -> Created.
	 *
	 * Does file IO; call on the game thread.
	 */
	static FInitResult InitProjectContext(const FString& ProjectRoot);

	/** The seeded AGENTS.md template (used when nothing else is found). */
	static FString BuildTemplate();

	// --- Interactive import panel (/import) ---

	/** One detected other-tool config the user can choose to import. */
	struct FDetectedSource
	{
		FString Id;     // stable identifier == user-facing relative path (e.g. "CLAUDE.md", ".cursor/rules/")
		FString Label;  // friendly tool name (e.g. "Claude", "Cursor")
		bool bIsDir = false;
	};

	/**
	 * Detect every importable other-tool config present under ProjectRoot
	 * (chain-format files like CLAUDE.md plus dedicated rule files/dirs).
	 * Kismet's own KISMET.md / AGENTS.md are never listed. No writes.
	 */
	static TArray<FDetectedSource> DetectSources(const FString& ProjectRoot);

	/**
	 * Import only the sources whose ids are in SelectedIds. Appends to an
	 * existing KISMET.md / AGENTS.md, or creates AGENTS.md. Does file IO;
	 * call on the game thread.
	 */
	static FInitResult ImportSelected(const FString& ProjectRoot, const TArray<FString>& SelectedIds);

	/** Kismet's own context file that already exists (KISMET.md/AGENTS.md), or empty. */
	static FString FindExistingTarget(const FString& ProjectRoot);

	/** Serialize the import-panel payload {target, existing_target, onboarding_dismissed, sources:[…]}. */
	static FString SourcesToJson(const TArray<FDetectedSource>& Sources,
		const FString& ExistingTarget, bool bOnboardingDismissed);

	/** Serialize an FInitResult as the JSON returned by the bridge. */
	static FString InitResultToJson(const FInitResult& Result);

	/** Serialize a status payload for GetProjectContextStatus(). */
	static FString StatusToJson(const FScanResult& Scan, bool bOnboardingDismissed);

private:
	/** Read a single other-format file and label it for the AGENTS.md import. */
	static bool TryImportSingleFile(const FString& ProjectRoot, const FString& RelPath,
		const FString& ToolLabel, FString& OutSection);

	/** Concatenate every *.md / *.mdc inside a rules directory, labeled. */
	static bool TryImportRulesDir(const FString& ProjectRoot, const FString& RelDir,
		const FString& ToolLabel, FString& OutSection);
};
