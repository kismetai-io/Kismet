// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

// Read_source path confinement, exercised through the production tool
// adapter (FKismetAIModuleToolInvoker -> FKismetAIModule::DoReadSource), not the
// JSON argument validator. Every forbidden fixture file carries a marker string;
// a denial must never return those bytes.

#include "KismetAIModule.h"
#include "KismetAIModuleToolInvoker.h"
#include "KismetAISourceConfinement.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <unistd.h>
#endif

namespace ReadSourceConfinementTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	const TCHAR* ForbiddenMarker = TEXT("P106_FORBIDDEN_MARKER_7f3a");
	const TCHAR* AllowedMarker = TEXT("P106_ALLOWED_MARKER_51c2");

	FString InvokeReadSource(const FString& File, const FString& Scope, const FString& Mode = TEXT("raw"))
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("mode"), Mode);
		Body->SetStringField(TEXT("file"), File);
		if (!Scope.IsEmpty())
		{
			Body->SetStringField(TEXT("scope"), Scope);
		}
		// The invoker directly, not the executor: these cases test the reader's own
		// confinement, below the argument validator that would refuse some first.
		FKismetAIModuleToolInvoker Invoker(FKismetAIModule::Get());
		FKismetAIToolInvocation Invocation;
		Invocation.ToolName = TEXT("read_source");
		Invocation.RequestBody = Body;
		Invocation.Caller = FKismetAICallerContext::InProcess();
		return Invoker.Invoke(Invocation).ModelString;
	}

	bool MakeSymlink(const FString& Target, const FString& LinkPath)
	{
#if PLATFORM_MAC || PLATFORM_LINUX
		::unlink(TCHAR_TO_UTF8(*LinkPath)); // removes an existing link itself, never its target
		return ::symlink(TCHAR_TO_UTF8(*Target), TCHAR_TO_UTF8(*LinkPath)) == 0;
#else
		return false;
#endif
	}

	void WriteFixture(const FString& Path, const TCHAR* Marker)
	{
		FFileHelper::SaveStringToFile(
			FString::Printf(TEXT("#pragma once\n// %s\nstruct FFixture {};\n"), Marker), *Path);
	}
}

