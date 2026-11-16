// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMcpServer.h"
#include "KismetAIToolRegistry.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* const ToolErrorOpen = TEXT("<tool_use_error>");
	const TCHAR* const ToolErrorClose = TEXT("</tool_use_error>");

	// Shown to the client's model once, at initialize. Short on purpose: the tool
	// descriptions carry the detail, this only sets the working order.
	const TCHAR* const ServerInstructions =
		TEXT("KismetAI drives the open Unreal Editor. Work in a read -> edit -> compile -> verify loop: ")
		TEXT("read_blueprint (mode describe/overview/t3d) before changing a graph, prefer insert_node and ")
		TEXT("edit_blueprint over hand-written T3D, call compile_blueprint after every change and fix what it ")
		TEXT("reports. Asset paths look like /Game/Folder/BP_Name. Tools run on the editor's game thread one ")
		TEXT("at a time.");

	// JSON-RPC answers an unreadable id with an explicit null.
	TSharedPtr<FJsonValue> IdOrNull(const TSharedPtr<FJsonValue>& Id)
	{
		if (Id.IsValid())
		{
			return Id;
		}
		return MakeShared<FJsonValueNull>();
	}

	FString SerializeCondensed(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	FString SerializeCondensed(const TArray<TSharedPtr<FJsonValue>>& Arr)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Arr, Writer);
		return Out;
	}

	// Non-empty and nothing but ASCII digits. FString::IsNumeric also accepts a
	// sign and a decimal point, which no address octet or port may carry.
	bool IsAllDigits(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Ch : Text)
		{
			if (Ch < TEXT('0') || Ch > TEXT('9'))
			{
				return false;
			}
		}
		return true;
	}

	// Dotted-quad IPv4 whose first octet is 127.
	bool IsIPv4Loopback(const FString& Address)
	{
		TArray<FString> Octets;
		Address.ParseIntoArray(Octets, TEXT("."), /*InCullEmpty=*/false);
		if (Octets.Num() != 4)
		{
			return false;
		}
		for (const FString& Octet : Octets)
		{
			if (Octet.Len() > 3 || !IsAllDigits(Octet))
			{
				return false;
			}
			if (FCString::Atoi(*Octet) > 255)
			{
				return false;
			}
		}
		return FCString::Atoi(*Octets[0]) == 127;
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction: derive the exposed tool set
// ─────────────────────────────────────────────────────────────────────────────

FKismetAIMcpServer::FKismetAIMcpServer(
	const TArray<TSharedPtr<FJsonValue>>& ToolSchemas,
	const TSet<FString>& DispatchableToolNames,
	FKismetAIMcpToolCallFn InCallTool,
	const FString& InServerVersion)
	: CallTool(MoveTemp(InCallTool))
	, ServerVersion(InServerVersion.IsEmpty() ? FString(TEXT("0.0.0")) : InServerVersion)
{
	TMap<FString, TSharedPtr<FJsonObject>> EntriesByName;

	for (const TSharedPtr<FJsonValue>& SchemaValue : ToolSchemas)
	{
		const TSharedPtr<FJsonObject> Schema = SchemaValue.IsValid() ? SchemaValue->AsObject() : nullptr;
		if (!Schema.IsValid())
		{
			continue;
		}

		FString Name;
		if (!Schema->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty() || EntriesByName.Contains(Name))
		{
			continue;
		}

		// Exposed = has a schema AND the editor can run it AND it is not one of
		// the agent's own in-process tools.
		const FKismetAIToolDefinition* Def = FKismetAIToolRegistry::Find(Name);
		if (!DispatchableToolNames.Contains(Name) || !Def || Def->bIsInProcess)
		{
			continue;
		}

		FString Description;
		Schema->TryGetStringField(TEXT("description"), Description);

		const TSharedPtr<FJsonObject>* InputSchemaPtr = nullptr;
		TSharedPtr<FJsonObject> InputSchema;
		if (Schema->TryGetObjectField(TEXT("input_schema"), InputSchemaPtr) && InputSchemaPtr && InputSchemaPtr->IsValid())
		{
			InputSchema = *InputSchemaPtr;
		}
		else
		{
			InputSchema = MakeShared<FJsonObject>();
			InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		}

		const FString Title = MakeToolTitle(Name);

		// readOnlyHint follows capability, not bIsIdempotentRead: read_logs and the
		// discovery tools are Inspect (they change nothing) but repeat-sensitive,
		// and a client should still treat them as safe to auto-approve.
		const bool bReadOnly = Def->Capability == EToolCapability::Inspect;
		TSharedPtr<FJsonObject> Annotations = MakeShared<FJsonObject>();
		Annotations->SetStringField(TEXT("title"), Title);
		Annotations->SetBoolField(TEXT("readOnlyHint"), bReadOnly);
		if (!bReadOnly)
		{
			// Only meaningful when readOnlyHint is false. compile_blueprint and
			// layout_graph write but do not destroy, so they are not flagged.
			Annotations->SetBoolField(TEXT("destructiveHint"), Def->bIsMutation);
		}
		Annotations->SetBoolField(TEXT("openWorldHint"), false);

		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Name);
		Entry->SetStringField(TEXT("title"), Title);
		Entry->SetStringField(TEXT("description"), Description);
		Entry->SetObjectField(TEXT("inputSchema"), InputSchema);
		Entry->SetObjectField(TEXT("annotations"), Annotations);
		EntriesByName.Add(Name, Entry);
	}

	EntriesByName.GetKeys(ExposedToolNames);
	ExposedToolNames.Sort();
	for (const FString& Name : ExposedToolNames)
	{
		ExposedToolSet.Add(Name);
		ToolListEntries.Add(MakeShared<FJsonValueObject>(EntriesByName.FindChecked(Name)));
	}
}

