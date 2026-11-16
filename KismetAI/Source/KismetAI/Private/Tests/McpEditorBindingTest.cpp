// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// PH5: the editor-side facts the MCP server depends on — the real tool
// catalogue (shipped schemas x the real dispatch table) and the per-install
// token file. Protocol behavior is covered by KismetAIRuntime's McpServerTest.

#include "KismetAIAgent.h"
#include "KismetAIMcpServer.h"
#include "KismetAIMcpToken.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAIToolInvoker.h"
#include "ScopedTempHome.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

constexpr auto McpEditorTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	// Only here to construct an FKismetAIAgent for its schema loader; never
	// invoked. Advertises the real dispatch table so nothing is faked that the
	// test asserts on.
	class FCatalogOnlyInvoker final : public IKismetAIToolInvoker
	{
	public:
		virtual FKismetAIInvokeResult Invoke(const FKismetAIToolInvocation& Invocation) override
		{
			return FKismetAIInvokeResult::FromModelString(TEXT("<tool_use_error>{\"success\":false}</tool_use_error>"));
		}
		virtual TSet<FString> GetDispatchableToolNames() const override
		{
			return FKismetAIModuleToolInvoker::DispatchableToolNames();
		}
		virtual FString ResolveHandlerName(const FString& ToolName, const FString& Mode) const override
		{
			return FKismetAIModuleToolInvoker::HandlerNameFor(ToolName, Mode);
		}
	};

	const TCHAR* const McpAgentLocalTools[] = {
		TEXT("enter_plan_mode"),
		TEXT("exit_plan_mode"),
		TEXT("ask_user"),
		TEXT("update_todos"),
		TEXT("discover_tools"),
	};
}

// tools/list over the shipped schemas and the real dispatch table: every editor
// tool the invoker can run is offered, none of the agent-local ones are, and
// each entry carries an object inputSchema.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpRealCatalogTest, "KismetAI.Mcp.RealCatalog", McpEditorTestFlags)
bool FKismetAIMcpRealCatalogTest::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config;
	FKismetAIAgent Agent(MakeUnique<FCatalogOnlyInvoker>(), Config);
	const TArray<TSharedPtr<FJsonValue>> Schemas = Agent.GetToolSchemasForExternalClient();
	TestTrue(TEXT("loaded the shipped schemas"), Schemas.Num() >= 30);

	const TSet<FString> Dispatchable = FKismetAIModuleToolInvoker::DispatchableToolNames();
	FKismetAIMcpServer Server(Schemas, Dispatchable, nullptr, TEXT("test"));
	const TArray<FString>& Exposed = Server.GetExposedToolNames();

	for (const FString& Tool : Dispatchable)
	{
		TestTrue(FString::Printf(TEXT("dispatchable editor tool '%s' is exposed over MCP"), *Tool), Exposed.Contains(Tool));
	}
	for (const TCHAR* Local : McpAgentLocalTools)
	{
		TestFalse(FString::Printf(TEXT("agent-local tool '%s' is not exposed"), Local), Exposed.Contains(FString(Local)));
	}
	TestEqual(TEXT("exposed count == dispatchable count"), Exposed.Num(), Dispatchable.Num());

	bool bNoContent = false;
	const FString Body = Server.HandleJsonRpc(TEXT(R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})"), bNoContent);
	TSharedPtr<FJsonObject> Response;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!TestTrue(TEXT("tools/list parses"), FJsonSerializer::Deserialize(Reader, Response) && Response.IsValid()))
	{
		return false;
	}
	for (const TSharedPtr<FJsonValue>& ToolValue : Response->GetObjectField(TEXT("result"))->GetArrayField(TEXT("tools")))
	{
		const TSharedPtr<FJsonObject> Tool = ToolValue->AsObject();
		const FString Name = Tool->GetStringField(TEXT("name"));
		const TSharedPtr<FJsonObject>* InputSchema = nullptr;
		if (TestTrue(FString::Printf(TEXT("%s has inputSchema"), *Name), Tool->TryGetObjectField(TEXT("inputSchema"), InputSchema) && InputSchema))
		{
			TestEqual(FString::Printf(TEXT("%s inputSchema.type"), *Name), (*InputSchema)->GetStringField(TEXT("type")), FString(TEXT("object")));
		}
		TestFalse(FString::Printf(TEXT("%s has a description"), *Name), Tool->GetStringField(TEXT("description")).IsEmpty());
		TestTrue(FString::Printf(TEXT("%s has annotations"), *Name), Tool->HasTypedField<EJson::Object>(TEXT("annotations")));
	}
	return true;
}

// The token is created once, persisted owner-only, and stable across calls —
// a client configures it once. A garbage file is replaced, not trusted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIMcpTokenStoreTest, "KismetAI.Mcp.TokenStore", McpEditorTestFlags)
bool FKismetAIMcpTokenStoreTest::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("McpTokenTest"));

	const FString Path = KismetAIMcpToken::GetTokenPath();
	TestTrue(TEXT("token lives in the config dir"), Path.EndsWith(TEXT(".kismet/mcp_token")));
	TestFalse(TEXT("no token before first use"), IFileManager::Get().FileExists(*Path));

	const FString First = KismetAIMcpToken::GetOrCreateToken();
	TestEqual(TEXT("64 hex chars"), First.Len(), 64);
	TestTrue(TEXT("usable"), KismetAIMcpToken::IsUsableToken(First));
	TestTrue(TEXT("persisted"), IFileManager::Get().FileExists(*Path));

	FString OnDisk;
	FFileHelper::LoadFileToString(OnDisk, *Path);
	TestEqual(TEXT("file holds the token"), OnDisk.TrimStartAndEnd(), First);

	TestEqual(TEXT("stable across calls"), KismetAIMcpToken::GetOrCreateToken(), First);

#if PLATFORM_MAC || PLATFORM_LINUX
	struct stat St;
	FTCHARToUTF8 PathUtf8(*Path);
	if (TestEqual(TEXT("stat"), stat(PathUtf8.Get(), &St), 0))
	{
		TestEqual(TEXT("mode 0600"), static_cast<int32>(St.st_mode & 0777), 0600);
	}
#endif

	// A user-chosen token that meets the bar is kept.
	const FString Custom = TEXT("my-own-token_0123456789abcdefghijklmnop");
	FFileHelper::SaveStringToFile(Custom, *Path);
	TestEqual(TEXT("custom token kept"), KismetAIMcpToken::GetOrCreateToken(), Custom);

	// Garbage is replaced with a fresh random token.
	AddExpectedError(TEXT("not a usable token"), EAutomationExpectedErrorFlags::Contains, 1);
	FFileHelper::SaveStringToFile(TEXT("short"), *Path);
	const FString Replaced = KismetAIMcpToken::GetOrCreateToken();
	TestTrue(TEXT("replaced token usable"), KismetAIMcpToken::IsUsableToken(Replaced));
	TestNotEqual(TEXT("replaced token is new"), Replaced, First);

	TestFalse(TEXT("whitespace not usable"), KismetAIMcpToken::IsUsableToken(TEXT("0123456789abcdef 0123456789abcdef")));
	return true;
}
