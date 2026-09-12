#include <AYApplication/GameFlowCoordinator.h>
#include <AYApplication/GameFlowReplayExchange.h>
#include <AYReplay/FileReplayPlayer.h>
#include <AYReplay/FileReplayRecorder.h>
#include <AYTest.h>

#include <filesystem>
#include <string>

namespace
{

using namespace ayt::app;

GameFlowPlan makePlan(GameFlowActionRegistry& registry,
                      int& guardCalls,
                      int& actionCalls)
{
    CHECK(registry.registerGuard({"test.allow", {}},
        [&guardCalls](const GameFlowGuardInvocation&) {
            ++guardCalls;
            return true;
        }));
    CHECK(registry.registerAction({"test.run", {}, false},
        [&actionCalls](const GameFlowActionInvocation&) {
            ++actionCalls;
            return GameFlowActionResult::succeeded();
        }));
    GameFlowDocument document;
    document.id = "file-flow";
    document.initialState = "idle";
    document.intents = {{"go", {
        {"name", GameFlowValueType::String, true, {}},
    }}};
    document.states = {{"idle"}, {"done"}, {"failed"}};
    GameFlowTransitionDefinition transition;
    transition.id = "choose";
    transition.fromState = "idle";
    transition.triggerIntent = "go";
    transition.toState = "done";
    transition.guard = {"test.allow", {}};
    transition.actions = {{"test.run", {}}};
    transition.onFailureState = "failed";
    document.transitions.push_back(std::move(transition));
    GameFlowPlan plan;
    CHECK(buildGameFlowPlan(document, registry, plan));
    return plan;
}

std::filesystem::path outputRoot()
{
    return std::filesystem::path(
        AY_APPLICATION_GAMEFLOW_REPLAY_OUTPUT_ROOT);
}

} // namespace

TEST_SUITE(GameFlowReplayTests)

TEST_CASE(canonical_json_preserves_tagged_payload_types)
{
    GameFlowDeterminismRecord source;
    source.sequence = 9u;
    source.kind = GameFlowDeterminismKind::Intent;
    source.flowId = "root";
    source.stateId = "idle";
    source.intentId = "go";
    source.intentOrigin = GameFlowDeterminismIntentOrigin::ActionCallback;
    source.programFingerprint = 0x123456789abcdef0ull;
    source.payload = {
        {"array", GameFlowValue::Array{std::int64_t(1), 1.0, "x"}},
        {"object", GameFlowValue::Object{{"enabled", true}}},
    };
    std::string encoded;
    CHECK(serializeGameFlowDeterminismRecord(source, encoded));
    CHECK_FALSE(encoded.empty());
    CHECK(encoded.front() == '{');
    CHECK(encoded.find("\n") == std::string::npos);

    GameFlowDeterminismRecord decoded;
    CHECK(deserializeGameFlowDeterminismRecord(encoded, decoded));
    CHECK(decoded == source);
    const auto preserved = decoded;
    std::string error;
    CHECK_FALSE(deserializeGameFlowDeterminismRecord(
        " " + encoded, decoded, &error));
    CHECK(decoded == preserved);
    CHECK(error.find("canonical") != std::string::npos);
}

TEST_CASE(file_capture_to_playback_is_deterministic_and_has_no_live_side_effects)
{
    const auto directory = outputRoot();
    std::filesystem::create_directories(directory);
    const auto base = directory / "gameflow-roundtrip.ayrp";
    const auto first = ayt::replay::FileReplayRecorder::rotationPathFor(
        base.string(), 0u);
    std::filesystem::remove(first);

    int guardCalls = 0;
    int actionCalls = 0;
    GameFlowActionRegistry registry;
    const GameFlowPlan plan = makePlan(registry, guardCalls, actionCalls);

    ayt::replay::ReplayFileHeader fileHeader{};
    fileHeader.engineVersion = 1u;
    fileHeader.schemaVersion = kGameFlowDeterminismSchemaVersion;
    fileHeader.tickRateMilliHz = 1000u;
    ayt::replay::FileReplayRecorder recorder(base.string());
    CHECK(recorder.beginSession(fileHeader));
    GameFlowReplayCaptureExchange capture(recorder);
    GameFlowCoordinator live;
    CHECK(live.setDeterminismExchange(&capture));
    CHECK(live.setPlan(&plan, &registry));
    CHECK(live.request("go", {{"name", "captured"}}));
    live.update(0.5);
    CHECK(live.currentState() == "done");
    CHECK(live.determinismHealthy());
    CHECK(recorder.endSession());
    CHECK(guardCalls == 1);
    CHECK(actionCalls == 1);

    ayt::replay::FileReplayPlayer player(first);
    CHECK(player.open() == ayt::replay::IReplayPlayer::Error::Ok);
    GameFlowReplayPlaybackExchange playback(player);
    guardCalls = 0;
    actionCalls = 0;
    GameFlowActionRegistry playbackRegistry;
    CHECK(playbackRegistry.registerGuardType({"test.allow", {}}));
    CHECK(playbackRegistry.registerActionType({"test.run", {}, false}));
    GameFlowCoordinator replayed;
    CHECK(replayed.setDeterminismExchange(&playback));
    CHECK(replayed.setPlan(&plan, &playbackRegistry));
    CHECK(replayed.request("go", {{"name", "captured"}}));
    replayed.update(0.5);
    CHECK(replayed.currentState() == "done");
    CHECK(replayed.determinismHealthy());
    CHECK(guardCalls == 0);
    CHECK(actionCalls == 0);
    player.close();
}

TEST_CASE(gameflow_event_ids_stay_in_the_documented_adapter_range)
{
    for (std::uint16_t value = 1u; value <= 11u; ++value) {
        const auto type = gameFlowReplayEventType(
            static_cast<GameFlowDeterminismKind>(value));
        CHECK(type >= 0x30000u);
        CHECK(type <= 0x3FFFFu);
    }
}

TEST_CASE(playback_adapter_retains_its_first_protocol_fault)
{
    const auto directory = outputRoot();
    std::filesystem::create_directories(directory);
    const auto base = directory / "gameflow-invalid.ayrp";
    const auto first = ayt::replay::FileReplayRecorder::rotationPathFor(
        base.string(), 0u);
    std::filesystem::remove(first);

    ayt::replay::ReplayFileHeader header{};
    ayt::replay::FileReplayRecorder recorder(base.string());
    CHECK(recorder.beginSession(header));
    constexpr std::string_view invalid = "{}";
    CHECK(recorder.recordEvent(1u, kEvtGameFlowIntent,
        reinterpret_cast<const std::uint8_t*>(invalid.data()),
        invalid.size()));
    CHECK(recorder.recordEvent(2u, kEvtGameFlowUpdate,
        reinterpret_cast<const std::uint8_t*>(invalid.data()),
        invalid.size()));
    CHECK(recorder.endSession());

    ayt::replay::FileReplayPlayer player(first);
    CHECK(player.open() == ayt::replay::IReplayPlayer::Error::Ok);
    GameFlowReplayPlaybackExchange playback(player);
    GameFlowDeterminismRecord record;
    std::string error;
    CHECK_FALSE(playback.exchange(record, error));
    CHECK_FALSE(playback.healthy());
    const std::string firstFault(playback.fault());
    CHECK_FALSE(firstFault.empty());
    CHECK(player.currentTick() == 1u);
    error.clear();
    CHECK_FALSE(playback.exchange(record, error));
    CHECK(error == firstFault);
    CHECK(playback.fault() == firstFault);
    CHECK(player.currentTick() == 1u);
}

TEST_SUITE_END
