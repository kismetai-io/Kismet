// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#include "KismetAIAuthority.h"
#include "KismetAIToolRegistry.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogKismetAIAuthority, Log, All);

namespace
{
	constexpr int32 MaxRecordedDecisions = 512;

	// Approval grants are short-lived by construction: "once" has to be used by
	// the call that asked, "session" is bounded even if nobody ends the session.
	constexpr double OnceApprovalTtlSeconds = 300.0;
	constexpr double SessionApprovalTtlSeconds = 4.0 * 3600.0;

	const TCHAR* OpName(EKismetAIAuthorityOp Op)
	{
		switch (Op)
		{
		case EKismetAIAuthorityOp::Edit:   return TEXT("edit");
		case EKismetAIAuthorityOp::Create: return TEXT("create");
		case EKismetAIAuthorityOp::Delete: return TEXT("delete");
		default:                           return TEXT("none");
		}
	}

	const EKismetAIAuthorityOp AllOps[] = { EKismetAIAuthorityOp::Edit, EKismetAIAuthorityOp::Create, EKismetAIAuthorityOp::Delete };

	FString StringField(const TSharedPtr<FJsonObject>& Body, const TCHAR* Field)
	{
		FString Out;
		if (Body.IsValid())
		{
			Body->TryGetStringField(Field, Out);
		}
		return Out;
	}

