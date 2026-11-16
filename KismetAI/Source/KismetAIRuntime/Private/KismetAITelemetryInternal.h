// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// Internals of KismetAITelemetry that need to be reachable from automation
// tests without becoming part of the public plugin API. Tests in
// Private/Tests/ can #include this directly; external modules cannot, because
// the header lives in Private/.
//
// Anything declared here MUST also be defined in KismetAITelemetry.cpp.

namespace KismetAITelemetryInternal
{
	enum class ECategory : uint8 { Always, Runs, ToolCalls, Errors, Crashes };

	// Map an event.type string to the telemetry category that gates it.
	// Mirrors categoryFor() in app/api/v1/telemetry/events/route.ts —
	// keep in sync.
	ECategory CategoryForEventType(const FString& Type);

	// Parametric sanitizer for diagnostic strings. Replaces:
	//   - ProjectDir prefix              -> <ProjectDir>/
	//   - HomeDir prefix                 -> <Home>/
	//   - (../)+(Users|home)/<name>      -> <Home>
	//   - /Users/<name>, /home/<name>    -> <Home>
	//   - [A-Z]:\Users\<name>            -> <Home>
	// Tests pin ProjectDir and HomeDir to known values for determinism. The
	// in-process wrapper SanitizeForTelemetry() supplies the real engine roots.
	FString SanitizeForTelemetryWithRoots(
		FString Msg,
		const FString& ProjectDir,
		const FString& HomeDir);
}
