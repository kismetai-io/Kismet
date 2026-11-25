// Copyright (c) 2026 Artem Tarasov
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "KismetAICancellation.h"
#include "KismetAIAgentTypes.h"
#include "KismetAIToolExecutor.h"
#include "KismetAIToolExecution.h"
#include "KismetAIToolPolicy.h"
#include "KismetAIContextProjection.h"
#include "KismetAIRequestBudget.h"
#include "KismetAIRunBudget.h"

KISMETAIRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogKismetAIAgent, Log, All);

// Callback for streaming text deltas from the Anthropic API
using FOnTextDelta = TFunction<void(const FString& DeltaText)>;

// Complete thinking block (fires before text starts streaming)
using FOnThinkingComplete = TFunction<void(const FString& ThinkingText)>;

// Each incremental thinking chunk (fires live during streaming)
using FOnThinkingDelta = TFunction<void(const FString& ThinkingDelta)>;

// Forward declarations
class ILLMProvider;
class IKismetAIToolInvoker;
class IKismetAIModelClient;
struct FModelCallRequest;
struct FModelCallbacks;
struct FSessionSnapshot;
struct FKismetAITurnContext;

// Outcome classification for a single model call, any provider.
enum class EModelCallResult : uint8
{
	Success,              // 200 OK, response parsed
	RetryableError,       // 429, 529, 408, 409, 5xx -- can retry
	NetworkUnreachable,   // curl exit 6/7 — DNS or TCP connect failed; fast-fail
	PromptTooLong,        // error.type == "invalid_request_error" with prompt_too_long
	MaxTokensOverflow,    // "input length and max_tokens exceed context limit"
	FatalError            // 400, 401, 403, other non-retryable errors
};

// Holds the outcome of a single model call, any provider.
struct FModelCallOutcome
{
	EModelCallResult Result = EModelCallResult::FatalError;
	FAgentMessage AssistantMessage;
	TSharedPtr<FJsonObject> Response;   // Response metadata or failed transport evidence, including partial usage
	TArray<TSharedPtr<FJsonObject>> UsageReports; // One report per attempted request, including retries
	int32 RequestCount = 0;
	int32 AttemptCount = 0;             // Invoked attempts, including local failures
	int32 EffectiveMaxTokens = 0;       // Last attempted output limit; zero before any attempt
	bool bUsageIncomplete = false;
	int32 HttpStatusCode = 0;
	FString ErrorType;                  // "overloaded_error", "rate_limit_error", etc.
	FString ErrorMessage;
	double RetryAfterSeconds = -1.0;    // From retry-after header, -1 if absent
	int32 InputTokens = 0;             // From error body, for MaxTokensOverflow
	int32 ContextLimit = 0;            // From error body, for MaxTokensOverflow
};

// User-facing error copy + actionable suggestions, derived from FModelCallOutcome
// by FKismetAIAgent::BuildUserFacingError. Wire-level error_type strings (SSE error_code,
// telemetry payload) are NOT remapped here — only the human-readable presentation.
struct FUserFacingError
{
	FString Message;
	TArray<FString> Suggestions;
};

// ── Context compaction ───────────────────────────────────────────────
// Compaction is a transaction over the history the agent holds in memory. A
// candidate projection is built from the unmodified current one, and replaces
// it only when every check below passed. Any other outcome leaves the caller's
// array, and every object in it, exactly as it was. The session journal is
// never touched: it stays the canonical record.
enum class ECompactionStatus : uint8
{
	// The candidate replaced the projection.
	Committed,
	// Too short to compact; nothing was attempted.
	NothingToCompact,
	// An attempt was made and rejected; the projection is unchanged.
	Failed,
};

struct KISMETAIRUNTIME_API FKismetAICompactionResult
{
	ECompactionStatus Status = ECompactionStatus::NothingToCompact;

	// Why an attempt failed, as a stable code:
	//   summary_unavailable    no compact prompt is loaded
	//   summary_timeout        the summary call timed out
	//   summary_provider_error the summary call failed (HTTP, network, auth)
	//   summary_prompt_too_long the summary input itself did not fit
	//   summary_truncated      the summary hit its output cap
	//   summary_empty          no usable text came back
	//   summary_tool_only      the model answered with a tool call, not text
	//   cancelled              the run was stopped during the summary call
	//   context_would_not_fit  even the compacted projection exceeds the window
	//   intent_check_failed    the candidate lost a user message (a bug guard)
	FString FailureCode;
	FString Detail;

	// False for a commit that only cleared old tool-result bodies (no rounds
	// were dropped, so nothing needed summarizing).
	bool bSummarized = false;
	int32 MessagesBefore = 0;
	int32 MessagesAfter = 0;
	int32 DroppedGroups = 0;
	int32 DroppedToolCalls = 0;
	// Genuine user messages from the dropped span, kept verbatim.
	int32 RetainedUserMessages = 0;
	int32 EstimatedTokensAfter = 0;

