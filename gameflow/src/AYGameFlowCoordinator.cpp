#include <AYApplication/GameFlowCoordinator.h>
#include <AYApplication/GameFlowProgram.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

namespace ayt::app
{
namespace
{

constexpr std::size_t kNoState = std::numeric_limits<std::size_t>::max();

std::string transitionKey(std::string_view state, std::string_view intent)
{
    std::string result;
    result.reserve(state.size() + intent.size() + 1u);
    result.append(state);
    result.push_back('\x1f');
    result.append(intent);
    return result;
}

bool valueMatches(const GameFlowValue& value, GameFlowValueType type)
{
    switch (type) {
    case GameFlowValueType::Boolean:
        return std::holds_alternative<bool>(value.data);
    case GameFlowValueType::Integer:
        return std::holds_alternative<std::int64_t>(value.data);
    case GameFlowValueType::Number:
        return std::holds_alternative<std::int64_t>(value.data)
            || std::holds_alternative<double>(value.data);
    case GameFlowValueType::String:
        return std::holds_alternative<std::string>(value.data);
    }
    return false;
}

bool normalizePayload(const std::vector<GameFlowFieldDefinition>& fields,
                      std::string_view contractName,
                      GameFlowPayload& payload,
                      const GameFlowPayload* fallback,
                      std::string_view reservedField,
                      std::string& error)
{
    std::map<std::string, const GameFlowFieldDefinition*, std::less<>> schema;
    for (const auto& field : fields) schema.emplace(field.id, &field);
    for (const auto& [id, value] : payload) {
        if (!reservedField.empty() && id == reservedField) continue;
        const auto found = schema.find(id);
        if (found == schema.end()) {
            error = std::string(contractName) + " received unknown field '"
                + id + "'.";
            return false;
        }
        if (!valueMatches(value, found->second->type)) {
            error = std::string(contractName) + " field '" + id
                + "' has the wrong type.";
            return false;
        }
    }
    if (!reservedField.empty()) payload.erase(std::string(reservedField));
    for (const auto& field : fields) {
        if (payload.contains(field.id)) continue;
        if (fallback != nullptr) {
            const auto source = fallback->find(field.id);
            if (source != fallback->end()
                && valueMatches(source->second, field.type)) {
                payload.emplace(field.id, source->second);
                continue;
            }
        }
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            payload.emplace(field.id, field.defaultValue);
        } else if (field.required) {
            error = std::string(contractName) + " requires field '"
                + field.id + "'.";
            return false;
        }
    }
    error.clear();
    return true;
}

bool normalizeIntentPayload(const GameFlowIntentDefinition& intent,
                            GameFlowPayload& payload,
                            std::string& error)
{
    return normalizePayload(intent.payload,
        "Intent '" + intent.id + "'", payload, nullptr, {}, error);
}

bool hasControlActions(const GameFlowPlan& plan) noexcept
{
    for (const auto& transition : plan.document.transitions) {
        for (const auto& action : transition.actions) {
            if (isGameFlowControlAction(action.action)) return true;
        }
    }
    return false;
}

GameFlowDeterminismOutcome determinismOutcome(
    GameFlowActionState value) noexcept
{
    switch (value) {
    case GameFlowActionState::Succeeded:
        return GameFlowDeterminismOutcome::Succeeded;
    case GameFlowActionState::Failed:
        return GameFlowDeterminismOutcome::Failed;
    case GameFlowActionState::Pending:
        return GameFlowDeterminismOutcome::Pending;
    case GameFlowActionState::Cancelled:
        return GameFlowDeterminismOutcome::Cancelled;
    }
    return GameFlowDeterminismOutcome::Failed;
}

std::optional<GameFlowActionState> actionState(
    GameFlowDeterminismOutcome value) noexcept
{
    switch (value) {
    case GameFlowDeterminismOutcome::Succeeded:
        return GameFlowActionState::Succeeded;
    case GameFlowDeterminismOutcome::Failed:
        return GameFlowActionState::Failed;
    case GameFlowDeterminismOutcome::Pending:
        return GameFlowActionState::Pending;
    case GameFlowDeterminismOutcome::Cancelled:
        return GameFlowActionState::Cancelled;
    default:
        return std::nullopt;
    }
}

bool sameDeterminismContext(
    const GameFlowDeterminismRecord& expected,
    const GameFlowDeterminismRecord& actual,
    std::string& error)
{
    const auto mismatch = [&](std::string_view field) {
        error = "GameFlow playback mismatch at sequence "
            + std::to_string(expected.sequence) + " (" + std::string(field)
            + ").";
        return false;
    };
    if (actual.schemaVersion != kGameFlowDeterminismSchemaVersion) {
        return mismatch("schemaVersion");
    }
    if (actual.sequence != expected.sequence) return mismatch("sequence");
    if (actual.kind != expected.kind) return mismatch("kind");
    if (actual.flowId != expected.flowId) return mismatch("flowId");
    if (actual.stateId != expected.stateId) return mismatch("stateId");
    if (actual.intentId != expected.intentId) return mismatch("intentId");
    if (actual.generation != expected.generation) return mismatch("generation");
    if (actual.actionExecutionId != expected.actionExecutionId) {
        return mismatch("actionExecutionId");
    }
    if (actual.callDepth != expected.callDepth) return mismatch("callDepth");

    switch (expected.kind) {
    case GameFlowDeterminismKind::Intent:
        if (actual.intentOrigin != expected.intentOrigin) {
            return mismatch("intentOrigin");
        }
        if (actual.payload != expected.payload) return mismatch("payload");
        break;
    case GameFlowDeterminismKind::Update:
        if (actual.deltaSeconds != expected.deltaSeconds) {
            return mismatch("deltaSeconds");
        }
        break;
    case GameFlowDeterminismKind::GuardOutcome:
        if (actual.transitionId != expected.transitionId) {
            return mismatch("transitionId");
        }
        if (actual.guardId != expected.guardId) return mismatch("guardId");
        break;
    case GameFlowDeterminismKind::TransitionDecision:
        break;
    case GameFlowDeterminismKind::ActionOutcome:
        if (actual.transitionId != expected.transitionId) {
            return mismatch("transitionId");
        }
        if (actual.actionId != expected.actionId) return mismatch("actionId");
        break;
    case GameFlowDeterminismKind::AsyncCompletion:
        if (actual.transitionId != expected.transitionId) {
            return mismatch("transitionId");
        }
        if (actual.actionId != expected.actionId) return mismatch("actionId");
        break;
    case GameFlowDeterminismKind::Cancellation:
        if (actual.cancellation != expected.cancellation) {
            return mismatch("cancellation");
        }
        if (actual.transitionId != expected.transitionId) {
            return mismatch("transitionId");
        }
        if (actual.intentId != expected.intentId) return mismatch("intentId");
        if (actual.targetFlowId != expected.targetFlowId) {
            return mismatch("targetFlowId");
        }
        break;
    case GameFlowDeterminismKind::SubflowEnter:
    case GameFlowDeterminismKind::SubflowReturn:
        if (actual.transitionId != expected.transitionId) {
            return mismatch("transitionId");
        }
        if (actual.targetFlowId != expected.targetFlowId) {
            return mismatch("targetFlowId");
        }
        break;
    case GameFlowDeterminismKind::Terminal:
        if (actual.transitionId != expected.transitionId) {
            return mismatch("transitionId");
        }
        if (actual.resultStateId != expected.resultStateId) {
            return mismatch("resultStateId");
        }
        if (actual.outcome != expected.outcome) return mismatch("outcome");
        break;
    case GameFlowDeterminismKind::ProgramManifest:
        if (actual.programFingerprint != expected.programFingerprint) {
            return mismatch("programFingerprint");
        }
        if (actual.payload != expected.payload) return mismatch("payload");
        break;
    }
    error.clear();
    return true;
}

} // namespace

bool buildGameFlowPlan(
    const GameFlowDocument& document,
    const GameFlowActionRegistry& registry,
    GameFlowPlan& plan,
    std::vector<GameFlowDiagnostic>* diagnostics)
{
    if (!validateGameFlow(document, &registry, diagnostics)) return false;

    GameFlowPlan built;
    built.document = document;
    for (auto& transition : built.document.transitions) {
        if (!transition.guard.guard.empty()) {
            const auto* definition = registry.findGuard(transition.guard.guard);
            for (const auto& field : definition->arguments) {
                if (!transition.guard.arguments.contains(field.id)
                    && !std::holds_alternative<std::monostate>(
                        field.defaultValue.data)) {
                    transition.guard.arguments.emplace(
                        field.id, field.defaultValue);
                }
            }
        }
        for (auto& action : transition.actions) {
            if (isGameFlowControlAction(action.action)) continue;
            const auto* definition = registry.findAction(action.action);
            for (const auto& field : definition->arguments) {
                if (!action.arguments.contains(field.id)
                    && !std::holds_alternative<std::monostate>(
                        field.defaultValue.data)) {
                    action.arguments.emplace(field.id, field.defaultValue);
                }
            }
        }
    }
    for (std::size_t index = 0; index < document.states.size(); ++index) {
        built.stateIndices.emplace(document.states[index].id, index);
    }
    for (std::size_t index = 0; index < document.intents.size(); ++index) {
        built.intentIndices.emplace(document.intents[index].id, index);
    }
    built.transitions.reserve(document.transitions.size());
    for (std::size_t index = 0; index < document.transitions.size(); ++index) {
        const auto& transition = built.document.transitions[index];
        const auto from = built.stateIndices.find(transition.fromState);
        const auto to = built.stateIndices.find(transition.toState);
        const auto intent = built.intentIndices.find(transition.triggerIntent);
        if (from == built.stateIndices.end() || to == built.stateIndices.end()
            || intent == built.intentIndices.end()) {
            if (diagnostics != nullptr) {
                diagnostics->push_back({GameFlowDiagnosticSeverity::Error,
                    "$.transitions[" + std::to_string(index) + "]",
                    "Normalized transition references an unavailable index."});
            }
            return false;
        }
        GameFlowNormalizedTransition normalized;
        normalized.documentIndex = index;
        normalized.fromState = from->second;
        normalized.toState = to->second;
        normalized.intent = intent->second;
        if (!transition.onFailureState.empty()) {
            const auto fallback = built.stateIndices.find(
                transition.onFailureState);
            if (fallback == built.stateIndices.end()) return false;
            normalized.onFailureState = fallback->second;
        }
        if (!transition.onCancelState.empty()) {
            const auto cancellation = built.stateIndices.find(
                transition.onCancelState);
            if (cancellation == built.stateIndices.end()) return false;
            normalized.onCancelState = cancellation->second;
        }
        built.transitions.push_back(normalized);
        built.transitionsByStateAndIntent[transitionKey(
            transition.fromState, transition.triggerIntent)].push_back(index);
    }
    for (auto& [key, candidates] : built.transitionsByStateAndIntent) {
        (void)key;
        std::stable_sort(candidates.begin(), candidates.end(),
            [&](std::size_t left, std::size_t right) {
                const auto& leftDefinition = built.document.transitions[
                    built.transitions[left].documentIndex];
                const auto& rightDefinition = built.document.transitions[
                    built.transitions[right].documentIndex];
                return leftDefinition.priority > rightDefinition.priority;
            });
    }
    plan = std::move(built);
    return true;
}