	// Mutating tools whose batch may consist only of inspection actions
	// ("get_component_property"). Such a batch is a read and needs no grant.
	bool BatchIsReadOnly(const TSharedPtr<FJsonObject>& Body)
	{
		const TArray<TSharedPtr<FJsonValue>>* Ops = nullptr;
		if (!Body.IsValid() || !Body->TryGetArrayField(TEXT("operations"), Ops) || !Ops || Ops->Num() == 0)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *Ops)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			FString Action;
			if (!V.IsValid() || !V->TryGetObject(Obj) || !Obj || !(*Obj)->TryGetStringField(TEXT("action"), Action))
			{
				return false;
			}
			if (!(Action == TEXT("get") || Action.StartsWith(TEXT("get_"))))
			{
				return false;
			}
		}
		return true;
	}

	FString JoinPath(const FString& Folder, const FString& Name)
	{
		FString F = Folder;
		while (F.EndsWith(TEXT("/")))
		{
			F.LeftChopInline(1);
		}
		return F + TEXT("/") + Name;
	}

	FString LeafOf(const FString& PackagePath)
	{
		int32 Slash = INDEX_NONE;
		return PackagePath.FindLastChar(TEXT('/'), Slash) ? PackagePath.Mid(Slash + 1) : PackagePath;
	}

	FString FolderOf(const FString& PackagePath)
	{
		int32 Slash = INDEX_NONE;
		return (PackagePath.FindLastChar(TEXT('/'), Slash) && Slash > 0) ? PackagePath.Left(Slash) : FString(TEXT("/"));
	}

	TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& S : In)
		{
			Out.Add(MakeShared<FJsonValueString>(S));
		}
		return Out;
	}

	FString EntryPointsToString(const TArray<EKismetAIEntryPoint>& EntryPoints)
	{
		TArray<FString> Names;
		for (EKismetAIEntryPoint E : EntryPoints)
		{
			Names.Add(LexToString(E));
		}
		return FString::Join(Names, TEXT(","));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Names
// ─────────────────────────────────────────────────────────────────────────────

FString LexToString(EKismetAIAuthorityOp Ops)
{
	TArray<FString> Names;
	for (EKismetAIAuthorityOp Op : AllOps)
	{
		if (EnumHasAnyFlags(Ops, Op))
		{
			Names.Add(OpName(Op));
		}
	}
	return Names.Num() ? FString::Join(Names, TEXT(",")) : FString(TEXT("none"));
}

const TCHAR* LexToString(EKismetAIGrantLifetime Lifetime)
{
	switch (Lifetime)
	{
	case EKismetAIGrantLifetime::Once:    return TEXT("once");
	case EKismetAIGrantLifetime::Session: return TEXT("session");
	default:                              return TEXT("until_revoked");
	}
}

const TCHAR* LexToString(EKismetAIApprovalResolution Resolution)
{
	switch (Resolution)
	{
	case EKismetAIApprovalResolution::Granted:         return TEXT("granted");
	case EKismetAIApprovalResolution::Declined:        return TEXT("declined");
	case EKismetAIApprovalResolution::UnknownRequest:  return TEXT("unknown_request");
	case EKismetAIApprovalResolution::AlreadyAnswered: return TEXT("already_answered");
	case EKismetAIApprovalResolution::Mismatch:        return TEXT("mismatch");
	case EKismetAIApprovalResolution::Expired:         return TEXT("expired");
	default:                                           return TEXT("unknown");
	}
}

bool KismetAIAuthority::ParseEntryPoint(const FString& Name, EKismetAIEntryPoint& Out)
{
	for (EKismetAIEntryPoint E : { EKismetAIEntryPoint::InProcess, EKismetAIEntryPoint::Panel,
		EKismetAIEntryPoint::HttpAgentRun, EKismetAIEntryPoint::Mcp, EKismetAIEntryPoint::Rest })
	{
		if (Name.Equals(LexToString(E), ESearchCase::IgnoreCase))
		{
			Out = E;
			return true;
		}
	}
	return false;
}

bool KismetAIAuthority::ParseOps(const TArray<FString>& Names, EKismetAIAuthorityOp& Out, FString& OutError)
{
	Out = EKismetAIAuthorityOp::None;
	for (const FString& Raw : Names)
	{
		const FString Name = Raw.TrimStartAndEnd();
		bool bFound = false;
		for (EKismetAIAuthorityOp Op : AllOps)
		{
			if (Name.Equals(OpName(Op), ESearchCase::IgnoreCase))
			{
				Out |= Op;
				bFound = true;
			}
		}
		if (!bFound)
		{
			OutError = FString::Printf(TEXT("unknown operation '%s' (expected edit, create or delete)"), *Name);
			return false;
		}
	}
	if (Out == EKismetAIAuthorityOp::None)
	{
		OutError = TEXT("at least one operation (edit, create, delete) is required");
		return false;
	}
	return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Path normalization
// ─────────────────────────────────────────────────────────────────────────────

bool KismetAIAuthority::NormalizeAssetReference(const FString& Raw, FString& OutPackagePath, FString* OutLeaf)
{
	OutPackagePath.Empty();
	if (OutLeaf)
	{
		OutLeaf->Empty();
	}

	FString S = Raw.TrimStartAndEnd();

	// Class'/Game/A/B.B' and "quoted" forms.
	int32 Quote = INDEX_NONE;
	if (S.EndsWith(TEXT("'")) && S.FindChar(TEXT('\''), Quote) && Quote < S.Len() - 1)
	{
		S = S.Mid(Quote + 1, S.Len() - Quote - 2);
	}
	S.TrimQuotesInline();
	S.ReplaceInline(TEXT("\\"), TEXT("/"));
	S.TrimStartAndEndInline();

	for (const TCHAR C : S)
	{
		if (C < 32 || C == TEXT('*') || C == TEXT('?') || C == TEXT('"') || C == TEXT('<') || C == TEXT('>') || C == TEXT('|'))
		{
			return false;
		}
	}
	if (S.IsEmpty())
	{
		return false;
	}

	// The object part of an object path: "/Game/A/BP.BP", "/Game/A/BP.BP:Sub".
	auto StripObjectPart = [](FString& Path)
	{
		int32 Slash = INDEX_NONE;
		Path.FindLastChar(TEXT('/'), Slash);
		const int32 LeafStart = Slash == INDEX_NONE ? 0 : Slash + 1;
		for (int32 i = LeafStart; i < Path.Len(); ++i)
		{
			if (Path[i] == TEXT('.') || Path[i] == TEXT(':'))
			{
				Path.LeftInline(i);
				break;
			}
		}
	};

	if (!S.StartsWith(TEXT("/")))
	{
		// A bare name ("BP_Player") or a relative path: the handler would look
		// it up by leaf name. Only the asset system can say which package that is.
		if (OutLeaf && !S.Contains(TEXT("/")))
		{
			FString Leaf = S;
			StripObjectPart(Leaf);
			*OutLeaf = Leaf;
		}
		return false;
	}

	TArray<FString> Segments;
	S.ParseIntoArray(Segments, TEXT("/"), /*CullEmpty=*/true);
	if (Segments.Num() == 0)
	{
		return false;
	}
	// The object part can only be in the last segment; a dot earlier is either a
	// traversal segment or a folder name with a dot, both refused.
	FString& Last = Segments.Last();
	StripObjectPart(Last);
	for (const FString& Seg : Segments)
	{
		if (Seg.IsEmpty() || Seg == TEXT(".") || Seg == TEXT("..") || Seg.Contains(TEXT(".")) || Seg.Contains(TEXT(":")))
		{
			return false;
		}
	}
	OutPackagePath = TEXT("/") + FString::Join(Segments, TEXT("/"));
	if (OutLeaf)
	{
		*OutLeaf = Segments.Last();
	}
	return true;
}

bool KismetAIAuthority::NormalizeScope(const FString& Raw, FString& OutScope)
{
	const FString Trimmed = Raw.TrimStartAndEnd();
	if (Trimmed == TEXT("/"))
	{
		OutScope = TEXT("/");
		return true;
	}
	return NormalizeAssetReference(Trimmed, OutScope);
}

bool KismetAIAuthority::IsPathWithinScope(const FString& PackagePath, const FString& Scope)
{
	if (PackagePath.IsEmpty() || Scope.IsEmpty())
	{
		return false;
	}
	if (Scope == TEXT("/"))
	{
		return PackagePath.StartsWith(TEXT("/"));
	}
	if (PackagePath.Equals(Scope, ESearchCase::IgnoreCase))
	{
		return true;
	}
	return PackagePath.Len() > Scope.Len()
		&& PackagePath.StartsWith(Scope, ESearchCase::IgnoreCase)
		&& PackagePath[Scope.Len()] == TEXT('/');
}

// ─────────────────────────────────────────────────────────────────────────────
// Operation descriptor
// ─────────────────────────────────────────────────────────────────────────────

void FKismetAIOperation::AddTarget(const FString& Field, const FString& Raw, EKismetAIAuthorityOp Effect)
{
	FKismetAIAuthorityTarget T;
	T.Field = Field;
	T.Raw = Raw;
	T.Effect = Effect;
	KismetAIAuthority::NormalizeAssetReference(Raw, T.PackagePath);
	Targets.Add(MoveTemp(T));
	Ops |= Effect;
}

bool FKismetAIOperation::HasUnresolvedTargets() const
{
	for (const FKismetAIAuthorityTarget& T : Targets)
	{
		if (!T.IsResolved())
		{
			return true;
		}
	}
	return Targets.Num() == 0 && RequiresAuthority();
}

FString FKismetAIOperation::Describe() const
{
	TArray<FString> Parts;
	for (const FKismetAIAuthorityTarget& T : Targets)
	{
		Parts.Add(FString::Printf(TEXT("%s '%s'"), *LexToString(T.Effect), T.IsResolved() ? *T.PackagePath : *T.Raw));
	}
	const FString What = Parts.Num() ? FString::Join(Parts, TEXT(", ")) : FString(TEXT("no assets"));
	return Action.IsEmpty()
		? FString::Printf(TEXT("%s: %s"), *ToolName, *What)
		: FString::Printf(TEXT("%s(%s): %s"), *ToolName, *Action, *What);
}

FString FKismetAIOperation::Fingerprint() const
{
	TArray<FString> Parts;
	for (const FKismetAIAuthorityTarget& T : Targets)
	{
		Parts.Add(FString::Printf(TEXT("%s=%s"), *LexToString(T.Effect),
			*(T.IsResolved() ? T.PackagePath : FString(TEXT("?")) + T.Raw).ToLower()));
	}
	Parts.Sort();
	return FString::Printf(TEXT("%s|%s|%s"), *ToolName, *Action.ToLower(), *FString::Join(Parts, TEXT(";")));
}

FKismetAIOperation KismetAIAuthority::DescribeOperation(const FString& ToolName, const TSharedPtr<FJsonObject>& Body)
{
	FKismetAIOperation Op;
	Op.ToolName = ToolName;

	const FKismetAIToolDefinition* Def = FKismetAIToolRegistry::Find(ToolName);
	if (!Def || Def->IsAllowedInPlanMode())
	{
		// Inspect and AgentState never need authority.
		return Op;
	}

	// One existing asset edited in place. Compiling and saving are edits: they
	// change what is on disk or in the generated class, inside the same asset.
	auto EditField = [&Op, &Body](const TCHAR* Field)
	{
		Op.AddTarget(Field, StringField(Body, Field), EKismetAIAuthorityOp::Edit);
	};

	static const TSet<FString> BlueprintEditTools = {
		TEXT("write_blueprint"), TEXT("compile_blueprint"), TEXT("edit_blueprint"), TEXT("insert_node"),
		TEXT("layout_graph"), TEXT("manage_blueprint"), TEXT("manage_components"), TEXT("manage_state_machine"),
	};
	static const TSet<FString> MaterialEditTools = {
		TEXT("create_material_expression"), TEXT("edit_material"), TEXT("compile_material"), TEXT("manage_material"),
	};
	static const TSet<FString> ReadableBatchTools = {
		TEXT("manage_blueprint"), TEXT("manage_components"), TEXT("manage_state_machine"), TEXT("manage_animation"),
	};

	if (ReadableBatchTools.Contains(ToolName) && BatchIsReadOnly(Body))
	{
		Op.Action = TEXT("get");
		return Op;
	}

	if (BlueprintEditTools.Contains(ToolName))
	{
		EditField(TEXT("blueprint"));
	}
	else if (MaterialEditTools.Contains(ToolName))
	{
		EditField(TEXT("material"));
	}
	else if (ToolName == TEXT("set_material_instance_parameter"))
	{
		EditField(TEXT("instance"));
	}
	else if (ToolName == TEXT("edit_widget"))
	{
		EditField(TEXT("widget"));
	}
	else if (ToolName == TEXT("manage_animation"))
	{
		EditField(TEXT("asset"));
	}
	else if (ToolName == TEXT("create_asset") || ToolName == TEXT("create_material_instance"))
	{
		// A new package at path/name. A name containing "/" or ".." would move
		// the package somewhere else; normalization refuses those, so such a
		// target stays unresolved and is never covered.
		Op.AddTarget(TEXT("path+name"),
			JoinPath(StringField(Body, TEXT("path")), StringField(Body, TEXT("name"))), EKismetAIAuthorityOp::Create);
	}
	else if (ToolName == TEXT("manage_asset"))
	{
		const FString Action = StringField(Body, TEXT("action"));
		const FString AssetPath = StringField(Body, TEXT("asset_path"));
		const FString NewName = StringField(Body, TEXT("new_name"));
		const FString NewPath = StringField(Body, TEXT("new_path"));
		Op.Action = Action;

		FString Normalized;
		const bool bSourceResolved = NormalizeAssetReference(AssetPath, Normalized);

		if (Action == TEXT("delete"))
		{
			Op.AddTarget(TEXT("asset_path"), AssetPath, EKismetAIAuthorityOp::Delete);
		}
		else if (Action == TEXT("rename") || Action == TEXT("move"))
		{
			// The asset leaves its path (and every referencer is rewritten to the
			// new one), so the source side is a deletion.
			Op.AddTarget(TEXT("asset_path"), AssetPath, EKismetAIAuthorityOp::Delete);
			const FString Folder = !NewPath.IsEmpty() ? NewPath : (bSourceResolved ? FolderOf(Normalized) : FString());
			const FString Name = !NewName.IsEmpty() ? NewName : (bSourceResolved ? LeafOf(Normalized) : FString());
			Op.AddTarget(TEXT("new_path+new_name"), (Folder.IsEmpty() || Name.IsEmpty()) ? FString() : JoinPath(Folder, Name),
				EKismetAIAuthorityOp::Create);
		}
		else if (Action == TEXT("duplicate"))
		{
			// The source is only read. With no new_name the engine picks a unique
			// name in the destination folder, so the folder itself must be in scope.
			const FString Folder = !NewPath.IsEmpty() ? NewPath : (bSourceResolved ? FolderOf(Normalized) : FString());
			Op.AddTarget(TEXT("new_path+new_name"),
				Folder.IsEmpty() ? FString() : (NewName.IsEmpty() ? Folder : JoinPath(Folder, NewName)),
				EKismetAIAuthorityOp::Create);
		}
		else
		{
			// "save" and anything unrecognised: an edit of the named asset.
			Op.AddTarget(TEXT("asset_path"), AssetPath, EKismetAIAuthorityOp::Edit);
		}
	}
	else
	{
		// A mutating tool with no rule. Refuse rather than guess: an empty,
		// unresolved target is never covered by any grant.
		Op.AddTarget(TEXT("(unclassified)"), FString(), EKismetAIAuthorityOp::Edit);
	}
	return Op;
}

// ─────────────────────────────────────────────────────────────────────────────
// Grants
// ─────────────────────────────────────────────────────────────────────────────

bool FKismetAIAuthorityGrant::IsActive(double NowSeconds) const
{
	return !bRevoked && !bConsumed && (ExpiresAtSeconds <= 0.0 || NowSeconds < ExpiresAtSeconds);
}

bool FKismetAIAuthorityGrant::Covers(EKismetAIEntryPoint EntryPoint, const FString& InSessionId, const FString& ToolName,
	EKismetAIAuthorityOp Effect, const FString& PackagePath) const
{
	if (!EntryPoints.Contains(EntryPoint))
	{
		return false;
	}
	if (!SessionId.IsEmpty() && SessionId != InSessionId)
	{
		return false;
	}
	if (!BoundToolName.IsEmpty() && BoundToolName != ToolName)
	{
		return false;
	}
	if (!EnumHasAllFlags(Operations, Effect))
	{
		return false;
	}
	for (const FString& Scope : PathScopes)
	{
		if (bExactPaths ? PackagePath.Equals(Scope, ESearchCase::IgnoreCase)
			: KismetAIAuthority::IsPathWithinScope(PackagePath, Scope))
		{
			return true;
		}
	}
	return false;
}

TSharedRef<FJsonObject> FKismetAIAuthorityGrant::ToJson() const
{
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("id"), Id);
	J->SetStringField(TEXT("label"), Label);
	J->SetStringField(TEXT("created_by"), CreatedBy);
	J->SetStringField(TEXT("created_at"), CreatedAt.ToIso8601());
	TArray<FString> EP;
	for (EKismetAIEntryPoint E : EntryPoints)
	{
		EP.Add(LexToString(E));
	}
	J->SetArrayField(TEXT("entry_points"), StringArray(EP));
	if (!SessionId.IsEmpty())
	{
		J->SetStringField(TEXT("session_id"), SessionId);
	}
	J->SetArrayField(TEXT("scope"), StringArray(PathScopes));
	J->SetBoolField(TEXT("exact_paths"), bExactPaths);
	TArray<FString> OpsArr;
	LexToString(Operations).ParseIntoArray(OpsArr, TEXT(","));
	J->SetArrayField(TEXT("operations"), StringArray(OpsArr));
	J->SetStringField(TEXT("lifetime"), LexToString(Lifetime));
	if (ExpiresAtSeconds > 0.0)
	{
		J->SetNumberField(TEXT("expires_in_seconds"), FMath::Max(0.0, ExpiresAtSeconds - FPlatformTime::Seconds()));
	}
	if (!BoundToolName.IsEmpty())
	{
		J->SetStringField(TEXT("tool"), BoundToolName);
	}
	if (!ApprovalRequestId.IsEmpty())
	{
		J->SetStringField(TEXT("approval_request_id"), ApprovalRequestId);
	}
	J->SetBoolField(TEXT("active"), IsActive(FPlatformTime::Seconds()));
	J->SetBoolField(TEXT("revoked"), bRevoked);
	J->SetBoolField(TEXT("consumed"), bConsumed);
	if (!RevokedBy.IsEmpty())
	{
		J->SetStringField(TEXT("revoked_by"), RevokedBy);
	}
	return J;
}

bool FKismetAIAuthorityGrant::FromJson(const FJsonObject& Json, FKismetAIAuthorityGrant& Out, FString& OutError)
{
	auto ReadStrings = [&Json](const TCHAR* Field) -> TArray<FString>
	{
		TArray<FString> Values;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		FString Single;
		if (Json.TryGetArrayField(Field, Arr) && Arr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString S;
				if (V.IsValid() && V->TryGetString(S))
				{
					Values.Add(S);
				}
				else
				{
					Values.Add(FString());  // a non-string entry fails validation below
				}
			}
		}
		else if (Json.TryGetStringField(Field, Single))
		{
			Values.Add(Single);
		}
		return Values;
	};

	FKismetAIAuthorityGrant G;

	for (const FString& Name : ReadStrings(TEXT("entry_points")))
	{
		EKismetAIEntryPoint E;
		if (!KismetAIAuthority::ParseEntryPoint(Name, E))
		{
			OutError = FString::Printf(TEXT("unknown entry point '%s' (expected rest, mcp, http_agent_run, panel)"), *Name);
			return false;
		}
		G.EntryPoints.AddUnique(E);
	}

	if (!KismetAIAuthority::ParseOps(ReadStrings(TEXT("operations")), G.Operations, OutError))
	{
		return false;
	}

	for (const FString& Raw : ReadStrings(TEXT("scope")))
	{
		FString Scope;
		if (!KismetAIAuthority::NormalizeScope(Raw, Scope))
		{
			OutError = FString::Printf(TEXT("scope '%s' is not an absolute asset path such as /Game/Folder"), *Raw);
			return false;
		}
		G.PathScopes.AddUnique(Scope);
	}

	Json.TryGetStringField(TEXT("label"), G.Label);
	Json.TryGetStringField(TEXT("session_id"), G.SessionId);

	FString Lifetime;
	if (Json.TryGetStringField(TEXT("lifetime"), Lifetime) && !Lifetime.IsEmpty())
	{
		if (Lifetime == TEXT("once"))                { G.Lifetime = EKismetAIGrantLifetime::Once; }
		else if (Lifetime == TEXT("session"))        { G.Lifetime = EKismetAIGrantLifetime::Session; }
		else if (Lifetime == TEXT("until_revoked"))  { G.Lifetime = EKismetAIGrantLifetime::UntilRevoked; }
		else
		{
			OutError = FString::Printf(TEXT("unknown lifetime '%s' (expected once, session, until_revoked)"), *Lifetime);
			return false;
		}
	}

	double ExpiresIn = 0.0;
	if (Json.TryGetNumberField(TEXT("expires_in_seconds"), ExpiresIn))
	{
		if (ExpiresIn <= 0.0)
		{
			OutError = TEXT("expires_in_seconds must be positive");
			return false;
		}
		G.ExpiresAtSeconds = FPlatformTime::Seconds() + ExpiresIn;
	}

	Out = MoveTemp(G);
	return true;
}

