// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0
//
// Scoped authority for editor mutations.
//
// Before this, every verified caller could mutate any asset it could name: the
// shipping policy allowed every mutation, and its only prompt degraded to
// "allow" when nobody could be asked. A token proves who is calling; it does not
// say what that caller may change. This file is that second half:
//
//   - an operation descriptor: which registered tool call does what to which
//     normalized asset path (edit an existing asset, create a new package,
//     delete or move one away);
//   - grants: explicit, recorded authority for one caller kind (entry point),
//     optionally one session, over component-aware path scopes and a set of
//     operation classes, with a lifetime (single use, session, until revoked);
//   - approvals: an interactive "allow?" bound to one concrete normalized
//     operation by a unique request id; a stale, duplicated, mismatched or
//     expired answer cannot create authority;
//   - an audit trail: every decision gets an id that the refusal message and
//     GET /api/v1/authority both carry.
//
// What it is not: a sandbox. In-process editor code keeps the editor's
// authority, and a holder of the REST token can create grants for itself. The
// value is that unattended authority is explicit, bounded, revocable and
// attributable — never implied by an unanswerable prompt or by history.
//
// The decision that counts is made on the game thread immediately before the
// handler (IKismetAIToolPolicy::EvaluateAdmission), so a grant revoked while a
// call is queued stops it.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/CriticalSection.h"
#include "Misc/DateTime.h"
#include "KismetAIToolExecution.h"
#include "HAL/PlatformTime.h"

// What an operation does to one asset. A bitmask so a grant can carry several.
//
// Edit is ordinary work inside an existing asset: graph edits, pin defaults,
// node deletion inside a graph, components, variables, compiling, saving. Users
// grant it once for a scope; there is no prompt per pin. Create makes a new
// package. Delete removes an asset from its path — delete, and the source side
// of rename and move (which also rewrites referencers) — and is never implied
// by Edit or Create.
enum class EKismetAIAuthorityOp : uint8
{
	None   = 0,
	Edit   = 1 << 0,
	Create = 1 << 1,
	Delete = 1 << 2,
};
ENUM_CLASS_FLAGS(EKismetAIAuthorityOp)

// "edit,create" — stable lowercase names, in a fixed order.
KISMETAIRUNTIME_API FString LexToString(EKismetAIAuthorityOp Ops);

// One asset an operation touches.
struct KISMETAIRUNTIME_API FKismetAIAuthorityTarget
{
	// The request field it came from ("blueprint", "asset_path", ...).
	FString Field;

	// As the request spelled it (path/name joined for creates).
	FString Raw;

	// Normalized long package path ("/Game/Folder/BP_Name"). Empty while the
	// reference is not an absolute path (a bare asset name the handler would
	// look up by leaf name) or is malformed (traversal, empty segments).
	FString PackagePath;

	EKismetAIAuthorityOp Effect = EKismetAIAuthorityOp::None;

	bool IsResolved() const { return !PackagePath.IsEmpty(); }
};

// What one registered tool call would do, derived from the body the handler
// reads. Read-only calls have Ops == None and need no authority.
struct KISMETAIRUNTIME_API FKismetAIOperation
{
	FString ToolName;

	// The sub-action when it changes the class (manage_asset's "delete").
	FString Action;

	// Union of the targets' effects.
	EKismetAIAuthorityOp Ops = EKismetAIAuthorityOp::None;

	TArray<FKismetAIAuthorityTarget> Targets;

	bool RequiresAuthority() const { return Ops != EKismetAIAuthorityOp::None; }
	bool HasUnresolvedTargets() const;

	// "delete /Game/A/BP_X, create /Game/B/BP_Y".
	FString Describe() const;

	// Identity of the normalized request: tool, action and every (effect,
	// lowercased path). An approval is bound to this.
	FString Fingerprint() const;

	void AddTarget(const FString& Field, const FString& Raw, EKismetAIAuthorityOp Effect);
};

namespace KismetAIAuthority
{
	// Normalizes an asset reference to a long package path without touching the
	// asset system: strips a Class'...' wrapper and quotes, turns backslashes
	// into slashes, collapses repeated slashes, drops the object part
	// (".Name", ":Sub") and a trailing slash. Returns false — with OutLeaf set to
	// the bare name when there is one — for anything that is not an absolute
	// path or contains "." / ".." segments or control characters.
	KISMETAIRUNTIME_API bool NormalizeAssetReference(const FString& Raw, FString& OutPackagePath, FString* OutLeaf = nullptr);

