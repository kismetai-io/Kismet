// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMcpRoute.h"
#include "KismetAIModule.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAISourceJobs.h"
#include "KismetAISourceContract.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "KismetAIAgent.h"
#include "KismetAIMcpServer.h"
#include "KismetAIMcpToken.h"

#include "HttpPath.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "KismetAIHttpTransport.h"
#include "Interfaces/IPluginManager.h"
#include "IPAddress.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIMcp, Log, All);

namespace
{
	// Same cap as the /api/v1 routes.
	constexpr int32 MaxMcpBodyBytes = 1 * 1024 * 1024;

	FString GetPluginVersion()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("KismetAI"));
		return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString();
	}

	// A fresh agent per call: the agent's run state (plan mode, cancellation
	// tree) is per-run, and nothing should carry from one external call to the
	// next. Construction is cheap and makes no network calls; the model client
	// it builds is never used on this path.
	FKismetAIAgentConfig MakeExternalAgentConfig(const FKismetAIModule& Module)
	{
		FKismetAIAgentConfig Config;
		Config.MaxToolResultChars = Module.GetAgentConfig().MaxToolResultChars;
		return Config;
	}

	// GT-only request-id routing. Worker jobs own separate protocol servers and
	// agents; neither this state nor the module is captured by worker work.
	struct FMcpRouteState
	{
		FKismetAIModule* Module = nullptr;
		TMap<FString, FString> PendingIds;
		TUniquePtr<FKismetAIMcpServer> SynchronousServer;

		FKismetAIMcpServer& GetSynchronousServer()
		{
			if (!SynchronousServer)
			{
				FKismetAIModule* ModulePtr = Module;
				SynchronousServer = MakeUnique<FKismetAIMcpServer>(KismetAIMcpRoute::GetToolSchemas(*Module),
					FKismetAIModuleToolInvoker::DispatchableToolNames(),
					[ModulePtr](const FString& Name, const TSharedPtr<FJsonObject>& Args)
					{
						return KismetAIMcpRoute::CallTool(*ModulePtr,
							FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("mcp-bearer")), Name, Args);
					}, GetPluginVersion());
			}
			return *SynchronousServer;
		}
	};

	FString RequestKey(const TSharedPtr<FJsonValue>& Id)
	{
		if (!Id.IsValid()) return FString();
		if (Id->Type == EJson::String) return TEXT("s:") + Id->AsString();
		if (Id->Type == EJson::Number) return FString::Printf(TEXT("n:%.17g"), Id->AsNumber());
		if (Id->Type == EJson::Null) return TEXT("null");
		return FString();
	}

	bool IsCancellation(const TSharedPtr<FJsonObject>& Message, FString& OutKey)
	{
		FString Method, Version;
		if (!Message.IsValid() || Message->HasField(TEXT("id"))
			|| !Message->TryGetStringField(TEXT("method"), Method) || Method != TEXT("notifications/cancelled")
			|| !Message->TryGetStringField(TEXT("jsonrpc"), Version) || Version != TEXT("2.0")) return false;
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!Message->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid()) return false;
		OutKey = RequestKey((*Params)->TryGetField(TEXT("requestId")));
		return !OutKey.IsEmpty();
	}

	void SendJsonError(const FHttpResultCallback& OnComplete, EHttpServerResponseCodes Code, const TCHAR* ErrorCode, const TCHAR* Message)
	{
		OnComplete(FHttpServerResponse::Error(Code, ErrorCode, Message));
	}

	void SendProtocolRefusal(const TArray<TSharedPtr<FJsonValue>>& Messages, bool bBatch,
		const FString& Reason, const FHttpResultCallback& OnComplete)
	{
		TArray<TSharedPtr<FJsonValue>> Replies;
		for (const auto& Value : Messages)
		{
			const auto Message = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
			FString Method;
			if (Message.IsValid() && !Message->HasField(TEXT("id"))
				&& (Message->TryGetStringField(TEXT("method"), Method) || Message->HasField(TEXT("result")) || Message->HasField(TEXT("error")))) continue;
			const auto Id = Message.IsValid() ? Message->TryGetField(TEXT("id")) : nullptr;
			auto Error = MakeShared<FJsonObject>();
			Error->SetNumberField(TEXT("code"), FKismetAIMcpServer::InvalidRequest);
			Error->SetStringField(TEXT("message"), Reason);
			auto Reply = MakeShared<FJsonObject>();
			Reply->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
			Reply->SetField(TEXT("id"), RequestKey(Id).IsEmpty() ? MakeShared<FJsonValueNull>() : Id);
			Reply->SetObjectField(TEXT("error"), Error);
			Replies.Add(MakeShared<FJsonValueObject>(Reply));
		}
		if (Replies.IsEmpty())
		{
			auto Accepted = FHttpServerResponse::Ok();
			Accepted->Code = EHttpServerResponseCodes::Accepted;
			OnComplete(MoveTemp(Accepted));
			return;
		}
		FString Text;
		const auto Writer = TJsonWriterFactory<>::Create(&Text);
		if (bBatch) FJsonSerializer::Serialize(Replies, Writer);
		else FJsonSerializer::Serialize(Replies[0]->AsObject().ToSharedRef(), Writer);
		OnComplete(FHttpServerResponse::Create(Text, TEXT("application/json")));
	}

	// Returns true when the request may proceed; otherwise has already answered.
	bool CheckMcpAccess(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
	{
		FKismetAIMcpRequestMeta Meta;
		if (Request.PeerAddress.IsValid())
		{
			Meta.PeerAddress = Request.PeerAddress->ToString(/*bAppendPort=*/false);
		}
		if (const TArray<FString>* Origins = Request.Headers.Find(TEXT("Origin")))
		{
			Meta.Origins = *Origins;
		}
		if (const TArray<FString>* Authorization = Request.Headers.Find(TEXT("Authorization")))
		{
			Meta.Authorization = *Authorization;
		}

		// Read per request (a few bytes) so deleting ~/.kismet/mcp_token rotates
		// the secret without an editor restart.
		const FString ExpectedToken = KismetAIMcpToken::GetOrCreateToken();

		switch (FKismetAIMcpServer::CheckAccess(Meta, ExpectedToken))
		{
		case EKismetAIMcpAccess::Allowed:
			return true;
		case EKismetAIMcpAccess::ForbiddenPeer:
			UE_LOG(LogKismetAIMcp, Warning, TEXT("Refused /mcp request from non-loopback peer '%s'"), *Meta.PeerAddress);
			SendJsonError(OnComplete, EHttpServerResponseCodes::Forbidden, TEXT("forbidden_peer"), TEXT("The MCP endpoint only accepts loopback connections"));
			return false;
		case EKismetAIMcpAccess::ForbiddenOrigin:
			UE_LOG(LogKismetAIMcp, Warning, TEXT("Refused /mcp request with non-local Origin"));
			SendJsonError(OnComplete, EHttpServerResponseCodes::Forbidden, TEXT("origin_forbidden"), TEXT("Origin must be absent, null, or a localhost origin"));
			return false;
		case EKismetAIMcpAccess::NotConfigured:
			SendJsonError(OnComplete, EHttpServerResponseCodes::ServerError, TEXT("auth_not_initialized"), TEXT("MCP token could not be created; see the editor log"));
			return false;
		case EKismetAIMcpAccess::Unauthorized:
		default:
			// Never log the presented or expected token.
			UE_LOG(LogKismetAIMcp, Verbose, TEXT("Refused /mcp request: missing or invalid bearer token"));
			SendJsonError(OnComplete, EHttpServerResponseCodes::Denied, TEXT("unauthorized"), TEXT("Missing or invalid 'Authorization: Bearer <token>' (token is in ~/.kismet/mcp_token)"));
			return false;
		}
	}
}

