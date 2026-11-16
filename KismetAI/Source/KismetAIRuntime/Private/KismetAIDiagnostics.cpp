// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#include "KismetAIDiagnostics.h"
#include "KismetAIToolRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_MAC || PLATFORM_LINUX
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace KismetAIDiagnosticsInternal
{
	constexpr int64 SafeLimit = 8 * 1024 * 1024;
	constexpr int64 RawLimit = 1024 * 1024;
	FCriticalSection Lock;
	uint64 Sequence = 0;

	bool Enabled(const TCHAR* Name)
	{
		const FString Value = FPlatformMisc::GetEnvironmentVariable(Name);
		return Value == TEXT("1") || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
	}

	FString Root()
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_DIAGNOSTICS_ROOT"));
		return FPaths::ConvertRelativePathToFull(Override.IsEmpty()
			? FPaths::ProjectSavedDir() / TEXT("Logs/KismetAI-diagnostics") : Override);
	}

	FString Name(bool bRaw)
	{
		return FString::Printf(TEXT("%s-%u.jsonl"), bRaw ? TEXT("raw") : TEXT("safe"), FPlatformProcess::GetCurrentProcessId());
	}

	bool Identity(const FString& Value)
	{
		FGuid Id;
		if (Value.Len() < 36 || !FGuid::ParseExact(Value.Left(36), EGuidFormats::DigitsWithHyphens, Id)) return false;
		FString Tail = Value.Mid(36);
		while (!Tail.IsEmpty())
		{
			if (!Tail.StartsWith(TEXT("/")) || Tail.Len() < 2) return false;
			Tail.RightChopInline(1);
			const TCHAR Kind = Tail[0];
			if (Kind != 't' && Kind != 'c' && Kind != 'r' && Kind != 'a' && Kind != 's' && Kind != 'm') return false;
			Tail.RightChopInline(1);
			int32 Digits = 0;
			while (Digits < Tail.Len() && Tail[Digits] >= '0' && Tail[Digits] <= '9') ++Digits;
			if (Kind != 'm' && Digits == 0) return false;
			Tail.RightChopInline(Digits);
		}
		return Value.Len() <= 180;
	}

	TSharedPtr<FJsonObject> Project(const TSharedPtr<FJsonObject>& Input, int32 Depth)
	{
		auto Out = MakeShared<FJsonObject>();
		if (!Input.IsValid() || Depth > 8) return Out;
		static const TSet<FString> Ids = {TEXT("run_id"), TEXT("client_run_id"), TEXT("turn_id"), TEXT("call_id"), TEXT("request_id"), TEXT("attempt_id"), TEXT("message_id"), TEXT("uuid"), TEXT("summary_request_id"), TEXT("replaces")};
		static const TSet<FString> Numbers = {TEXT("schema"), TEXT("attempt"), TEXT("idx"), TEXT("call_index"), TEXT("call_count"), TEXT("duration_ms"), TEXT("http_status"), TEXT("input_tokens"), TEXT("output_tokens"), TEXT("cache_read_tokens"), TEXT("cache_creation_tokens"), TEXT("cache_read_input_tokens"), TEXT("cache_creation_input_tokens"), TEXT("total_tokens"), TEXT("reasoning_tokens"), TEXT("num_steps"), TEXT("num_tool_calls"), TEXT("steps"), TEXT("tokens"), TEXT("result_bytes"), TEXT("compile_error_count"), TEXT("messages_before"), TEXT("messages_after"), TEXT("dropped_groups"), TEXT("dropped_tool_calls"), TEXT("retained_user_messages"), TEXT("estimated_tokens_after"), TEXT("tokens_before"), TEXT("tokens_after"), TEXT("effective_limit"), TEXT("context_window"), TEXT("output_reserve"), TEXT("last_reported_input_tokens"), TEXT("elapsed_seconds"), TEXT("model_requests"), TEXT("transport_requests"), TEXT("tool_calls"), TEXT("output_bytes"), TEXT("estimated_spend_usd"), TEXT("last_failed_model_http_status"), TEXT("transcript_messages")};
		static const TSet<FString> Bools = {TEXT("journal_committed"), TEXT("success"), TEXT("will_retry"), TEXT("usage_complete"), TEXT("output_complete"), TEXT("is_mutation"), TEXT("attempted"), TEXT("handler_not_started"), TEXT("cancel_requested_while_running"), TEXT("applied_in_memory"), TEXT("package_was_dirty_before"), TEXT("save_included_preexisting_changes"), TEXT("retry_safe"), TEXT("persistence_complete"), TEXT("summarized")};
		static const TSet<FString> Objects = {TEXT("payload"), TEXT("mutation_outcome"), TEXT("run_budget"), TEXT("usage"), TEXT("result"), TEXT("estimate"), TEXT("summary"), TEXT("projection"), TEXT("completion_summary")};
		static const TSet<FString> Labels = {TEXT("type"), TEXT("record"), TEXT("status"), TEXT("outcome"), TEXT("phase"), TEXT("role"), TEXT("stop_reason"), TEXT("finish_reason"), TEXT("compile_status"), TEXT("save_status"), TEXT("recovery_status"), TEXT("usage_provenance"), TEXT("usage_scope"), TEXT("usage_basis"), TEXT("spend_basis"), TEXT("source"), TEXT("op"), TEXT("level"), TEXT("failure_code"), TEXT("error_code")};
		static const TSet<FString> Values = {
			TEXT("user_message"), TEXT("assistant_message"), TEXT("tool_result_message"), TEXT("tool_call"), TEXT("model_attempt"), TEXT("context_compaction"), TEXT("session_meta"), TEXT("session_header"), TEXT("mutation_intent"), TEXT("mutation_reconciliation"), TEXT("run_budget"),
			TEXT("attempt_start"), TEXT("attempt_end"), TEXT("done"), TEXT("error"), TEXT("plugin_loaded"), TEXT("plugin_unloaded"), TEXT("api_error"), TEXT("log_warning"), TEXT("log_error"), TEXT("compile_error"), TEXT("t3d_inject_error"), TEXT("editor_map_opened"),
			TEXT("completed"), TEXT("complete"), TEXT("failed"), TEXT("cancelled"), TEXT("aborted"), TEXT("interrupted"), TEXT("uncertain"), TEXT("partial"), TEXT("succeeded"), TEXT("short_circuited"), TEXT("not_started"), TEXT("not_run"), TEXT("not_requested"), TEXT("saved"), TEXT("restored"), TEXT("partially_restored"), TEXT("restore_failed"), TEXT("not_tracked"), TEXT("committed"), TEXT("nothing_to_compact"),
			TEXT("preparing"), TEXT("waiting_for_model"), TEXT("executing_tools"), TEXT("committing"), TEXT("projecting_context"), TEXT("waiting_for_user"), TEXT("cancelling"), TEXT("finishing"), TEXT("finished"), TEXT("idle"), TEXT("prepare"), TEXT("model_wait"), TEXT("tools"), TEXT("commit"), TEXT("context_projection"),
			TEXT("end_turn"), TEXT("max_tokens"), TEXT("tool_use"), TEXT("stop"), TEXT("length"), TEXT("max_steps"), TEXT("stop_sequence"), TEXT("pause_turn"), TEXT("refusal"), TEXT("plan_ready"),
			TEXT("reported"), TEXT("provider_reported"), TEXT("estimated"), TEXT("unavailable"), TEXT("attempt"), TEXT("request"), TEXT("user"), TEXT("assistant"), TEXT("tool_result"), TEXT("system"), TEXT("warning"), TEXT("microcompact"), TEXT("summarize"),
			TEXT("context_pressure"), TEXT("prompt_too_long"), TEXT("end_of_turn"), TEXT("resume"), TEXT("authentication_error"), TEXT("rate_limit_error"), TEXT("api_error"), TEXT("overloaded_error"), TEXT("invalid_request_error"), TEXT("network_error"), TEXT("summary_unavailable"), TEXT("summary_timeout"), TEXT("summary_provider_error"), TEXT("summary_prompt_too_long"), TEXT("summary_truncated"), TEXT("summary_empty"), TEXT("summary_tool_only"), TEXT("context_would_not_fit"), TEXT("intent_check_failed"), TEXT("session_journal_write_failed"), TEXT("unresolved_operation_evidence"), TEXT("run_budget_exhausted"), TEXT("run_budget_unavailable")};
		for (const auto& Pair : Input->Values)
		{
			const FString Key(Pair.Key);
			const auto& Value = Pair.Value;
			if (!Value.IsValid()) continue;
			if (Value->Type == EJson::Number && Numbers.Contains(Key) && FMath::IsFinite(Value->AsNumber())) Out->SetNumberField(Key, Value->AsNumber());
			else if (Value->Type == EJson::Boolean && Bools.Contains(Key)) Out->SetBoolField(Key, Value->AsBool());
			else if (Value->Type == EJson::String)
			{
				const FString Text = Value->AsString();
				if ((Ids.Contains(Key) && Identity(Text)) || (Labels.Contains(Key) && Values.Contains(Text))
					|| ((Key == TEXT("tool") || Key == TEXT("tool_name")) && FKismetAIToolRegistry::Find(Text))) Out->SetStringField(Key, Text);
			}
			else if (Value->Type == EJson::Object && Objects.Contains(Key)) Out->SetObjectField(Key, Project(Value->AsObject(), Depth + 1));
		}
		return Out;
	}

	FString Json(const TSharedPtr<FJsonObject>& Object)
	{
		FString Result;
		FJsonSerializer::Serialize(Object.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Result));
		return Result + TEXT("\n");
	}

	bool OwnedName(const FString& File, bool& bRaw)
	{
		bRaw = File.StartsWith(TEXT("raw-"));
		if (!bRaw && !File.StartsWith(TEXT("safe-"))) return false;
		FString Pid = File.Mid(bRaw ? 4 : 5);
		if (Pid.EndsWith(TEXT(".previous.jsonl")) && !bRaw) Pid.LeftChopInline(15);
		else if (Pid.EndsWith(TEXT(".jsonl"))) Pid.LeftChopInline(6);
		else return false;
		return !Pid.IsEmpty() && Pid.Len() <= 10 && Pid.IsNumeric();
	}

