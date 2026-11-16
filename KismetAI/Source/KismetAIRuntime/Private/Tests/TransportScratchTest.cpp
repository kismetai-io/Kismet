// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModelClient.h"
#include "KismetAITransportScratch.h"
#include "Fixtures/SSEFixtures.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <unistd.h>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_PrivateOwnedScratch,
	"KismetAI.Transport.PrivateOwnedScratch", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTransport_PrivateOwnedScratch::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FPlatformProcess::UserTempDir(), TEXT("KismetScratchTest-"),
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString ScratchRoot = Root / TEXT("scratch");
	const FString Endpoint = Root / TEXT("endpoint");
	const FString OldRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TRANSPORT_SCRATCH_ROOT"));
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMETAI_TRANSPORT_SCRATCH_ROOT"), *ScratchRoot);
	ON_SCOPE_EXIT
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMETAI_TRANSPORT_SCRATCH_ROOT"), *OldRoot);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	};
	IFileManager::Get().MakeDirectory(*(Endpoint / TEXT("v1")), true);
	FFileHelper::SaveStringToFile(UTF8_TO_TCHAR(KismetAISSEFixtures::TextOnly()),
		*(Endpoint / TEXT("v1/messages")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	uint32 DeadPid = 0;
#if PLATFORM_WINDOWS
	FProcHandle Dead = FPlatformProcess::CreateProc(TEXT("cmd.exe"), TEXT("/c exit 0"), false, true, true, &DeadPid, 0, nullptr, nullptr);
#else
	FProcHandle Dead = FPlatformProcess::CreateProc(TEXT("/usr/bin/true"), TEXT(""), false, true, true, &DeadPid, 0, nullptr, nullptr);
#endif
	if (!TestTrue(TEXT("disposable owner process launched"), Dead.IsValid())) return false;
	FPlatformProcess::WaitForProc(Dead);
	FPlatformProcess::CloseProc(Dead);
	auto Seed = [&](uint32 OwnerPid, const FString& Phase)
	{
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		const FString Dir = ScratchRoot / Id;
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString Marker = FString::Printf(TEXT("{\"schema\":1,\"request_id\":\"%s\",\"owner_pid\":%u,\"phase\":\"%s\",\"curl_pid\":0}"), *Id, OwnerPid, *Phase);
		FFileHelper::SaveStringToFile(Marker, *(Dir / TEXT("owner.json")));
		FFileHelper::SaveStringToFile(TEXT("fake-secret"), *(Dir / TEXT("curl.cfg")));
		return Dir;
	};
	const FString Abandoned = Seed(DeadPid, TEXT("prepared"));
	const FString Active = Seed(FPlatformProcess::GetCurrentProcessId(), TEXT("prepared"));
	const FString Uncertain = Seed(DeadPid, TEXT("launching"));
	const FString Unknown = ScratchRoot / TEXT("unowned-legacy");
	IFileManager::Get().MakeDirectory(*Unknown, true);

#if PLATFORM_MAC || PLATFORM_LINUX
	const FString Outside = Root / TEXT("outside-secret");
	FFileHelper::SaveStringToFile(TEXT("leave-me-intact"), *Outside);
	const FString LinkedFileDir = Seed(DeadPid, TEXT("prepared"));
	IFileManager::Get().Delete(*(LinkedFileDir / TEXT("curl.cfg")));
	TestEqual(TEXT("file substitution fixture created"), symlink(TCHAR_TO_UTF8(*Outside), TCHAR_TO_UTF8(*(LinkedFileDir / TEXT("curl.cfg")))), 0);
	const FString LinkedDir = ScratchRoot / FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	TestEqual(TEXT("directory substitution fixture created"), symlink(TCHAR_TO_UTF8(*Active), TCHAR_TO_UTF8(*LinkedDir)), 0);
#endif

	FKismetAIAgentConfig Config;
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.ProviderId = TEXT("anthropic");
	Config.ApiKey = TEXT("scratch-test-fake-key");
	Config.ProviderBaseUrls.Add(TEXT("anthropic"), TEXT("file://") + Endpoint);
	Config.bProviderEnvironmentResolved = true;
	FKismetAICurlModelClient Client(Config);
	bool bObserved = false;
	FModelCallbacks Callbacks;
	Callbacks.OnTextDelta = [&](const FString&)
	{
		if (bObserved) return;
		bObserved = true;
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(ScratchRoot / TEXT("*")), false, true);
		FString RequestDir;
		for (const FString& Dir : Dirs)
		{
			if (IFileManager::Get().FileExists(*(ScratchRoot / Dir / TEXT("body.json")))) RequestDir = ScratchRoot / Dir;
		}
		TestFalse(TEXT("real active call uses owned disposable scratch"), RequestDir.IsEmpty());
		if (RequestDir.IsEmpty()) return;
		TestTrue(TEXT("owner marker accompanies active request"), IFileManager::Get().FileExists(*(RequestDir / TEXT("owner.json"))));
#if PLATFORM_MAC || PLATFORM_LINUX
		struct stat Info;
		TestTrue(TEXT("request directory stat"), stat(TCHAR_TO_UTF8(*RequestDir), &Info) == 0);
		TestEqual(TEXT("request directory private before payload write"), static_cast<int32>(Info.st_mode & 0777), 0700);
		for (const FString& Name : {FString(TEXT("body.json")), FString(TEXT("response.sse")), FString(TEXT("curl.cfg")), FString(TEXT("owner.json"))})
		{
			if (TestTrue(*Name, stat(TCHAR_TO_UTF8(*(RequestDir / Name)), &Info) == 0))
				TestEqual(TEXT("scratch file permissions"), static_cast<int32>(Info.st_mode & 0777), 0600);
		}
#endif
	};
	AddExpectedError(TEXT("LLM API returned status 0"), EAutomationExpectedErrorFlags::Contains, 0);
	Client.Call(FModelCallRequest({}, {}, TEXT("private-prompt"), Config), Callbacks);
	TestTrue(TEXT("real curl delivered fixture bytes"), bObserved);
#if PLATFORM_MAC || PLATFORM_LINUX
	TestFalse(TEXT("proven abandoned request is removed"), IFileManager::Get().DirectoryExists(*Abandoned));
	TestTrue(TEXT("file substitution preserves directory"), IFileManager::Get().DirectoryExists(*LinkedFileDir));
	struct stat LinkInfo;
	TestTrue(TEXT("directory symlink is untouched"), lstat(TCHAR_TO_UTF8(*LinkedDir), &LinkInfo) == 0 && S_ISLNK(LinkInfo.st_mode));
	TestTrue(TEXT("file symlink is untouched"), lstat(TCHAR_TO_UTF8(*(LinkedFileDir / TEXT("curl.cfg"))), &LinkInfo) == 0 && S_ISLNK(LinkInfo.st_mode));
	FString OutsideContent;
	FFileHelper::LoadFileToString(OutsideContent, *Outside);
	TestEqual(TEXT("symlink target remains intact"), OutsideContent, FString(TEXT("leave-me-intact")));
	unlink(TCHAR_TO_UTF8(*LinkedDir));
	unlink(TCHAR_TO_UTF8(*(LinkedFileDir / TEXT("curl.cfg"))));
#else
	TestTrue(TEXT("unsupported liveness proof preserves abandoned request"), IFileManager::Get().DirectoryExists(*Abandoned));
#endif
	TestTrue(TEXT("live owner protected"), IFileManager::Get().DirectoryExists(*Active));
	TestTrue(TEXT("unrecorded child launch preserved"), IFileManager::Get().DirectoryExists(*Uncertain));
	TestTrue(TEXT("unknown legacy directory preserved"), IFileManager::Get().DirectoryExists(*Unknown));
	TArray<FString> ResidualBodies;
	IFileManager::Get().FindFilesRecursive(ResidualBodies, *ScratchRoot, TEXT("body.json"), true, false);
	TestEqual(TEXT("normal call cleans prompt"), ResidualBodies.Num(), 0);
	return true;
}

