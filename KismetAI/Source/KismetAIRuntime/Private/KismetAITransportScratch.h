// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "CoreMinimal.h"

// Request-owned transport files, separate from diagnostics and recovery journals.
class FKismetAITransportScratch
{
public:
	FKismetAITransportScratch() = default;
	FKismetAITransportScratch(const FKismetAITransportScratch&) = delete;
	FKismetAITransportScratch& operator=(const FKismetAITransportScratch&) = delete;
	~FKismetAITransportScratch();
	bool Begin(const FString& RequestDirectory);
	bool WriteText(const FString& Name, const FString& Text);
	bool MarkLaunching();
	bool MarkRunning(uint32 ProcessId);
	static int32 Sweep(const FString& Root);
private:
	bool WriteOwner(const TCHAR* Phase, uint32 ProcessId);
	FString Directory;
#if PLATFORM_MAC || PLATFORM_LINUX
	int RootDescriptor = -1;
	int DirectoryDescriptor = -1;
#endif
};