TSharedRef<FJsonObject> FKismetAIAuthorityDecision::ToJson() const
{
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetStringField(TEXT("id"), Id);
	J->SetStringField(TEXT("time"), Time.ToIso8601());
	J->SetStringField(TEXT("phase"), Phase);
	J->SetBoolField(TEXT("allowed"), bAllowed);
	if (!ToolName.IsEmpty()) J->SetStringField(TEXT("tool"), ToolName);
	if (!Operation.IsEmpty()) J->SetStringField(TEXT("operation"), Operation);
	J->SetStringField(TEXT("entry_point"), LexToString(EntryPoint));
	if (!Principal.IsEmpty()) J->SetStringField(TEXT("principal"), Principal);
	if (!SessionId.IsEmpty()) J->SetStringField(TEXT("session_id"), SessionId);
	if (GrantIds.Num()) J->SetArrayField(TEXT("grant_ids"), StringArray(GrantIds));
	if (!Code.IsEmpty()) J->SetStringField(TEXT("code"), Code);
	if (!Reason.IsEmpty()) J->SetStringField(TEXT("reason"), Reason);
	return J;
}

// ─────────────────────────────────────────────────────────────────────────────
// Store
// ─────────────────────────────────────────────────────────────────────────────

FString FKismetAIAuthorityStore::AddGrant(FKismetAIAuthorityGrant Grant, FString& OutError)
{
	FScopeLock Lock(&Mutex);
	return AddGrantLocked(MoveTemp(Grant), OutError);
}