class GameFlowCoordinator::Impl
{
public:
    struct DispatchScope
    {
        explicit DispatchScope(std::size_t& value) : depth(value) { ++depth; }
        ~DispatchScope() { --depth; }
        std::size_t& depth;
    };

    struct IntentRequest
    {
        std::size_t intentIndex = 0;
        GameFlowPayload payload;
    };

    struct ActiveTransition
    {
        GameFlowGeneration generation = 0;
        std::size_t transitionIndex = 0;
        IntentRequest request;
        std::size_t nextAction = 0;
        std::size_t activeActionIndex = kNoGameFlowActionIndex;
        double elapsedSeconds = 0.0;
        GameFlowActionExecutionId activeActionExecution = 0;
        GameFlowActionExecutionId pendingAction = 0;
        GameFlowActionCancellationHandler onCancel;
        bool waitingForSubflow = false;
    };

    struct Frame
    {
        const GameFlowPlan* plan = nullptr;
        std::uint64_t instanceSerial = 0;
        std::size_t currentStateIndex = kNoState;
        GameFlowPayload parameters;
        std::deque<IntentRequest> requests;
        std::optional<ActiveTransition> active;
        GameFlowPayload lastSubflowResult;
        std::string returnedFlowId;
    };

    const GameFlowProgram* program = nullptr;
    const GameFlowPlan* legacyPlan = nullptr;
    const GameFlowActionRegistry* registry = nullptr;
    std::vector<Frame> frames;
    GameFlowGeneration nextGeneration = 1;
    GameFlowActionExecutionId nextActionExecutionId = 1;
    std::uint64_t nextFrameSerial = 1;
    std::uint64_t nextTraceSerial = 1;
    std::size_t dispatchDepth = 0;
    std::vector<GameFlowTraceEntry> traces;
    GameFlowDiagnostics diagnostics;
    std::size_t publicCallDepth = 0;
    std::size_t externalCallbackDepth = 0;
    GameFlowDeterminismIntentOrigin callbackOrigin =
        GameFlowDeterminismIntentOrigin::External;
    IGameFlowDeterminismExchange* determinism = nullptr;
    std::uint64_t nextDeterminismSequence = 1;
    std::optional<GameFlowDeterminismRecord> bufferedPlaybackRecord;
    bool determinismFaulted = false;
    std::string determinismFault;

    struct ExternalCallbackScope
    {
        explicit ExternalCallbackScope(
            Impl& value,
            GameFlowDeterminismIntentOrigin origin) noexcept
            : owner(value), previous(value.callbackOrigin)
        {
            ++owner.externalCallbackDepth;
            owner.callbackOrigin = origin;
        }
        ~ExternalCallbackScope()
        {
            owner.callbackOrigin = previous;
            --owner.externalCallbackDepth;
        }
        Impl& owner;
        GameFlowDeterminismIntentOrigin previous;
    };

    struct PublicCallScope
    {
        explicit PublicCallScope(Impl& value) noexcept : owner(value)
        {
            ++owner.publicCallDepth;
        }
        ~PublicCallScope()
        {
            if (--owner.publicCallDepth == 0u) {
                owner.diagnostics.flushObserver();
            }
        }
        Impl& owner;
    };

    struct UpdateTimingScope
    {
        UpdateTimingScope(Impl& value, double delta) noexcept
            : owner(value), deltaSeconds(delta), started(Clock::now())
        {
        }
        ~UpdateTimingScope()
        {
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::nanoseconds>(Clock::now() - started).count();
            owner.diagnostics.recordUpdate(deltaSeconds,
                elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0u);
        }

        using Clock = std::chrono::steady_clock;
        Impl& owner;
        double deltaSeconds = 0.0;
        Clock::time_point started;
    };

    [[nodiscard]] bool mutationAllowed(std::string* error = nullptr) const
    {
        if (determinismFaulted) {
            if (error != nullptr) {
                *error = determinismFault.empty()
                    ? "GameFlow determinism exchange failed."
                    : determinismFault;
            }
            return false;
        }
        if (externalCallbackDepth != 0u) {
            if (error != nullptr) {
                *error = "GameFlow mutation is not allowed from a callback; "
                    "request() is the reentrant-safe ingress.";
            }
            return false;
        }
        return true;
    }

    void retainDeterminismFault(std::string message) noexcept
    {
        if (determinismFaulted) return;
        determinismFaulted = true;
        try {
            determinismFault = message.empty()
                ? "GameFlow determinism exchange failed." : std::move(message);
            appendTrace(0, 0, {}, determinismFault);
        } catch (...) {
            try {
                determinismFault = "GameFlow determinism exchange failed.";
            } catch (...) {
            }
        }
    }

    bool readDeterminismRecord(
        GameFlowDeterminismRecord& record, std::string& error)
    {
        if (bufferedPlaybackRecord.has_value()) {
            record = std::move(*bufferedPlaybackRecord);
            bufferedPlaybackRecord.reset();
            error.clear();
            return true;
        }
        bool ok = false;
        try {
            ExternalCallbackScope callback(*this,
                GameFlowDeterminismIntentOrigin::DeterminismExchange);
            ok = determinism->exchange(record, error);
        } catch (const std::exception& exception) {
            error = std::string("GameFlow determinism exchange threw: ")
                + exception.what();
        } catch (...) {
            error = "GameFlow determinism exchange threw.";
        }
        if (!ok) {
            if (error.empty() && !determinism->healthy()) {
                error = std::string(determinism->fault());
            }
            retainDeterminismFault(std::move(error));
            return false;
        }
        return true;
    }

    bool injectRecordedIntent(
        const GameFlowDeterminismRecord& actual,
        GameFlowDeterminismIntentOrigin allowedOrigin,
        std::string& error)
    {
        if (actual.kind != GameFlowDeterminismKind::Intent) {
            error = "GameFlow playback callback injection expected an intent.";
            return false;
        }
        const bool observerIntent = actual.intentOrigin
            == GameFlowDeterminismIntentOrigin::ObserverCallback;
        if (!observerIntent && actual.intentOrigin != allowedOrigin) {
            error = "GameFlow playback encountered an intent from an "
                "unexpected callback origin.";
            return false;
        }
        if (actual.intentOrigin
            == GameFlowDeterminismIntentOrigin::DeterminismExchange
            || actual.intentOrigin
                == GameFlowDeterminismIntentOrigin::External) {
            error = "GameFlow playback cannot inject this intent origin.";
            return false;
        }
        if (actual.callDepth >= frames.size()) {
            error = "GameFlow playback callback intent targets an unavailable "
                "flow frame.";
            return false;
        }
        Frame& frame = frames[actual.callDepth];
        const auto found = frame.plan->intentIndices.find(actual.intentId);
        if (found == frame.plan->intentIndices.end()) {
            error = "GameFlow playback callback intent is not declared by its "
                "recorded flow.";
            return false;
        }
        GameFlowPayload normalized = actual.payload;
        if (!normalizeIntentPayload(
                frame.plan->document.intents[found->second], normalized, error)
            || normalized != actual.payload) {
            if (error.empty()) {
                error = "GameFlow playback callback intent payload is not "
                    "canonical.";
            }
            return false;
        }
        auto expected = this->record(
            GameFlowDeterminismKind::Intent, actual.callDepth);
        expected.schemaVersion = kGameFlowDeterminismSchemaVersion;
        expected.sequence = nextDeterminismSequence;
        expected.intentOrigin = actual.intentOrigin;
        expected.intentId = actual.intentId;
        expected.payload = actual.payload;
        if (!sameDeterminismContext(expected, actual, error)) return false;

        frame.requests.push_back({found->second, actual.payload});
        emit(GameFlowEventKind::IntentQueued,
            GameFlowEventReason::None, 0, 0, {}, actual.intentId, {}, {},
            actual.callDepth);
        ++nextDeterminismSequence;
        return true;
    }

    bool exchange(GameFlowDeterminismRecord& record,
                  GameFlowDeterminismIntentOrigin callbackIntentOrigin =
                      GameFlowDeterminismIntentOrigin::DeterminismExchange)
    {
        if (determinismFaulted) return false;
        if (determinism == nullptr) return true;
        record.schemaVersion = kGameFlowDeterminismSchemaVersion;
        const GameFlowDeterminismRecord expectedBase = record;
        for (;;) {
            GameFlowDeterminismRecord expected = expectedBase;
            expected.sequence = nextDeterminismSequence;
            record = expected;
            std::string error;
            if (!readDeterminismRecord(record, error)) return false;
            if (determinism->mode() == GameFlowDeterminismMode::Playback) {
                const bool matchingIntentOrigin = expected.kind
                        == GameFlowDeterminismKind::Intent
                    && record.kind == GameFlowDeterminismKind::Intent
                    && expected.intentOrigin == record.intentOrigin;
                const bool mayInject = record.kind
                        == GameFlowDeterminismKind::Intent
                    && !matchingIntentOrigin
                    && (record.intentOrigin
                            == GameFlowDeterminismIntentOrigin::ObserverCallback
                        || record.intentOrigin == callbackIntentOrigin);
                if (mayInject) {
                    if (!injectRecordedIntent(
                            record, callbackIntentOrigin, error)) {
                        retainDeterminismFault(std::move(error));
                        return false;
                    }
                    continue;
                }
                if (!sameDeterminismContext(expected, record, error)) {
                    retainDeterminismFault(std::move(error));
                    return false;
                }
            }
            ++nextDeterminismSequence;
            return true;
        }
    }