const TArray<FString>& FKismetAIMcpServer::GetSupportedProtocolVersions()
{
	// Newest first. Every revision here shares the same initialize / tools/list /
	// tools/call message shapes for what this server implements.
	static const TArray<FString> Versions = {
		TEXT("2025-11-25"),
		TEXT("2025-06-18"),
		TEXT("2025-03-26"),
		TEXT("2024-11-05"),
	};
	return Versions;
}

// ─────────────────────────────────────────────────────────────────────────────
// JSON-RPC
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAIMcpServer::HandleJsonRpc(const FString& Body, bool& bOutNoContent)
{
	bOutNoContent = false;

	TSharedPtr<FJsonValue> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return SerializeCondensed(MakeError(nullptr, ParseError, TEXT("Parse error")));
	}

	if (Root->Type == EJson::Array)
	{
		const TArray<TSharedPtr<FJsonValue>>& Batch = Root->AsArray();
		if (Batch.Num() == 0)
		{
			return SerializeCondensed(MakeError(nullptr, InvalidRequest, TEXT("Empty batch")));
		}

		TArray<TSharedPtr<FJsonValue>> Responses;
		for (const TSharedPtr<FJsonValue>& Message : Batch)
		{
			if (TSharedPtr<FJsonObject> Response = HandleMessage(Message))
			{
				Responses.Add(MakeShared<FJsonValueObject>(Response));
			}
		}
		if (Responses.Num() == 0)
		{
			bOutNoContent = true;
			return FString();
		}
		return SerializeCondensed(Responses);
	}

	TSharedPtr<FJsonObject> Response = HandleMessage(Root);
	if (!Response.IsValid())
	{
		bOutNoContent = true;
		return FString();
	}
	return SerializeCondensed(Response);
}

