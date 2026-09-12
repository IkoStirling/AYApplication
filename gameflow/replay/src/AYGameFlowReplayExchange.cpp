#include <AYApplication/GameFlowReplayExchange.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ayt::app
{
namespace
{

using json = nlohmann::json;
constexpr std::size_t kMaximumWireBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaximumValueDepth = 64u;
constexpr std::size_t kMaximumValueNodes = 100000u;

json encodeValue(const GameFlowValue& value)
{
    return std::visit([](const auto& stored) -> json {
        using T = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return {{"type", "null"}, {"value", nullptr}};
        } else if constexpr (std::is_same_v<T, bool>) {
            return {{"type", "boolean"}, {"value", stored}};
        } else if constexpr (std::is_same_v<T, std::int64_t>) {
            return {{"type", "integer"}, {"value", stored}};
        } else if constexpr (std::is_same_v<T, double>) {
            if (!std::isfinite(stored)) {
                throw std::runtime_error(
                    "GameFlow replay numbers must be finite.");
            }
            return {{"type", "number"}, {"value", stored}};
        } else if constexpr (std::is_same_v<T, std::string>) {
            return {{"type", "string"}, {"value", stored}};
        } else if constexpr (std::is_same_v<T, GameFlowValue::Array>) {
            json values = json::array();
            for (const auto& item : stored) values.push_back(encodeValue(item));
            return {{"type", "array"}, {"value", std::move(values)}};
        } else {
            json values = json::object();
            for (const auto& [key, item] : stored) {
                values[key] = encodeValue(item);
            }
            return {{"type", "object"}, {"value", std::move(values)}};
        }
    }, value.data);
}

GameFlowValue decodeValue(const json& encoded,
                          std::size_t depth,
                          std::size_t& nodes)
{
    if (depth > kMaximumValueDepth || ++nodes > kMaximumValueNodes) {
        throw std::runtime_error("GameFlow replay value exceeds its limit.");
    }
    if (!encoded.is_object() || encoded.size() != 2u
        || !encoded.contains("type") || !encoded.contains("value")) {
        throw std::runtime_error("Invalid tagged GameFlow replay value.");
    }
    const std::string type = encoded.at("type").get<std::string>();
    const json& value = encoded.at("value");
    if (type == "null") {
        if (!value.is_null()) throw std::runtime_error("Invalid null value.");
        return {};
    }
    if (type == "boolean") return GameFlowValue(value.get<bool>());
    if (type == "integer") return GameFlowValue(value.get<std::int64_t>());
    if (type == "number") {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            throw std::runtime_error("Replay number is not finite.");
        }
        return GameFlowValue(number);
    }
    if (type == "string") return GameFlowValue(value.get<std::string>());
    if (type == "array") {
        if (!value.is_array()) throw std::runtime_error("Invalid array value.");
        GameFlowValue::Array result;
        result.reserve(value.size());
        for (const auto& item : value) {
            result.push_back(decodeValue(item, depth + 1u, nodes));
        }
        return GameFlowValue(std::move(result));
    }
    if (type == "object") {
        if (!value.is_object()) {
            throw std::runtime_error("Invalid object value.");
        }
        GameFlowValue::Object result;
        for (auto item = value.begin(); item != value.end(); ++item) {
            result.emplace(item.key(),
                decodeValue(item.value(), depth + 1u, nodes));
        }
        return GameFlowValue(std::move(result));
    }
    throw std::runtime_error("Unknown GameFlow replay value type.");
}

json encodePayload(const GameFlowPayload& payload)
{
    json result = json::object();
    for (const auto& [key, value] : payload) result[key] = encodeValue(value);
    return result;
}

GameFlowPayload decodePayload(const json& encoded)
{
    if (!encoded.is_object()) {
        throw std::runtime_error("GameFlow replay payload must be an object.");
    }
    GameFlowPayload result;
    std::size_t nodes = 0u;
    for (auto item = encoded.begin(); item != encoded.end(); ++item) {
        result.emplace(item.key(), decodeValue(item.value(), 1u, nodes));
    }
    return result;
}