    bool drainCallbackIntents(GameFlowDeterminismIntentOrigin origin) noexcept
    {
        if (determinismFaulted || determinism == nullptr
            || determinism->mode() != GameFlowDeterminismMode::Playback) {
            return !determinismFaulted;
        }
        try {
            for (;;) {
                GameFlowDeterminismRecord actual;
                std::string error;
                if (!readDeterminismRecord(actual, error)) return false;
                if (actual.kind != GameFlowDeterminismKind::Intent
                    || actual.intentOrigin != origin) {
                    bufferedPlaybackRecord = std::move(actual);
                    return true;
                }
                if (!injectRecordedIntent(actual, origin, error)) {
                    retainDeterminismFault(std::move(error));
                    return false;
                }
            }
        } catch (const std::exception& exception) {
            retainDeterminismFault(
                std::string("GameFlow callback intent injection threw: ")
                + exception.what());
        } catch (...) {
            retainDeterminismFault(
                "GameFlow callback intent injection threw.");
        }
        return false;
    }

    GameFlowDeterminismRecord record(GameFlowDeterminismKind kind,
                                     std::size_t frameIndex) const
    {
        GameFlowDeterminismRecord value;
        value.kind = kind;
        value.callDepth = frameIndex;
        if (frameIndex >= frames.size()) return value;
        const Frame& frame = frames[frameIndex];
        value.flowId = frame.plan->document.id;
        if (frame.currentStateIndex != kNoState) {
            value.stateId = frame.plan->document.states[
                frame.currentStateIndex].id;
        }
        return value;
    }

    std::size_t resolveInitial(const GameFlowPlan& plan,
                               std::size_t stateIndex) const
    {
        while (stateIndex != kNoState) {
            const auto& state = plan.document.states[stateIndex];
            if (state.initialChild.empty()) break;
            const auto child = plan.stateIndices.find(state.initialChild);
            if (child == plan.stateIndices.end()) return kNoState;
            stateIndex = child->second;
        }
        return stateIndex;
    }

    Frame makeFrame(const GameFlowPlan* plan, GameFlowPayload parameters)
    {
        Frame frame;
        frame.plan = plan;
        frame.instanceSerial = nextFrameSerial++;
        frame.parameters = std::move(parameters);
        const auto initial = plan->stateIndices.find(plan->document.initialState);
        frame.currentStateIndex = initial == plan->stateIndices.end()
            ? kNoState : resolveInitial(*plan, initial->second);
        return frame;
    }

    void emit(GameFlowEventKind kind,
              GameFlowEventReason reason = GameFlowEventReason::None,
              GameFlowGeneration generation = 0,
              GameFlowActionExecutionId actionExecutionId = 0,
              std::string_view transition = {},
              std::string_view intent = {},
              std::string_view action = {},
              std::string_view guard = {},
              std::size_t frameIndex = kNoState) noexcept
    {
        try {
            if (frameIndex == kNoState && !frames.empty()) {
                frameIndex = frames.size() - 1u;
            }
            GameFlowEvent event;
            event.kind = kind;
            event.reason = reason;
            event.generation = generation;
            event.actionExecutionId = actionExecutionId;
            event.transitionId = transition;
            event.intentId = intent;
            event.actionId = action;
            event.guardId = guard;
            if (frameIndex < frames.size()) {
                const Frame& frame = frames[frameIndex];
                event.flowId = frame.plan->document.id;
                event.callDepth = frameIndex;
                if (frame.currentStateIndex != kNoState) {
                    event.stateId = frame.plan->document.states[
                        frame.currentStateIndex].id;
                }
            }
            diagnostics.record(std::move(event));
            diagnostics.observeQueueDepth(queuedCount());
            diagnostics.observeCallDepth(
                frames.empty() ? 0u : frames.size() - 1u);
        } catch (...) {
            // Structured diagnostics are observational and never affect flow.
        }
    }

    void appendTrace(GameFlowGeneration generation,
                     GameFlowActionExecutionId actionExecutionId,
                     std::string transition,
                     std::string detail,
                     std::size_t frameIndex = kNoState)
    {
        if (frameIndex == kNoState && !frames.empty()) {
            frameIndex = frames.size() - 1u;
        }
        std::string state;
        std::string flow;
        std::size_t depth = 0;
        if (frameIndex < frames.size()) {
            const Frame& frame = frames[frameIndex];
            flow = frame.plan->document.id;
            depth = frameIndex;
            if (frame.currentStateIndex != kNoState) {
                state = frame.plan->document.states[
                    frame.currentStateIndex].id;
            }
        }
        GameFlowTraceEntry entry;
        entry.serial = nextTraceSerial++;
        entry.generation = generation;
        entry.actionExecutionId = actionExecutionId;
        entry.state = std::move(state);
        entry.transition = std::move(transition);
        entry.detail = std::move(detail);
        entry.flowId = std::move(flow);
        entry.callDepth = depth;
        traces.push_back(std::move(entry));
        if (traces.size() > 512u) traces.erase(traces.begin());
    }

    std::optional<std::size_t> selectTransition(
        Frame& frame, const IntentRequest& request)
    {
        std::size_t stateIndex = frame.currentStateIndex;
        const auto& intent = frame.plan->document.intents[request.intentIndex];
        while (stateIndex != kNoState) {
            const auto& state = frame.plan->document.states[stateIndex];
            const auto candidates =
                frame.plan->transitionsByStateAndIntent.find(
                    transitionKey(state.id, intent.id));
            if (candidates !=
                frame.plan->transitionsByStateAndIntent.end()) {
                for (const std::size_t transitionIndex : candidates->second) {
                    const auto& normalized =
                        frame.plan->transitions[transitionIndex];
                    const auto& transition = frame.plan->document.transitions[
                        normalized.documentIndex];
                    if (transition.guard.guard.empty()) {
                        auto decision = record(
                            GameFlowDeterminismKind::TransitionDecision,
                            frames.size() - 1u);
                        decision.intentId = intent.id;
                        decision.transitionId = transition.id;
                        decision.outcome = GameFlowDeterminismOutcome::Accepted;
                        if (!exchange(decision)) return std::nullopt;
                        if (decision.outcome !=
                                GameFlowDeterminismOutcome::Accepted
                            || decision.transitionId != transition.id) {
                            retainDeterminismFault(
                                "GameFlow playback selected an invalid "
                                "unguarded transition.");
                            return std::nullopt;
                        }
                        return transitionIndex;
                    }

                    auto guardRecord = record(
                        GameFlowDeterminismKind::GuardOutcome,
                        frames.size() - 1u);
                    guardRecord.intentId = intent.id;
                    guardRecord.transitionId = transition.id;
                    guardRecord.guardId = transition.guard.guard;
                    GameFlowEventReason failureReason =
                        GameFlowEventReason::None;
                    if (determinism != nullptr
                        && determinism->mode()
                            == GameFlowDeterminismMode::Playback) {
                        if (!exchange(guardRecord,
                                GameFlowDeterminismIntentOrigin::GuardCallback)) {
                            return std::nullopt;
                        }
                    } else {
                        const auto* handler = registry->findGuardHandler(
                            transition.guard.guard);
                        if (handler == nullptr) {
                            guardRecord.outcome =
                                GameFlowDeterminismOutcome::Failed;
                            failureReason = GameFlowEventReason::MissingHandler;
                        } else {
                            try {
                                const GameFlowGuardInvocation invocation{
                                    frame.plan->document.id,
                                    transition.id,
                                    intent.id,
                                    &request.payload,
                                    &transition.guard.arguments,
                                    &frame.parameters,
                                    &frame.lastSubflowResult,
                                    frame.returnedFlowId,
                                };
                                ExternalCallbackScope callback(*this,
                                    GameFlowDeterminismIntentOrigin::GuardCallback);
                                guardRecord.outcome = (*handler)(invocation)
                                    ? GameFlowDeterminismOutcome::Accepted
                                    : GameFlowDeterminismOutcome::Rejected;
                            } catch (const std::exception& exception) {
                                guardRecord.outcome =
                                    GameFlowDeterminismOutcome::Failed;
                                failureReason =
                                    GameFlowEventReason::HandlerException;
                                appendTrace(0, 0, transition.id,
                                    std::string("guard threw: ")
                                        + exception.what());
                            } catch (...) {
                                guardRecord.outcome =
                                    GameFlowDeterminismOutcome::Failed;
                                failureReason =
                                    GameFlowEventReason::HandlerException;
                                appendTrace(0, 0, transition.id,
                                    "guard threw an unknown exception");
                            }
                        }
                        if (!exchange(guardRecord,
                                GameFlowDeterminismIntentOrigin::GuardCallback)) {
                            return std::nullopt;
                        }
                    }

                    if (guardRecord.outcome
                        == GameFlowDeterminismOutcome::Accepted) {
                        emit(GameFlowEventKind::GuardAccepted,
                            GameFlowEventReason::None, 0, 0,
                            transition.id, intent.id, {},
                            transition.guard.guard);
                        auto decision = record(
                            GameFlowDeterminismKind::TransitionDecision,
                            frames.size() - 1u);
                        decision.intentId = intent.id;
                        decision.transitionId = transition.id;
                        decision.outcome = GameFlowDeterminismOutcome::Accepted;
                        if (!exchange(decision)) return std::nullopt;
                        if (decision.outcome !=
                                GameFlowDeterminismOutcome::Accepted
                            || decision.transitionId != transition.id) {
                            retainDeterminismFault(
                                "GameFlow playback transition decision drifted "
                                "from its guard outcome.");
                            return std::nullopt;
                        }
                        return transitionIndex;
                    }
                    if (guardRecord.outcome
                        == GameFlowDeterminismOutcome::Rejected) {
                        emit(GameFlowEventKind::GuardRejected,
                            GameFlowEventReason::None, 0, 0,
                            transition.id, intent.id, {},
                            transition.guard.guard);
                    } else if (guardRecord.outcome
                        == GameFlowDeterminismOutcome::Failed) {
                        emit(GameFlowEventKind::GuardFailed,
                            failureReason, 0, 0, transition.id, intent.id,
                            {}, transition.guard.guard);
                    } else {
                        retainDeterminismFault(
                            "GameFlow playback supplied an invalid guard outcome.");
                        return std::nullopt;
                    }
                }
            }
            if (state.parent.empty()) break;
            const auto parent = frame.plan->stateIndices.find(state.parent);
            if (parent == frame.plan->stateIndices.end()) break;
            stateIndex = parent->second;
        }
        auto decision = record(GameFlowDeterminismKind::TransitionDecision,
            frames.size() - 1u);
        decision.intentId = intent.id;
        decision.outcome = GameFlowDeterminismOutcome::Unmatched;
        if (!exchange(decision)) return std::nullopt;
        if (decision.outcome != GameFlowDeterminismOutcome::Unmatched
            || !decision.transitionId.empty()) {
            retainDeterminismFault(
                "GameFlow playback supplied an unavailable transition.");
        }
        return std::nullopt;
    }