#if PLATFORM_MAC || PLATFORM_LINUX
	int OpenRoot(const FString& Directory)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Directory), true);
		if (mkdir(TCHAR_TO_UTF8(*Directory), 0700) != 0 && errno != EEXIST) return -1;
		const int Fd = open(TCHAR_TO_UTF8(*Directory), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		struct stat Info;
		if (Fd < 0) return -1;
		if (fstat(Fd, &Info) != 0 || Info.st_uid != geteuid() || !S_ISDIR(Info.st_mode) || fchmod(Fd, 0700) != 0)
		{
			close(Fd);
			return -1;
		}
		return Fd;
	}

	bool RegularOwned(int RootFd, const FString& File, struct stat& Info)
	{
		return fstatat(RootFd, TCHAR_TO_UTF8(*File), &Info, AT_SYMLINK_NOFOLLOW) == 0
			&& S_ISREG(Info.st_mode) && Info.st_uid == geteuid() && Info.st_nlink == 1;
	}

	void Expire(int RootFd, const FString& Directory)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.jsonl")), true, false);
		for (const FString& File : Files)
		{
			bool bRaw;
			struct stat Info;
			if (!OwnedName(File, bRaw) || !RegularOwned(RootFd, File, Info)) continue;
			const int64 Age = FDateTime::UtcNow().ToUnixTimestamp() - Info.st_mtime;
			if (Age > (bRaw ? 86400 : 7 * 86400)) unlinkat(RootFd, TCHAR_TO_UTF8(*File), 0);
		}
	}

	void Write(const FString& Text, bool bRaw)
	{
		const FTCHARToUTF8 Bytes(*Text);
		const int64 Limit = bRaw ? RawLimit : SafeLimit;
		if (Bytes.Length() > Limit) return;
		const FString Directory = Root();
		const int RootFd = OpenRoot(Directory);
		if (RootFd < 0) return;
		Expire(RootFd, Directory);
		const FString File = Name(bRaw);
		struct stat Info;
		if (!bRaw && RegularOwned(RootFd, File, Info) && Info.st_size + Bytes.Length() > Limit)
		{
			const FString Previous = File.LeftChop(6) + TEXT(".previous.jsonl");
			// rename replaces the directory entry, never follows an old target symlink.
			if (renameat(RootFd, TCHAR_TO_UTF8(*File), RootFd, TCHAR_TO_UTF8(*Previous)) != 0) { close(RootFd); return; }
		}
		const int Fd = openat(RootFd, TCHAR_TO_UTF8(*File), O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK | (bRaw ? 0 : O_APPEND), 0600);
		if (Fd >= 0)
		{
			if (fstat(Fd, &Info) == 0 && S_ISREG(Info.st_mode) && Info.st_uid == geteuid() && Info.st_nlink == 1
				&& fchmod(Fd, 0600) == 0 && (!bRaw || ftruncate(Fd, 0) == 0))
			{
				int32 Offset = 0;
				while (Offset < Bytes.Length())
				{
					const ssize_t Count = write(Fd, Bytes.Get() + Offset, Bytes.Length() - Offset);
					if (Count <= 0) break;
					Offset += static_cast<int32>(Count);
				}
			}
			close(Fd);
		}
		close(RootFd);
	}
