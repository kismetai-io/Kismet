// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIKeyStore.h"
#include "ScopedTempHome.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"

constexpr auto KismetAIKeyStoreTestFlags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_RoundTrip, "KismetAI.KeyStore.RoundTrip", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_RoundTrip::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	const FString Sample = TEXT("sk-ant-api03-test-deadbeef-0123456789ABCDEF");
	TestTrue(TEXT("Save"), KismetAIKeyStore::SaveAnthropicKey(Sample));
	TestTrue(TEXT("HasAnthropicKey after save"), KismetAIKeyStore::HasAnthropicKey());

	FString Loaded;
	TestTrue(TEXT("Load"), KismetAIKeyStore::LoadAnthropicKey(Loaded));
	TestEqual(TEXT("Round-trip equals input"), Loaded, Sample);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_FileNotPlaintext, "KismetAI.KeyStore.FileNotPlaintext", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_FileNotPlaintext::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	const FString Sample = TEXT("sk-ant-api03-secret-canary-marker-abcdef");
	TestTrue(TEXT("Save"), KismetAIKeyStore::SaveAnthropicKey(Sample));

	TArray<uint8> Bytes;
	TestTrue(TEXT("Read on disk"), FFileHelper::LoadFileToArray(Bytes, *KismetAIKeyStore::GetAnthropicKeyPath()));

	// Search for the plaintext substring in the file bytes — must not appear.
	FTCHARToUTF8 Needle(*Sample);
	bool bFound = false;
	if (Bytes.Num() >= Needle.Length())
	{
		for (int32 Idx = 0; Idx <= Bytes.Num() - Needle.Length(); ++Idx)
		{
			if (FMemory::Memcmp(Bytes.GetData() + Idx, Needle.Get(), Needle.Length()) == 0)
			{
				bFound = true;
				break;
			}
		}
	}
	TestFalse(TEXT("Plaintext key must not appear in file bytes"), bFound);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_RejectsEmpty, "KismetAI.KeyStore.RejectsEmpty", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_RejectsEmpty::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));
	TestFalse(TEXT("Save rejects empty input"), KismetAIKeyStore::SaveAnthropicKey(TEXT("")));
	TestFalse(TEXT("Has remains false"), KismetAIKeyStore::HasAnthropicKey());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_TamperedSaltFailsLoad, "KismetAI.KeyStore.TamperedSaltFailsLoad", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_TamperedSaltFailsLoad::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	TestTrue(TEXT("Save"), KismetAIKeyStore::SaveAnthropicKey(TEXT("sk-ant-test-tamper-salt")));

	const FString Path = KismetAIKeyStore::GetAnthropicKeyPath();
	TArray<uint8> Bytes;
	TestTrue(TEXT("Read back"), FFileHelper::LoadFileToArray(Bytes, *Path));
	TestTrue(TEXT("File contains header + at least one block"), Bytes.Num() >= 4 + 16 + 16);

	// Flip a byte in the file salt (bytes 4..19). This changes the derived
	// AES key, so every ciphertext block decrypts to garbage and the
	// sentinel/PKCS7 check rejects with overwhelming probability.
	Bytes[10] ^= 0xFF;
	TestTrue(TEXT("Write tampered"), FFileHelper::SaveArrayToFile(Bytes, *Path));

	FString Loaded;
	TestFalse(TEXT("Load rejects tampered salt"), KismetAIKeyStore::LoadAnthropicKey(Loaded));
	TestTrue(TEXT("Loaded is empty after rejection"), Loaded.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_TamperedFirstBlockFailsLoad, "KismetAI.KeyStore.TamperedFirstBlockFailsLoad", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_TamperedFirstBlockFailsLoad::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	TestTrue(TEXT("Save"), KismetAIKeyStore::SaveAnthropicKey(TEXT("sk-ant-test-tamper-block1")));

	const FString Path = KismetAIKeyStore::GetAnthropicKeyPath();
	TArray<uint8> Bytes;
	TestTrue(TEXT("Read back"), FFileHelper::LoadFileToArray(Bytes, *Path));
	TestTrue(TEXT("File contains header + first block"), Bytes.Num() >= 4 + 16 + 16);

	// Flip a byte in the first ciphertext block (bytes 20..35). With ECB
	// this scrambles only block 1, where the "ANT:" sentinel lives, so the
	// sentinel check rejects with probability 255/256. Tier-1 has no MAC,
	// so post-block-1 tampering is intentionally best-effort.
	Bytes[20] ^= 0xFF;
	TestTrue(TEXT("Write tampered"), FFileHelper::SaveArrayToFile(Bytes, *Path));

	FString Loaded;
	TestFalse(TEXT("Load rejects tampered first block"), KismetAIKeyStore::LoadAnthropicKey(Loaded));
	TestTrue(TEXT("Loaded is empty after rejection"), Loaded.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_BadMagicFailsLoad, "KismetAI.KeyStore.BadMagicFailsLoad", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_BadMagicFailsLoad::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	TestTrue(TEXT("Save"), KismetAIKeyStore::SaveAnthropicKey(TEXT("sk-ant-test-magic")));

	const FString Path = KismetAIKeyStore::GetAnthropicKeyPath();
	TArray<uint8> Bytes;
	TestTrue(TEXT("Read back"), FFileHelper::LoadFileToArray(Bytes, *Path));
	TestTrue(TEXT("Has at least magic bytes"), Bytes.Num() >= 4);
	Bytes[0] ^= 0xFF;
	TestTrue(TEXT("Write"), FFileHelper::SaveArrayToFile(Bytes, *Path));

	FString Loaded;
	TestFalse(TEXT("Load rejects wrong magic"), KismetAIKeyStore::LoadAnthropicKey(Loaded));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_MissingReturnsFalse, "KismetAI.KeyStore.MissingReturnsFalse", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_MissingReturnsFalse::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	TestFalse(TEXT("HasAnthropicKey when missing"), KismetAIKeyStore::HasAnthropicKey());
	FString Loaded;
	TestFalse(TEXT("Load returns false when missing"), KismetAIKeyStore::LoadAnthropicKey(Loaded));
	TestTrue(TEXT("Loaded is empty"), Loaded.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_Clear, "KismetAI.KeyStore.Clear", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_Clear::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	TestTrue(TEXT("Save"), KismetAIKeyStore::SaveAnthropicKey(TEXT("sk-ant-test-clear")));
	TestTrue(TEXT("Has after save"), KismetAIKeyStore::HasAnthropicKey());
	TestTrue(TEXT("Clear succeeds"), KismetAIKeyStore::ClearAnthropicKey());
	TestFalse(TEXT("Has after clear"), KismetAIKeyStore::HasAnthropicKey());
	TestTrue(TEXT("Clear is idempotent"), KismetAIKeyStore::ClearAnthropicKey());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_MiniMaxRoundTrip, "KismetAI.KeyStore.MiniMaxRoundTrip", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_MiniMaxRoundTrip::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	const FString Sample = TEXT("sk-minimax-test-deadbeef-0123456789ABCDEF");
	TestTrue(TEXT("Save MiniMax key"), KismetAIKeyStore::SaveMinimaxKey(Sample));
	TestTrue(TEXT("HasMinimaxKey after save"), KismetAIKeyStore::HasMinimaxKey());
	TestFalse(TEXT("HasAnthropicKey remains false"), KismetAIKeyStore::HasAnthropicKey());
	TestFalse(TEXT("HasOpenAIKey remains false"), KismetAIKeyStore::HasOpenAIKey());

	FString Loaded;
	TestTrue(TEXT("Load MiniMax key"), KismetAIKeyStore::LoadMinimaxKey(Loaded));
	TestEqual(TEXT("MiniMax round-trip equals input"), Loaded, Sample);

	FString WrongProviderLoaded;
	TestFalse(TEXT("MiniMax key cannot be loaded as Anthropic key"),
		KismetAIKeyStore::LoadAnthropicKey(WrongProviderLoaded));
	TestTrue(TEXT("Clear MiniMax key succeeds"), KismetAIKeyStore::ClearMinimaxKey());
	TestFalse(TEXT("HasMinimaxKey after clear"), KismetAIKeyStore::HasMinimaxKey());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKismetAIKeyStore_OverwriteChangesCiphertext, "KismetAI.KeyStore.OverwriteChangesCiphertext", KismetAIKeyStoreTestFlags)
bool FKismetAIKeyStore_OverwriteChangesCiphertext::RunTest(const FString& Parameters)
{
	FScopedTempHome Home(TEXT("KeyStoreTest"));

	const FString Sample = TEXT("sk-ant-test-same-plaintext-twice");

	TestTrue(TEXT("Save 1"), KismetAIKeyStore::SaveAnthropicKey(Sample));
	TArray<uint8> Bytes1;
	TestTrue(TEXT("Read 1"), FFileHelper::LoadFileToArray(Bytes1, *KismetAIKeyStore::GetAnthropicKeyPath()));

	TestTrue(TEXT("Save 2"), KismetAIKeyStore::SaveAnthropicKey(Sample));
	TArray<uint8> Bytes2;
	TestTrue(TEXT("Read 2"), FFileHelper::LoadFileToArray(Bytes2, *KismetAIKeyStore::GetAnthropicKeyPath()));

	// Per-file random salt should produce different ciphertexts for the
	// same plaintext on consecutive saves.
	TestEqual(TEXT("Same length"), Bytes1.Num(), Bytes2.Num());
	bool bDiffers = Bytes1.Num() != Bytes2.Num();
	if (!bDiffers)
	{
		bDiffers = FMemory::Memcmp(Bytes1.GetData(), Bytes2.GetData(), Bytes1.Num()) != 0;
	}
	TestTrue(TEXT("Ciphertext differs across saves"), bDiffers);

	// Both still decrypt to the same plaintext.
	FString Loaded;
	TestTrue(TEXT("Load after second save"), KismetAIKeyStore::LoadAnthropicKey(Loaded));
	TestEqual(TEXT("Decrypts to original"), Loaded, Sample);
	return true;
}