    void emitActiveAction(Frame& frame,
                          GameFlowEventKind kind,
                          GameFlowEventReason reason,
                          std::size_t frameIndex) noexcept
    {
        if (!frame.active.has_value()) return;
        const ActiveTransition& active = *frame.active;
        const auto& normalized = frame.plan->transitions[
            active.transitionIndex];
        const auto& transition = frame.plan->document.transitions[
            normalized.documentIndex];
        if (active.activeActionIndex >= transition.actions.size()) return;
        const auto& action = transition.actions[active.activeActionIndex];
        if (isGameFlowControlAction(action.action)) return;
        const auto& intent = frame.plan->document.intents[
            active.request.intentIndex];
        emit(kind, reason, active.generation,
            active.pendingAction != 0
                ? active.pendingAction : active.activeActionExecution,
            transition.id, intent.id, action.action, {}, frameIndex);
    }

    void finishTop(GameFlowActionState result, std::string message,
                   GameFlowEventReason reason = GameFlowEventReason::None)
    {
        if (frames.empty() || !frames.back().active.has_value()) return;
        Frame& frame = frames.back();
        const ActiveTransition snapshot = *frame.active;
        const auto& normalized =
            frame.plan->transitions[snapshot.transitionIndex];
        const auto& transition = frame.plan->document.transitions[
            normalized.documentIndex];
        std::size_t routedState = frame.currentStateIndex;
        std::string detail;
        switch (result) {
        case GameFlowActionState::Succeeded:
            routedState = normalized.toState;
            detail = "transition succeeded";
            break;
        case GameFlowActionState::Failed:
            if (normalized.onFailureState != kNoState) {
                routedState = normalized.onFailureState;
            }
            detail = "transition failed";
            break;
        case GameFlowActionState::Cancelled:
            if (normalized.onCancelState != kNoState) {
                routedState = normalized.onCancelState;
            }
            detail = "transition cancelled";
            break;
        case GameFlowActionState::Pending:
            return;
        }
        const auto& intent = frame.plan->document.intents[
            snapshot.request.intentIndex];
        auto terminal = record(GameFlowDeterminismKind::Terminal,
            frames.size() - 1u);
        terminal.generation = snapshot.generation;
        terminal.actionExecutionId = snapshot.pendingAction != 0
            ? snapshot.pendingAction : snapshot.activeActionExecution;
        terminal.transitionId = transition.id;
        terminal.intentId = intent.id;
        terminal.outcome = determinismOutcome(result);
        if (routedState != kNoState) {
            const std::size_t resolved = resolveInitial(*frame.plan, routedState);
            if (resolved != kNoState) {
                terminal.resultStateId = frame.plan->document.states[resolved].id;
            }
        }
        if (!exchange(terminal)) return;
        frame.active.reset();
        frame.currentStateIndex = resolveInitial(*frame.plan, routedState);
        GameFlowEventKind eventKind = GameFlowEventKind::TransitionFailed;
        if (result == GameFlowActionState::Succeeded) {
            eventKind = GameFlowEventKind::TransitionSucceeded;
        } else if (result == GameFlowActionState::Cancelled) {
            eventKind = GameFlowEventKind::TransitionCancelled;
        } else if (reason == GameFlowEventReason::Timeout) {
            eventKind = GameFlowEventKind::TransitionTimedOut;
        }
        emit(eventKind, reason, snapshot.generation,
            snapshot.pendingAction != 0
                ? snapshot.pendingAction : snapshot.activeActionExecution,
            transition.id, intent.id, {}, {}, frames.size() - 1u);
        if (!message.empty()) detail += ": " + message;
        appendTrace(snapshot.generation, snapshot.pendingAction,
            transition.id, std::move(detail));
    }

    void invokeCancellation(Frame& frame, std::size_t frameIndex,
                            GameFlowEventReason reason) noexcept
    {
        if (!frame.active.has_value()) return;
        const bool hasPendingAction = frame.active->pendingAction != 0;
        if (frame.active->onCancel) {
            auto callback = std::move(frame.active->onCancel);
            try {
                ExternalCallbackScope external(*this,
                    GameFlowDeterminismIntentOrigin::CancellationCallback);
                callback();
            } catch (...) {
                // Cancellation is best-effort and never changes the route.
            }
        }
        if (hasPendingAction
            && (reason == GameFlowEventReason::ExplicitCancellation
                || reason == GameFlowEventReason::Timeout)) {
            // Playback has no live cancellation closure. Consume and inject
            // its captured requests while the cancelled frame still exists;
            // buffer the following protocol record for the normal exchange.
            (void)drainCallbackIntents(
                GameFlowDeterminismIntentOrigin::CancellationCallback);
        }
        if (hasPendingAction) {
            emitActiveAction(frame, GameFlowEventKind::ActionCancelled,
                reason, frameIndex);
        }
    }

    void cancelFramesFrom(std::size_t first,
                          GameFlowEventReason reason =
                              GameFlowEventReason::ExplicitCancellation) noexcept
    {
        for (std::size_t index = frames.size(); index > first; --index) {
            Frame& frame = frames[index - 1u];
            invokeCancellation(frame, index - 1u, reason);
            if (determinismFaulted) return;
            if (index - 1u > first || first > 0u) {
                GameFlowGeneration generation = 0;
                GameFlowActionExecutionId executionId = 0;
                std::string_view transition;
                std::string_view intent;
                if (frame.active.has_value()) {
                    const ActiveTransition& active = *frame.active;
                    const auto& normalized = frame.plan->transitions[
                        active.transitionIndex];
                    transition = frame.plan->document.transitions[
                        normalized.documentIndex].id;
                    intent = frame.plan->document.intents[
                        active.request.intentIndex].id;
                    generation = active.generation;
                    executionId = active.pendingAction != 0
                        ? active.pendingAction : active.activeActionExecution;
                }
                emit(GameFlowEventKind::SubflowCancelled,
                    reason, generation, executionId,
                    transition, intent, {}, {}, index - 1u);
            }
        }
    }

    bool enterSubflow(const GameFlowActionCall& action)
    {
        Frame& parent = frames.back();
        ActiveTransition& active = *parent.active;
        const auto& transition = parent.plan->document.transitions[
            parent.plan->transitions[active.transitionIndex].documentIndex];
        const auto idValue = action.arguments.find(
            std::string(kGameFlowSubflowIdArgument));
        const auto* subflowId = idValue == action.arguments.end()
            ? nullptr : std::get_if<std::string>(&idValue->second.data);
        if (program == nullptr || subflowId == nullptr || subflowId->empty()) {
            finishTop(GameFlowActionState::Failed,
                "flow.enter has no resolvable subflowId",
                GameFlowEventReason::InvalidContract);
            return false;
        }
        const GameFlowPlan* child = program->findPlan(*subflowId);
        if (child == nullptr) {
            finishTop(GameFlowActionState::Failed,
                "subflow '" + *subflowId + "' is unavailable",
                GameFlowEventReason::SubflowUnavailable);
            return false;
        }
        if (frames.size() >= program->maxCallDepth) {
            finishTop(GameFlowActionState::Failed,
                "subflow call depth limit reached",
                GameFlowEventReason::CallDepthLimit);
            return false;
        }
        if (std::any_of(frames.begin(), frames.end(),
                [subflowId](const Frame& frame) {
                    return frame.plan->document.id == *subflowId;
        })) {
            finishTop(GameFlowActionState::Failed,
                "recursive subflow call rejected for '" + *subflowId + "'",
                GameFlowEventReason::RecursiveCall);
            return false;
        }

        GameFlowPayload parameters = action.arguments;
        std::string error;
        if (!normalizePayload(child->document.entryParameters,
                "Subflow '" + *subflowId + "' entry", parameters,
                &active.request.payload, kGameFlowSubflowIdArgument, error)) {
            finishTop(GameFlowActionState::Failed, std::move(error),
                GameFlowEventReason::InvalidContract);
            return false;
        }

        const GameFlowActionExecutionId executionId =
            nextActionExecutionId++;
        auto deterministic = record(GameFlowDeterminismKind::SubflowEnter,
            frames.size() - 1u);
        deterministic.generation = active.generation;
        deterministic.actionExecutionId = executionId;
        deterministic.transitionId = transition.id;
        deterministic.intentId = parent.plan->document.intents[
            active.request.intentIndex].id;
        deterministic.targetFlowId = *subflowId;
        deterministic.payload = parameters;
        if (!exchange(deterministic)) return false;
        if (determinism != nullptr
            && determinism->mode() == GameFlowDeterminismMode::Playback) {
            GameFlowPayload normalizedPlayback = deterministic.payload;
            if (!normalizePayload(child->document.entryParameters,
                    "Recorded subflow entry", normalizedPlayback, nullptr, {},
                    error)
                || normalizedPlayback != deterministic.payload) {
                retainDeterminismFault(error.empty()
                    ? "Recorded subflow entry payload is not canonical."
                    : std::move(error));
                return false;
            }
            parameters = std::move(deterministic.payload);
        }

        parent.lastSubflowResult.clear();
        parent.returnedFlowId.clear();
        active.activeActionExecution = executionId;
        active.waitingForSubflow = true;
        const GameFlowGeneration generation = active.generation;
        const std::string transitionId = transition.id;
        const std::string intentId = parent.plan->document.intents[
            active.request.intentIndex].id;
        appendTrace(generation, executionId, transitionId,
            "entering subflow '" + *subflowId + "'");
        frames.push_back(makeFrame(child, std::move(parameters)));
        emit(GameFlowEventKind::SubflowEntered,
            GameFlowEventReason::None, generation, executionId,
            transitionId, intentId, {}, {}, frames.size() - 1u);
        appendTrace(0, 0, {}, "subflow entered");
        return true;
    }

