// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModule.h"
#include "KismetAISourceConfinement.h"
#include "KismetAISourceSearch.h"
#include "HAL/PlatformTime.h"
#include "KismetAIToolResult.h"
#include "KismetAIValidation.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "Serialization/Archive.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <unistd.h>
#endif

namespace SourceSearchTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	TSharedPtr<FJsonObject> Request(const FString& Pattern)
	{
		auto Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("mode"), TEXT("find"));
		Args->SetStringField(TEXT("scope"), TEXT("project"));
		Args->SetStringField(TEXT("pattern"), Pattern);
		Args->SetNumberField(TEXT("limit"), 20);
		return Args;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchSparseBudget, "KismetAI.ReadSource.Search.SparseBudget", SourceSearchTest::Flags)
bool FSourceSearchSparseBudget::RunTest(const FString& Parameters)
{
	KismetAISourceSearch::Close();
	ON_SCOPE_EXIT { KismetAISourceSearch::Close(); };
	const FString Base = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetP206-"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Base, false, true); };
	FKismetAISourceRoots Roots;
	Roots.ProjectDir = Base / TEXT("Project");
	Roots.EngineDir = Base / TEXT("Engine");
	Roots.bIncludeDiscoveredPlugins = false;
	const FString Source = Roots.ProjectDir / TEXT("Source");
	IFileManager::Get().MakeDirectory(*Source, true);
	for (int32 Index = 0; Index < 4096; ++Index)
	{
		if (!FFileHelper::SaveStringToFile(TEXT("// sparse fixture\n"), *(Source / FString::Printf(TEXT("Noise%05d.h"), Index))))
		{
			AddError(TEXT("Could not create sparse fixture"));
			return false;
		}
	}
	KismetAISourceConfinement::FScopedRootsOverride Override(Roots);
	const FToolExecutionResult Result = FKismetAIModule::Get().DoReadSource(SourceSearchTest::Request(TEXT("*Needle.h")));
	TestTrue(TEXT("Bounded search returns a partial success"), Result.bSuccess);
	if (!Result.Data.IsValid()) return false;
	bool bPartial = false;
	TestTrue(TEXT("Explicit partial metadata"), Result.Data->TryGetBoolField(TEXT("partial"), bPartial) && bPartial);
	FString Cursor;
	TestTrue(TEXT("Sparse page can continue without replay"), Result.Data->TryGetStringField(TEXT("cursor"), Cursor) && !Cursor.IsEmpty());
	const TSharedPtr<FJsonObject>* Metrics = nullptr;
	TestTrue(TEXT("Actual traversal metrics supplied"), Result.Data->TryGetObjectField(TEXT("metrics"), Metrics));
	if (Metrics)
	{
		double Entries = 0;
		TestTrue(TEXT("Actual entries are positive and capped"), (*Metrics)->TryGetNumberField(TEXT("entries_considered"), Entries) && Entries > 0 && Entries <= 1024);
		int32 TotalEntries = static_cast<int32>(Entries);
		auto Args = SourceSearchTest::Request(TEXT("*Needle.h"));
		for (int32 Page = 0; !Cursor.IsEmpty() && Page < 20; ++Page)
		{
			Args->SetStringField(TEXT("cursor"), Cursor);
			const auto Next = FKismetAIModule::Get().DoReadSource(Args);
			if (!TestTrue(TEXT("Sparse continuation succeeds"), Next.bSuccess)) return false;
			const double Work = Next.Data->GetObjectField(TEXT("metrics"))->GetNumberField(TEXT("entries_considered"));
			TestTrue(TEXT("Every sparse page respects work cap"), Work <= 1024);
			TotalEntries += static_cast<int32>(Work);
			Cursor.Empty();
			Next.Data->TryGetStringField(TEXT("cursor"), Cursor);
		}
		TestTrue(TEXT("Sparse traversal completes"), Cursor.IsEmpty());
		TestEqual(TEXT("Sparse traversal never replays entries"), TotalEntries, 4096);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchRejectRecursivePattern, "KismetAI.ReadSource.Search.UnsupportedPattern", SourceSearchTest::Flags)
bool FSourceSearchRejectRecursivePattern::RunTest(const FString& Parameters)
{
	const FToolExecutionResult Result = FKismetAIModule::Get().DoReadSource(SourceSearchTest::Request(TEXT("**Needle.h")));
	TestFalse(TEXT("Recursive glob syntax is explicitly refused"), Result.bSuccess);
	TestEqual(TEXT("Stable unsupported-pattern error"), Result.ErrorCode, FString(TEXT("UNSUPPORTED_PATTERN")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchPatternValidation, "KismetAI.ReadSource.Search.PatternValidation", SourceSearchTest::Flags)
bool FSourceSearchPatternValidation::RunTest(const FString& Parameters)
{
	auto Broad = SourceSearchTest::Request(TEXT("*.h"));
	Broad->SetStringField(TEXT("scope"), TEXT("engine"));
	TestTrue(TEXT("Bounded engine header glob is supported"), FKismetAIValidation::ValidateReadSource(Broad).bIsValid);
	for (const TCHAR* Pattern : {TEXT("**.h"), TEXT("../*.h"), TEXT("[AB].h")})
	{
		TestFalse(FString::Printf(TEXT("Unsupported basename syntax: %s"), Pattern), FKismetAIValidation::ValidateReadSource(SourceSearchTest::Request(Pattern)).bIsValid);
	}
	return true;
}

namespace SourceSearchTest
{
	struct FFixture
	{
		FString Base = FString(FPlatformProcess::UserTempDir()) / (TEXT("KismetP206-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		FString Source = Base / TEXT("Source");
		TArray<FString> Roots;
		FFixture()
		{
			KismetAISourceSearch::Close();
			IFileManager::Get().MakeDirectory(*Source, true);
			Roots.Add(KismetAISourceConfinement::CanonicalizeExisting(Source));
		}
		~FFixture()
		{
			KismetAISourceSearch::Close();
			IFileManager::Get().DeleteDirectory(*Base, false, true);
		}
		bool Write(const FString& Relative, const FString& Content = TEXT("// source fixture\n"))
		{
			const FString Path = Source / Relative;
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
			return FFileHelper::SaveStringToFile(Content, *Path);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchPagination, "KismetAI.ReadSource.Search.Pagination", SourceSearchTest::Flags)
bool FSourceSearchPagination::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 53; ++Index) TestTrue(TEXT("Write matching header"), Fixture.Write(FString::Printf(TEXT("Match%03d.h"), Index)));
	auto Args = SourceSearchTest::Request(TEXT("Match*.h"));
	FKismetAICancellationToken Token;
	int32 Total = 0, Entries = 0;
	TSet<FString> Lines;
	for (int32 Page = 0; Page < 10; ++Page)
	{
		const auto Result = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
		if (!TestTrue(TEXT("Page succeeds"), Result.bSuccess) || !Result.Data) return false;
		Total += static_cast<int32>(Result.Data->GetNumberField(TEXT("count")));
		const auto Metrics = Result.Data->GetObjectField(TEXT("metrics"));
		Entries += static_cast<int32>(Metrics->GetNumberField(TEXT("entries_considered")));
		TestTrue(TEXT("Independent entry budget"), Metrics->GetNumberField(TEXT("entries_considered")) <= 1024);
		TestTrue(TEXT("Independent directory budget"), Metrics->GetNumberField(TEXT("directories_visited")) <= 128);
		TestEqual(TEXT("Discovery reads no content"), Metrics->GetNumberField(TEXT("bytes_read")), 0.0);
		TestTrue(TEXT("Text output is bounded"), Metrics->GetNumberField(TEXT("output_text_bytes")) <= 16384);
		TArray<FString> PageLines;
		Result.Data->GetStringField(TEXT("result")).ParseIntoArrayLines(PageLines);
		for (const FString& Line : PageLines)
		{
			TestFalse(TEXT("Unchanged tree has no duplicate result"), Lines.Contains(Line));
			Lines.Add(Line);
		}
		FString Cursor;
		if (!Result.Data->TryGetStringField(TEXT("cursor"), Cursor)) break;
		Args->SetStringField(TEXT("cursor"), Cursor);
	}
	TestEqual(TEXT("All results across pages"), Total, 53);
	TestEqual(TEXT("Enumeration never replays entries"), Entries, 53);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchCursorAuthority, "KismetAI.ReadSource.Search.CursorAuthority", SourceSearchTest::Flags)
bool FSourceSearchCursorAuthority::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 40; ++Index) Fixture.Write(FString::Printf(TEXT("Match%d.h"), Index));
	auto Args = SourceSearchTest::Request(TEXT("*.h"));
	FKismetAICancellationToken Token;
	const auto First = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
	if (!TestTrue(TEXT("Initial search"), First.bSuccess)) return false;
	FString Cursor = First.Data->GetStringField(TEXT("cursor"));
	Args->SetStringField(TEXT("cursor"), Cursor);
	Args->SetStringField(TEXT("pattern"), TEXT("Match*.h"));
	TestEqual(TEXT("Query cannot change under a cursor"), KismetAISourceSearch::Find(Args, Fixture.Roots, Token).ErrorCode, FString(TEXT("CURSOR_MISMATCH")));
	Args->SetStringField(TEXT("pattern"), TEXT("*.h"));
	TestEqual(TEXT("Roots cannot change under a cursor"), KismetAISourceSearch::Find(Args, TArray<FString>(), Token).ErrorCode, FString(TEXT("CURSOR_MISMATCH")));
	const auto Second = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
	TestTrue(TEXT("Original query continues"), Second.bSuccess);
	TestEqual(TEXT("Consumed token cannot replay a page"), KismetAISourceSearch::Find(Args, Fixture.Roots, Token).ErrorCode, FString(TEXT("INVALID_CURSOR")));
	if (Second.Data && Second.Data->TryGetStringField(TEXT("cursor"), Cursor))
	{
		Args->SetStringField(TEXT("cursor"), Cursor);
		Token.Cancel();
		TestEqual(TEXT("Cancellation invalidates retained cursor"), KismetAISourceSearch::Find(Args, Fixture.Roots, Token).ErrorCode, FString(TEXT("INVALID_CURSOR")));
	}
	FKismetAICancellationToken Cancelled;
	Cancelled.Cancel();
	TestEqual(TEXT("Pre-cancel starts no traversal"), KismetAISourceSearch::Find(SourceSearchTest::Request(TEXT("*.h")), Fixture.Roots, Cancelled).ErrorCode, FString(TEXT("CANCELLED")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchCursorCapacity, "KismetAI.ReadSource.Search.CursorCapacity", SourceSearchTest::Flags)
bool FSourceSearchCursorCapacity::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 30; ++Index) Fixture.Write(FString::Printf(TEXT("Match%d.h"), Index));
	FKismetAICancellationToken Token;
	for (int32 Index = 0; Index < 8; ++Index) TestTrue(TEXT("Bounded cursor slot admitted"), KismetAISourceSearch::Find(SourceSearchTest::Request(TEXT("*.h")), Fixture.Roots, Token).bSuccess);
	TestEqual(TEXT("Ninth retained cursor refused"), KismetAISourceSearch::Find(SourceSearchTest::Request(TEXT("*.h")), Fixture.Roots, Token).ErrorCode, FString(TEXT("SOURCE_BUSY")));
	KismetAISourceSearch::Close();
	TestTrue(TEXT("Close releases all cursor slots"), KismetAISourceSearch::Find(SourceSearchTest::Request(TEXT("*.h")), Fixture.Roots, Token).bSuccess);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchConfinedMutation, "KismetAI.ReadSource.Search.ConfinedMutation", SourceSearchTest::Flags)
bool FSourceSearchConfinedMutation::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 40; ++Index) Fixture.Write(FString::Printf(TEXT("Allowed%d.h"), Index));
	const FString Outside = Fixture.Base / TEXT("Source-private");
	IFileManager::Get().MakeDirectory(*Outside, true);
	FFileHelper::SaveStringToFile(TEXT("secret"), *(Outside / TEXT("Forbidden.h")));
#if PLATFORM_MAC || PLATFORM_LINUX
	TestEqual(TEXT("Create excluded directory link"), ::symlink(TCHAR_TO_UTF8(*Outside), TCHAR_TO_UTF8(*(Fixture.Source / TEXT("Alias")))), 0);
	TestEqual(TEXT("Create excluded file link"), ::symlink(TCHAR_TO_UTF8(*(Outside / TEXT("Forbidden.h"))), TCHAR_TO_UTF8(*(Fixture.Source / TEXT("Forbidden.h")))), 0);
#endif
	auto Args = SourceSearchTest::Request(TEXT("*.h"));
	FKismetAICancellationToken Token;
	for (int32 Page = 0; Page < 10; ++Page)
	{
		const auto Result = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
		if (!TestTrue(TEXT("Live-tree page succeeds"), Result.bSuccess)) return false;
		TestFalse(TEXT("Excluded names never returned"), Result.Data->GetStringField(TEXT("result")).Contains(TEXT("Forbidden")));
		TestTrue(TEXT("Mutation limitations explicit"), Result.Data->GetStringField(TEXT("continuation_semantics")).Contains(TEXT("omit or duplicate")));
		FString Cursor;
		if (!Result.Data->TryGetStringField(TEXT("cursor"), Cursor)) break;
		Args->SetStringField(TEXT("cursor"), Cursor);
		if (Page == 0) Fixture.Write(TEXT("AddedBetweenPages.h"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReadBudgets, "KismetAI.ReadSource.Search.ReadBudgets", SourceSearchTest::Flags)
bool FSourceReadBudgets::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	TestTrue(TEXT("Large source fixture"), Fixture.Write(TEXT("Large.h"), FString::ChrN(1048577, TEXT('x'))));
	TArray<uint8> Bytes;
	FString Final;
	FKismetAISourceReadMetrics Metrics;
	TestEqual(TEXT("Oversized read explicitly refused"), KismetAISourceConfinement::ReadConfined(Fixture.Source / TEXT("Large.h"), Fixture.Roots, Bytes, Final, FKismetAISourceReadOptions(), &Metrics), EKismetAISourceReadStatus::TooLarge);
	TestEqual(TEXT("Oversized source content not read"), Metrics.BytesRead, static_cast<int64>(0));
	Fixture.Write(TEXT("Small.h"));
	FKismetAICancellationToken Token;
	Token.Cancel();
	FKismetAISourceReadOptions Options;
	Options.Cancellation = &Token;
	TestEqual(TEXT("Cancelled read"), KismetAISourceConfinement::ReadConfined(Fixture.Source / TEXT("Small.h"), Fixture.Roots, Bytes, Final, Options, &Metrics), EKismetAISourceReadStatus::Cancelled);
	Options.Cancellation = nullptr;
	Options.DeadlineSeconds = FPlatformTime::Seconds() - 1;
	TestEqual(TEXT("Elapsed work deadline"), KismetAISourceConfinement::ReadConfined(Fixture.Source / TEXT("Small.h"), Fixture.Roots, Bytes, Final, Options, &Metrics), EKismetAISourceReadStatus::Deadline);
	Options.DeadlineSeconds = 0;
	TestEqual(TEXT("Normal confined read"), KismetAISourceConfinement::ReadConfined(Fixture.Source / TEXT("Small.h"), Fixture.Roots, Bytes, Final, Options, &Metrics), EKismetAISourceReadStatus::Ok);
	TestEqual(TEXT("Measured actual bytes"), Metrics.BytesRead, static_cast<int64>(Bytes.Num()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchDepthBudget, "KismetAI.ReadSource.Search.DepthBudget", SourceSearchTest::Flags)
bool FSourceSearchDepthBudget::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	FString Relative;
	for (int32 Depth = 0; Depth < 70; ++Depth) Relative /= TEXT("D");
	TestTrue(TEXT("Deep fixture"), Fixture.Write(Relative / TEXT("Deep.h")));
	FKismetAICancellationToken Token;
	auto Args = SourceSearchTest::Request(TEXT("*.h"));
	bool bFinished = false;
	for (int32 Page = 0; Page < 8; ++Page)
	{
		const auto Result = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
		if (!TestTrue(TEXT("Depth-limited page succeeds"), Result.bSuccess)) return false;
		TestEqual(TEXT("Over-depth file never traversed"), Result.Data->GetNumberField(TEXT("count")), 0.0);
		FString Cursor;
		if (!Result.Data->TryGetStringField(TEXT("cursor"), Cursor))
		{
			TestTrue(TEXT("Unsearched depth is explicitly partial"), Result.Data->GetBoolField(TEXT("partial")));
			bFinished = true;
			break;
		}
		Args->SetStringField(TEXT("cursor"), Cursor);
	}
	TestTrue(TEXT("Depth cap terminates"), bFinished);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchIdleReclamation, "KismetAI.ReadSource.Search.IdleReclamation", SourceSearchTest::Flags)
bool FSourceSearchIdleReclamation::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 30; ++Index) Fixture.Write(FString::Printf(TEXT("Match%d.h"), Index));
	FKismetAICancellationToken Token;
	auto Args = SourceSearchTest::Request(TEXT("*.h"));
	const auto First = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
	if (!TestTrue(TEXT("Initial cursor"), First.bSuccess)) return false;
	Args->SetStringField(TEXT("cursor"), First.Data->GetStringField(TEXT("cursor")));
	KismetAISourceSearch::ReapExpired(FPlatformTime::Seconds() + 61);
	TestEqual(TEXT("Idle expiry invalidates cursor without another search"), KismetAISourceSearch::Find(Args, Fixture.Roots, Token).ErrorCode, FString(TEXT("INVALID_CURSOR")));
	Args->RemoveField(TEXT("cursor"));
	const auto Next = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
	if (!TestTrue(TEXT("Fresh cursor after expiry"), Next.bSuccess)) return false;
	Args->SetStringField(TEXT("cursor"), Next.Data->GetStringField(TEXT("cursor")));
	Token.Cancel();
	KismetAISourceSearch::ReapExpired();
	FKismetAICancellationToken Fresh;
	TestEqual(TEXT("Cancelled cursor reclaimed by pump"), KismetAISourceSearch::Find(Args, Fixture.Roots, Fresh).ErrorCode, FString(TEXT("INVALID_CURSOR")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchActiveCancellation, "KismetAI.ReadSource.Search.ActiveCancellation", SourceSearchTest::Flags)
bool FSourceSearchActiveCancellation::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 2048; ++Index) Fixture.Write(FString::Printf(TEXT("Noise%d.h"), Index));
	FKismetAICancellationToken Token;
	auto Args = SourceSearchTest::Request(TEXT("*Needle.h"));
	const auto First = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
	if (!TestTrue(TEXT("Initial sparse cursor"), First.bSuccess)) return false;
	Args->SetStringField(TEXT("cursor"), First.Data->GetStringField(TEXT("cursor")));
	int32 ObservedEntries = 0;
	const auto Cancelled = KismetAISourceSearch::Find(Args, Fixture.Roots, Token, 0, [&](int32 Entries)
	{
		ObservedEntries = Entries;
		if (Entries == 128) Token.Cancel();
	});
	TestEqual(TEXT("Cancellation occurs inside real enumeration"), ObservedEntries, 128);
	TestEqual(TEXT("Active traversal cancellation is explicit"), Cancelled.ErrorCode, FString(TEXT("CANCELLED")));
	FKismetAICancellationToken Fresh;
	TestEqual(TEXT("Cancelled cursor cannot resume"), KismetAISourceSearch::Find(Args, Fixture.Roots, Fresh).ErrorCode, FString(TEXT("INVALID_CURSOR")));
	Args->RemoveField(TEXT("cursor"));
	for (int32 Slot = 0; Slot < 8; ++Slot)
	{
		TestTrue(TEXT("Cancelled active traversal releases reserved capacity"), KismetAISourceSearch::Find(Args, Fixture.Roots, Fresh).bSuccess);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchSerializedBudget, "KismetAI.ReadSource.Search.SerializedBudget", SourceSearchTest::Flags)
bool FSourceSearchSerializedBudget::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	FString Relative;
#if PLATFORM_MAC || PLATFORM_LINUX
	for (int32 Level = 0; Level < 6; ++Level) Relative /= FString::ChrN(100, static_cast<TCHAR>(1));
#else
	for (int32 Level = 0; Level < 6; ++Level) Relative /= FString::ChrN(20, TEXT('x'));
#endif
	for (int32 Index = 0; Index < 24; ++Index)
	{
		if (!TestTrue(TEXT("Escaping fixture created"), Fixture.Write(Relative / FString::Printf(TEXT("Entry%d.h"), Index)))) return false;
	}
	FKismetAICancellationToken Token;
	auto Args = SourceSearchTest::Request(TEXT("*.h"));
	int32 Total = 0;
	bool bFinished = false;
	for (int32 Page = 0; Page < 32; ++Page)
	{
		const auto Result = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
		if (!TestTrue(TEXT("Escaped output page succeeds"), Result.bSuccess)) return false;
		TestTrue(TEXT("Actual serialized envelope bounded"), FTCHARToUTF8(*Result.ToEnvelopeString()).Length() <= 32768);
		Total += static_cast<int32>(Result.Data->GetNumberField(TEXT("count")));
		FString Cursor;
		if (!Result.Data->TryGetStringField(TEXT("cursor"), Cursor)) { bFinished = true; break; }
		Args->SetStringField(TEXT("cursor"), Cursor);
	}
	TestTrue(TEXT("Escaped output pagination finishes"), bFinished);
	TestEqual(TEXT("Output cap loses no entries"), Total, 24);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchIndependentWorkBudgets, "KismetAI.ReadSource.Search.IndependentWorkBudgets", SourceSearchTest::Flags)
bool FSourceSearchIndependentWorkBudgets::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 256; ++Index) Fixture.Write(FString::Printf(TEXT("Dir%d/Noise.h"), Index));
	FKismetAICancellationToken Token;
	auto Args = SourceSearchTest::Request(TEXT("*Needle.h"));
	const auto Timed = KismetAISourceSearch::Find(Args, Fixture.Roots, Token, FPlatformTime::Seconds() - 1);
	if (!TestTrue(TEXT("Expired work budget returns resumable partial"), Timed.bSuccess)) return false;
	TestEqual(TEXT("Time budget reason"), Timed.Data->GetStringField(TEXT("stop_reason")), FString(TEXT("time_budget")));
	TestEqual(TEXT("No traversal after deadline"), Timed.Data->GetObjectField(TEXT("metrics"))->GetNumberField(TEXT("entries_considered")), 0.0);
	Args->SetStringField(TEXT("cursor"), Timed.Data->GetStringField(TEXT("cursor")));
	const auto Page = KismetAISourceSearch::Find(Args, Fixture.Roots, Token);
	if (!TestTrue(TEXT("Continuation after expired page"), Page.bSuccess)) return false;
	const auto Metrics = Page.Data->GetObjectField(TEXT("metrics"));
	TestTrue(TEXT("Directory work is capped independently of result count"), Metrics->GetNumberField(TEXT("directories_visited")) <= 128);
	TestTrue(TEXT("Entry work is independently capped"), Metrics->GetNumberField(TEXT("entries_considered")) <= 1024);
	TestEqual(TEXT("Sparse tree returns zero matches"), Page.Data->GetNumberField(TEXT("count")), 0.0);
	TestTrue(TEXT("Budget-limited sparse directory traversal is partial"), Page.Data->GetBoolField(TEXT("partial")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchLatePageCancellation, "KismetAI.ReadSource.Search.LatePageCancellation", SourceSearchTest::Flags)
bool FSourceSearchLatePageCancellation::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	for (int32 Index = 0; Index < 41; ++Index) Fixture.Write(FString::Printf(TEXT("Entry%d.h"), Index));
	FKismetAICancellationToken OriginToken, PageToken, FreshToken;
	auto Args = SourceSearchTest::Request(TEXT("*.h"));
	const auto First = KismetAISourceSearch::Find(Args, Fixture.Roots, OriginToken);
	if (!TestTrue(TEXT("Original page succeeds"), First.bSuccess)) return false;
	Args->SetStringField(TEXT("cursor"), First.Data->GetStringField(TEXT("cursor")));
	const auto Second = KismetAISourceSearch::Find(Args, Fixture.Roots, PageToken);
	if (!TestTrue(TEXT("Independent continuation token succeeds"), Second.bSuccess)) return false;
	Args->SetStringField(TEXT("cursor"), Second.Data->GetStringField(TEXT("cursor")));
	PageToken.Cancel();
	KismetAISourceSearch::ReapExpired();
	TestEqual(TEXT("Cancellation after continuation work invalidates its rotated cursor"), KismetAISourceSearch::Find(Args, Fixture.Roots, FreshToken).ErrorCode, FString(TEXT("INVALID_CURSOR")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchProbeFailurePartial, "KismetAI.ReadSource.Search.ProbeFailurePartial", SourceSearchTest::Flags)
bool FSourceSearchProbeFailurePartial::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	Fixture.Write(TEXT("Match.h"));
	FKismetAICancellationToken Token;
	bool bDeleted = false;
	const auto Result = KismetAISourceSearch::Find(SourceSearchTest::Request(TEXT("*.h")), Fixture.Roots, Token, 0, [&](int32 Entries)
	{
		if (Entries == 1) bDeleted = IFileManager::Get().Delete(*(Fixture.Source / TEXT("Match.h")));
	});
	TestTrue(TEXT("File disappears between enumeration and confined probe"), bDeleted);
	if (!TestTrue(TEXT("Probe failure retains a usable response"), Result.bSuccess)) return false;
	TestTrue(TEXT("Unattested matching candidate makes result explicitly partial"), Result.Data->GetBoolField(TEXT("partial")));
	TestEqual(TEXT("No false result for disappeared file"), Result.Data->GetNumberField(TEXT("count")), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceReadConcurrentTruncation, "KismetAI.ReadSource.Search.ConcurrentTruncation", SourceSearchTest::Flags)
bool FSourceReadConcurrentTruncation::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	Fixture.Write(TEXT("Changing.h"), FString::ChrN(128 * 1024, TEXT('x')));
	bool bTruncated = false;
	FKismetAISourceReadOptions Options;
	Options.OnChunkForTests = [&](int64 Bytes)
	{
		if (Bytes == 64 * 1024)
		{
			TUniquePtr<FArchive> Writer(IFileManager::Get().CreateFileWriter(*(Fixture.Source / TEXT("Changing.h")), FILEWRITE_AllowRead));
			bTruncated = Writer.IsValid() && Writer->Close();
		}
	};
	TArray<uint8> Bytes;
	FString Final;
	FKismetAISourceReadMetrics Metrics;
	const auto Status = KismetAISourceConfinement::ReadConfined(Fixture.Source / TEXT("Changing.h"), Fixture.Roots, Bytes, Final, Options, &Metrics);
	TestTrue(TEXT("File truncated after first actual read chunk"), bTruncated);
	TestEqual(TEXT("Premature EOF is explicit file-change outcome"), Status, EKismetAISourceReadStatus::Changed);
	TestEqual(TEXT("Partial content is not presented as complete"), Bytes.Num(), 0);
	TestEqual(TEXT("Actual bytes read remain reported"), Metrics.BytesRead, static_cast<int64>(64 * 1024));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSourceSearchPathologicalPattern, "KismetAI.ReadSource.Search.PathologicalPattern", SourceSearchTest::Flags)
bool FSourceSearchPathologicalPattern::RunTest(const FString& Parameters)
{
	SourceSearchTest::FFixture Fixture;
	FString Pattern;
	for (int32 Index = 0; Index < 60; ++Index) Pattern += TEXT("*a");
	Pattern += TEXT("Z.h");
	TestEqual(TEXT("Adversarial pattern stays inside public length contract"), Pattern.Len(), 123);
	TestTrue(TEXT("Alternating-star pattern is supported syntax"), KismetAISourceContract::PatternError(Pattern).IsEmpty());
	const FString Prefix = FString::ChrN(200, TEXT('a'));
	for (int32 Index = 0; Index < 256; ++Index)
	{
		const FString Name = Prefix + FString::Printf(TEXT("%03d.h"), Index);
		TestFalse(TEXT("Long repeated candidate cannot satisfy final literal"), KismetAISourceContract::Matches(Name, Pattern));
		if (!TestTrue(TEXT("Pathological-pattern fixture created"), Fixture.Write(Name))) return false;
	}
	FKismetAICancellationToken Token;
	const double Started = FPlatformTime::Seconds();
	const auto Result = KismetAISourceSearch::Find(SourceSearchTest::Request(Pattern), Fixture.Roots, Token);
	const double Elapsed = FPlatformTime::Seconds() - Started;
	if (!TestTrue(TEXT("Pathological supported search returns bounded response"), Result.bSuccess)) return false;
	const auto Metrics = Result.Data->GetObjectField(TEXT("metrics"));
	TestEqual(TEXT("Pathological pattern returns no false matches"), Result.Data->GetNumberField(TEXT("count")), 0.0);
	TestTrue(TEXT("Pathological pattern respects entry budget"), Metrics->GetNumberField(TEXT("entries_considered")) > 0 && Metrics->GetNumberField(TEXT("entries_considered")) <= 1024);
	TestTrue(TEXT("Pathological pattern respects directory budget"), Metrics->GetNumberField(TEXT("directories_visited")) <= 128);
	TestEqual(TEXT("Filename matching reads no source content"), Metrics->GetNumberField(TEXT("bytes_read")), 0.0);
	TestTrue(TEXT("Serialized pathological-pattern result is bounded"), FTCHARToUTF8(*Result.ToEnvelopeString()).Length() <= 32768);
	// Local disposable-disk measurement, not a guarantee about blocking network filesystems.
	TestTrue(TEXT("Pathological pattern completes local page within one second"), Elapsed < 1.0);
	AddInfo(FString::Printf(TEXT("Pathological glob: %d chars, 256 long candidates, %.3f ms, %.0f entries, %.0f directories"), Pattern.Len(), Elapsed * 1000, Metrics->GetNumberField(TEXT("entries_considered")), Metrics->GetNumberField(TEXT("directories_visited"))));
	return true;
}
