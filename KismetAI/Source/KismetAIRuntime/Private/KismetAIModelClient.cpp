// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelClient.h"
#include "KismetAIModelDiscovery.h"
#include "KismetAIProviderProfile.h"
#include "KismetAIModelCatalog.h"
#include "KismetAIRequestBudget.h"
#include "KismetAILLMProvider.h"
#include "KismetAIAnthropicProvider.h"
#include "KismetAILog.h"
#include "KismetAITransportScratch.h"
#include "KismetAISSEParser.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Internationalization/Regex.h"

namespace
{
	FString ModelJsonToString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
			= TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Output;
	}
}

namespace
{
	TSharedPtr<FJsonObject> SnapshotObject(const TSharedPtr<FJsonObject>& Source)
	{
		if (!Source.IsValid()) return nullptr;
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(Source, Copy);
		return Copy;
	}

	TArray<TSharedPtr<FJsonValue>> SnapshotValues(const TArray<TSharedPtr<FJsonValue>>& Source)
	{
		TSharedPtr<FJsonObject> Holder = MakeShared<FJsonObject>();
		Holder->SetArrayField(TEXT("values"), Source);
		return SnapshotObject(Holder)->GetArrayField(TEXT("values"));
	}

	TArray<FAgentMessage> SnapshotMessages(const TArray<FAgentMessage>& Source)
	{
		TArray<FAgentMessage> Copy = Source;
		for (FAgentMessage& Message : Copy)
		{
			Message.RawJson = SnapshotObject(Message.RawJson);
			for (FAgentContentPart& Part : Message.Parts)
			{
				Part.ToolInput = SnapshotObject(Part.ToolInput);
				Part.RawJson = SnapshotObject(Part.RawJson);
				if (Part.RawValue.IsValid()) Part.RawValue = SnapshotValues({Part.RawValue})[0];
			}
		}
		return Copy;
	}
}

FKismetAIAgentConfig FModelCallRequest::SnapshotConfig(const FKismetAIAgentConfig& InConfig)
{
	FKismetAIAgentConfig Copy = InConfig;
	if (!Copy.bProviderEnvironmentResolved)
	{
		Copy.ProviderId = KismetAIProviders::ResolveProviderId(Copy);
		FString SelectionError;
		if (KismetAIProviders::ValidateSelection(Copy, SelectionError))
		{
			const FKismetAIProviderProfile& Profile = KismetAIProviders::ResolveProfile(Copy);
			Copy.ProviderBaseUrls.Add(Profile.Id, KismetAIProviders::GetBaseUrl(Copy, Profile));
		}
		Copy.ProviderOrganization = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_ORG_ID"));
		Copy.ProviderProject = FPlatformMisc::GetEnvironmentVariable(TEXT("OPENAI_PROJECT_ID"));
		Copy.AdaptiveOutputTokensOverride = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_MAX_OUTPUT_TOKENS"));
		Copy.bProviderEnvironmentResolved = true;
	}
	return Copy;
}

bool FModelCallRequest::ValidatePolicy(const FKismetAIAgentConfig& InConfig, FString& OutError)
{
	OutError = InConfig.RunPolicyError;
	if (!OutError.IsEmpty()) return false;
	if (!KismetAIProviders::ValidateSelection(InConfig, OutError)) return false;
	if (InConfig.OperatingContextLimit < 0 || InConfig.MaxTokens <= 0)
	{
		OutError = TEXT("Operating context must be nonnegative and requested output must be positive.");
		return false;
	}
	int64 Override = 0;
	for (const TCHAR Digit : InConfig.AdaptiveOutputTokensOverride)
	{
		if (Digit < TCHAR('0') || Digit > TCHAR('9') || Override > (MAX_int32 - (Digit - TCHAR('0'))) / 10)
		{
			OutError = TEXT("KISMETAI_MAX_OUTPUT_TOKENS must be a positive integer; it controls adaptive next turns only.");
			return false;
		}
		Override = Override * 10 + Digit - TCHAR('0');
	}
	if (!InConfig.AdaptiveOutputTokensOverride.IsEmpty() && Override == 0)
	{
		OutError = TEXT("KISMETAI_MAX_OUTPUT_TOKENS must be positive.");
		return false;
	}
	const FKismetAIModelEntry& Entry = FKismetAIModelCatalog::Get().FindOrDefaults(InConfig.Model);
	if (InConfig.ThinkingMode == EThinkingMode::Enabled
		&& (KismetAIProviders::ResolveProfile(InConfig).Protocol != EKismetAIWireProtocol::AnthropicMessages
			|| !Entry.Caps.bThinking || !Entry.Caps.bBudgetThinking || Entry.Caps.bReasoning
			|| InConfig.ThinkingBudgetTokens <= 0))
	{
		OutError = TEXT("Explicit budget thinking requires a supporting Anthropic protocol/model and a positive thinking budget. Choose adaptive or disabled thinking otherwise.");
		return false;
	}
	const int64 Largest = FMath::Max<int64>(InConfig.MaxTokens, Override);
	if ((Entry.MaxOutputTokens > 0 && Largest > Entry.MaxOutputTokens)
		|| (InConfig.OperatingContextLimit > 0 && Largest >= InConfig.OperatingContextLimit))
	{
		OutError = TEXT("Requested output exceeds the model output metadata or leaves no input within the configured operating context.");
		return false;
	}
	return true;
}

