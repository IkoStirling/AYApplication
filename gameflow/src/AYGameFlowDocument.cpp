#include <AYApplication/GameFlowActionRegistry.h>
#include <AYApplication/GameFlowDocument.h>
#include <AYApplication/GameFlowMigration.h>
#include <AYApplication/GameFlowProgram.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace ayt::app
{
namespace
{

using json = nlohmann::json;

void addDiagnostic(std::vector<GameFlowDiagnostic>* diagnostics,
                   GameFlowDiagnosticSeverity severity,
                   std::string path,
                   std::string message)
{
    if (diagnostics == nullptr) return;
    diagnostics->push_back(
        {severity, std::move(path), std::move(message)});
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

bool validateFields(const std::vector<GameFlowFieldDefinition>& fields,
                    std::string_view path,
                    std::vector<GameFlowDiagnostic>* diagnostics)
{
    bool valid = true;
    std::set<std::string, std::less<>> ids;
    for (std::size_t index = 0; index < fields.size(); ++index) {
        const auto& field = fields[index];
        const std::string fieldPath = std::string(path) + "["
            + std::to_string(index) + "]";
        if (field.id.empty()) {
            addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error,
                fieldPath + ".id", "Field id must not be empty.");
            valid = false;
        } else if (!ids.insert(field.id).second) {
            addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error,
                fieldPath + ".id", "Duplicate field id '" + field.id + "'.");
            valid = false;
        }
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)
            && !valueMatches(field.defaultValue, field.type)) {
            addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error,
                fieldPath + ".default",
                "Default value does not match type '"
                    + std::string(gameFlowValueTypeName(field.type)) + "'.");
            valid = false;
        }
    }
    return valid;
}

bool validateArguments(
    const GameFlowPayload& arguments,
    const std::vector<GameFlowFieldDefinition>& fields,
    std::string_view path,
    std::vector<GameFlowDiagnostic>* diagnostics)
{
    bool valid = true;
    std::map<std::string, const GameFlowFieldDefinition*, std::less<>> schema;
    for (const auto& field : fields) schema.emplace(field.id, &field);
    for (const auto& [id, value] : arguments) {
        const auto found = schema.find(id);
        if (found == schema.end()) {
            addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error,
                std::string(path) + "." + id,
                "Unknown authored argument '" + id + "'.");
            valid = false;
        } else if (!valueMatches(value, found->second->type)) {
            addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error,
                std::string(path) + "." + id,
                "Argument does not match type '"
                    + std::string(gameFlowValueTypeName(found->second->type))
                    + "'.");
            valid = false;
        }
    }
    for (const auto& field : fields) {
        if (field.required && !arguments.contains(field.id)
            && std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error,
                std::string(path) + "." + field.id,
                "Required authored argument is missing.");
            valid = false;
        }
    }
    return valid;
}

GameFlowValueType decodeValueType(std::string_view value)
{
    if (value == "boolean") return GameFlowValueType::Boolean;
    if (value == "integer") return GameFlowValueType::Integer;
    if (value == "number") return GameFlowValueType::Number;
    if (value == "string") return GameFlowValueType::String;
    throw std::runtime_error("Unknown GameFlow value type '"
        + std::string(value) + "'.");
}

GameFlowValue decodeValue(const json& value)
{
    if (value.is_null()) return GameFlowValue{};
    if (value.is_boolean()) return GameFlowValue(value.get<bool>());
    if (value.is_number_integer()) {
        return GameFlowValue(value.get<std::int64_t>());
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            throw std::runtime_error("Unsigned integer exceeds int64 range.");
        }
        return GameFlowValue(static_cast<std::int64_t>(number));
    }
    if (value.is_number_float()) return GameFlowValue(value.get<double>());
    if (value.is_string()) return GameFlowValue(value.get<std::string>());
    if (value.is_array()) {
        GameFlowValue::Array result;
        result.reserve(value.size());
        for (const auto& item : value) result.push_back(decodeValue(item));
        return GameFlowValue(std::move(result));
    }
    if (value.is_object()) {
        GameFlowValue::Object result;
        for (auto item = value.begin(); item != value.end(); ++item) {
            result.emplace(item.key(), decodeValue(item.value()));
        }
        return GameFlowValue(std::move(result));
    }
    throw std::runtime_error("Unsupported JSON value.");
}