	// What a committed compaction did, as the record the journal keeps
	// and the reducer replays (KismetAIContextProjection.h). The live candidate
	// is built by applying this record, so live and resumed projections come
	// from the same facts. Unset unless Status is Committed.
	bool bHasProjection = false;
	KismetAIContextProjection::FProjectionRecord Projection;
	// Token-estimate provenance (KismetAIRequestBudget.h): the request as it
	// stood before the decision and the usable window it was judged against.
	// EstimatedTokens* are the decision figures (calibrated by reported usage
	// when a measurement exists); EstimateBefore/After carry their components,
	// basis, window, reservation and limit source for the journal record.
	int32 EstimatedTokensBefore = 0;
	int32 EffectiveLimit = 0;
	bool bHasEstimates = false;
	FKismetAIRequestEstimate EstimateBefore;
	FKismetAIRequestEstimate EstimateAfter;
	// Summary provenance (bSummarized only): what the summarizer reported.
	int32 SummaryInputTokens = 0;
	int32 SummaryOutputTokens = 0;
	int32 SummaryTranscriptMessages = 0;

	bool IsCommitted() const { return Status == ECompactionStatus::Committed; }
	TSharedPtr<FJsonObject> ToJson() const;
};

// What the summarizer produced. There is no fallback text: a failure is a
// failure, and the caller must not treat it as a summary.
struct FKismetAISummaryOutcome
{
	bool bSucceeded = false;
	FString Text;
	FString FailureCode;
	FString Detail;
	// Provider-reported usage of the summary call (0 when not reported).
	int32 InputTokens = 0;
	int32 OutputTokens = 0;
};

// A single tool call from the LLM
struct FKismetAIToolCall
{
	FString Id;         // "toolu_01A09q..."
	FString Name;       // "read_blueprint"
	TSharedPtr<FJsonObject> Arguments;  // parsed input object
};

// Extended thinking mode
enum class EThinkingMode : uint8
{
	Disabled,    // No thinking blocks
	Enabled,     // Budget-based: budget_tokens specified
	Adaptive     // Model-managed (Opus 4.6+ / Sonnet 4.6+)
};

// Effort level — controls how much reasoning effort the model applies
// Sent via output_config.effort with beta header effort-2025-11-24
enum class EEffortLevel : uint8
{
	Low,         // Quick, minimal overhead
	Medium,      // Balanced (recommended for Opus 4.6)
	High,        // Comprehensive, extensive reasoning (API default when not specified)
	XHigh,       // Extended high effort (Opus 4.8 / 4.7)
	Max,         // Maximum effort (Opus 4.8 / 4.7)
};

// Token usage statistics
struct FKismetAITokenStats
{
	int32 TotalInputTokens = 0;
	int32 TotalOutputTokens = 0;
	int32 TotalCacheReadTokens = 0;
	int32 TotalCacheWriteTokens = 0;
	int32 TurnCount = 0;
	int32 RequestCount = 0;
	int32 AttemptCount = 0;             // Invoked attempts, including local failures
	int32 EffectiveMaxTokens = 0;       // Last attempted output limit; zero before any attempt
	int32 TotalReportedTokens = 0;
	bool bUsageComplete = true;
	double EstimatedCostUSD = 0.0;

	// Per-turn stats (reset each turn)
	int32 LastTurnInputTokens = 0;
	int32 LastTurnOutputTokens = 0;

	// Context utilization
	float ContextUsagePercent = 0.0f;
};

// Model cost per million tokens
struct FModelCost
{
	double InputPerMillion = 3.0;
	double OutputPerMillion = 15.0;
	double CacheReadPerMillion = 0.3;
	double CacheWritePerMillion = 3.75;
};

// Agent configuration
struct FKismetAIAgentConfig
{
	bool bSkillRestricted = false;
	TSet<FString> SkillTools;
	FString SkillInstructions;
	FString SelectedSkill;
	bool bProviderConfirmationRequired = false;
	FString ApiKey;                     // Anthropic API key
	FString OpenAIApiKey;               // OpenAI API key (loaded from OPENAI_API_KEY or openai_key.enc)
	FString MinimaxApiKey;              // MiniMax API key (loaded from MINIMAX_API_KEY or minimax_key.enc)
	FString Model = TEXT("claude-opus-5-5");
	// Provider profile id (Config/providers.json). Empty = the catalog's provider
	// for Model, which is what every run used before profiles existed.
	FString ProviderId;
	// Keys for profile slots other than anthropic/openai/minimax (which
	// keep their historical fields above). Slot -> key.
	TMap<FString, FString> ProviderKeys;
	// User base-URL overrides per profile id (local runtimes, custom endpoints).
	TMap<FString, FString> ProviderBaseUrls;
	// User context windows for local and custom models (lowercase model id ->
	// tokens). Most local runtimes report none or load less than the model's
	// maximum; the picker lets the user set the figure the server really uses.
	TMap<FString, int32> ModelContextWindows;
	FString ApiBaseUrl = TEXT("https://api.anthropic.com");
	// Resolved once at admission/request construction; providers never reread
	// routing or budget environment variables for a frozen run.
	bool bProviderEnvironmentResolved = false;
	FString ProviderOrganization;
	FString ProviderProject;
	FString AdaptiveOutputTokensOverride;
	FKismetAIRunLimits RunLimits;
	FString RunBudgetError; // Invalid persisted configuration fails admission closed.
	FString RunPolicyError;
	int32 OperatingContextLimit = 0; // Optional operator restriction; zero leaves catalog/assumed budgeting unchanged.
	int32 MaxSteps = 128;
	int32 MaxTokens = 4096;
	float Temperature = 1.0f;
	int32 MaxToolResultChars = 30000;  // System-wide default for tool result truncation

	// Adaptive max_tokens
	int32 MaxTokensDefault = 4096;     // Standard budget
	int32 MaxTokensLow = 2048;         // For read/compile/planning turns
	int32 MaxTokensHigh = 8192;        // For write_blueprint turns (T3D generation)
	bool bAdaptiveMaxTokens = true;    // Enable/disable adaptive adjustment