    bool returnFromSubflow(const GameFlowActionCall& action)
    {
        if (frames.size() <= 1u) {
            finishTop(GameFlowActionState::Failed,
                "flow.return cannot execute in the root flow",
                GameFlowEventReason::RootReturn);
            return false;
        }
        Frame& child = frames.back();
        ActiveTransition& childActive = *child.active;
        const auto& childTransition = child.plan->document.transitions[
            child.plan->transitions[
                childActive.transitionIndex].documentIndex];
        GameFlowPayload result = action.arguments;
        std::string error;
        if (!normalizePayload(child.plan->document.result,
                "Subflow '" + child.plan->document.id + "' result", result,
                &childActive.request.payload, {}, error)) {
            finishTop(GameFlowActionState::Failed, std::move(error),
                GameFlowEventReason::InvalidContract);
            return false;
        }

        const GameFlowActionExecutionId executionId =
            nextActionExecutionId++;
        auto deterministic = record(GameFlowDeterminismKind::SubflowReturn,
            frames.size() - 1u);
        deterministic.generation = childActive.generation;
        deterministic.actionExecutionId = executionId;
        deterministic.transitionId = childTransition.id;
        deterministic.intentId = child.plan->document.intents[
            childActive.request.intentIndex].id;
        deterministic.targetFlowId = child.plan->document.id;
        deterministic.payload = result;
        if (!exchange(deterministic)) return false;
        if (determinism != nullptr
            && determinism->mode() == GameFlowDeterminismMode::Playback) {
            GameFlowPayload normalizedPlayback = deterministic.payload;
            if (!normalizePayload(child.plan->document.result,
                    "Recorded subflow result", normalizedPlayback, nullptr, {},
                    error)
                || normalizedPlayback != deterministic.payload) {
                retainDeterminismFault(error.empty()
                    ? "Recorded subflow result payload is not canonical."
                    : std::move(error));
                return false;
            }
            result = std::move(deterministic.payload);
        }
        appendTrace(childActive.generation, executionId,
            childTransition.id, "returning from subflow");
        emit(GameFlowEventKind::SubflowReturned,
            GameFlowEventReason::None, childActive.generation, executionId,
            childTransition.id,
            child.plan->document.intents[
                childActive.request.intentIndex].id,
            {}, {}, frames.size() - 1u);
        const std::string returnedFlow = child.plan->document.id;
        child.active.reset();
        child.requests.clear();
        frames.pop_back();

        Frame& parent = frames.back();
        if (!parent.active.has_value()
            || !parent.active->waitingForSubflow) {
            emit(GameFlowEventKind::ConfigurationRejected,
                GameFlowEventReason::MissingCaller);
            appendTrace(0, 0, {},
                "subflow return found no suspended caller");
            return false;
        }
        parent.lastSubflowResult = std::move(result);
        parent.returnedFlowId = returnedFlow;
        parent.active->waitingForSubflow = false;
        parent.active->activeActionIndex = kNoGameFlowActionIndex;
        parent.active->activeActionExecution = 0;
        parent.active->pendingAction = 0;
        parent.active->onCancel = {};
        const auto& parentTransition = parent.plan->document.transitions[
            parent.plan->transitions[
                parent.active->transitionIndex].documentIndex];
        appendTrace(parent.active->generation, executionId,
            parentTransition.id,
            "subflow '" + returnedFlow + "' returned");
        driveActions();
        return true;
    }

    void driveActions()
    {
        while (!frames.empty() && frames.back().active.has_value()) {
            Frame& frame = frames.back();
            ActiveTransition& active = *frame.active;
            if (active.waitingForSubflow) return;
            const auto& normalized =
                frame.plan->transitions[active.transitionIndex];
            const auto& transition = frame.plan->document.transitions[
                normalized.documentIndex];
            if (active.nextAction >= transition.actions.size()) {
                finishTop(GameFlowActionState::Succeeded, {});
                return;
            }

            active.activeActionIndex = active.nextAction++;
            const auto& action = transition.actions[active.activeActionIndex];
            if (action.action == kGameFlowActionEnter) {
                (void)enterSubflow(action);
                return;
            }
            if (action.action == kGameFlowActionReturn) {
                (void)returnFromSubflow(action);
                return;
            }

            const auto* definition = registry->findAction(action.action);
            const auto* handler = registry->findActionHandler(action.action);
            if (definition == nullptr) {
                emitActiveAction(frame, GameFlowEventKind::ActionFailed,
                    GameFlowEventReason::MissingHandler, frames.size() - 1u);
                finishTop(GameFlowActionState::Failed,
                    "action '" + action.action + "' is unavailable",
                    GameFlowEventReason::MissingHandler);
                return;
            }

            const GameFlowActionExecutionId executionId =
                nextActionExecutionId++;
            active.activeActionExecution = executionId;
            GameFlowActionResult result;
            GameFlowEventReason resultReason = GameFlowEventReason::None;
            const auto& intent = frame.plan->document.intents[
                active.request.intentIndex];
            emitActiveAction(frame, GameFlowEventKind::ActionStarted,
                GameFlowEventReason::None, frames.size() - 1u);
            appendTrace(active.generation, executionId, transition.id,
                "starting action '" + action.action + "'");

            auto deterministic = record(
                GameFlowDeterminismKind::ActionOutcome,
                frames.size() - 1u);
            deterministic.generation = active.generation;
            deterministic.actionExecutionId = executionId;
            deterministic.transitionId = transition.id;
            deterministic.intentId = intent.id;
            deterministic.actionId = action.action;
            if (determinism != nullptr
                && determinism->mode() == GameFlowDeterminismMode::Playback) {
                if (!exchange(deterministic,
                        GameFlowDeterminismIntentOrigin::ActionCallback)) {
                    return;
                }
                const auto recordedState = actionState(deterministic.outcome);
                if (!recordedState.has_value()) {
                    retainDeterminismFault(
                        "GameFlow playback supplied an invalid action outcome.");
                    return;
                }
                result.state = *recordedState;
            } else {
                if (handler == nullptr) {
                    resultReason = GameFlowEventReason::MissingHandler;
                    result = GameFlowActionResult::failed(
                        "registered action handler is unavailable");
                } else {
                    try {
                        const GameFlowActionInvocation invocation{
                            active.generation,
                            executionId,
                            frame.plan->document.id,
                            transition.id,
                            intent.id,
                            &active.request.payload,
                            &action.arguments,
                            &frame.parameters,
                            &frame.lastSubflowResult,
                            frame.returnedFlowId,
                        };
                        ExternalCallbackScope callback(*this,
                            GameFlowDeterminismIntentOrigin::ActionCallback);
                        result = (*handler)(invocation);
                    } catch (const std::exception& exception) {
                        resultReason = GameFlowEventReason::HandlerException;
                        result = GameFlowActionResult::failed(
                            std::string("action threw: ") + exception.what());
                    } catch (...) {
                        resultReason = GameFlowEventReason::HandlerException;
                        result = GameFlowActionResult::failed(
                            "action threw an unknown exception");
                    }
                }
                deterministic.outcome = determinismOutcome(result.state);
                if (!exchange(deterministic,
                        GameFlowDeterminismIntentOrigin::ActionCallback)) {
                    return;
                }
            }

            if (result.state == GameFlowActionState::Pending) {
                if (!definition->asynchronous) {
                    emitActiveAction(frame, GameFlowEventKind::ActionFailed,
                        GameFlowEventReason::InvalidAsyncResult,
                        frames.size() - 1u);
                    finishTop(GameFlowActionState::Failed,
                        "synchronous action '" + action.action
                            + "' returned Pending",
                        GameFlowEventReason::InvalidAsyncResult);
                    return;
                }
                active.pendingAction = executionId;
                active.onCancel = std::move(result.onCancel);
                emitActiveAction(frame, GameFlowEventKind::ActionPending,
                    GameFlowEventReason::None, frames.size() - 1u);
                appendTrace(active.generation, executionId, transition.id,
                    "action pending");
                return;
            }
            if (result.state == GameFlowActionState::Succeeded) {
                emitActiveAction(frame, GameFlowEventKind::ActionSucceeded,
                    GameFlowEventReason::None, frames.size() - 1u);
                active.activeActionIndex = kNoGameFlowActionIndex;
                active.activeActionExecution = 0;
                continue;
            }
            emitActiveAction(frame,
                result.state == GameFlowActionState::Cancelled
                    ? GameFlowEventKind::ActionCancelled
                    : GameFlowEventKind::ActionFailed,
                resultReason, frames.size() - 1u);
            finishTop(result.state, std::move(result.message), resultReason);
            return;
        }
    }