GameFlowDeterminismKind decodeKind(std::string_view value)
{
    if (value == "intent") return GameFlowDeterminismKind::Intent;
    if (value == "update") return GameFlowDeterminismKind::Update;
    if (value == "guardOutcome") {
        return GameFlowDeterminismKind::GuardOutcome;
    }
    if (value == "transitionDecision") {
        return GameFlowDeterminismKind::TransitionDecision;
    }
    if (value == "actionOutcome") {
        return GameFlowDeterminismKind::ActionOutcome;
    }
    if (value == "asyncCompletion") {
        return GameFlowDeterminismKind::AsyncCompletion;
    }
    if (value == "cancellation") {
        return GameFlowDeterminismKind::Cancellation;
    }
    if (value == "subflowEnter") {
        return GameFlowDeterminismKind::SubflowEnter;
    }
    if (value == "subflowReturn") {
        return GameFlowDeterminismKind::SubflowReturn;
    }
    if (value == "terminal") return GameFlowDeterminismKind::Terminal;
    if (value == "programManifest") {
        return GameFlowDeterminismKind::ProgramManifest;
    }
    throw std::runtime_error("Unknown GameFlow replay record kind.");
}

GameFlowDeterminismIntentOrigin decodeIntentOrigin(std::string_view value)
{
    if (value == "external") {
        return GameFlowDeterminismIntentOrigin::External;
    }
    if (value == "guard") {
        return GameFlowDeterminismIntentOrigin::GuardCallback;
    }
    if (value == "action") {
        return GameFlowDeterminismIntentOrigin::ActionCallback;
    }
    if (value == "cancellation") {
        return GameFlowDeterminismIntentOrigin::CancellationCallback;
    }
    if (value == "observer") {
        return GameFlowDeterminismIntentOrigin::ObserverCallback;
    }
    if (value == "exchange") {
        return GameFlowDeterminismIntentOrigin::DeterminismExchange;
    }
    throw std::runtime_error("Unknown GameFlow replay intent origin.");
}

GameFlowDeterminismOutcome decodeOutcome(std::string_view value)
{
    if (value == "none") return GameFlowDeterminismOutcome::None;
    if (value == "accepted") return GameFlowDeterminismOutcome::Accepted;
    if (value == "rejected") return GameFlowDeterminismOutcome::Rejected;
    if (value == "unmatched") return GameFlowDeterminismOutcome::Unmatched;
    if (value == "succeeded") return GameFlowDeterminismOutcome::Succeeded;
    if (value == "failed") return GameFlowDeterminismOutcome::Failed;
    if (value == "pending") return GameFlowDeterminismOutcome::Pending;
    if (value == "cancelled") return GameFlowDeterminismOutcome::Cancelled;
    throw std::runtime_error("Unknown GameFlow replay outcome.");
}

GameFlowDeterminismCancellation decodeCancellation(std::string_view value)
{
    if (value == "none") return GameFlowDeterminismCancellation::None;
    if (value == "active") {
        return GameFlowDeterminismCancellation::ActiveTransition;
    }
    if (value == "subflow") {
        return GameFlowDeterminismCancellation::SubflowCall;
    }
    throw std::runtime_error("Unknown GameFlow replay cancellation scope.");
}

json encodeRecord(const GameFlowDeterminismRecord& value)
{
    if (!std::isfinite(value.deltaSeconds)) {
        throw std::runtime_error("GameFlow replay delta must be finite.");
    }
    return {
        {"actionExecutionId", value.actionExecutionId},
        {"actionId", value.actionId},
        {"callDepth", value.callDepth},
        {"cancellation", gameFlowDeterminismCancellationName(
            value.cancellation)},
        {"deltaSeconds", value.deltaSeconds},
        {"flowId", value.flowId},
        {"generation", value.generation},
        {"guardId", value.guardId},
        {"intentId", value.intentId},
        {"intentOrigin", gameFlowDeterminismIntentOriginName(
            value.intentOrigin)},
        {"kind", gameFlowDeterminismKindName(value.kind)},
        {"outcome", gameFlowDeterminismOutcomeName(value.outcome)},
        {"payload", encodePayload(value.payload)},
        {"programFingerprint", value.programFingerprint},
        {"resultStateId", value.resultStateId},
        {"schemaVersion", value.schemaVersion},
        {"sequence", value.sequence},
        {"stateId", value.stateId},
        {"targetFlowId", value.targetFlowId},
        {"transitionId", value.transitionId},
    };
}