FString FKismetAIAuthorityStore::AddGrantLocked(FKismetAIAuthorityGrant Grant, FString& OutError)
{
	if (Grant.EntryPoints.Num() == 0 || Grant.EntryPoints.Contains(EKismetAIEntryPoint::Unspecified))
	{
		OutError = TEXT("a grant must name the entry points it applies to");
		return FString();
	}
	if (Grant.Operations == EKismetAIAuthorityOp::None)
	{
		OutError = TEXT("a grant must allow at least one operation");
		return FString();
	}
	if (Grant.PathScopes.Num() == 0)
	{
		OutError = TEXT("a grant must name at least one scope");
		return FString();
	}
	for (FString& Scope : Grant.PathScopes)
	{
		FString Normalized;
		if (!KismetAIAuthority::NormalizeScope(Scope, Normalized))
		{
			OutError = FString::Printf(TEXT("scope '%s' is not an absolute asset path"), *Scope);
			return FString();
		}
		Scope = Normalized;
	}
	if (Grant.Lifetime == EKismetAIGrantLifetime::Session && Grant.SessionId.IsEmpty())
	{
		OutError = TEXT("a session grant needs a session id");
		return FString();
	}

	Grant.Id = FString::Printf(TEXT("grant-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
	Grant.CreatedAt = FDateTime::UtcNow();
	Grant.bRevoked = false;
	Grant.bConsumed = false;

	FKismetAIAuthorityDecision D;
	D.Phase = TEXT("grant");
	D.bAllowed = true;
	D.Principal = Grant.CreatedBy;
	D.SessionId = Grant.SessionId;
	D.GrantIds = { Grant.Id };
	D.Reason = FString::Printf(TEXT("%s to %s for %s in [%s] (%s%s)"),
		*LexToString(Grant.Operations), *EntryPointsToString(Grant.EntryPoints),
		Grant.SessionId.IsEmpty() ? TEXT("any session") : *FString::Printf(TEXT("session %s"), *Grant.SessionId),
		*FString::Join(Grant.PathScopes, TEXT(", ")), LexToString(Grant.Lifetime),
		Grant.Label.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", '%s'"), *Grant.Label));
	RecordDecisionLocked(MoveTemp(D));

	const FString Id = Grant.Id;
	Grants.Add(MoveTemp(Grant));
	return Id;
}