	// A grant scope: a normalized path ("/Game/Sandbox") or "/" for every mount.
	KISMETAIRUNTIME_API bool NormalizeScope(const FString& Raw, FString& OutScope);

	// Component-aware and case-insensitive: "/Game/A" covers "/Game/A" and
	// "/Game/A/B", not "/Game/AB".
	KISMETAIRUNTIME_API bool IsPathWithinScope(const FString& PackagePath, const FString& Scope);

	// Classifies a call to a registered tool from the body its handler reads.
	// Every tool whose capability is not Inspect/AgentState has a rule here;
	// KismetAI.ScopedAuthority.EveryMutatingToolClassified asserts it.
	KISMETAIRUNTIME_API FKismetAIOperation DescribeOperation(const FString& ToolName, const TSharedPtr<FJsonObject>& Body);

	KISMETAIRUNTIME_API bool ParseEntryPoint(const FString& Name, EKismetAIEntryPoint& Out);
	KISMETAIRUNTIME_API bool ParseOps(const TArray<FString>& Names, EKismetAIAuthorityOp& Out, FString& OutError);
}

enum class EKismetAIGrantLifetime : uint8
{
	// Consumed by the first operation it authorizes at admission.
	Once,
	// Bound to one session id; also revoked by whoever opened it (an HTTP agent
	// run revokes its own grant when the run ends).
	Session,
	// Standing until revoked or ExpiresAt.
	UntilRevoked,
};

KISMETAIRUNTIME_API const TCHAR* LexToString(EKismetAIGrantLifetime Lifetime);

struct KISMETAIRUNTIME_API FKismetAIAuthorityGrant
{
	FString Id;
	FString Label;

	// Who created it, as that transport verified them ("editor-user",
	// "x-kismet-token", "approval:<request id>").
	FString CreatedBy;
	FDateTime CreatedAt;

	// Caller kinds this grant applies to. Required: a grant never applies to
	// "whoever happens to call".
	TArray<EKismetAIEntryPoint> EntryPoints;

	// Empty: any session (and calls with none, such as REST and MCP).
	FString SessionId;

	// Normalized scopes. With bExactPaths they are exact packages (approvals).
	TArray<FString> PathScopes;
	bool bExactPaths = false;

	EKismetAIAuthorityOp Operations = EKismetAIAuthorityOp::None;

	EKismetAIGrantLifetime Lifetime = EKismetAIGrantLifetime::UntilRevoked;

	// FPlatformTime::Seconds() deadline; 0 = none.
	double ExpiresAtSeconds = 0.0;

	// Set on approval grants: only this tool may use it.
	FString BoundToolName;
	FString ApprovalRequestId;

	bool bRevoked = false;
	bool bConsumed = false;
	FString RevokedBy;

	bool IsActive(double NowSeconds) const;
	bool Covers(EKismetAIEntryPoint EntryPoint, const FString& InSessionId, const FString& ToolName,
		EKismetAIAuthorityOp Effect, const FString& PackagePath) const;
	TSharedRef<FJsonObject> ToJson() const;

	// Parses the JSON form REST and /agent/run accept:
	//   { "entry_points": ["mcp"], "operations": ["edit","create"],
	//     "scope": ["/Game/Sandbox"], "label": "...", "lifetime": "until_revoked"|"once",
	//     "expires_in_seconds": 3600 }
	// "scope" may be a string or an array. Entry points, operations and at least
	// one valid scope are required.
	static bool FromJson(const FJsonObject& Json, FKismetAIAuthorityGrant& Out, FString& OutError);
};

// An interactive request for authority the caller does not hold, bound to one
// normalized operation.
struct KISMETAIRUNTIME_API FKismetAIApprovalRequest
{
	FString RequestId;
	FString Fingerprint;
	FKismetAIOperation Operation;
	FKismetAICallerContext Caller;
	FString SessionId;
	double ExpiresAtSeconds = 0.0;

