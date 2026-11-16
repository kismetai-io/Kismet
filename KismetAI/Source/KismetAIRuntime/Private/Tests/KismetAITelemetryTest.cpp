// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAITelemetryInternal.h"
#include "KismetAITelemetry.h"
#include "Misc/AutomationTest.h"

constexpr auto KismetAITelemetryTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// CategoryForEventType — must stay in sync with categoryFor() in
// app/api/v1/telemetry/events/route.ts. If you add an event.type on the
// plugin side, add it both here AND in the backend mapping.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_CategoryForEventType, "KismetAI.Telemetry.CategoryForEventType", KismetAITelemetryTestFlags)

bool FTelemetry_CategoryForEventType::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::CategoryForEventType;
	using KismetAITelemetryInternal::ECategory;

	TestEqual(TEXT("tool_call routes to ToolCalls"),
		(uint8)CategoryForEventType(TEXT("tool_call")), (uint8)ECategory::ToolCalls);

	TestEqual(TEXT("log_warning routes to Errors"),
		(uint8)CategoryForEventType(TEXT("log_warning")), (uint8)ECategory::Errors);
	TestEqual(TEXT("log_error routes to Errors"),
		(uint8)CategoryForEventType(TEXT("log_error")), (uint8)ECategory::Errors);
	TestEqual(TEXT("api_error routes to Errors"),
		(uint8)CategoryForEventType(TEXT("api_error")), (uint8)ECategory::Errors);
	TestEqual(TEXT("t3d_inject_error routes to Errors"),
		(uint8)CategoryForEventType(TEXT("t3d_inject_error")), (uint8)ECategory::Errors);
	TestEqual(TEXT("compile_error routes to Errors"),
		(uint8)CategoryForEventType(TEXT("compile_error")), (uint8)ECategory::Errors);

	// Heartbeats must remain Always — privacy claim depends on these being
	// the only events that flow with all four categories off.
	TestEqual(TEXT("plugin_loaded routes to Always"),
		(uint8)CategoryForEventType(TEXT("plugin_loaded")), (uint8)ECategory::Always);
	TestEqual(TEXT("plugin_unloaded routes to Always"),
		(uint8)CategoryForEventType(TEXT("plugin_unloaded")), (uint8)ECategory::Always);

	// Unknown event types fall through to Always. Server-side categoryFor()
	// does the same — both sides default to letting unknown events through.
	TestEqual(TEXT("unknown event type defaults to Always"),
		(uint8)CategoryForEventType(TEXT("future_event_type_xyz")), (uint8)ECategory::Always);
	TestEqual(TEXT("empty event type defaults to Always"),
		(uint8)CategoryForEventType(TEXT("")), (uint8)ECategory::Always);

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SanitizeForTelemetryWithRoots — defence-in-depth scrubber that strips
// absolute filesystem paths, home dirs, and ../-traversal patterns from
// diagnostic strings before they enter the telemetry stream.
//
// Tests pin ProjectDir + HomeDir to fixed values so behaviour is
// reproducible across machines and CI.
// ─────────────────────────────────────────────────────────────────────────────

namespace
{
	const FString kProject = TEXT("/Users/alice/projects/MyProject/");
	const FString kHome    = TEXT("/Users/alice");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_Passthrough, "KismetAI.Telemetry.Sanitize.Passthrough", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_Passthrough::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	TestEqual(TEXT("empty string"),
		SanitizeForTelemetryWithRoots(TEXT(""), kProject, kHome), FString(TEXT("")));