TSharedPtr<FJsonObject> FKismetAIMcpServer::HandleMessage(const TSharedPtr<FJsonValue>& Message)
{
	const TSharedPtr<FJsonObject> Obj = (Message.IsValid() && Message->Type == EJson::Object) ? Message->AsObject() : nullptr;
	if (!Obj.IsValid())
	{
		return MakeError(nullptr, InvalidRequest, TEXT("Invalid Request"));
	}

	const bool bHasId = Obj->HasField(TEXT("id"));
	TSharedPtr<FJsonValue> Id = Obj->TryGetField(TEXT("id"));
	if (Id.IsValid() && Id->Type != EJson::String && Id->Type != EJson::Number && Id->Type != EJson::Null)
	{
		return MakeError(nullptr, InvalidRequest, TEXT("Invalid Request: id must be a string or number"));
	}

	FString Method;
	if (!Obj->TryGetStringField(TEXT("method"), Method))
	{
		// A response to a server-initiated request. This server never sends any,
		// so there is nothing to correlate; acknowledge and drop.
		if (Obj->HasField(TEXT("result")) || Obj->HasField(TEXT("error")))
		{
			return nullptr;
		}
		return MakeError(Id, InvalidRequest, TEXT("Invalid Request: missing method"));
	}

	// Notifications (no id) never get a reply, even malformed or unknown ones.
	// notifications/initialized and notifications/cancelled need no action: every
	// call here is synchronous, so there is nothing left to cancel by the time a
	// cancellation could arrive.
	if (!bHasId)
	{
		return nullptr;
	}

	FString JsonRpcVersion;
	if (!Obj->TryGetStringField(TEXT("jsonrpc"), JsonRpcVersion) || JsonRpcVersion != TEXT("2.0"))
	{
		return MakeError(Id, InvalidRequest, TEXT("Invalid Request: jsonrpc must be \"2.0\""));
	}

	TSharedPtr<FJsonObject> Params;
	if (const TSharedPtr<FJsonValue> ParamsValue = Obj->TryGetField(TEXT("params")))
	{
		if (ParamsValue->Type == EJson::Object)
		{
			Params = ParamsValue->AsObject();
		}
		else if (ParamsValue->Type != EJson::Null)
		{
			return MakeError(Id, InvalidParams, TEXT("params must be an object"));
		}
	}
	if (!Params.IsValid())
	{
		Params = MakeShared<FJsonObject>();
	}

	if (Method == TEXT("initialize"))
	{
		return MakeResult(Id, HandleInitialize(Params));
	}
	if (Method == TEXT("ping"))
	{
		return MakeResult(Id, MakeShared<FJsonObject>());
	}
	if (Method == TEXT("tools/list"))
	{
		// No pagination: the whole catalogue is a few dozen entries.
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), ToolListEntries);
		return MakeResult(Id, Result);
	}
	if (Method == TEXT("tools/call"))
	{
		FString ErrorMessage;
		TSharedPtr<FJsonObject> Result = HandleToolsCall(Params, ErrorMessage);
		if (!Result.IsValid())
		{
			return MakeError(Id, ErrorMessage.IsEmpty() ? InternalError : InvalidParams,
				ErrorMessage.IsEmpty() ? TEXT("Internal error") : ErrorMessage);
		}
		return MakeResult(Id, Result);
	}

	return MakeError(Id, MethodNotFound, FString::Printf(TEXT("Method not found: %s"), *Method));
}

TSharedPtr<FJsonObject> FKismetAIMcpServer::HandleInitialize(const TSharedPtr<FJsonObject>& Params) const
{
	const TArray<FString>& Supported = GetSupportedProtocolVersions();

	FString Requested;
	Params->TryGetStringField(TEXT("protocolVersion"), Requested);
	const FString Negotiated = Supported.Contains(Requested) ? Requested : Supported[0];

	TSharedPtr<FJsonObject> Tools = MakeShared<FJsonObject>();
	Tools->SetBoolField(TEXT("listChanged"), false);
	TSharedPtr<FJsonObject> Capabilities = MakeShared<FJsonObject>();
	Capabilities->SetObjectField(TEXT("tools"), Tools);

	TSharedPtr<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
	ServerInfo->SetStringField(TEXT("name"), TEXT("kismetai"));
	ServerInfo->SetStringField(TEXT("title"), TEXT("KismetAI"));
	ServerInfo->SetStringField(TEXT("version"), ServerVersion);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("protocolVersion"), Negotiated);
	Result->SetObjectField(TEXT("capabilities"), Capabilities);
	Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
	Result->SetStringField(TEXT("instructions"), ServerInstructions);
	return Result;
}

TSharedPtr<FJsonObject> FKismetAIMcpServer::HandleToolsCall(const TSharedPtr<FJsonObject>& Params, FString& OutErrorMessage)
{
	FString Name;
	if (!Params->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		OutErrorMessage = TEXT("tools/call requires a string 'name'");
		return nullptr;
	}

	// Unknown tools are a protocol error (-32602), as in the MCP spec's own
	// example, rather than an isError result: the client asked for something
	// tools/list never offered, which is a caller bug, not a tool failure.
	if (!ExposedToolSet.Contains(Name))
	{
		OutErrorMessage = FString::Printf(TEXT("Unknown tool: %s"), *Name);
		return nullptr;
	}

	TSharedPtr<FJsonObject> Arguments;
	if (const TSharedPtr<FJsonValue> ArgsValue = Params->TryGetField(TEXT("arguments")))
	{
		if (ArgsValue->Type == EJson::Object)
		{
			Arguments = ArgsValue->AsObject();
		}
		else if (ArgsValue->Type != EJson::Null)
		{
			OutErrorMessage = TEXT("tools/call 'arguments' must be an object");
			return nullptr;
		}
	}
	if (!Arguments.IsValid())
	{
		Arguments = MakeShared<FJsonObject>();
	}

	if (!CallTool)
	{
		// Leaves OutErrorMessage empty -> InternalError.
		return nullptr;
	}

	FString Text;
	bool bIsError = false;
	ShapeToolResult(CallTool(Name, Arguments), Text, bIsError);

	TSharedPtr<FJsonObject> TextBlock = MakeShared<FJsonObject>();
	TextBlock->SetStringField(TEXT("type"), TEXT("text"));
	TextBlock->SetStringField(TEXT("text"), Text);

	TArray<TSharedPtr<FJsonValue>> Content;
	Content.Add(MakeShared<FJsonValueObject>(TextBlock));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("content"), Content);
	Result->SetBoolField(TEXT("isError"), bIsError);
	return Result;
}

