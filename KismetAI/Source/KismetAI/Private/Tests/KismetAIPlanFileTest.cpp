// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIPlanFile.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"

constexpr auto PlanFileTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

// ─────────────────────────────────────────────────────────────────────────────
// Slugify tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_Slugify_Basic,
	"KismetAI.PlanFile.Slugify.Basic",
	PlanFileTestFlags)
bool FPlanFile_Slugify_Basic::RunTest(const FString& Parameters)
{
	TestEqual(
		TEXT("Title with spaces becomes snake_case"),
		FKismetAIPlanFile::Slugify(TEXT("Hello World Blueprint")),
		FString(TEXT("hello_world_blueprint")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_Slugify_CollapsePunctuation,
	"KismetAI.PlanFile.Slugify.CollapsePunctuation",
	PlanFileTestFlags)
bool FPlanFile_Slugify_CollapsePunctuation::RunTest(const FString& Parameters)
{
	// Multiple consecutive non-alphanumeric chars collapse to one underscore.
	// Leading and trailing underscores are trimmed.
	FString Result = FKismetAIPlanFile::Slugify(TEXT("  -- Hello!!World --  "));
	TestEqual(TEXT("Punctuation run collapses and trims"), Result, FString(TEXT("hello_world")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_Slugify_EmptyFallback,
	"KismetAI.PlanFile.Slugify.EmptyFallback",
	PlanFileTestFlags)
bool FPlanFile_Slugify_EmptyFallback::RunTest(const FString& Parameters)
{
	// Empty string -> "plan"
	TestEqual(TEXT("Empty input returns 'plan'"),
		FKismetAIPlanFile::Slugify(TEXT("")),
		FString(TEXT("plan")));

	// All symbols -> "plan"
	TestEqual(TEXT("All-symbol input returns 'plan'"),
		FKismetAIPlanFile::Slugify(TEXT("---!!!---")),
		FString(TEXT("plan")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// MakeFileName tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_MakeFileName_Basic,
	"KismetAI.PlanFile.MakeFileName.Basic",
	PlanFileTestFlags)
bool FPlanFile_MakeFileName_Basic::RunTest(const FString& Parameters)
{
	FString FileName = FKismetAIPlanFile::MakeFileName(
		TEXT("hello_world_blueprint"),
		TEXT("8d454f27"));

	TestEqual(
		TEXT("Filename is Slug_Hash.plan.md"),
		FileName,
		FString(TEXT("hello_world_blueprint_8d454f27.plan.md")));
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildMarkdown tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_BuildMarkdown_Structure,
	"KismetAI.PlanFile.BuildMarkdown.Structure",
	PlanFileTestFlags)
bool FPlanFile_BuildMarkdown_Structure::RunTest(const FString& Parameters)
{
	// Build a Todos array with one completed and one pending entry.
	TArray<TSharedPtr<FJsonValue>> Todos;

	TSharedPtr<FJsonObject> CompletedTodo = MakeShared<FJsonObject>();
	CompletedTodo->SetStringField(TEXT("content"), TEXT("Design the system"));
	CompletedTodo->SetStringField(TEXT("status"),  TEXT("completed"));
	Todos.Add(MakeShared<FJsonValueObject>(CompletedTodo));

	TSharedPtr<FJsonObject> PendingTodo = MakeShared<FJsonObject>();
	PendingTodo->SetStringField(TEXT("content"), TEXT("Implement it"));
	PendingTodo->SetStringField(TEXT("status"),  TEXT("pending"));
	Todos.Add(MakeShared<FJsonValueObject>(PendingTodo));

	FString Markdown = FKismetAIPlanFile::BuildMarkdown(
		TEXT("My Plan"),
		TEXT("A short description."),
		TEXT("Step 1: do the thing."),
		Todos,
		TEXT("my_plan"),
		TEXT("2026-06-08T00:00:00Z"));

	// ── Frontmatter ──
	TestTrue(TEXT("Has opening frontmatter delimiter"),   Markdown.Contains(TEXT("---")));
	TestTrue(TEXT("Has title: in frontmatter"),           Markdown.Contains(TEXT("title: My Plan")));
	TestTrue(TEXT("Has slug: in frontmatter"),            Markdown.Contains(TEXT("slug: my_plan")));
	TestTrue(TEXT("Has created: in frontmatter"),         Markdown.Contains(TEXT("created: 2026-06-08T00:00:00Z")));

	// ── H1 heading ──
	TestTrue(TEXT("Has # heading"),                       Markdown.Contains(TEXT("# My Plan")));

	// ── Description ──
	TestTrue(TEXT("Has description text"),                Markdown.Contains(TEXT("A short description.")));

	// ── To-dos section ──
	TestTrue(TEXT("Has ## To-dos heading"),               Markdown.Contains(TEXT("## To-dos")));
	TestTrue(TEXT("Completed todo is checked"),           Markdown.Contains(TEXT("- [x] Design the system")));
	TestTrue(TEXT("Pending todo is unchecked"),           Markdown.Contains(TEXT("- [ ] Implement it")));

	// ── Plan section ──
	TestTrue(TEXT("Has ## Plan heading"),                 Markdown.Contains(TEXT("## Plan")));
	TestTrue(TEXT("Body text is present"),                Markdown.Contains(TEXT("Step 1: do the thing.")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_BuildMarkdown_EmptyTodos,
	"KismetAI.PlanFile.BuildMarkdown.EmptyTodos",
	PlanFileTestFlags)
bool FPlanFile_BuildMarkdown_EmptyTodos::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> EmptyTodos;

	FString Markdown = FKismetAIPlanFile::BuildMarkdown(
		TEXT("No Todos Plan"),
		TEXT(""),
		TEXT("Just the body."),
		EmptyTodos,
		TEXT("no_todos_plan"),
		TEXT("2026-06-08T00:00:00Z"));

	// Empty Todos -> no To-dos section at all.
	TestFalse(TEXT("No ## To-dos section when todos empty"), Markdown.Contains(TEXT("## To-dos")));

	// Plan section still present.
	TestTrue(TEXT("## Plan heading still present"), Markdown.Contains(TEXT("## Plan")));
	TestTrue(TEXT("Body still present"),            Markdown.Contains(TEXT("Just the body.")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_BuildMarkdown_EmptyDescription,
	"KismetAI.PlanFile.BuildMarkdown.EmptyDescription",
	PlanFileTestFlags)
bool FPlanFile_BuildMarkdown_EmptyDescription::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> EmptyTodos;

	FString Markdown = FKismetAIPlanFile::BuildMarkdown(
		TEXT("Plan Without Desc"),
		TEXT(""),
		TEXT("Body here."),
		EmptyTodos,
		TEXT("plan_without_desc"),
		TEXT("2026-06-08T00:00:00Z"));

	// Description line should not appear.
	TestFalse(TEXT("No empty description line"), Markdown.Contains(TEXT("\n\n\n")));
	TestTrue(TEXT("Title heading present"), Markdown.Contains(TEXT("# Plan Without Desc")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_BuildMarkdown_MalformedTodosSkipped,
	"KismetAI.PlanFile.BuildMarkdown.MalformedTodosSkipped",
	PlanFileTestFlags)
bool FPlanFile_BuildMarkdown_MalformedTodosSkipped::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Todos;

	// A valid entry.
	TSharedPtr<FJsonObject> GoodTodo = MakeShared<FJsonObject>();
	GoodTodo->SetStringField(TEXT("content"), TEXT("Valid entry"));
	GoodTodo->SetStringField(TEXT("status"),  TEXT("pending"));
	Todos.Add(MakeShared<FJsonValueObject>(GoodTodo));

	// A null entry (should be skipped without crash).
	Todos.Add(nullptr);

	// A string value (not an object — should be skipped).
	Todos.Add(MakeShared<FJsonValueString>(TEXT("not an object")));

	// An object missing "content" (should be skipped).
	TSharedPtr<FJsonObject> MissingContent = MakeShared<FJsonObject>();
	MissingContent->SetStringField(TEXT("status"), TEXT("pending"));
	Todos.Add(MakeShared<FJsonValueObject>(MissingContent));

	FString Markdown = FKismetAIPlanFile::BuildMarkdown(
		TEXT("Test"),
		TEXT(""),
		TEXT("body"),
		Todos,
		TEXT("test"),
		TEXT("2026-06-08T00:00:00Z"));

	TestTrue(TEXT("Valid entry is present"),   Markdown.Contains(TEXT("- [ ] Valid entry")));
	TestTrue(TEXT("To-dos section present"),   Markdown.Contains(TEXT("## To-dos")));

	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildPlanReadyEvent tests
// ─────────────────────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlanFile_BuildPlanReadyEvent_BackwardsCompat,
	"KismetAI.PlanFile.BuildPlanReadyEvent.BackwardsCompat",
	PlanFileTestFlags)
bool FPlanFile_BuildPlanReadyEvent_BackwardsCompat::RunTest(const FString& Parameters)
{
	TArray<TSharedPtr<FJsonValue>> Todos;

	TSharedPtr<FJsonObject> Todo1 = MakeShared<FJsonObject>();
	Todo1->SetStringField(TEXT("content"), TEXT("First todo"));
	Todo1->SetStringField(TEXT("status"),  TEXT("completed"));
	Todos.Add(MakeShared<FJsonValueObject>(Todo1));

	TSharedPtr<FJsonObject> Todo2 = MakeShared<FJsonObject>();
	Todo2->SetStringField(TEXT("content"), TEXT("Second todo"));
	Todo2->SetStringField(TEXT("status"),  TEXT("pending"));
	Todos.Add(MakeShared<FJsonValueObject>(Todo2));

	const FString Body      = TEXT("Step A, Step B, Step C.");
	const FString Title     = TEXT("My Blueprint Plan");
	const FString FilePath  = TEXT("/some/path/my_blueprint_plan_8d454f27.plan.md");
	const FString Slug      = TEXT("my_blueprint_plan");
	const FString Desc      = TEXT("Builds a cool blueprint.");

	TSharedPtr<FJsonObject> Event = FKismetAIPlanFile::BuildPlanReadyEvent(
		Title, Desc, Body, Todos, FilePath, Slug);

	TestTrue(TEXT("Event object is valid"), Event.IsValid());

	// type
	FString TypeVal;
	TestTrue(TEXT("Has 'type' field"),        Event->TryGetStringField(TEXT("type"), TypeVal));
	TestEqual(TEXT("type is 'plan_ready'"),   TypeVal, FString(TEXT("plan_ready")));

	// plan — backwards-compatibility guard: must equal Body
	FString PlanVal;
	TestTrue(TEXT("Has 'plan' field"),                    Event->TryGetStringField(TEXT("plan"), PlanVal));
	TestEqual(TEXT("'plan' field equals Body (guard)"), PlanVal, Body);

	// title
	FString TitleVal;
	TestTrue(TEXT("Has 'title' field"),       Event->TryGetStringField(TEXT("title"), TitleVal));
	TestEqual(TEXT("title is populated"),     TitleVal, Title);

	// file_path
	FString FilePathVal;
	TestTrue(TEXT("Has 'file_path' field"),   Event->TryGetStringField(TEXT("file_path"), FilePathVal));
	TestEqual(TEXT("file_path is populated"), FilePathVal, FilePath);

	// slug
	FString SlugVal;
	TestTrue(TEXT("Has 'slug' field"),        Event->TryGetStringField(TEXT("slug"), SlugVal));
	TestEqual(TEXT("slug is populated"),      SlugVal, Slug);

	// todos array count
	const TArray<TSharedPtr<FJsonValue>>* TodosArr = nullptr;
	TestTrue(TEXT("Has 'todos' array"),       Event->TryGetArrayField(TEXT("todos"), TodosArr));
	TestTrue(TEXT("Todos array is non-null"), TodosArr != nullptr);
	if (TodosArr)
	{
		TestEqual(TEXT("todos has expected count"), TodosArr->Num(), 2);
	}

	// "created" must NOT be present in the event
	TestFalse(TEXT("No 'created' field in event"), Event->HasField(TEXT("created")));

	return true;
}
