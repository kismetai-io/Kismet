// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// The Anthropic codec's contract is round-trip fidelity.
//
// Every session file written by any shipped build must keep loading
// (00-PLAN.md §6.4), and the messages the model sees on resume must be
// byte-identical to what it saw the first time. So these tests decode real
// message shapes and assert the re-encoded JSON matches the input exactly —
// including the fields the typed struct does not model, like a thinking block's
// signature, which the API rejects the turn without.

#include "KismetAIAgentTypes.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

constexpr auto TypesTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace
{
	TSharedPtr<FJsonObject> ParseObject(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		FJsonSerializer::Deserialize(Reader, Obj);
		return Obj;
	}

	FString Serialize(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer
			= TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}

	// Decode then re-encode, and compare the canonical serialization of both.
	// Comparing serialized forms rather than raw input text keeps the assertion
	// about content rather than key ordering or whitespace.
	bool RoundTrips(const FString& InputJson, FString& OutBefore, FString& OutAfter)
	{
		const TSharedPtr<FJsonObject> In = ParseObject(InputJson);
		if (!In.IsValid()) { return false; }
		OutBefore = Serialize(In);

		FAgentMessage Msg;
		if (!KismetAIAgentCodec::FromAnthropicJson(In, Msg)) { return false; }
		OutAfter = Serialize(KismetAIAgentCodec::ToAnthropicJson(Msg));
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_PlainStringContent, "KismetAI.AgentTypes.PlainStringContent", TypesTestFlags)
bool FTypes_PlainStringContent::RunTest(const FString& Parameters)
{
	// The loop writes a bare-string user message when there are no images. If the
	// codec promoted it to a block array, resume would change the bytes sent.
	const FString Json = TEXT("{\"role\":\"user\",\"content\":\"add a print node\"}");
	FString Before, After;
	if (!RoundTrips(Json, Before, After)) { AddError(TEXT("decode failed")); return false; }

	TestEqual(TEXT("bare string survives as a bare string"), After, Before);

	FAgentMessage Msg;
	KismetAIAgentCodec::FromAnthropicJson(ParseObject(Json), Msg);
	TestTrue(TEXT("flagged as plain-string content"), Msg.bContentWasPlainString);
	TestEqual(TEXT("text is readable"), Msg.GetText(), FString(TEXT("add a print node")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_TextBlockArray, "KismetAI.AgentTypes.TextBlockArray", TypesTestFlags)
bool FTypes_TextBlockArray::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Done.\"}]}");
	FString Before, After;
	if (!RoundTrips(Json, Before, After)) { AddError(TEXT("decode failed")); return false; }
	TestEqual(TEXT("block array round-trips"), After, Before);
	return true;
}

// The one that matters most: a thinking block carries a `signature` the codec
// does not model. Losing it makes the API reject the next turn.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_ThinkingSignaturePreserved, "KismetAI.AgentTypes.ThinkingSignaturePreserved", TypesTestFlags)
bool FTypes_ThinkingSignaturePreserved::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"role\":\"assistant\",\"content\":[")
		TEXT("{\"type\":\"thinking\",\"thinking\":\"step one\",\"signature\":\"abc123SIG\"},")
		TEXT("{\"type\":\"text\",\"text\":\"Here goes.\"}]}");

	FString Before, After;
	if (!RoundTrips(Json, Before, After)) { AddError(TEXT("decode failed")); return false; }

	TestEqual(TEXT("thinking block round-trips whole"), After, Before);
	TestTrue(TEXT("signature survives"), After.Contains(TEXT("abc123SIG")));
	TestTrue(TEXT("the thinking payload survives"), After.Contains(TEXT("step one")));
	// A thinking block's payload lives under "thinking", not "text". Writing a
	// "text" field here would change the bytes sent to the API.
	TestFalse(TEXT("no spurious text field is added"), After.Contains(TEXT("\"text\":\"\"")));

	FAgentMessage Msg;
	KismetAIAgentCodec::FromAnthropicJson(ParseObject(Json), Msg);
	if (Msg.Parts.Num() != 2) { AddError(TEXT("expected two parts")); return false; }
	TestEqual(TEXT("thinking payload is readable as Text"), Msg.Parts[0].Text, FString(TEXT("step one")));
	// GetText() concatenates only real text parts, never thinking.
	TestEqual(TEXT("GetText ignores thinking"), Msg.GetText(), FString(TEXT("Here goes.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_ToolUseRoundTrip, "KismetAI.AgentTypes.ToolUseRoundTrip", TypesTestFlags)
bool FTypes_ToolUseRoundTrip::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"role\":\"assistant\",\"content\":[")
		TEXT("{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"read_blueprint\",")
		TEXT("\"input\":{\"blueprint\":\"/Game/BP_Player\",\"mode\":\"describe\"}}]}");

	FString Before, After;
	if (!RoundTrips(Json, Before, After)) { AddError(TEXT("decode failed")); return false; }
	TestEqual(TEXT("tool_use round-trips"), After, Before);

	FAgentMessage Msg;
	KismetAIAgentCodec::FromAnthropicJson(ParseObject(Json), Msg);
	TestTrue(TEXT("HasToolUse sees it"), Msg.HasToolUse());
	if (Msg.Parts.Num() != 1) { AddError(TEXT("expected one part")); return false; }
	TestEqual(TEXT("tool id decoded"), Msg.Parts[0].ToolUseId, FString(TEXT("toolu_01")));
	TestEqual(TEXT("tool name decoded"), Msg.Parts[0].ToolName, FString(TEXT("read_blueprint")));
	TestTrue(TEXT("tool input decoded"), Msg.Parts[0].ToolInput.IsValid());
	return true;
}

// is_error must be absent when false, not written as false — the loop omits it,
// and adding it would change the bytes of every successful tool result.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_ToolResultErrorFlag, "KismetAI.AgentTypes.ToolResultErrorFlag", TypesTestFlags)
bool FTypes_ToolResultErrorFlag::RunTest(const FString& Parameters)
{
	const FString Ok = TEXT("{\"role\":\"user\",\"content\":[")
		TEXT("{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_01\",\"content\":\"{\\\"success\\\":true}\"}]}");
	FString Before, After;
	if (!RoundTrips(Ok, Before, After)) { AddError(TEXT("decode failed")); return false; }
	TestEqual(TEXT("successful tool_result round-trips"), After, Before);
	TestFalse(TEXT("is_error is not introduced"), After.Contains(TEXT("is_error")));

	const FString Err = TEXT("{\"role\":\"user\",\"content\":[")
		TEXT("{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_02\",\"content\":\"boom\",\"is_error\":true}]}");
	if (!RoundTrips(Err, Before, After)) { AddError(TEXT("decode failed")); return false; }
	TestEqual(TEXT("failed tool_result round-trips"), After, Before);
	TestTrue(TEXT("is_error survives"), After.Contains(TEXT("\"is_error\":true")));
	return true;
}

// Images carry a nested `source` object the typed struct does not model.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_ImageSourcePreserved, "KismetAI.AgentTypes.ImageSourcePreserved", TypesTestFlags)
bool FTypes_ImageSourcePreserved::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"role\":\"user\",\"content\":[")
		TEXT("{\"type\":\"image\",\"source\":{\"type\":\"base64\",\"media_type\":\"image/png\",\"data\":\"iVBORw0KGgo=\"}},")
		TEXT("{\"type\":\"text\",\"text\":\"what is this\"}]}");

	FString Before, After;
	if (!RoundTrips(Json, Before, After)) { AddError(TEXT("decode failed")); return false; }
	TestEqual(TEXT("image message round-trips"), After, Before);
	TestTrue(TEXT("base64 payload survives"), After.Contains(TEXT("iVBORw0KGgo=")));
	return true;
}