json encodeValue(const GameFlowValue& value)
{
    return std::visit([](const auto& stored) -> json {
        using T = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
            return nullptr;
        } else if constexpr (std::is_same_v<T, GameFlowValue::Array>) {
            json result = json::array();
            for (const auto& item : stored) result.push_back(encodeValue(item));
            return result;
        } else if constexpr (std::is_same_v<T, GameFlowValue::Object>) {
            json result = json::object();
            for (const auto& [key, item] : stored) {
                result[key] = encodeValue(item);
            }
            return result;
        } else {
            return json(stored);
        }
    }, value.data);
}

GameFlowPayload decodePayload(const json& value)
{
    if (!value.is_object()) {
        throw std::runtime_error("Expected an object payload.");
    }
    GameFlowPayload result;
    for (auto item = value.begin(); item != value.end(); ++item) {
        result.emplace(item.key(), decodeValue(item.value()));
    }
    return result;
}

json encodePayload(const GameFlowPayload& payload)
{
    json result = json::object();
    for (const auto& [id, value] : payload) result[id] = encodeValue(value);
    return result;
}

std::vector<GameFlowFieldDefinition> decodeFields(const json& values)
{
    if (!values.is_array()) throw std::runtime_error("Expected a field array.");
    std::vector<GameFlowFieldDefinition> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (!value.is_object()) throw std::runtime_error("Field must be an object.");
        GameFlowFieldDefinition field;
        field.id = value.at("id").get<std::string>();
        field.type = decodeValueType(value.at("type").get<std::string>());
        field.required = value.value("required", false);
        if (value.contains("default")) field.defaultValue = decodeValue(value["default"]);
        result.push_back(std::move(field));
    }
    return result;
}

json encodeFields(const std::vector<GameFlowFieldDefinition>& fields)
{
    json result = json::array();
    for (const auto& field : fields) {
        json value = {
            {"id", field.id},
            {"type", gameFlowValueTypeName(field.type)},
            {"required", field.required},
        };
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            value["default"] = encodeValue(field.defaultValue);
        }
        result.push_back(std::move(value));
    }
    return result;
}

bool hasError(const std::vector<GameFlowDiagnostic>* diagnostics)
{
    return diagnostics != nullptr && std::any_of(
        diagnostics->begin(), diagnostics->end(),
        [](const GameFlowDiagnostic& value) {
            return value.severity == GameFlowDiagnosticSeverity::Error;
        });
}

} // namespace

const GameFlowIntentDefinition* GameFlowDocument::findIntent(
    std::string_view value) const noexcept
{
    const auto found = std::find_if(intents.begin(), intents.end(),
        [value](const auto& intent) { return intent.id == value; });
    return found == intents.end() ? nullptr : &*found;
}

const GameFlowStateDefinition* GameFlowDocument::findState(
    std::string_view value) const noexcept
{
    const auto found = std::find_if(states.begin(), states.end(),
        [value](const auto& state) { return state.id == value; });
    return found == states.end() ? nullptr : &*found;
}