int32 FKismetAIAuthorityStore::Revoke(const FString& IdOrLabel, const FString& RevokedBy)
{
	FScopeLock Lock(&Mutex);
	TArray<FString> Revoked;
	for (FKismetAIAuthorityGrant& G : Grants)
	{
		if (!G.bRevoked && (G.Id == IdOrLabel || (!G.Label.IsEmpty() && G.Label == IdOrLabel)))
		{
			G.bRevoked = true;
			G.RevokedBy = RevokedBy;
			Revoked.Add(G.Id);
		}
	}
	if (Revoked.Num())
	{
		FKismetAIAuthorityDecision D;
		D.Phase = TEXT("revoke");
		D.bAllowed = true;
		D.Principal = RevokedBy;
		D.GrantIds = Revoked;
		RecordDecisionLocked(MoveTemp(D));
	}
	return Revoked.Num();
}

int32 FKismetAIAuthorityStore::RevokeAll(const FString& RevokedBy)
{
	FScopeLock Lock(&Mutex);
	TArray<FString> Revoked;
	for (FKismetAIAuthorityGrant& G : Grants)
	{
		if (!G.bRevoked)
		{
			G.bRevoked = true;
			G.RevokedBy = RevokedBy;
			Revoked.Add(G.Id);
		}
	}
	if (Revoked.Num())
	{
		FKismetAIAuthorityDecision D;
		D.Phase = TEXT("revoke");
		D.bAllowed = true;
		D.Principal = RevokedBy;
		D.GrantIds = Revoked;
		RecordDecisionLocked(MoveTemp(D));
	}
	return Revoked.Num();
}