// Real roots: the editor's own ProjectDir/EngineDir. The only fixture is a sibling of
// ProjectDir/Source (never inside it), removed afterwards. Symlink cases run against the
// disposable fixture below so the real Source tree is never written to.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSourceConfinement_RealRoots, "KismetAI.ReadSource.Confinement.RealRoots", ReadSourceConfinementTest::Flags)
bool FReadSourceConfinement_RealRoots::RunTest(const FString& Parameters)
{
	using namespace ReadSourceConfinementTest;

	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString SiblingDir = ProjectDir / TEXT("Source-KismetAIP106");
	const FString OutsideDir = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()) / TEXT("KismetAIP106Outside");
	IFileManager::Get().DeleteDirectory(*SiblingDir, false, true);
	IFileManager::Get().DeleteDirectory(*OutsideDir, false, true);

	WriteFixture(SiblingDir / TEXT("private.h"), ForbiddenMarker);
	WriteFixture(OutsideDir / TEXT("secret.h"), ForbiddenMarker);

	// 1. Adjacent-prefix sibling of the allowed Source root, by absolute path.
	{
		const FString R = InvokeReadSource(SiblingDir / TEXT("private.h"), TEXT("project"));
		TestFalse(TEXT("Sibling Source-* directory: marker bytes not returned"), R.Contains(ForbiddenMarker));
		TestTrue(TEXT("Sibling Source-* directory: ACCESS_DENIED"), R.Contains(TEXT("ACCESS_DENIED")));
	}
	// 2. Same sibling with no explicit scope (union of all permitted roots).
	{
		const FString R = InvokeReadSource(SiblingDir / TEXT("private.h"), FString());
		TestFalse(TEXT("Sibling (no scope): marker bytes not returned"), R.Contains(ForbiddenMarker));
	}
	// 3. Parent traversal through a relative path (the HTTP route skips the validator).
	{
		const FString R = InvokeReadSource(TEXT("KismetDevProject/../../Source-KismetAIP106/private.h"), TEXT("project"));
		TestFalse(TEXT("Relative traversal: marker bytes not returned"), R.Contains(ForbiddenMarker));
	}
	// 4. Project-only scope must not inherit engine permission for an absolute engine path.
	{
		const FString EngineHeader = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Source/Runtime/Engine/Classes/GameFramework/Actor.h"));
		const FString R = InvokeReadSource(EngineHeader, TEXT("project"));
		TestTrue(TEXT("Engine absolute path under scope=project: ACCESS_DENIED"), R.Contains(TEXT("ACCESS_DENIED")));
		TestFalse(TEXT("Engine absolute path under scope=project: no content"), R.Contains(TEXT("class ENGINE_API AActor")));
	}
	// Positive controls: engine-relative and engine-absolute reads under engine scope.
	{
		const FString R = InvokeReadSource(TEXT("Runtime/Engine/Classes/GameFramework/Actor.h"), TEXT("engine"), TEXT("outline"));
		TestTrue(TEXT("Engine-relative outline succeeds"), R.Contains(TEXT("AActor")) && !R.Contains(TEXT("ACCESS_DENIED")) && !R.Contains(TEXT("FILE_NOT_FOUND")));
		const FString EngineHeader = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Source/Runtime/Engine/Classes/GameFramework/Actor.h"));
		const FString R2 = InvokeReadSource(EngineHeader, TEXT("engine"));
		TestTrue(TEXT("Engine-absolute raw read under scope=engine succeeds"), R2.Contains(TEXT("Actor.h")) && !R2.Contains(TEXT("ACCESS_DENIED")));
		const FString R3 = InvokeReadSource(EngineHeader, FString());
		TestTrue(TEXT("Engine-absolute raw read with scope omitted succeeds"), R3.Contains(TEXT("Actor.h")) && !R3.Contains(TEXT("ACCESS_DENIED")));
		// The dev project links KismetAI into <Project>/Plugins with a symlink; a plugin UE
		// discovered there stays readable at its canonical location.
		const FString R4 = InvokeReadSource(TEXT("KismetAI/Source/KismetAI/Private/KismetAISourceConfinement.h"), TEXT("plugins"));
		TestTrue(TEXT("Discovered (symlinked) project plugin is readable under scope=plugins"), R4.Contains(TEXT("EKismetAISourceScope")) && !R4.Contains(TEXT("ACCESS_DENIED")));
	}

	IFileManager::Get().DeleteDirectory(*SiblingDir, false, true);
	IFileManager::Get().DeleteDirectory(*OutsideDir, false, true);
	return true;
}