bool validateGameFlow(const GameFlowDocument& document,
                      const GameFlowActionRegistry* registry,
                      std::vector<GameFlowDiagnostic>* diagnostics)
{
    if (diagnostics != nullptr) diagnostics->clear();
    bool valid = true;
    const auto error = [&](std::string path, std::string message) {
        valid = false;
        addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error,
            std::move(path), std::move(message));
    };

    if (document.schemaVersion != kGameFlowSchemaVersion) {
        error("$.schemaVersion", "Unsupported GameFlow schema version.");
    }
    if (document.id.empty()) error("$.id", "Flow id must not be empty.");
    if (document.initialState.empty()) {
        error("$.initialState", "Initial state must not be empty.");
    }
    if (!validateFields(
            document.entryParameters, "$.entryParameters", diagnostics)) {
        valid = false;
    }
    if (!validateFields(document.result, "$.result", diagnostics)) {
        valid = false;
    }

    std::map<std::string, std::size_t, std::less<>> stateIndices;
    for (std::size_t index = 0; index < document.states.size(); ++index) {
        const auto& state = document.states[index];
        const std::string path = "$.states[" + std::to_string(index) + "]";
        if (state.id.empty()) {
            error(path + ".id", "State id must not be empty.");
        } else if (!stateIndices.emplace(state.id, index).second) {
            error(path + ".id", "Duplicate state id '" + state.id + "'.");
        }
    }
    if (!document.initialState.empty()
        && !stateIndices.contains(document.initialState)) {
        error("$.initialState", "Initial state '" + document.initialState
            + "' does not exist.");
    }

    for (std::size_t index = 0; index < document.states.size(); ++index) {
        const auto& state = document.states[index];
        const std::string path = "$.states[" + std::to_string(index) + "]";
        if (!state.parent.empty() && !stateIndices.contains(state.parent)) {
            error(path + ".parent", "Parent state '" + state.parent
                + "' does not exist.");
        }
        if (!state.initialChild.empty()) {
            const auto child = stateIndices.find(state.initialChild);
            if (child == stateIndices.end()) {
                error(path + ".initialChild", "Initial child '"
                    + state.initialChild + "' does not exist.");
            } else if (document.states[child->second].parent != state.id) {
                error(path + ".initialChild", "Initial child must name a direct child.");
            }
        }
    }

    std::vector<std::uint8_t> parentMarks(document.states.size(), 0u);
    std::function<void(std::size_t)> visitParent = [&](std::size_t index) {
        if (parentMarks[index] == 2u) return;
        if (parentMarks[index] == 1u) {
            error("$.states[" + std::to_string(index) + "].parent",
                "State parent hierarchy contains a cycle.");
            return;
        }
        parentMarks[index] = 1u;
        const auto& parent = document.states[index].parent;
        const auto found = stateIndices.find(parent);
        if (!parent.empty() && found != stateIndices.end()) visitParent(found->second);
        parentMarks[index] = 2u;
    };
    for (std::size_t index = 0; index < document.states.size(); ++index) {
        visitParent(index);
    }

    std::map<std::string, std::size_t, std::less<>> intentIndices;
    for (std::size_t index = 0; index < document.intents.size(); ++index) {
        const auto& intent = document.intents[index];
        const std::string path = "$.intents[" + std::to_string(index) + "]";
        if (intent.id.empty()) {
            error(path + ".id", "Intent id must not be empty.");
        } else if (!intentIndices.emplace(intent.id, index).second) {
            error(path + ".id", "Duplicate intent id '" + intent.id + "'.");
        }
        if (!validateFields(
                intent.payload, path + ".payload", diagnostics)) {
            valid = false;
        }
    }

    std::set<std::string, std::less<>> transitionIds;
    for (std::size_t index = 0; index < document.transitions.size(); ++index) {
        const auto& transition = document.transitions[index];
        const std::string path = "$.transitions[" + std::to_string(index) + "]";
        if (transition.id.empty()) {
            error(path + ".id", "Transition id must not be empty.");
        } else if (!transitionIds.insert(transition.id).second) {
            error(path + ".id", "Duplicate transition id '"
                + transition.id + "'.");
        }
        const auto validateStateReference = [&](std::string_view field,
                                                const std::string& value,
                                                bool optional) {
            if (value.empty() && optional) return;
            if (!stateIndices.contains(value)) {
                error(path + "." + std::string(field), "State '" + value
                    + "' does not exist.");
            }
        };
        validateStateReference("from", transition.fromState, false);
        validateStateReference("to", transition.toState, false);
        validateStateReference("onFailure", transition.onFailureState, true);
        validateStateReference("onCancel", transition.onCancelState, true);
        if (!intentIndices.contains(transition.triggerIntent)) {
            error(path + ".intent", "Intent '" + transition.triggerIntent
                + "' does not exist.");
        }
        if (!std::isfinite(transition.timeoutSeconds)
            || transition.timeoutSeconds < 0.0) {
            error(path + ".timeoutSeconds",
                "Timeout must be finite and non-negative.");
        }

        if (!transition.guard.guard.empty()) {
            if (registry == nullptr) {
                // Structural-only validation deliberately preserves host types.
            } else if (const auto* definition =
                           registry->findGuard(transition.guard.guard)) {
                if (!validateArguments(transition.guard.arguments,
                        definition->arguments, path + ".guard.arguments",
                        diagnostics)) {
                    valid = false;
                }
            } else {
                error(path + ".guard.id", "Guard '" + transition.guard.guard
                    + "' is not registered.");
            }
        }

        for (std::size_t actionIndex = 0;
             actionIndex < transition.actions.size(); ++actionIndex) {
            const auto& action = transition.actions[actionIndex];
            const std::string actionPath = path + ".actions["
                + std::to_string(actionIndex) + "]";
            if (action.action.empty()) {
                error(actionPath + ".id", "Action id must not be empty.");
            } else if (action.action == kGameFlowActionEnter) {
                const auto subflow = action.arguments.find(
                    std::string(kGameFlowSubflowIdArgument));
                const auto* subflowId = subflow == action.arguments.end()
                    ? nullptr
                    : std::get_if<std::string>(&subflow->second.data);
                if (subflowId == nullptr || subflowId->empty()) {
                    error(actionPath + ".arguments.subflowId",
                        "flow.enter requires a non-empty string subflowId.");
                }
            } else if (action.action == kGameFlowActionReturn) {
                // Result fields are validated against this document by the
                // complete-program compiler, which also knows the caller.
            } else if (registry != nullptr) {
                if (const auto* definition = registry->findAction(action.action)) {
                    if (!validateArguments(action.arguments,
                            definition->arguments, actionPath + ".arguments",
                            diagnostics)) {
                        valid = false;
                    }
                } else {
                    error(actionPath + ".id", "Action '" + action.action
                        + "' is not registered.");
                }
            }
        }
    }
    return valid && !hasError(diagnostics);
}