TSharedPtr<FJsonObject> FKismetAIMcpServer::MakeResult(const TSharedPtr<FJsonValue>& Id, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), IdOrNull(Id));
	TSharedPtr<FJsonObject> ResultObject = Result;
	if (!ResultObject.IsValid())
	{
		ResultObject = MakeShared<FJsonObject>();
	}
	Response->SetObjectField(TEXT("result"), ResultObject);
	return Response;
}

TSharedPtr<FJsonObject> FKismetAIMcpServer::MakeError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Message)
{
	TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), Code);
	Error->SetStringField(TEXT("message"), Message);

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	Response->SetField(TEXT("id"), IdOrNull(Id));
	Response->SetObjectField(TEXT("error"), Error);
	return Response;
}

// ─────────────────────────────────────────────────────────────────────────────
// Result shaping
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIMcpServer::ShapeToolResult(const FString& RawResult, FString& OutText, bool& bOutIsError)
{
	const FString Trimmed = RawResult.TrimStartAndEnd();

	if (Trimmed.StartsWith(ToolErrorOpen, ESearchCase::CaseSensitive))
	{
		// Truncation can append a note after the closing tag, so remove the tag
		// wherever it is rather than requiring it at the very end.
		FString Inner = Trimmed.RightChop(FCString::Strlen(ToolErrorOpen));
		const int32 CloseIndex = Inner.Find(ToolErrorClose, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (CloseIndex != INDEX_NONE)
		{
			Inner = Inner.Left(CloseIndex) + Inner.RightChop(CloseIndex + FCString::Strlen(ToolErrorClose));
		}
		OutText = Inner.TrimStartAndEnd();
		bOutIsError = true;
		return;
	}

	OutText = RawResult;
	bOutIsError = false;

	// Handlers report failure through the wrapper, but an unwrapped envelope
	// with "success":false is still a failure as far as the caller is concerned.
	if (Trimmed.StartsWith(TEXT("{")))
	{
		TSharedPtr<FJsonObject> Parsed;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		bool bSuccess = true;
		if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid()
			&& Parsed->TryGetBoolField(TEXT("success"), bSuccess) && !bSuccess)
		{
			bOutIsError = true;
		}
	}
}

FString FKismetAIMcpServer::MakeToolTitle(const FString& ToolName)
{
	TArray<FString> Words;
	ToolName.ParseIntoArray(Words, TEXT("_"), /*InCullEmpty=*/true);
	for (FString& Word : Words)
	{
		if (Word.Len() > 0)
		{
			Word[0] = FChar::ToUpper(Word[0]);
		}
	}
	return FString::Join(Words, TEXT(" "));
}

// ─────────────────────────────────────────────────────────────────────────────
// Access checks
// ─────────────────────────────────────────────────────────────────────────────

EKismetAIMcpAccess FKismetAIMcpServer::CheckAccess(const FKismetAIMcpRequestMeta& Meta, const FString& ExpectedToken)
{
	if (!IsLoopbackAddress(Meta.PeerAddress))
	{
		return EKismetAIMcpAccess::ForbiddenPeer;
	}
	if (!IsOriginAllowed(Meta.Origins))
	{
		return EKismetAIMcpAccess::ForbiddenOrigin;
	}
	if (ExpectedToken.IsEmpty())
	{
		return EKismetAIMcpAccess::NotConfigured;
	}
	if (!IsAuthorized(Meta.Authorization, ExpectedToken))
	{
		return EKismetAIMcpAccess::Unauthorized;
	}
	return EKismetAIMcpAccess::Allowed;
}