// Disposable fixture with its own project and engine roots (installed through the
// reader's test seam), an adjacent-prefix sibling, nested files, symlinks and an outside
// target. Calls go through the production tool adapter.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSourceConfinement_Fixture, "KismetAI.ReadSource.Confinement.Fixture", ReadSourceConfinementTest::Flags)
bool FReadSourceConfinement_Fixture::RunTest(const FString& Parameters)
{
	using namespace ReadSourceConfinementTest;

	const FString Base = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()) / TEXT("KismetAIP106Fixture");
	IFileManager::Get().DeleteDirectory(*Base, false, true);
	const FString Project = Base / TEXT("Project");
	const FString Engine = Base / TEXT("Engine");
	const FString Outside = Base / TEXT("Outside");
	const FString Mod = Project / TEXT("Source/Mod");

	WriteFixture(Mod / TEXT("A.h"), AllowedMarker);
	WriteFixture(Mod / TEXT("Nested/B.h"), AllowedMarker);
	FFileHelper::SaveStringToFile(FString(ForbiddenMarker), *(Mod / TEXT("data.txt")));
	WriteFixture(Project / TEXT("Source-private/private.h"), ForbiddenMarker);
	WriteFixture(Project / TEXT("Plugins/PP/Source/PP.h"), AllowedMarker);
	WriteFixture(Engine / TEXT("Source/Runtime/E.h"), AllowedMarker);
	WriteFixture(Engine / TEXT("Plugins/EP/Source/EP.h"), AllowedMarker);
	WriteFixture(Outside / TEXT("secret.h"), ForbiddenMarker);

	FKismetAISourceRoots Roots;
	Roots.ProjectDir = Project;
	Roots.EngineDir = Engine;
	Roots.bIncludeDiscoveredPlugins = false;
	KismetAISourceConfinement::FScopedRootsOverride Override(Roots);

	auto ExpectAllowed = [this](const FString& Label, const FString& R)
	{
		TestTrue(Label + TEXT(": allowed content returned"), R.Contains(AllowedMarker));
		TestFalse(Label + TEXT(": not denied"), R.Contains(TEXT("ACCESS_DENIED")));
	};
	auto ExpectDenied = [this](const FString& Label, const FString& R)
	{
		TestFalse(Label + TEXT(": forbidden marker not returned"), R.Contains(ForbiddenMarker));
		TestTrue(Label + TEXT(": ACCESS_DENIED or FILE_NOT_FOUND"), R.Contains(TEXT("ACCESS_DENIED")) || R.Contains(TEXT("FILE_NOT_FOUND")));
	};

	// Positive controls: relative, root-relative, absolute, nested, mixed separators, other scopes.
	ExpectAllowed(TEXT("Relative project path"), InvokeReadSource(TEXT("Mod/A.h"), TEXT("project")));
	ExpectAllowed(TEXT("Root-relative project path"), InvokeReadSource(TEXT("Source/Mod/A.h"), FString()));
	ExpectAllowed(TEXT("Absolute project path"), InvokeReadSource(Mod / TEXT("A.h"), TEXT("project")));
	ExpectAllowed(TEXT("Nested header"), InvokeReadSource(TEXT("Mod/Nested/B.h"), TEXT("project")));
	ExpectAllowed(TEXT("Mixed separators"), InvokeReadSource(TEXT("Mod\\Nested\\B.h"), TEXT("project")));
	ExpectAllowed(TEXT("Engine relative"), InvokeReadSource(TEXT("Runtime/E.h"), TEXT("engine")));
	ExpectAllowed(TEXT("Engine absolute, scope omitted"), InvokeReadSource(Engine / TEXT("Source/Runtime/E.h"), FString()));
	ExpectAllowed(TEXT("Engine plugin"), InvokeReadSource(TEXT("EP/Source/EP.h"), TEXT("plugins")));
	ExpectAllowed(TEXT("Project plugin"), InvokeReadSource(TEXT("PP/Source/PP.h"), TEXT("plugins")));

	// Adjacent-prefix sibling, under every scope.
	const FString Sibling = Project / TEXT("Source-private/private.h");
	ExpectDenied(TEXT("Sibling, scope project"), InvokeReadSource(Sibling, TEXT("project")));
	ExpectDenied(TEXT("Sibling, scope omitted"), InvokeReadSource(Sibling, FString()));
	ExpectDenied(TEXT("Sibling, scope engine"), InvokeReadSource(Sibling, TEXT("engine")));

	// Traversal and mixed-separator traversal (the HTTP route has no argument validator).
	ExpectDenied(TEXT("Relative traversal"), InvokeReadSource(TEXT("Mod/../../Source-private/private.h"), TEXT("project")));
	ExpectDenied(TEXT("Backslash traversal"), InvokeReadSource(TEXT("Mod\\..\\..\\Source-private\\private.h"), TEXT("project")));
	ExpectDenied(TEXT("Absolute traversal"), InvokeReadSource(Mod / TEXT("../../../Outside/secret.h"), FString()));
	ExpectDenied(TEXT("Traversal to outside"), InvokeReadSource(TEXT("../../Outside/secret.h"), TEXT("project")));

	// Selected scope: project-only must not inherit engine permission, and vice versa.
	ExpectDenied(TEXT("Engine absolute under scope project"), InvokeReadSource(Engine / TEXT("Source/Runtime/E.h"), TEXT("project")));
	{
		const FString R = InvokeReadSource(Engine / TEXT("Source/Runtime/E.h"), TEXT("project"));
		TestFalse(TEXT("Engine absolute under scope project: no engine bytes"), R.Contains(AllowedMarker));
	}
	{
		const FString R = InvokeReadSource(Mod / TEXT("A.h"), TEXT("engine"));
		TestTrue(TEXT("Project absolute under scope engine: ACCESS_DENIED"), R.Contains(TEXT("ACCESS_DENIED")));
		TestFalse(TEXT("Project absolute under scope engine: no project bytes"), R.Contains(AllowedMarker));
	}
	{
		const FString R = InvokeReadSource(Mod / TEXT("A.h"), TEXT("everything"));
		TestTrue(TEXT("Unknown scope is rejected"), R.Contains(TEXT("INVALID_SCOPE")));
	}

	// Case behavior on this host's filesystem.
	const FString UpperPath = Project / TEXT("SOURCE/MOD/A.h");
	if (FPaths::FileExists(UpperPath))
	{
		// Case-insensitive volume (macOS default, Windows): the variant names the same
		// file, so it is allowed and reported at its on-disk spelling.
		const FString R = InvokeReadSource(UpperPath, TEXT("project"));
		ExpectAllowed(TEXT("Case variant on case-insensitive volume"), R);
		TestTrue(TEXT("Case variant reports the canonical on-disk path"), R.Contains(TEXT("Source/Mod/A.h")));
		ExpectDenied(TEXT("Case variant of the sibling"), InvokeReadSource(Project / TEXT("SOURCE-PRIVATE/private.h"), TEXT("project")));
	}
	else
	{
		AddInfo(TEXT("Case-sensitive volume: case variants name different (absent) files"));
		ExpectDenied(TEXT("Case variant on case-sensitive volume"), InvokeReadSource(UpperPath, TEXT("project")));
	}

	// Symlinks.
	if (MakeSymlink(Outside / TEXT("secret.h"), Mod / TEXT("LinkOut.h")))
	{
		ExpectDenied(TEXT("File symlink to outside (relative)"), InvokeReadSource(TEXT("Mod/LinkOut.h"), TEXT("project")));
		ExpectDenied(TEXT("File symlink to outside (absolute)"), InvokeReadSource(Mod / TEXT("LinkOut.h"), FString()));

		TestTrue(TEXT("Create directory symlink"), MakeSymlink(Outside, Mod / TEXT("DirLink")));
		ExpectDenied(TEXT("Directory symlink to outside"), InvokeReadSource(TEXT("Mod/DirLink/secret.h"), TEXT("project")));

		TestTrue(TEXT("Create inside symlink"), MakeSymlink(Mod / TEXT("Nested/B.h"), Mod / TEXT("LinkIn.h")));
		ExpectAllowed(TEXT("Symlink to a file inside the root"), InvokeReadSource(TEXT("Mod/LinkIn.h"), TEXT("project")));

		TestTrue(TEXT("Create symlink to non-source file"), MakeSymlink(Mod / TEXT("data.txt"), Mod / TEXT("Disguised.h")));
		{
			const FString R = InvokeReadSource(TEXT("Mod/Disguised.h"), TEXT("project"));
			TestFalse(TEXT("Symlinked .txt behind a .h name: no bytes"), R.Contains(ForbiddenMarker));
			TestTrue(TEXT("Symlinked .txt behind a .h name: ACCESS_DENIED"), R.Contains(TEXT("ACCESS_DENIED")));
		}

		// Swap between candidate selection and open: the decision is made on the handle that
		// is read, so a link repointed after the lexical step is judged by its new target.
		TestTrue(TEXT("Create swap link (inside)"), MakeSymlink(Mod / TEXT("A.h"), Mod / TEXT("Swap.h")));
		const FString Candidate = Mod / TEXT("Swap.h");
		TestTrue(TEXT("Swap link passes lexical selection"), FPaths::FileExists(Candidate));
		TestTrue(TEXT("Repoint swap link to outside"), MakeSymlink(Outside / TEXT("secret.h"), Candidate));
		{
			const TArray<FString> Permitted = KismetAISourceConfinement::GetPermittedRoots(Roots, EKismetAISourceScope::Project);
			TArray<uint8> Bytes;
			FString Final;
			const EKismetAISourceReadStatus Status = KismetAISourceConfinement::ReadConfined(Candidate, Permitted, Bytes, Final);
			TestTrue(TEXT("Swapped link is denied at open"), Status == EKismetAISourceReadStatus::OutsideRoots);
			TestEqual(TEXT("Swapped link: no bytes read"), Bytes.Num(), 0);
		}
	}
	else
	{
		AddInfo(TEXT("Symlink cases not run: symlink creation unsupported on this platform"));
	}

	// Component-aware containment unit checks.
	TestTrue(TEXT("Child is within root"), KismetAISourceConfinement::IsWithinRoot(TEXT("/p/Source/A.h"), TEXT("/p/Source")));
	TestFalse(TEXT("Adjacent prefix is not within root"), KismetAISourceConfinement::IsWithinRoot(TEXT("/p/Source-private/A.h"), TEXT("/p/Source")));
	TestFalse(TEXT("Root itself is not a file inside it"), KismetAISourceConfinement::IsWithinRoot(TEXT("/p/Source"), TEXT("/p/Source")));
	TestTrue(TEXT("Trailing slash on root"), KismetAISourceConfinement::IsWithinRoot(TEXT("/p/Source/A.h"), TEXT("/p/Source/")));

	IFileManager::Get().DeleteDirectory(*Base, false, true);
	return true;
}