bool FModelCallRequest::Validate(FString& OutError) const
{
	if (!ValidatePolicy(Config, OutError)) return false;
	if (Config.OperatingContextLimit > 0)
	{
		const auto Budget = FKismetAIContextBudget::Resolve(Config.Model, Config.MaxTokens, Config.OperatingContextLimit);
		const auto Estimate = FKismetAIRequestEstimate::Estimate(SystemPrompt, Tools,
			KismetAIAgentCodec::ToAnthropicArray(Messages), FKismetAIUsageCalibration(), Budget);
		if (static_cast<int64>(Estimate.Tokens) + Config.MaxTokens > Budget.ContextWindow)
		{
			OutError = TEXT("Estimated request plus output exceeds the configured operating context; no request was dispatched.");
			return false;
		}
	}
	return true;
}

FModelCallRequest::FModelCallRequest(const TArray<FAgentMessage>& InMessages,
	const TArray<TSharedPtr<FJsonValue>>& InTools, const FString& InSystemPrompt,
	const FKismetAIAgentConfig& InConfig)
	: Messages(SnapshotMessages(InMessages))
	, Tools(SnapshotValues(InTools))
	, SystemPrompt(InSystemPrompt)
	, Config(SnapshotConfig(InConfig))
{
}

FKismetAICurlModelClient::FKismetAICurlModelClient(const FString& ModelId)
	: Provider(MakeProviderForModel(ModelId))
{
	ProviderProfileId = KismetAIProviders::ProviderIdForModel(ModelId);
}

FKismetAICurlModelClient::FKismetAICurlModelClient(const FKismetAIAgentConfig& Config)
{
	ProviderProfileId = KismetAIProviders::ResolveProviderId(Config);
	FString SelectionError;
	if (KismetAIProviders::ValidateSelection(Config, SelectionError)) Provider = MakeProviderForConfig(Config);
}

// Out-of-line: Provider is a TUniquePtr<ILLMProvider>, forward-declared in the header.
FKismetAICurlModelClient::~FKismetAICurlModelClient() = default;

void FKismetAICurlModelClient::Cancel()
{
	bCancelled = true;

	FScopeLock Lock(&CurlProcMutex);
	if (ActiveCurlProc.IsValid())
	{
		FPlatformProcess::TerminateProc(ActiveCurlProc, /*KillTree=*/true);
	}
}

FString FKismetAICurlModelClient::GetProviderName() const
{
	return Provider.IsValid() ? Provider->GetName() : FString();
}

