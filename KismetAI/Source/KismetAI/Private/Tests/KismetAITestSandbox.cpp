// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Every KismetAI automation test writes its session journals and run
// checkpoints into a private temporary folder, never into the project's
// Saved/KismetAI.
//
// Before this, each test that started an agent run left a journal in
// Saved/KismetAI/sessions (over a thousand after a few gate runs), and every
// run that touched an asset left a pending checkpoint manifest in
// Saved/KismetAI/checkpoints that startup recovery then offered back as a
// pending Reject. The folder is chosen when a test starts and deleted when it
// ends, so a test that forgets its own cleanup cannot leak either.
//
// Only tests whose path starts with "KismetAI" (this plugin and KismetAIEval)
// are sandboxed. Other plugins' tests, and the editor outside a test, keep the
// default folders.

#include "KismetAIModule.h"
#include "KismetAISessionStorage.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAITestSandbox, Log, All);

namespace KismetAITestSandbox
{
	class FSandbox
	{
	public:
		FSandbox()
		{
			FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
			StartHandle = Framework.OnTestStartEvent.AddRaw(this, &FSandbox::OnTestStart);
			EndHandle = Framework.OnTestEndEvent.AddRaw(this, &FSandbox::OnTestEnd);
		}

		~FSandbox()
		{
			// The framework singleton was constructed before this object (the
			// constructor called Get()), so it is still alive here.
			FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
			Framework.OnTestStartEvent.Remove(StartHandle);
			Framework.OnTestEndEvent.Remove(EndHandle);
		}

	private:
		static bool IsKismetTest(const FAutomationTestBase* Test)
		{
			return Test && Test->GetTestFullName().StartsWith(TEXT("KismetAI"));
		}

		static FKismetAIModule* LoadedModule()
		{
			return FModuleManager::Get().IsModuleLoaded(TEXT("KismetAI"))
				? &FModuleManager::GetModuleChecked<FKismetAIModule>(TEXT("KismetAI"))
				: nullptr;
		}

		void OnTestStart(FAutomationTestBase* Test)
		{
			if (!IsKismetTest(Test) || !IsInGameThread())
			{
				return;
			}
			Restore();
			Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(),
				TEXT("KismetAITests"), FString::Printf(TEXT("%u-%s"), FPlatformProcess::GetCurrentProcessId(),
					*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12))));
			IFileManager::Get().MakeDirectory(*(Root / TEXT("sessions")), true);
			FKismetAISessionStorage::SetSessionsDirectoryOverrideForTests(Root / TEXT("sessions"));
			if (FKismetAIModule* Module = LoadedModule())
			{
				Module->SetCheckpointRootForTests(Root / TEXT("checkpoints"));
			}
		}

		void OnTestEnd(FAutomationTestBase* Test)
		{
			if (IsKismetTest(Test) && IsInGameThread())
			{
				Restore();
			}
		}

		void Restore()
		{
			if (Root.IsEmpty())
			{
				return;
			}
			FKismetAISessionStorage::SetSessionsDirectoryOverrideForTests(FString());
			if (FKismetAIModule* Module = LoadedModule())
			{
				Module->SetCheckpointRootForTests(FString());
			}
			if (!IFileManager::Get().DeleteDirectory(*Root, false, true))
			{
				UE_LOG(LogKismetAITestSandbox, Warning, TEXT("Could not delete the test sandbox %s"), *Root);
			}
			Root.Empty();
		}

		FDelegateHandle StartHandle;
		FDelegateHandle EndHandle;
		FString Root;
	};

	static FSandbox Sandbox;
}
