#include <AYApplication/GameFlowDeterminism.h>
#include <AYApplication/GameFlowCoordinator.h>
#include <AYApplication/GameFlowProgram.h>

#include <bit>
#include <type_traits>

namespace ayt::app
{
namespace
{

class StableHash
{
public:
    void byte(std::uint8_t value) noexcept
    {
        hash ^= value;
        hash *= 1099511628211ull;
    }

    template <class Value>
    void integer(Value value) noexcept
    {
        using Unsigned = std::make_unsigned_t<Value>;
        Unsigned encoded = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
            byte(static_cast<std::uint8_t>(encoded & 0xffu));
            encoded >>= 8u;
        }
    }

    void text(std::string_view value) noexcept
    {
        integer<std::uint64_t>(value.size());
        for (const unsigned char character : value) byte(character);
    }

    std::uint64_t hash = 14695981039346656037ull;
};

void hashValue(StableHash& hash, const GameFlowValue& value) noexcept
{
    hash.integer<std::uint8_t>(static_cast<std::uint8_t>(value.data.index()));
    std::visit([&hash](const auto& stored) noexcept {
        using T = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return;
        } else if constexpr (std::is_same_v<T, bool>) {
            hash.integer<std::uint8_t>(stored ? 1u : 0u);
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            hash.integer(stored);
        } else if constexpr (std::is_same_v<T, double>) {
            hash.integer(std::bit_cast<std::uint64_t>(stored));
        } else if constexpr (std::is_same_v<T, std::string>) {
            hash.text(stored);
        } else if constexpr (std::is_same_v<T, GameFlowValue::Array>) {
            hash.integer<std::uint64_t>(stored.size());
            for (const auto& item : stored) hashValue(hash, item);
        } else {
            hash.integer<std::uint64_t>(stored.size());
            for (const auto& [key, item] : stored) {
                hash.text(key);
                hashValue(hash, item);
            }
        }
    }, value.data);
}

void hashPayload(StableHash& hash, const GameFlowPayload& payload) noexcept
{
    hash.integer<std::uint64_t>(payload.size());
    for (const auto& [key, value] : payload) {
        hash.text(key);
        hashValue(hash, value);
    }
}

void hashFields(StableHash& hash,
                const std::vector<GameFlowFieldDefinition>& fields) noexcept
{
    hash.integer<std::uint64_t>(fields.size());
    for (const auto& field : fields) {
        hash.text(field.id);
        hash.integer<std::uint8_t>(static_cast<std::uint8_t>(field.type));
        hash.integer<std::uint8_t>(field.required ? 1u : 0u);
        hashValue(hash, field.defaultValue);
    }
}

void hashPlan(StableHash& hash, const GameFlowPlan& plan) noexcept
{
    const auto& document = plan.document;
    hash.integer(document.schemaVersion);
    hash.text(document.id);
    hash.text(document.initialState);
    hashFields(hash, document.entryParameters);
    hashFields(hash, document.result);

    hash.integer<std::uint64_t>(document.intents.size());
    for (const auto& intent : document.intents) {
        hash.text(intent.id);
        hashFields(hash, intent.payload);
    }
    hash.integer<std::uint64_t>(document.states.size());
    for (const auto& state : document.states) {
        hash.text(state.id);
        hash.text(state.parent);
        hash.text(state.initialChild);
    }
    hash.integer<std::uint64_t>(document.transitions.size());
    for (const auto& transition : document.transitions) {
        hash.text(transition.id);
        hash.text(transition.fromState);
        hash.text(transition.triggerIntent);
        hash.text(transition.toState);
        hash.text(transition.guard.guard);
        hashPayload(hash, transition.guard.arguments);
        hash.integer<std::uint64_t>(transition.actions.size());
        for (const auto& action : transition.actions) {
            hash.text(action.action);
            hashPayload(hash, action.arguments);
        }
        hash.text(transition.onFailureState);
        hash.text(transition.onCancelState);
        hash.integer(std::bit_cast<std::uint64_t>(transition.timeoutSeconds));
        hash.integer(transition.priority);
    }

    // These indices are the data actually consumed by the coordinator and
    // catch a malformed or independently produced normalization result.
    hash.integer<std::uint64_t>(plan.transitions.size());
    for (const auto& transition : plan.transitions) {
        hash.integer<std::uint64_t>(transition.documentIndex);
        hash.integer<std::uint64_t>(transition.fromState);
        hash.integer<std::uint64_t>(transition.toState);
        hash.integer<std::uint64_t>(transition.intent);
        hash.integer<std::uint64_t>(transition.onFailureState);
        hash.integer<std::uint64_t>(transition.onCancelState);
    }
    hash.integer<std::uint64_t>(plan.stateIndices.size());
    for (const auto& [id, index] : plan.stateIndices) {
        hash.text(id);
        hash.integer<std::uint64_t>(index);
    }
    hash.integer<std::uint64_t>(plan.intentIndices.size());
    for (const auto& [id, index] : plan.intentIndices) {
        hash.text(id);
        hash.integer<std::uint64_t>(index);
    }
    hash.integer<std::uint64_t>(plan.transitionsByStateAndIntent.size());
    for (const auto& [key, transitions] :
            plan.transitionsByStateAndIntent) {
        hash.text(key);
        hash.integer<std::uint64_t>(transitions.size());
        for (const std::size_t index : transitions) {
            hash.integer<std::uint64_t>(index);
        }
    }
}

} // namespace

