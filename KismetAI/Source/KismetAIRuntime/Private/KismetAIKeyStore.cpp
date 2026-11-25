// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIKeyStore.h"
#include "KismetAIPaths.h"

#include "HAL/FileManager.h"
#include "Misc/AES.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#include <errno.h>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIKeyStore, Log, All);

namespace
{
	constexpr int32 AesBlockSize = 16;
	constexpr int32 FileSaltSize = 16;
	constexpr int32 MaxPlaintextLen = 4096;

	const uint8 FileMagic[4] = { 'K', 'M', 'S', '1' };

	// Per-provider sentinels — different bytes so an Anthropic file loaded as
	// OpenAI (or vice versa) fails the sentinel check rather than decrypting
	// to garbage silently.
	const uint8 AnthropicSentinel[4] = { 'A', 'N', 'T', ':' };
	const uint8 OpenAISentinel[4]    = { 'O', 'A', 'I', ':' };
	const uint8 MinimaxSentinel[4]   = { 'M', 'M', 'X', ':' };
	// Profile slots added with provider profiles (deepseek, openrouter, ...).
	// The slot name is part of the derivation info, so one slot's file cannot
	// be read as another's.
	const uint8 ProviderSentinel[4]  = { 'P', 'K', 'Y', ':' };

	// SHA1 chain over (Tag || GlobalSalt || LoginId || FileSalt). 20 bytes out.
	void HashChunk(uint8 Tag, const FString& GlobalSalt, const FString& LoginId,
	               const TArray<uint8>& FileSalt, uint8 OutHash[20])
	{
		FSHA1 H;
		H.Update(&Tag, 1);

		FTCHARToUTF8 SaltUtf8(*GlobalSalt);
		H.Update((const uint8*)SaltUtf8.Get(), SaltUtf8.Length());

		FTCHARToUTF8 LoginUtf8(*LoginId);
		H.Update((const uint8*)LoginUtf8.Get(), LoginUtf8.Length());

		H.Update(FileSalt.GetData(), FileSalt.Num());
		H.Final();
		H.GetHash(OutHash);
	}

	// Path-parameterized key derivation. GlobalSalt binds the derived key to a
	// specific file purpose (prevents cross-file key reuse).
	FAES::FAESKey DeriveKey(const TArray<uint8>& FileSalt, const FString& GlobalSalt)
	{
		const FString LoginId = FPlatformMisc::GetLoginId();

		uint8 Hash1[20];
		uint8 Hash2[20];
		HashChunk(0x01, GlobalSalt, LoginId, FileSalt, Hash1);
		HashChunk(0x02, GlobalSalt, LoginId, FileSalt, Hash2);

		FAES::FAESKey Key;
		FMemory::Memcpy(Key.Key, Hash1, 16);
		FMemory::Memcpy(Key.Key + 16, Hash2, 16);

		FMemory::Memset(Hash1, 0, sizeof(Hash1));
		FMemory::Memset(Hash2, 0, sizeof(Hash2));
		return Key;
	}

	void Wipe(TArray<uint8>& Buf)
	{
		if (Buf.Num() > 0)
		{
			FMemory::Memset(Buf.GetData(), 0, Buf.Num());
		}
	}

	// FFileHelper::SaveArrayToFile creates with default OS perms (typically
	// 0644 on POSIX). Tighten to 0600 so other users on the box can't read it.
	// Failures here are not fatal — the key file still works — but we log a
	// warning so a permissive umask, an exotic filesystem, or a sandbox that
	// blocks chmod doesn't silently leave the key world-readable.
	void RestrictKeyStorePerms(const FString& Path)
	{
#if PLATFORM_MAC || PLATFORM_LINUX
		FTCHARToUTF8 PathUtf8(*Path);
		if (chmod(PathUtf8.Get(), S_IRUSR | S_IWUSR) != 0)
		{
			UE_LOG(LogKismetAIKeyStore, Warning,
				TEXT("KeyStore: chmod 0600 failed on %s (errno=%d) — file may be world-readable"),
				*Path, errno);
		}
#else
		(void)Path;
#endif
	}

