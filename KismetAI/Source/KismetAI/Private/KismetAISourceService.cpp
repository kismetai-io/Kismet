// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAISourceService.h"
#include "KismetAISourceSearch.h"
#include "KismetAISourceContract.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace ReadSourceHelpers
{

// Content rule only (extension). Confinement lives in KismetAISourceConfinement and is
// applied to the OS-resolved target of the open handle, not to this lexical string.
bool HasAllowedExtension(const FString& Path)
{
	return KismetAISourceConfinement::HasAllowedSourceExtension(Path);
}

// Picks the lexical candidate a request names. This is only path *selection*; whether the
// candidate may be read is decided afterwards by KismetAISourceConfinement::ReadConfined.
// Relative paths resolve against the selected scope's directories ("project" when scope is
// omitted, as before).
FString ResolvePath(const FString& File, const FString& Scope, const FKismetAISourceRoots& Roots)
{
	auto Existing = [](const FString& Path) -> FString
	{
		const FString Full = FPaths::ConvertRelativePathToFull(Path);
		return (FPaths::FileExists(Full) && HasAllowedExtension(Full)) ? Full : FString();
	};

	if (!FPaths::IsRelative(File))
	{
		return Existing(File);
	}

	// Accept both "Module/Foo.h" (relative to Source/) and the "Source/Module/Foo.h" form
	// that find mode emits (relative to the root), so a find result round-trips.
	TArray<FString> Candidates;
	if (Scope == TEXT("project") || Scope.IsEmpty())
	{
		Candidates.Add(Roots.ProjectDir / TEXT("Source") / File);
		Candidates.Add(Roots.ProjectDir / File);
	}
	if (Scope == TEXT("engine") || Scope.IsEmpty())
	{
		Candidates.Add(Roots.EngineDir / TEXT("Source") / File);
		Candidates.Add(Roots.EngineDir / File);
	}
	if (Scope == TEXT("plugins") || Scope.IsEmpty())
	{
		Candidates.Add(Roots.EngineDir / TEXT("Plugins") / File);
		Candidates.Add(Roots.ProjectDir / TEXT("Plugins") / File);
	}
	for (const FString& Candidate : Candidates)
	{
		const FString Found = Existing(Candidate);
		if (!Found.IsEmpty())
		{
			return Found;
		}
	}
	return FString();
}


struct FOutlineEntry
{
	int32 LineNumber;
	FString Comment;
	FString Macro;       // UFUNCTION(...), UPROPERTY(...), UCLASS(...), etc.
	FString Declaration; // Function signature or property declaration
	FString Kind;        // "property", "function", "class", "struct", "enum", "delegate"
};

void ExtractOutline(const TArray<FString>& Lines, TArray<FOutlineEntry>& OutEntries, const FString& Query,
	const FKismetAICancellationToken& Token, double Deadline, bool& bPartial)
{
	FString QueryLower = Query.ToLower();
	int32 NumLines = Lines.Num();

	auto CaptureCommentBlock = [&](int32 MacroLine) -> FString
	{
		// Walk backwards from the macro line to find preceding /** */ comment
		int32 CommentEnd = MacroLine - 1;
		while (CommentEnd >= 0 && Lines[CommentEnd].TrimStartAndEnd().IsEmpty())
		{
			CommentEnd--;
		}
		if (CommentEnd < 0 || !Lines[CommentEnd].TrimStartAndEnd().EndsWith(TEXT("*/")))
		{
			return FString();
		}

		int32 CommentStart = CommentEnd;
		while (CommentStart > FMath::Max(0, CommentEnd - 64))
		{
			FString Trimmed = Lines[CommentStart].TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("/**")) || Trimmed.StartsWith(TEXT("/*")))
			{
				break;
			}
			CommentStart--;
		}

		FString Comment;
		for (int32 i = CommentStart; i <= CommentEnd; i++)
		{
			if (!Comment.IsEmpty()) Comment += TEXT("\n");
			Comment += Lines[i].Left(4096);
			if (Comment.Len() > 4096) { bPartial = true; Comment.LeftInline(4096); break; }
		}
		return Comment;
	};

	auto ReadForwardUntilSemicolon = [&](int32 StartLine, int32 MaxLines = 10) -> FString
	{
		FString Accumulated;
		int32 BraceDepth = 0;
		for (int32 i = StartLine; i < FMath::Min(StartLine + MaxLines, NumLines); i++)
		{
			if (Lines[i].Len() > 4096) bPartial = true;
			FString Trimmed = Lines[i].Left(4096).TrimStartAndEnd();
			if (!Accumulated.IsEmpty()) Accumulated += TEXT(" ");
			Accumulated += Trimmed.Left(4096);
			if (Accumulated.Len() > 4096) { bPartial = true; Accumulated.LeftInline(4096); break; }

			for (TCHAR Ch : Trimmed)
			{
				if (Ch == TEXT('{')) BraceDepth++;
				else if (Ch == TEXT('}')) BraceDepth--;
			}

			if (Trimmed.EndsWith(TEXT(";")) || BraceDepth > 0)
			{
				if (BraceDepth > 0)
				{
					// Function with body — just take up to the opening brace
					int32 BracePos;
					if (Accumulated.FindChar(TEXT('{'), BracePos))
					{
						Accumulated = Accumulated.Left(BracePos).TrimEnd();
					}
				}
				break;
			}
		}
		return Accumulated;
	};

	for (int32 i = 0; i < NumLines; i++)
	{
		if (Token.IsCancelled() || FPlatformTime::Seconds() >= Deadline || OutEntries.Num() >= 500)
		{ bPartial = true; break; }
		if (Lines[i].Len() > 4096) bPartial = true;
		FString Trimmed = Lines[i].Left(4096).TrimStartAndEnd();

		FOutlineEntry Entry;
		Entry.LineNumber = i + 1; // 1-based

		bool bMatch = false;

		// UCLASS / USTRUCT / UENUM
		if (Trimmed.StartsWith(TEXT("UCLASS(")) || Trimmed.StartsWith(TEXT("USTRUCT(")) || Trimmed.StartsWith(TEXT("UENUM(")))
		{
			Entry.Macro = ReadForwardUntilSemicolon(i, 3);
			// Read the class/struct/enum declaration line(s) after the macro
			int32 DeclLine = i + 1;
			while (DeclLine < NumLines)
			{
				FString DT = Lines[DeclLine].TrimStartAndEnd();
				if (DT.Contains(TEXT("class ")) || DT.Contains(TEXT("struct ")) || DT.Contains(TEXT("enum ")))
				{
					break;
				}
				if (DT.StartsWith(TEXT("UCLASS")) || DT.StartsWith(TEXT("USTRUCT")) || DT.StartsWith(TEXT("UENUM")))
				{
					DeclLine++;
					continue;
				}
				break;
			}
			if (DeclLine < NumLines)
			{
				Entry.Declaration = Lines[DeclLine].Left(4096).TrimStartAndEnd();
				if (Entry.Declaration.EndsWith(TEXT("{")))
				{
					Entry.Declaration = Entry.Declaration.LeftChop(1).TrimEnd();
				}
			}
			Entry.Kind = Trimmed.StartsWith(TEXT("UCLASS")) ? TEXT("class")
				: Trimmed.StartsWith(TEXT("USTRUCT")) ? TEXT("struct") : TEXT("enum");
			Entry.Comment = CaptureCommentBlock(i);
			bMatch = true;
		}
		// UFUNCTION
		else if (Trimmed.StartsWith(TEXT("UFUNCTION(")))
		{
			Entry.Macro = Trimmed;
			if (!Trimmed.Contains(TEXT(")")))
			{
				int32 j = i + 1;
				while (j < FMath::Min(i + 5, NumLines))
				{
					Entry.Macro += TEXT(" ") + Lines[j].Left(4096).TrimStartAndEnd();
					if (Lines[j].Contains(TEXT(")"))) break;
					j++;
				}
			}
			int32 FuncLine = i + 1;
			while (FuncLine < FMath::Min(i + 5, NumLines))
			{
				FString FL = Lines[FuncLine].TrimStartAndEnd();
				if (!FL.IsEmpty() && !FL.StartsWith(TEXT("UFUNCTION")) && FL != TEXT(")"))
				{
					break;
				}
				FuncLine++;
			}
			if (FuncLine < NumLines)
			{
				Entry.Declaration = ReadForwardUntilSemicolon(FuncLine, 5);
			}
			Entry.Kind = TEXT("function");
			Entry.Comment = CaptureCommentBlock(i);
			bMatch = true;
		}
		// UPROPERTY
		else if (Trimmed.StartsWith(TEXT("UPROPERTY(")))
		{
			Entry.Macro = Trimmed;
			if (!Trimmed.Contains(TEXT(")")))
			{
				int32 j = i + 1;
				while (j < FMath::Min(i + 5, NumLines))
				{
					Entry.Macro += TEXT(" ") + Lines[j].Left(4096).TrimStartAndEnd();
					if (Lines[j].Contains(TEXT(")"))) break;
					j++;
				}
			}
			int32 PropLine = i + 1;
			while (PropLine < FMath::Min(i + 5, NumLines))
			{
				FString PL = Lines[PropLine].TrimStartAndEnd();
				if (!PL.IsEmpty() && !PL.StartsWith(TEXT("UPROPERTY")) && PL != TEXT(")"))
				{
					break;
				}
				PropLine++;
			}
			if (PropLine < NumLines)
			{
				Entry.Declaration = ReadForwardUntilSemicolon(PropLine, 3);
			}
			Entry.Kind = TEXT("property");
			Entry.Comment = CaptureCommentBlock(i);
			bMatch = true;
		}
		// DECLARE_*DELEGATE*
		else if (Trimmed.StartsWith(TEXT("DECLARE_")) && Trimmed.Contains(TEXT("DELEGATE")))
		{
			Entry.Macro = ReadForwardUntilSemicolon(i, 5);
			Entry.Declaration = Entry.Macro;
			Entry.Kind = TEXT("delegate");
			Entry.Comment = CaptureCommentBlock(i);
			bMatch = true;
		}

		if (bMatch)
		{
			if (!QueryLower.IsEmpty())
			{
				FString SearchText = (Entry.Comment + Entry.Macro + Entry.Declaration).ToLower();
				if (!SearchText.Contains(QueryLower))
				{
					continue;
				}
			}
			OutEntries.Add(MoveTemp(Entry));
		}
	}
}

