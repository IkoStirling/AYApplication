#include <AYApplication/GameFlowContract.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <string_view>

namespace ayt::app
{
namespace
{
using json = nlohmann::json;

bool onlyKeys(const json& value,
              std::initializer_list<std::string_view> allowed,
              std::string_view role,
              std::string& error)
{
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (std::find(allowed.begin(), allowed.end(), item.key())
            == allowed.end()) {
            error = std::string(role) + " contains unknown field '"
                + item.key() + "'.";
            return false;
        }
    }
    return true;
}

bool valueType(std::string_view name, GameFlowValueType& result)
{
    if (name == "boolean") result = GameFlowValueType::Boolean;
    else if (name == "integer") result = GameFlowValueType::Integer;
    else if (name == "number") result = GameFlowValueType::Number;
    else if (name == "string") result = GameFlowValueType::String;
    else return false;
    return true;
}

bool defaultValue(const json& value, GameFlowValueType type,
                  GameFlowValue& result)
{
    if (type == GameFlowValueType::Boolean && value.is_boolean()) {
        result = value.get<bool>(); return true;
    }
    if (type == GameFlowValueType::String && value.is_string()) {
        result = value.get<std::string>(); return true;
    }
    if (type == GameFlowValueType::Integer && value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(
                (std::numeric_limits<std::int64_t>::max)())) return false;
        result = static_cast<std::int64_t>(number); return true;
    }
    if (type == GameFlowValueType::Integer && value.is_number_integer()) {
        result = value.get<std::int64_t>(); return true;
    }
    if (type == GameFlowValueType::Number) {
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int64_t>::max)())) return false;
            result = static_cast<std::int64_t>(number); return true;
        }
        if (value.is_number_integer()) {
            result = value.get<std::int64_t>(); return true;
        }
        if (value.is_number_float()) {
            result = value.get<double>(); return true;
        }
    }
    return false;
}

bool fields(const json& value, std::vector<GameFlowFieldDefinition>& result,
            std::string& error)
{
    if (!value.is_array()) {
        error = "Contract fields must be an array."; return false;
    }
    for (const auto& item : value) {
        if (!item.is_object() || !item.contains("id")
            || !item["id"].is_string() || item["id"].get<std::string>().empty()
            || !item.contains("type") || !item["type"].is_string()
            || !onlyKeys(item, {"id", "type", "required", "default"},
                         "Contract field", error)) {
            if (error.empty()) error = "Every contract field needs non-empty id and type strings.";
            return false;
        }
        GameFlowFieldDefinition field;
        field.id = item["id"].get<std::string>();
        if (!valueType(item["type"].get<std::string>(), field.type)) {
            error = "Unknown GameFlow field type for '" + field.id + "'.";
            return false;
        }
        if (item.contains("required")) {
            if (!item["required"].is_boolean()) {
                error = "Contract field required must be boolean."; return false;
            }
            field.required = item["required"].get<bool>();
        }
        if (item.contains("default")
            && !defaultValue(item["default"], field.type, field.defaultValue)) {
            error = "Contract default for '" + field.id + "' does not match its type.";
            return false;
        }
        result.push_back(std::move(field));
    }
    return true;
}

bool referenceKind(std::string_view name, GameFlowReferenceKind& result)
{
    if (name == "asset") result = GameFlowReferenceKind::AssetPath;
    else if (name == "world") result = GameFlowReferenceKind::WorldId;
    else if (name == "ui-entry") result = GameFlowReferenceKind::UIFlowEntry;
    else if (name == "ui-context") result = GameFlowReferenceKind::UIContext;
    else if (name == "ui-signal") result = GameFlowReferenceKind::UISignal;
    else return false;
    return true;
}
}

bool loadGameFlowContract(std::string_view jsonText,
                          GameFlowActionRegistry& registry,
                          std::string* error)
{
    std::string message;
    try {
        const json root = json::parse(jsonText);
        if (!root.is_object() || root.value("schemaVersion", 0u) != 1u
            || !onlyKeys(root, {"schemaVersion", "actions", "guards"},
                         "GameFlow contract manifest", message)) {
            if (message.empty()) message = "GameFlow contract manifest requires schemaVersion 1.";
            if (error) *error = message;
            return false;
        }
        const json actions = root.value("actions", json::array());
        const json guards = root.value("guards", json::array());
        if (!actions.is_array() || !guards.is_array()) {
            message = "GameFlow contract actions and guards must be arrays.";
            if (error) *error = message;
            return false;
        }
        for (const auto& item : actions) {
            if (!item.is_object() || !item.contains("id")
                || !item["id"].is_string()
                || !onlyKeys(item, {"id", "arguments", "asynchronous", "references"},
                             "GameFlow action contract", message)) {
                if (message.empty()) message = "Every GameFlow action contract needs an id string.";
                if (error) *error = message;
                return false;
            }
            GameFlowActionTypeDefinition definition;
            definition.id = item["id"].get<std::string>();
            if (!fields(item.value("arguments", json::array()),
                        definition.arguments, message)) {
                if (error) *error = message; return false;
            }
            if (item.contains("asynchronous")) {
                if (!item["asynchronous"].is_boolean()) {
                    message = "Action asynchronous must be boolean.";
                    if (error) *error = message; return false;
                }
                definition.asynchronous = item["asynchronous"].get<bool>();
            }
            const json refs = item.value("references", json::array());
            if (!refs.is_array()) {
                message = "Action references must be an array.";
                if (error) *error = message; return false;
            }
            for (const auto& ref : refs) {
                if (!ref.is_object() || !ref.contains("argument")
                    || !ref["argument"].is_string() || !ref.contains("kind")
                    || !ref["kind"].is_string()
                    || !onlyKeys(ref, {"argument", "kind", "allowEmpty"},
                                 "Action reference", message)) {
                    if (message.empty()) message = "Every action reference needs argument and kind strings.";
                    if (error) *error = message; return false;
                }
                GameFlowActionReferenceDefinition reference;
                reference.argumentId = ref["argument"].get<std::string>();
                if (!referenceKind(ref["kind"].get<std::string>(), reference.kind)) {
                    message = "Unknown action reference kind for '" + reference.argumentId + "'.";
                    if (error) *error = message; return false;
                }
                if (ref.contains("allowEmpty")) {
                    if (!ref["allowEmpty"].is_boolean()) {
                        message = "Action reference allowEmpty must be boolean.";
                        if (error) *error = message; return false;
                    }
                    reference.allowEmpty = ref["allowEmpty"].get<bool>();
                }
                definition.references.push_back(std::move(reference));
            }
            if (!registry.registerActionType(
                    std::move(definition), false, &message)) {
                if (error) *error = message; return false;
            }
        }
        for (const auto& item : guards) {
            if (!item.is_object() || !item.contains("id")
                || !item["id"].is_string()
                || !onlyKeys(item, {"id", "arguments"},
                             "GameFlow guard contract", message)) {
                if (message.empty()) message = "Every GameFlow guard contract needs an id string.";
                if (error) *error = message; return false;
            }
            GameFlowGuardTypeDefinition definition;
            definition.id = item["id"].get<std::string>();
            if (!fields(item.value("arguments", json::array()),
                        definition.arguments, message)
                || !registry.registerGuardType(
                    std::move(definition), false, &message)) {
                if (error) *error = message; return false;
            }
        }
    } catch (const std::exception& exception) {
        message = std::string("GameFlow contract manifest is invalid: ")
            + exception.what();
        if (error) *error = message;
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace ayt::app
