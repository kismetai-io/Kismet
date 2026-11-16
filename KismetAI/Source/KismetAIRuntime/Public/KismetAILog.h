// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Logging/LogMacros.h"

// Logging policy for KismetAI. Three macros, three audiences:
//
//   KISMETAI_LOG       — always compiled. Use for things end users / studios
//                        running the plugin actually need: missing API key,
//                        HTTP server bind failures, system prompt load
//                        failures, updater progress. Tier B in the audit.
//
//   KISMETAI_DEV_LOG   — compiled only when KISMETAI_DEV_LOGGING=1 (default
//                        in Development/DebugGame, off in Shipping/Test).
//                        Use for architecture-revealing detail: tool dispatch,
//                        compaction strategy, API request shape, retry/backoff
//                        traces, settings load detail. Tier A in the audit.
//                        Format string literals are physically absent from
//                        the binary in non-dev builds — `strings <dylib>`
//                        cannot recover them.
//
//   KISMETAI_TRACE     — same compile gate as KISMETAI_DEV_LOG, but emits at
//                        Verbose level. Use for hot-path content that would
//                        flood Output Log even in dev: assistant text,
//                        tool_use names, tool_result snippets, per-turn
//                        markers. Tier S in the audit. Opt in with
//                        `Log LogKismetAIAgent Verbose` at runtime.
//
// The #if-gating (not runtime branching) is load-bearing: if you write
// `if (false) UE_LOG(...)` the format literal stays in the .dylib's string
// table and `strings` finds it. Always gate with #if/#else/#endif.

#ifndef KISMETAI_DEV_LOGGING
#define KISMETAI_DEV_LOGGING 1
#endif

#define KISMETAI_LOG(Cat, Verb, Fmt, ...) UE_LOG(Cat, Verb, Fmt, ##__VA_ARGS__)

#if KISMETAI_DEV_LOGGING
	#define KISMETAI_DEV_LOG(Cat, Verb, Fmt, ...) UE_LOG(Cat, Verb, Fmt, ##__VA_ARGS__)
	#define KISMETAI_TRACE(Cat, Fmt, ...) UE_LOG(Cat, Verbose, Fmt, ##__VA_ARGS__)
#else
	#define KISMETAI_DEV_LOG(Cat, Verb, Fmt, ...) do {} while(0)
	#define KISMETAI_TRACE(Cat, Fmt, ...) do {} while(0)
#endif