	// When thinking/reasoning is active, reasoning tokens are billed against
	// max_tokens (Anthropic adaptive/extended thinking; OpenAI reasoning models).
	// Floor the per-turn budget so reasoning can't consume the whole cap and
	// truncate before the model emits its tool_use. Applies to both providers.
	int32 MaxTokensThinkingFloor = 16384;  // Min output budget when thinking is active
	int32 MaxTokensCeiling = 32000;        // Upper bound when bumping budget on truncation retries
	int32 MaxTruncationRetries = 3;        // Cap consecutive max_tokens continuations before giving up

	// Fast mode (research preview) — trades 2× token price for ~2.5× speed.
	// OFF by default; opt-in only. Populated at startup from KISMETAI_FAST_MODE
	// ("1"/"true"). Only affects models with a fast tier.
	bool bFastMode = false;

	// Extended thinking (default: Adaptive, matching Claude Code behavior)
	EThinkingMode ThinkingMode = EThinkingMode::Adaptive;
	int32 ThinkingBudgetTokens = 10000;  // Only used when ThinkingMode == Enabled

	// Effort level (default: Medium, matching Claude Code recommendation for Opus 4.6)
	// Only effective for models that support it (Opus 4.6, Sonnet 4.6)
	EEffortLevel EffortLevel = EEffortLevel::Medium;

	// First-load onboarding banner (project-context /init prompt). Once the user
	// dismisses it, we stop offering it. Persisted to settings.json.
	bool bOnboardingDismissed = false;

	// First-run Kismet account welcome. The panel recommends signing in once,
	// on the first open without a session; "Continue without an account" or a
	// completed sign-in sets this so it never re-appears. Persisted to settings.json.
	bool bAccountWelcomeSeen = false;

	// Model ids last picked in the panel, most recent first (at most
	// MaxRecentModels). The picker shows them as a "Recent" section above the
	// provider groups. Persisted to settings.json as "recent_models".
	TArray<FString> RecentModels;
	static constexpr int32 MaxRecentModels = 3;

	// Preferred editor for the "View Plan" action — a source-code accessor FName
	// (e.g. "VisualStudioCode"), or empty/"system" for the OS default app.
	// Persisted to settings.json.
	FString PlanEditor;

	// Retry configuration
	int32 MaxRetries = 5;
	double RetryBaseDelayMs = 500.0;    // 500ms base, doubles each attempt
	double RetryMaxDelayMs = 32000.0;   // Cap at 32 seconds
	double RetryJitterFactor = 0.25;    // Additive jitter: 0 to 25% of base

	// Context overflow thresholds
	float ContextOverflowThreshold = 0.90f;
};

// Callback for agent step events (for UI and SSE streaming)
DECLARE_DELEGATE_TwoParams(FOnAgentStep, const FString& /*EventType*/, const FString& /*EventJson*/);

// Worker activity only. SessionController remains the owner of admission,
// cancellation, worker lifetime and lease release.
enum class EKismetAIRunPhase : uint8
{
	Preparing, AwaitingModel, ExecutingTools, Committing, ProjectingContext,
	WaitingForUser, Cancelling, Finishing, Finished
};
enum class EKismetAIRunOutcome : uint8
{
	Completed, WaitingForUser, Cancelled, Failed, Uncertain
};
KISMETAIRUNTIME_API const TCHAR* LexToString(EKismetAIRunPhase Phase);
KISMETAIRUNTIME_API const TCHAR* LexToString(EKismetAIRunOutcome Outcome);

// The agent loop
class KISMETAIRUNTIME_API FKismetAIAgent
{
public:
	// Production: the caller supplies the tool invoker and the agent takes
	// ownership of it. The editor module builds an FKismetAIModuleToolInvoker;
	// the runtime never names that type, which is what keeps this module free of
	// the editor.
	FKismetAIAgent(TUniquePtr<IKismetAIToolInvoker> InToolInvoker, const FKismetAIAgentConfig& InConfig);

	// Tests: inject both seams, so the loop runs with no editor, no network and
	// no API key. Both must outlive the agent.
	FKismetAIAgent(IKismetAIToolInvoker& InToolInvoker, IKismetAIModelClient& InModelClient,
		const FKismetAIAgentConfig& InConfig);

	// Defined out-of-line in the .cpp so the TUniquePtr<ILLMProvider> member (a
	// forward-declared type in this header) is destroyed where the full type is visible.
	~FKismetAIAgent();

	// Run synchronously on its owning worker; editor tools dispatch to the game thread.
	// Returns the final text response.
	// Exactly one error OR done event terminates a run; questions remain resumable.
	// SessionId: empty generates a new session; supplied IDs resume existing journals.
	// bStartNewSession permits an ID freshly allocated by the run owner, never an existing journal.
	FString RunTask(const FString& UserMessage, FOnAgentStep StepCallback = FOnAgentStep(), const FString& ImagesJson = TEXT(""), const FString& SessionId = TEXT(""), bool bStartNewSession = false);

	EKismetAIRunPhase GetRunPhase() const;

	// Get the session ID for the current/last run
	const FString& GetSessionId() const { return CurrentSessionId; }