    void acceptActionResult(GameFlowActionResult result)
    {
        if (frames.empty() || !frames.back().active.has_value()) return;
        Frame& frame = frames.back();
        ActiveTransition& active = *frame.active;
        if (result.state == GameFlowActionState::Succeeded) {
            emitActiveAction(frame, GameFlowEventKind::ActionSucceeded,
                GameFlowEventReason::None, frames.size() - 1u);
            active.pendingAction = 0;
            active.activeActionIndex = kNoGameFlowActionIndex;
            active.activeActionExecution = 0;
            active.onCancel = {};
            driveActions();
            return;
        }
        if (result.state == GameFlowActionState::Pending) return;
        emitActiveAction(frame,
            result.state == GameFlowActionState::Cancelled
                ? GameFlowEventKind::ActionCancelled
                : GameFlowEventKind::ActionFailed,
            GameFlowEventReason::None, frames.size() - 1u);
        active.pendingAction = 0;
        active.onCancel = {};
        finishTop(result.state, std::move(result.message));
    }

    void beginTop(std::size_t transitionIndex, IntentRequest request)
    {
        Frame& frame = frames.back();
        const auto& normalized = frame.plan->transitions[transitionIndex];
        const auto& transition = frame.plan->document.transitions[
            normalized.documentIndex];
        ActiveTransition value;
        value.generation = nextGeneration++;
        value.transitionIndex = transitionIndex;
        value.request = std::move(request);
        frame.lastSubflowResult.clear();
        frame.returnedFlowId.clear();
        frame.active = std::move(value);
        const auto& intent = frame.plan->document.intents[
            frame.active->request.intentIndex];
        emit(GameFlowEventKind::TransitionStarted,
            GameFlowEventReason::None, frame.active->generation, 0,
            transition.id, intent.id, {}, {}, frames.size() - 1u);
        appendTrace(frame.active->generation, 0, transition.id,
            "transition started");
        driveActions();
    }

    std::size_t queuedCount() const noexcept
    {
        std::size_t count = 0;
        for (const auto& frame : frames) count += frame.requests.size();
        return count;
    }

    bool anyActive() const noexcept
    {
        return std::any_of(frames.begin(), frames.end(),
            [](const Frame& frame) { return frame.active.has_value(); });
    }
};

GameFlowCoordinator::GameFlowCoordinator()
    : _impl(std::make_unique<Impl>())
{
}

GameFlowCoordinator::~GameFlowCoordinator() = default;
GameFlowCoordinator::GameFlowCoordinator(GameFlowCoordinator&&) noexcept = default;
GameFlowCoordinator& GameFlowCoordinator::operator=(
    GameFlowCoordinator&&) noexcept = default;

bool GameFlowCoordinator::setPlan(
    const GameFlowPlan* plan,
    const GameFlowActionRegistry* registry,
    std::string* error)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed(error)) return false;
    reset();
    if (plan == nullptr || registry == nullptr) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) *error = "GameFlow plan and registry are required.";
        return false;
    }
    if (hasControlActions(*plan)) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) {
            *error = "A GameFlowProgram is required for flow.enter/flow.return.";
        }
        return false;
    }
    const auto initial = plan->stateIndices.find(plan->document.initialState);
    if (initial == plan->stateIndices.end()) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) *error = "GameFlow plan has no valid initial state.";
        return false;
    }
    const std::size_t resolvedInitial = _impl->resolveInitial(
        *plan, initial->second);
    if (resolvedInitial == kNoState) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) {
            *error = "GameFlow plan has no valid resolved initial state.";
        }
        return false;
    }
    GameFlowDeterminismRecord manifest;
    manifest.kind = GameFlowDeterminismKind::ProgramManifest;
    manifest.flowId = plan->document.id;
    manifest.stateId = plan->document.states[resolvedInitial].id;
    manifest.programFingerprint = gameFlowPlanFingerprint(*plan);
    if (!_impl->exchange(manifest)) {
        if (error != nullptr) *error = _impl->determinismFault;
        return false;
    }
    _impl->legacyPlan = plan;
    _impl->registry = registry;
    _impl->frames.push_back(_impl->makeFrame(plan, {}));
    _impl->emit(GameFlowEventKind::ProgramInitialized);
    _impl->appendTrace(0, 0, {}, "flow initialized");
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowCoordinator::setProgram(
    const GameFlowProgram* program,
    const GameFlowActionRegistry* registry,
    GameFlowPayload rootParameters,
    std::string* error)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed(error)) return false;
    reset();
    if (program == nullptr || registry == nullptr) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) {
            *error = "GameFlow program and registry are required.";
        }
        return false;
    }
    const GameFlowPlan* root = program->findPlan(program->rootFlowId);
    if (root == nullptr) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) *error = "GameFlow program has no root plan.";
        return false;
    }
    std::string localError;
    if (!normalizePayload(root->document.entryParameters,
            "Root flow entry", rootParameters, nullptr, {}, localError)) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidContract);
        if (error != nullptr) *error = std::move(localError);
        return false;
    }
    const auto initial = root->stateIndices.find(root->document.initialState);
    const std::size_t resolvedInitial = initial == root->stateIndices.end()
        ? kNoState : _impl->resolveInitial(*root, initial->second);
    if (resolvedInitial == kNoState) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) {
            *error = "GameFlow root plan has no valid initial state.";
        }
        return false;
    }
    GameFlowDeterminismRecord manifest;
    manifest.kind = GameFlowDeterminismKind::ProgramManifest;
    manifest.flowId = root->document.id;
    manifest.stateId = root->document.states[resolvedInitial].id;
    manifest.programFingerprint = gameFlowProgramFingerprint(*program);
    manifest.payload = rootParameters;
    if (!_impl->exchange(manifest)) {
        if (error != nullptr) *error = _impl->determinismFault;
        return false;
    }
    _impl->program = program;
    _impl->registry = registry;
    _impl->frames.push_back(_impl->makeFrame(root, std::move(rootParameters)));
    _impl->emit(GameFlowEventKind::ProgramInitialized);
    _impl->appendTrace(0, 0, {}, "program initialized");
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowCoordinator::replaceProgram(
    const GameFlowProgram* program,
    const GameFlowActionRegistry* registry,
    GameFlowPayload rootParameters,
    std::string* error)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed(error)) return false;
    if (!reloadSafePoint()) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::UnsafeReloadPoint);
        if (error != nullptr) *error = "GameFlow is not at a reload safe point.";
        return false;
    }
    if (program == nullptr || registry == nullptr) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) {
            *error = "GameFlow program and registry are required.";
        }
        return false;
    }
    const GameFlowPlan* root = program->findPlan(program->rootFlowId);
    if (root == nullptr) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) *error = "GameFlow program has no root plan.";
        return false;
    }
    std::string localError;
    if (!normalizePayload(root->document.entryParameters,
            "Root flow entry", rootParameters, nullptr, {}, localError)) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidContract);
        if (error != nullptr) *error = std::move(localError);
        return false;
    }

    const std::string previousFlow = std::string(currentFlow());
    const std::string previousState = std::string(currentState());
    const auto initial = root->stateIndices.find(root->document.initialState);
    std::size_t candidateState = initial == root->stateIndices.end()
        ? kNoState : _impl->resolveInitial(*root, initial->second);
    if (candidateState == kNoState) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) {
            *error = "GameFlow root plan has no valid initial state.";
        }
        return false;
    }

    bool preserved = false;
    if (root->document.id == previousFlow) {
        const auto state = root->stateIndices.find(previousState);
        if (state != root->stateIndices.end()) {
            candidateState = _impl->resolveInitial(*root, state->second);
            preserved = candidateState != kNoState;
        }
    }

    GameFlowDeterminismRecord manifest;
    manifest.kind = GameFlowDeterminismKind::ProgramManifest;
    manifest.flowId = root->document.id;
    manifest.stateId = root->document.states[candidateState].id;
    manifest.programFingerprint = gameFlowProgramFingerprint(*program);
    manifest.payload = rootParameters;
    if (!_impl->exchange(manifest)) {
        if (error != nullptr) *error = _impl->determinismFault;
        return false;
    }
    Impl::Frame candidate = _impl->makeFrame(
        root, std::move(rootParameters));
    candidate.currentStateIndex = candidateState;

    _impl->program = program;
    _impl->legacyPlan = nullptr;
    _impl->registry = registry;
    _impl->frames.clear();
    _impl->frames.push_back(std::move(candidate));
    _impl->emit(GameFlowEventKind::ProgramReloaded);
    _impl->appendTrace(0, 0, {}, preserved
        ? "program reloaded; root state preserved"
        : "program reloaded; root state reset");
    if (error != nullptr) error->clear();
    return true;
}

void GameFlowCoordinator::reset() noexcept
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed()) return;
    const bool wasReady = !_impl->frames.empty();
    _impl->cancelFramesFrom(0u, GameFlowEventReason::RuntimeReset);
    if (_impl->determinismFaulted) return;
    if (wasReady) _impl->emit(GameFlowEventKind::CoordinatorReset);
    _impl->frames.clear();
    _impl->program = nullptr;
    _impl->legacyPlan = nullptr;
    _impl->registry = nullptr;
}

GameFlowRequestResult GameFlowCoordinator::request(
    std::string_view intent,
    GameFlowPayload payload)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (_impl->callbackOrigin
        == GameFlowDeterminismIntentOrigin::DeterminismExchange) {
        return {GameFlowRequestState::NotReady,
            "GameFlow request() cannot reenter its determinism exchange."};
    }
    if (_impl->determinismFaulted) {
        return {GameFlowRequestState::DeterminismFault,
            std::string(_impl->determinismFault.empty()
                ? "GameFlow determinism exchange failed."
                : _impl->determinismFault)};
    }
    if (_impl->frames.empty() || _impl->registry == nullptr) {
        _impl->emit(GameFlowEventKind::IntentRejected,
            GameFlowEventReason::NotReady);
        return {GameFlowRequestState::NotReady,
            "GameFlow coordinator has no plan."};
    }
    auto& frame = _impl->frames.back();
    const auto found = frame.plan->intentIndices.find(intent);
    if (found == frame.plan->intentIndices.end()) {
        _impl->emit(GameFlowEventKind::IntentRejected,
            GameFlowEventReason::UnknownIntent);
        return {GameFlowRequestState::UnknownIntent,
            "Intent '" + std::string(intent) + "' is not declared by flow '"
                + frame.plan->document.id + "'."};
    }
    std::string error;
    if (!normalizeIntentPayload(
            frame.plan->document.intents[found->second], payload, error)) {
        _impl->emit(GameFlowEventKind::IntentRejected,
            GameFlowEventReason::InvalidPayload, 0, 0, {},
            frame.plan->document.intents[found->second].id);
        return {GameFlowRequestState::InvalidPayload, std::move(error)};
    }
    auto deterministic = _impl->record(GameFlowDeterminismKind::Intent,
        _impl->frames.size() - 1u);
    deterministic.intentId = frame.plan->document.intents[found->second].id;
    deterministic.payload = payload;
    deterministic.intentOrigin = _impl->callbackOrigin;
    if (!_impl->exchange(deterministic)) {
        return {GameFlowRequestState::DeterminismFault,
            std::string(_impl->determinismFault.empty()
                ? "GameFlow determinism exchange failed."
                : _impl->determinismFault)};
    }
    if (_impl->determinism != nullptr
        && _impl->determinism->mode()
            == GameFlowDeterminismMode::Playback) {
        payload = std::move(deterministic.payload);
    }
    frame.requests.push_back({found->second, std::move(payload)});
    _impl->emit(GameFlowEventKind::IntentQueued,
        GameFlowEventReason::None, 0, 0, {},
        frame.plan->document.intents[found->second].id);
    return {GameFlowRequestState::Queued, {}};
}