FString FormatOutline(const TArray<FOutlineEntry>& Entries, const FString& FilePath, int32 TotalLines, const FString& Query, bool& bPartial)
{
	FString Output;

	Output += FString::Printf(TEXT("// %s (%d lines)\n"), *FPaths::GetCleanFilename(FilePath), TotalLines);

	for (const FOutlineEntry& E : Entries)
	{
		if (E.Kind == TEXT("class") || E.Kind == TEXT("struct") || E.Kind == TEXT("enum"))
		{
			const FString Declaration = FString::Printf(TEXT("// %s\n"), *E.Declaration);
			if (Output.Len() + Declaration.Len() > KismetAISourceContract::MaxOutputBytes / 4)
			{ bPartial = true; continue; }
			Output += Declaration;
		}
	}

	if (!Query.IsEmpty())
	{
		int32 TotalDecls = 0;
		for (const FOutlineEntry& E : Entries)
		{
			if (E.Kind != TEXT("class") && E.Kind != TEXT("struct") && E.Kind != TEXT("enum"))
			{
				TotalDecls++;
			}
		}
		Output += FString::Printf(TEXT("// Filtered by \"%s\" (%d matches)\n"), *Query, TotalDecls);
	}

	auto AppendSection = [&](const FString& Title, const FString& Kind)
	{
		TArray<const FOutlineEntry*> SectionEntries;
		for (const FOutlineEntry& E : Entries)
		{
			if (E.Kind == Kind) SectionEntries.Add(&E);
		}
		if (SectionEntries.Num() == 0) return;

		Output += FString::Printf(TEXT("\n=== %s (%d) ===\n\n"), *Title, SectionEntries.Num());
		for (const FOutlineEntry* E : SectionEntries)
		{
			if (Output.Len() + E->Comment.Len() + E->Macro.Len() + E->Declaration.Len() + 100 > KismetAISourceContract::MaxOutputBytes / 4) { bPartial = true; return; }
			if (!E->Comment.IsEmpty())
			{
				Output += E->Comment + TEXT("\n");
			}
			Output += FString::Printf(TEXT("[L%d] %s\n"), E->LineNumber, *E->Macro);
			if (!E->Declaration.IsEmpty() && E->Declaration != E->Macro)
			{
				Output += E->Declaration + TEXT("\n");
			}
			Output += TEXT("\n");
		}
	};

	AppendSection(TEXT("PROPERTIES"), TEXT("property"));
	AppendSection(TEXT("FUNCTIONS"), TEXT("function"));
	AppendSection(TEXT("DELEGATES"), TEXT("delegate"));
	AppendSection(TEXT("ENUMS"), TEXT("enum"));

	return Output;
}

} // namespace ReadSourceHelpers

