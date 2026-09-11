#include <AYApplication/GameFlowCoordinator.h>

#include <algorithm>
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

bool normalizePayload(const GameFlowIntentDefinition& intent,
                      GameFlowPayload& payload,
                      std::string& error)
{
    std::map<std::string, const GameFlowFieldDefinition*, std::less<>> fields;
    for (const auto& field : intent.payload) fields.emplace(field.id, &field);
    for (const auto& [id, value] : payload) {
        const auto found = fields.find(id);
        if (found == fields.end()) {
            error = "Intent '" + intent.id + "' received unknown field '"
                + id + "'.";
            return false;
        }
        if (!valueMatches(value, found->second->type)) {
            error = "Intent field '" + id + "' has the wrong type.";
            return false;
        }
    }
    for (const auto& field : intent.payload) {
        if (payload.contains(field.id)) continue;
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            payload.emplace(field.id, field.defaultValue);
        } else if (field.required) {
            error = "Intent '" + intent.id + "' requires field '"
                + field.id + "'.";
            return false;
        }
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
        double elapsedSeconds = 0.0;
        GameFlowActionExecutionId pendingAction = 0;
        GameFlowActionCancellationHandler onCancel;
    };

    const GameFlowPlan* plan = nullptr;
    const GameFlowActionRegistry* registry = nullptr;
    std::size_t currentStateIndex = kNoState;
    std::deque<IntentRequest> requests;
    std::optional<ActiveTransition> active;
    GameFlowGeneration nextGeneration = 1;
    GameFlowActionExecutionId nextActionExecutionId = 1;
    std::uint64_t nextTraceSerial = 1;
    std::vector<GameFlowTraceEntry> traces;

    std::size_t resolveInitial(std::size_t stateIndex) const
    {
        while (stateIndex != kNoState) {
            const auto& state = plan->document.states[stateIndex];
            if (state.initialChild.empty()) break;
            const auto child = plan->stateIndices.find(state.initialChild);
            if (child == plan->stateIndices.end()) return kNoState;
            stateIndex = child->second;
        }
        return stateIndex;
    }

    void appendTrace(GameFlowGeneration generation,
                     GameFlowActionExecutionId actionExecutionId,
                     std::string transition,
                     std::string detail)
    {
        std::string state;
        if (plan != nullptr && currentStateIndex != kNoState) {
            state = plan->document.states[currentStateIndex].id;
        }
        traces.push_back({nextTraceSerial++, generation, actionExecutionId,
            std::move(state), std::move(transition), std::move(detail)});
        if (traces.size() > 512u) traces.erase(traces.begin());
    }

    std::optional<std::size_t> selectTransition(const IntentRequest& request)
    {
        std::size_t stateIndex = currentStateIndex;
        const auto& intent = plan->document.intents[request.intentIndex];
        while (stateIndex != kNoState) {
            const auto& state = plan->document.states[stateIndex];
            const auto candidates = plan->transitionsByStateAndIntent.find(
                transitionKey(state.id, intent.id));
            if (candidates != plan->transitionsByStateAndIntent.end()) {
                for (const std::size_t transitionIndex : candidates->second) {
                    const auto& normalized = plan->transitions[transitionIndex];
                    const auto& transition = plan->document.transitions[
                        normalized.documentIndex];
                    if (transition.guard.guard.empty()) return transitionIndex;
                    const auto* handler = registry->findGuardHandler(
                        transition.guard.guard);
                    if (handler == nullptr) {
                        appendTrace(0, 0, transition.id,
                            "registered guard handler is unavailable");
                        continue;
                    }
                    try {
                        const GameFlowGuardInvocation invocation{
                            plan->document.id,
                            transition.id,
                            intent.id,
                            &request.payload,
                            &transition.guard.arguments,
                        };
                        if ((*handler)(invocation)) return transitionIndex;
                    } catch (const std::exception& exception) {
                        appendTrace(0, 0, transition.id,
                            std::string("guard threw: ") + exception.what());
                    } catch (...) {
                        appendTrace(0, 0, transition.id,
                            "guard threw an unknown exception");
                    }
                }
            }
            if (state.parent.empty()) break;
            const auto parent = plan->stateIndices.find(state.parent);
            if (parent == plan->stateIndices.end()) break;
            stateIndex = parent->second;
        }
        return std::nullopt;
    }

    void finish(GameFlowActionState result, std::string message)
    {
        const ActiveTransition snapshot = *active;
        const auto& normalized = plan->transitions[snapshot.transitionIndex];
        const auto& transition = plan->document.transitions[
            normalized.documentIndex];
        std::size_t routedState = currentStateIndex;
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
        active.reset();
        currentStateIndex = resolveInitial(routedState);
        if (!message.empty()) detail += ": " + message;
        appendTrace(snapshot.generation, snapshot.pendingAction,
            transition.id, std::move(detail));
    }

    void acceptActionResult(GameFlowActionResult result)
    {
        if (!active.has_value()) return;
        if (result.state == GameFlowActionState::Succeeded) {
            active->pendingAction = 0;
            active->onCancel = {};
            driveActions();
            return;
        }
        if (result.state == GameFlowActionState::Pending) return;
        active->pendingAction = 0;
        active->onCancel = {};
        finish(result.state, std::move(result.message));
    }

    void driveActions()
    {
        while (active.has_value()) {
            const auto& normalized = plan->transitions[active->transitionIndex];
            const auto& transition = plan->document.transitions[
                normalized.documentIndex];
            if (active->nextAction >= transition.actions.size()) {
                finish(GameFlowActionState::Succeeded, {});
                return;
            }

            const auto& action = transition.actions[active->nextAction++];
            const auto* definition = registry->findAction(action.action);
            const auto* handler = registry->findActionHandler(action.action);
            if (definition == nullptr || handler == nullptr) {
                finish(GameFlowActionState::Failed,
                    "action '" + action.action + "' is unavailable");
                return;
            }

            const GameFlowActionExecutionId executionId =
                nextActionExecutionId++;
            GameFlowActionResult result;
            try {
                const auto& intent = plan->document.intents[
                    active->request.intentIndex];
                const GameFlowActionInvocation invocation{
                    active->generation,
                    executionId,
                    plan->document.id,
                    transition.id,
                    intent.id,
                    &active->request.payload,
                    &action.arguments,
                };
                appendTrace(active->generation, executionId, transition.id,
                    "starting action '" + action.action + "'");
                result = (*handler)(invocation);
            } catch (const std::exception& exception) {
                result = GameFlowActionResult::failed(
                    std::string("action threw: ") + exception.what());
            } catch (...) {
                result = GameFlowActionResult::failed(
                    "action threw an unknown exception");
            }

            if (result.state == GameFlowActionState::Pending) {
                if (!definition->asynchronous) {
                    finish(GameFlowActionState::Failed,
                        "synchronous action '" + action.action
                            + "' returned Pending");
                    return;
                }
                active->pendingAction = executionId;
                active->onCancel = std::move(result.onCancel);
                appendTrace(active->generation, executionId, transition.id,
                    "action pending");
                return;
            }
            if (result.state == GameFlowActionState::Succeeded) continue;
            finish(result.state, std::move(result.message));
            return;
        }
    }

    void begin(std::size_t transitionIndex, IntentRequest request)
    {
        const auto& normalized = plan->transitions[transitionIndex];
        const auto& transition = plan->document.transitions[
            normalized.documentIndex];
        ActiveTransition value;
        value.generation = nextGeneration++;
        value.transitionIndex = transitionIndex;
        value.request = std::move(request);
        active = std::move(value);
        appendTrace(active->generation, 0, transition.id,
            "transition started");
        driveActions();
    }

    void invokeCancellation() noexcept
    {
        if (!active.has_value() || !active->onCancel) return;
        auto callback = std::move(active->onCancel);
        try {
            callback();
        } catch (...) {
            // Cancellation is best-effort and never changes the selected route.
        }
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
    reset();
    if (plan == nullptr || registry == nullptr) {
        if (error != nullptr) *error = "GameFlow plan and registry are required.";
        return false;
    }
    const auto initial = plan->stateIndices.find(plan->document.initialState);
    if (initial == plan->stateIndices.end()) {
        if (error != nullptr) *error = "GameFlow plan has no valid initial state.";
        return false;
    }
    _impl->plan = plan;
    _impl->registry = registry;
    _impl->currentStateIndex = _impl->resolveInitial(initial->second);
    _impl->appendTrace(0, 0, {}, "flow initialized");
    if (error != nullptr) error->clear();
    return true;
}

