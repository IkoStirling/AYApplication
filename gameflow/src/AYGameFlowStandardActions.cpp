#include <AYApplication/GameFlowStandardActions.h>

#include <utility>
#include <vector>

namespace ayt::app
{
bool gameFlowActionTypeCompatible(
    const GameFlowActionTypeDefinition& value,
    const GameFlowActionTypeDefinition& expected) noexcept
{
    if (value.id != expected.id
        || value.asynchronous != expected.asynchronous
        || value.arguments.size() != expected.arguments.size()
        || value.references != expected.references) {
        return false;
    }
    for (std::size_t index = 0; index < value.arguments.size(); ++index) {
        const auto& left = value.arguments[index];
        const auto& right = expected.arguments[index];
        if (left.id != right.id || left.type != right.type
            || left.required != right.required
            || left.defaultValue != right.defaultValue) {
            return false;
        }
    }
    return true;
}

namespace
{

const std::string* stringArgument(const GameFlowActionCall& action,
                                  std::string_view id)
{
    const auto found = action.arguments.find(id);
    return found == action.arguments.end()
        ? nullptr : std::get_if<std::string>(&found->second.data);
}

bool fail(std::string message, std::string* error)
{
    if (error != nullptr) *error = std::move(message);
    return false;
}

} // namespace

GameFlowActionTypeDefinition gameFlowWorldActionType()
{
    return {
        std::string(kGameFlowActionWorldReplace),
        {{"worldId", GameFlowValueType::String, true, {}}},
        true,
        {{"worldId", GameFlowReferenceKind::WorldId, false}},
    };
}

std::vector<GameFlowActionTypeDefinition> gameFlowUIActionTypes()
{
    return {
        {std::string(kGameFlowActionUIFlowStart),
            {{"entry", GameFlowValueType::String, false, ""}}, false,
            {{"entry", GameFlowReferenceKind::UIFlowEntry, true}}},
        {std::string(kGameFlowActionUIContextActivate),
            {{"activationId", GameFlowValueType::String, true, {}},
             {"contextId", GameFlowValueType::String, true, {}},
             {"scope", GameFlowValueType::String, false, "application"},
             {"scopeKey", GameFlowValueType::String, false, ""}}, false,
            {{"contextId", GameFlowReferenceKind::UIContext, false}}},
        {std::string(kGameFlowActionUIContextDeactivate),
            {{"activationId", GameFlowValueType::String, true, {}}}, false},
        {std::string(kGameFlowActionUISignalEmit),
            {{"signalId", GameFlowValueType::String, true, {}}}, false,
            {{"signalId", GameFlowReferenceKind::UISignal, false}}},
    };
}

bool validateGameFlowStandardActionSemantics(
    const GameFlowActionCall& action,
    std::string* error)
{
    if (action.action == kGameFlowActionWorldReplace) {
        const std::string* world = stringArgument(action, "worldId");
        if (world == nullptr || world->empty()) {
            return fail("world.replace requires a non-empty worldId.", error);
        }
    } else if (action.action == kGameFlowActionUIFlowStart) {
        if (stringArgument(action, "entry") == nullptr) {
            return fail("ui.flow.start requires a string entry.", error);
        }
    } else if (action.action == kGameFlowActionUIContextActivate) {
        const std::string* activation = stringArgument(action, "activationId");
        const std::string* context = stringArgument(action, "contextId");
        const std::string* scope = stringArgument(action, "scope");
        const std::string* scopeKey = stringArgument(action, "scopeKey");
        if (activation == nullptr || activation->empty()) {
            return fail(
                "ui.context.activate requires a non-empty activationId.",
                error);
        }
        if (context == nullptr || context->empty()) {
            return fail(
                "ui.context.activate requires a non-empty contextId.", error);
        }
        if (scope == nullptr
            || (*scope != "application" && *scope != "world"
                && *scope != "owner" && *scope != "transient")) {
            return fail("ui.context.activate has an invalid scope.", error);
        }
        if (scopeKey == nullptr
            || (*scope == "application" && !scopeKey->empty()
                && *scopeKey != "application")) {
            return fail(
                "ui.context.activate has an invalid application scopeKey.",
                error);
        }
    } else if (action.action == kGameFlowActionUIContextDeactivate) {
        const std::string* activation = stringArgument(action, "activationId");
        if (activation == nullptr || activation->empty()) {
            return fail(
                "ui.context.deactivate requires a non-empty activationId.",
                error);
        }
    } else if (action.action == kGameFlowActionUISignalEmit) {
        const std::string* signal = stringArgument(action, "signalId");
        if (signal == nullptr || signal->empty()) {
            return fail("ui.signal.emit requires a non-empty signalId.", error);
        }
    }
    if (error != nullptr) error->clear();
    return true;
}

bool registerGameFlowWorldActionType(
    GameFlowActionRegistry& registry,
    std::string* error)
{
    GameFlowActionTypeDefinition expected = gameFlowWorldActionType();
    const auto* existing = registry.findAction(expected.id);
    if (existing != nullptr) {
        if (gameFlowActionTypeCompatible(*existing, expected)) {
            if (error != nullptr) error->clear();
            return true;
        }
        if (error != nullptr) {
            *error = "Existing world.replace action type is incompatible.";
        }
        return false;
    }
    return registry.registerActionType(std::move(expected), false, error);
}

bool registerGameFlowUIActionTypes(
    GameFlowActionRegistry& registry,
    std::string* error)
{
    std::vector<std::string> added;
    for (auto definition : gameFlowUIActionTypes()) {
        const auto* existing = registry.findAction(definition.id);
        if (existing != nullptr) {
            if (gameFlowActionTypeCompatible(*existing, definition)) continue;
            for (auto current = added.rbegin(); current != added.rend(); ++current) {
                (void)registry.unregisterAction(*current);
            }
            if (error != nullptr) {
                *error = "Existing GameFlow action type '" + definition.id
                    + "' is incompatible with the standard UI contract.";
            }
            return false;
        }
        const std::string id = definition.id;
        if (!registry.registerActionType(std::move(definition), false, error)) {
            for (auto current = added.rbegin(); current != added.rend(); ++current) {
                (void)registry.unregisterAction(*current);
            }
            return false;
        }
        added.push_back(id);
    }
    if (error != nullptr) error->clear();
    return true;
}

} // namespace ayt::app