	// Test seam: runs before every session-journal append RunTask makes.
	// Return false to simulate a failed write — the entry is then NOT written and
	// the loop takes its append-failure path. Also a deterministic place to
	// Abort() between "call A recorded" and "call B starts". Unbound in
	// production.
	using FJournalAppendHook = TFunction<bool(const TSharedPtr<FJsonObject>& Entry)>;
	void SetJournalAppendHookForTests(FJournalAppendHook InHook) { JournalAppendHook = MoveTemp(InHook); }

	// The run id the next RunTask uses as its journal/event run id
	// (`run_id`), so the id an owner hands out (the session controller's
	// run id, the HTTP run resource) is the one in the journal and in every
	// event. Consumed by that RunTask; empty means RunTask makes its own.
	void SetRunIdForNextRun(const FString& InRunId);

	// Abort the current run. Safe from any thread (the game thread, while the
	// worker is mid-turn). Cancels the run token, kills any in-flight model call
	// and wakes a pending ask_user/approval wait.
	//
	// A Stop that arrives before the worker has entered RunTask is kept
	// and applied to the run that starts next, which then ends "aborted"
	// without a model request. It does not carry past that run.
	void Abort();

	// Enable interactive tools (ask_user) for this run. OFF by default so the
	// eval/headless path (HandleAgentRun) can never block on a user that isn't
	// there; the live panel path (LaunchAgent) opts in. When disabled, ask_user
	// returns immediately telling the model to proceed on its best judgment.
	void SetInteractiveAllowed(bool bAllowed);

	// Who this agent's tool calls run on behalf of. The transport that
	// creates the agent sets what it verified: the panel the editor user,
	// /agent/run the X-Kismet-Token caller, MCP the bearer-token client. An agent
	// nobody configured is InProcess — code already inside the editor. Every
	// editor tool call carries this into FKismetAIToolExecutor and the policy.
	void SetCallerContext(const FKismetAICallerContext& InCaller);
	FKismetAICallerContext GetCallerContext() const;

	// Called by the session owner after admission. Later setters affect the
	// next user run; direct RunTask callers capture at entry instead.
	void PrepareRunInputs();

	// Deliver a user's answer to the pending ask_user question and wake the
	// worker thread blocked inside ExecuteTool. Returns false if no question is
	// pending, QuestionId does not match the outstanding one (e.g. a stale
	// answer arriving after the question was already resolved or superseded),
	// or the run has been cancelled (an answer never revives a stopped
	// run, even in the instant before its worker wakes to disarm the question).
	// Called on the game thread from the bridge / HTTP answer route.
	//
	// The first answer to a question is the only one: a second answer before the
	// worker collects it is refused rather than overwriting the first. An
	// approval question only accepts an answer that arrived through the
	// panel (AnsweredVia == Panel) — the editor user, not a REST token holder.
	bool SubmitAnswer(const FString& QuestionId, const FString& AnswersJson,
		EKismetAIEntryPoint AnsweredVia = EKismetAIEntryPoint::Unspecified);

	// The id of the question or approval request this run is waiting on; empty
	// when none. Any thread. The panel learns it from the "question" event; a
	// test driving the panel launch path, which has no panel, reads it here.
	FString GetPendingQuestionId() const;

	// --- External clients (MCP, see KismetAIMcpServer.h) ---

	// Run one editor tool for a caller outside this agent's loop, through the
	// same marshalling, validation, policy and truncation the model's calls get.
	// The in-process tools (plan mode, ask_user, update_todos, discover_tools)
	// are refused with UNKNOWN_TOOL: they act on this agent's run state, which an
	// external caller does not have. Null Arguments are treated as {}. Returns
	// the result as the model would see it, <tool_use_error> wrapper included.
	FString ExecuteToolForExternalClient(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments);

	// Every tool schema KismetAI ships (core + the five on-demand files), in the
	// Anthropic shape, loaded through the same cached loader the loop uses.
	TArray<TSharedPtr<FJsonValue>> GetToolSchemasForExternalClient();

	// --- Static methods (public for testability) ---

	// Compute retry delay based on attempt index and config
	static double ComputeRetryDelay(int32 RetryIndex, const FKismetAIAgentConfig& Config, double RetryAfterHint);

	// Per-result retry budget: NetworkUnreachable fast-fails after 1 retry; all
	// other results honor Config.MaxRetries. Pure helper, exposed for testability.
	static int32 GetMaxRetriesForResult(EModelCallResult Result, const FKismetAIAgentConfig& Config);

	// Map a raw FModelCallOutcome to a user-facing message + suggestions.
	// Selection order:
	//   1. NetworkUnreachable -> "could not reach API / check connection"
	//   2. overloaded_error OR HTTP 529 -> "overloaded ... try again in a moment"
	//   3. Other RetryableError -> echo Outcome.ErrorMessage + "Try again later"
	//   4. Anything else (PromptTooLong, FatalError, MaxTokensOverflow) ->
	//      echo Outcome.ErrorMessage + the generic [api key, prompt size, retry] set
	// Wire-level Outcome.ErrorType is intentionally not changed here — SSE error_code
	// and telemetry error_type stay bound to Outcome.ErrorType so downstream tooling
	// (eval framework, dashboards) keeps working unchanged.
	static FUserFacingError BuildUserFacingError(const FModelCallOutcome& Outcome);

