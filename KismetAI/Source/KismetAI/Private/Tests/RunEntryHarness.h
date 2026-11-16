// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Drives the module's real run entry points with a controlled provider.
//
// Extracted from ExternalRunsTest.cpp so the release-scenario tests
// (ReleaseScenariosTest.cpp) use the same harness instead of a second
// one. Only the model client is scripted (FSlowModelClient). The module's agent
// factory seam builds each run's agent around it and the module's real tool
// invoker; admission, caller context, grants, the worker, the game-thread
// dispatcher state machine, journal and finalization are production. Synchronous
// tests inject a TaskGraph scheduler; latent tests select the normal frame ticker.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIEngineCompat.h"
#include "KismetAIExternalRuns.h"
#include "KismetAISessionController.h"
#include "KismetAIModule.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIAgent.h"
#include "KismetAIModelClientInterface.h"
#include "KismetAIToolPolicy.h"
#include "Async/TaskGraphInterfaces.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <atomic>

// Reaches the module's private run routes and the agent factory seam.
struct FKismetAIExternalRunTestAccess
{
	using FFactory = TFunction<TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe>(const FKismetAIAgentConfig&)>;

	static FKismetAIAgentConfig& Config(FKismetAIModule& M) { return M.AgentConfig; }
	static void LoadSettings(FKismetAIModule& M) { M.LoadSettings(); }
	static void SetFactory(FKismetAIModule& M, FFactory F) { M.AgentFactoryForTests = MoveTemp(F); }
	static FKismetAISessionController& Session(FKismetAIModule& M) { return *M.Session; }
	static FKismetAIExternalRunRegistry& Registry(FKismetAIModule& M) { return *M.ExternalRuns; }
	static bool Run(FKismetAIModule& M, const FHttpServerRequest& R, const FHttpResultCallback& C) { return M.HandleAgentRun(R, C); }
	static bool Submit(FKismetAIModule& M, const FHttpServerRequest& R, const FHttpResultCallback& C) { return M.HandleAgentRunSubmit(R, C); }
	static bool Status(FKismetAIModule& M, const FHttpServerRequest& R, const FHttpResultCallback& C) { return M.HandleAgentRunStatus(R, C); }
	static bool Events(FKismetAIModule& M, const FHttpServerRequest& R, const FHttpResultCallback& C) { return M.HandleAgentRunEvents(R, C); }
	static bool Cancel(FKismetAIModule& M, const FHttpServerRequest& R, const FHttpResultCallback& C) { return M.HandleAgentRunCancel(R, C); }
};

namespace KismetAIRunEntryHarness
{
	constexpr uint32 PromptMs = 5000;
	using FAccess = FKismetAIExternalRunTestAccess;

	struct FLatch
	{
		FEvent* Event;
		FLatch() : Event(FPlatformProcess::GetSynchEventFromPool(true)) {}
		~FLatch() { FPlatformProcess::ReturnSynchEventToPool(Event); }
		void Trigger() { Event->Trigger(); }
		bool Wait(uint32 Ms) { return Event->Wait(Ms); }
	};

	inline FModelCallOutcome ResponseTurn(const TArray<TSharedPtr<FJsonValue>>& Blocks, const TCHAR* StopReason)
	{
		TSharedPtr<FJsonObject> Usage = MakeShared<FJsonObject>();
		Usage->SetNumberField(TEXT("input_tokens"), 100);
		Usage->SetNumberField(TEXT("output_tokens"), 10);
		TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
		Resp->SetStringField(TEXT("stop_reason"), StopReason);
		Resp->SetObjectField(TEXT("usage"), Usage);
		FModelCallOutcome Out;
		Out.Result = EModelCallResult::Success;
		Out.Response = Resp;
		Out.AssistantMessage.Role = EAgentRole::Assistant;
		Out.AssistantMessage.Parts = KismetAIAgentCodec::FromParts(Blocks);
		Out.HttpStatusCode = 200;
		Out.RequestCount = 1;
		return Out;
	}