// Explicit external crash harness only; not selected by the KismetAI product suite.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_ScratchCrashProbe,
	"LocalDiagnostics.ScratchCrashProbe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTransport_ScratchCrashProbe::RunTest(const FString& Parameters)
{
	const FString Base = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_SCRATCH_PROBE_BASE_URL"));
	const FString Root = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TRANSPORT_SCRATCH_ROOT"));
	FString PortAndPath = Base.Mid(17);
	FString Port, Path;
	if (!PortAndPath.Split(TEXT("/"), &Port, &Path)) Port = PortAndPath;
	const bool bSafeUrl = Base.StartsWith(TEXT("http://127.0.0.1:"))
		&& !Port.IsEmpty() && Port.IsNumeric() && FCString::Atoi(*Port) > 0 && FCString::Atoi(*Port) <= 65535
		&& !Base.Contains(TEXT("@")) && !Base.Contains(TEXT("\\")) && !Base.Contains(TEXT("\""))
		&& !Base.Contains(TEXT(" ")) && !Base.Contains(TEXT("\n")) && !Base.Contains(TEXT("\r"));
	if (!bSafeUrl || Root.IsEmpty())
	{
		AddError(TEXT("Scratch crash probe requires an explicit localhost fixture and disposable scratch root."));
		return false;
	}
	FKismetAIAgentConfig Config;
	Config.Model = TEXT("claude-sonnet-4-6");
	Config.ProviderId = TEXT("anthropic");
	Config.ApiKey = TEXT("scratch-crash-fake-key");
	Config.ProviderBaseUrls.Add(TEXT("anthropic"), Base);
	Config.bProviderEnvironmentResolved = true;
	FKismetAICurlModelClient Client(Config);
	const FModelCallOutcome Outcome = Client.Call(FModelCallRequest({}, {}, TEXT("scratch-crash-fake-prompt"), Config), {});
	TestEqual(TEXT("controlled localhost request completed"), Outcome.Result, EModelCallResult::Success);
	return true;
}
#if PLATFORM_MAC || PLATFORM_LINUX
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTransport_HostileScratchParentRefused,
	"KismetAI.Transport.HostileScratchParentRefused", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FTransport_HostileScratchParentRefused::RunTest(const FString& Parameters)
{
	const FString Base = FPaths::Combine(FPlatformProcess::UserTempDir(),
		TEXT("KismetScratchParent-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString Parent = Base / TEXT("hostile");
	const FString Request = Parent / TEXT("llm") / FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	IFileManager::Get().MakeDirectory(*Parent, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Base, false, true); };
	TestEqual(TEXT("shared-writable ancestor fixture"), chmod(TCHAR_TO_UTF8(*Parent), 0777), 0);
	FKismetAITransportScratch Scratch;
	TestFalse(TEXT("refuse root under substitutable ancestor"), Scratch.Begin(Request));
	TestFalse(TEXT("no request directory before privacy established"), IFileManager::Get().DirectoryExists(*Request));
	return true;
}
#endif
