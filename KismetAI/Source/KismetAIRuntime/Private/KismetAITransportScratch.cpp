// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAITransportScratch.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_MAC || PLATFORM_LINUX
#include <errno.h>
#include <fcntl.h>
#include <csignal>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
	const TCHAR* ScratchFiles[] = {TEXT("body.json"), TEXT("response.sse"), TEXT("curl.cfg"), TEXT("owner.json")};

	bool IsScratchFile(const FString& Name)
	{
		for (const TCHAR* Allowed : ScratchFiles) if (Name == Allowed) return true;
		return false;
	}

#if PLATFORM_MAC || PLATFORM_LINUX
	bool OwnedObject(const FString& Path, bool bDirectory)
	{
		struct stat Info;
		return lstat(TCHAR_TO_UTF8(*Path), &Info) == 0 && Info.st_uid == geteuid()
			&& (bDirectory ? S_ISDIR(Info.st_mode) : (S_ISREG(Info.st_mode) && Info.st_nlink == 1));
	}

	bool OwnedDescriptor(int Descriptor, bool bDirectory)
	{
		struct stat Info;
		return Descriptor >= 0 && fstat(Descriptor, &Info) == 0 && Info.st_uid == geteuid()
			&& (bDirectory ? S_ISDIR(Info.st_mode) : (S_ISREG(Info.st_mode) && Info.st_nlink == 1));
	}

	bool ProvenDead(uint32 Pid)
	{
		// EPERM and PID reuse are not absence evidence. Never turn an unknown into deletion.
		return Pid > 0 && Pid <= MAX_int32 && kill(static_cast<pid_t>(Pid), 0) == -1 && errno == ESRCH;
	}

	bool HasOnlyOwnedFiles(int DirectoryDescriptor)
	{
		const int Copy = dup(DirectoryDescriptor);
		if (Copy < 0) return false;
		DIR* Entries = fdopendir(Copy);
		if (!Entries) { close(Copy); return false; }
		ON_SCOPE_EXIT { closedir(Entries); };
		// dup shares the directory offset; reset it before inspecting another time.
		rewinddir(Entries);
		errno = 0;
		while (const dirent* Entry = readdir(Entries))
		{
			const FString Name = UTF8_TO_TCHAR(Entry->d_name);
			if (Name == TEXT(".") || Name == TEXT("..")) continue;
			struct stat Info;
			if (!IsScratchFile(Name) || fstatat(DirectoryDescriptor, Entry->d_name, &Info, AT_SYMLINK_NOFOLLOW) != 0
				|| !S_ISREG(Info.st_mode) || Info.st_nlink != 1 || Info.st_uid != geteuid()) return false;
			errno = 0;
		}
		return errno == 0;
	}

	bool MatchesDirectory(int RootDescriptor, const FString& Name, int DirectoryDescriptor)
	{
		struct stat Current, Opened;
		return fstatat(RootDescriptor, TCHAR_TO_UTF8(*Name), &Current, AT_SYMLINK_NOFOLLOW) == 0
			&& S_ISDIR(Current.st_mode) && fstat(DirectoryDescriptor, &Opened) == 0
			&& Current.st_dev == Opened.st_dev && Current.st_ino == Opened.st_ino;
	}

	bool DeleteOpenedDirectory(int RootDescriptor, const FString& Name, int DirectoryDescriptor)
	{
		if (!OwnedDescriptor(DirectoryDescriptor, true) || !MatchesDirectory(RootDescriptor, Name, DirectoryDescriptor)
			|| !HasOnlyOwnedFiles(DirectoryDescriptor)) return false;
		// Relative unlinking stays attached to the classified directory even if its pathname is replaced.
		for (const TCHAR* File : ScratchFiles) unlinkat(DirectoryDescriptor, TCHAR_TO_UTF8(File), 0);
		return MatchesDirectory(RootDescriptor, Name, DirectoryDescriptor)
			&& unlinkat(RootDescriptor, TCHAR_TO_UTF8(*Name), AT_REMOVEDIR) == 0;
	}

	bool ReadOwner(int DirectoryDescriptor, FString& Out)
	{
		const int Descriptor = openat(DirectoryDescriptor, "owner.json", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
		if (Descriptor < 0) return false;
		ON_SCOPE_EXIT { close(Descriptor); };
		struct stat Info;
		if (!OwnedDescriptor(Descriptor, false) || fstat(Descriptor, &Info) != 0 || Info.st_size <= 0 || Info.st_size > 4096) return false;
		TArray<ANSICHAR> Bytes;
		Bytes.SetNumUninitialized(static_cast<int32>(Info.st_size));
		int32 Offset = 0;
		while (Offset < Bytes.Num())
		{
			const ssize_t Count = read(Descriptor, Bytes.GetData() + Offset, Bytes.Num() - Offset);
			if (Count < 0 && errno == EINTR) continue;
			if (Count <= 0) return false;
			Offset += static_cast<int32>(Count);
		}
		const FUTF8ToTCHAR Converted(Bytes.GetData(), Bytes.Num());
		Out = FString(Converted.Length(), Converted.Get());
		return true;
	}
#else
	void DeleteOwnedFiles(const FString& Directory)
	{
		for (const TCHAR* Name : ScratchFiles)
			IFileManager::Get().Delete(*(Directory / Name), false, true, true);
		IFileManager::Get().DeleteDirectory(*Directory, false, false);
	}
#endif
}

