// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAuth.h"
#include "KismetAISettings.h"
#include "ScopedTempHome.h"

#include "Misc/AutomationTest.h"

constexpr auto KismetAIAuthTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_MachineIdStable, "KismetAI.Auth.MachineIdStable", KismetAIAuthTestFlags)
bool FKismetAIAuth_MachineIdStable::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("AuthTest"));

	const FString Id1 = KismetAIAuth::GetMachineId();
	TestTrue(TEXT("MachineId non-empty"), !Id1.IsEmpty());

	const FString Id2 = KismetAIAuth::GetMachineId();
	TestEqual(TEXT("MachineId stable across calls"), Id1, Id2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_SessionRoundTrip, "KismetAI.Auth.SessionRoundTrip", KismetAIAuthTestFlags)
bool FKismetAIAuth_SessionRoundTrip::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("AuthTest"));

	TestFalse(TEXT("HasSession initially false"), KismetAIAuth::HasSession());

	FKismetAIAuthSession In;
	In.Token = TEXT("kismet-test-token-abcdef");
	In.MachineId = TEXT("11111111-2222-3333-4444-555555555555");
	In.UserId = TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	In.Email = TEXT("dev@example.com");
	In.DisplayName = TEXT("Dev");

	TestTrue(TEXT("Save"), KismetAIAuth::SaveSession(In));
	TestTrue(TEXT("Has after save"), KismetAIAuth::HasSession());

	FKismetAIAuthSession Out;
	TestTrue(TEXT("Load"), KismetAIAuth::LoadSession(Out));
	TestEqual(TEXT("Token"), Out.Token, In.Token);
	TestEqual(TEXT("MachineId"), Out.MachineId, In.MachineId);
	TestEqual(TEXT("UserId"), Out.UserId, In.UserId);
	TestEqual(TEXT("Email"), Out.Email, In.Email);
	TestEqual(TEXT("DisplayName"), Out.DisplayName, In.DisplayName);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_SessionRejectsEmptyToken, "KismetAI.Auth.SessionRejectsEmptyToken", KismetAIAuthTestFlags)
bool FKismetAIAuth_SessionRejectsEmptyToken::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("AuthTest"));

	FKismetAIAuthSession Empty;
	TestFalse(TEXT("Save rejects empty session"), KismetAIAuth::SaveSession(Empty));
	TestFalse(TEXT("Has remains false"), KismetAIAuth::HasSession());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_Clear, "KismetAI.Auth.Clear", KismetAIAuthTestFlags)
bool FKismetAIAuth_Clear::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("AuthTest"));

	FKismetAIAuthSession In;
	In.Token = TEXT("t");
	In.MachineId = TEXT("m");
	TestTrue(TEXT("Save"), KismetAIAuth::SaveSession(In));
	TestTrue(TEXT("Has after save"), KismetAIAuth::HasSession());
	TestTrue(TEXT("Clear succeeds"), KismetAIAuth::ClearSession());
	TestFalse(TEXT("Has after clear"), KismetAIAuth::HasSession());
	TestTrue(TEXT("Clear is idempotent"), KismetAIAuth::ClearSession());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_LoadMissingReturnsFalse, "KismetAI.Auth.LoadMissingReturnsFalse", KismetAIAuthTestFlags)
bool FKismetAIAuth_LoadMissingReturnsFalse::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("AuthTest"));

	FKismetAIAuthSession Out;
	TestFalse(TEXT("Load returns false when missing"), KismetAIAuth::LoadSession(Out));
	TestFalse(TEXT("Out is invalid"), Out.IsValid());
	return true;
}