TArray<FKismetAIAuthorityGrant> FKismetAIAuthorityStore::GetGrants(bool bIncludeInactive) const
{
	FScopeLock Lock(&Mutex);
	const double Now = FPlatformTime::Seconds();
	TArray<FKismetAIAuthorityGrant> Out;
	for (const FKismetAIAuthorityGrant& G : Grants)
	{
		if (bIncludeInactive || G.IsActive(Now))
		{
			Out.Add(G);
		}
	}
	return Out;
}

TOptional<FKismetAIAuthorityGrant> FKismetAIAuthorityStore::FindGrant(const FString& Id) const
{
	FScopeLock Lock(&Mutex);
	for (const FKismetAIAuthorityGrant& G : Grants)
	{
		if (G.Id == Id)
		{
			return G;
		}
	}
	return {};
}

FKismetAIAuthorityCheck FKismetAIAuthorityStore::Check(const FKismetAIOperation& Operation,
	const FKismetAICallerContext& Caller, const FString& SessionId, bool bConsume)
{
	FKismetAIAuthorityCheck Result;
	if (!Operation.RequiresAuthority())
	{
		Result.bAllowed = true;
		return Result;
	}

	FScopeLock Lock(&Mutex);
	const double Now = FPlatformTime::Seconds();
	TArray<int32> Used;

	if (Operation.Targets.Num() == 0)
	{
		Result.Reason = FString::Printf(TEXT("'%s' names no asset KismetAI can scope"), *Operation.ToolName);
		return Result;
	}

	for (const FKismetAIAuthorityTarget& T : Operation.Targets)
	{
		if (!T.IsResolved())
		{
			Result.Reason = T.Raw.IsEmpty()
				? FString::Printf(TEXT("'%s' could not be matched to an asset path (field '%s')"), *Operation.ToolName, *T.Field)
				: FString::Printf(TEXT("'%s' (field '%s') is not a full asset path KismetAI can scope; use an absolute path such as /Game/Folder/Asset"), *T.Raw, *T.Field);
			return Result;
		}

		// Standing grants first, so a broad grant does not burn a single-use
		// approval meant for something else.
		int32 Found = INDEX_NONE;
		for (int32 Pass = 0; Pass < 2 && Found == INDEX_NONE; ++Pass)
		{
			for (int32 i = 0; i < Grants.Num(); ++i)
			{
				const FKismetAIAuthorityGrant& G = Grants[i];
				const bool bOnce = G.Lifetime == EKismetAIGrantLifetime::Once;
				if ((Pass == 0) == bOnce || !G.IsActive(Now))
				{
					continue;
				}
				if (G.Covers(Caller.EntryPoint, SessionId, Operation.ToolName, T.Effect, T.PackagePath))
				{
					Found = i;
					break;
				}
			}
		}

		if (Found == INDEX_NONE)
		{
			// Say which part is missing: no grant for this caller at all, a grant
			// without this operation class, or a scope that does not reach.
			bool bAnyForCaller = false;
			bool bAnyWithOp = false;
			for (const FKismetAIAuthorityGrant& G : Grants)
			{
				if (!G.IsActive(Now) || !G.EntryPoints.Contains(Caller.EntryPoint)
					|| (!G.SessionId.IsEmpty() && G.SessionId != SessionId))
				{
					continue;
				}
				bAnyForCaller = true;
				bAnyWithOp |= EnumHasAllFlags(G.Operations, T.Effect);
			}
			const FString Target = FString::Printf(TEXT("%s '%s'"), *LexToString(T.Effect), *T.PackagePath);
			if (!bAnyForCaller)
			{
				Result.Reason = FString::Printf(TEXT("no grant gives %s callers authority to %s"), LexToString(Caller.EntryPoint), *Target);
			}
			else if (!bAnyWithOp)
			{
				Result.Reason = FString::Printf(TEXT("%s callers hold no '%s' grant, so they may not %s"),
					LexToString(Caller.EntryPoint), *LexToString(T.Effect), *Target);
			}
			else
			{
				Result.Reason = FString::Printf(TEXT("%s is outside every scope granted to %s callers"), *Target, LexToString(Caller.EntryPoint));
			}
			return Result;
		}
		Used.AddUnique(Found);
	}

	Result.bAllowed = true;
	for (int32 i : Used)
	{
		Result.GrantIds.Add(Grants[i].Id);
		if (bConsume && Grants[i].Lifetime == EKismetAIGrantLifetime::Once)
		{
			Grants[i].bConsumed = true;
		}
	}
	return Result;
}