// Forward compatibility: a block type this build has never heard of must come
// back out unchanged rather than being dropped, or a session written by a newer
// build would silently lose content when an older build touches it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_UnknownBlockPreserved, "KismetAI.AgentTypes.UnknownBlockPreserved", TypesTestFlags)
bool FTypes_UnknownBlockPreserved::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("{\"role\":\"assistant\",\"content\":[")
		TEXT("{\"type\":\"future_block\",\"payload\":{\"x\":1},\"note\":\"from a newer build\"}]}");

	FString Before, After;
	if (!RoundTrips(Json, Before, After)) { AddError(TEXT("decode failed")); return false; }
	TestEqual(TEXT("unknown block round-trips untouched"), After, Before);

	FAgentMessage Msg;
	KismetAIAgentCodec::FromAnthropicJson(ParseObject(Json), Msg);
	if (Msg.Parts.Num() != 1) { AddError(TEXT("expected one part")); return false; }
	TestTrue(TEXT("classified as Unknown"), Msg.Parts[0].Type == EAgentPartType::Unknown);
	return true;
}

// A whole conversation, in the exact shape the loop builds it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_ConversationArrayRoundTrip, "KismetAI.AgentTypes.ConversationArrayRoundTrip", TypesTestFlags)
bool FTypes_ConversationArrayRoundTrip::RunTest(const FString& Parameters)
{
	const FString Json = TEXT("[")
		TEXT("{\"role\":\"user\",\"content\":\"add a print node\"},")
		TEXT("{\"role\":\"assistant\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"insert_node\",\"input\":{\"blueprint\":\"/Game/BP\"}}]},")
		TEXT("{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"{\\\"success\\\":true}\"}]},")
		TEXT("{\"role\":\"assistant\",\"content\":[{\"type\":\"text\",\"text\":\"Added.\"}]}")
		TEXT("]");

	TArray<TSharedPtr<FJsonValue>> In;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, In))
	{
		AddError(TEXT("could not parse the conversation"));
		return false;
	}

	const TArray<FAgentMessage> Typed = KismetAIAgentCodec::FromAnthropicArray(In);
	TestEqual(TEXT("all four turns decoded"), Typed.Num(), 4);

	const TArray<TSharedPtr<FJsonValue>> Out = KismetAIAgentCodec::ToAnthropicArray(Typed);
	TestEqual(TEXT("all four turns re-encoded"), Out.Num(), In.Num());

	for (int32 i = 0; i < In.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("turn %d round-trips"), i),
			Serialize(Out[i]->AsObject()), Serialize(In[i]->AsObject()));
	}

	// Pairing is readable from the typed form, which is what the context
	// policy and the journal reducer rely on.
	TestTrue(TEXT("turn 2 is an assistant tool_use"),
		Typed[1].Role == EAgentRole::Assistant && Typed[1].HasToolUse());
	TestTrue(TEXT("turn 3 pairs it by id"),
		Typed[2].Parts.Num() == 1 && Typed[2].Parts[0].ToolUseId == TEXT("t1"));
	return true;
}

