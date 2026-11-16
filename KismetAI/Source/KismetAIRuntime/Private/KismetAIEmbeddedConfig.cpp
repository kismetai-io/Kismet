// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIEmbeddedConfig.h"

#include "Misc/FileHelper.h"

#ifndef KISMETAI_EMBED_CONFIG
#define KISMETAI_EMBED_CONFIG 0
#endif

#if KISMETAI_EMBED_CONFIG
#include "EmbeddedConfig.g.inl"
#endif

namespace KismetAIEmbeddedConfig
{
	bool IsEmbedded()
	{
#if KISMETAI_EMBED_CONFIG
		return true;
#else
		return false;
#endif
	}

#if KISMETAI_EMBED_CONFIG
	// Mirror of fnv1a32 in Scripts/embed_config.py. Must stay in lockstep
	// with the codegen — change one, change the other. Hashes the UTF-8
	// encoding of the filename so the result matches Python regardless of
	// TCHAR width (wchar_t on Windows, char16/UTF-16 elsewhere).
	static uint32 NameHashFNV1a(const TCHAR* FileName)
	{
		const FTCHARToUTF8 Utf8(FileName);
		const uint8* Bytes = reinterpret_cast<const uint8*>(Utf8.Get());
		const int32 Length = Utf8.Length();
		uint32 H = 0x811C9DC5u;
		for (int32 I = 0; I < Length; ++I)
		{
			H ^= Bytes[I];
			H = H * 0x01000193u;
		}
		return H;
	}
#endif

	bool TryLoadEmbedded(const TCHAR* FileName, FString& OutContent)
	{
#if KISMETAI_EMBED_CONFIG
		using namespace KismetAIEmbeddedConfig_Generated;
		const uint32 WantHash = NameHashFNV1a(FileName);
		for (int32 Index = 0; Index < EntryCount; ++Index)
		{
			const FEntry& Entry = Entries[Index];
			if (Entry.NameHash != WantHash)
			{
				continue;
			}

			if (Entry.Len <= 0)
			{
				OutContent.Reset();
				return true;
			}

			FFileHelper::BufferToString(OutContent, Entry.Data, Entry.Len);
			return true;
		}
#else
		(void)FileName;
		(void)OutContent;
#endif
		return false;
	}

	bool LoadConfigFile(const TCHAR* FileName, const FString& DiskPath, FString& OutContent)
	{
		if (TryLoadEmbedded(FileName, OutContent))
		{
			return true;
		}
		return FFileHelper::LoadFileToString(OutContent, *DiskPath);
	}
}