bool GameFlowSerializer::deserialize(
    std::string_view jsonText,
    GameFlowDocument& document,
    std::vector<GameFlowDiagnostic>* diagnostics)
{
    return deserialize(jsonText, document, diagnostics, nullptr);
}

bool GameFlowSerializer::deserialize(
    std::string_view jsonText,
    GameFlowDocument& document,
    std::vector<GameFlowDiagnostic>* diagnostics,
    GameFlowMigrationReport* migrationReport)
{
    if (diagnostics != nullptr) diagnostics->clear();
    std::string migratedJson;
    if (!migrateGameFlowJson(
            jsonText, migratedJson, migrationReport, diagnostics, false)) {
        return false;
    }
    try {
        const json root = json::parse(migratedJson);
        if (!root.is_object()) throw std::runtime_error("Root must be an object.");

        GameFlowDocument decoded;
        decoded.schemaVersion = root.at("schemaVersion").get<std::uint32_t>();
        decoded.id = root.at("id").get<std::string>();
        decoded.initialState = root.at("initialState").get<std::string>();
        if (root.contains("entryParameters")) {
            decoded.entryParameters = decodeFields(root["entryParameters"]);
        }
        if (root.contains("result")) {
            decoded.result = decodeFields(root["result"]);
        }
        if (root.contains("extensions")) {
            if (!root["extensions"].is_object()) {
                throw std::runtime_error("'extensions' must be an object.");
            }
            decoded.extensions = decodePayload(root["extensions"]);
        }

        if (root.contains("intents")) {
            if (!root["intents"].is_array()) {
                throw std::runtime_error("'intents' must be an array.");
            }
            for (const auto& value : root["intents"]) {
                GameFlowIntentDefinition intent;
                intent.id = value.at("id").get<std::string>();
                if (value.contains("payload")) {
                    intent.payload = decodeFields(value["payload"]);
                }
                decoded.intents.push_back(std::move(intent));
            }
        }

        if (!root.contains("states") || !root["states"].is_array()) {
            throw std::runtime_error("'states' must be an array.");
        }
        for (const auto& value : root["states"]) {
            decoded.states.push_back({
                value.at("id").get<std::string>(),
                value.value("parent", std::string{}),
                value.value("initialChild", std::string{}),
            });
        }

        if (root.contains("transitions")) {
            if (!root["transitions"].is_array()) {
                throw std::runtime_error("'transitions' must be an array.");
            }
            for (const auto& value : root["transitions"]) {
                GameFlowTransitionDefinition transition;
                transition.id = value.at("id").get<std::string>();
                transition.fromState = value.at("from").get<std::string>();
                transition.triggerIntent = value.at("intent").get<std::string>();
                transition.toState = value.at("to").get<std::string>();
                transition.onFailureState =
                    value.value("onFailure", std::string{});
                transition.onCancelState = value.value("onCancel", std::string{});
                transition.timeoutSeconds = value.value("timeoutSeconds", 0.0);
                transition.priority = value.value("priority", 0);
                if (value.contains("guard")) {
                    const auto& guard = value["guard"];
                    transition.guard.guard = guard.at("id").get<std::string>();
                    if (guard.contains("arguments")) {
                        transition.guard.arguments = decodePayload(guard["arguments"]);
                    }
                }
                if (value.contains("actions")) {
                    if (!value["actions"].is_array()) {
                        throw std::runtime_error("'actions' must be an array.");
                    }
                    for (const auto& actionValue : value["actions"]) {
                        GameFlowActionCall action;
                        action.action = actionValue.at("id").get<std::string>();
                        if (actionValue.contains("arguments")) {
                            action.arguments =
                                decodePayload(actionValue["arguments"]);
                        }
                        transition.actions.push_back(std::move(action));
                    }
                }
                decoded.transitions.push_back(std::move(transition));
            }
        }

        if (!validateGameFlow(decoded, nullptr, diagnostics)) return false;
        document = std::move(decoded);
        return true;
    } catch (const std::exception& exception) {
        addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error, "$",
            std::string("GameFlow JSON is invalid: ") + exception.what());
        return false;
    }
}

