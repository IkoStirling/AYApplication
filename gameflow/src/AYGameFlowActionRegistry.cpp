#include <AYApplication/GameFlowActionRegistry.h>
#include <AYApplication/GameFlowProgram.h>

#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace ayt::app
{
namespace
{

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
                    std::string& error)
{
    std::set<std::string, std::less<>> ids;
    for (const auto& field : fields) {
        if (field.id.empty()) {
            error = "Field id must not be empty.";
            return false;
        }
        if (!ids.insert(field.id).second) {
            error = "Duplicate field id '" + field.id + "'.";
            return false;
        }
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)
            && !valueMatches(field.defaultValue, field.type)) {
            error = "Default value for field '" + field.id
                + "' does not match its declared type.";
            return false;
        }
    }
    return true;
}

} // namespace

GameFlowActionResult GameFlowActionResult::succeeded()
{
    return {};
}

GameFlowActionResult GameFlowActionResult::pending(
    GameFlowActionCancellationHandler cancellation)
{
    GameFlowActionResult result;
    result.state = GameFlowActionState::Pending;
    result.onCancel = std::move(cancellation);
    return result;
}

GameFlowActionResult GameFlowActionResult::failed(std::string value)
{
    GameFlowActionResult result;
    result.state = GameFlowActionState::Failed;
    result.message = std::move(value);
    return result;
}

GameFlowActionResult GameFlowActionResult::cancelled(std::string value)
{
    GameFlowActionResult result;
    result.state = GameFlowActionState::Cancelled;
    result.message = std::move(value);
    return result;
}

class GameFlowActionRegistry::Impl
{
public:
    struct ActionEntry
    {
        GameFlowActionTypeDefinition definition;
        GameFlowActionHandler handler;
    };

    struct GuardEntry
    {
        GameFlowGuardTypeDefinition definition;
        GameFlowGuardHandler handler;
    };

    std::map<std::string, ActionEntry, std::less<>> actions;
    std::map<std::string, GuardEntry, std::less<>> guards;
};

GameFlowActionRegistry::GameFlowActionRegistry()
    : _impl(std::make_unique<Impl>())
{
}

GameFlowActionRegistry::~GameFlowActionRegistry() = default;
GameFlowActionRegistry::GameFlowActionRegistry(
    GameFlowActionRegistry&&) noexcept = default;
GameFlowActionRegistry& GameFlowActionRegistry::operator=(
    GameFlowActionRegistry&&) noexcept = default;

