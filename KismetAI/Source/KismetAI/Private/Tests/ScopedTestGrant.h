// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// A grant on the module's authority store for the life of one test.
// Tests that drive a transport (panel, MCP, REST, HTTP agent run) through the
// real policy must hold explicit authority exactly like a real caller does.

#pragma once

#include "CoreMinimal.h"
#include "KismetAIModule.h"
#include "KismetAIAuthority.h"

struct FKismetAIScopedTestGrant
{
	FKismetAIScopedTestGrant(std::initializer_list<EKismetAIEntryPoint> EntryPoints, EKismetAIAuthorityOp Operations,
		std::initializer_list<const TCHAR*> Scopes, const FString& SessionId = FString())
	{
		FKismetAIAuthorityGrant G;
		G.Label = TEXT("automation-test");
		G.CreatedBy = TEXT("automation-test");
		G.EntryPoints = EntryPoints;
		G.Operations = Operations;
		for (const TCHAR* Scope : Scopes)
		{
			G.PathScopes.Add(Scope);
		}
		if (!SessionId.IsEmpty())
		{
			G.SessionId = SessionId;
			G.Lifetime = EKismetAIGrantLifetime::Session;
		}
		FString Error;
		Id = FKismetAIModule::Get().GetAuthorityStore().AddGrant(G, Error);
		ensureMsgf(!Id.IsEmpty(), TEXT("test grant refused: %s"), *Error);
	}

	~FKismetAIScopedTestGrant()
	{
		Revoke();
	}

	void Revoke()
	{
		if (!Id.IsEmpty())
		{
			FKismetAIModule::Get().GetAuthorityStore().Revoke(Id, TEXT("automation-test"));
			Id.Empty();
		}
	}

	FString Id;
};
