// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIMcpToken.h"
#include "KismetAIKeyStore.h"
#include "KismetAIPaths.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIMcpToken, Log, All);

namespace
{
	constexpr int32 TokenBytes = 32;
	constexpr int32 MinTokenChars = 32;
}

FString KismetAIMcpToken::GetTokenPath()
{
	return KismetAIPaths::GetConfigDir() / TEXT("mcp_token");
}

bool KismetAIMcpToken::IsUsableToken(const FString& Token)
{
	if (Token.Len() < MinTokenChars)
	{
		return false;
	}
	for (const TCHAR Ch : Token)
	{
		const bool bAllowed = (Ch >= TEXT('0') && Ch <= TEXT('9'))
			|| (Ch >= TEXT('a') && Ch <= TEXT('z'))
			|| (Ch >= TEXT('A') && Ch <= TEXT('Z'))
			|| Ch == TEXT('-') || Ch == TEXT('_');
		if (!bAllowed)
		{
			return false;
		}
	}
	return true;
}

FString KismetAIMcpToken::GetOrCreateToken()
{
	const FString Path = GetTokenPath();

	FString Existing;
	if (FFileHelper::LoadFileToString(Existing, *Path))
	{
		Existing.TrimStartAndEndInline();
		if (IsUsableToken(Existing))
		{
			// Tighten on every read too, in case the file was copied in or its
			// mode was loosened since it was written.
			KismetAIKeyStore::RestrictFileToOwner(Path);
			return Existing;
		}
		UE_LOG(LogKismetAIMcpToken, Warning, TEXT("MCP token file %s is not a usable token; replacing it"), *Path);
	}

	TArray<uint8> Bytes;
	KismetAIKeyStore::GenerateRandomBytes(Bytes, TokenBytes);
	FString Token;
	Token.Reserve(TokenBytes * 2);
	for (const uint8 Byte : Bytes)
	{
		Token.Appendf(TEXT("%02x"), Byte);
	}
	FMemory::Memset(Bytes.GetData(), 0, Bytes.Num());

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (!FFileHelper::SaveStringToFile(Token, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogKismetAIMcpToken, Error, TEXT("Could not write MCP token to %s; the /mcp endpoint will refuse requests"), *Path);
		return FString();
	}
	KismetAIKeyStore::RestrictFileToOwner(Path);
	UE_LOG(LogKismetAIMcpToken, Log, TEXT("Generated MCP token at %s"), *Path);
	return Token;
}