bool GameFlowActionRegistry::registerActionType(
    GameFlowActionTypeDefinition definition,
    bool replace,
    std::string* error)
{
    std::string validationError;
    if (definition.id.empty()) {
        validationError = "Action id must not be empty.";
    } else if (isGameFlowControlAction(definition.id)) {
        validationError = "Action id '" + definition.id
            + "' is reserved by the GameFlow coordinator.";
    } else {
        validateFields(definition.arguments, validationError);
    }
    if (!validationError.empty()) {
        if (error != nullptr) *error = std::move(validationError);
        return false;
    }
    const auto found = _impl->actions.find(definition.id);
    if (found != _impl->actions.end()) {
        if (!replace) {
            if (error != nullptr) {
                *error = "Action '" + definition.id
                    + "' is already registered.";
            }
            return false;
        }
        found->second.definition = std::move(definition);
    } else {
        const std::string id = definition.id;
        _impl->actions.emplace(id,
            Impl::ActionEntry{std::move(definition), {}});
    }
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowActionRegistry::registerGuardType(
    GameFlowGuardTypeDefinition definition,
    bool replace,
    std::string* error)
{
    std::string validationError;
    if (definition.id.empty()) {
        validationError = "Guard id must not be empty.";
    } else {
        validateFields(definition.arguments, validationError);
    }
    if (!validationError.empty()) {
        if (error != nullptr) *error = std::move(validationError);
        return false;
    }
    const auto found = _impl->guards.find(definition.id);
    if (found != _impl->guards.end()) {
        if (!replace) {
            if (error != nullptr) {
                *error = "Guard '" + definition.id
                    + "' is already registered.";
            }
            return false;
        }
        found->second.definition = std::move(definition);
    } else {
        const std::string id = definition.id;
        _impl->guards.emplace(id,
            Impl::GuardEntry{std::move(definition), {}});
    }
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowActionRegistry::setActionHandler(
    std::string_view id,
    GameFlowActionHandler handler,
    std::string* error)
{
    if (isGameFlowControlAction(id)) {
        if (error != nullptr) {
            *error = "GameFlow control actions cannot have host handlers.";
        }
        return false;
    }
    const auto found = _impl->actions.find(id);
    if (found == _impl->actions.end()) {
        if (error != nullptr) {
            *error = "Action type '" + std::string(id)
                + "' must be registered before its handler.";
        }
        return false;
    }
    if (!handler) {
        if (error != nullptr) *error = "Action handler must be callable.";
        return false;
    }
    found->second.handler = std::move(handler);
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowActionRegistry::setGuardHandler(
    std::string_view id,
    GameFlowGuardHandler handler,
    std::string* error)
{
    const auto found = _impl->guards.find(id);
    if (found == _impl->guards.end()) {
        if (error != nullptr) {
            *error = "Guard type '" + std::string(id)
                + "' must be registered before its handler.";
        }
        return false;
    }
    if (!handler) {
        if (error != nullptr) *error = "Guard handler must be callable.";
        return false;
    }
    found->second.handler = std::move(handler);
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowActionRegistry::clearActionHandler(std::string_view id) noexcept
{
    const auto found = _impl->actions.find(id);
    if (found == _impl->actions.end()) return false;
    found->second.handler = {};
    return true;
}

bool GameFlowActionRegistry::clearGuardHandler(std::string_view id) noexcept
{
    const auto found = _impl->guards.find(id);
    if (found == _impl->guards.end()) return false;
    found->second.handler = {};
    return true;
}

bool GameFlowActionRegistry::registerAction(
    GameFlowActionTypeDefinition definition,
    GameFlowActionHandler handler,
    bool replace,
    std::string* error)
{
    std::string validationError;
    if (definition.id.empty()) {
        validationError = "Action id must not be empty.";
    } else if (!handler) {
        validationError = "Action handler must be callable.";
    } else {
        validateFields(definition.arguments, validationError);
    }
    if (!validationError.empty()) {
        if (error != nullptr) *error = std::move(validationError);
        return false;
    }

    const auto found = _impl->actions.find(definition.id);
    if (found != _impl->actions.end() && !replace) {
        if (error != nullptr) {
            *error = "Action '" + definition.id + "' is already registered.";
        }
        return false;
    }
    const std::string id = definition.id;
    _impl->actions.insert_or_assign(
        id, Impl::ActionEntry{std::move(definition), std::move(handler)});
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowActionRegistry::registerGuard(
    GameFlowGuardTypeDefinition definition,
    GameFlowGuardHandler handler,
    bool replace,
    std::string* error)
{
    std::string validationError;
    if (definition.id.empty()) {
        validationError = "Guard id must not be empty.";
    } else if (!handler) {
        validationError = "Guard handler must be callable.";
    } else {
        validateFields(definition.arguments, validationError);
    }
    if (!validationError.empty()) {
        if (error != nullptr) *error = std::move(validationError);
        return false;
    }

    const auto found = _impl->guards.find(definition.id);
    if (found != _impl->guards.end() && !replace) {
        if (error != nullptr) {
            *error = "Guard '" + definition.id + "' is already registered.";
        }
        return false;
    }
    const std::string id = definition.id;
    _impl->guards.insert_or_assign(
        id, Impl::GuardEntry{std::move(definition), std::move(handler)});
    if (error != nullptr) error->clear();
    return true;
}

bool GameFlowActionRegistry::unregisterAction(std::string_view id)
{
    return _impl->actions.erase(id) != 0u;
}

bool GameFlowActionRegistry::unregisterGuard(std::string_view id)
{
    return _impl->guards.erase(id) != 0u;
}

void GameFlowActionRegistry::clear() noexcept
{
    _impl->actions.clear();
    _impl->guards.clear();
}

const GameFlowActionTypeDefinition* GameFlowActionRegistry::findAction(
    std::string_view id) const noexcept
{
    const auto found = _impl->actions.find(id);
    return found == _impl->actions.end() ? nullptr : &found->second.definition;
}

const GameFlowGuardTypeDefinition* GameFlowActionRegistry::findGuard(
    std::string_view id) const noexcept
{
    const auto found = _impl->guards.find(id);
    return found == _impl->guards.end() ? nullptr : &found->second.definition;
}

const GameFlowActionHandler* GameFlowActionRegistry::findActionHandler(
    std::string_view id) const noexcept
{
    const auto found = _impl->actions.find(id);
    return found == _impl->actions.end() || !found->second.handler
        ? nullptr : &found->second.handler;
}

const GameFlowGuardHandler* GameFlowActionRegistry::findGuardHandler(
    std::string_view id) const noexcept
{
    const auto found = _impl->guards.find(id);
    return found == _impl->guards.end() || !found->second.handler
        ? nullptr : &found->second.handler;
}

std::vector<GameFlowActionTypeDefinition>
GameFlowActionRegistry::actionTypes() const
{
    std::vector<GameFlowActionTypeDefinition> result;
    result.reserve(_impl->actions.size());
    for (const auto& [id, entry] : _impl->actions) {
        (void)id;
        result.push_back(entry.definition);
    }
    return result;
}

std::vector<GameFlowGuardTypeDefinition>
GameFlowActionRegistry::guardTypes() const
{
    std::vector<GameFlowGuardTypeDefinition> result;
    result.reserve(_impl->guards.size());
    for (const auto& [id, entry] : _impl->guards) {
        (void)id;
        result.push_back(entry.definition);
    }
    return result;
}

} // namespace ayt::app