#else
	void ExpirePortable(const FString& Directory)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.jsonl")), true, false);
		for (const FString& File : Files)
		{
			bool bRaw;
			if (!OwnedName(File, bRaw)) continue;
			const FString Path = Directory / File;
			const FTimespan Age = FDateTime::UtcNow() - IFileManager::Get().GetTimeStamp(*Path);
			if (Age > FTimespan::FromDays(bRaw ? 1 : 7)) IFileManager::Get().Delete(*Path, false, false, true);
		}
	}

	void Write(const FString& Text, bool bRaw)
	{
		// Raw capture requires the owner-only creation contract above.
		if (bRaw) return;
		const FString Directory = Root();
		IFileManager::Get().MakeDirectory(*Directory, true);
		ExpirePortable(Directory);
		const FString File = Directory / Name(false);
		if (IFileManager::Get().FileSize(*File) + Text.Len() * sizeof(TCHAR) > SafeLimit)
			IFileManager::Get().Move(*(File.LeftChop(6) + TEXT(".previous.jsonl")), *File, true, true);
		FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
	}
#endif

	void MaintainExisting()
	{
		// Opt-out prevents new captures, not expiry of already-retained secrets.
		// Do not create directories merely because a disabled reporter was called.
		const FString Directory = Root();
#if PLATFORM_MAC || PLATFORM_LINUX
		const int RootFd = open(TCHAR_TO_UTF8(*Directory), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
		if (RootFd < 0) return;
		struct stat Info;
		if (fstat(RootFd, &Info) == 0 && S_ISDIR(Info.st_mode) && Info.st_uid == geteuid()) Expire(RootFd, Directory);
		close(RootFd);
#else
		if (IFileManager::Get().DirectoryExists(*Directory)) ExpirePortable(Directory);
#endif
	}
}

