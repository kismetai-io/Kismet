// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIHttpTransport.h"
#include "KismetAIEngineCompat.h"
#include "HttpServerResponse.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Misc/AutomationTest.h"

namespace KismetAIHttpTransportTests
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	FKismetAIHttpRequestParser::EResult Append(FKismetAIHttpRequestParser& Parser, const FString& Text)
	{
		FTCHARToUTF8 Bytes(*Text);
		return Parser.Append(reinterpret_cast<const uint8*>(Bytes.Get()), Bytes.Length());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpEarlyBodyTest, "KismetAI.HttpTransport.EarlyBodyBound", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpEarlyBodyTest::RunTest(const FString& Parameters)
{
	FKismetAIHttpRequestParser Parser;
	const auto Result = KismetAIHttpTransportTests::Append(Parser,
		TEXT("POST /api/v1/compile HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\n\r\n"));
	TestTrue(TEXT("oversized length rejected without any body bytes"), Result == FKismetAIHttpRequestParser::EResult::Rejected);
	TestEqual(TEXT("payload limit status"), Parser.GetErrorStatus(), 413);
	TestTrue(TEXT("advertised body was not allocated"), Parser.GetBufferedBytes() <= FKismetAIHttpRequestParser::MaxHeaderBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpHeaderBoundTest, "KismetAI.HttpTransport.FragmentedHeaderBound", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpHeaderBoundTest::RunTest(const FString& Parameters)
{
	FKismetAIHttpRequestParser Parser;
	KismetAIHttpTransportTests::Append(Parser, TEXT("GET /api/v1/status HTTP/1.1\r\nX-Large: "));
	const uint8 Byte = 'x';
	FKismetAIHttpRequestParser::EResult Result = FKismetAIHttpRequestParser::EResult::Incomplete;
	for (int32 Index = 0; Index <= FKismetAIHttpRequestParser::MaxHeaderBytes; ++Index)
	{
		Result = Parser.Append(&Byte, 1);
		TestTrue(TEXT("every incremental append stays within header bound"), Parser.GetBufferedBytes() <= FKismetAIHttpRequestParser::MaxHeaderBytes);
		if (Result == FKismetAIHttpRequestParser::EResult::Rejected) break;
	}
	TestTrue(TEXT("unclosed oversized headers rejected"), Result == FKismetAIHttpRequestParser::EResult::Rejected);
	TestEqual(TEXT("header status"), Parser.GetErrorStatus(), 431);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpFramingTest, "KismetAI.HttpTransport.RejectAmbiguousFraming", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpFramingTest::RunTest(const FString& Parameters)
{
	const TArray<FString> InvalidHeaders = {
		TEXT("Content-Length: 1\r\nContent-Length: 2\r\n"),
		TEXT("Content-Length: 1\r\ncontent-length: 1\r\n"),
		TEXT("Content-Length: -1\r\n"), TEXT("Content-Length: +1\r\n"),
		TEXT("Content-Length: 1, 1\r\n"), TEXT("Content-Length: 999999999999999999999999\r\n"),
		TEXT("Content-Length: 1garbage\r\n"), TEXT("Transfer-Encoding: chunked\r\n"),
		TEXT("Transfer-Encoding: identity\r\nContent-Length: 1\r\n"),
		TEXT("Expect: 100-continue\r\nContent-Length: 1\r\n"),
		TEXT("Content-Length : 1\r\n"), TEXT(" continuation\r\n")
	};
	for (const FString& Headers : InvalidHeaders)
	{
		FKismetAIHttpRequestParser Parser;
		const auto Result = KismetAIHttpTransportTests::Append(Parser, TEXT("POST /mcp HTTP/1.1\r\nHost: localhost\r\n") + Headers + TEXT("\r\n"));
		TestTrue(*FString::Printf(TEXT("ambiguous framing refused: %s"), *Headers), Result == FKismetAIHttpRequestParser::EResult::Rejected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpValidRequestTest, "KismetAI.HttpTransport.FragmentedValidRequest", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpValidRequestTest::RunTest(const FString& Parameters)
{
	FKismetAIHttpRequestParser Parser;
	const FString Input = TEXT("POST /api/v1/agent/runs?run_id=a%2Bb&after=12 HTTP/1.1\r\nHost: localhost\r\nX-Kismet-Token: native\r\nContent-Length: 2\r\n\r\n{}");
	FTCHARToUTF8 Bytes(*Input);
	for (int32 Index = 0; Index < Bytes.Length(); ++Index)
	{
		const auto Result = Parser.Append(reinterpret_cast<const uint8*>(Bytes.Get()) + Index, 1);
		TestTrue(TEXT("only complete after the final body byte"), Result == (Index + 1 == Bytes.Length()
			? FKismetAIHttpRequestParser::EResult::Complete : FKismetAIHttpRequestParser::EResult::Incomplete));
	}
	const auto Request = Parser.GetRequest();
	if (!TestTrue(TEXT("request exists"), Request.IsValid())) return false;
	TestEqual(TEXT("body preserved"), Request->Body.Num(), 2);
	TestEqual(TEXT("query decoded once"), Request->QueryParams.FindRef(TEXT("run_id")), FString(TEXT("a+b")));
	TestEqual(TEXT("path excludes query"), KismetAIHttpCompat::GetRequestPath(*Request), FString(TEXT("/api/v1/agent/runs")));
	TestTrue(TEXT("native token header remains accessible"), Request->Headers.Contains(TEXT("X-Kismet-Token")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpPeerTest, "KismetAI.HttpTransport.LoopbackPeer", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpPeerTest::RunTest(const FString& Parameters)
{
	for (const FString& Ip : { FString(TEXT("127.0.0.1")), FString(TEXT("127.12.34.56")), FString(TEXT("192.168.1.2")), FString(TEXT("0.0.0.0")) })
	{
		const auto Address = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		bool bValid = false;
		Address->SetIp(*Ip, bValid);
		TestTrue(TEXT("test address parsed"), bValid);
		TestEqual(*Ip, FKismetAIHttpTransport::IsLoopbackPeer(*Address), Ip.StartsWith(TEXT("127.")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpRouteTest, "KismetAI.HttpTransport.RouteBoundary", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpRouteTest::RunTest(const FString& Parameters)
{
	auto Router = MakeShared<FKismetAIHttpTransport>();
	FString Relative;
	const auto Handle = Router->BindRoute(FHttpPath(TEXT("/api/v1/status")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([&Relative](const FHttpServerRequest& Request, const FHttpResultCallback& Callback)
		{
			Relative = Request.RelativePath.GetPath(); Callback(FHttpServerResponse::Ok()); return true;
		}));
	auto Request = MakeShared<FHttpServerRequest>();
	Request->Verb = EHttpServerRequestVerbs::VERB_GET;
	KismetAIHttpCompat::SetRequestPath(*Request, TEXT("/api/v1/status/child"));
	TestTrue(TEXT("segment child matches"), Router->Query(Request, [](TUniquePtr<FHttpServerResponse>&&) {}));
	TestEqual(TEXT("handler receives relative suffix"), Relative, FString(TEXT("/child")));
	KismetAIHttpCompat::SetRequestPath(*Request, TEXT("/api/v1/statusx"));
	TestFalse(TEXT("prefix collision does not match"), Router->Query(Request, [](TUniquePtr<FHttpServerResponse>&&) {}));
	KismetAIHttpCompat::SetRequestPath(*Request, TEXT("/API/V1/STATUS"));
	TestFalse(TEXT("route matching is case sensitive"), Router->Query(Request, [](TUniquePtr<FHttpServerResponse>&&) {}));
	Router->UnbindRoute(Handle);
	KismetAIHttpCompat::SetRequestPath(*Request, TEXT("/api/v1/status"));
	TestFalse(TEXT("unbound handler cannot run"), Router->Query(Request, [](TUniquePtr<FHttpServerResponse>&&) {}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpStopDispatchTest, "KismetAI.HttpTransport.StopDuringDispatch", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpStopDispatchTest::RunTest(const FString& Parameters)
{
	auto Router = MakeShared<FKismetAIHttpTransport>();
	int32 FallbackCalls = 0;
	const auto Fallback = Router->BindRoute(FHttpPath(TEXT("/api")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([&FallbackCalls](const FHttpServerRequest&, const FHttpResultCallback&)
		{
			++FallbackCalls; return true;
		}));
	const auto Stopper = Router->BindRoute(FHttpPath(TEXT("/api/stop")), EHttpServerRequestVerbs::VERB_GET,
		FHttpRequestHandler::CreateLambda([&Router](const FHttpServerRequest&, const FHttpResultCallback&)
		{
			Router->Stop(); return false;
		}));
	TestTrue(TEXT("fallback registered"), Fallback.IsValid());
	TestTrue(TEXT("stopper registered"), Stopper.IsValid());
	auto Request = MakeShared<FHttpServerRequest>();
	Request->Verb = EHttpServerRequestVerbs::VERB_GET;
	KismetAIHttpCompat::SetRequestPath(*Request, TEXT("/api/stop"));
	Router->Query(Request, [](TUniquePtr<FHttpServerResponse>&&) {});
	TestEqual(TEXT("Stop revokes undispatched routes even inside an active query"), FallbackCalls, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpEmptyPostTest, "KismetAI.HttpTransport.EmptyPostCompatibility", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpEmptyPostTest::RunTest(const FString& Parameters)
{
	FKismetAIHttpRequestParser Parser;
	const auto Result = KismetAIHttpTransportTests::Append(Parser, TEXT("POST /mcp HTTP/1.1\r\nHost: localhost\r\n\r\n"));
	TestTrue(TEXT("no length means zero bytes; existing empty POST reaches auth"), Result == FKismetAIHttpRequestParser::EResult::Complete);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpMaxBodyTest, "KismetAI.HttpTransport.MaxBodyCoalescedAndFragmented", KismetAIHttpTransportTests::Flags)
bool FKismetAIHttpMaxBodyTest::RunTest(const FString& Parameters)
{
	const FString Header = FString::Printf(TEXT("POST /api/v1/compile HTTP/1.1\r\nHost: localhost\r\nContent-Length: %d\r\n\r\n"), FKismetAIHttpRequestParser::MaxBodyBytes);
	FTCHARToUTF8 HeaderBytes(*Header);
	TArray<uint8> Input(reinterpret_cast<const uint8*>(HeaderBytes.Get()), HeaderBytes.Length());
	Input.AddZeroed(FKismetAIHttpRequestParser::MaxBodyBytes);
	FKismetAIHttpRequestParser Coalesced;
	TestTrue(TEXT("coalesced body is not charged against header limit"), Coalesced.Append(Input.GetData(), Input.Num()) == FKismetAIHttpRequestParser::EResult::Complete);
	TestEqual(TEXT("exact body boundary accepted"), Coalesced.GetRequest()->Body.Num(), FKismetAIHttpRequestParser::MaxBodyBytes);
	FKismetAIHttpRequestParser Fragmented;
	const int32 First = HeaderBytes.Length() - 1;
	TestTrue(TEXT("fragmented terminator incomplete"), Fragmented.Append(Input.GetData(), First) == FKismetAIHttpRequestParser::EResult::Incomplete);
	TestTrue(TEXT("terminator and first body byte coalesced"), Fragmented.Append(Input.GetData() + First, 2) == FKismetAIHttpRequestParser::EResult::Incomplete);
	TestTrue(TEXT("remaining body accepted at exact limit"), Fragmented.Append(Input.GetData() + First + 2, Input.Num() - First - 2) == FKismetAIHttpRequestParser::EResult::Complete);
	TestEqual(TEXT("retained byte boundary"), Fragmented.GetBufferedBytes(), FKismetAIHttpRequestParser::MaxBodyBytes);
	const uint8 Extra = 1;
	TestTrue(TEXT("one extra byte cannot append beyond cap"), Fragmented.Append(&Extra, 1) == FKismetAIHttpRequestParser::EResult::Rejected);
	TestEqual(TEXT("rejected append leaves body bounded"), Fragmented.GetBufferedBytes(), FKismetAIHttpRequestParser::MaxBodyBytes);
	return true;
}