struct FSourceRootMetrics
{
	int32 ResolutionAttempts = 0;
	double ElapsedMs = 0;
};

FToolExecutionResult ExecuteBody(const TSharedPtr<FJsonObject>& Args,
	const FKismetAISourceSnapshot& Snapshot, const FKismetAICancellationToken& Token, FSourceRootMetrics& RootMetrics)
{
	const double Started = FPlatformTime::Seconds();
	TSharedPtr<FJsonObject> Body = Args;
	if (!Body.IsValid())
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_JSON"), TEXT("Could not parse request body"), {});
	}

	FString Mode = Body->HasField(TEXT("mode")) ? Body->GetStringField(TEXT("mode")) : TEXT("outline");
	FString Scope = Body->HasField(TEXT("scope")) ? Body->GetStringField(TEXT("scope")) : TEXT("project");

	FString File;
	Body->TryGetStringField(TEXT("file"), File);

	// Selected scope decides which roots may be read. An explicit scope confines the read
	// to that scope's roots (project-only never inherits engine permission); an omitted
	// scope resolves relative paths against the project, as before, and admits any
	// permitted root for absolute paths.
	EKismetAISourceScope SelectedScope = EKismetAISourceScope::Any;
	if (Body->HasField(TEXT("scope")) && !KismetAISourceConfinement::TryParseScope(Scope, SelectedScope))
	{
		return FToolExecutionResult::Error(400, TEXT("INVALID_SCOPE"),
			FString::Printf(TEXT("Unknown scope '%s'"), *Scope),
			{TEXT("Use scope \"project\", \"engine\" or \"plugins\", or omit it")});
	}

	if (Mode == TEXT("find"))
	{
		FString Pattern;
		Body->TryGetStringField(TEXT("pattern"), Pattern);
		const FString Error = KismetAISourceContract::PatternError(Pattern);
		if (!Error.IsEmpty()) return FToolExecutionResult::Error(400, TEXT("UNSUPPORTED_PATTERN"), Error);
	}
	if ((SelectedScope == EKismetAISourceScope::Any || SelectedScope == EKismetAISourceScope::Plugins) &&
		Snapshot.DiscoveredPluginDirs.Num() > KismetAISourceContract::MaxDiscoveredPluginDirs)
		return FToolExecutionResult::Error(413, TEXT("SOURCE_ROOT_LIMIT"), TEXT("Too many discovered plugins for source discovery."));
	const FKismetAISourceRoots& Roots = Snapshot.Roots;
	RootMetrics.ResolutionAttempts = SelectedScope == EKismetAISourceScope::Any ? 4 : SelectedScope == EKismetAISourceScope::Plugins ? 2 : 1;
	TArray<FString> PermittedRoots = KismetAISourceConfinement::GetPermittedRoots(Roots, SelectedScope);
	if (SelectedScope == EKismetAISourceScope::Any || SelectedScope == EKismetAISourceScope::Plugins)
	{
		for (const FString& PluginDir : Snapshot.DiscoveredPluginDirs)
		{
			if (Token.IsCancelled()) return FToolExecutionResult::Error(409, TEXT("CANCELLED"), TEXT("Source request cancelled."));
			if (FPlatformTime::Seconds() - Started >= KismetAISourceContract::PageSeconds)
				return FToolExecutionResult::Error(408, TEXT("SOURCE_ROOT_BUDGET"), TEXT("Source root resolution exceeded its cooperative budget; choose a narrower scope."));
			++RootMetrics.ResolutionAttempts;
			const FString Canonical = KismetAISourceConfinement::CanonicalizeExisting(PluginDir);
			bool bCovered = Canonical.IsEmpty();
			for (const FString& Root : PermittedRoots)
				bCovered |= Root == Canonical || KismetAISourceConfinement::IsWithinRoot(Canonical, Root);
			if (!bCovered) PermittedRoots.Add(Canonical);
		}
	}
	RootMetrics.ElapsedMs = (FPlatformTime::Seconds() - Started) * 1000;
	if (Mode == TEXT("find")) return KismetAISourceSearch::Find(Body, PermittedRoots, Token, Started + KismetAISourceContract::PageSeconds);
	if (Mode != TEXT("raw") && Mode != TEXT("outline"))
		return FToolExecutionResult::Error(400, TEXT("INVALID_MODE"), TEXT("Use find, raw or outline."));
	const FString Candidate = ReadSourceHelpers::ResolvePath(File, Scope, Roots);
	if (Candidate.IsEmpty()) return FToolExecutionResult::Error(404, TEXT("FILE_NOT_FOUND"), TEXT("Source file not found."));
	TArray<uint8> RawBytes;
	FString ResolvedPath;
	FKismetAISourceReadOptions Options;
	Options.Cancellation = &Token;
	Options.DeadlineSeconds = Started + KismetAISourceContract::PageSeconds;
	FKismetAISourceReadMetrics ReadMetrics;
	const EKismetAISourceReadStatus Status = KismetAISourceConfinement::ReadConfined(Candidate, PermittedRoots, RawBytes, ResolvedPath, Options, &ReadMetrics);
	if (Status == EKismetAISourceReadStatus::Cancelled)
		return FToolExecutionResult::Error(409, TEXT("CANCELLED"), TEXT("Source read cancelled."));
	if (Status == EKismetAISourceReadStatus::TooLarge || Status == EKismetAISourceReadStatus::Deadline || Status == EKismetAISourceReadStatus::Changed)
	{
		auto Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("result"), Status == EKismetAISourceReadStatus::Changed
			? TEXT("The source file changed during reading; no complete file is available. Retry the read.")
			: TEXT("Source read stopped at its byte or cooperative time budget; no complete file is available."));
		Data->SetBoolField(TEXT("partial"), true);
		Data->SetBoolField(TEXT("truncated"), true);
		Data->SetStringField(TEXT("stop_reason"), Status == EKismetAISourceReadStatus::TooLarge ? TEXT("read_bytes") : Status == EKismetAISourceReadStatus::Changed ? TEXT("file_changed") : TEXT("deadline"));
		auto Metrics = MakeShared<FJsonObject>();
		Metrics->SetNumberField(TEXT("bytes_read"), ReadMetrics.BytesRead);
		Metrics->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - Started) * 1000);
		Data->SetObjectField(TEXT("metrics"), Metrics);
		return FToolExecutionResult::Success(Data);
	}
	auto FinishRead = [&ReadMetrics, Started](const TSharedPtr<FJsonObject>& Data)
	{
		auto Metrics = MakeShared<FJsonObject>();
		Metrics->SetNumberField(TEXT("directories_visited"), 0);
		Metrics->SetNumberField(TEXT("entries_considered"), 0);
		Metrics->SetNumberField(TEXT("files_considered"), 1);
		Metrics->SetNumberField(TEXT("bytes_read"), ReadMetrics.BytesRead);
		Metrics->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - Started) * 1000);
		Data->SetObjectField(TEXT("metrics"), Metrics);
		return FToolExecutionResult::Success(Data);
	};
	switch (Status)
	{
	case EKismetAISourceReadStatus::Ok:
		break;
	case EKismetAISourceReadStatus::NotFound:
		return FToolExecutionResult::Error(404, TEXT("FILE_NOT_FOUND"),
			FString::Printf(TEXT("Source file not found: %s"), *File),
			{TEXT("Use read_source(mode=\"find\") to search for files first")});
	case EKismetAISourceReadStatus::OutsideRoots:
		return FToolExecutionResult::Error(403, TEXT("ACCESS_DENIED"),
			SelectedScope == EKismetAISourceScope::Any
				? FString(TEXT("File resolves outside the permitted source directories"))
				: FString::Printf(TEXT("File resolves outside the permitted source directories for scope '%s'"), *Scope),
			{TEXT("Read files under <Project>/Source (scope project), Engine/Source (scope engine), or Engine/Plugins and <Project>/Plugins (scope plugins)")});
	case EKismetAISourceReadStatus::BadExtension:
		return FToolExecutionResult::Error(403, TEXT("ACCESS_DENIED"),
			TEXT("File does not resolve to a .h, .cpp, .inl or .cs source file"), {});
	case EKismetAISourceReadStatus::NotRegularFile:
		return FToolExecutionResult::Error(403, TEXT("ACCESS_DENIED"),
			TEXT("Path is not a regular file"), {});
	case EKismetAISourceReadStatus::ReadFailed:
	default:
		return FToolExecutionResult::Error(500, TEXT("READ_FAILED"),
			FString::Printf(TEXT("Failed to read file: %s"), *File), {});
	}

	TArray<FString> Lines;
	KismetAISourceConfinement::BytesToLines(RawBytes, Lines);

	int32 TotalLines = Lines.Num();

	if (Mode == TEXT("raw"))
	{
		int32 Offset = Body->HasField(TEXT("offset")) ? static_cast<int32>(Body->GetNumberField(TEXT("offset"))) : 0;
		int32 Limit = Body->HasField(TEXT("limit")) ? static_cast<int32>(Body->GetNumberField(TEXT("limit"))) : 200;
		Limit = FMath::Clamp(Limit, 1, 500);
		Offset = FMath::Clamp(Offset, 0, FMath::Max(0, TotalLines - 1));

		int32 EndLine = FMath::Min(Offset + Limit, TotalLines);
		const int32 HeaderReserve = FPaths::GetCleanFilename(ResolvedPath).Len() + 100;
		FString Output;

		for (int32 i = Offset; i < EndLine; i++)
		{
			const FString Row = FString::Printf(TEXT("%d: %s\n"), i + 1, *Lines[i]);
			if (HeaderReserve + Output.Len() + Row.Len() > KismetAISourceContract::MaxOutputBytes / 4)
			{
				if (i == Offset) return FToolExecutionResult::Error(413, TEXT("SOURCE_LINE_LIMIT"), TEXT("A source line exceeds the output budget; use outline or a different line range."));
				EndLine = i; break;
			}
			Output += Row;
		}

		Output = FString::Printf(TEXT("// %s (%d lines) — showing lines %d-%d\n\n"),
			*FPaths::GetCleanFilename(ResolvedPath), TotalLines, Offset + 1, EndLine) + Output;
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("result"), Output);
		Data->SetNumberField(TEXT("total_lines"), TotalLines);
		Data->SetNumberField(TEXT("offset"), Offset);
		Data->SetNumberField(TEXT("limit"), Limit);
		Data->SetBoolField(TEXT("truncated"), EndLine < TotalLines);
		Data->SetBoolField(TEXT("partial"), EndLine < TotalLines);
		Data->SetNumberField(TEXT("next_offset"), EndLine);
		Data->SetStringField(TEXT("file_path"), ResolvedPath);
		return FinishRead(Data);
	}

	// OUTLINE MODE
	FString Query = Body->HasField(TEXT("query")) ? Body->GetStringField(TEXT("query")) : FString();

	TArray<ReadSourceHelpers::FOutlineEntry> Entries;
	bool bPartial = false;
	ReadSourceHelpers::ExtractOutline(Lines, Entries, Query, Token, Options.DeadlineSeconds, bPartial);
	if (Token.IsCancelled()) return FToolExecutionResult::Error(409, TEXT("CANCELLED"), TEXT("Source read cancelled."));

	int32 Limit = Body->HasField(TEXT("limit")) ? static_cast<int32>(Body->GetNumberField(TEXT("limit"))) : 200;
	Limit = FMath::Clamp(Limit, 1, 500);
	int32 TotalEntries = Entries.Num();
	if (Entries.Num() > Limit)
	{
		Entries.SetNum(Limit);
	}

	FString Output = ReadSourceHelpers::FormatOutline(Entries, ResolvedPath, TotalLines, Query, bPartial);

	if (TotalEntries > Limit)
	{
		Output += FString::Printf(TEXT("\n... truncated (%d of %d declarations shown). Use 'query' to filter.\n"),
			Limit, TotalEntries);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("result"), Output);
	Data->SetNumberField(TEXT("declarations_considered"), TotalEntries);
	if (!bPartial) Data->SetNumberField(TEXT("total_declarations"), TotalEntries);
	Data->SetBoolField(TEXT("partial"), bPartial || TotalEntries > Limit || Output.Len() >= KismetAISourceContract::MaxOutputBytes / 4 - 100);
	Data->SetBoolField(TEXT("truncated"), Data->GetBoolField(TEXT("partial")));
	Data->SetNumberField(TEXT("total_lines"), TotalLines);
	Data->SetStringField(TEXT("file_path"), ResolvedPath);
	return FinishRead(Data);
}