GameFlowDeterminismRecord decodeRecord(const json& value)
{
    if (!value.is_object() || value.size() != 20u) {
        throw std::runtime_error(
            "GameFlow replay record has an invalid field set.");
    }
    GameFlowDeterminismRecord result;
    result.actionExecutionId = value.at("actionExecutionId").get<
        GameFlowActionExecutionId>();
    result.actionId = value.at("actionId").get<std::string>();
    const auto depth = value.at("callDepth").get<std::uint64_t>();
    if (depth > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("GameFlow replay call depth overflows.");
    }
    result.callDepth = static_cast<std::size_t>(depth);
    result.cancellation = decodeCancellation(
        value.at("cancellation").get<std::string>());
    result.deltaSeconds = value.at("deltaSeconds").get<double>();
    if (!std::isfinite(result.deltaSeconds)) {
        throw std::runtime_error("GameFlow replay delta is not finite.");
    }
    result.flowId = value.at("flowId").get<std::string>();
    result.generation = value.at("generation").get<GameFlowGeneration>();
    result.guardId = value.at("guardId").get<std::string>();
    result.intentId = value.at("intentId").get<std::string>();
    result.intentOrigin = decodeIntentOrigin(
        value.at("intentOrigin").get<std::string>());
    result.kind = decodeKind(value.at("kind").get<std::string>());
    result.outcome = decodeOutcome(value.at("outcome").get<std::string>());
    result.payload = decodePayload(value.at("payload"));
    result.programFingerprint = value.at("programFingerprint").get<
        std::uint64_t>();
    result.resultStateId = value.at("resultStateId").get<std::string>();
    result.schemaVersion = value.at("schemaVersion").get<std::uint32_t>();
    result.sequence = value.at("sequence").get<std::uint64_t>();
    result.stateId = value.at("stateId").get<std::string>();
    result.targetFlowId = value.at("targetFlowId").get<std::string>();
    result.transitionId = value.at("transitionId").get<std::string>();
    return result;
}

std::string replayError(ayt::replay::IReplayPlayer::Error value)
{
    return "AYReplay read failed with error "
        + std::to_string(static_cast<unsigned>(value)) + ".";
}

} // namespace

ayt::replay::ReplayEventType gameFlowReplayEventType(
    GameFlowDeterminismKind kind) noexcept
{
    switch (kind) {
    case GameFlowDeterminismKind::Intent: return kEvtGameFlowIntent;
    case GameFlowDeterminismKind::Update: return kEvtGameFlowUpdate;
    case GameFlowDeterminismKind::GuardOutcome:
        return kEvtGameFlowGuardOutcome;
    case GameFlowDeterminismKind::TransitionDecision:
        return kEvtGameFlowTransitionDecision;
    case GameFlowDeterminismKind::ActionOutcome:
        return kEvtGameFlowActionOutcome;
    case GameFlowDeterminismKind::AsyncCompletion:
        return kEvtGameFlowAsyncCompletion;
    case GameFlowDeterminismKind::Cancellation:
        return kEvtGameFlowCancellation;
    case GameFlowDeterminismKind::SubflowEnter:
        return kEvtGameFlowSubflowEnter;
    case GameFlowDeterminismKind::SubflowReturn:
        return kEvtGameFlowSubflowReturn;
    case GameFlowDeterminismKind::Terminal: return kEvtGameFlowTerminal;
    case GameFlowDeterminismKind::ProgramManifest:
        return kEvtGameFlowProgramManifest;
    }
    return 0u;
}

bool serializeGameFlowDeterminismRecord(
    const GameFlowDeterminismRecord& record,
    std::string& jsonText,
    std::string* error)
{
    try {
        if (record.schemaVersion != kGameFlowDeterminismSchemaVersion) {
            throw std::runtime_error(
                "Unsupported GameFlow determinism schema version.");
        }
        std::string encoded = encodeRecord(record).dump();
        if (encoded.size() > kMaximumWireBytes) {
            throw std::runtime_error("GameFlow replay record is too large.");
        }
        jsonText = std::move(encoded);
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    } catch (...) {
        if (error != nullptr) *error = "Could not encode GameFlow replay JSON.";
        return false;
    }
}

bool deserializeGameFlowDeterminismRecord(
    std::string_view jsonText,
    GameFlowDeterminismRecord& record,
    std::string* error)
{
    try {
        if (jsonText.size() > kMaximumWireBytes) {
            throw std::runtime_error("GameFlow replay record is too large.");
        }
        const json parsed = json::parse(jsonText.begin(), jsonText.end());
        GameFlowDeterminismRecord decoded = decodeRecord(parsed);
        if (decoded.schemaVersion != kGameFlowDeterminismSchemaVersion) {
            throw std::runtime_error(
                "Unsupported GameFlow determinism schema version.");
        }
        if (encodeRecord(decoded).dump() != jsonText) {
            throw std::runtime_error(
                "GameFlow replay JSON is not in canonical form.");
        }
        record = std::move(decoded);
        if (error != nullptr) error->clear();
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) *error = exception.what();
        return false;
    } catch (...) {
        if (error != nullptr) *error = "Could not decode GameFlow replay JSON.";
        return false;
    }
}

GameFlowReplayCaptureExchange::GameFlowReplayCaptureExchange(
    ayt::replay::IReplayRecorder& recorder) noexcept
    : _recorder(&recorder)
{
}

