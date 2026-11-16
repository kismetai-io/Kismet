// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Budget the request that will actually be sent.
//
// The bug (R3): the context-pressure pass decided to microcompact from the
// input count the provider reported for the PREVIOUS request, cleared old
// tool-result bodies, then decided whether a full summary was needed from the
// same, unchanged count. A 96%-full request whose bulk was one old tool result
// was summarized even though clearing that result left the next request at a
// fraction of the window. In the other direction the reported count belonged to
// the previous request, so tool results appended since then — and anything
// else added after the check — reached the provider unbudgeted.
//
// These tests drive RunTask through the scripted seams and inspect what was
// sent (how many summary requests, what the main requests carried, how large
// they were), not the arithmetic of a threshold helper.

#include "Fakes/ScriptedAgentSeams.h"
#include "KismetAISessionStorage.h"
#include "KismetAITurnContext.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

using namespace KismetAITestFakes;

constexpr auto RequestBudgetTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

namespace RequestBudgetTest
{
	const TCHAR* const SmallModel = TEXT("claude-haiku-4-5-20251001");  // 200,000-token window
	const TCHAR* const UnknownModel = TEXT("acme-local-model-v1");      // not in the catalog
	const TCHAR* const Prompt = TEXT("Review BP_Door and tidy its event graph.");
	const TCHAR* const Summary = TEXT("<summary>The agent reviewed BP_Door's event graph.</summary>");
	const TCHAR* const ClearedMarker = TEXT("[tool result cleared");

	FKismetAIAgentConfig MakeBudgetConfig(const TCHAR* Model = SmallModel)
	{
		FKismetAIAgentConfig C;
		C.ApiKey = TEXT("test-key-unused");
		C.Model = Model;
		C.MaxSteps = 6;
		C.MaxRetries = 0;
		C.RetryJitterFactor = 0.0;
		C.ThinkingMode = EThinkingMode::Disabled;
		return C;
	}

