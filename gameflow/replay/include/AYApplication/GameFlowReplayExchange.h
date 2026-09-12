#pragma once

#include <AYApplication/GameFlowDeterminism.h>
#include <AYReplay/IReplayPlayer.h>
#include <AYReplay/IReplayRecorder.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ayt::app
{

inline constexpr ayt::replay::ReplayEventType kEvtGameFlowIntent = 0x30001u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowUpdate = 0x30002u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowGuardOutcome =
    0x30003u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowTransitionDecision =
    0x30004u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowActionOutcome =
    0x30005u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowAsyncCompletion =
    0x30006u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowCancellation =
    0x30007u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowSubflowEnter =
    0x30008u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowSubflowReturn =
    0x30009u;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowTerminal = 0x3000Au;
inline constexpr ayt::replay::ReplayEventType kEvtGameFlowProgramManifest =
    0x3000Bu;

[[nodiscard]] ayt::replay::ReplayEventType gameFlowReplayEventType(
    GameFlowDeterminismKind kind) noexcept;

// Canonical wire JSON has sorted object keys, fixed fields, tagged values,
// and no insignificant whitespace. Decode rejects non-canonical encodings.
[[nodiscard]] bool serializeGameFlowDeterminismRecord(
    const GameFlowDeterminismRecord& record,
    std::string& jsonText,
    std::string* error = nullptr);
[[nodiscard]] bool deserializeGameFlowDeterminismRecord(
    std::string_view jsonText,
    GameFlowDeterminismRecord& record,
    std::string* error = nullptr);

class GameFlowReplayCaptureExchange final
    : public IGameFlowDeterminismExchange
{
public:
    explicit GameFlowReplayCaptureExchange(
        ayt::replay::IReplayRecorder& recorder) noexcept;

    [[nodiscard]] GameFlowDeterminismMode mode() const noexcept override;
    bool exchange(GameFlowDeterminismRecord& record,
                  std::string& error) override;
    [[nodiscard]] bool healthy() const noexcept override;
    [[nodiscard]] std::string_view fault() const noexcept override;

private:
    void retainFault(std::string error) noexcept;
    ayt::replay::IReplayRecorder* _recorder = nullptr;
    bool _faulted = false;
    std::string _fault;
};

class GameFlowReplayPlaybackExchange final
    : public IGameFlowDeterminismExchange
{
public:
    explicit GameFlowReplayPlaybackExchange(
        ayt::replay::IReplayPlayer& player) noexcept;

    [[nodiscard]] GameFlowDeterminismMode mode() const noexcept override;
    bool exchange(GameFlowDeterminismRecord& record,
                  std::string& error) override;
    [[nodiscard]] bool healthy() const noexcept override;
    [[nodiscard]] std::string_view fault() const noexcept override;

private:
    void retainFault(std::string error) noexcept;
    ayt::replay::IReplayPlayer* _player = nullptr;
    bool _faulted = false;
    std::string _fault;
};

} // namespace ayt::app