// Line splitting must match FFileHelper::LoadFileToStringArray so outline/raw output is
// unchanged by reading through a handle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FReadSourceConfinement_LineSplit, "KismetAI.ReadSource.Confinement.LineSplit", ReadSourceConfinementTest::Flags)
bool FReadSourceConfinement_LineSplit::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()) / TEXT("KismetAIP106Lines.h");
	const char* Variants[] = {
		"a\nb\n", "a\r\nb", "\xEF\xBB\xBF" "first\r\n\r\nthird\r", "", "\n\n", "x\ry\n\rz",
	};
	for (const char* Variant : Variants)
	{
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Variant), FCStringAnsi::Strlen(Variant));
		FFileHelper::SaveArrayToFile(Bytes, *Path);
		TArray<FString> Expected;
		FFileHelper::LoadFileToStringArray(Expected, *Path);
		TArray<FString> Actual;
		KismetAISourceConfinement::BytesToLines(Bytes, Actual);
		TestEqual(FString::Printf(TEXT("Line count for variant of %d bytes"), Bytes.Num()), Actual.Num(), Expected.Num());
		for (int32 Index = 0; Index < FMath::Min(Actual.Num(), Expected.Num()); ++Index)
		{
			TestEqual(TEXT("Line text"), Actual[Index], Expected[Index]);
		}
	}
	// UTF-16LE with BOM.
	{
		const uint8 Utf16[] = { 0xFF, 0xFE, 'h', 0, 'i', 0, '\n', 0, 'x', 0 };
		TArray<uint8> Bytes(Utf16, UE_ARRAY_COUNT(Utf16));
		FFileHelper::SaveArrayToFile(Bytes, *Path);
		TArray<FString> Expected;
		FFileHelper::LoadFileToStringArray(Expected, *Path);
		TArray<FString> Actual;
		KismetAISourceConfinement::BytesToLines(Bytes, Actual);
		TestEqual(TEXT("UTF-16 line count"), Actual.Num(), Expected.Num());
		TestEqual(TEXT("UTF-16 matches loader"), FString::Join(Actual, TEXT("|")), FString::Join(Expected, TEXT("|")));
	}
	IFileManager::Get().Delete(*Path, false, true, true);
	return true;
}