	inline TSharedPtr<FJsonValue> ToolUseBlock(const FString& Id, const FString& Name, const TSharedPtr<FJsonObject>& Input)
	{
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("tool_use"));
		Block->SetStringField(TEXT("id"), Id);
		Block->SetStringField(TEXT("name"), Name);
		Block->SetObjectField(TEXT("input"), Input);
		return MakeShared<FJsonValueObject>(Block);
	}

	inline FModelCallOutcome TextTurn(const FString& Text)
	{
		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), Text);
		return ResponseTurn({ MakeShared<FJsonValueObject>(Block) }, TEXT("end_turn"));
	}

	inline FModelCallOutcome ToolTurn(const FString& Id, const FString& Name, const TSharedPtr<FJsonObject>& Input)
	{
		return ResponseTurn({ ToolUseBlock(Id, Name, Input) }, TEXT("tool_use"));
	}

	// The controlled provider: returns Script in order (the last entry repeats),
	// records every request's messages, and parks call number BlockOnCall until
	// Cancel() or Unblock(). Completed counts calls that returned.
	class FSlowModelClient final : public IKismetAIModelClient
	{
	public:
		TArray<FModelCallOutcome> Script;
		int32 BlockOnCall = -1;
		// How long the blocked call waits before returning by itself (a slow
		// provider rather than a stuck one).
		uint32 BlockTimeoutMs = MAX_uint32;
		FLatch Entered;
		FLatch Wake;
		std::atomic<int32> Calls{ 0 };
		std::atomic<int32> Completed{ 0 };
		std::atomic<bool> bCancelled{ false };

		virtual FModelCallOutcome Call(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override { return Respond(Request); }
		virtual FModelCallOutcome CallWithRetry(const FModelCallRequest& Request, const FModelCallbacks& Callbacks) override { return Respond(Request); }
		virtual void Cancel() override { bCancelled = true; Wake.Trigger(); }
		virtual FString GetProviderName() const override { return TEXT("anthropic"); }
		virtual int32 CacheTtlSeconds() const override { return 300; }
		void Unblock() { Wake.Trigger(); }

		// The messages of every request sent so far, in order.
		TArray<TArray<TSharedPtr<FJsonValue>>> GetRequests() const
		{
			FScopeLock Lock(&RequestsLock);
			return Requests;
		}

	private:
		FModelCallOutcome Respond(const FModelCallRequest& Request)
		{
			{
				FScopeLock Lock(&RequestsLock);
				Requests.Add(KismetAIAgentCodec::ToAnthropicArray(Request.Messages));
			}
			const int32 Index = Calls++;
			if (Index == BlockOnCall)
			{
				Entered.Trigger();
				Wake.Wait(BlockTimeoutMs);
				if (bCancelled)
				{
					++Completed;
					FModelCallOutcome Aborted;
					Aborted.Result = EModelCallResult::FatalError;
					Aborted.ErrorMessage = TEXT("Aborted");
					return Aborted;
				}
			}
			++Completed;
			if (Script.Num() == 0) { return TextTurn(TEXT("done")); }
			return Script[FMath::Min(Index, Script.Num() - 1)];
		}

		mutable FCriticalSection RequestsLock;
		TArray<TArray<TSharedPtr<FJsonValue>>> Requests;
	};

	class FCountingPolicy final : public IKismetAIToolPolicy
	{
	public:
		virtual FPolicyVerdict Evaluate(const FPolicyContext& Context) const override { return FPolicyVerdict::Allow(); }
		virtual FPolicyVerdict EvaluateAdmission(const FPolicyContext& Context) const override
		{
			++Admissions;
			return FPolicyVerdict::Allow();
		}
		mutable std::atomic<int32> Admissions{ 0 };
	};

	// The seams the module-created agent borrows. Kept alive by the test until
	// the run's worker has exited.
	struct FSeams
	{
		FSlowModelClient Model;
		FCountingPolicy Policy;
		TSharedRef<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe> Dispatcher = MakeShared<FKismetAIGameThreadDispatcher, ESPMode::ThreadSafe>();
		TUniquePtr<FKismetAIModuleToolInvoker> Invoker = MakeUnique<FKismetAIModuleToolInvoker>(FKismetAIModule::Get(), Dispatcher);
		// Runs on each agent the factory builds (game thread), e.g. to install
		// a journal append hook.
		TFunction<void(FKismetAIAgent&)> ConfigureAgent;

		TSharedPtr<FKismetAIAgent, ESPMode::ThreadSafe> GetLastAgent() const
		{
			FScopeLock Lock(&AgentLock);
			return LastAgent.Pin();
		}
		void SetLastAgent(const TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe>& Agent)
		{
			FScopeLock Lock(&AgentLock);
			LastAgent = Agent;
		}

	private:
		mutable FCriticalSection AgentLock;
		TWeakPtr<FKismetAIAgent, ESPMode::ThreadSafe> LastAgent;
	};

	// Installs the agent factory for one test — and, unless the test wants the
	// production policy and its grants, a counting allow-everything policy —
	// and removes both whatever happens.
	struct FScopedModuleSeams
	{
		enum class EPolicy : uint8 { CountingAllowAll, Production };

		FKismetAIModule& Module;
		TSharedRef<FSeams, ESPMode::ThreadSafe> Seams = MakeShared<FSeams, ESPMode::ThreadSafe>();
		FCountingPolicy& Policy;

		explicit FScopedModuleSeams(EPolicy InPolicy = EPolicy::CountingAllowAll, bool bProductionScheduler = false)
			: Module(FKismetAIModule::Get()), Policy(Seams->Policy)
		{
			// Synchronous domain tests explicitly control queue progress. Real
			// frame scheduling is covered by latent production-scheduler tests.
			if (!bProductionScheduler)
			{
				Seams->Dispatcher->SetSchedulerForTests([](TFunction<void()> Callback)
				{
					AsyncTask(ENamedThreads::GameThread, MoveTemp(Callback));
				});
			}
			TSharedRef<FSeams, ESPMode::ThreadSafe> S = Seams;
			FAccess::SetFactory(Module, [S](const FKismetAIAgentConfig& Config)
			{
				FKismetAIAgentConfig C = Config;
				C.MaxRetries = 0;
				C.ThinkingMode = EThinkingMode::Disabled;
				TSharedRef<FKismetAIAgent, ESPMode::ThreadSafe> Agent = MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(*S->Invoker, S->Model, C);
				if (S->ConfigureAgent)
				{
					S->ConfigureAgent(*Agent);
				}
				S->SetLastAgent(Agent);
				return Agent;
			});
			if (InPolicy == EPolicy::CountingAllowAll)
			{
				Module.SetToolPolicyOverrideForTests(&Policy);
			}
		}
		~FScopedModuleSeams()
		{
			Seams->Model.Cancel();
			Seams->Dispatcher->Close();
			if (FAccess::Session(Module).NumLiveWorkers() != 0)
			{
				FAccess::Session(Module).CancelActive();
				// A failing fixture must never free dependencies borrowed by its
				// worker. Keep them alive until real frame progress drains it.
				UE_LOG(LogTemp, Error, TEXT("Run-entry fixture cleanup reached a live worker"));
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([KeepAlive = Seams](float)
				{
					return FAccess::Session(FKismetAIModule::Get()).NumLiveWorkers() != 0;
				}));
			}
			FAccess::SetFactory(Module, nullptr);
			Module.SetToolPolicyOverrideForTests(nullptr);
		}
	};

	inline FHttpServerRequest MakeRequest(const FString& Body, const TMap<FString, FString>& Query = {})
	{
		FHttpServerRequest Req;
		FTCHARToUTF8 Utf8(*Body);
		Req.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Req.QueryParams = Query;
		return Req;
	}

	inline FString BodyText(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Conv.Length(), Conv.Get());
	}

	struct FRunReply
	{
		int32 Code = 0;
		TSharedPtr<FJsonObject> Json;
		TUniquePtr<FHttpServerResponse> Response;

		TSharedPtr<FJsonObject> Data() const
		{
			const TSharedPtr<FJsonObject>* D = nullptr;
			return Json.IsValid() && Json->TryGetObjectField(TEXT("data"), D) && D ? *D : nullptr;
		}
		FString ErrorCode() const
		{
			const TSharedPtr<FJsonObject>* E = nullptr;
			return Json.IsValid() && Json->TryGetObjectField(TEXT("error"), E) && E ? (*E)->GetStringField(TEXT("error_code")) : FString();
		}
		// The chunked stream of this reply; not valid for an ordinary buffered response.
		KismetAIHttpCompat::FStream GetStream() const
		{
			return Response.IsValid() ? KismetAIHttpCompat::PeekStream(*Response) : KismetAIHttpCompat::FStream();
		}
		// What a streamed response has queued so far (consumes it).
		FString DrainStream()
		{
			FString Out;
			const KismetAIHttpCompat::FStream Stream = GetStream();
			if (Stream.IsValid())
			{
				TArray<uint8> Chunk;
				while (Stream.Queue->Dequeue(Chunk)) { Out += BodyText(Chunk); }
			}
			return Out;
		}
	};

	template <typename HandlerType>
	FRunReply Call(HandlerType Handler, const FHttpServerRequest& Req)
	{
		FRunReply Reply;
		Handler(FKismetAIModule::Get(), Req, [&Reply](TUniquePtr<FHttpServerResponse>&& Response)
		{
			Reply.Code = Response.IsValid() ? static_cast<int32>(Response->Code) : 0;
			if (Response.IsValid() && !KismetAIHttpCompat::PeekStream(*Response).IsValid())
			{
				FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BodyText(Response->Body)), Reply.Json);
			}
			Reply.Response = MoveTemp(Response);
		});
		return Reply;
	}

	inline FRunReply GetStatus(const FString& RunId) { return Call(&FAccess::Status, MakeRequest(FString(), { { TEXT("run_id"), RunId } })); }
	inline FRunReply GetEvents(const FString& RunId, int64 After, bool bStream = false)
	{
		TMap<FString, FString> Q = { { TEXT("run_id"), RunId }, { TEXT("after"), LexToString(After) } };
		if (bStream) { Q.Add(TEXT("stream"), TEXT("1")); }
		return Call(&FAccess::Events, MakeRequest(FString(), Q));
	}
	inline FRunReply PostCancel(const FString& RunId)
	{
		return Call(&FAccess::Cancel, MakeRequest(FString::Printf(TEXT("{\"run_id\":\"%s\"}"), *RunId)));
	}

	// Pump game-thread tasks (tool callbacks, run finalization) until Done.
	// The deadline only bounds a failure; nothing passes by waiting.
	template <typename PredType>
	bool PumpUntil(PredType Done, double Seconds = 10.0)
	{
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (!Done())
		{
			if (FPlatformTime::Seconds() > Deadline) { return false; }
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.002f);
		}
		return true;
	}

	inline FString StateOf(const FRunReply& Status)
	{
		const TSharedPtr<FJsonObject> D = Status.Data();
		return D.IsValid() ? D->GetStringField(TEXT("state")) : FString(TEXT("<none>"));
	}

	inline TArray<FString> EventTypes(const FRunReply& Page)
	{
		TArray<FString> Out;
		const TSharedPtr<FJsonObject> D = Page.Data();
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (D.IsValid() && D->TryGetArrayField(TEXT("events"), Events))
		{
			for (const TSharedPtr<FJsonValue>& V : *Events) { Out.Add(V->AsObject()->GetStringField(TEXT("type"))); }
		}
		return Out;
	}

	// Ends the run if a test bailed out early and waits for its worker, so the
	// seams it borrows outlive it.
	inline void SettleRun(FKismetAIModule& Module, const FString& RunId, FSlowModelClient& Model)
	{
		Model.Unblock();
		if (const FKismetAIExternalRunPtr Record = FAccess::Registry(Module).Find(RunId))
		{
			const FKismetAIRunPtr Run = Record->GetControllerRun();
			if (!PumpUntil([&Record]() { return Record->IsFinished(); }))
			{
				UE_LOG(LogTemp, Error, TEXT("Run-entry fixture did not finish before cleanup deadline"));
				if (Run.IsValid()) FAccess::Session(Module).Cancel(Run.ToSharedRef());
			}
			if (Run.IsValid() && !FAccess::Session(Module).WaitForWorkerExit(Run.ToSharedRef(), PromptMs))
			{
				UE_LOG(LogTemp, Error, TEXT("Run-entry fixture worker did not exit before cleanup deadline"));
			}
		}
	}
}