	const FString Innocuous = TEXT("compile failed: pin 'execute' not found on node K2Node_0");
	TestEqual(TEXT("innocuous diagnostic untouched"),
		SanitizeForTelemetryWithRoots(Innocuous, kProject, kHome), Innocuous);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_ProjectDir, "KismetAI.Telemetry.Sanitize.ProjectDir", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_ProjectDir::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	const FString Input  = TEXT("Failed to read /Users/alice/projects/MyProject/Saved/Logs/foo.log");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("alice gone from output"), Output.Contains(TEXT("alice")));
	TestTrue(TEXT("ProjectDir marker present"), Output.Contains(TEXT("<ProjectDir>/")));
	TestTrue(TEXT("relative tail preserved"), Output.Contains(TEXT("Saved/Logs/foo.log")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_HomeDir, "KismetAI.Telemetry.Sanitize.HomeDir", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_HomeDir::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	// HomeDir prefix replacement triggers when the path does not start with
	// ProjectDir (which is a longer match). Use a sibling path under the
	// user's home that is NOT inside the project.
	const FString Input  = TEXT("Cannot open /Users/alice/Downloads/sample.uasset");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("alice gone from output"), Output.Contains(TEXT("alice")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));
	TestTrue(TEXT("relative tail preserved"), Output.Contains(TEXT("Downloads/sample.uasset")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_RelativeTraversal, "KismetAI.Telemetry.Sanitize.RelativeTraversal", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_RelativeTraversal::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	// The pattern UE actually emitted in the original session leak. Note the
	// inputs deliberately use a username different from kHome so we can be
	// sure the traversal rule fires (not the HomeDir prefix rule).
	const FString Input  = TEXT("Failed to load session file: ../../../../../../bob/projects/Foo/Saved/KismetAI/sessions/abc.jsonl");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("traversal segment removed"),
		Output.Contains(TEXT("../../../../../../bob")));
	TestTrue(TEXT("home marker substituted"), Output.Contains(TEXT("<Home>")));
	TestTrue(TEXT("relative tail preserved"),
		Output.Contains(TEXT("/projects/Foo/Saved/KismetAI/sessions/abc.jsonl")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_AbsoluteUsersFallback, "KismetAI.Telemetry.Sanitize.AbsoluteUsersFallback", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_AbsoluteUsersFallback::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	// Path for a DIFFERENT user, so neither ProjectDir nor HomeDir prefix
	// matches. The /Users/<name> absolute-fallback rule should still fire.
	const FString Input  = TEXT("Cannot read /Users/bob/secret/file.txt");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("bob gone"), Output.Contains(TEXT("bob")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));
	TestTrue(TEXT("tail preserved"), Output.Contains(TEXT("/secret/file.txt")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_LinuxHome, "KismetAI.Telemetry.Sanitize.LinuxHome", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_LinuxHome::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	const FString Input  = TEXT("stat failed on /home/dev/code/MyProject/Saved/foo");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("dev gone"), Output.Contains(TEXT("dev/code")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));
	TestTrue(TEXT("tail preserved"), Output.Contains(TEXT("/code/MyProject/Saved/foo")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_WindowsHome, "KismetAI.Telemetry.Sanitize.WindowsHome", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_WindowsHome::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	const FString Input  = TEXT("Win path: C:\\Users\\carol\\Projects\\Foo\\Saved\\log.txt");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("carol gone"), Output.Contains(TEXT("carol")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));
	// Tail after the user segment is preserved (backslash-separated).
	TestTrue(TEXT("tail preserved"), Output.Contains(TEXT("Projects\\Foo\\Saved\\log.txt")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_WindowsForwardSlashHome, "KismetAI.Telemetry.Sanitize.WindowsForwardSlashHome", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_WindowsForwardSlashHome::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	// UE sometimes normalises Windows paths to forward slashes; the rule
	// matches both separators.
	const FString Input  = TEXT("Path C:/Users/carol/AppData/Local/foo");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("carol gone"), Output.Contains(TEXT("carol")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_MultipleHits, "KismetAI.Telemetry.Sanitize.MultipleHits", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_MultipleHits::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	const FString Input  = TEXT("copy /Users/bob/a.txt -> /Users/bob/b.txt failed");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("no bob anywhere"), Output.Contains(TEXT("bob")));

	// Two replacement sites — count the Home marker.
	int32 Count = 0;
	int32 From = 0;
	while (true)
	{
		const int32 At = Output.Find(TEXT("<Home>"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (At == INDEX_NONE) break;
		Count++;
		From = At + 1;
	}
	TestEqual(TEXT("both paths replaced"), Count, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_EmptyRoots, "KismetAI.Telemetry.Sanitize.EmptyRoots", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_EmptyRoots::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	// Empty roots → only the absolute-pattern rules fire. Prefix replacement
	// is skipped (replacing an empty string would corrupt the message).
	const FString Input  = TEXT("read /Users/eve/foo");
	const FString Output = SanitizeForTelemetryWithRoots(Input, FString(), FString());

	TestFalse(TEXT("eve gone via absolute rule"), Output.Contains(TEXT("eve")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_QuotedPathTerminator, "KismetAI.Telemetry.Sanitize.QuotedPathTerminator", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_QuotedPathTerminator::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	// A JSON-embedded path: the regex must stop at the closing quote rather
	// than swallowing it. The character class in the rule excludes /, \s, ".
	const FString Input  = TEXT("{\"path\":\"/Users/dave/foo.txt\"}");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("dave gone"), Output.Contains(TEXT("dave")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));
	TestTrue(TEXT("JSON quoting preserved"), Output.Contains(TEXT("foo.txt\"}")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Sanitize_OriginalLeakRepro, "KismetAI.Telemetry.Sanitize.OriginalLeakRepro", KismetAITelemetryTestFlags)
bool FTelemetry_Sanitize_OriginalLeakRepro::RunTest(const FString& Parameters)
{
	using KismetAITelemetryInternal::SanitizeForTelemetryWithRoots;

	// Verbatim repro of the leak that motivated this work, with username
	// substituted so the test is portable across machines.
	const FString Input  = TEXT("Failed to load session file: ../../../../../../alice/projects/KismetAI/test/ue57/Saved/KismetAI/sessions/f50cc896-4b40-6425-3e11-a1b13aae990e.jsonl");
	const FString Output = SanitizeForTelemetryWithRoots(Input, kProject, kHome);

	TestFalse(TEXT("no traversal prefix"), Output.Contains(TEXT("../../../../../../alice")));
	TestFalse(TEXT("no username"), Output.Contains(TEXT("alice")));
	TestTrue(TEXT("Home marker present"), Output.Contains(TEXT("<Home>")));
	TestTrue(TEXT("session UUID preserved"),
		Output.Contains(TEXT("f50cc896-4b40-6425-3e11-a1b13aae990e.jsonl")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Telemetry consent mirror — UpdateConsent makes the state "known" and stores
// the acknowledged value. The tri-state contract (unknown never reads as
// not-consented) is what keeps the UI from gating on an unconfirmed default,
// so both transitions must be observable.
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Consent_UpdateMirror, "KismetAI.Telemetry.Consent.UpdateMirror", KismetAITelemetryTestFlags)
bool FTelemetry_Consent_UpdateMirror::RunTest(const FString& Parameters)
{
	// Server says "acknowledged" → known + consented.
	KismetAITelemetry::UpdateConsent(true);
	TestTrue(TEXT("consent is known after a server-confirmed value"), KismetAITelemetry::IsConsentKnown());
	TestTrue(TEXT("consented mirrors true"), KismetAITelemetry::HasConsented());

	// Server says "not acknowledged" → still known, but not consented (this is
	// the state that triggers the consent notice in the UI).
	KismetAITelemetry::UpdateConsent(false);
	TestTrue(TEXT("consent remains known"), KismetAITelemetry::IsConsentKnown());
	TestFalse(TEXT("consented mirrors false"), KismetAITelemetry::HasConsented());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTelemetry_Secrets, "KismetAI.Telemetry.Sanitize.Secrets", KismetAITelemetryTestFlags)
bool FTelemetry_Secrets::RunTest(const FString& Parameters)
{
	const TArray<FString> Inputs = {
		TEXT("Authorization: Bearer fake-header-secret"),
		TEXT("https://user:fake-password@host/v1?api_key=fake-query-secret"),
		TEXT("{\"error\":{\"headers\":{\"x-api-key\":\"fake-nested-secret\"}}}"),
		TEXT("failed /private/tmp/fake-scratch-secret/curl.cfg")
	};
	for (const FString& Input : Inputs)
	{
		const FString Safe = KismetAITelemetryInternal::SanitizeForTelemetryWithRoots(Input, FString(), FString());
		TestFalse(TEXT("credentials, URLs and temporary paths are removed"), Safe.Contains(TEXT("fake-")));
	}
	return true;
}
