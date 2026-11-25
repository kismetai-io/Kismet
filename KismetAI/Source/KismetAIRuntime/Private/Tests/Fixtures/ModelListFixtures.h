// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Model list bodies as the providers send them, trimmed to the fields the
// parsers read plus a few they must ignore. Shapes follow the vendors' list
// endpoints as documented on 2026-11-25 (OpenRouter /api/v1/models, Anthropic
// /v1/models, Moonshot /v1/models, Gemini /v1beta/models, Ollama /api/tags and
// /api/show, LM Studio /api/v1/models, llama.cpp / vLLM /v1/models), and the
// update / add / drop / malformed cases of Eden's catalog fixtures.

#pragma once

#include "CoreMinimal.h"

namespace KismetAIModelListFixtures
{
	// Two curated Anthropic models; the live list updates one, drops the other
	// (claude-test-old is absent), and adds a newer model of the first family.
	inline FString TestCatalog()
	{
		return TEXT(R"json({
			"schema_version": 1,
			"defaults": { "context_window": 200000, "pricing": { "input": 3.0, "output": 15.0 } },
			"models": [
				{ "id": "claude-opus-5-5", "alias": "opus", "display_name": "Opus 5.5", "provider": "anthropic",
				  "tier": "premium", "picker": true, "context_window": 1000000, "max_output_tokens": 128000,
				  "openrouter_id": "anthropic/claude-opus-5.5", "openrouter_name": "Claude Opus 5.5",
				  "pricing": { "input": 4.0, "output": 20.0, "cache_read": 0.2, "cache_write": 5.0 },
				  "capabilities": { "adaptive_thinking": true, "budget_thinking": false, "thinking_always_on": true,
				    "sampling_params": false, "effort": ["low", "medium", "high", "xhigh", "max"] } },
				{ "id": "claude-test-old", "alias": "old", "display_name": "Old Test", "provider": "anthropic",
				  "tier": "fast", "picker": true, "context_window": 200000,
				  "pricing": { "input": 1.0, "output": 5.0 } },
				{ "id": "x-ai/grok-4.7", "alias": "grok", "display_name": "Grok 4.7", "provider": "openrouter",
				  "tier": "standard", "picker": true, "context_window": 500000,
				  "pricing": { "input": 2.0, "output": 6.0 },
				  "capabilities": { "thinking": false, "reasoning": true, "effort": ["low", "medium", "high"] } }
			]
		})json");
	}

	inline FString AnthropicList()
	{
		return TEXT(R"json({
			"data": [
				{ "type": "model", "id": "claude-opus-5-6", "display_name": "Claude Opus 5.6",
				  "created_at": "2026-11-20T00:00:00Z", "max_input_tokens": 1000000, "max_tokens": 128000,
				  "capabilities": { "image_input": { "supported": true } } },
				{ "type": "model", "id": "claude-opus-5-5", "display_name": "Claude Opus 5.5",
				  "created_at": "2026-09-01T00:00:00Z", "max_input_tokens": 900000, "max_tokens": 64000,
				  "capabilities": { "image_input": { "supported": true } } },
				{ "type": "model", "id": "claude-opus-5-5-20261101", "display_name": "Claude Opus 5.5 (snapshot)",
				  "created_at": "2026-11-01T00:00:00Z", "max_input_tokens": 1000000, "max_tokens": 128000 },
				{ "type": "model", "id": "claude-opus-4-8", "display_name": "Claude Opus 4.8",
				  "created_at": "2026-03-01T00:00:00Z", "max_input_tokens": 1000000, "max_tokens": 128000 }
			],
			"has_more": false, "first_id": "claude-opus-5-6", "last_id": "claude-opus-4-8"
		})json");
	}

	inline FString OpenRouterList()
	{
		return TEXT(R"json({
			"data": [
				{ "id": "anthropic/claude-opus-5.5", "name": "Anthropic: Claude Opus 5.5", "created": 1756684800,
				  "context_length": 1000000,
				  "architecture": { "input_modalities": ["text", "image"], "output_modalities": ["text"] },
				  "top_provider": { "context_length": 1000000, "max_completion_tokens": 128000, "is_moderated": false },
				  "supported_parameters": ["tools", "tool_choice", "reasoning", "max_tokens"],
				  "pricing": { "prompt": "0.000004", "completion": "0.00002", "input_cache_read": "0.0000002" } },
				{ "id": "x-ai/grok-4.7", "name": "xAI: Grok 4.7", "created": 1760000000,
				  "context_length": 500000,
				  "architecture": { "input_modalities": ["text"] },
				  "top_provider": { "max_completion_tokens": 450000 },
				  "supported_parameters": ["max_tokens", "temperature"],
				  "pricing": { "prompt": "0.000002", "completion": "0.000006" } },
				{ "id": "x-ai/grok-4.7:batch", "name": "xAI: Grok 4.7 (batch)", "created": 1790000000,
				  "context_length": 500000, "pricing": { "prompt": "0.000001", "completion": "0.000003" } },
				{ "id": "openrouter/auto", "name": "Auto Router", "created": 1700000000,
				  "context_length": 2000000, "pricing": { "prompt": "-1", "completion": "-1" } }
			]
		})json");
	}

	inline FString MoonshotList()
	{
		return TEXT(R"json({ "object": "list", "data": [
			{ "id": "kimi-k3.1-pro", "object": "model", "created": 1760000000, "owned_by": "moonshot",
			  "context_length": 1048576, "supports_image_in": true, "supports_reasoning": true },
			{ "id": "kimi-k3", "object": "model", "created": 1750000000, "owned_by": "moonshot", "context_length": 1048576 }
		] })json");
	}

	inline FString GoogleList()
	{
		return TEXT(R"json({ "models": [
			{ "name": "models/gemini-4-argon", "displayName": "Gemini 4 Argon", "inputTokenLimit": 1048576,
			  "outputTokenLimit": 65536, "supportedGenerationMethods": ["generateContent", "countTokens"] },
			{ "name": "models/text-embedding-005", "displayName": "Embedding", "inputTokenLimit": 2048,
			  "supportedGenerationMethods": ["embedContent"] }
		], "nextPageToken": "" })json");
	}

	inline FString Malformed() { return TEXT(R"json({"data": [ {"id": "claude-opus-5-5", )json"); }
	inline FString WrongShape() { return TEXT(R"json({"error": {"type": "authentication_error", "message": "invalid x-api-key"}})json"); }

	inline FString OllamaTags()
	{
		return TEXT(R"json({ "models": [
			{ "name": "qwen3:8b", "model": "qwen3:8b", "modified_at": "2026-11-20T10:11:12.123456789+01:00",
			  "size": 5200000000, "details": { "family": "qwen3", "parameter_size": "8.2B" } },
			{ "name": "gemma2:2b", "model": "gemma2:2b", "modified_at": "2026-10-01T00:00:00Z",
			  "details": { "family": "gemma2" } },
			{ "name": "mystery:latest", "model": "mystery:latest", "modified_at": "2026-10-01T00:00:00Z" }
		] })json");
	}

	inline FString OllamaShowQwen()
	{
		return TEXT(R"json({ "parameters": "temperature 0.6\nnum_ctx 40960", "capabilities": ["completion", "tools", "thinking"],
			"model_info": { "general.architecture": "qwen3", "qwen3.context_length": 131072, "qwen3.embedding_length": 4096 } })json");
	}

	inline FString OllamaShowGemma()
	{
		return TEXT(R"json({ "parameters": "stop \"<end_of_turn>\"", "capabilities": ["completion"],
			"model_info": { "general.architecture": "gemma2", "gemma2.context_length": 8192 } })json");
	}

	inline FString LMStudioList()
	{
		return TEXT(R"json({ "models": [
			{ "type": "llm", "publisher": "qwen", "key": "qwen/qwen3-coder-30b", "display_name": "Qwen3 Coder 30B",
			  "max_context_length": 262144, "format": "gguf",
			  "loaded_instances": [ { "id": "qwen/qwen3-coder-30b", "config": { "context_length": 32768 } } ],
			  "capabilities": { "vision": false, "trained_for_tool_use": true } },
			{ "type": "llm", "key": "google/gemma-3-12b", "display_name": "Gemma 3 12B", "max_context_length": 131072,
			  "loaded_instances": [], "capabilities": { "vision": true, "trained_for_tool_use": false } },
			{ "type": "embedding", "key": "text-embedding-nomic", "display_name": "Nomic Embed", "max_context_length": 2048 }
		] })json");
	}

	// llama.cpp llama-server and vLLM: OpenAI shape with their own context fields.
	inline FString GenericOpenAIList()
	{
		return TEXT(R"json({ "object": "list", "data": [
			{ "id": "Qwen/Qwen3-32B", "object": "model", "created": 1760000000, "owned_by": "vllm", "max_model_len": 65536 },
			{ "id": "local-gguf", "object": "model", "owned_by": "llamacpp", "meta": { "n_ctx_train": 16384 } },
			{ "id": "no-context-reported", "object": "model" }
		] })json");
	}
}