void GameFlowCoordinator::update(double deltaSeconds)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed()) return;
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0) deltaSeconds = 0.0;
    if (_impl->frames.empty()) return;
    auto deterministic = _impl->record(GameFlowDeterminismKind::Update,
        _impl->frames.size() - 1u);
    deterministic.deltaSeconds = deltaSeconds;
    if (!_impl->exchange(deterministic)) return;
    if (_impl->determinism != nullptr
        && _impl->determinism->mode()
            == GameFlowDeterminismMode::Playback) {
        deltaSeconds = deterministic.deltaSeconds;
    }
    Impl::UpdateTimingScope timing(*_impl, deltaSeconds);
    Impl::DispatchScope dispatch(_impl->dispatchDepth);

    std::size_t expired = kNoState;
    for (std::size_t index = 0; index < _impl->frames.size(); ++index) {
        auto& frame = _impl->frames[index];
        if (!frame.active.has_value()) continue;
        const auto& transition = frame.plan->document.transitions[
            frame.plan->transitions[
                frame.active->transitionIndex].documentIndex];
        frame.active->elapsedSeconds += deltaSeconds;
        if (expired == kNoState && transition.timeoutSeconds > 0.0
            && frame.active->elapsedSeconds >= transition.timeoutSeconds) {
            expired = index;
        }
    }
    if (expired != kNoState) {
        _impl->cancelFramesFrom(expired, GameFlowEventReason::Timeout);
        if (_impl->determinismFaulted) return;
        _impl->frames.resize(expired + 1u);
        _impl->finishTop(GameFlowActionState::Failed,
            "transition timed out", GameFlowEventReason::Timeout);
    }

    std::size_t safetyCounter = 0;
    while (!_impl->frames.empty()
           && !_impl->frames.back().active.has_value()
           && !_impl->frames.back().requests.empty()
           && safetyCounter++ < 1024u) {
        auto& frame = _impl->frames.back();
        auto request = frame.requests.front();
        const auto selected = _impl->selectTransition(frame, request);
        if (_impl->determinismFaulted) return;
        frame.requests.pop_front();
        if (!selected.has_value()) {
            const auto& intent = frame.plan->document.intents[
                request.intentIndex];
            _impl->emit(GameFlowEventKind::IntentUnmatched,
                GameFlowEventReason::NoMatchingTransition, 0, 0, {},
                intent.id);
            _impl->appendTrace(0, 0, {},
                "no transition accepted intent '" + intent.id + "'");
            continue;
        }
        _impl->beginTop(*selected, std::move(request));
    }
}

bool GameFlowCoordinator::completeAction(
    GameFlowActionExecutionId executionId,
    GameFlowActionResult result,
    std::string* error)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed(error)) return false;
    if (_impl->frames.empty()
        || !_impl->frames.back().active.has_value()
        || _impl->frames.back().active->pendingAction == 0
        || _impl->frames.back().active->pendingAction != executionId) {
        _impl->emit(GameFlowEventKind::ActionCompletionRejected,
            GameFlowEventReason::StaleCompletion, 0, executionId);
        if (error != nullptr) {
            *error = "Action completion is stale or does not match pending work.";
        }
        return false;
    }
    if (result.state == GameFlowActionState::Pending) {
        _impl->emit(GameFlowEventKind::ActionCompletionRejected,
            GameFlowEventReason::InvalidAsyncResult, 0, executionId);
        if (error != nullptr) *error = "A completion result cannot remain Pending.";
        return false;
    }
    auto& frame = _impl->frames.back();
    const auto& active = *frame.active;
    const auto& normalized = frame.plan->transitions[active.transitionIndex];
    const auto& transition = frame.plan->document.transitions[
        normalized.documentIndex];
    auto deterministic = _impl->record(
        GameFlowDeterminismKind::AsyncCompletion,
        _impl->frames.size() - 1u);
    deterministic.generation = active.generation;
    deterministic.actionExecutionId = executionId;
    deterministic.transitionId = transition.id;
    deterministic.intentId = frame.plan->document.intents[
        active.request.intentIndex].id;
    if (active.activeActionIndex < transition.actions.size()) {
        deterministic.actionId = transition.actions[
            active.activeActionIndex].action;
    }
    deterministic.outcome = determinismOutcome(result.state);
    if (!_impl->exchange(deterministic)) {
        if (error != nullptr) *error = determinismFault();
        return false;
    }
    if (_impl->determinism != nullptr
        && _impl->determinism->mode()
            == GameFlowDeterminismMode::Playback) {
        const auto recorded = actionState(deterministic.outcome);
        if (!recorded.has_value()
            || *recorded == GameFlowActionState::Pending) {
            _impl->retainDeterminismFault(
                "GameFlow playback supplied an invalid async completion.");
            if (error != nullptr) *error = determinismFault();
            return false;
        }
        result.state = *recorded;
        result.message.clear();
        result.onCancel = {};
    }
    if (error != nullptr) error->clear();
    _impl->acceptActionResult(std::move(result));
    return true;
}

bool GameFlowCoordinator::cancelActive(std::string message)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed()) return false;
    if (_impl->frames.empty()
        || !_impl->frames.back().active.has_value()) return false;
    auto& frame = _impl->frames.back();
    const auto& active = *frame.active;
    const auto& transition = frame.plan->document.transitions[
        frame.plan->transitions[active.transitionIndex].documentIndex];
    auto deterministic = _impl->record(
        GameFlowDeterminismKind::Cancellation,
        _impl->frames.size() - 1u);
    deterministic.cancellation =
        GameFlowDeterminismCancellation::ActiveTransition;
    deterministic.generation = active.generation;
    deterministic.actionExecutionId = active.pendingAction;
    deterministic.transitionId = transition.id;
    deterministic.intentId = frame.plan->document.intents[
        active.request.intentIndex].id;
    if (!_impl->exchange(deterministic)) return false;
    _impl->invokeCancellation(_impl->frames.back(),
        _impl->frames.size() - 1u,
        GameFlowEventReason::ExplicitCancellation);
    if (_impl->determinismFaulted) return false;
    _impl->finishTop(GameFlowActionState::Cancelled, std::move(message),
        GameFlowEventReason::ExplicitCancellation);
    return true;
}

bool GameFlowCoordinator::cancelSubflowCall(std::string message)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed()) return false;
    if (_impl->frames.size() <= 1u) return false;
    const auto& child = _impl->frames.back();
    auto deterministic = _impl->record(
        GameFlowDeterminismKind::Cancellation,
        _impl->frames.size() - 1u);
    deterministic.cancellation = GameFlowDeterminismCancellation::SubflowCall;
    deterministic.targetFlowId = child.plan->document.id;
    if (child.active.has_value()) {
        const auto& active = *child.active;
        const auto& transition = child.plan->document.transitions[
            child.plan->transitions[active.transitionIndex].documentIndex];
        deterministic.generation = active.generation;
        deterministic.actionExecutionId = active.pendingAction;
        deterministic.transitionId = transition.id;
        deterministic.intentId = child.plan->document.intents[
            active.request.intentIndex].id;
    }
    if (!_impl->exchange(deterministic)) return false;
    const std::size_t childIndex = _impl->frames.size() - 1u;
    _impl->cancelFramesFrom(childIndex);
    if (_impl->determinismFaulted) return false;
    _impl->frames.resize(childIndex);
    if (!_impl->frames.back().active.has_value()
        || !_impl->frames.back().active->waitingForSubflow) return false;
    _impl->finishTop(GameFlowActionState::Cancelled, std::move(message),
        GameFlowEventReason::ExplicitCancellation);
    return true;
}

std::string_view GameFlowCoordinator::currentState() const noexcept
{
    if (_impl->frames.empty()) return {};
    const auto& frame = _impl->frames.back();
    if (frame.currentStateIndex == kNoState) return {};
    return frame.plan->document.states[frame.currentStateIndex].id;
}

std::string_view GameFlowCoordinator::currentFlow() const noexcept
{
    return _impl->frames.empty()
        ? std::string_view{} : std::string_view(
            _impl->frames.back().plan->document.id);
}

std::string GameFlowCoordinator::qualifiedState() const
{
    if (_impl->frames.empty()) return {};
    return std::string(currentFlow()) + "::" + std::string(currentState());
}

std::size_t GameFlowCoordinator::callDepth() const noexcept
{
    return _impl->frames.empty() ? 0u : _impl->frames.size() - 1u;
}

std::string_view GameFlowCoordinator::activeTransition() const noexcept
{
    if (_impl->frames.empty()
        || !_impl->frames.back().active.has_value()
        || _impl->frames.back().active->waitingForSubflow) return {};
    const auto& frame = _impl->frames.back();
    const auto& normalized =
        frame.plan->transitions[frame.active->transitionIndex];
    return frame.plan->document.transitions[normalized.documentIndex].id;
}