	FString JsonText(const TSharedPtr<FJsonValue>& Value)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Out;
	}

	FString Line(const TSharedPtr<FJsonObject>& Obj)
	{
		return JsonText(MakeShared<FJsonValueObject>(Obj)) + TEXT("\n");
	}

	FString Header(const TCHAR* Model)
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("type"), TEXT("session_start"));
		S->SetNumberField(TEXT("v"), 1);
		S->SetStringField(TEXT("model"), Model);
		return Line(S);
	}

	FString User(const FString& Text)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("user_message"));
		E->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		E->SetStringField(TEXT("content"), Text);
		return Line(E);
	}

	FString AssistantText(const FString& Text)
	{
		TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("type"), TEXT("assistant_message"));
		E->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		E->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(MakeTextBlock(Text)) });
		return Line(E);
	}

	FString Round(const FString& Id, const FString& Result, const FString& Prose = FString())
	{
		TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
		Input->SetStringField(TEXT("blueprint"), TEXT("/Game/BP_Door"));
		TArray<TSharedPtr<FJsonValue>> Blocks;
		if (!Prose.IsEmpty())
		{
			Blocks.Add(MakeShared<FJsonValueObject>(MakeTextBlock(Prose)));
		}
		Blocks.Add(MakeShared<FJsonValueObject>(MakeToolUseBlock(Id, TEXT("read_blueprint"), Input)));

		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(TEXT("type"), TEXT("assistant_message"));
		A->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		A->SetArrayField(TEXT("content"), Blocks);

		TSharedPtr<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("tool_result"));
		Block->SetStringField(TEXT("tool_use_id"), Id);
		Block->SetStringField(TEXT("content"), Result);

		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("type"), TEXT("tool_result_message"));
		R->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString());
		R->SetArrayField(TEXT("results"), { MakeShared<FJsonValueObject>(Block) });
		return Line(A) + Line(R);
	}

	FString Repeat(const FString& Unit, int32 MinChars)
	{
		FString Out;
		Out.Reserve(MinChars + Unit.Len());
		while (Out.Len() < MinChars)
		{
			Out += Unit;
		}
		return Out;
	}

	struct FBudgetSession
	{
		FString SessionId;
		explicit FBudgetSession(const FString& Jsonl)
		{
			SessionId = FKismetAISessionStorage::CreateSessionId();
			FFileHelper::SaveStringToFile(Jsonl, *FKismetAISessionStorage::GetSessionFilePath(SessionId),
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		~FBudgetSession() { FKismetAISessionStorage::DeleteSession(SessionId); }
		FString Text() const
		{
			FString Out;
			FFileHelper::LoadFileToString(Out, *FKismetAISessionStorage::GetSessionFilePath(SessionId));
			return Out;
		}
		TArray<TSharedPtr<FJsonObject>> EntriesOfType(const FString& Type) const
		{
			TArray<FString> Lines;
			Text().ParseIntoArrayLines(Lines);
			TArray<TSharedPtr<FJsonObject>> Out;
			for (const FString& L : Lines)
			{
				TSharedPtr<FJsonObject> Obj;
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(L);
				if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() && Obj->GetStringField(TEXT("type")) == Type)
				{
					Out.Add(Obj);
				}
			}
			return Out;
		}
	};

	// The local size of a captured request, by the same rough rule the runtime
	// uses (about four characters per token, a flat cost per attachment):
	// messages, system prompt and every tool schema that was sent.
	int32 CapturedTokens(const FCapturedRequest& R)
	{
		int64 FixedChars = R.SystemPrompt.Len();
		for (const TSharedPtr<FJsonValue>& Tool : R.Tools)
		{
			FixedChars += JsonText(Tool).Len();
		}
		return FKismetAIContextPolicy::EstimateMessageTokens(R.Messages) + static_cast<int32>((FixedChars + 3) / 4);
	}

	FString AllText(const TArray<TSharedPtr<FJsonValue>>& Messages)
	{
		FString Out;
		for (const TSharedPtr<FJsonValue>& M : Messages)
		{
			Out += JsonText(M);
		}
		return Out;
	}

	TArray<const FCapturedRequest*> MainCalls(const FScriptedModelClient& Model)
	{
		TArray<const FCapturedRequest*> Out;
		for (const FCapturedRequest& R : Model.GetRequests())
		{
			if (R.Tools.Num() > 0)
			{
				Out.Add(&R);
			}
		}
		return Out;
	}

	int32 SummaryCalls(const FScriptedModelClient& Model)
	{
		return Model.NumCalls() - MainCalls(Model).Num();
	}

	TSharedPtr<FJsonObject> Args(const TCHAR* Key, const FString& Value)
	{
		TSharedPtr<FJsonObject> A = MakeShared<FJsonObject>();
		A->SetStringField(Key, Value);
		return A;
	}

	// Usable window of the 200k model for a 4,096-token default output budget,
	// and 96% of it: the previous request really was almost full.
	constexpr int32 SmallUsable = 200000 - 4096;
	const int32 NearlyFull = static_cast<int32>(SmallUsable * 0.96);
}

namespace RBT = RequestBudgetTest;

// R3, first half. The previous request was 96% full, and almost all of it was
// one old tool result. Clearing that result is enough: the next request must be
// sent without a summary, and with the result cleared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_ClearedResultAvoidsSummary,
	"KismetAI.RequestBudget.ClearedResultAvoidsSummary", RequestBudgetTestFlags)