void GameFlowCoordinator::reset() noexcept
{
    _impl->invokeCancellation();
    _impl->active.reset();
    _impl->requests.clear();
    _impl->plan = nullptr;
    _impl->registry = nullptr;
    _impl->currentStateIndex = kNoState;
}

GameFlowRequestResult GameFlowCoordinator::request(
    std::string_view intent,
    GameFlowPayload payload)
{
    if (_impl->plan == nullptr || _impl->registry == nullptr) {
        return {GameFlowRequestState::NotReady,
            "GameFlow coordinator has no plan."};
    }
    const auto found = _impl->plan->intentIndices.find(intent);
    if (found == _impl->plan->intentIndices.end()) {
        return {GameFlowRequestState::UnknownIntent,
            "Intent '" + std::string(intent) + "' is not declared."};
    }
    std::string error;
    if (!normalizePayload(
            _impl->plan->document.intents[found->second], payload, error)) {
        return {GameFlowRequestState::InvalidPayload, std::move(error)};
    }
    _impl->requests.push_back({found->second, std::move(payload)});
    return {GameFlowRequestState::Queued, {}};
}

void GameFlowCoordinator::update(double deltaSeconds)
{
    if (_impl->plan == nullptr) return;
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0) deltaSeconds = 0.0;

    if (_impl->active.has_value()) {
        const auto& normalized =
            _impl->plan->transitions[_impl->active->transitionIndex];
        const auto& transition = _impl->plan->document.transitions[
            normalized.documentIndex];
        _impl->active->elapsedSeconds += deltaSeconds;
        if (transition.timeoutSeconds > 0.0
            && _impl->active->elapsedSeconds >= transition.timeoutSeconds) {
            _impl->invokeCancellation();
            _impl->finish(GameFlowActionState::Failed, "transition timed out");
        }
    }

    std::size_t safetyCounter = 0;
    while (!_impl->active.has_value() && !_impl->requests.empty()
           && safetyCounter++ < 1024u) {
        auto request = std::move(_impl->requests.front());
        _impl->requests.pop_front();
        const auto selected = _impl->selectTransition(request);
        if (!selected.has_value()) {
            const auto& intent = _impl->plan->document.intents[
                request.intentIndex];
            _impl->appendTrace(0, 0, {},
                "no transition accepted intent '" + intent.id + "'");
            continue;
        }
        _impl->begin(*selected, std::move(request));
    }
}