bool FKismetAIMcpServer::IsLoopbackAddress(const FString& PeerAddress)
{
	FString Address = PeerAddress.TrimStartAndEnd().ToLower();
	if (Address.StartsWith(TEXT("[")) && Address.EndsWith(TEXT("]")))
	{
		Address = Address.Mid(1, Address.Len() - 2);
	}
	// Drop an IPv6 zone index ("::1%lo0").
	int32 ZoneIndex = INDEX_NONE;
	if (Address.FindChar(TEXT('%'), ZoneIndex))
	{
		Address.LeftInline(ZoneIndex);
	}
	if (Address.IsEmpty())
	{
		return false;
	}

	if (Address == TEXT("::1") || Address == TEXT("0:0:0:0:0:0:0:1"))
	{
		return true;
	}

	// IPv4-mapped IPv6: a dual-stack listener reports 127.0.0.1 this way.
	static const TCHAR* const MappedPrefixes[] = { TEXT("::ffff:"), TEXT("0:0:0:0:0:ffff:") };
	for (const TCHAR* Prefix : MappedPrefixes)
	{
		if (Address.StartsWith(Prefix))
		{
			return IsIPv4Loopback(Address.RightChop(FCString::Strlen(Prefix)));
		}
	}

	return IsIPv4Loopback(Address);
}

bool FKismetAIMcpServer::IsOriginAllowed(const TArray<FString>& Origins)
{
	for (const FString& RawOrigin : Origins)
	{
		const FString Origin = RawOrigin.TrimStartAndEnd().ToLower();
		if (Origin.IsEmpty() || Origin == TEXT("null"))
		{
			continue;
		}

		FString Rest;
		if (Origin.StartsWith(TEXT("http://")))
		{
			Rest = Origin.RightChop(7);
		}
		else if (Origin.StartsWith(TEXT("https://")))
		{
			Rest = Origin.RightChop(8);
		}
		else
		{
			return false;
		}

		// An origin has no path, but tolerate a trailing slash.
		int32 SlashIndex = INDEX_NONE;
		if (Rest.FindChar(TEXT('/'), SlashIndex))
		{
			Rest.LeftInline(SlashIndex);
		}
		// Userinfo has no place in an origin; refuse rather than parse around it.
		if (Rest.Contains(TEXT("@")))
		{
			return false;
		}

		FString Host;
		FString Port;
		if (Rest.StartsWith(TEXT("[")))
		{
			int32 CloseIndex = INDEX_NONE;
			if (!Rest.FindChar(TEXT(']'), CloseIndex))
			{
				return false;
			}
			Host = Rest.Left(CloseIndex + 1);
			Port = Rest.RightChop(CloseIndex + 1);
		}
		else
		{
			int32 ColonIndex = INDEX_NONE;
			if (Rest.FindChar(TEXT(':'), ColonIndex))
			{
				Host = Rest.Left(ColonIndex);
				Port = Rest.RightChop(ColonIndex);
			}
			else
			{
				Host = Rest;
			}
		}

		if (!Port.IsEmpty() && (!Port.StartsWith(TEXT(":")) || !IsAllDigits(Port.RightChop(1))))
		{
			return false;
		}

		if (Host != TEXT("localhost") && Host != TEXT("127.0.0.1") && Host != TEXT("[::1]"))
		{
			return false;
		}
	}
	return true;
}

bool FKismetAIMcpServer::IsAuthorized(const TArray<FString>& AuthorizationHeaders, const FString& ExpectedToken)
{
	// Exactly one header: two values means a proxy or client is confused, and
	// picking one of them would be guessing.
	if (ExpectedToken.IsEmpty() || AuthorizationHeaders.Num() != 1)
	{
		return false;
	}

	const FString Value = AuthorizationHeaders[0].TrimStartAndEnd();
	static const FString Scheme = TEXT("Bearer ");
	if (!Value.StartsWith(Scheme, ESearchCase::IgnoreCase))
	{
		return false;
	}
	const FString Presented = Value.RightChop(Scheme.Len()).TrimStartAndEnd();
	return !Presented.IsEmpty() && ConstantTimeEquals(Presented, ExpectedToken);
}

bool FKismetAIMcpServer::ConstantTimeEquals(const FString& A, const FString& B)
{
	// Walk the longer of the two so the loop length does not depend on where
	// (or whether) the strings first differ.
	const int32 LenA = A.Len();
	const int32 LenB = B.Len();
	const int32 MaxLen = FMath::Max(LenA, LenB);
	uint32 Diff = static_cast<uint32>(LenA ^ LenB);
	for (int32 Index = 0; Index < MaxLen; ++Index)
	{
		const uint32 CharA = Index < LenA ? static_cast<uint32>(A[Index]) : 0u;
		const uint32 CharB = Index < LenB ? static_cast<uint32>(B[Index]) : 0u;
		Diff |= CharA ^ CharB;
	}
	return Diff == 0;
}