bool FRequestBudget_ClearedResultAvoidsSummary::RunTest(const FString& Parameters)
{
	// ~120k tokens of old T3D in one result; the rest is small.
	const FString Huge = RBT::Repeat(TEXT("Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction End Object "), 480000);
	FString J = RBT::Header(RBT::SmallModel) + RBT::User(RBT::Prompt);
	J += RBT::Round(TEXT("toolu_huge"), Huge);
	J += RBT::Round(TEXT("toolu_s1"), TEXT("{\"nodes\":[]}"));
	J += RBT::Round(TEXT("toolu_s2"), TEXT("{\"nodes\":[]}"));
	J += RBT::Round(TEXT("toolu_s3"), TEXT("{\"nodes\":[]}"));
	J += RBT::AssistantText(TEXT("Graph reviewed."));
	RBT::FBudgetSession Session(J);

	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("toolu_next"), TEXT("read_blueprint"), RBT::Args(TEXT("blueprint"), TEXT("/Game/BP_Door"))) },
			TEXT("tool_use"), RBT::NearlyFull, 50),
		MakeTextTurn(RBT::Summary),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	{
		FKismetAIAgent Agent(Tools, Model, RBT::MakeBudgetConfig());
		Agent.RunTask(TEXT("Continue."), Rec.MakeCallback(), TEXT(""), Session.SessionId);
	}

	TestEqual(TEXT("no summary request: clearing the old result was enough"), RBT::SummaryCalls(Model), 0);
	const TArray<const FCapturedRequest*> Main = RBT::MainCalls(Model);
	if (!TestEqual(TEXT("two main requests"), Main.Num(), 2))
	{
		return false;
	}
	const FString Next = RBT::AllText(Main[1]->Messages);
	TestFalse(TEXT("the huge old result is not sent again"), Next.Contains(TEXT("K2Node_CallFunction End Object Begin")));
	TestTrue(TEXT("it is cleared, not dropped"), Next.Contains(RBT::ClearedMarker));
	TestTrue(TEXT("the original request is still there"), Next.Contains(RBT::Prompt));
	TestTrue(TEXT("the next request is far below the window"), RBT::CapturedTokens(*Main[1]) < RBT::SmallUsable / 2);
	TestFalse(TEXT("no summary was journaled"), Session.Text().Contains(TEXT("\"op\":\"summarize\"")));

	// The decision is explained with the estimate of the request it was about.
	bool bCalibrated = false;
	for (const TSharedPtr<FJsonObject>& E : Session.EntriesOfType(TEXT("context_compaction")))
	{
		const TSharedPtr<FJsonObject>* Estimate = nullptr;
		FString Basis;
		if (E->TryGetObjectField(TEXT("estimate"), Estimate) && (*Estimate)->TryGetStringField(TEXT("basis"), Basis)
			&& Basis == TEXT("reported_usage_plus_estimated_delta"))
		{
			bCalibrated = true;
			TestEqual(TEXT("the reported count is kept apart from the estimate"),
				(int32)(*Estimate)->GetNumberField(TEXT("last_reported_input_tokens")), RBT::NearlyFull);
			TestTrue(TEXT("the estimate after clearing is recomputed, not the reported count"),
				(*Estimate)->GetNumberField(TEXT("tokens_after")) < RBT::SmallUsable / 2);
		}
	}
	TestTrue(TEXT("the microcompaction record is calibrated by the reported usage"), bCalibrated);
	return true;
}

// R3, second half. The previous request was just as full, but the bulk is
// assistant prose that clearing tool output cannot shrink. A summary is still
// needed, and the request after it fits.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_RetainedContentStillSummarizes,
	"KismetAI.RequestBudget.RetainedContentStillSummarizes", RequestBudgetTestFlags)