GameFlowGeneration GameFlowCoordinator::activeGeneration() const noexcept
{
    if (_impl->frames.empty()
        || !_impl->frames.back().active.has_value()) return 0;
    return _impl->frames.back().active->generation;
}

GameFlowActionExecutionId GameFlowCoordinator::pendingAction() const noexcept
{
    if (_impl->frames.empty()
        || !_impl->frames.back().active.has_value()) return 0;
    return _impl->frames.back().active->pendingAction;
}

std::size_t GameFlowCoordinator::queuedIntentCount() const noexcept
{
    return _impl->queuedCount();
}

bool GameFlowCoordinator::busy() const noexcept
{
    return _impl->anyActive();
}

bool GameFlowCoordinator::reloadSafePoint() const noexcept
{
    return _impl->dispatchDepth == 0u
        && _impl->frames.size() == 1u
        && !_impl->frames.front().active.has_value()
        && _impl->frames.front().requests.empty();
}

bool GameFlowCoordinator::restoreState(
    std::string_view flowId,
    std::string_view stateId,
    std::string* error)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (!_impl->mutationAllowed(error)) return false;
    if (!reloadSafePoint()) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::UnsafeReloadPoint);
        if (error != nullptr) *error = "GameFlow is not at a reload safe point.";
        return false;
    }
    auto& frame = _impl->frames.front();
    if (frame.plan->document.id != flowId) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) *error = "Reloaded GameFlow root id changed.";
        return false;
    }
    const auto state = frame.plan->stateIndices.find(stateId);
    if (state == frame.plan->stateIndices.end()) {
        _impl->emit(GameFlowEventKind::ConfigurationRejected,
            GameFlowEventReason::InvalidConfiguration);
        if (error != nullptr) {
            *error = "Reloaded GameFlow does not contain state '"
                + std::string(stateId) + "'.";
        }
        return false;
    }
    frame.currentStateIndex = _impl->resolveInitial(*frame.plan, state->second);
    _impl->emit(GameFlowEventKind::StateRestored,
        GameFlowEventReason::None, 0, 0, {}, {}, {}, {}, 0u);
    _impl->appendTrace(0, 0, {}, "state restored after reload", 0u);
    if (error != nullptr) error->clear();
    return true;
}

GameFlowCoordinatorSnapshot GameFlowCoordinator::snapshot() const
{
    GameFlowCoordinatorSnapshot result;
    result.currentStateId = currentState();
    result.currentFlowId = currentFlow();
    result.qualifiedStateId = qualifiedState();
    result.callDepth = callDepth();
    result.queuedIntentCount = _impl->queuedCount();
    result.busy = _impl->anyActive();
    result.frames.reserve(_impl->frames.size());
    for (const auto& frame : _impl->frames) {
        GameFlowStackFrameSnapshot snapshot;
        snapshot.instanceSerial = frame.instanceSerial;
        snapshot.flowId = frame.plan->document.id;
        if (frame.currentStateIndex != kNoState) {
            snapshot.stateId = frame.plan->document.states[
                frame.currentStateIndex].id;
        }
        if (frame.active.has_value() && frame.active->waitingForSubflow) {
            const auto& normalized = frame.plan->transitions[
                frame.active->transitionIndex];
            snapshot.suspendedTransitionId =
                frame.plan->document.transitions[
                    normalized.documentIndex].id;
        }
        result.frames.push_back(std::move(snapshot));
    }

    if (_impl->frames.empty() || _impl->registry == nullptr) {
        result.status = GameFlowCoordinatorStatus::NotReady;
        return result;
    }
    const auto& frame = _impl->frames.back();
    if (!frame.active.has_value()) {
        if (!frame.requests.empty()) {
            result.status = GameFlowCoordinatorStatus::Queued;
        } else if (_impl->frames.size() > 1u) {
            result.status = GameFlowCoordinatorStatus::WaitingForSubflow;
        } else {
            result.status = GameFlowCoordinatorStatus::Idle;
        }
        return result;
    }
    if (frame.active->waitingForSubflow) {
        result.status = GameFlowCoordinatorStatus::WaitingForSubflow;
        return result;
    }

    const auto& active = *frame.active;
    const auto& normalized = frame.plan->transitions[active.transitionIndex];
    const auto& transition = frame.plan->document.transitions[
        normalized.documentIndex];
    result.status = active.pendingAction == 0
        ? GameFlowCoordinatorStatus::Running
        : GameFlowCoordinatorStatus::WaitingForAction;
    result.activeTransitionId = transition.id;
    result.generation = active.generation;
    result.executionId = active.activeActionExecution;
    result.activeActionIndex = active.activeActionIndex;
    if (active.activeActionIndex < transition.actions.size()) {
        result.activeActionId = transition.actions[
            active.activeActionIndex].action;
    }
    return result;
}

const std::vector<GameFlowTraceEntry>& GameFlowCoordinator::trace() const noexcept
{
    return _impl->traces;
}

void GameFlowCoordinator::clearTrace() noexcept
{
    _impl->traces.clear();
}

void GameFlowCoordinator::setEventHistoryCapacity(std::size_t capacity) noexcept
{
    _impl->diagnostics.setHistoryCapacity(capacity);
}

std::size_t GameFlowCoordinator::eventHistoryCapacity() const noexcept
{
    return _impl->diagnostics.historyCapacity();
}

const std::deque<GameFlowEvent>&
GameFlowCoordinator::eventHistory() const noexcept
{
    return _impl->diagnostics.events();
}

void GameFlowCoordinator::clearEventHistory() noexcept
{
    _impl->diagnostics.clearEvents();
}

const GameFlowMetrics& GameFlowCoordinator::metrics() const noexcept
{
    return _impl->diagnostics.metrics();
}

void GameFlowCoordinator::resetMetrics() noexcept
{
    _impl->diagnostics.resetMetrics();
    _impl->diagnostics.observeQueueDepth(_impl->queuedCount());
    _impl->diagnostics.observeCallDepth(callDepth());
}

void GameFlowCoordinator::setEventObserver(
    GameFlowEventObserver observer) noexcept
{
    if (_impl->externalCallbackDepth != 0u) return;
    if (!observer) {
        _impl->diagnostics.setObserver({});
        return;
    }
    try {
        Impl* owner = _impl.get();
        _impl->diagnostics.setObserver(
            [owner, callback = std::move(observer)](
                const GameFlowEvent& event) {
                Impl::ExternalCallbackScope scope(*owner,
                    GameFlowDeterminismIntentOrigin::ObserverCallback);
                callback(event);
            });
    } catch (...) {
        _impl->diagnostics.setObserver({});
    }
}

GameFlowDiagnosticsSnapshot GameFlowCoordinator::diagnosticsSnapshot() const
{
    const GameFlowCoordinatorSnapshot coordinator = snapshot();
    GameFlowRuntimeDiagnosticState runtime;
    runtime.ready = coordinator.status != GameFlowCoordinatorStatus::NotReady;
    runtime.busy = coordinator.busy;
    runtime.waitingForAction = coordinator.status
        == GameFlowCoordinatorStatus::WaitingForAction;
    runtime.waitingForSubflow = coordinator.status
        == GameFlowCoordinatorStatus::WaitingForSubflow;
    runtime.currentFlowId = coordinator.currentFlowId;
    runtime.currentStateId = coordinator.currentStateId;
    runtime.activeTransitionId = coordinator.activeTransitionId;
    runtime.activeActionId = coordinator.activeActionId;
    runtime.generation = coordinator.generation;
    runtime.actionExecutionId = coordinator.executionId;
    runtime.queuedIntentCount = coordinator.queuedIntentCount;
    runtime.callDepth = coordinator.callDepth;
    runtime.frames.reserve(coordinator.frames.size());
    for (const auto& frame : coordinator.frames) {
        runtime.frames.push_back({frame.instanceSerial, frame.flowId,
            frame.stateId, frame.suspendedTransitionId});
    }
    return _impl->diagnostics.snapshot(std::move(runtime));
}

bool GameFlowCoordinator::setDeterminismExchange(
    IGameFlowDeterminismExchange* exchange,
    std::string* error)
{
    Impl::PublicCallScope publicCall(*_impl);
    if (_impl->externalCallbackDepth != 0u) {
        if (error != nullptr) {
            *error = "GameFlow determinism exchange cannot be changed from "
                "a callback.";
        }
        return false;
    }
    if (_impl->determinismFaulted) {
        // A faulted stream may be explicitly detached so reset() can recover.
        if (exchange != nullptr) {
            if (error != nullptr) {
                *error = "Detach the faulted GameFlow determinism exchange "
                    "before attaching another one.";
            }
            return false;
        }
    } else if (!_impl->frames.empty() && !reloadSafePoint()) {
        if (error != nullptr) {
            *error = "GameFlow determinism exchange can only change at a "
                "reload safe point.";
        }
        return false;
    }
    if (exchange != nullptr && !exchange->healthy()) {
        if (error != nullptr) {
            *error = exchange->fault().empty()
                ? "GameFlow determinism exchange is faulted."
                : std::string(exchange->fault());
        }
        return false;
    }
    _impl->determinism = exchange;
    _impl->nextDeterminismSequence = 1u;
    _impl->bufferedPlaybackRecord.reset();
    _impl->determinismFaulted = false;
    _impl->determinismFault.clear();
    if (error != nullptr) error->clear();
    return true;
}

IGameFlowDeterminismExchange*
GameFlowCoordinator::determinismExchange() const noexcept
{
    return _impl->determinism;
}

bool GameFlowCoordinator::determinismHealthy() const noexcept
{
    return !_impl->determinismFaulted
        && (_impl->determinism == nullptr || _impl->determinism->healthy());
}

std::string_view GameFlowCoordinator::determinismFault() const noexcept
{
    if (_impl->determinismFaulted) {
        return _impl->determinismFault.empty()
            ? std::string_view("GameFlow determinism exchange failed.")
            : std::string_view(_impl->determinismFault);
    }
    return _impl->determinism == nullptr
        ? std::string_view{} : _impl->determinism->fault();
}

} // namespace ayt::app