	// Pure classifier for Anthropic API call outcomes. Maps the post-curl signals
	// (process exit code, HTTP status, parsed error fields, abort flag) to one of
	// the EModelCallResult error categories. Only called on the error path
	// (HTTP != 200 or curl failure); the success path bypasses classification.
	// This is a free function in spirit — no instance state — exposed as static
	// so the classification table can be exercised from automation tests.
	static EModelCallResult ClassifyCallOutcome(
		int32 ExitCode,
		int32 ResponseCode,
		const FString& ErrorType,
		const FString& ErrorMessage,
		bool bAborted);

	// Model context limits
	static int32 GetModelContextLimit(const FString& ModelName);
	static int32 ComputeContextInputTokens(
		const FString& ProviderName,
		int32 InputTokens,
		int32 CacheReadTokens,
		int32 CacheWriteTokens);

	// Phase 1: Microcompaction - clear old tool result content, keep conversation flow.
	// Copy-on-write: a cleared result is a new block in a new message object, so
	// any other holder of the original objects (a compaction source snapshot)
	// still sees the full body. When OutClearedBodies is given, each cleared
	// body is recorded there by tool_use_id so a later summary can use the
	// evidence instead of the "[tool result cleared]" marker. Returns how many
	// results were cleared.
	// OutClearedIds receives the tool_use_ids cleared by this call, in order: the
	// replayable fact a context_compaction record carries.
	static int32 MicrocompactToolResults(TArray<TSharedPtr<FJsonValue>>& Messages, int32 KeepRecentN = 2,
		TMap<FString, FString>* OutClearedBodies = nullptr, TArray<FString>* OutClearedIds = nullptr);

	// Phase 2: Full trim, as a transaction. Keeps the opening message and
	// the last two rounds, replaces the rounds between with a model summary, and
	// keeps every genuine user message from that span verbatim, in order, after
	// the summary. Messages changes only when the result is Committed; on any
	// failure (no usable summary, the result would still not fit, a user message
	// would be lost) Messages and its objects are untouched.
	FKismetAICompactionResult TrimConversation(TArray<TSharedPtr<FJsonValue>>& Messages);
	FKismetAICompactionResult TrimConversation(TArray<FAgentMessage>& Messages);

	// The text-only transcript the summarizer reads. Tool calls and results are
	// rendered as text (so the summary request needs no tool schemas on any
	// provider), cleared results are restored from ClearedBodies, thinking is
	// dropped, and genuine user messages are rendered in full.
	static FString BuildSummaryTranscript(const TArray<TSharedPtr<FJsonValue>>& Messages,
		const TMap<FString, FString>& ClearedBodies);

	// Test seam: the compact prompt is normally loaded by RunTask's config load.
	void SetCompactPromptForTests(const FString& Prompt) { CachedCompactPrompt = Prompt; }

	// Model cost lookup. Fast mode is opt-in: bFastMode defaults to false so the
	// pure default never returns fast-tier pricing. Callers pass Config.bFastMode.
	static FModelCost GetModelCost(const FString& ModelName, bool bFastMode = false);
	// Same, choosing a long-context price tier by prompt size where the model has
	// one (Haiku 5.5 above 100K tokens, GPT-6 above 272K).
	static FModelCost GetModelCost(const FString& ModelName, bool bFastMode, int32 PromptTokens);

	// Token stats (accessible for UI)
	const FKismetAITokenStats& GetTokenStats() const { return TokenStats; }

	// Does the current model support thinking?
	static bool ModelSupportsThinking(const FString& ModelName);

	// Does the current model support adaptive thinking?
	static bool ModelSupportsAdaptiveThinking(const FString& ModelName);

	// Convert between EThinkingMode and string ("disabled", "enabled", "adaptive")
	static FString ThinkingModeToString(EThinkingMode Mode);
	static EThinkingMode StringToThinkingMode(const FString& Str);

	// Returns display name for thinking mode ("Off", "Budget", "Adaptive")
	static FString GetThinkingModeDisplayName(EThinkingMode Mode);

	// Does the model support the effort parameter? (Opus 4.6, Sonnet 4.6)
	static bool ModelSupportsEffort(const FString& ModelName);

	// Convert between EEffortLevel and string ("low", "medium", "high")
	static FString EffortLevelToString(EEffortLevel Level);
	static TOptional<EEffortLevel> StringToEffortLevel(const FString& Str);

	// Returns display name for effort level ("Low", "Medium", "High", "XHigh", "Max")
	static FString GetEffortLevelDisplayName(EEffortLevel Level);

	// Does the model use the legacy {type:"enabled", budget_tokens:N} thinking path?
	// False for claude-opus-4-7 (uses adaptive thinking only).
	static bool ModelSupportsExtendedThinking(const FString& ModelName);

	// Does the model accept temperature/top_p/top_k sampling parameters?
	// False for claude-opus-4-7.
	static bool ModelAcceptsSamplingParams(const FString& ModelName);

	// Does the model require the interleaved-thinking-2025-05-14 beta header?
	// False for claude-opus-4-7.
	static bool ModelNeedsLegacyThinkingBetas(const FString& ModelName);

	// Does the model support extended effort levels xhigh/max?
	// True for claude-opus-4-8 and claude-opus-4-7.
	static bool ModelSupportsExtendedEffortLevels(const FString& ModelName);

	// Does the model accept mid-conversation system messages — {"role":"system"}
	// entries appended to the messages array to update instructions mid-task
	// without breaking the prompt cache? GA on claude-opus-4-8 only.
	static bool ModelSupportsMidConversationSystem(const FString& ModelName);