// Sessions are scoped per backend host: a token issued on one backend must not
// be visible to — or cleared by — another. This is what stops a wrong-backend
// 401 (e.g. a dev token hitting production after an update) from wiping the
// session for the env the user actually logged into.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_SessionScopedPerBackend, "KismetAI.Auth.SessionScopedPerBackend", KismetAIAuthTestFlags)
bool FKismetAIAuth_SessionScopedPerBackend::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("AuthTest"));
	const FString OldEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_BACKEND_URL"));

	auto SetBackend = [](const TCHAR* Url) { FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), Url); };

	// Log in against production.
	SetBackend(TEXT("https://kismetai.io"));
	FKismetAIAuthSession Prod;
	Prod.Token = TEXT("prod-token");
	Prod.MachineId = TEXT("m");
	TestTrue(TEXT("Save prod session"), KismetAIAuth::SaveSession(Prod));
	TestTrue(TEXT("Prod has session"), KismetAIAuth::HasSession());

	// Switch to dev: production's session must not be visible here.
	SetBackend(TEXT("https://dev.kismetai.io"));
	TestFalse(TEXT("Dev sees no session yet"), KismetAIAuth::HasSession());

	FKismetAIAuthSession Dev;
	Dev.Token = TEXT("dev-token");
	Dev.MachineId = TEXT("m");
	TestTrue(TEXT("Save dev session"), KismetAIAuth::SaveSession(Dev));

	// Both sessions now coexist — switching back to prod still finds the prod token.
	SetBackend(TEXT("https://kismetai.io"));
	FKismetAIAuthSession OutProd;
	TestTrue(TEXT("Prod session survives dev login"), KismetAIAuth::LoadSession(OutProd));
	TestEqual(TEXT("Prod token intact"), OutProd.Token, FString(TEXT("prod-token")));

	// A clear on dev (the wrong-backend-401 path) must not touch the prod slot.
	SetBackend(TEXT("https://dev.kismetai.io"));
	TestTrue(TEXT("Clear dev session"), KismetAIAuth::ClearSession());
	TestFalse(TEXT("Dev session gone"), KismetAIAuth::HasSession());

	SetBackend(TEXT("https://kismetai.io"));
	TestTrue(TEXT("Prod session untouched by dev clear"), KismetAIAuth::HasSession());

	// Restore env.
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), OldEnv.IsEmpty() ? TEXT("") : *OldEnv);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_BackendUrlEnvOverride, "KismetAI.Auth.BackendUrlEnvOverride", KismetAIAuthTestFlags)
bool FKismetAIAuth_BackendUrlEnvOverride::RunTest(const FString& Parameters)
{
	const FString Old = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_BACKEND_URL"));

	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), TEXT("http://localhost:3000"));
	const FString Override = KismetAIAuth::GetBackendUrl();
	TestEqual(TEXT("Env override applied"), Override, FString(TEXT("http://localhost:3000")));

	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), TEXT(""));
	const FString Default = KismetAIAuth::GetBackendUrl();
	TestTrue(TEXT("Default is non-empty"), !Default.IsEmpty());
	TestTrue(TEXT("Default starts with https"), Default.StartsWith(TEXT("https://")));

	// Restore
	if (Old.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), TEXT(""));
	}
	else
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), *Old);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIAuth_BackendUrlSettingsResolution, "KismetAI.Auth.BackendUrlSettingsResolution", KismetAIAuthTestFlags)
bool FKismetAIAuth_BackendUrlSettingsResolution::RunTest(const FString& Parameters)
{
	const FString OldEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("KISMET_BACKEND_URL"));
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), TEXT(""));

	UKismetAISettings* Settings = GetMutableDefault<UKismetAISettings>();
	const EKismetAIBackend OldBackend = Settings->Backend;
	const FString OldCustomUrl = Settings->CustomUrl;

	// Env var still wins over settings.
	Settings->Backend = EKismetAIBackend::Development;
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), TEXT("http://localhost:9999"));
	TestEqual(TEXT("Env var overrides Development"), KismetAIAuth::GetBackendUrl(), FString(TEXT("http://localhost:9999")));
	FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), TEXT(""));

	// Production
	Settings->Backend = EKismetAIBackend::Production;
	TestEqual(TEXT("Production resolves to kismetai.io"), KismetAIAuth::GetBackendUrl(), FString(TEXT("https://kismetai.io")));

	// Development
	Settings->Backend = EKismetAIBackend::Development;
	TestEqual(TEXT("Development resolves to dev.kismetai.io"), KismetAIAuth::GetBackendUrl(), FString(TEXT("https://dev.kismetai.io")));

	// Custom + non-empty CustomUrl
	Settings->Backend = EKismetAIBackend::Custom;
	Settings->CustomUrl = TEXT("https://staging.example.com");
	TestEqual(TEXT("Custom uses CustomUrl"), KismetAIAuth::GetBackendUrl(), FString(TEXT("https://staging.example.com")));

	// Custom + empty CustomUrl falls back to default.
	Settings->CustomUrl = TEXT("");
	TestEqual(TEXT("Custom + empty falls back to default"), KismetAIAuth::GetBackendUrl(), FString(TEXT("https://kismetai.io")));

	// Restore CDO + env.
	Settings->Backend = OldBackend;
	Settings->CustomUrl = OldCustomUrl;
	if (OldEnv.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), TEXT(""));
	}
	else
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("KISMET_BACKEND_URL"), *OldEnv);
	}
	return true;
}