bool GameFlowCoordinator::completeAction(
    GameFlowActionExecutionId executionId,
    GameFlowActionResult result,
    std::string* error)
{
    if (!_impl->active.has_value()
        || _impl->active->pendingAction == 0
        || _impl->active->pendingAction != executionId) {
        if (error != nullptr) {
            *error = "Action completion is stale or does not match pending work.";
        }
        return false;
    }
    if (result.state == GameFlowActionState::Pending) {
        if (error != nullptr) *error = "A completion result cannot remain Pending.";
        return false;
    }
    if (error != nullptr) error->clear();
    _impl->acceptActionResult(std::move(result));
    return true;
}

bool GameFlowCoordinator::cancelActive(std::string message)
{
    if (!_impl->active.has_value()) return false;
    _impl->invokeCancellation();
    _impl->finish(GameFlowActionState::Cancelled, std::move(message));
    return true;
}

std::string_view GameFlowCoordinator::currentState() const noexcept
{
    if (_impl->plan == nullptr || _impl->currentStateIndex == kNoState) return {};
    return _impl->plan->document.states[_impl->currentStateIndex].id;
}

std::string_view GameFlowCoordinator::activeTransition() const noexcept
{
    if (_impl->plan == nullptr || !_impl->active.has_value()) return {};
    const auto& normalized =
        _impl->plan->transitions[_impl->active->transitionIndex];
    return _impl->plan->document.transitions[normalized.documentIndex].id;
}

GameFlowGeneration GameFlowCoordinator::activeGeneration() const noexcept
{
    return _impl->active.has_value() ? _impl->active->generation : 0;
}

GameFlowActionExecutionId GameFlowCoordinator::pendingAction() const noexcept
{
    return _impl->active.has_value() ? _impl->active->pendingAction : 0;
}

std::size_t GameFlowCoordinator::queuedIntentCount() const noexcept
{
    return _impl->requests.size();
}

bool GameFlowCoordinator::busy() const noexcept
{
    return _impl->active.has_value();
}

const std::vector<GameFlowTraceEntry>& GameFlowCoordinator::trace() const noexcept
{
    return _impl->traces;
}

void GameFlowCoordinator::clearTrace() noexcept
{
    _impl->traces.clear();
}

} // namespace ayt::app