	// Build a mid-conversation system message value ({"role":"system","content":...})
	// for appending to the messages array. Gate on ModelSupportsMidConversationSystem
	// and obey the API placement rules before sending (see implementation comment).
	static TSharedPtr<FJsonValue> MakeSystemMessage(const FString& InstructionText);

	// Does the model have a faster/premium tier that fast mode toggles?
	// Anthropic: Opus 5.5. OpenAI priority tier: gpt-5.5, gpt-5.4, gpt-5.4-mini.
	// MiniMax: MiniMax-M2.7 -> MiniMax-M2.7-highspeed.
	// (Kept in sync with GetModelCost's fast branches and ui.html.)
	static bool ModelSupportsFastMode(const FString& ModelName);

	// Strip trailing thinking blocks from last assistant message before API call
	static void FilterThinkingFromHistory(TArray<TSharedPtr<FJsonValue>>& Messages);

	// What the loop should do after a model turn, given the normalized stop_reason
	// (both providers emit "tool_use" | "end_turn" | "max_tokens"), the number of
	// fully-parsed tool calls, and how many consecutive truncations we've already
	// continued through.
	enum class ETurnAction : uint8
	{
		Done,              // genuine end of turn — finish the run
		ExecuteTools,      // tool calls present — run them and loop
		ContinueTruncated  // output truncated before a tool call — re-request with more budget
	};
	static ETurnAction DecideTurnAction(const FString& StopReason, int32 NumToolCalls,
		int32 ConsecutiveTruncations, int32 MaxTruncationRetries);

	// True when a tool result string carries an error marker (the same markers the
	// loop uses to flag is_error): a <tool_use_error> wrapper or "success":false.
	static bool IsToolResultError(const FString& Result);

	// Per-mutation loop-health outcome, used to keep a failed/rolled-back edit from
	// counting as progress and to short-circuit identical resends (R3b/R4).
	enum class EMutationOutcome : uint8
	{
		NotAMutation,    // not a mutation tool, or it was short-circuited — ignore
		FailedRecord,    // mutation tool errored — record the call hash, no progress
		SucceededClear   // mutation tool succeeded — count progress, clear failed set
	};
	static EMutationOutcome ClassifyMutationOutcome(bool bIsMutationTool,
		bool bShortCircuited, bool bToolErrored);

	// Validate and repair tool_use/tool_result pairing in message history.
	// Ensures every tool_use in an assistant message has exactly one tool_result
	// in the immediately following user message and that tool_use ids are unique.
	// Never removes a genuine user message; a missing result is answered with an
	// OUTCOME_UNRECORDED error. See KismetAIHistoryRepair.h.
	static void ValidateToolPairing(TArray<TSharedPtr<FJsonValue>>& Messages);
	static void ValidateToolPairing(TArray<FAgentMessage>& Messages);

	// Plan mode read-only gate. While plan mode is active the agent may inspect
	// project/Blueprint state, discover schemas, and call exit_plan_mode, but it
	// must not mutate assets.
	static bool IsToolAllowedInPlanMode(const FString& ToolName);

	// Model alias resolution: "opus" -> "claude-opus-5-5", etc. (Config/models.json).
	// Returns empty string if alias/model ID is not recognized.
	static FString ResolveModelAlias(const FString& AliasOrModelId);

	// Returns short display name for a full model ID: "claude-opus-5-5" -> "Opus 5.5"
	static FString GetModelDisplayName(const FString& ModelId);

	// All available aliases and their descriptions (for UI)
	struct FModelAliasInfo
	{
		FString Alias;
		FString ModelId;
		FString DisplayName;
		int32 ContextWindow;
		FString Tier;               // "premium", "standard", "fast" — empty for unlabelled models
		FString RecommendedEffort;  // e.g. "xhigh" for Opus 4.7; empty = no recommendation
		FString Provider;           // "anthropic" / "openai" / "minimax"
		TArray<FString> EffortLevels;  // accepted effort values; empty = no effort control
		bool bSupportsFastMode = false;
		FString ContextSource;      // "catalog", "live", "reported", "user", "assumed"
		bool bDiscovered = false;   // listed by a local or custom server, not the catalog
		bool bToolCalls = true;
		bool bImageInput = true;
		double InputPrice = 0.0;    // $ per million tokens; 0 for local models
		double OutputPrice = 0.0;
	};
	static TArray<FModelAliasInfo> GetAvailableModels();

	// Tool discovery: extract tool names from discover_tools calls in conversation messages.
	// Static overload for testability — takes the set of known extended tool names explicitly.
	static TSet<FString> ExtractDiscoveredToolNames(const TArray<TSharedPtr<FJsonValue>>& Messages, const TSet<FString>& KnownExtendedTools);

	// Request shape helpers (pure static, exposed for testability):

	// Write the thinking, temperature, and output_config fields into Body.
	// Sets bThinkingEnabled=true iff a thinking field was emitted.
	// Sets bEffortEnabled=true iff an output_config.effort field was emitted.
	// Respects (sampling params), (thinking shape), (effort downgrade).
	static void ApplyModelRequestShape(
		TSharedPtr<FJsonObject>& Body,
		const FKismetAIAgentConfig& Config,
		bool& bThinkingEnabled,
		bool& bEffortEnabled);