// Parts constructed in code (no RawJson) must encode from typed fields alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_SynthesizedPartsEncode, "KismetAI.AgentTypes.SynthesizedPartsEncode", TypesTestFlags)
bool FTypes_SynthesizedPartsEncode::RunTest(const FString& Parameters)
{
	FAgentMessage Msg;
	Msg.Role = EAgentRole::User;
	Msg.Parts.Add(FAgentContentPart::MakeToolResult(TEXT("t9"), TEXT("cancelled"), /*bError=*/true));

	const FString Out = Serialize(KismetAIAgentCodec::ToAnthropicJson(Msg));
	TestTrue(TEXT("role emitted"), Out.Contains(TEXT("\"role\":\"user\"")));
	TestTrue(TEXT("type emitted"), Out.Contains(TEXT("\"type\":\"tool_result\"")));
	TestTrue(TEXT("id emitted"), Out.Contains(TEXT("\"tool_use_id\":\"t9\"")));
	TestTrue(TEXT("error flag emitted"), Out.Contains(TEXT("\"is_error\":true")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTypes_ExtensionsAndMalformedInput, "KismetAI.AgentTypes.ExtensionsAndMalformedInput", TypesTestFlags)
bool FTypes_ExtensionsAndMalformedInput::RunTest(const FString& Parameters)
{
	const FString Json = TEXT(R"({"role":"assistant","provider_extension":{"opaque":"keep"},"content":[{"type":"tool_use","id":"bad","name":"read_blueprint","input":"invalid"}]})");
	FString Before, After;
	if (!RoundTrips(Json, Before, After)) { AddError(TEXT("decode failed")); return false; }
	const auto Result = ParseObject(After);
	TestEqual(TEXT("opaque message extension survives"), Result->GetObjectField(TEXT("provider_extension"))->GetStringField(TEXT("opaque")), FString(TEXT("keep")));
	const auto& Parts = Result->GetArrayField(TEXT("content"));
	TestEqual(TEXT("part order/count survives"), Parts.Num(), 1);
	TestEqual(TEXT("invalid input is not fabricated as an empty object"), Parts[0]->AsObject()->GetStringField(TEXT("input")), FString(TEXT("invalid")));
	TestEqual(TEXT("call identity survives"), Parts[0]->AsObject()->GetStringField(TEXT("id")), FString(TEXT("bad")));
	return true;
}