bool FRequestBudget_RetainedContentStillSummarizes::RunTest(const FString& Parameters)
{
	const FString Prose = RBT::Repeat(TEXT("I inspected the next node and its pins in detail before deciding. "), 240000);
	FString J = RBT::Header(RBT::SmallModel) + RBT::User(RBT::Prompt);
	J += RBT::Round(TEXT("toolu_p1"), TEXT("{\"nodes\":[]}"), Prose);
	J += RBT::Round(TEXT("toolu_p2"), TEXT("{\"nodes\":[]}"), Prose);
	J += RBT::Round(TEXT("toolu_s1"), TEXT("{\"nodes\":[]}"));
	J += RBT::Round(TEXT("toolu_s2"), TEXT("{\"nodes\":[]}"));
	J += RBT::AssistantText(TEXT("Graph reviewed."));
	RBT::FBudgetSession Session(J);

	FScriptedModelClient Model({
		MakeResponse({ MakeToolUseBlock(TEXT("toolu_next"), TEXT("read_blueprint"), RBT::Args(TEXT("blueprint"), TEXT("/Game/BP_Door"))) },
			TEXT("tool_use"), RBT::NearlyFull, 50),
		MakeTextTurn(RBT::Summary),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	{
		FKismetAIAgent Agent(Tools, Model, RBT::MakeBudgetConfig());
		Agent.RunTask(TEXT("Continue."), Rec.MakeCallback(), TEXT(""), Session.SessionId);
	}

	if (!TestEqual(TEXT("main, summary, main"), Model.NumCalls(), 3))
	{
		return false;
	}
	TestEqual(TEXT("the second call is the summary"), Model.GetRequests()[1].Tools.Num(), 0);
	const FCapturedRequest& After = Model.GetRequests()[2];
	TestTrue(TEXT("the summary is in the next request"), RBT::AllText(After.Messages).Contains(TEXT("reviewed BP_Door")));
	TestFalse(TEXT("the prose is gone"), RBT::AllText(After.Messages).Contains(TEXT("pins in detail before deciding. I inspected")));
	TestTrue(TEXT("the request after the summary fits"), RBT::CapturedTokens(After) < RBT::SmallUsable / 2);
	return true;
}

// A small previous request, then tool results large enough to overflow: the
// next request is budgeted before it is sent — the oldest of the new results is
// cleared — instead of being judged by the previous, small, reported count.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_NewLargeResultsBudgetedBeforeSend,
	"KismetAI.RequestBudget.NewLargeResultsBudgetedBeforeSend", RequestBudgetTestFlags)
bool FRequestBudget_NewLargeResultsBudgetedBeforeSend::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config = RBT::MakeBudgetConfig();
	Config.MaxToolResultChars = 400000;  // a project that allows long search output

	const FString Listing = RBT::Repeat(TEXT("/Game/Props/SM_Crate_Variant.SM_Crate_Variant StaticMesh "), 320000);
	FScriptedToolInvoker Tools;
	Tools.SetResult(TEXT("search_assets"), Listing);

	FScriptedModelClient Model({
		MakeResponse({
				MakeToolUseBlock(TEXT("toolu_a"), TEXT("search_assets"), RBT::Args(TEXT("query"), TEXT("crate"))),
				MakeToolUseBlock(TEXT("toolu_b"), TEXT("search_assets"), RBT::Args(TEXT("query"), TEXT("barrel"))),
				MakeToolUseBlock(TEXT("toolu_c"), TEXT("read_blueprint"), RBT::Args(TEXT("blueprint"), TEXT("/Game/BP_Door"))) },
			TEXT("tool_use"), 26000, 80),
		MakeTextTurn(TEXT("Done.")) });
	FEventRecorder Rec;
	{
		FKismetAIAgent Agent(Tools, Model, Config);
		Agent.RunTask(RBT::Prompt, Rec.MakeCallback());
	}

	TestEqual(TEXT("both searches ran"), Tools.CountCalls(TEXT("search_assets")), 2);
	const TArray<const FCapturedRequest*> Main = RBT::MainCalls(Model);
	if (!TestEqual(TEXT("two main requests"), Main.Num(), 2))
	{
		return false;
	}
	const int32 Sent = RBT::CapturedTokens(*Main[1]);
	TestTrue(FString::Printf(TEXT("the request carrying the new results (~%d tokens) is under the compaction threshold"), Sent),
		Sent <= static_cast<int32>(RBT::SmallUsable * 0.90f));
	TestTrue(TEXT("the oldest new result was cleared before sending"), RBT::AllText(Main[1]->Messages).Contains(RBT::ClearedMarker));
	TestTrue(TEXT("the budget decision is reported"), Rec.HasType(TEXT("context_budget")));
	return true;
}

// The system prompt alone (here a selected skill's instructions) does not fit.
// Trimming history cannot help, so nothing is sent: the run stops visibly
// instead of sending a request the estimate says cannot fit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_OversizedFixedContentStops,
	"KismetAI.RequestBudget.OversizedFixedContentStops", RequestBudgetTestFlags)