	// Build the comma-joined anthropic-beta header value.
	// Returns empty string when no beta headers are needed.
	// Respects interleaved-thinking-2025-05-14 only when bThinkingEnabled &&
	//   ModelNeedsLegacyThinkingBetas; effort-2025-11-24 only when bEffortEnabled &&
	//   !ModelSupportsExtendedEffortLevels.
	static FString BuildAnthropicBetaHeader(
		const FKismetAIAgentConfig& Config,
		bool bThinkingEnabled,
		bool bEffortEnabled);

private:
	// The agent's only route to the editor (KismetAIToolInvoker.h). Owned when the
	// module constructor built it, borrowed when a test injected one.
	TUniquePtr<IKismetAIToolInvoker> OwnedToolInvoker;
	IKismetAIToolInvoker& ToolInvoker;

	// The whole model-call boundary (KismetAIModelClient.h): provider encoding,
	// transport, stream decoding, retry and cancellation. Owned when the agent
	// built it, borrowed when a test injected one.
	TUniquePtr<IKismetAIModelClient> OwnedModelClient;
	IKismetAIModelClient& ModelClient;
	const FKismetAIAgentConfig InitialConfig;
	FKismetAIAgentConfig Config;  // Worker-local adaptive/truncation budgets
	mutable FCriticalSection RunInputsMutex;
	TSharedPtr<const FKismetAITurnContext, ESPMode::ThreadSafe> PreparedRunInputs;
	TSharedPtr<const FKismetAITurnContext, ESPMode::ThreadSafe> ActiveRunInputs;
	TSharedRef<const FKismetAITurnContext, ESPMode::ThreadSafe> CaptureRunInputsLocked() const;
	bool IsInteractiveAllowed() const;
	// Cancelled by Abort(). Each tool call derives a child, so a cancelled run
	// cancels work already in flight rather than only work not yet started.
	//
	// RunTask replaces it at run start while Abort may read it from
	// another thread. Both happen under RunTokenMutex. Every other access is
	// the worker's own read, which cannot overlap the worker's own write.
	FKismetAICancellationToken RunToken;
	mutable FCriticalSection RunTokenMutex;
	TAtomic<EKismetAIRunPhase> RunPhase { EKismetAIRunPhase::Finished };
	bool bRunActive = false;         // inside RunTask (guarded)
	bool bAbortBeforeRun = false;    // Abort() arrived while no run was active (guarded)
	FString PendingRunId;            // SetRunIdForNextRun, consumed by RunTask (guarded)
	FKismetAICancellationToken CopyRunToken() const;

	// Session persistence
	FString CurrentSessionId;
	TMap<FString, FString> ObservedAssetRevisions; // Worker-owned, reset for each admitted run.
	bool bRequireReadRevision = false;
	FString SessionBlueprintPath;  // Tracked for session_meta
	FJournalAppendHook JournalAppendHook;

	// Every session-journal write RunTask makes goes through here, so the hook
	// sees them all and a failed write is reported rather than swallowed.
	bool AppendJournalEntry(const TSharedPtr<FJsonObject>& Entry);

	// Cached config files (loaded once on first call)
	FString CachedSystemPrompt;
	FString CachedCompactPrompt;
	TArray<TSharedPtr<FJsonValue>> CachedCoreToolsArray;     // Always sent (core + discover_tools)
	TMap<FString, TSharedPtr<FJsonValue>> ExtendedToolsMap;   // Keyed by tool name, loaded on demand
	TArray<TSharedPtr<FJsonValue>> ActiveToolsArray;          // Composed per API call (core + discovered)
	TSet<FString> PersistentDiscoveredTools;                   // Survives compaction within a session
	bool bConfigLoaded = false;
	void LoadConfigFiles();

	// Plan mode state is per RunTask invocation. ExitPlanMode ends the run with
	// a plan_ready status; approval/resume UI is a separate layer.
	bool bPlanModeActive = false;
	bool bPlanModeExitRequested = false;
	FString CurrentPlanText;
	FString CurrentPlanTitle;
	FString CurrentPlanDescription;
	FString CurrentPlanFilePath;
	FString CurrentPlanSlug;
	TArray<TSharedPtr<FJsonValue>> LastTodosJson;

	// --- Interactive ask_user channel ---
	// The agent can suspend the ReAct loop mid-turn, push a "question" event to
	// the UI, and block the worker thread until the user answers (or aborts /
	// times out). Only enabled on the live panel path (see SetInteractiveAllowed).
	bool bInteractiveAllowed = false;
	FKismetAICallerContext CallerContext = FKismetAICallerContext::InProcess();
	FEventRef AnswerEvent{ EEventMode::ManualReset };   // signaled by SubmitAnswer/Abort
	mutable FCriticalSection AnswerMutex;                // guards the fields below
	FString PendingQuestionId;                           // non-empty while a question is outstanding
	FString PendingAnswerJson;                           // answers payload set by SubmitAnswer
	bool bAnswerReady = false;
	bool bPendingRequiresEditorUser = false;             // approval questions: panel answers only

	// Arms the channel, emits a "question" event with QuestionId and waits for
	// the answer, the run's cancellation or the timeout. Returns true with the
	// answers payload when answered.
	enum class EUserAnswerWait : uint8 { Answered, Aborted, TimedOut };
	EUserAnswerWait AskUserAndWait(const FString& QuestionId, const TArray<TSharedPtr<FJsonValue>>& Questions,
		double TimeoutSeconds, bool bRequireEditorUser, FString& OutAnswersJson);

	// The executor's approver on interactive runs: asks the editor user
	// to allow one bound operation, once or for this session.
	FKismetAIApprovalAnswer RequestApprovalFromUser(const FPolicyVerdict& Verdict);