FKismetAIApprovalRequest FKismetAIAuthorityStore::OpenApprovalRequest(const FKismetAIOperation& Operation,
	const FKismetAICallerContext& Caller, const FString& SessionId, double TtlSeconds)
{
	FKismetAIApprovalRequest R;
	R.RequestId = FString::Printf(TEXT("approval-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower());
	R.Fingerprint = Operation.Fingerprint();
	R.Operation = Operation;
	R.Caller = Caller;
	R.SessionId = SessionId;
	R.ExpiresAtSeconds = FPlatformTime::Seconds() + FMath::Max(1.0, TtlSeconds);
	R.Prompt = FString::Printf(TEXT("Allow KismetAI to %s?"), *Operation.Describe());

	FScopeLock Lock(&Mutex);
	// Drop long-dead requests so the map cannot grow without bound.
	const double Now = FPlatformTime::Seconds();
	for (auto It = Approvals.CreateIterator(); It; ++It)
	{
		if (It->Value.Request.ExpiresAtSeconds + 3600.0 < Now)
		{
			It.RemoveCurrent();
		}
	}
	FPendingApproval& P = Approvals.Add(R.RequestId);
	P.Request = R;
	return R;
}

EKismetAIApprovalResolution FKismetAIAuthorityStore::ResolveApproval(const FKismetAIApprovalAnswer& Answer,
	const FKismetAIOperation& Operation, const FKismetAICallerContext& Caller, const FString& SessionId, FString* OutGrantId)
{
	FScopeLock Lock(&Mutex);

	auto Record = [this, &Answer, &Operation, &Caller, &SessionId](EKismetAIApprovalResolution Res, const FString& GrantId)
	{
		FKismetAIAuthorityDecision D;
		D.Phase = TEXT("approval");
		D.ToolName = Operation.ToolName;
		D.Operation = Operation.Describe();
		D.EntryPoint = Caller.EntryPoint;
		D.Principal = Answer.AnsweredBy.IsEmpty() ? Caller.Principal : Answer.AnsweredBy;
		D.SessionId = SessionId;
		D.bAllowed = Res == EKismetAIApprovalResolution::Granted;
		if (!GrantId.IsEmpty())
		{
			D.GrantIds = { GrantId };
		}
		D.Code = LexToString(Res);
		D.Reason = FString::Printf(TEXT("request %s"), *Answer.RequestId);
		RecordDecisionLocked(MoveTemp(D));
		return Res;
	};

	FPendingApproval* Pending = Approvals.Find(Answer.RequestId);
	if (!Pending)
	{
		return Record(EKismetAIApprovalResolution::UnknownRequest, FString());
	}
	if (Pending->bAnswered)
	{
		return Record(EKismetAIApprovalResolution::AlreadyAnswered, FString());
	}
	// The first answer settles the request whatever it says.
	Pending->bAnswered = true;

	const FKismetAIApprovalRequest& Req = Pending->Request;
	if (Req.Fingerprint != Operation.Fingerprint() || Req.Caller.EntryPoint != Caller.EntryPoint
		|| Req.Caller.Principal != Caller.Principal || Req.SessionId != SessionId)
	{
		return Record(EKismetAIApprovalResolution::Mismatch, FString());
	}
	if (FPlatformTime::Seconds() >= Req.ExpiresAtSeconds)
	{
		return Record(EKismetAIApprovalResolution::Expired, FString());
	}
	if (Answer.Decision == EKismetAIApprovalDecision::Deny)
	{
		return Record(EKismetAIApprovalResolution::Declined, FString());
	}

	// Exactly the approved assets and operation classes, for this caller.
	FKismetAIAuthorityGrant G;
	G.Label = TEXT("approval");
	G.CreatedBy = FString::Printf(TEXT("approval:%s"), Answer.AnsweredBy.IsEmpty() ? TEXT("user") : *Answer.AnsweredBy);
	G.EntryPoints = { Caller.EntryPoint };
	G.SessionId = SessionId;
	G.bExactPaths = true;
	G.ApprovalRequestId = Req.RequestId;
	for (const FKismetAIAuthorityTarget& T : Req.Operation.Targets)
	{
		if (T.IsResolved())
		{
			G.PathScopes.AddUnique(T.PackagePath);
		}
		G.Operations |= T.Effect;
	}
	if (Answer.Decision == EKismetAIApprovalDecision::AllowSession && !SessionId.IsEmpty())
	{
		G.Lifetime = EKismetAIGrantLifetime::Session;
		G.ExpiresAtSeconds = FPlatformTime::Seconds() + SessionApprovalTtlSeconds;
	}
	else
	{
		G.Lifetime = EKismetAIGrantLifetime::Once;
		G.BoundToolName = Req.Operation.ToolName;
		G.ExpiresAtSeconds = FPlatformTime::Seconds() + OnceApprovalTtlSeconds;
	}

	FString Error;
	const FString GrantId = AddGrantLocked(MoveTemp(G), Error);
	if (GrantId.IsEmpty())
	{
		return Record(EKismetAIApprovalResolution::Mismatch, FString());
	}
	if (OutGrantId)
	{
		*OutGrantId = GrantId;
	}
	return Record(EKismetAIApprovalResolution::Granted, GrantId);
}

FString FKismetAIAuthorityStore::RecordDecision(FKismetAIAuthorityDecision Decision)
{
	FScopeLock Lock(&Mutex);
	return RecordDecisionLocked(MoveTemp(Decision));
}

FString FKismetAIAuthorityStore::RecordDecisionLocked(FKismetAIAuthorityDecision Decision)
{
	Decision.Id = FString::Printf(TEXT("authz-%lld"), ++DecisionSeq);
	Decision.Time = FDateTime::UtcNow();

	const FString Line = FString::Printf(TEXT("[%s] %s %s %s caller=%s/%s session=%s grants=[%s] %s %s"),
		*Decision.Id, *Decision.Phase, Decision.bAllowed ? TEXT("allow") : TEXT("deny"),
		Decision.Operation.IsEmpty() ? *Decision.ToolName : *Decision.Operation,
		LexToString(Decision.EntryPoint), *Decision.Principal, *Decision.SessionId,
		*FString::Join(Decision.GrantIds, TEXT(",")), *Decision.Code, *Decision.Reason);
	if (Decision.bAllowed)
	{
		UE_LOG(LogKismetAIAuthority, Log, TEXT("%s"), *Line);
	}
	else
	{
		UE_LOG(LogKismetAIAuthority, Warning, TEXT("%s"), *Line);
	}

	const FString Id = Decision.Id;
	Decisions.Add(MoveTemp(Decision));
	if (Decisions.Num() > MaxRecordedDecisions)
	{
		Decisions.RemoveAt(0, Decisions.Num() - MaxRecordedDecisions);
	}
	return Id;
}

TArray<FKismetAIAuthorityDecision> FKismetAIAuthorityStore::GetRecentDecisions(int32 Max) const
{
	FScopeLock Lock(&Mutex);
	const int32 Start = FMath::Max(0, Decisions.Num() - FMath::Max(0, Max));
	TArray<FKismetAIAuthorityDecision> Out;
	for (int32 i = Start; i < Decisions.Num(); ++i)
	{
		Out.Add(Decisions[i]);
	}
	return Out;
}

TSharedRef<FJsonObject> FKismetAIAuthorityStore::ToJson(int32 MaxDecisions) const
{
	TArray<TSharedPtr<FJsonValue>> GrantArr;
	for (const FKismetAIAuthorityGrant& G : GetGrants(/*bIncludeInactive=*/false))
	{
		GrantArr.Add(MakeShared<FJsonValueObject>(G.ToJson()));
	}
	TArray<TSharedPtr<FJsonValue>> DecisionArr;
	for (const FKismetAIAuthorityDecision& D : GetRecentDecisions(MaxDecisions))
	{
		DecisionArr.Add(MakeShared<FJsonValueObject>(D.ToJson()));
	}
	TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetArrayField(TEXT("grants"), GrantArr);
	J->SetArrayField(TEXT("decisions"), DecisionArr);
	return J;
}