GameFlowDeterminismMode
GameFlowReplayCaptureExchange::mode() const noexcept
{
    return GameFlowDeterminismMode::Capture;
}

bool GameFlowReplayCaptureExchange::exchange(
    GameFlowDeterminismRecord& record, std::string& error)
{
    if (_faulted) {
        error = _fault.empty() ? "GameFlow replay capture failed." : _fault;
        return false;
    }
    try {
        if (_recorder == nullptr || !_recorder->isOpen()) {
            retainFault("AYReplay recorder is not open.");
        } else {
            std::string payload;
            if (!serializeGameFlowDeterminismRecord(record, payload, &error)) {
                retainFault(std::move(error));
            } else if (!_recorder->recordEvent(record.sequence,
                    gameFlowReplayEventType(record.kind),
                    reinterpret_cast<const std::uint8_t*>(payload.data()),
                    payload.size())) {
                retainFault("AYReplay could not write a GameFlow event.");
            }
        }
    } catch (const std::exception& exception) {
        retainFault(std::string("GameFlow replay capture threw: ")
            + exception.what());
    } catch (...) {
        retainFault("GameFlow replay capture threw.");
    }
    error = _fault;
    return !_faulted;
}

bool GameFlowReplayCaptureExchange::healthy() const noexcept
{
    return !_faulted;
}

std::string_view GameFlowReplayCaptureExchange::fault() const noexcept
{
    return _faulted && _fault.empty()
        ? std::string_view("GameFlow replay capture failed.")
        : std::string_view(_fault);
}

void GameFlowReplayCaptureExchange::retainFault(std::string error) noexcept
{
    if (_faulted) return;
    _faulted = true;
    try {
        _fault = error.empty() ? "GameFlow replay capture failed."
                               : std::move(error);
    } catch (...) {
    }
}

GameFlowReplayPlaybackExchange::GameFlowReplayPlaybackExchange(
    ayt::replay::IReplayPlayer& player) noexcept
    : _player(&player)
{
}

GameFlowDeterminismMode
GameFlowReplayPlaybackExchange::mode() const noexcept
{
    return GameFlowDeterminismMode::Playback;
}

bool GameFlowReplayPlaybackExchange::exchange(
    GameFlowDeterminismRecord& record, std::string& error)
{
    if (_faulted) {
        error = _fault.empty() ? "GameFlow replay playback failed." : _fault;
        return false;
    }
    try {
        ayt::replay::ReplayEventHeader header{};
        std::vector<std::uint8_t> payload;
        bool checkpoint = false;
        const auto read = _player == nullptr
            ? ayt::replay::IReplayPlayer::Error::IoError
            : _player->readNextEvent(header, payload, &checkpoint);
        if (read != ayt::replay::IReplayPlayer::Error::Ok) {
            retainFault(replayError(read));
        } else if (checkpoint) {
            retainFault("Unexpected checkpoint in the GameFlow stream.");
        } else if (header.eventType
            == ayt::replay::kEvtFoundation_SessionEnd) {
            retainFault("GameFlow replay stream ended early.");
        } else {
            GameFlowDeterminismRecord decoded;
            const std::string_view text(
                reinterpret_cast<const char*>(payload.data()), payload.size());
            if (!deserializeGameFlowDeterminismRecord(text, decoded, &error)) {
                retainFault(std::move(error));
            } else if (header.eventType
                != gameFlowReplayEventType(decoded.kind)) {
                retainFault(
                    "GameFlow replay event id does not match its JSON kind.");
            } else if (header.tick != decoded.sequence) {
                retainFault(
                    "GameFlow replay tick does not match its sequence.");
            } else {
                record = std::move(decoded);
            }
        }
    } catch (const std::exception& exception) {
        retainFault(std::string("GameFlow replay playback threw: ")
            + exception.what());
    } catch (...) {
        retainFault("GameFlow replay playback threw.");
    }
    error = _fault;
    return !_faulted;
}

bool GameFlowReplayPlaybackExchange::healthy() const noexcept
{
    return !_faulted;
}

std::string_view GameFlowReplayPlaybackExchange::fault() const noexcept
{
    return _faulted && _fault.empty()
        ? std::string_view("GameFlow replay playback failed.")
        : std::string_view(_fault);
}

void GameFlowReplayPlaybackExchange::retainFault(std::string error) noexcept
{
    if (_faulted) return;
    _faulted = true;
    try {
        _fault = error.empty() ? "GameFlow replay playback failed."
                               : std::move(error);
    } catch (...) {
    }
}

} // namespace ayt::app
