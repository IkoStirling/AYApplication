#pragma once

#include <AYApplication/GameFlowActionRegistry.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ayt::app
{

inline constexpr std::uint32_t kGameFlowDeterminismSchemaVersion = 1u;

enum class GameFlowDeterminismMode : std::uint8_t
{
    Capture,
    Playback,
};

// Values are stable because the optional AYReplay adapter uses them to select
// event ids in the 0x30000 adapter range.
enum class GameFlowDeterminismKind : std::uint16_t
{
    Intent = 1,
    Update = 2,
    GuardOutcome = 3,
    TransitionDecision = 4,
    ActionOutcome = 5,
    AsyncCompletion = 6,
    Cancellation = 7,
    SubflowEnter = 8,
    SubflowReturn = 9,
    Terminal = 10,
    ProgramManifest = 11,
};

enum class GameFlowDeterminismOutcome : std::uint8_t
{
    None,
    Accepted,
    Rejected,
    Unmatched,
    Succeeded,
    Failed,
    Pending,
    Cancelled,
};

enum class GameFlowDeterminismCancellation : std::uint8_t
{
    None,
    ActiveTransition,
    SubflowCall,
};

enum class GameFlowDeterminismIntentOrigin : std::uint8_t
{
    External,
    GuardCallback,
    ActionCallback,
    CancellationCallback,
    ObserverCallback,
    DeterminismExchange,
};

// This is an owning, engine-level exchange record. It deliberately contains
// no JSON or AYReplay types, keeping AYApplicationGameFlow usable by headless
// products that do not link the replay foundation.
struct GameFlowDeterminismRecord
{
    std::uint32_t schemaVersion = kGameFlowDeterminismSchemaVersion;
    std::uint64_t sequence = 0;
    GameFlowDeterminismKind kind = GameFlowDeterminismKind::Intent;
    GameFlowDeterminismOutcome outcome = GameFlowDeterminismOutcome::None;
    GameFlowDeterminismCancellation cancellation =
        GameFlowDeterminismCancellation::None;
    GameFlowDeterminismIntentOrigin intentOrigin =
        GameFlowDeterminismIntentOrigin::External;
    GameFlowGeneration generation = 0;
    GameFlowActionExecutionId actionExecutionId = 0;
    std::uint64_t programFingerprint = 0;
    std::size_t callDepth = 0;
    double deltaSeconds = 0.0;
    std::string flowId;
    std::string stateId;
    std::string resultStateId;
    std::string intentId;
    std::string transitionId;
    std::string guardId;
    std::string actionId;
    std::string targetFlowId;
    GameFlowPayload payload;

    friend bool operator==(
        const GameFlowDeterminismRecord&,
        const GameFlowDeterminismRecord&) = default;
};

struct GameFlowPlan;
struct GameFlowProgram;

// Stable FNV-1a fingerprints cover normalized behavior data. Editor-only
// extensions and diagnostics are intentionally excluded.
[[nodiscard]] std::uint64_t gameFlowPlanFingerprint(
    const GameFlowPlan& plan) noexcept;
[[nodiscard]] std::uint64_t gameFlowProgramFingerprint(
    const GameFlowProgram& program) noexcept;

// Capture implementations persist the supplied record. Playback
// implementations replace it with the next recorded value. Implementations
// must be main-thread confined and retain their first fault.
class IGameFlowDeterminismExchange
{
public:
    virtual ~IGameFlowDeterminismExchange() = default;
    [[nodiscard]] virtual GameFlowDeterminismMode mode() const noexcept = 0;
    virtual bool exchange(
        GameFlowDeterminismRecord& record, std::string& error) = 0;
    [[nodiscard]] virtual bool healthy() const noexcept = 0;
    [[nodiscard]] virtual std::string_view fault() const noexcept = 0;
};

[[nodiscard]] const char* gameFlowDeterminismKindName(
    GameFlowDeterminismKind value) noexcept;
[[nodiscard]] const char* gameFlowDeterminismOutcomeName(
    GameFlowDeterminismOutcome value) noexcept;
[[nodiscard]] const char* gameFlowDeterminismCancellationName(
    GameFlowDeterminismCancellation value) noexcept;
[[nodiscard]] const char* gameFlowDeterminismIntentOriginName(
    GameFlowDeterminismIntentOrigin value) noexcept;

} // namespace ayt::app