	// What the user is asked.
	FString Prompt;
};

enum class EKismetAIApprovalDecision : uint8
{
	Deny,
	// A single-use grant for exactly this operation.
	AllowOnce,
	// A session grant for these exact assets and operation classes.
	AllowSession,
};

struct KISMETAIRUNTIME_API FKismetAIApprovalAnswer
{
	// Must equal the request being resolved.
	FString RequestId;
	EKismetAIApprovalDecision Decision = EKismetAIApprovalDecision::Deny;
	FString AnsweredBy;
};

enum class EKismetAIApprovalResolution : uint8
{
	Granted,
	Declined,
	UnknownRequest,
	AlreadyAnswered,
	Mismatch,
	Expired,
};

KISMETAIRUNTIME_API const TCHAR* LexToString(EKismetAIApprovalResolution Resolution);

// One recorded decision.
struct KISMETAIRUNTIME_API FKismetAIAuthorityDecision
{
	FString Id;
	FDateTime Time;
	// "pre_dispatch", "admission", "approval", "grant", "revoke".
	FString Phase;
	FString ToolName;
	FString Operation;
	EKismetAIEntryPoint EntryPoint = EKismetAIEntryPoint::Unspecified;
	FString Principal;
	FString SessionId;
	bool bAllowed = false;
	TArray<FString> GrantIds;
	FString Code;
	FString Reason;

	TSharedRef<FJsonObject> ToJson() const;
};

struct KISMETAIRUNTIME_API FKismetAIAuthorityCheck
{
	bool bAllowed = false;
	TArray<FString> GrantIds;
	// Why not, naming the first target nothing covers.
	FString Reason;
};

// Thread-safe. One per module; the policy and every transport share it.
class KISMETAIRUNTIME_API FKismetAIAuthorityStore
{
public:
	// Validates and adds a grant; returns its id, or empty with OutError.
	FString AddGrant(FKismetAIAuthorityGrant Grant, FString& OutError);

	// By id or label. Returns how many were revoked.
	int32 Revoke(const FString& IdOrLabel, const FString& RevokedBy);
	int32 RevokeAll(const FString& RevokedBy);

	TArray<FKismetAIAuthorityGrant> GetGrants(bool bIncludeInactive = false) const;
	TOptional<FKismetAIAuthorityGrant> FindGrant(const FString& Id) const;

	// Does a grant cover every target? With bConsume, single-use grants that
	// authorized it are spent. Unresolved targets are never covered.
	FKismetAIAuthorityCheck Check(const FKismetAIOperation& Operation, const FKismetAICallerContext& Caller,
		const FString& SessionId, bool bConsume);

	// Opens a request bound to Operation's fingerprint.
	FKismetAIApprovalRequest OpenApprovalRequest(const FKismetAIOperation& Operation,
		const FKismetAICallerContext& Caller, const FString& SessionId, double TtlSeconds = 600.0);

	// The first answer to a request is the only one; later answers, answers for
	// another request or for a different operation, and expired requests are
	// refused and create nothing. On Granted, OutGrantId names the new grant.
	EKismetAIApprovalResolution ResolveApproval(const FKismetAIApprovalAnswer& Answer,
		const FKismetAIOperation& Operation, const FKismetAICallerContext& Caller, const FString& SessionId,
		FString* OutGrantId = nullptr);

	// Records (and logs) a decision; returns its id.
	FString RecordDecision(FKismetAIAuthorityDecision Decision);
	TArray<FKismetAIAuthorityDecision> GetRecentDecisions(int32 Max = 100) const;

	TSharedRef<FJsonObject> ToJson(int32 MaxDecisions = 100) const;

private:
	struct FPendingApproval
	{
		FKismetAIApprovalRequest Request;
		bool bAnswered = false;
	};

	FString AddGrantLocked(FKismetAIAuthorityGrant Grant, FString& OutError);
	FString RecordDecisionLocked(FKismetAIAuthorityDecision Decision);

	mutable FCriticalSection Mutex;
	TArray<FKismetAIAuthorityGrant> Grants;
	TMap<FString, FPendingApproval> Approvals;
	TArray<FKismetAIAuthorityDecision> Decisions;
	int64 DecisionSeq = 0;
};