	// 16 cryptographically-OK random bytes via two FGuid::NewGuid() calls.
	// FGuid uses platform RNG (SecRandomCopyBytes on Mac, BCryptGenRandom on
	// Windows, /dev/urandom on Linux).
	void FillRandomBytes(TArray<uint8>& OutBytes, int32 NumBytes)
	{
		OutBytes.SetNumUninitialized(NumBytes);
		int32 Filled = 0;
		while (Filled < NumBytes)
		{
			const FGuid G = FGuid::NewGuid();
			static_assert(sizeof(FGuid) == 16, "FGuid expected to be 16 bytes");
			const int32 Take = FMath::Min<int32>(NumBytes - Filled, 16);
			FMemory::Memcpy(OutBytes.GetData() + Filled, &G, Take);
			Filled += Take;
		}
	}

	// ─────────────────────────────────────────────────────────────────────────
	// Shared encrypt/decrypt internals (path-parameterized).
	// Anthropic and OpenAI public functions delegate here; only the path,
	// global salt, and sentinel bytes differ.
	// ─────────────────────────────────────────────────────────────────────────

	bool SaveKeyInternal(
		const FString& PlaintextKey,
		const FString& FilePath,
		const FString& GlobalSalt,
		const uint8 Sentinel[4])
	{
		if (PlaintextKey.IsEmpty())
		{
			return false;
		}

		FTCHARToUTF8 KeyUtf8(*PlaintextKey);
		const int32 KeyLen = KeyUtf8.Length();
		if (KeyLen <= 0 || KeyLen > MaxPlaintextLen)
		{
			return false;
		}

		TArray<uint8> FileSalt;
		FillRandomBytes(FileSalt, FileSaltSize);

		// Plaintext layout pre-encryption: [sentinel][key bytes][PKCS7 padding]
		const int32 RawLen = 4 + KeyLen;  // sentinel is always 4 bytes
		const int32 PadLen = AesBlockSize - (RawLen % AesBlockSize);
		const int32 PaddedLen = RawLen + PadLen;

		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(PaddedLen);
		FMemory::Memcpy(Buffer.GetData(), Sentinel, 4);
		FMemory::Memcpy(Buffer.GetData() + 4, KeyUtf8.Get(), KeyLen);
		for (int32 Idx = RawLen; Idx < PaddedLen; ++Idx)
		{
			Buffer[Idx] = (uint8)PadLen;
		}

		FAES::FAESKey Key = DeriveKey(FileSalt, GlobalSalt);
		FAES::EncryptData(Buffer.GetData(), Buffer.Num(), Key);
		FMemory::Memset(Key.Key, 0, sizeof(Key.Key));

		TArray<uint8> FileBytes;
		FileBytes.Reserve(sizeof(FileMagic) + FileSaltSize + PaddedLen);
		FileBytes.Append(FileMagic, sizeof(FileMagic));
		FileBytes.Append(FileSalt);
		FileBytes.Append(Buffer);

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), /*Tree=*/true);

		const bool bWrote = FFileHelper::SaveArrayToFile(FileBytes, *FilePath);
		Wipe(Buffer);
		if (!bWrote)
		{
			UE_LOG(LogKismetAIKeyStore, Warning, TEXT("KeyStore: failed to write %s"), *FilePath);
			return false;
		}

		RestrictKeyStorePerms(FilePath);
		return true;
	}

	bool LoadKeyInternal(
		FString& OutPlaintextKey,
		const FString& FilePath,
		const FString& GlobalSalt,
		const uint8 Sentinel[4])
	{
		OutPlaintextKey.Reset();

		TArray<uint8> FileBytes;
		if (!FFileHelper::LoadFileToArray(FileBytes, *FilePath))
		{
			return false;
		}

		const int32 HeaderSize = sizeof(FileMagic) + FileSaltSize;
		if (FileBytes.Num() < HeaderSize + AesBlockSize)
		{
			return false;
		}
		if (FMemory::Memcmp(FileBytes.GetData(), FileMagic, sizeof(FileMagic)) != 0)
		{
			return false;
		}
		const int32 CipherLen = FileBytes.Num() - HeaderSize;
		if (CipherLen <= 0 || (CipherLen % AesBlockSize) != 0)
		{
			return false;
		}

		TArray<uint8> FileSalt;
		FileSalt.Append(FileBytes.GetData() + sizeof(FileMagic), FileSaltSize);

		TArray<uint8> Buffer;
		Buffer.Append(FileBytes.GetData() + HeaderSize, CipherLen);

		FAES::FAESKey Key = DeriveKey(FileSalt, GlobalSalt);
		FAES::DecryptData(Buffer.GetData(), Buffer.Num(), Key);
		FMemory::Memset(Key.Key, 0, sizeof(Key.Key));

		// PKCS7 unpad.
		if (Buffer.Num() == 0)
		{
			return false;
		}
		const uint8 PadLen = Buffer.Last();
		if (PadLen == 0 || PadLen > AesBlockSize || (int32)PadLen > Buffer.Num())
		{
			Wipe(Buffer);
			return false;
		}
		for (int32 Idx = Buffer.Num() - PadLen; Idx < Buffer.Num(); ++Idx)
		{
			if (Buffer[Idx] != PadLen)
			{
				Wipe(Buffer);
				return false;
			}
		}
		const int32 PayloadLen = Buffer.Num() - PadLen;

		// Sentinel check — catches wrong-machine derive and most tampering,
		// and cross-file mix-ups (loading OpenAI key as Anthropic or vice versa).
		if (PayloadLen < 4)
		{
			Wipe(Buffer);
			return false;
		}
		if (FMemory::Memcmp(Buffer.GetData(), Sentinel, 4) != 0)
		{
			Wipe(Buffer);
			return false;
		}

		const int32 KeyByteLen = PayloadLen - 4;
		if (KeyByteLen <= 0 || KeyByteLen > MaxPlaintextLen)
		{
			Wipe(Buffer);
			return false;
		}

		// Copy out as NUL-terminated UTF-8, then convert to FString.
		TArray<uint8> Cstr;
		Cstr.Reserve(KeyByteLen + 1);
		Cstr.Append(Buffer.GetData() + 4, KeyByteLen);
		Cstr.Add(0);
		OutPlaintextKey = FString(UTF8_TO_TCHAR((const char*)Cstr.GetData()));

		Wipe(Buffer);
		Wipe(Cstr);
		return !OutPlaintextKey.IsEmpty();
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Anthropic key — anthropic_key.enc
// ─────────────────────────────────────────────────────────────────────────────

FString KismetAIKeyStore::GetAnthropicKeyPath()
{
	return KismetAIPaths::GetConfigDir() / TEXT("anthropic_key.enc");
}

bool KismetAIKeyStore::HasAnthropicKey()
{
	return IFileManager::Get().FileExists(*GetAnthropicKeyPath());
}

bool KismetAIKeyStore::SaveAnthropicKey(const FString& PlaintextKey)
{
	return SaveKeyInternal(
		PlaintextKey,
		GetAnthropicKeyPath(),
		TEXT("kismet/v1/anthropic_key"),
		AnthropicSentinel);
}

bool KismetAIKeyStore::LoadAnthropicKey(FString& OutPlaintextKey)
{
	return LoadKeyInternal(
		OutPlaintextKey,
		GetAnthropicKeyPath(),
		TEXT("kismet/v1/anthropic_key"),
		AnthropicSentinel);
}

bool KismetAIKeyStore::ClearAnthropicKey()
{
	const FString Path = GetAnthropicKeyPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return true;
	}
	return IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenAI key — openai_key.enc
// ─────────────────────────────────────────────────────────────────────────────

FString KismetAIKeyStore::GetOpenAIKeyPath()
{
	return KismetAIPaths::GetConfigDir() / TEXT("openai_key.enc");
}

bool KismetAIKeyStore::HasOpenAIKey()
{
	return IFileManager::Get().FileExists(*GetOpenAIKeyPath());
}

bool KismetAIKeyStore::SaveOpenAIKey(const FString& PlaintextKey)
{
	return SaveKeyInternal(
		PlaintextKey,
		GetOpenAIKeyPath(),
		TEXT("kismet/v1/openai_key"),
		OpenAISentinel);
}

bool KismetAIKeyStore::LoadOpenAIKey(FString& OutPlaintextKey)
{
	return LoadKeyInternal(
		OutPlaintextKey,
		GetOpenAIKeyPath(),
		TEXT("kismet/v1/openai_key"),
		OpenAISentinel);
}

bool KismetAIKeyStore::ClearOpenAIKey()
{
	const FString Path = GetOpenAIKeyPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return true;
	}
	return IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
}

