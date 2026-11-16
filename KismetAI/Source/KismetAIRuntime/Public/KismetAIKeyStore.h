// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"

// Tier-1 at-rest protection for the user's BYOK Anthropic API key.
//
// Stored at <ConfigDir>/anthropic_key.enc, encrypted AES-256-ECB with a key
// derived from FPlatformMisc::GetLoginId() (per-user-per-machine) plus a
// per-file random salt. PKCS7-padded; a 4-byte plaintext sentinel in the
// first block detects wrong-machine derive and salt/first-block tampering.
//
// Threat model — what this protects against:
//   - Casual `cat ~/.kismet/anthropic_key.enc` reveals nothing useful.
//   - Copying ~/.kismet/ to another machine fails to decrypt (different
//     LoginId).
//   - Tampering with the file salt or the first ciphertext block reliably
//     fails the sentinel check.
//
// Threat model — what this does NOT protect against:
//   - Malware running as the same OS user (it can re-derive the same key).
//   - Targeted bit-flips in ciphertext blocks past the first one. There is
//     no MAC; ECB confines damage to the flipped block, so a bit-flip in
//     blocks 2..N may produce a partially-correct decryption that passes
//     the sentinel. Acceptable for tier-1; tier-2 (OS keychain) replaces
//     this whole module rather than adding a MAC.
namespace KismetAIKeyStore
{
	// <ConfigDir>/anthropic_key.enc (mirrors FKismetAIModule::GetConfigDir()).
	KISMETAIRUNTIME_API FString GetAnthropicKeyPath();

	// True if the encrypted key file exists on disk.
	KISMETAIRUNTIME_API bool HasAnthropicKey();

	// Encrypt PlaintextKey and persist. Overwrites any existing file. Empty
	// keys are rejected (use ClearAnthropicKey to remove). On POSIX the file
	// is chmod'd to 0600 after writing.
	KISMETAIRUNTIME_API bool SaveAnthropicKey(const FString& PlaintextKey);

	// Read, decrypt, and validate. Returns false (and leaves OutPlaintextKey
	// empty) if the file is missing, malformed, or fails the sentinel check
	// — e.g. ~/.kismet/ was copied to another machine, or GetLoginId changed.
	KISMETAIRUNTIME_API bool LoadAnthropicKey(FString& OutPlaintextKey);

	// Delete the file. Returns true if the file is gone afterwards (whether
	// it was deleted or never existed).
	KISMETAIRUNTIME_API bool ClearAnthropicKey();

	// OpenAI key — <ConfigDir>/openai_key.enc.
	// Same AES-256-ECB + PKCS7 + per-file salt scheme as the Anthropic key.
	// Sentinel bytes differ so a file mix-up is detected at load time.

	// <ConfigDir>/openai_key.enc
	KISMETAIRUNTIME_API FString GetOpenAIKeyPath();

	// True if the encrypted OpenAI key file exists on disk.
	KISMETAIRUNTIME_API bool HasOpenAIKey();

	// Encrypt PlaintextKey and persist to openai_key.enc. Empty keys rejected.
	KISMETAIRUNTIME_API bool SaveOpenAIKey(const FString& PlaintextKey);

	// Read, decrypt, and validate the OpenAI key. Returns false on any failure.
	KISMETAIRUNTIME_API bool LoadOpenAIKey(FString& OutPlaintextKey);

	// Delete openai_key.enc. Returns true if the file is gone afterwards.
	KISMETAIRUNTIME_API bool ClearOpenAIKey();

	// MiniMax key — <ConfigDir>/minimax_key.enc.
	// Same AES-256-ECB + PKCS7 + per-file salt scheme as the Anthropic/OpenAI keys.
	// Sentinel bytes differ so a file mix-up is detected at load time.

	// <ConfigDir>/minimax_key.enc
	KISMETAIRUNTIME_API FString GetMinimaxKeyPath();

	// True if the encrypted MiniMax key file exists on disk.
	KISMETAIRUNTIME_API bool HasMinimaxKey();

	// Encrypt PlaintextKey and persist to minimax_key.enc. Empty keys rejected.
	KISMETAIRUNTIME_API bool SaveMinimaxKey(const FString& PlaintextKey);

	// Read, decrypt, and validate the MiniMax key. Returns false on any failure.
	KISMETAIRUNTIME_API bool LoadMinimaxKey(FString& OutPlaintextKey);

	// Delete minimax_key.enc. Returns true if the file is gone afterwards.
	KISMETAIRUNTIME_API bool ClearMinimaxKey();

	// --- shared helpers for other secrets under ~/.kismet -------------------

	// chmod 0600 on POSIX (no-op elsewhere). Logs a warning on failure rather
	// than failing: the file still works, it is just not tightened.
	// Provider-profile key slots (Config/providers.json "key_slot"). The
	// anthropic/openai/minimax slots are the files above; any other slot
	// ([a-z0-9-], max 64) is ~/.kismet/<slot>_key.enc.
	KISMETAIRUNTIME_API bool IsValidSlot(const FString& Slot);
	KISMETAIRUNTIME_API FString GetProviderKeyPath(const FString& Slot);
	KISMETAIRUNTIME_API bool SaveProviderKey(const FString& Slot, const FString& PlaintextKey);
	KISMETAIRUNTIME_API bool LoadProviderKey(const FString& Slot, FString& OutPlaintextKey);
	KISMETAIRUNTIME_API bool HasProviderKey(const FString& Slot);
	KISMETAIRUNTIME_API bool ClearProviderKey(const FString& Slot);

	KISMETAIRUNTIME_API void RestrictFileToOwner(const FString& Path);

	// NumBytes of platform-RNG output (the same source the key salts use).
	KISMETAIRUNTIME_API void GenerateRandomBytes(TArray<uint8>& OutBytes, int32 NumBytes);
}