std::uint64_t gameFlowPlanFingerprint(const GameFlowPlan& plan) noexcept
{
    StableHash hash;
    hash.text("AY.GameFlow.Plan.v1");
    hashPlan(hash, plan);
    return hash.hash;
}

std::uint64_t gameFlowProgramFingerprint(
    const GameFlowProgram& program) noexcept
{
    StableHash hash;
    hash.text("AY.GameFlow.Program.v1");
    hash.text(program.rootFlowId);
    hash.integer<std::uint64_t>(program.maxCallDepth);
    hash.integer<std::uint64_t>(program.plans.size());
    for (const auto& [id, plan] : program.plans) {
        hash.text(id);
        hashPlan(hash, plan);
    }
    return hash.hash;
}

const char* gameFlowDeterminismKindName(
    GameFlowDeterminismKind value) noexcept
{
    switch (value) {
    case GameFlowDeterminismKind::Intent: return "intent";
    case GameFlowDeterminismKind::Update: return "update";
    case GameFlowDeterminismKind::GuardOutcome: return "guardOutcome";
    case GameFlowDeterminismKind::TransitionDecision:
        return "transitionDecision";
    case GameFlowDeterminismKind::ActionOutcome: return "actionOutcome";
    case GameFlowDeterminismKind::AsyncCompletion: return "asyncCompletion";
    case GameFlowDeterminismKind::Cancellation: return "cancellation";
    case GameFlowDeterminismKind::SubflowEnter: return "subflowEnter";
    case GameFlowDeterminismKind::SubflowReturn: return "subflowReturn";
    case GameFlowDeterminismKind::Terminal: return "terminal";
    case GameFlowDeterminismKind::ProgramManifest: return "programManifest";
    }
    return "unknown";
}

const char* gameFlowDeterminismOutcomeName(
    GameFlowDeterminismOutcome value) noexcept
{
    switch (value) {
    case GameFlowDeterminismOutcome::None: return "none";
    case GameFlowDeterminismOutcome::Accepted: return "accepted";
    case GameFlowDeterminismOutcome::Rejected: return "rejected";
    case GameFlowDeterminismOutcome::Unmatched: return "unmatched";
    case GameFlowDeterminismOutcome::Succeeded: return "succeeded";
    case GameFlowDeterminismOutcome::Failed: return "failed";
    case GameFlowDeterminismOutcome::Pending: return "pending";
    case GameFlowDeterminismOutcome::Cancelled: return "cancelled";
    }
    return "unknown";
}

const char* gameFlowDeterminismCancellationName(
    GameFlowDeterminismCancellation value) noexcept
{
    switch (value) {
    case GameFlowDeterminismCancellation::None: return "none";
    case GameFlowDeterminismCancellation::ActiveTransition: return "active";
    case GameFlowDeterminismCancellation::SubflowCall: return "subflow";
    }
    return "unknown";
}

const char* gameFlowDeterminismIntentOriginName(
    GameFlowDeterminismIntentOrigin value) noexcept
{
    switch (value) {
    case GameFlowDeterminismIntentOrigin::External: return "external";
    case GameFlowDeterminismIntentOrigin::GuardCallback: return "guard";
    case GameFlowDeterminismIntentOrigin::ActionCallback: return "action";
    case GameFlowDeterminismIntentOrigin::CancellationCallback:
        return "cancellation";
    case GameFlowDeterminismIntentOrigin::ObserverCallback: return "observer";
    case GameFlowDeterminismIntentOrigin::DeterminismExchange:
        return "exchange";
    }
    return "unknown";
}

} // namespace ayt::app