FKismetAITransportScratch::~FKismetAITransportScratch()
{
#if PLATFORM_MAC || PLATFORM_LINUX
	if (DirectoryDescriptor >= 0)
	{
		DeleteOpenedDirectory(RootDescriptor, FPaths::GetCleanFilename(Directory), DirectoryDescriptor);
		close(DirectoryDescriptor);
	}
	if (RootDescriptor >= 0) close(RootDescriptor);
#else
	if (!Directory.IsEmpty()) DeleteOwnedFiles(Directory);
#endif
}

bool FKismetAITransportScratch::Begin(const FString& RequestDirectory)
{
	const FString Root = FPaths::GetPath(RequestDirectory);
#if PLATFORM_MAC || PLATFORM_LINUX
	const FString Parent = FPaths::GetPath(Root);
	if (!IFileManager::Get().MakeDirectory(*Parent, true)) return false;
	struct stat ParentInfo;
	if (lstat(TCHAR_TO_UTF8(*Parent), &ParentInfo) != 0 || !S_ISDIR(ParentInfo.st_mode)
		|| ParentInfo.st_uid != geteuid() || (ParentInfo.st_mode & 0022) != 0) return false;
#endif
	if (!IFileManager::Get().MakeDirectory(*Root, true)) return false;
#if PLATFORM_MAC || PLATFORM_LINUX
	if (!OwnedObject(Root, true)) return false;
	RootDescriptor = open(TCHAR_TO_UTF8(*Root), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (!OwnedDescriptor(RootDescriptor, true) || fchmod(RootDescriptor, 0700) != 0) return false;
#endif
	Sweep(Root);
#if PLATFORM_MAC || PLATFORM_LINUX
	const FString RequestName = FPaths::GetCleanFilename(RequestDirectory);
	if (mkdirat(RootDescriptor, TCHAR_TO_UTF8(*RequestName), 0700) != 0) return false;
	DirectoryDescriptor = openat(RootDescriptor, TCHAR_TO_UTF8(*RequestName), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (!OwnedDescriptor(DirectoryDescriptor, true)) return false;
#else
	// Windows inherits the user temp directory ACL; crash sweeping is deliberately unsupported.
	if (IFileManager::Get().DirectoryExists(*RequestDirectory)
		|| !IFileManager::Get().MakeDirectory(*RequestDirectory, false)) return false;
#endif
	Directory = RequestDirectory;
	for (const TCHAR* Name : ScratchFiles)
	{
#if PLATFORM_MAC || PLATFORM_LINUX
		const int Descriptor = openat(DirectoryDescriptor, TCHAR_TO_UTF8(Name), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (Descriptor < 0) return false;
		if (close(Descriptor) != 0) return false;
#else
		if (!FFileHelper::SaveStringToFile(TEXT(""), *(Directory / Name))) return false;
#endif
	}
	return WriteOwner(TEXT("prepared"), 0);
}

bool FKismetAITransportScratch::WriteText(const FString& Name, const FString& Text)
{
	if (Directory.IsEmpty() || !IsScratchFile(Name)) return false;
#if PLATFORM_MAC || PLATFORM_LINUX
	const int Descriptor = openat(DirectoryDescriptor, TCHAR_TO_UTF8(*Name), O_WRONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (Descriptor < 0) return false;
	ON_SCOPE_EXIT { close(Descriptor); };
	if (!OwnedDescriptor(Descriptor, false) || fchmod(Descriptor, 0600) != 0 || ftruncate(Descriptor, 0) != 0) return false;
	const FTCHARToUTF8 Bytes(*Text);
	int32 Offset = 0;
	while (Offset < Bytes.Length())
	{
		const ssize_t Count = write(Descriptor, Bytes.Get() + Offset, Bytes.Length() - Offset);
		if (Count < 0 && errno == EINTR) continue;
		if (Count <= 0) return false;
		Offset += static_cast<int32>(Count);
	}
	return true;
#else
	return FFileHelper::SaveStringToFile(Text, *(Directory / Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
#endif
}

bool FKismetAITransportScratch::WriteOwner(const TCHAR* Phase, uint32 ProcessId)
{
	const FString Marker = FString::Printf(
		TEXT("{\"schema\":1,\"request_id\":\"%s\",\"owner_pid\":%u,\"phase\":\"%s\",\"curl_pid\":%u}"),
		*FPaths::GetCleanFilename(Directory), FPlatformProcess::GetCurrentProcessId(), Phase, ProcessId);
	// A torn marker is retained by Sweep. The launching marker is persisted before CreateProc.
	return WriteText(TEXT("owner.json"), Marker);
}

bool FKismetAITransportScratch::MarkLaunching() { return WriteOwner(TEXT("launching"), 0); }
bool FKismetAITransportScratch::MarkRunning(uint32 ProcessId) { return ProcessId > 0 && WriteOwner(TEXT("running"), ProcessId); }

int32 FKismetAITransportScratch::Sweep(const FString& Root)
{
	int32 Removed = 0;
#if PLATFORM_MAC || PLATFORM_LINUX
	const int RootDescriptor = open(TCHAR_TO_UTF8(*Root), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (RootDescriptor < 0) return 0;
	ON_SCOPE_EXIT { close(RootDescriptor); };
	if (!OwnedDescriptor(RootDescriptor, true)) return 0;
	TArray<FString> Dirs;
	IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true);
	for (const FString& Name : Dirs)
	{
		FGuid RequestId;
		if (!FGuid::ParseExact(Name, EGuidFormats::DigitsWithHyphensLower, RequestId)) continue;
		const int DirectoryDescriptor = openat(RootDescriptor, TCHAR_TO_UTF8(*Name), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (DirectoryDescriptor < 0) continue;
		ON_SCOPE_EXIT { close(DirectoryDescriptor); };
		if (!OwnedDescriptor(DirectoryDescriptor, true) || !HasOnlyOwnedFiles(DirectoryDescriptor)) continue;
		FString Marker;
		TSharedPtr<FJsonObject> Owner;
		if (!ReadOwner(DirectoryDescriptor, Marker)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Marker), Owner) || !Owner.IsValid()) continue;
		double Schema = 0, OwnerPid = 0, CurlPid = 0;
		FString Id, Phase;
		if (!Owner->TryGetNumberField(TEXT("schema"), Schema) || Schema != 1
			|| !Owner->TryGetStringField(TEXT("request_id"), Id) || Id != Name
			|| !Owner->TryGetStringField(TEXT("phase"), Phase)
			|| !Owner->TryGetNumberField(TEXT("owner_pid"), OwnerPid) || !FMath::IsFinite(OwnerPid) || OwnerPid < 1 || OwnerPid > MAX_int32
			|| FMath::FloorToDouble(OwnerPid) != OwnerPid
			|| !Owner->TryGetNumberField(TEXT("curl_pid"), CurlPid) || !FMath::IsFinite(CurlPid) || CurlPid < 0 || CurlPid > MAX_int32
			|| FMath::FloorToDouble(CurlPid) != CurlPid) continue;
		if (!ProvenDead(static_cast<uint32>(OwnerPid))) continue;
		if (Phase == TEXT("prepared") && CurlPid == 0) {}
		else if (Phase == TEXT("running") && CurlPid > 0 && ProvenDead(static_cast<uint32>(CurlPid))) {}
		else continue; // launching, malformed and unknown ownership remain for manual inspection.
		if (DeleteOpenedDirectory(RootDescriptor, Name, DirectoryDescriptor)) ++Removed;
	}
#endif
	return Removed;
}