bool GameFlowSerializer::serialize(
    const GameFlowDocument& document,
    std::string& jsonText,
    std::vector<GameFlowDiagnostic>* diagnostics,
    bool pretty)
{
    if (!validateGameFlow(document, nullptr, diagnostics)) return false;
    try {
        json root = {
            {"schemaVersion", document.schemaVersion},
            {"id", document.id},
            {"initialState", document.initialState},
            {"entryParameters", encodeFields(document.entryParameters)},
            {"result", encodeFields(document.result)},
            {"extensions", encodePayload(document.extensions)},
            {"intents", json::array()},
            {"states", json::array()},
            {"transitions", json::array()},
        };
        for (const auto& intent : document.intents) {
            json value = {{"id", intent.id}};
            if (!intent.payload.empty()) value["payload"] = encodeFields(intent.payload);
            root["intents"].push_back(std::move(value));
        }
        for (const auto& state : document.states) {
            json value = {{"id", state.id}};
            if (!state.parent.empty()) value["parent"] = state.parent;
            if (!state.initialChild.empty()) {
                value["initialChild"] = state.initialChild;
            }
            root["states"].push_back(std::move(value));
        }
        for (const auto& transition : document.transitions) {
            json value = {
                {"id", transition.id},
                {"from", transition.fromState},
                {"intent", transition.triggerIntent},
                {"to", transition.toState},
            };
            if (!transition.guard.guard.empty()) {
                value["guard"] = {{"id", transition.guard.guard}};
                if (!transition.guard.arguments.empty()) {
                    value["guard"]["arguments"] =
                        encodePayload(transition.guard.arguments);
                }
            }
            if (!transition.actions.empty()) {
                value["actions"] = json::array();
                for (const auto& action : transition.actions) {
                    json actionValue = {{"id", action.action}};
                    if (!action.arguments.empty()) {
                        actionValue["arguments"] = encodePayload(action.arguments);
                    }
                    value["actions"].push_back(std::move(actionValue));
                }
            }
            if (!transition.onFailureState.empty()) {
                value["onFailure"] = transition.onFailureState;
            }
            if (!transition.onCancelState.empty()) {
                value["onCancel"] = transition.onCancelState;
            }
            if (transition.timeoutSeconds > 0.0) {
                value["timeoutSeconds"] = transition.timeoutSeconds;
            }
            if (transition.priority != 0) value["priority"] = transition.priority;
            root["transitions"].push_back(std::move(value));
        }
        jsonText = root.dump(pretty ? 2 : -1);
        return true;
    } catch (const std::exception& exception) {
        addDiagnostic(diagnostics, GameFlowDiagnosticSeverity::Error, "$",
            std::string("GameFlow JSON could not be serialized: ")
                + exception.what());
        return false;
    }
}

const char* gameFlowValueTypeName(GameFlowValueType value) noexcept
{
    switch (value) {
    case GameFlowValueType::Boolean: return "boolean";
    case GameFlowValueType::Integer: return "integer";
    case GameFlowValueType::Number: return "number";
    case GameFlowValueType::String: return "string";
    }
    return "string";
}

} // namespace ayt::app