int32 FKismetAICurlModelClient::CacheTtlSeconds() const
{
	return Provider.IsValid() ? Provider->CacheTtlSeconds() : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// curl invocation helpers
//
// Headers (which carry the API key) go into a --config file rather than argv:
// process arguments are readable by any local process for the duration of the
// call, and the call can last minutes. The config file lives in the per-call
// scratch directory and is deleted before CallLLMOnce returns.
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAICurlModelClient::BuildCurlConfigFile(const TArray<FString>& Headers)
{
	// curl's config parser honours backslash escapes inside a double-quoted
	// value, so \ and " are the only characters that need escaping. Order
	// matters: escape backslashes first or the quote escapes get doubled.
	auto EscapeValue = [](const FString& In) -> FString
	{
		return In.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
	};

	FString Out;
	Out += FString::Printf(TEXT("header = \"Content-Type: application/json\"\n"));
	for (const FString& H : Headers)
	{
		Out += FString::Printf(TEXT("header = \"%s\"\n"), *EscapeValue(H));
	}
	return Out;
}

FString FKismetAICurlModelClient::BuildCurlArgs(
	const FString& Url,
	const FString& ConfigPath,
	const FString& BodyPath,
	const FString& OutputPath)
{
	// Double-quoted args, split by CreateProc (not /bin/sh) — see CallLLMOnce.
	return FString::Printf(
		TEXT("-sS --no-buffer --connect-timeout 10 --max-time 300 -X POST \"%s\" ")
		TEXT("--config \"%s\" ")
		TEXT("-d \"@%s\" ")
		TEXT("-o \"%s\" ")
		TEXT("-w \"%%{http_code}\""),
		*Url, *ConfigPath, *BodyPath, *OutputPath);
}

FString FKismetAICurlModelClient::MakeCallScratchDir()
{
	const FString Override = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TRANSPORT_SCRATCH_ROOT"));
	const FString Root = FPaths::ConvertRelativePathToFull(Override.IsEmpty()
		? FString(FPlatformProcess::UserTempDir()) / TEXT("KismetAI") / TEXT("llm") : Override);
	return Root / FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

FModelCallOutcome FKismetAICurlModelClient::Call(
	const FModelCallRequest& Request,
	const FModelCallbacks& Callbacks)
{
	const FKismetAIAgentConfig Config = Request.Config;
	FString SelectionError;
	if (!KismetAIProviders::ValidateSelection(Config, SelectionError))
	{
		FModelCallOutcome Refused;
		Refused.Result = EModelCallResult::FatalError;
		Refused.ErrorType = TEXT("unsupported_provider");
		Refused.ErrorMessage = SelectionError;
		return Refused;
	}
	FString PolicyError;
	if (!Request.Validate(PolicyError))
	{
		FModelCallOutcome Refused;
		Refused.Result = EModelCallResult::FatalError;
		Refused.ErrorType = TEXT("invalid_model_policy");
		Refused.ErrorMessage = PolicyError;
		return Refused;
	}
	const TArray<FAgentMessage>& Messages = Request.Messages;
	const FOnThinkingComplete& OnThinkingComplete = Callbacks.OnThinkingComplete;
	const FOnThinkingDelta& OnThinkingDelta = Callbacks.OnThinkingDelta;
	// The run's provider can change between calls (the user picks another model
	// or profile mid-session). Rebuild when the resolved profile changes so a
	// request never goes out through the previous provider's endpoint and key.
	const FString WantedProfile = KismetAIProviders::ResolveProviderId(Config);
	if (!Provider.IsValid() || WantedProfile != ProviderProfileId)
	{
		Provider = MakeProviderForConfig(Config);
		ProviderProfileId = WantedProfile;
	}
	if (!Provider.IsValid())
	{
		FModelCallOutcome Unsupported;
		Unsupported.Result = EModelCallResult::FatalError;
		Unsupported.ErrorType = TEXT("unsupported_provider");
		Unsupported.ErrorMessage = FString::Printf(
			TEXT("Provider '%s' uses a protocol this build cannot send yet."), *WantedProfile);
		return Unsupported;
	}

	// Local and custom servers: say "Ollama not running at <url>" or "this
	// model has no tool support" before the first request, instead of a
	// generic transport error or a request that hangs. Once per base URL and
	// model per client; a run reuses its client.
	{
		const FKismetAIProviderProfile& Profile = KismetAIProviders::ResolveProfile(Config);
		const FString PreflightKey = Profile.Id + TEXT("|") + KismetAIProviders::GetBaseUrl(Config, Profile) + TEXT("|") + Config.Model;
		if (KismetAIProviders::IsSelfHosted(Profile) && PreflightKey != LastPreflightKey)
		{
			FKismetAICurlHttpFetcher Fetcher;
			FString PreflightType;
			FString PreflightMessage;
			if (!KismetAIModelsApi::PreflightLocal(Config, Profile, Fetcher, PreflightType, PreflightMessage))
			{
				FModelCallOutcome Refused;
				Refused.Result = EModelCallResult::FatalError;
				Refused.ErrorType = PreflightType;
				Refused.ErrorMessage = PreflightMessage;
				return Refused;
			}
			LastPreflightKey = PreflightKey;
		}
	}

	// Build request body via provider (behavior-preserving for Anthropic; parity).
	FLLMRequestShape Shape;
	TSharedPtr<FJsonObject> Body = Provider->BuildRequestBody(
		Config, Messages, Request.Tools, Request.SystemPrompt, Shape);

	FString RequestBody = ModelJsonToString(Body);

	KISMETAI_DEV_LOG(LogKismetAIAgent, Verbose, TEXT("LLM request body length: %d"), RequestBody.Len());

	// SSE streaming: provider-allocated stream state.
	TSharedPtr<ILLMStreamState> State = Provider->CreateStreamState();
	State->OnThinkingComplete = OnThinkingComplete;
	State->OnThinkingDelta = OnThinkingDelta;

	// Use curl subprocess for SSE streaming.
	// UE5's FHttpModule on macOS (NSURLSession) does not reliably support
	// incremental streaming from background threads. curl is robust and portable.

	const FString TempDir = MakeCallScratchDir();
	const FString BodyPath = TempDir / TEXT("body.json");
	const FString OutputPath = TempDir / TEXT("response.sse");
	const FString ConfigPath = TempDir / TEXT("curl.cfg");
	FKismetAITransportScratch Scratch;
	auto ScratchFailure = []()
	{
		FModelCallOutcome Failed;
		Failed.Result = EModelCallResult::FatalError;
		Failed.ErrorType = TEXT("transport_scratch_unavailable");
		Failed.ErrorMessage = TEXT("Could not prepare private local model transport files.");
		return Failed;
	};
	const TArray<FString> ExtraHeaders = Provider->GetExtraCurlHeaders(Config, Shape);
	if (!Scratch.Begin(TempDir) || !Scratch.WriteText(TEXT("body.json"), RequestBody)
		|| !Scratch.WriteText(TEXT("curl.cfg"), BuildCurlConfigFile(ExtraHeaders))
		|| !Scratch.MarkLaunching()) return ScratchFailure();

	const FString Url = Provider->GetEndpointUrl(Config);

	KISMETAI_DEV_LOG(LogKismetAIAgent, Log, TEXT("Running curl..."));

	// Build double-quoted arg string and let CreateProc do the argv split.
	// Bypassing /bin/sh removes the single-quote escape hazard the previous
	// shell-wrapped invocation had (any single quote inside Url, BodyPath, or
	// a header value terminated the quoted region).
	const FString Args = BuildCurlArgs(Url, ConfigPath, BodyPath, OutputPath);

#if PLATFORM_WINDOWS
	// Windows ships curl.exe in System32 since Windows 10 1803.
	const TCHAR* CurlExe = TEXT("curl.exe");
#else
	const TCHAR* CurlExe = TEXT("/usr/bin/curl");
#endif

	// CreateProc + polled wait so Abort() can kill the curl subprocess and
	// return the worker thread promptly during ShutdownModule. ExecProcess
	// blocks uninterruptibly and would let the worker outlive the module.
	void* StdOutReadPipe = nullptr;
	void* StdOutWritePipe = nullptr;
	FPlatformProcess::CreatePipe(StdOutReadPipe, StdOutWritePipe);

	// Window-suppression flags for CreateProc, expressed as
	// (bLaunchDetached, bLaunchHidden, bLaunchReallyHidden).
#if PLATFORM_WINDOWS
	// curl.exe is a console-subsystem app, so launching it from the GUI editor
	// pops up a C:\Windows\System32\curl.exe console window for every API call.
	// FWindowsPlatformProcess::CreateProc only adds CREATE_NO_WINDOW (the flag
	// that actually suppresses the console) when bLaunchDetached && bLaunchReallyHidden
	// are both true; bLaunchHidden alone just minimizes the window (SW_SHOWMINNOACTIVE),
	// which still flashes. CREATE_NO_WINDOW keeps a hidden console so our redirected
	// stdout pipe and TerminateProc/IsProcRunning still work.
	const bool bDetached = true;
	const bool bHidden = false;
	const bool bReallyHidden = true;
#else
	// On Unix bLaunchDetached makes the proc handle "fire and forget", which breaks
	// the IsProcRunning/GetProcReturnCode/TerminateProc polling below. There is no
	// console-window problem here, so keep the plain hidden launch.
	const bool bDetached = false;
	const bool bHidden = true;
	const bool bReallyHidden = false;
#endif

	uint32 CurlPid = 0;
	FProcHandle Proc = FPlatformProcess::CreateProc(
		CurlExe, *Args, bDetached, bHidden, bReallyHidden,
		/*OutProcessID=*/&CurlPid, /*PriorityModifier=*/0, /*OptionalWorkingDirectory=*/nullptr,
		StdOutWritePipe, /*PipeWriteChild=*/nullptr);

	if (Proc.IsValid() && !Scratch.MarkRunning(CurlPid))
	{
		FPlatformProcess::TerminateProc(Proc, true);
		FPlatformProcess::WaitForProc(Proc);
		FPlatformProcess::CloseProc(Proc);
		FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);
		return ScratchFailure();
	}

	bool bExecOk = Proc.IsValid();
	int32 ExitCode = -1;
	FString StdOut;

	// --- incremental SSE consumption ---------------------------------------
	//
	// curl writes the body to OutputPath as it arrives (--no-buffer). Reading
	// the file as it grows and feeding the parser lets OnTextDelta /
	// OnThinkingDelta fire DURING the call instead of in one burst after curl
	// exits, which is what made the panel appear frozen for a whole reasoning
	// turn and then dump everything at once.
	//
	// Status handling is untouched: the http_code still arrives on the stdout
	// pipe via -w, and the whole file is still read after exit for the error
	// path.
	FKismetAIStreamDecoder Decoder(*State, Callbacks);
	int64 StreamOffset = 0;

	// Read whatever has been appended since the last call and feed it to the
	// parser, dispatching any complete events. The parser buffers a partial
	// trailing line, so a read that lands mid-line is safe.
	auto PumpStream = [&]()
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*OutputPath));
		if (!Reader) { return; }

		const int64 Size = Reader->TotalSize();
		if (Size <= StreamOffset) { return; }

		const int64 NewBytes = Size - StreamOffset;
		TArray<uint8> Chunk;
		Chunk.SetNumUninitialized(static_cast<int32>(NewBytes));
		Reader->Seek(StreamOffset);
		Reader->Serialize(Chunk.GetData(), NewBytes);
		StreamOffset = Size;

		Decoder.Feed(Chunk.GetData(), NewBytes);
	};

	if (bExecOk)
	{
		{
			FScopeLock Lock(&CurlProcMutex);
			ActiveCurlProc = Proc;
		}

		while (FPlatformProcess::IsProcRunning(Proc))
		{
			StdOut += FPlatformProcess::ReadPipe(StdOutReadPipe);
			PumpStream();
			FPlatformProcess::Sleep(0.05f);
			if (bCancelled)
			{
				FPlatformProcess::TerminateProc(Proc, /*KillTree=*/true);
				FPlatformProcess::WaitForProc(Proc);
				break;
			}
		}
		// Drain any remaining bytes after the process exits.
		StdOut += FPlatformProcess::ReadPipe(StdOutReadPipe);
		PumpStream();

		FPlatformProcess::GetProcReturnCode(Proc, &ExitCode);

		{
			FScopeLock Lock(&CurlProcMutex);
			ActiveCurlProc.Reset();
		}
		FPlatformProcess::CloseProc(Proc);
	}

	FPlatformProcess::ClosePipe(StdOutReadPipe, StdOutWritePipe);

	// StdOut contains the HTTP status code (from -w '%{http_code}')
	int32 ResponseCode = FCString::Atoi(*StdOut.TrimStartAndEnd());

	KISMETAI_DEV_LOG(LogKismetAIAgent, Log, TEXT("curl done: exit=%d, http=%d, exec=%d, aborted=%d"),
		ExitCode, ResponseCode, bExecOk, bCancelled ? 1 : 0);

	// Read the SSE output file as raw bytes
	TArray<uint8> RawBytes;
	FFileHelper::LoadFileToArray(RawBytes, *OutputPath);
	// Length-taking constructor: RawBytes is a TArray<uint8> with no terminator,
	// and FUTF8ToTCHAR built from an explicit length does not add one, so
	// FString(Conv.Get()) reads past the end of the response buffer.
	FUTF8ToTCHAR RawOutputConv(reinterpret_cast<const ANSICHAR*>(RawBytes.GetData()), RawBytes.Num());
	FString RawOutput(RawOutputConv.Length(), RawOutputConv.Get());

	KISMETAI_DEV_LOG(LogKismetAIAgent, Log, TEXT("Response: %d bytes"), RawBytes.Num());

	// Final flush. The stream was already consumed incrementally during the poll
	// loop; this only covers a last event with no trailing newline, which the
	// parser cannot dispatch until it sees one.
	Decoder.Finish();

	FModelCallOutcome Outcome = ClassifyAttempt(*Provider, *State, RawOutput, ExitCode, ResponseCode, bExecOk, bCancelled);
	Outcome.EffectiveMaxTokens = Config.MaxTokens;
	Outcome.AttemptCount = 1;
	return Outcome;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stream decoding
