// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIModule.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"

namespace
{
	class FWaitForHttpStartup final : public IAutomationLatentCommand
	{
	public:
		explicit FWaitForHttpStartup(FAutomationTestBase* InTest)
			: Test(InTest), Deadline(FPlatformTime::Seconds() + 10.0) {}

		virtual bool Update() override
		{
			if (FKismetAIModule::Get().IsHttpServerRunning())
			{
				Test->AddInfo(TEXT("HTTP listener recovered after the occupied port was released"));
				return true;
			}
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Test->AddError(TEXT("HTTP listener did not retry after the occupied port was released"));
				return true;
			}
			return false;
		}
	private:
		FAutomationTestBase* Test;
		double Deadline;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIHttpStartupRetryTest,
	"KismetAI.HttpStartup.OccupiedPortRecovers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FKismetAIHttpStartupRetryTest::RunTest(const FString& Parameters)
{
	// The opt-in harness owns a loopback socket on KISMETAI_PORT before launching
	// the editor, then closes it when this private synchronization file appears.
	const FString ReleaseFile = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMETAI_TEST_OCCUPIED_PORT_RELEASE_FILE"));
	if (ReleaseFile.IsEmpty())
	{
		AddInfo(TEXT("Occupied-port startup probe not selected (no local fixture)"));
		return true;
	}
	FKismetAIModule& Module = FKismetAIModule::Get();
	TestFalse(TEXT("a failed socket bind must not report the HTTP server running"), Module.IsHttpServerRunning());
	TestTrue(TEXT("a failed startup schedules recovery"), Module.HttpStartupRetryHandle.IsValid());
	Module.StopHttpServer();
	TestFalse(TEXT("stopping an unbound server cancels its startup retry"), Module.HttpStartupRetryHandle.IsValid());
	TestEqual(TEXT("stopping clears the retry deadline"), Module.HttpStartupRetryDeadline, 0.0);
	AddExpectedError(FString::Printf(TEXT("HttpListener unable to bind to 127.0.0.1:%d"), Module.ServerPort), EAutomationExpectedErrorFlags::Contains, 1);
	Module.StartHttpServer();
	TestFalse(TEXT("retry remains honest while the port is occupied"), Module.IsHttpServerRunning());
	TestTrue(TEXT("a fresh start schedules a new bounded retry"), Module.HttpStartupRetryHandle.IsValid());
	if (!TestTrue(TEXT("signal the owned socket fixture to release the port"), FFileHelper::SaveStringToFile(TEXT("release"), *ReleaseFile))) return false;
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForHttpStartup(this));
	return true;
}
