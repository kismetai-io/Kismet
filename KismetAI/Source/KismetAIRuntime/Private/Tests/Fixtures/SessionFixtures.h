// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Session JSONL fixtures.
//
// Synthetic rather than captured, deliberately: a real session embeds the whole
// 43 KB system prompt and the user's actual prompts, neither of which belongs in
// the repo. These reproduce the *shapes* the reducer has to survive, which is
// what the tests are about — every entry type the writer emits, plus the three
// quirks that are easy to get wrong:
//
//   - tool_result_message stores its blocks under "results", not "content"
//   - assistant_message content is the verbatim provider block array, including
//     thinking blocks WITH their signatures
//   - session_meta is re-appended at EOF on every terminal path, so the LAST one
//     wins and earlier ones must not confuse the reducer

#pragma once

#include "CoreMinimal.h"

namespace KismetAISessionFixtures
{
	// A completed run: prompt, tool call, result, answer.
	inline const TCHAR* Completed()
	{
		return TEXT(
			"{\"type\":\"session_start\",\"v\":1,\"uuid\":\"u0\",\"timestamp\":\"2026-09-01T10:00:00Z\",\"model\":\"claude-sonnet-4-6\",\"blueprint\":\"/Game/BP_Player\",\"max_steps\":128,\"system_prompt\":\"(elided)\",\"tools\":[\"read_blueprint\"]}\n"
			"{\"type\":\"user_message\",\"uuid\":\"u1\",\"timestamp\":\"2026-09-01T10:00:01Z\",\"content\":\"add a print node\",\"blueprint\":\"/Game/BP_Player\"}\n"
			"{\"type\":\"assistant_message\",\"uuid\":\"u2\",\"timestamp\":\"2026-09-01T10:00:05Z\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"plan it\",\"signature\":\"SIG123\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"insert_node\",\"input\":{\"blueprint\":\"/Game/BP_Player\",\"node\":\"Print String\"}}]}\n"
			"{\"type\":\"tool_result_message\",\"uuid\":\"u3\",\"timestamp\":\"2026-09-01T10:00:06Z\",\"results\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"{\\\"success\\\":true}\"}]}\n"
			"{\"type\":\"token_stats\",\"timestamp\":\"2026-09-01T10:00:07Z\",\"input\":1200,\"output\":80,\"cost_usd\":0.01,\"turn\":1}\n"
			"{\"type\":\"assistant_message\",\"uuid\":\"u4\",\"timestamp\":\"2026-09-01T10:00:08Z\",\"content\":[{\"type\":\"text\",\"text\":\"Added it.\"}]}\n"
			"{\"type\":\"session_meta\",\"session_id\":\"s1\",\"model\":\"claude-sonnet-4-6\",\"status\":\"completed\",\"stop_reason\":\"end_turn\",\"turn_count\":2}\n");
	}

	// Aborted mid-tool: the tool_use has no matching result. This is the shape
	// that must not break resume.
	inline const TCHAR* AbortedMidTool()
	{
		return TEXT(
			"{\"type\":\"session_start\",\"v\":1,\"uuid\":\"a0\",\"timestamp\":\"2026-09-01T11:00:00Z\",\"model\":\"claude-sonnet-4-6\"}\n"
			"{\"type\":\"user_message\",\"uuid\":\"a1\",\"timestamp\":\"2026-09-01T11:00:01Z\",\"content\":\"do a big edit\"}\n"
			"{\"type\":\"assistant_message\",\"uuid\":\"a2\",\"timestamp\":\"2026-09-01T11:00:04Z\",\"content\":[{\"type\":\"tool_use\",\"id\":\"t9\",\"name\":\"edit_blueprint\",\"input\":{\"blueprint\":\"/Game/BP\"}}]}\n"
			"{\"type\":\"session_meta\",\"session_id\":\"s2\",\"status\":\"aborted\",\"stop_reason\":\"aborted\",\"turn_count\":1}\n");
	}

	// A failed run records the API error, then terminal metadata.
	inline const TCHAR* Failed()
	{
		return TEXT(
			"{\"type\":\"session_start\",\"v\":1,\"uuid\":\"f0\",\"timestamp\":\"2026-09-01T12:00:00Z\",\"model\":\"claude-sonnet-4-6\"}\n"
			"{\"type\":\"user_message\",\"uuid\":\"f1\",\"timestamp\":\"2026-09-01T12:00:01Z\",\"content\":\"hello\"}\n"
			"{\"type\":\"error\",\"uuid\":\"f2\",\"timestamp\":\"2026-09-01T12:00:02Z\",\"error_code\":\"authentication_error\",\"message\":\"invalid x-api-key\",\"http_status\":401,\"suggestions\":[\"Check the API key\"]}\n"
			"{\"type\":\"session_meta\",\"session_id\":\"s3\",\"status\":\"failed\",\"stop_reason\":\"authentication_error\"}\n");
	}

	// Killed mid-write: the final line is a truncated fragment, which is what a
	// crashed editor leaves behind.
	inline const TCHAR* TruncatedTail()
	{
		return TEXT(
			"{\"type\":\"session_start\",\"v\":1,\"uuid\":\"x0\",\"timestamp\":\"2026-09-01T13:00:00Z\",\"model\":\"claude-sonnet-4-6\"}\n"
			"{\"type\":\"user_message\",\"uuid\":\"x1\",\"timestamp\":\"2026-09-01T13:00:01Z\",\"content\":\"go\"}\n"
			"{\"type\":\"assistant_message\",\"uuid\":\"x2\",\"timestamp\":\"2026-09-01T13:00:0");
	}

	// Written by a build that knows entry types this one does not.
	inline const TCHAR* WithUnknownEntries()
	{
		return TEXT(
			"{\"type\":\"session_start\",\"v\":2,\"uuid\":\"n0\",\"timestamp\":\"2026-09-01T14:00:00Z\",\"model\":\"claude-opus-9\"}\n"
			"{\"type\":\"future_entry\",\"payload\":{\"whatever\":1}}\n"
			"{\"type\":\"user_message\",\"uuid\":\"n1\",\"timestamp\":\"2026-09-01T14:00:01Z\",\"content\":\"hi\"}\n"
			"{\"type\":\"assistant_message\",\"uuid\":\"n2\",\"timestamp\":\"2026-09-01T14:00:02Z\",\"content\":[{\"type\":\"text\",\"text\":\"hello\"}]}\n");
	}
}