TSharedPtr<FJsonObject> KismetAIDiagnostics::SafeProjection(const TSharedPtr<FJsonObject>& Payload)
{
	return KismetAIDiagnosticsInternal::Project(Payload, 0);
}

FString KismetAIDiagnostics::GetSafeLogPath()
{
	return KismetAIDiagnosticsInternal::Root() / KismetAIDiagnosticsInternal::Name(false);
}

void KismetAIDiagnostics::Record(const FString& Event, const TSharedPtr<FJsonObject>& Payload)
{
	using namespace KismetAIDiagnosticsInternal;
	FScopeLock Guard(&Lock);
	MaintainExisting();
	if (Enabled(TEXT("KISMET_DIAGNOSTICS_DISABLE"))) return;
	static const TSet<FString> Events = {TEXT("journal_append"), TEXT("attempt_start"), TEXT("attempt_end"), TEXT("legacy_telemetry"), TEXT("compaction")};
	auto Entry = MakeShared<FJsonObject>();
	Entry->SetNumberField(TEXT("schema"), 1);
	Entry->SetNumberField(TEXT("sequence"), ++Sequence);
	Entry->SetStringField(TEXT("ts"), FDateTime::UtcNow().ToIso8601());
	Entry->SetStringField(TEXT("event"), Events.Contains(Event) ? Event : TEXT("other"));
	Entry->SetStringField(TEXT("evidence"), TEXT("diagnostic_only"));
	Entry->SetStringField(TEXT("completeness"), TEXT("not_guaranteed"));
	Entry->SetObjectField(TEXT("payload"), SafeProjection(Payload));
	Write(Json(Entry), false);
}

void KismetAIDiagnostics::CapturePrompt(const FString& RunId, const FString& Prompt)
{
	using namespace KismetAIDiagnosticsInternal;
	FScopeLock Guard(&Lock);
	MaintainExisting();
	if (Enabled(TEXT("KISMET_DIAGNOSTICS_DISABLE")) || !Enabled(TEXT("KISMET_DIAGNOSTICS_RAW_PROMPTS"))) return;
	auto Entry = MakeShared<FJsonObject>();
	Entry->SetNumberField(TEXT("schema"), 1);
	Entry->SetStringField(TEXT("event"), TEXT("raw_prompt"));
	Entry->SetStringField(TEXT("ts"), FDateTime::UtcNow().ToIso8601());
	if (Identity(RunId)) Entry->SetStringField(TEXT("run_id"), RunId);
	Entry->SetStringField(TEXT("prompt"), Prompt);
	Write(Json(Entry), true);
}