// ─────────────────────────────────────────────────────────────────────────────

FKismetAIStreamDecoder::FKismetAIStreamDecoder(ILLMStreamState& InState, const FModelCallbacks& InCallbacks)
	: State(InState)
	, Callbacks(InCallbacks)
{
}

void FKismetAIStreamDecoder::Feed(const uint8* Bytes, int64 Length)
{
	if (Length <= 0) { return; }
	bAnyBytes = true;
	bLastByteWasNewline = Bytes[Length - 1] == '\n';
	Parser.Feed(Bytes, Length);
	DispatchPending();
}

void FKismetAIStreamDecoder::Finish()
{
	if (bAnyBytes && !bLastByteWasNewline)
	{
		const uint8 Newline = '\n';
		Parser.Feed(&Newline, 1);
		bLastByteWasNewline = true;
		DispatchPending();
	}
}

void FKismetAIStreamDecoder::DispatchPending()
{
	for (const FKismetAISSEEvent& Event : Parser.DequeueEvents())
	{
		if (!Event.Data.IsValid()) { continue; }
		FString DeltaText;
		const bool bIsTextDelta = State.ProcessEvent(Event.EventType, Event.Data, DeltaText);
		if (bIsTextDelta && Callbacks.OnTextDelta)
		{
			Callbacks.OnTextDelta(DeltaText);
		}
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Attempt classification
// ─────────────────────────────────────────────────────────────────────────────

FModelCallOutcome FKismetAICurlModelClient::ClassifyAttempt(
	const ILLMProvider& Provider,
	const ILLMStreamState& State,
	const FString& RawOutput,
	int32 ExitCode,
	int32 ResponseCode,
	bool bExecOk,
	bool bCancelled)
{
	const bool bHttpSuccess = bExecOk && ExitCode == 0 && ResponseCode > 0 && !bCancelled;

	// Classify the outcome
	FModelCallOutcome Outcome;
	Outcome.HttpStatusCode = ResponseCode;
	Outcome.RequestCount = bExecOk ? 1 : 0;
	Outcome.Response = State.BuildResponseJson();
	const TSharedPtr<FJsonObject>* ReportedUsage;
	if (Outcome.Response.IsValid() && Outcome.Response->TryGetObjectField(TEXT("usage"), ReportedUsage))
	{
		Outcome.UsageReports.Add(*ReportedUsage);
	}
	bool bUsageComplete = false;
	if (Outcome.Response.IsValid()) Outcome.Response->TryGetBoolField(TEXT("usage_complete"), bUsageComplete);
	Outcome.bUsageIncomplete = (ResponseCode == 200 || Outcome.UsageReports.Num() > 0) && !bUsageComplete;
	// After a timeout/disconnect the provider may have processed the request even
	// when curl never received its status. DNS/TCP-connect failures sent nothing.
	Outcome.bUsageIncomplete |= bExecOk && ExitCode != 0 && ExitCode != 6 && ExitCode != 7 && !bUsageComplete;

	if (ResponseCode != 200 || !bHttpSuccess)
	{
		// Curl error or non-SSE response body.
		const FString& RawBody = RawOutput;
		KISMETAI_DEV_LOG(LogKismetAIAgent, Error, TEXT("LLM API returned status %d (curl exit %d)"),
			ResponseCode, ExitCode);

		// Note: retry-after header is not available via curl subprocess.
		// The retry logic uses default backoff which is sufficient.

		// Try to parse error body via provider (extracts error.type + error.message).
		TSharedPtr<FJsonObject> ErrorJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawBody);
		if (FJsonSerializer::Deserialize(Reader, ErrorJson))
		{
			Provider.ParseErrorBody(ErrorJson, Outcome);
		}

		// If no error message was extracted (e.g. curl connection failure with no response),
		// provide a user-friendly message instead of raw curl output
		if (Outcome.ErrorMessage.IsEmpty() && ExitCode != 0)
		{
			Outcome.ErrorMessage = TEXT("Could not connect to the selected model provider — check its address and your connection");
			if (Outcome.ErrorType.IsEmpty())
			{
				Outcome.ErrorType = TEXT("connection_error");
			}
		}

		if (Outcome.ErrorMessage.IsEmpty())
		{
			Outcome.ErrorMessage = FString::Printf(TEXT("The model provider rejected the request (HTTP %d). Check your credentials and provider settings."), ResponseCode);
			if (Outcome.ErrorType.IsEmpty()) Outcome.ErrorType = TEXT("provider_error");
		}

		// Classify error result via the pure free helper. The MaxTokensOverflow
		// regex parse stays inline because it mutates Outcome fields the
		// classifier doesn't see (InputTokens, ContextLimit).
		Outcome.Result = FKismetAIAgent::ClassifyCallOutcome(
			ExitCode, ResponseCode, Outcome.ErrorType, Outcome.ErrorMessage, bCancelled);

		if (Outcome.Result == EModelCallResult::MaxTokensOverflow)
		{
			// Parse "input length and `max_tokens` exceed context limit: {input} + {max} > {limit}"
			FRegexPattern Pattern(TEXT("(\\d+)\\s*\\+\\s*\\d+\\s*>\\s*(\\d+)"));
			FRegexMatcher Matcher(Pattern, Outcome.ErrorMessage);
			if (Matcher.FindNext())
			{
				Outcome.InputTokens = FCString::Atoi(*Matcher.GetCaptureGroup(1));
				Outcome.ContextLimit = FCString::Atoi(*Matcher.GetCaptureGroup(2));
			}
		}
		return Outcome;
	}

	// Providers can deliver errors inside an HTTP200 stream. Preserve known
	// usage, but never dispatch partial tool calls as a successful response.
	FString StreamErrorMessage;
	FString StreamErrorCode;
	if (State.HasStreamError(StreamErrorMessage, StreamErrorCode))
	{
		Outcome.Result = EModelCallResult::RetryableError;
		Outcome.HttpStatusCode = 200;
		Outcome.ErrorType = StreamErrorCode.IsEmpty() ? TEXT("stream_error") : StreamErrorCode;
		Outcome.ErrorMessage = StreamErrorMessage.IsEmpty()
			? TEXT("The model provider returned a failed response mid-stream")
			: StreamErrorMessage;
		if (Outcome.ErrorType.Contains(TEXT("auth")) || Outcome.ErrorType.Contains(TEXT("api_key"))
			|| Outcome.ErrorType == TEXT("permission_error") || Outcome.ErrorType == TEXT("insufficient_quota")
			|| Outcome.ErrorType == TEXT("malformed_tool_arguments"))
		{
			Outcome.Result = EModelCallResult::FatalError;
		}
		KISMETAI_LOG(LogKismetAIAgent, Warning,
			TEXT("API call returned 200 with an in-stream error"));
		return Outcome;
	}
	bool bStreamComplete = false;
	if (!Outcome.Response.IsValid()
		|| !Outcome.Response->TryGetBoolField(TEXT("stream_complete"), bStreamComplete) || !bStreamComplete)
	{
		Outcome.Result = EModelCallResult::RetryableError;
		Outcome.ErrorType = TEXT("incomplete_stream");
		Outcome.ErrorMessage = TEXT("The provider response ended before completion. Please retry; no partial tool calls were executed.");
		return Outcome;
	}

	// Commit content to the canonical message only after the complete attempt
	// passed transport and provider validation. Response envelope metadata is
	// not message metadata and must never be replayed into a provider request.
	TSharedPtr<FJsonObject> Assistant = MakeShared<FJsonObject>();
	Assistant->SetStringField(TEXT("role"), TEXT("assistant"));
	const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
	if (Outcome.Response->TryGetArrayField(TEXT("content"), Content))
	{
		Assistant->SetArrayField(TEXT("content"), *Content);
	}
	KismetAIAgentCodec::FromAnthropicJson(Assistant, Outcome.AssistantMessage);
	Outcome.Response->RemoveField(TEXT("content"));

	// Success path
	Outcome.Result = EModelCallResult::Success;
	Outcome.HttpStatusCode = 200;
	KISMETAI_DEV_LOG(LogKismetAIAgent, Log, TEXT("API call success, response valid=%d"), Outcome.Response.IsValid());

	// Log cache performance
	if (Outcome.Response.IsValid())
	{
		const TSharedPtr<FJsonObject>* UsageObj;
		if (Outcome.Response->TryGetObjectField(TEXT("usage"), UsageObj))
		{
			int32 InputTokens = (*UsageObj)->GetIntegerField(TEXT("input_tokens"));
			int32 OutputTokens = (*UsageObj)->GetIntegerField(TEXT("output_tokens"));
			int32 CacheRead = 0;
			int32 CacheWrite = 0;
			(*UsageObj)->TryGetNumberField(TEXT("cache_read_input_tokens"), CacheRead);
			(*UsageObj)->TryGetNumberField(TEXT("cache_creation_input_tokens"), CacheWrite);

			KISMETAI_DEV_LOG(LogKismetAIAgent, Log, TEXT("Tokens: input=%d output=%d cache_read=%d cache_write=%d"),
				InputTokens, OutputTokens, CacheRead, CacheWrite);
		}
	}

	return Outcome;
}

// ─────────────────────────────────────────────────────────────────────────────
// Retry delay computation
// ─────────────────────────────────────────────────────────────────────────────

FModelCallOutcome FKismetAICurlModelClient::CallWithRetry(
	const FModelCallRequest& Request,
	const FModelCallbacks& Callbacks)
{
	return RunRetryLoop(Request, Callbacks,
		[this, &Callbacks](const FModelCallRequest& AttemptRequest) { return Call(AttemptRequest, Callbacks); },
		[this]() { return static_cast<bool>(bCancelled); });
}

FModelCallOutcome FKismetAICurlModelClient::RunRetryLoop(
	const FModelCallRequest& Request,
	const FModelCallbacks& Callbacks,
	TFunctionRef<FModelCallOutcome(const FModelCallRequest&)> CallOnce,
	TFunctionRef<bool()> IsCancelled)
{
	FKismetAIAgentConfig Config = Request.Config;
	FModelCallOutcome Outcome;
	if (!Request.Validate(Outcome.ErrorMessage))
	{
		Outcome.Result = EModelCallResult::FatalError;
		Outcome.ErrorType = TEXT("invalid_model_policy");
		return Outcome;
	}
	TArray<TSharedPtr<FJsonObject>> UsageReports;
	int32 RequestCount = 0;
	bool bUsageIncomplete = false;

	// The loop bound is Config.MaxRetries (the largest per-result budget). The
	// effective per-result budget — recomputed after each call — decides
	// whether to keep retrying. NetworkUnreachable's budget is 1, so a second
	// failure of that type breaks out via the EffectiveMaxRetries check below.
	for (int32 Attempt = 0; Attempt <= Config.MaxRetries; Attempt++)
	{
		if (IsCancelled())
		{
			Outcome.Result = EModelCallResult::FatalError;
			Outcome.ErrorMessage = TEXT("Aborted");
			return Outcome;
		}

		if (Attempt > 0)
		{
			// NetworkUnreachable retries get a fixed short delay (no exponential
			// backoff): the failure is definite, so a long wait won't help.
			// 1.0s leaves margin under the ≤2s cap.
			double DelaySeconds = (Outcome.Result == EModelCallResult::NetworkUnreachable)
				? 1.0
				: FKismetAIAgent::ComputeRetryDelay(Attempt - 1, Config, Outcome.RetryAfterSeconds);
			KISMETAI_LOG(LogKismetAIAgent, Warning, TEXT("Retrying API call (attempt %d/%d) after %.1fs delay (status=%d)"),
				Attempt, Config.MaxRetries, DelaySeconds, Outcome.HttpStatusCode);
			const double Deadline = FPlatformTime::Seconds() + DelaySeconds;
			while (!IsCancelled() && FPlatformTime::Seconds() < Deadline)
			{
				FPlatformProcess::Sleep(FMath::Clamp(Deadline - FPlatformTime::Seconds(), 0.0, 0.05));
			}
			if (IsCancelled())
			{
				Outcome.Result = EModelCallResult::FatalError;
				Outcome.ErrorMessage = TEXT("Aborted");
				return Outcome;
			}
		}

		if (Callbacks.AdmitAttempt)
		{
			const FString Refused = Callbacks.AdmitAttempt();
			if (!Refused.IsEmpty())
			{
				Outcome.Result = EModelCallResult::FatalError;
				Outcome.ErrorType = TEXT("run_budget_exhausted");
				Outcome.ErrorMessage = Refused;
				return Outcome;
			}
		}

		if (Callbacks.OnAttemptStart)
		{
			Callbacks.OnAttemptStart(Attempt);
		}
		const FModelCallRequest AttemptRequest{Request.Messages, Request.Tools, Request.SystemPrompt, Config};
		Outcome = CallOnce(AttemptRequest);
		if (Callbacks.OnAttemptCompleted) Callbacks.OnAttemptCompleted(Outcome);
		Outcome.EffectiveMaxTokens = Config.MaxTokens;
		Outcome.AttemptCount = Attempt + 1;
		UsageReports.Append(Outcome.UsageReports);
		RequestCount += Outcome.RequestCount;
		bUsageIncomplete |= Outcome.bUsageIncomplete;
		Outcome.UsageReports = UsageReports;
		Outcome.RequestCount = RequestCount;
		Outcome.bUsageIncomplete = bUsageIncomplete;

		if (Outcome.Result == EModelCallResult::Success)
		{
			return Outcome;
		}

		if (Callbacks.CheckAttemptAllowed)
		{
			const FString Refused = Callbacks.CheckAttemptAllowed();
			if (!Refused.IsEmpty())
			{
				Outcome.Result = EModelCallResult::FatalError;
				Outcome.ErrorType = TEXT("run_budget_exhausted");
				Outcome.ErrorMessage = Refused;
				return Outcome;
			}
		}

		if (Outcome.Result == EModelCallResult::MaxTokensOverflow)
		{
			// Reduce MaxTokens and retry: available = contextLimit - inputTokens - 1000 (safety buffer)
			const int32 Window = Config.OperatingContextLimit > 0
				? FMath::Min(Outcome.ContextLimit, Config.OperatingContextLimit) : Outcome.ContextLimit;
			const int64 Available = static_cast<int64>(Window) - Outcome.InputTokens - 1000;
			if (Outcome.InputTokens < 0 || Outcome.ContextLimit <= 0 || Available < 3000 || Available >= Config.MaxTokens)
			{
				Outcome.Result = EModelCallResult::FatalError;
				Outcome.ErrorMessage = TEXT("Provider limit does not permit a valid strictly smaller output allowance");
				return Outcome;
			}
			KISMETAI_DEV_LOG(LogKismetAIAgent, Warning, TEXT("MaxTokens overflow, reducing from %d to %d"),
				Config.MaxTokens, static_cast<int32>(Available));
			Config.MaxTokens = static_cast<int32>(Available);
			if (Attempt < Config.MaxRetries && Callbacks.OnAttemptRetrying)
			{
				Callbacks.OnAttemptRetrying(Attempt, Outcome);
			}
			continue;  // Retry immediately, no backoff needed
		}

		// Only RetryableError and NetworkUnreachable are retried. PromptTooLong
		// and FatalError return immediately; any other unexpected category is
		// treated as non-retryable to keep the retry surface tight.
		if (Outcome.Result != EModelCallResult::RetryableError
			&& Outcome.Result != EModelCallResult::NetworkUnreachable)
		{
			return Outcome;
		}

		// Per-result retry budget: NetworkUnreachable allows only 1 retry; all
		// other retryable categories use Config.MaxRetries. Break out as soon
		// as we've spent the budget so callers don't see N failed attempts
		// when the budget was 1.
		const int32 EffectiveMaxRetries = FKismetAIAgent::GetMaxRetriesForResult(Outcome.Result, Config);
		if (Attempt >= EffectiveMaxRetries)
		{
			KISMETAI_LOG(LogKismetAIAgent, Error, TEXT("API call failed after %d retries (last status=%d, result=%d)"),
				EffectiveMaxRetries, Outcome.HttpStatusCode, (int32)Outcome.Result);
			return Outcome;
		}
		if (Attempt < Config.MaxRetries && Callbacks.OnAttemptRetrying)
		{
			Callbacks.OnAttemptRetrying(Attempt, Outcome);
		}
	}

	// Exhausted retries (RetryableError-only path; NetworkUnreachable returns above).
	KISMETAI_LOG(LogKismetAIAgent, Error, TEXT("API call failed after %d retries (last status=%d)"),
		Config.MaxRetries, Outcome.HttpStatusCode);
	return Outcome;
}

// ─────────────────────────────────────────────────────────────────────────────
// Context management
// ─────────────────────────────────────────────────────────────────────────────