FKismetAISourceSnapshot KismetAISourceService::Prepare()
{
	check(IsInGameThread());
	FKismetAISourceSnapshot Snapshot;
	Snapshot.Roots = KismetAISourceConfinement::GetActiveRoots();
	if (Snapshot.Roots.bIncludeDiscoveredPlugins)
	{
		for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetDiscoveredPlugins())
		{
			const FString Dir = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
			if (FPaths::IsUnderDirectory(Dir, Snapshot.Roots.EngineDir / TEXT("Plugins")) ||
				FPaths::IsUnderDirectory(Dir, Snapshot.Roots.ProjectDir / TEXT("Plugins")))
				Snapshot.DiscoveredPluginDirs.Add(Dir);
			if (Snapshot.DiscoveredPluginDirs.Num() > KismetAISourceContract::MaxDiscoveredPluginDirs) break;
		}
	}
	Snapshot.Roots.bIncludeDiscoveredPlugins = false;
	return Snapshot;
}

FToolExecutionResult KismetAISourceService::Execute(const TSharedPtr<FJsonObject>& Args,
	const FKismetAISourceSnapshot& Snapshot, const FKismetAICancellationToken& Token)
{
	const double Started = FPlatformTime::Seconds();
	if (Token.IsCancelled()) return FToolExecutionResult::Error(409, TEXT("CANCELLED"), TEXT("Source request cancelled."));
	FSourceRootMetrics RootMetrics;
	FToolExecutionResult Result = ExecuteBody(Args, Snapshot, Token, RootMetrics);
	if (Result.Data.IsValid())
	{
		TSharedPtr<FJsonObject> Metrics;
		const TSharedPtr<FJsonObject>* Existing = nullptr;
		if (Result.Data->TryGetObjectField(TEXT("metrics"), Existing)) Metrics = *Existing;
		else { Metrics = MakeShared<FJsonObject>(); Result.Data->SetObjectField(TEXT("metrics"), Metrics); }
		Metrics->SetNumberField(TEXT("elapsed_ms"), (FPlatformTime::Seconds() - Started) * 1000);
		Metrics->SetNumberField(TEXT("root_resolution_attempts"), RootMetrics.ResolutionAttempts);
		Metrics->SetNumberField(TEXT("root_resolution_ms"), RootMetrics.ElapsedMs);
		FString Text;
		if (Result.Data->TryGetStringField(TEXT("result"), Text)) Metrics->SetNumberField(TEXT("output_text_bytes"), FTCHARToUTF8(*Text).Length());
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			FString Serialized;
			FJsonSerializer::Serialize(Result.Data.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized));
			Metrics->SetNumberField(TEXT("serialized_data_bytes"), FTCHARToUTF8(*Serialized).Length());
			Metrics->SetNumberField(TEXT("result_bytes"), FTCHARToUTF8(*Result.ToEnvelopeString()).Length());
		}
	}
	if (FTCHARToUTF8(*Result.ToEnvelopeString()).Length() > KismetAISourceContract::MaxSerializedResultBytes)
		return FToolExecutionResult::Error(413, TEXT("SOURCE_OUTPUT_LIMIT"), TEXT("The serialized source result exceeds 32 KiB; narrow the request."));
	return Result;
}