	// The step callback for the active RunTask, copied at loop entry so in-process
	// tools (ask_user, update_todos) can stream events without threading it through
	// every ExecuteTool signature. Cleared on RunTask exit.
	FOnAgentStep ActiveStepCallback;

	// In-process tool handlers (no HTTP round-trip; touch agent/run state directly).
	FToolCallOutcome HandleAskUserTool(const TSharedPtr<FJsonObject>& Arguments);
	FToolCallOutcome HandleUpdateTodosTool(const TSharedPtr<FJsonObject>& Arguments);

	// Tool discovery system (mirrors Claude Code's ToolSearch pattern)
	// Scans conversation messages for discover_tools calls to determine which extended tools are active.
	// Message-driven: no persistent state, computed fresh before each API call.
	TSet<FString> ExtractDiscoveredToolNames(const TArray<TSharedPtr<FJsonValue>>& Messages) const;
	void ComposeToolsArray(const TArray<TSharedPtr<FJsonValue>>& Messages);
	void ComposeToolsArray(const TArray<FAgentMessage>& Messages);

	// Freeze composed tools after history loading and request fitting.
	FKismetAITurnContext MakeTurnContext(const FString& RunId, const FString& TurnId, int32 TurnIndex, bool bThinkingActive);
	FModelCallRequest MakeModelRequest(const TArray<FAgentMessage>& Messages, const FKismetAITurnContext& Turn) const;

	// Execute a local or editor tool and retain its authoritative outcome.
	FToolCallOutcome ExecuteTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments);

	// ── mutation intent and the uncertain-replay guard ────────────────
	// Set by the loop around one ExecuteTool call so ExecuteTool can journal a
	// mutating call's mutation_intent (its targets' before-state) right before
	// the executor runs it. Unset for external clients (no journal).
	struct FActiveCallIdentity
	{
		bool bSet = false;
		FString RunId;
		FString TurnId;
		int32 CallIndex = 0;
		FString ToolUseId;
		FString ArgsCanonical;
	};
	FActiveCallIdentity ActiveCall;

	// Replay keys (tool + canonical arguments) of calls whose outcome was never
	// recorded and whose reconciliation did not show "not applied". An
	// identical mutating call is refused for the rest of the run
	// (RECONCILIATION_REQUIRED); the value is the verdict, for the message.
	TMap<FString, FString> UncertainReplayKeys;

	// Reconcile every uncertain call the snapshot has no reconciliation for:
	// capture the targets' state now, compare with the intent's before-state,
	// journal a mutation_reconciliation and emit it as an event. Returns true
	// when anything was appended (the caller reloads the snapshot).
	bool ReconcileUncertainCalls(const FSessionSnapshot& Loaded, const FOnAgentStep& StepCallback);

	// Token tracking
	FKismetAITokenStats TokenStats;
	int32 CumulativeInputTokens = 0;
	int32 CumulativeOutputTokens = 0;
	// Input count the provider reported for the last main request, as reported:
	// telemetry and diagnostics. It describes a request already sent, so no
	// budget decision reads it directly; UsageCalibration pairs it with
	// the local estimate of that same request.
	int32 LastReportedInputTokens = 0;
	void RecordModelUsage(const FModelCallOutcome& Outcome, bool bUpdateContext);
	TSharedPtr<FKismetAIRunBudget> ActiveRunBudget;
	FString DiagnosticRunId;
	FString DiagnosticSummaryRequestId;
	int32 DiagnosticSummaryIndex = 0;
	FModelCallOutcome CallBudgetedModel(const FModelCallRequest& Request, const FModelCallbacks& Callbacks, bool bUpdateContext);

	// The last provider measurement of a main request, beside the local
	// estimate of what was sent. Kept across later failed calls: the pair still
	// describes the request it measured.
	FKismetAIUsageCalibration UsageCalibration;

	// The window, output reservation and limit provenance for the next request
	// (Config.Model, Config.MaxTokens).
	FKismetAIContextBudget GetContextBudget() const;

	// Usable input window for the next request: GetContextBudget().EffectiveLimit.
	int32 GetEffectiveContextLimit() const;

	// AI-powered conversation summarization. A text-only request with no tool
	// schemas; anything other than usable text is reported as a failure.
	FKismetAISummaryOutcome SummarizeConversation(const TArray<TSharedPtr<FJsonValue>>& MessagesToSummarize,
		const TMap<FString, FString>& ClearedBodies);

	// Estimate of the request that would carry Messages with the current system
	// prompt and the tool schemas currently composed (ComposeToolsArray first),
	// calibrated by UsageCalibration when a measurement exists. Every budget
	// decision reads this, recomputed after each projection change.
	FKismetAIRequestEstimate EstimateNextRequest(const TArray<TSharedPtr<FJsonValue>>& Messages) const;
	FKismetAIRequestEstimate EstimateNextRequest(const TArray<FAgentMessage>& Messages) const;

	// EstimateNextRequest(Messages).Tokens.
	int32 EstimateRequestTokens(const TArray<TSharedPtr<FJsonValue>>& Messages) const;
	int32 EstimateRequestTokens(const TArray<FAgentMessage>& Messages) const;

	// Bodies microcompaction cleared from the in-memory projection during this
	// run, by tool_use_id: the evidence a later summary should read.
	TMap<FString, FString> ClearedToolResultBodies;
};
