// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// SSE transcript corpus.
//
// Synthetic but wire-accurate: these reproduce the event sequences and field
// shapes Anthropic actually sends, without embedding a real prompt or response.
// They exist so the streaming path can be exercised over the cases that break
// it — thinking blocks with signatures, tool_use assembled from partial JSON
// deltas, a non-SSE error body, and a stream cut mid-event.

#pragma once

#include "CoreMinimal.h"

namespace KismetAISSEFixtures
{
	// Plain text answer, no tools.
	inline const ANSICHAR* TextOnly()
	{
		return
			"event: message_start\n"
			"data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":12,\"output_tokens\":1}}}\n\n"
			"event: content_block_start\n"
			"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"The graph \"}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"already has that node.\"}}\n\n"
			"event: content_block_stop\n"
			"data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
			"event: message_delta\n"
			"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":9}}\n\n"
			"event: message_stop\n"
			"data: {\"type\":\"message_stop\"}\n\n";
	}

	// Thinking block with a signature, then text. The signature must survive:
	// the API rejects a follow-up turn whose thinking block has lost it.
	inline const ANSICHAR* WithThinking()
	{
		return
			"event: message_start\n"
			"data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":40,\"output_tokens\":1}}}\n\n"
			"event: content_block_start\n"
			"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"check the pins \"}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"signature_delta\",\"signature\":\"SIGabc123\"}}\n\n"
            "event: content_block_stop\n"
			"data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
			"event: content_block_start\n"
			"data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"text_delta\",\"text\":\"Done.\"}}\n\n"
			"event: content_block_stop\n"
			"data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
			"event: message_delta\n"
			"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"}}\n\n"
			"event: message_stop\n"
			"data: {\"type\":\"message_stop\"}\n\n";
	}

	// A tool_use whose input arrives as partial JSON across several deltas —
	// the case a naive per-event JSON parse gets wrong.
	inline const ANSICHAR* WithToolUse()
	{
		return
			"event: message_start\n"
			"data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":30,\"output_tokens\":1}}}\n\n"
			"event: content_block_start\n"
			"data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_9\",\"name\":\"read_blueprint\",\"input\":{}}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"blueprint\\\":\"}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"/Game/BP_Player\\\"}\"}}\n\n"
			"event: content_block_stop\n"
			"data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
			"event: message_delta\n"
			"data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"}}\n\n"
			"event: message_stop\n"
			"data: {\"type\":\"message_stop\"}\n\n";
	}

	// An HTTP error body: plain JSON, not SSE at all. Feeding it to the parser
	// must yield no events rather than garbage.
	inline const ANSICHAR* ErrorBody()
	{
		return "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\",\"message\":\"Overloaded\"}}";
	}

	// Connection dropped mid-event.
	inline const ANSICHAR* TruncatedMidEvent()
	{
		return
			"event: message_start\n"
			"data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":5,\"output_tokens\":1}}}\n\n"
			"event: content_block_delta\n"
			"data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_de";
	}
}