FString KismetAIMcpRoute::CallTool(FKismetAIModule& Module, const FKismetAICallerContext& Caller,
	const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments)
{
	FKismetAIAgent Agent(MakeUnique<FKismetAIModuleToolInvoker>(Module), MakeExternalAgentConfig(Module));
	Agent.SetCallerContext(Caller);
	return Agent.ExecuteToolForExternalClient(ToolName, Arguments);
}

TArray<TSharedPtr<FJsonValue>> KismetAIMcpRoute::GetToolSchemas(FKismetAIModule& Module)
{
	FKismetAIAgent SchemaAgent(MakeUnique<FKismetAIModuleToolInvoker>(Module), MakeExternalAgentConfig(Module));
	return SchemaAgent.GetToolSchemasForExternalClient();
}

void KismetAIMcpRoute::Bind(FKismetAIModule& Module, const TSharedPtr<FKismetAIHttpTransport>& Router, TArray<FHttpRouteHandle>& OutHandles)
{
	if (!Router.IsValid())
	{
		return;
	}

	// BindRoute returns null when the route already exists; StopHttpServer
	// ensure()s on every handle it unbinds, so keep only real ones.
	auto AddHandle = [&OutHandles](const FHttpRouteHandle& Handle)
	{
		if (Handle.IsValid())
		{
			OutHandles.Add(Handle);
		}
		else
		{
			UE_LOG(LogKismetAIMcp, Warning, TEXT("Could not bind an /mcp route (already bound?)"));
		}
	};

	TSharedPtr<FMcpRouteState> State = MakeShared<FMcpRouteState>();
	State->Module = &Module;

	// Source-only requests use owned bounded jobs. Other editor tools retain
	// synchronous GT execution; mixed tool batches are refused explicitly.
	AddHandle(Router->BindRoute(
		FHttpPath(TEXT("/mcp")),
		EHttpServerRequestVerbs::VERB_POST,
		FHttpRequestHandler::CreateLambda([State](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) -> bool
		{
			if (!CheckMcpAccess(Request, OnComplete))
			{
				return true;
			}
			if (Request.Body.Num() > MaxMcpBodyBytes)
			{
				SendJsonError(OnComplete, EHttpServerResponseCodes::RequestTooLarge, TEXT("body_too_large"), TEXT("Request body exceeds 1 MiB cap"));
				return true;
			}

			const FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
			const FString Body(Converter.Length(), Converter.Get());

			TSharedPtr<FJsonValue> Root;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Root);
			TArray<TSharedPtr<FJsonValue>> Messages;
			if (Root.IsValid() && Root->Type == EJson::Array) Messages = Root->AsArray();
			else if (Root.IsValid()) Messages.Add(Root);
			if (Messages.Num() > KismetAISourceContract::MaxMcpBatch)
			{
				OnComplete(FHttpServerResponse::Create(
					FString(TEXT("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32600,\"message\":\"Batch exceeds 32 messages\"}}")), TEXT("application/json")));
				return true;
			}

			FKismetAISourceJobs& Jobs = State->Module->GetSourceJobs();
			TArray<FString> RequestIds;
			TArray<FString> CancelKeys;
			bool bOnlyCancellations = Messages.Num() > 0;
			bool bHasSource = false;
			bool bHasOtherTool = false;
			for (const TSharedPtr<FJsonValue>& MessageValue : Messages)
			{
				const TSharedPtr<FJsonObject> Message = MessageValue->Type == EJson::Object ? MessageValue->AsObject() : nullptr;
				FString CancelKey;
				if (IsCancellation(Message, CancelKey))
				{
					CancelKeys.Add(CancelKey);
					continue;
				}
				bOnlyCancellations = false;
				FString Method, ToolName;
				const TSharedPtr<FJsonObject>* Params = nullptr;
				if (Message.IsValid() && Message->TryGetStringField(TEXT("method"), Method) && Method == TEXT("tools/call")
					&& Message->TryGetObjectField(TEXT("params"), Params) && Params && Params->IsValid())
				{
					(*Params)->TryGetStringField(TEXT("name"), ToolName);
					bHasSource |= ToolName == TEXT("read_source");
					bHasOtherTool |= ToolName != TEXT("read_source");
				}
				const FString Id = Message.IsValid() ? RequestKey(Message->TryGetField(TEXT("id"))) : FString();
				if (!Id.IsEmpty())
				{
					if (State->PendingIds.Contains(Id) || RequestIds.Contains(Id))
					{
						SendProtocolRefusal(Messages, Root.IsValid() && Root->Type == EJson::Array, TEXT("A request with this id is already pending"), OnComplete);
						return true;
					}
					RequestIds.Add(Id);
				}
			}
			if (bHasSource && bHasOtherTool)
			{
				SendProtocolRefusal(Messages, Root.IsValid() && Root->Type == EJson::Array,
					TEXT("Send source reads separately from other tool calls"), OnComplete);
				return true;
			}
			for (const FString& CancelKey : CancelKeys)
			{
				if (const FString* JobKey = State->PendingIds.Find(CancelKey)) Jobs.Cancel(*JobKey);
			}
			if (bOnlyCancellations)
			{
				auto Accepted = FHttpServerResponse::Ok();
				Accepted->Code = EHttpServerResponseCodes::Accepted;
				OnComplete(MoveTemp(Accepted));
				return true;
			}
			if (!bHasSource)
			{
				bool bNoContent = false;
				const FString ResponseBody = State->GetSynchronousServer().HandleJsonRpc(Body, bNoContent);
				auto Response = bNoContent ? FHttpServerResponse::Ok() : FHttpServerResponse::Create(ResponseBody, TEXT("application/json"));
				Response->Code = bNoContent ? EHttpServerResponseCodes::Accepted : EHttpServerResponseCodes::Ok;
				OnComplete(MoveTemp(Response));
				return true;
			}
			if (Jobs.NumActive() >= KismetAISourceContract::MaxJobs)
			{
				SendJsonError(OnComplete, static_cast<EHttpServerResponseCodes>(429), TEXT("SOURCE_JOBS_FULL"), TEXT("The source request queue is full; retry after a request finishes"));
				return true;
			}

			const auto Agent = MakeShared<FKismetAIAgent, ESPMode::ThreadSafe>(
				MakeUnique<FKismetAIModuleToolInvoker>(*State->Module), MakeExternalAgentConfig(*State->Module));
			Agent->SetCallerContext(FKismetAICallerContext::Verified(EKismetAIEntryPoint::Mcp, TEXT("mcp-bearer")));
			FString SchemaJson;
			FJsonSerializer::Serialize(KismetAIMcpRoute::GetToolSchemas(*State->Module), TJsonWriterFactory<>::Create(&SchemaJson));
			const FString Version = GetPluginVersion();
			const FString JobKey = TEXT("mcp:") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			const auto Submitted = Jobs.Submit(JobKey,
				[Agent, Body, SchemaJson, Version](const FKismetAICancellationToken& Token)
				{
					// Keep cancellation connected through GT delivery or Close, including
					// after this worker returns. Job-token destruction releases the listener;
					// Agent owns a separate run token, so this ownership has no cycle.
					Token.Subscribe([Agent] { Agent->Abort(); });
					TArray<TSharedPtr<FJsonValue>> Schemas;
					FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SchemaJson), Schemas);
					FKismetAIMcpServer Server(Schemas, FKismetAIModuleToolInvoker::DispatchableToolNames(),
						[Agent](const FString& ToolName, const TSharedPtr<FJsonObject>& Args)
						{
							if (ToolName != TEXT("read_source")) return FToolExecutionResult::Error(400, TEXT("MIXED_SOURCE_BATCH"), TEXT("Send source reads separately from other tool calls")).ToModelString();
							return Agent->ExecuteToolForExternalClient(ToolName, Args);
						}, Version);
					FKismetAISourceJobs::FResult Result;
					Result.Text = Server.HandleJsonRpc(Body, Result.bNoContent);
					return Result;
				},
				[State, RequestIds, OnComplete](FKismetAISourceJobs::FResult&& Result)
				{
					for (const FString& Id : RequestIds) State->PendingIds.Remove(Id);
					if (Result.bNoContent)
					{
						auto Accepted = FHttpServerResponse::Ok();
						Accepted->Code = EHttpServerResponseCodes::Accepted;
						OnComplete(MoveTemp(Accepted));
					}
					else
					{
						auto Response = FHttpServerResponse::Create(Result.Text, TEXT("application/json"));
						if (Result.bCancelled) Response->Headers.Add(TEXT("X-Kismet-Cancel-Requested"), { TEXT("true") });
						OnComplete(MoveTemp(Response));
					}
				});
			if (Submitted == FKismetAISourceJobs::ESubmit::Started)
			{
				for (const FString& Id : RequestIds) State->PendingIds.Add(Id, JobKey);
			}
			else SendJsonError(OnComplete, static_cast<EHttpServerResponseCodes>(503), TEXT("SOURCE_JOBS_CLOSED"), TEXT("The source request service is unavailable"));
			return true;
		})));

	// The transport lets a server decline both: no SSE stream for
	// server-initiated messages, and no session to delete. Bound one verb per
	// route so UnbindRoute's per-verb lookup finds exactly the handle it owns.
	auto MethodNotAllowed = FHttpRequestHandler::CreateLambda([](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete) -> bool
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Error(
			EHttpServerResponseCodes::BadMethod, TEXT("method_not_allowed"), TEXT("Use POST for MCP JSON-RPC"));
		Response->Headers.Add(TEXT("Allow"), { TEXT("POST") });
		OnComplete(MoveTemp(Response));
		return true;
	});
	AddHandle(Router->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_GET, MethodNotAllowed));
	AddHandle(Router->BindRoute(FHttpPath(TEXT("/mcp")), EHttpServerRequestVerbs::VERB_DELETE, MethodNotAllowed));
}