bool FRequestBudget_OversizedFixedContentStops::RunTest(const FString& Parameters)
{
	FKismetAIAgentConfig Config = RBT::MakeBudgetConfig();
	Config.bSkillRestricted = true;
	Config.SelectedSkill = TEXT("pasted-reference");
	Config.SkillTools = { TEXT("read_blueprint") };
	Config.SkillInstructions = RBT::Repeat(TEXT("Reference material the skill author pasted into the instructions. "), 900000);

	FScriptedModelClient Model({ MakeToolTurn(TEXT("toolu_r"), TEXT("read_blueprint"), RBT::Args(TEXT("blueprint"), TEXT("/Game/BP_Door"))),
		MakeTextTurn(TEXT("Done.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	FString Result;
	{
		FKismetAIAgent Agent(Tools, Model, Config);
		Result = Agent.RunTask(RBT::Prompt, Rec.MakeCallback());
	}

	TestEqual(TEXT("no request was sent"), Model.NumCalls(), 0);
	TestEqual(TEXT("no tool was dispatched"), Tools.CallLog.Num(), 0);
	const FString* Error = Rec.FirstJsonOfType(TEXT("error"));
	TestTrue(TEXT("a visible error names context compaction"), Error && Error->Contains(TEXT("context_compaction_failed")));
	const FString* Budget = Rec.FirstJsonOfType(TEXT("context_budget"));
	TestTrue(TEXT("the budget event attributes the size to the system prompt"),
		Budget && Budget->Contains(TEXT("\"system_tokens\"")) && Budget->Contains(TEXT("\"decision\":\"stop\"")));
	return true;
}

// An unknown model id has no catalog window. The budget assumes a conservative
// one and says so; the history (with images in the new message) is adapted
// against that assumption rather than against an optimistic default.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_UnknownModelConservative,
	"KismetAI.RequestBudget.UnknownModelIsConservativeAndAttributed", RequestBudgetTestFlags)
bool FRequestBudget_UnknownModelConservative::RunTest(const FString& Parameters)
{
	const FString Big = RBT::Repeat(TEXT("{\"node\":\"K2Node_CallFunction\",\"pins\":[\"exec\",\"then\"]} "), 160000);
	FString J = RBT::Header(RBT::UnknownModel) + RBT::User(RBT::Prompt);
	J += RBT::Round(TEXT("toolu_b1"), Big);
	J += RBT::Round(TEXT("toolu_b2"), Big);
	J += RBT::Round(TEXT("toolu_b3"), Big);
	J += RBT::Round(TEXT("toolu_s1"), TEXT("{\"nodes\":[]}"));
	J += RBT::Round(TEXT("toolu_s2"), TEXT("{\"nodes\":[]}"));
	J += RBT::AssistantText(TEXT("Graph reviewed."));
	RBT::FBudgetSession Session(J);

	const FString Images = TEXT("[{\"media_type\":\"image/png\",\"data\":\"iVBORw0KGgo=\"},"
		"{\"media_type\":\"image/png\",\"data\":\"iVBORw0KGgo=\"}]");
	FScriptedModelClient Model({ MakeTextTurn(TEXT("Ok.")) });
	FScriptedToolInvoker Tools;
	FEventRecorder Rec;
	{
		FKismetAIAgent Agent(Tools, Model, RBT::MakeBudgetConfig(RBT::UnknownModel));
		Agent.RunTask(TEXT("Here is a screenshot of the graph."), Rec.MakeCallback(), Images, Session.SessionId);
	}

	const TArray<const FCapturedRequest*> Main = RBT::MainCalls(Model);
	if (!TestEqual(TEXT("one main request"), Main.Num(), 1))
	{
		return false;
	}
	const int32 Sent = RBT::CapturedTokens(*Main[0]);
	TestTrue(FString::Printf(TEXT("the request (~%d tokens) fits a conservative 128k window"), Sent), Sent <= 128000 - 4096);
	TestTrue(TEXT("both images are still sent"), RBT::AllText(Main[0]->Messages).Contains(TEXT("iVBORw0KGgo=")));

	bool bAttributed = false;
	for (const TSharedPtr<FJsonObject>& E : Session.EntriesOfType(TEXT("context_compaction")))
	{
		const TSharedPtr<FJsonObject>* Estimate = nullptr;
		FString LimitSource;
		if (E->TryGetObjectField(TEXT("estimate"), Estimate) && (*Estimate)->TryGetStringField(TEXT("limit_source"), LimitSource))
		{
			bAttributed = true;
			TestEqual(TEXT("the window is labelled as an assumption"), LimitSource, FString(TEXT("assumed_unknown_model")));
			TestEqual(TEXT("the assumed window"), (int32)(*Estimate)->GetNumberField(TEXT("context_window")), 128000);
			const TSharedPtr<FJsonObject>* Before = nullptr;
			TestTrue(TEXT("components are recorded"), (*Estimate)->TryGetObjectField(TEXT("before"), Before));
			if (Before)
			{
				TestEqual(TEXT("both images are counted"), (int32)(*Before)->GetNumberField(TEXT("attachments")), 2);
				TestTrue(TEXT("at a stated per-attachment cost"), (*Before)->HasField(TEXT("attachment_tokens_each")));
			}
		}
	}
	TestTrue(TEXT("the adaptation is journaled with limit provenance"), bAttributed);
	return true;
}

// The estimator itself: what it counts, how a measurement anchors it, and the
// limit it is judged against. (The tests above are the acceptance; these pin
// the rules they rely on.)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_EstimateRules,
	"KismetAI.RequestBudget.EstimateRules", RequestBudgetTestFlags)
bool FRequestBudget_EstimateRules::RunTest(const FString& Parameters)
{
	auto TextMsg = [](const TCHAR* Role, const FString& Text)
	{
		TSharedPtr<FJsonObject> M = MakeShared<FJsonObject>();
		M->SetStringField(TEXT("role"), Role);
		M->SetStringField(TEXT("content"), Text);
		return MakeShared<FJsonValueObject>(M);
	};
	TSharedPtr<FJsonObject> Image = MakeShared<FJsonObject>();
	Image->SetStringField(TEXT("type"), TEXT("image"));
	TSharedPtr<FJsonObject> WithImage = MakeShared<FJsonObject>();
	WithImage->SetStringField(TEXT("role"), TEXT("user"));
	WithImage->SetArrayField(TEXT("content"), { MakeShared<FJsonValueObject>(Image), MakeShared<FJsonValueObject>(MakeTextBlock(TEXT("see"))) });

	TArray<TSharedPtr<FJsonValue>> Messages = { TextMsg(TEXT("user"), RBT::Repeat(TEXT("abcd"), 16000)), MakeShared<FJsonValueObject>(WithImage) };
	TArray<TSharedPtr<FJsonValue>> Tools = { MakeShared<FJsonValueObject>(KismetAITestFakes::MakeToolUseBlock(TEXT("t"), RBT::Repeat(TEXT("x"), 800), MakeShared<FJsonObject>())) };
	const FString System = RBT::Repeat(TEXT("s"), 2000);
	const FKismetAIContextBudget Budget = FKismetAIContextBudget::Resolve(RBT::SmallModel, 4096);

	// Every part of the request is counted.
	const FKismetAIRequestEstimate Local = FKismetAIRequestEstimate::Estimate(System, Tools, Messages, FKismetAIUsageCalibration(), Budget);
	TestEqual(TEXT("system prompt"), Local.SystemTokens, 500);
	TestTrue(TEXT("tool schemas"), Local.ToolSchemaTokens >= 200);
	TestEqual(TEXT("one attachment"), Local.AttachmentCount, 1);
	TestTrue(TEXT("the attachment is charged at the flat cost"), Local.MessageTokens >= 4000 + FKismetAIRequestEstimate::AttachmentTokensEach);
	TestEqual(TEXT("local = system + tools + messages"), Local.LocalTokens, Local.SystemTokens + Local.ToolSchemaTokens + Local.MessageTokens);
	TestFalse(TEXT("no measurement: not calibrated"), Local.bCalibrated);
	TestEqual(TEXT("uncalibrated decision figure is the local estimate"), Local.Tokens, Local.LocalTokens);

	// A measurement of a request the heuristic under-counted by 25%: the same
	// request is estimated at the measured count, growth is scaled, removal
	// lowers it, and it never drops below the plain local estimate.
	FKismetAIUsageCalibration Cal;
	Cal.bValid = true;
	Cal.EstimatedInputTokens = Local.LocalTokens;
	Cal.ReportedInputTokens = static_cast<int32>(Local.LocalTokens * 1.25);
	const FKismetAIRequestEstimate Same = FKismetAIRequestEstimate::Estimate(System, Tools, Messages, Cal, Budget);
	TestTrue(TEXT("calibrated"), Same.bCalibrated);
	TestEqual(TEXT("the measured request is estimated at its measurement"), Same.Tokens, Cal.ReportedInputTokens);

	TArray<TSharedPtr<FJsonValue>> Grown = Messages;
	Grown.Add(TextMsg(TEXT("user"), RBT::Repeat(TEXT("abcd"), 16000)));  // +4,000 local tokens
	const FKismetAIRequestEstimate More = FKismetAIRequestEstimate::Estimate(System, Tools, Grown, Cal, Budget);
	TestTrue(TEXT("growth is scaled by the measured ratio"), FMath::Abs(More.Tokens - (Cal.ReportedInputTokens + 5000)) <= 8);

	TArray<TSharedPtr<FJsonValue>> Shrunk = { MakeShared<FJsonValueObject>(WithImage) };
	const FKismetAIRequestEstimate Less = FKismetAIRequestEstimate::Estimate(System, Tools, Shrunk, Cal, Budget);
	TestTrue(TEXT("removal lowers the estimate"), Less.Tokens < Same.Tokens);
	TestTrue(TEXT("never below the local estimate"), Less.Tokens >= Less.LocalTokens);

	// A measurement smaller than the local estimate does not lower the figure
	// below the local estimate; an implausible one is not extrapolated past 4x.
	FKismetAIUsageCalibration Low = Cal;
	Low.ReportedInputTokens = 10;
	TestEqual(TEXT("an under-report cannot shrink the estimate"),
		FKismetAIRequestEstimate::Estimate(System, Tools, Grown, Low, Budget).Tokens,
		FKismetAIRequestEstimate::Estimate(System, Tools, Grown, FKismetAIUsageCalibration(), Budget).Tokens);
	FKismetAIUsageCalibration Absurd = Cal;
	Absurd.ReportedInputTokens = Local.LocalTokens * 100;
	TestEqual(TEXT("ratio is capped"), Absurd.Ratio(), FKismetAIUsageCalibration::MaxRatio);

	// Limits: catalog, suffix, unknown (assumed and labelled), reserve cap.
	TestTrue(TEXT("catalog model"), Budget.LimitSource == EKismetAIContextLimitSource::Catalog && Budget.ContextWindow == 200000);
	TestEqual(TEXT("reserve is the requested max_tokens"), Budget.OutputReserve, 4096);
	TestEqual(TEXT("effective limit"), Budget.EffectiveLimit, 200000 - 4096);
	const FKismetAIContextBudget Big = FKismetAIContextBudget::Resolve(RBT::SmallModel, 64000);
	TestEqual(TEXT("reserve is capped"), Big.OutputReserve, FKismetAIContextBudget::OutputReserveCap);
	const FKismetAIContextBudget Suffix = FKismetAIContextBudget::Resolve(TEXT("claude-sonnet-4-20250514-1m"), 4096);
	TestTrue(TEXT("unsupported suffix retains catalog capacity"), Suffix.LimitSource == EKismetAIContextLimitSource::Catalog && Suffix.ContextWindow == 200000);
	const FKismetAIContextBudget Unknown = FKismetAIContextBudget::Resolve(RBT::UnknownModel, 4096);
	TestTrue(TEXT("unknown model is an assumption"), Unknown.LimitSource == EKismetAIContextLimitSource::AssumedUnknownModel);
	TestEqual(TEXT("unknown model window is conservative"), Unknown.ContextWindow, FKismetAIContextBudget::UnknownModelWindow);
	TestTrue(TEXT("the catalog default is still reported"), Unknown.CatalogWindow == FKismetAIAgent::GetModelContextLimit(RBT::UnknownModel));

	// Provenance survives serialization.
	const TSharedPtr<FJsonObject> Json = More.ToJson();
	TestEqual(TEXT("basis"), Json->GetStringField(TEXT("basis")), FString(TEXT("reported_usage_plus_estimated_delta")));
	TestEqual(TEXT("limit source"), Json->GetStringField(TEXT("limit_source")), FString(TEXT("catalog")));
	TestTrue(TEXT("calibration pair"), Json->GetObjectField(TEXT("calibration"))->HasField(TEXT("reported_input_tokens")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_UnknownSuffixDoesNotInventCapacity,
	"KismetAI.RequestBudget.UnknownSuffixDoesNotInventCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRequestBudget_UnknownSuffixDoesNotInventCapacity::RunTest(const FString& Parameters)
{
	const FKismetAIContextBudget Unknown = FKismetAIContextBudget::Resolve(TEXT("unlisted-local-model-1m"), 4096);
	TestEqual(TEXT("unknown suffix keeps conservative assumption"), Unknown.ContextWindow, FKismetAIContextBudget::UnknownModelWindow);
	TestTrue(TEXT("unknown suffix is labelled assumed"), Unknown.LimitSource == EKismetAIContextLimitSource::AssumedUnknownModel);
	const FKismetAIContextBudget Known = FKismetAIContextBudget::Resolve(TEXT("claude-sonnet-4-20250514-1m"), 4096);
	TestEqual(TEXT("known suffix cannot expand declared capacity"), Known.ContextWindow, 200000);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_KnownSuffixRequiresCapacityEvidence,
	"KismetAI.RequestBudget.KnownSuffixRequiresCapacityEvidence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRequestBudget_KnownSuffixRequiresCapacityEvidence::RunTest(const FString& Parameters)
{
	const FKismetAIContextBudget Haiku = FKismetAIContextBudget::Resolve(TEXT("claude-haiku-4-5-20251001-1m"), 4096);
	TestEqual(TEXT("Haiku suffix cannot manufacture capacity"), Haiku.ContextWindow, 200000);
	for (const FString& Model : { FString(TEXT("claude-opus-4-6-1m")), FString(TEXT("claude-sonnet-4-6-1m")), FString(TEXT("opus-1m")), FString(TEXT("sonnet-1m")) })
	{
		TestEqual(Model + TEXT(" retains documented capacity"), FKismetAIContextBudget::Resolve(Model, 4096).ContextWindow, 1000000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FRequestBudget_OperatingLimitAndProvenance,
	"KismetAI.RequestBudget.OperatingLimitAndProvenance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FRequestBudget_OperatingLimitAndProvenance::RunTest(const FString& Parameters)
{
	const FKismetAIContextBudget Budget = FKismetAIContextBudget::Resolve(RBT::UnknownModel, 1024, 32768);
	TestEqual(TEXT("configured local operating restriction"), Budget.ContextWindow, 32768);
	TestEqual(TEXT("unrestricted assumption retained"), Budget.UnrestrictedContextWindow, 128000);
	TestEqual(TEXT("restriction provenance retained"), Budget.OperatingContextLimit, 32768);
	TestEqual(TEXT("catalog default remains an assumption"), Budget.CatalogWindow, 200000);
	TestTrue(TEXT("restriction does not certify unknown capacity"), Budget.LimitSource == EKismetAIContextLimitSource::AssumedUnknownModel);
	TestEqual(TEXT("output reservation within operating window"), Budget.EffectiveLimit, 31744);
	FKismetAIRequestEstimate Estimate;
	Estimate.Budget = Budget;
	const TSharedPtr<FJsonObject> Json = Estimate.ToJson();
	double Unrestricted = 0;
	double Operating = 0;
	TestTrue(TEXT("serialized unrestricted provenance"), Json->TryGetNumberField(TEXT("unrestricted_context_window"), Unrestricted));
	TestTrue(TEXT("serialized operating provenance"), Json->TryGetNumberField(TEXT("operating_context_limit"), Operating));
	TestEqual(TEXT("serialized unrestricted value"), Unrestricted, 128000.0);
	TestEqual(TEXT("serialized operating value"), Operating, 32768.0);
	const FKismetAIContextBudget Known = FKismetAIContextBudget::Resolve(RBT::SmallModel, 4096, 500000);
	TestEqual(TEXT("restriction cannot enlarge catalog capacity"), Known.ContextWindow, 200000);
	TestEqual(TEXT("larger configured restriction still recorded"), Known.OperatingContextLimit, 500000);
	const FKismetAIContextBudget Unset = FKismetAIContextBudget::Resolve(RBT::SmallModel, 4096);
	TestEqual(TEXT("unset preserves capacity"), Unset.ContextWindow, 200000);
	TestEqual(TEXT("unset preserves usable input"), Unset.EffectiveLimit, 195904);
	return true;
}