// ─────────────────────────────────────────────────────────────────────────────
// MiniMax key — minimax_key.enc
// ─────────────────────────────────────────────────────────────────────────────

FString KismetAIKeyStore::GetMinimaxKeyPath()
{
	return KismetAIPaths::GetConfigDir() / TEXT("minimax_key.enc");
}

bool KismetAIKeyStore::HasMinimaxKey()
{
	return IFileManager::Get().FileExists(*GetMinimaxKeyPath());
}

bool KismetAIKeyStore::SaveMinimaxKey(const FString& PlaintextKey)
{
	return SaveKeyInternal(
		PlaintextKey,
		GetMinimaxKeyPath(),
		TEXT("kismet/v1/minimax_key"),
		MinimaxSentinel);
}

bool KismetAIKeyStore::LoadMinimaxKey(FString& OutPlaintextKey)
{
	return LoadKeyInternal(
		OutPlaintextKey,
		GetMinimaxKeyPath(),
		TEXT("kismet/v1/minimax_key"),
		MinimaxSentinel);
}

bool KismetAIKeyStore::ClearMinimaxKey()
{
	const FString Path = GetMinimaxKeyPath();
	if (!IFileManager::Get().FileExists(*Path))
	{
		return true;
	}
	return IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared file helpers — reused by other ~/.kismet secrets (the MCP token)
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// Provider-profile slots. anthropic/openai/minimax keep their historical files
// and sentinels; every other slot is <slot>_key.enc.
// ─────────────────────────────────────────────────────────────────────────────

bool KismetAIKeyStore::IsValidSlot(const FString& Slot)
{
	if (Slot.IsEmpty() || Slot.Len() > 64)
	{
		return false;
	}
	for (const TCHAR Ch : Slot)
	{
		if (!((Ch >= TEXT('a') && Ch <= TEXT('z')) || (Ch >= TEXT('0') && Ch <= TEXT('9')) || Ch == TEXT('-')))
		{
			return false;
		}
	}
	return true;
}

FString KismetAIKeyStore::GetProviderKeyPath(const FString& Slot)
{
	if (Slot == TEXT("anthropic")) return GetAnthropicKeyPath();
	if (Slot == TEXT("openai"))    return GetOpenAIKeyPath();
	if (Slot == TEXT("minimax"))   return GetMinimaxKeyPath();
	if (!IsValidSlot(Slot))        return FString();
	return KismetAIPaths::GetConfigDir() / (Slot + TEXT("_key.enc"));
}

bool KismetAIKeyStore::SaveProviderKey(const FString& Slot, const FString& PlaintextKey)
{
	if (Slot == TEXT("anthropic")) return SaveAnthropicKey(PlaintextKey);
	if (Slot == TEXT("openai"))    return SaveOpenAIKey(PlaintextKey);
	if (Slot == TEXT("minimax"))   return SaveMinimaxKey(PlaintextKey);
	const FString Path = GetProviderKeyPath(Slot);
	if (Path.IsEmpty()) return false;
	return SaveKeyInternal(PlaintextKey, Path, FString::Printf(TEXT("kismet/v1/%s_key"), *Slot), ProviderSentinel);
}

bool KismetAIKeyStore::LoadProviderKey(const FString& Slot, FString& OutPlaintextKey)
{
	if (Slot == TEXT("anthropic")) return LoadAnthropicKey(OutPlaintextKey);
	if (Slot == TEXT("openai"))    return LoadOpenAIKey(OutPlaintextKey);
	if (Slot == TEXT("minimax"))   return LoadMinimaxKey(OutPlaintextKey);
	const FString Path = GetProviderKeyPath(Slot);
	if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path)) return false;
	return LoadKeyInternal(OutPlaintextKey, Path, FString::Printf(TEXT("kismet/v1/%s_key"), *Slot), ProviderSentinel);
}

bool KismetAIKeyStore::HasProviderKey(const FString& Slot)
{
	const FString Path = GetProviderKeyPath(Slot);
	return !Path.IsEmpty() && IFileManager::Get().FileExists(*Path);
}

bool KismetAIKeyStore::ClearProviderKey(const FString& Slot)
{
	const FString Path = GetProviderKeyPath(Slot);
	if (Path.IsEmpty()) return false;
	if (!IFileManager::Get().FileExists(*Path)) return true;
	return IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
}

void KismetAIKeyStore::RestrictFileToOwner(const FString& Path)
{
	RestrictKeyStorePerms(Path);
}

void KismetAIKeyStore::GenerateRandomBytes(TArray<uint8>& OutBytes, int32 NumBytes)
{
	FillRandomBytes(OutBytes, NumBytes);
}
