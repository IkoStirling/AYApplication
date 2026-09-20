#include <AYApplication/GameFlowUIBridge.h>

#include <AYApplication/GameFlowRuntime.h>
#include <AYApplication/UIFlowRuntime.h>

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>

namespace ayt::app
{
namespace
{

using ayt::ui::UIFlowActionDefinition;
using ayt::ui::UIFlowDocument;
using ayt::ui::UIFlowFieldDefinition;
using ayt::ui::UIFlowScope;
using ayt::ui::UIFlowSignalDefinition;
using ayt::ui::UIFlowValue;
using ayt::ui::UIFlowValueType;

bool isNull(const GameFlowValue& value)
{
    return std::holds_alternative<std::monostate>(value.data);
}

bool isNull(const UIFlowValue& value)
{
    return std::holds_alternative<std::monostate>(value.data);
}

GameFlowValue toGameFlowValue(const UIFlowValue& value);
UIFlowValue toUIFlowValue(const GameFlowValue& value);

GameFlowValue::Array toGameFlowArray(const UIFlowValue::Array& values)
{
    GameFlowValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(toGameFlowValue(value));
    }
    return result;
}

GameFlowValue::Object toGameFlowObject(const UIFlowValue::Object& values)
{
    GameFlowValue::Object result;
    for (const auto& [key, value] : values) {
        result.emplace(key, toGameFlowValue(value));
    }
    return result;
}

GameFlowValue toGameFlowValue(const UIFlowValue& value)
{
    return std::visit([](const auto& stored) -> GameFlowValue {
        using T = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<T, UIFlowValue::Array>) {
            return GameFlowValue(toGameFlowArray(stored));
        } else if constexpr (std::is_same_v<T, UIFlowValue::Object>) {
            return GameFlowValue(toGameFlowObject(stored));
        } else {
            return GameFlowValue(stored);
        }
    }, value.data);
}

UIFlowValue::Array toUIFlowArray(const GameFlowValue::Array& values)
{
    UIFlowValue::Array result;
    result.reserve(values.size());
    for (const auto& value : values) {
        result.push_back(toUIFlowValue(value));
    }
    return result;
}

UIFlowValue::Object toUIFlowObject(const GameFlowValue::Object& values)
{
    UIFlowValue::Object result;
    for (const auto& [key, value] : values) {
        result.emplace(key, toUIFlowValue(value));
    }
    return result;
}

UIFlowValue toUIFlowValue(const GameFlowValue& value)
{
    return std::visit([](const auto& stored) -> UIFlowValue {
        using T = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<T, GameFlowValue::Array>) {
            return UIFlowValue(toUIFlowArray(stored));
        } else if constexpr (std::is_same_v<T, GameFlowValue::Object>) {
            return UIFlowValue(toUIFlowObject(stored));
        } else {
            return UIFlowValue(stored);
        }
    }, value.data);
}

const GameFlowFieldDefinition* findField(
    const std::vector<GameFlowFieldDefinition>& fields,
    std::string_view id)
{
    const auto found = std::find_if(fields.begin(), fields.end(),
        [id](const GameFlowFieldDefinition& field) {
            return field.id == id;
        });
    return found == fields.end() ? nullptr : &*found;
}

const UIFlowFieldDefinition* findField(
    const std::vector<UIFlowFieldDefinition>& fields,
    std::string_view id)
{
    const auto found = std::find_if(fields.begin(), fields.end(),
        [id](const UIFlowFieldDefinition& field) {
            return field.id == id;
        });
    return found == fields.end() ? nullptr : &*found;
}

bool uiTypeCanFeedGameType(UIFlowValueType source, GameFlowValueType target)
{
    switch (target) {
    case GameFlowValueType::Boolean:
        return source == UIFlowValueType::Boolean;
    case GameFlowValueType::Integer:
        return source == UIFlowValueType::Integer;
    case GameFlowValueType::Number:
        return source == UIFlowValueType::Integer
            || source == UIFlowValueType::Number;
    case GameFlowValueType::String:
        return source == UIFlowValueType::String
            || source == UIFlowValueType::Entity
            || source == UIFlowValueType::Asset;
    }
    return false;
}

bool gameTypeCanFeedUIType(GameFlowValueType source, UIFlowValueType target)
{
    switch (target) {
    case UIFlowValueType::Boolean:
        return source == GameFlowValueType::Boolean;
    case UIFlowValueType::Integer:
        return source == GameFlowValueType::Integer;
    case UIFlowValueType::Number:
        return source == GameFlowValueType::Integer
            || source == GameFlowValueType::Number;
    case UIFlowValueType::String:
    case UIFlowValueType::Entity:
    case UIFlowValueType::Asset:
        return source == GameFlowValueType::String;
    }
    return false;
}

bool validateSignalToIntentSchema(
    const UIFlowSignalDefinition& signal,
    const GameFlowIntentDefinition& intent,
    std::string& error)
{
    for (const auto& target : intent.payload) {
        const UIFlowFieldDefinition* source = findField(signal.payload, target.id);
        if (source == nullptr) {
            if (target.required && isNull(target.defaultValue)) {
                error = "UI signal '" + signal.id + "' cannot supply required "
                    "GameFlow intent field '" + target.id + "'.";
                return false;
            }
            continue;
        }
        if (!uiTypeCanFeedGameType(source->type, target.type)) {
            error = "UI signal '" + signal.id + "' field '" + target.id
                + "' is incompatible with GameFlow intent '" + intent.id
                + "'.";
            return false;
        }
        if (target.required && isNull(target.defaultValue)
            && !source->required && isNull(source->defaultValue)) {
            error = "UI signal '" + signal.id + "' may omit required "
                "GameFlow intent field '" + target.id + "'.";
            return false;
        }
    }
    return true;
}

bool validateIntentToSignalSchema(
    const GameFlowIntentDefinition& intent,
    const UIFlowSignalDefinition& signal,
    std::string& error)
{
    for (const auto& target : signal.payload) {
        const GameFlowFieldDefinition* source = findField(intent.payload, target.id);
        if (source == nullptr) {
            if (target.required && isNull(target.defaultValue)) {
                error = "GameFlow intent '" + intent.id
                    + "' cannot supply required UI signal field '"
                    + target.id + "'.";
                return false;
            }
            continue;
        }
        if (!gameTypeCanFeedUIType(source->type, target.type)) {
            error = "GameFlow intent '" + intent.id + "' field '" + target.id
                + "' is incompatible with UI signal '" + signal.id + "'.";
            return false;
        }
        if (target.required && isNull(target.defaultValue)
            && !source->required && isNull(source->defaultValue)) {
            error = "GameFlow intent '" + intent.id
                + "' may omit required UI signal field '" + target.id + "'.";
            return false;
        }
    }
    return true;
}

GameFlowPayload filterForIntent(
    const UIFlowPayload& payload,
    const GameFlowIntentDefinition& intent)
{
    GameFlowPayload result;
    for (const auto& field : intent.payload) {
        const auto found = payload.find(field.id);
        if (found != payload.end() && !isNull(found->second)) {
            result.emplace(field.id, toGameFlowValue(found->second));
        }
    }
    return result;
}

UIFlowPayload filterForSignal(
    const GameFlowPayload& payload,
    const UIFlowSignalDefinition& signal)
{
    UIFlowPayload result;
    for (const auto& field : signal.payload) {
        const auto found = payload.find(field.id);
        if (found != payload.end() && !isNull(found->second)) {
            result.emplace(field.id, toUIFlowValue(found->second));
        }
    }
    return result;
}

const std::string* stringArgument(
    const GameFlowActionInvocation& invocation,
    std::string_view id)
{
    if (invocation.arguments == nullptr) return nullptr;
    const auto found = invocation.arguments->find(id);
    if (found == invocation.arguments->end()) return nullptr;
    return std::get_if<std::string>(&found->second.data);
}

bool parseScope(std::string_view value, UIFlowScope& scope)
{
    if (value == "application") {
        scope = UIFlowScope::Application;
    } else if (value == "world") {
        scope = UIFlowScope::World;
    } else if (value == "owner") {
        scope = UIFlowScope::Owner;
    } else if (value == "transient") {
        scope = UIFlowScope::Transient;
    } else {
        return false;
    }
    return true;
}

bool validateRequestAction(const UIFlowDocument& uiDocument,
                           std::string& error)
{
    const UIFlowActionDefinition* action =
        uiDocument.findAction(kUIFlowActionGameFlowRequest);
    if (action == nullptr) {
        error = "UIFlow document must declare Action 'gameflow.request'.";
        return false;
    }
    const UIFlowFieldDefinition* intent = findField(action->inputs, "intent");
    if (intent == nullptr || intent->type != UIFlowValueType::String
        || !intent->required) {
        error = "UIFlow Action 'gameflow.request' must declare required "
            "string input 'intent'.";
        return false;
    }
    return true;
}

bool validateActionReferences(const GameFlowDocument& gameDocument,
                              const UIFlowDocument& uiDocument,
                              std::string& error)
{
    for (const auto& transition : gameDocument.transitions) {
        const GameFlowIntentDefinition* sourceIntent =
            gameDocument.findIntent(transition.triggerIntent);
        for (const auto& action : transition.actions) {
            std::string semanticError;
            if (!validateGameFlowStandardActionSemantics(
                    action, &semanticError)) {
                error = "GameFlow transition '" + transition.id + "': "
                    + semanticError;
                return false;
            }
            if (action.action == kGameFlowActionUIFlowStart) {
                const auto found = action.arguments.find("entry");
                const auto* entry = found == action.arguments.end()
                    ? nullptr : std::get_if<std::string>(&found->second.data);
                if (entry != nullptr && !entry->empty()
                    && uiDocument.findEntry(*entry) == nullptr) {
                    error = "GameFlow transition '" + transition.id
                        + "' references unknown UIFlow entry '" + *entry + "'.";
                    return false;
                }
            } else if (action.action == kGameFlowActionUIContextActivate) {
                const auto context = action.arguments.find("contextId");
                const auto* contextId = context == action.arguments.end()
                    ? nullptr : std::get_if<std::string>(&context->second.data);
                if (contextId == nullptr || contextId->empty()
                    || uiDocument.findContext(*contextId) == nullptr) {
                    error = "GameFlow transition '" + transition.id
                        + "' references an unknown UIFlow Context.";
                    return false;
                }
            } else if (action.action == kGameFlowActionUISignalEmit) {
                const auto signalValue = action.arguments.find("signalId");
                const auto* signalId = signalValue == action.arguments.end()
                    ? nullptr : std::get_if<std::string>(&signalValue->second.data);
                const UIFlowSignalDefinition* signal = signalId == nullptr
                    ? nullptr : uiDocument.findSignal(*signalId);
                if (signal == nullptr) {
                    error = "GameFlow transition '" + transition.id
                        + "' references an unknown UIFlow Signal.";
                    return false;
                }
                if (sourceIntent == nullptr
                    || !validateIntentToSignalSchema(
                        *sourceIntent, *signal, error)) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool validateBridgeContract(
    const GameFlowProgram& gameProgram,
    const GameFlowActionRegistry& registry,
    const UIFlowDocument& uiDocument,
    const GameFlowUIBridgeConfig& config,
    std::string& error)
{
    for (const auto& expected : gameFlowUIActionTypes()) {
        const auto* existing = registry.findAction(expected.id);
        if (existing == nullptr
            || !gameFlowActionTypeCompatible(*existing, expected)) {
            error =
                "GameFlow UI action metadata is missing or incompatible: '"
                + expected.id + "'.";
            return false;
        }
    }

    std::string validationError;
    if (config.enableRequestAction
        && !validateRequestAction(uiDocument, validationError)) {
        error = std::move(validationError);
        return false;
    }
    for (const auto& [flowId, plan] : gameProgram.plans) {
        if (!validateActionReferences(
                plan.document, uiDocument, validationError)) {
            error = "GameFlow '" + flowId + "': "
                + std::move(validationError);
            return false;
        }
    }

    std::set<std::string, std::less<>> mappedPairs;
    for (const auto& binding : config.signalBindings) {
        if (binding.signalId.empty() || binding.intentId.empty()) {
            error =
                "GameFlow UI signal bindings require non-empty ids.";
            return false;
        }
        const std::string pairKey =
            binding.signalId + "\x1f" + binding.intentId;
        if (!mappedPairs.insert(pairKey).second) {
            error = "UI signal-to-intent binding is duplicated: '"
                + binding.signalId + "' -> '" + binding.intentId + "'.";
            return false;
        }
        const UIFlowSignalDefinition* signal =
            uiDocument.findSignal(binding.signalId);
        if (signal == nullptr) {
            error = "Unknown UIFlow Signal in GameFlow binding: '"
                + binding.signalId + "'.";
            return false;
        }
        bool foundIntent = false;
        for (const auto& [flowId, plan] : gameProgram.plans) {
            const GameFlowIntentDefinition* intent =
                plan.document.findIntent(binding.intentId);
            if (intent == nullptr) continue;
            foundIntent = true;
            if (!validateSignalToIntentSchema(
                    *signal, *intent, validationError)) {
                error = "GameFlow '" + flowId + "': "
                    + std::move(validationError);
                return false;
            }
        }
        if (!foundIntent) {
            error = "Unknown GameFlow intent in UI signal binding: '"
                + binding.intentId + "'.";
            return false;
        }
    }

    error.clear();
    return true;
}

} // namespace

class GameFlowUIBridge::Impl
{
public:
    struct State
    {
        GameFlowRuntime* gameFlow = nullptr;
        UIFlowRuntime* uiFlow = nullptr;
        bool installed = false;
        std::map<std::string, UIFlowContextHandle, std::less<>> activations;
        std::string lastError;

        void fail(std::string message)
        {
            lastError = std::move(message);
        }
    };

    struct HandlerBinding
    {
        std::string id;
        GameFlowActionHandler previous;
        bool applied = false;
    };

    Impl(GameFlowRuntime& gameFlowValue,
         UIFlowRuntime& uiFlowValue,
         GameFlowUIBridgeConfig configValue)
        : config(std::move(configValue)), state(std::make_shared<State>())
    {
        state->gameFlow = &gameFlowValue;
        state->uiFlow = &uiFlowValue;
    }

    GameFlowUIBridgeConfig config;
    std::shared_ptr<State> state;
    std::vector<HandlerBinding> handlers;
    std::vector<UIFlowSignalSubscription> subscriptions;
    GameFlowReloadValidatorToken gameReloadValidator = 0;
    UIFlowDocumentValidatorToken uiDocumentValidator = 0;
    bool requestActionRegistered = false;
    bool applicationCommandRegistered = false;

    void restoreHandlers() noexcept
    {
        for (auto value = handlers.rbegin(); value != handlers.rend(); ++value) {
            if (!value->applied) continue;
            if (value->previous) {
                std::string ignored;
                (void)state->gameFlow->bindActionHandler(
                    value->id, std::move(value->previous), &ignored);
            } else {
                (void)state->gameFlow->unbindActionHandler(value->id);
            }
        }
        handlers.clear();
    }

    void rollback() noexcept
    {
        if (gameReloadValidator != 0) {
            (void)state->gameFlow->removeReloadValidator(
                gameReloadValidator);
            gameReloadValidator = 0;
        }
        if (uiDocumentValidator != 0) {
            (void)state->uiFlow->removeDocumentValidator(
                uiDocumentValidator);
            uiDocumentValidator = 0;
        }
        for (const auto subscription : subscriptions) {
            (void)state->uiFlow->unsubscribeSignal(subscription);
        }
        subscriptions.clear();
        if (requestActionRegistered) {
            (void)state->uiFlow->unregisterAction(
                kUIFlowActionGameFlowRequest);
            requestActionRegistered = false;
        }
        if (applicationCommandRegistered) {
            state->uiFlow->unregisterApplicationCommandHandler();
            applicationCommandRegistered = false;
        }
        restoreHandlers();
    }
};

GameFlowUIBridge::GameFlowUIBridge(
    GameFlowRuntime& gameFlow,
    UIFlowRuntime& uiFlow,
    GameFlowUIBridgeConfig config)
    : _impl(std::make_unique<Impl>(
        gameFlow, uiFlow, std::move(config)))
{
}

GameFlowUIBridge::~GameFlowUIBridge()
{
    uninstall();
}

GameFlowUIBridge::GameFlowUIBridge(GameFlowUIBridge&&) noexcept = default;
GameFlowUIBridge& GameFlowUIBridge::operator=(GameFlowUIBridge&& other) noexcept
{
    if (this != &other) {
        uninstall();
        _impl = std::move(other._impl);
    }
    return *this;
}

bool GameFlowUIBridge::install(std::string* error)
{
    if (_impl == nullptr) {
        if (error != nullptr) *error = "GameFlow UI bridge was moved from.";
        return false;
    }
    if (_impl->state->installed) {
        if (error != nullptr) error->clear();
        return true;
    }

    auto fail = [&](std::string message) {
        _impl->state->installed = false;
        _impl->rollback();
        _impl->state->fail(std::move(message));
        if (error != nullptr) *error = _impl->state->lastError;
        return false;
    };

    GameFlowRuntime& gameFlow = *_impl->state->gameFlow;
    UIFlowRuntime& uiFlow = *_impl->state->uiFlow;
    const GameFlowActionRegistry* registry = gameFlow.registry();
    const GameFlowProgram* gameProgram = gameFlow.program();
    const UIFlowDocument* uiDocument = uiFlow.document();
    if (!gameFlow.ready() || registry == nullptr || gameProgram == nullptr) {
        return fail("GameFlow runtime is not ready for the UI bridge.");
    }
    if (!uiFlow.isLoaded() || uiDocument == nullptr) {
        return fail("UIFlow runtime is not loaded for the GameFlow bridge.");
    }

    std::string validationError;
    if (!validateBridgeContract(*gameProgram, *registry, *uiDocument,
            _impl->config, validationError)) {
        return fail(std::move(validationError));
    }

    const std::weak_ptr<Impl::State> weak = _impl->state;
    if (!uiFlow.registerApplicationCommandHandler(
            [weak](std::string_view commandId, UIFlowPayload payload,
                   std::string* commandError) {
                const auto state = weak.lock();
                if (!state || !state->installed) {
                    if (commandError != nullptr) {
                        *commandError = "GameFlow UI bridge is unavailable.";
                    }
                    return false;
                }
                const GameFlowDocument* document =
                    state->gameFlow->activeDocument();
                const GameFlowIntentDefinition* intent = document == nullptr
                    ? nullptr : document->findIntent(commandId);
                if (intent == nullptr) {
                    const std::string message =
                        "Unknown GameFlow application command '"
                        + std::string(commandId) + "'.";
                    state->fail(message);
                    if (commandError != nullptr) *commandError = message;
                    return false;
                }
                const GameFlowRequestResult result = state->gameFlow->request(
                    commandId, filterForIntent(payload, *intent));
                if (!result) {
                    state->fail(result.message);
                    if (commandError != nullptr) *commandError = result.message;
                    return false;
                }
                if (commandError != nullptr) commandError->clear();
                return true;
            })) {
        return fail("UIFlow already has an application command router.");
    }
    _impl->applicationCommandRegistered = true;

    if (_impl->config.enableRequestAction) {
        if (!uiFlow.registerAction(std::string(kUIFlowActionGameFlowRequest),
                [weak](const UIFlowActionInvocation& invocation) {
                    const auto state = weak.lock();
                    if (!state || !state->installed) {
                        return UIFlowActionResult::failure(
                            "GameFlow UI bridge is unavailable.");
                    }
                    const auto intentValue = invocation.inputs.find("intent");
                    const auto* intentId = intentValue == invocation.inputs.end()
                        ? nullptr : std::get_if<std::string>(
                            &intentValue->second.data);
                    if (intentId == nullptr || intentId->empty()) {
                        return UIFlowActionResult::failure(
                            "gameflow.request requires a non-empty intent.");
                    }
                    const GameFlowDocument* document =
                        state->gameFlow->activeDocument();
                    const GameFlowIntentDefinition* intent = document == nullptr
                        ? nullptr : document->findIntent(*intentId);
                    if (intent == nullptr) {
                        return UIFlowActionResult::failure(
                            "Unknown GameFlow intent '" + *intentId + "'.");
                    }

                    UIFlowPayload source;
                    const auto payloadValue = invocation.inputs.find("payload");
                    if (payloadValue != invocation.inputs.end()
                        && !isNull(payloadValue->second)) {
                        const auto* object = std::get_if<UIFlowValue::Object>(
                            &payloadValue->second.data);
                        if (object == nullptr) {
                            return UIFlowActionResult::failure(
                                "gameflow.request payload must be an object.");
                        }
                        source.insert(object->begin(), object->end());
                    } else {
                        for (const auto& [id, value] : invocation.inputs) {
                            if (id != "intent" && id != "payload") {
                                source.emplace(id, value);
                            }
                        }
                    }
                    const GameFlowRequestResult result = state->gameFlow->request(
                        *intentId, filterForIntent(source, *intent));
                    if (!result) {
                        state->fail(result.message);
                        return UIFlowActionResult::failure(result.message);
                    }
                    return UIFlowActionResult::success();
                }, false)) {
            return fail("UIFlow Action 'gameflow.request' already has a handler.");
        }
        _impl->requestActionRegistered = true;
    }

    _impl->handlers.clear();
    for (const auto& definition : gameFlowUIActionTypes()) {
        Impl::HandlerBinding binding;
        binding.id = definition.id;
        if (const auto* previous = registry->findActionHandler(binding.id)) {
            binding.previous = *previous;
        }
        _impl->handlers.push_back(std::move(binding));
    }

    const auto bind = [&](std::string_view id, GameFlowActionHandler handler) {
        std::string bindingError;
        if (!gameFlow.bindActionHandler(id, std::move(handler), &bindingError)) {
            return fail(bindingError.empty()
                ? "Failed to bind GameFlow UI action '" + std::string(id) + "'."
                : std::move(bindingError));
        }
        const auto found = std::find_if(
            _impl->handlers.begin(), _impl->handlers.end(),
            [id](const Impl::HandlerBinding& value) { return value.id == id; });
        if (found != _impl->handlers.end()) found->applied = true;
        return true;
    };

    if (!bind(kGameFlowActionUIFlowStart,
            [weak](const GameFlowActionInvocation& invocation) {
                const auto state = weak.lock();
                if (!state || !state->installed) {
                    return GameFlowActionResult::failed(
                        "GameFlow UI bridge is unavailable.");
                }
                if (state->uiFlow->hasPendingGraphExecution()) {
                    return GameFlowActionResult::failed(
                        "UIFlow cannot restart while a graph is pending.");
                }
                const std::string* entry = stringArgument(invocation, "entry");
                std::string startError;
                if (entry == nullptr
                    || !state->uiFlow->start(*entry, &startError)) {
                    state->fail(startError.empty()
                        ? "ui.flow.start requires a valid entry."
                        : startError);
                    return GameFlowActionResult::failed(state->lastError);
                }
                // start() transactionally rebuilds UIFlow's Context set, so
                // every bridge-owned manual handle from the old run is stale.
                state->activations.clear();
                return GameFlowActionResult::succeeded();
            })) return false;

    if (!bind(kGameFlowActionUIContextActivate,
            [weak](const GameFlowActionInvocation& invocation) {
                const auto state = weak.lock();
                if (!state || !state->installed) {
                    return GameFlowActionResult::failed(
                        "GameFlow UI bridge is unavailable.");
                }
                const std::string* activationId =
                    stringArgument(invocation, "activationId");
                const std::string* contextId =
                    stringArgument(invocation, "contextId");
                const std::string* scopeName =
                    stringArgument(invocation, "scope");
                const std::string* scopeKey =
                    stringArgument(invocation, "scopeKey");
                if (activationId == nullptr || activationId->empty()
                    || contextId == nullptr || contextId->empty()
                    || scopeName == nullptr || scopeKey == nullptr) {
                    return GameFlowActionResult::failed(
                        "ui.context.activate has invalid arguments.");
                }
                if (state->activations.contains(*activationId)) {
                    return GameFlowActionResult::failed(
                        "UI Context activation id '" + *activationId
                            + "' is already active.");
                }
                UIFlowScope scope = UIFlowScope::Application;
                if (!parseScope(*scopeName, scope)) {
                    return GameFlowActionResult::failed(
                        "Unknown UI Context scope '" + *scopeName + "'.");
                }
                if (scope == UIFlowScope::Application
                    && !scopeKey->empty() && *scopeKey != "application") {
                    return GameFlowActionResult::failed(
                        "Application UI Context scope key must be 'application'.");
                }
                UIFlowContextActivationOptions options;
                options.lifetime.scope = scope;
                options.lifetime.key = *scopeKey;
                std::string activationError;
                const UIFlowContextHandle handle = state->uiFlow->activateContext(
                    *contextId, std::move(options), &activationError);
                if (handle == 0) {
                    state->fail(std::move(activationError));
                    return GameFlowActionResult::failed(state->lastError);
                }
                state->activations.emplace(*activationId, handle);
                return GameFlowActionResult::succeeded();
            })) return false;

    if (!bind(kGameFlowActionUIContextDeactivate,
            [weak](const GameFlowActionInvocation& invocation) {
                const auto state = weak.lock();
                if (!state || !state->installed) {
                    return GameFlowActionResult::failed(
                        "GameFlow UI bridge is unavailable.");
                }
                const std::string* activationId =
                    stringArgument(invocation, "activationId");
                const auto found = activationId == nullptr
                    ? state->activations.end()
                    : state->activations.find(*activationId);
                if (found == state->activations.end()) {
                    return GameFlowActionResult::failed(
                        "Unknown UI Context activation id.");
                }
                std::string deactivateError;
                if (!state->uiFlow->deactivateContext(
                        found->second, &deactivateError)) {
                    if (deactivateError == "Unknown manual Context handle.") {
                        // The owning World/Owner scope may have retired the
                        // Context before GameFlow reaches its cleanup action.
                        state->activations.erase(found);
                        return GameFlowActionResult::succeeded();
                    }
                    state->fail(std::move(deactivateError));
                    return GameFlowActionResult::failed(state->lastError);
                }
                state->activations.erase(found);
                return GameFlowActionResult::succeeded();
            })) return false;

    if (!bind(kGameFlowActionUISignalEmit,
            [weak](const GameFlowActionInvocation& invocation) {
                const auto state = weak.lock();
                if (!state || !state->installed) {
                    return GameFlowActionResult::failed(
                        "GameFlow UI bridge is unavailable.");
                }
                const std::string* signalId =
                    stringArgument(invocation, "signalId");
                const UIFlowDocument* document = state->uiFlow->document();
                const UIFlowSignalDefinition* signal =
                    signalId == nullptr || document == nullptr
                    ? nullptr : document->findSignal(*signalId);
                if (signal == nullptr || invocation.intentPayload == nullptr) {
                    return GameFlowActionResult::failed(
                        "ui.signal.emit requires a declared signal and payload.");
                }
                std::string signalError;
                if (!state->uiFlow->emitSignal(*signalId,
                        filterForSignal(*invocation.intentPayload, *signal),
                        &signalError)) {
                    state->fail(std::move(signalError));
                    return GameFlowActionResult::failed(state->lastError);
                }
                return GameFlowActionResult::succeeded();
            })) return false;

    for (const auto& mapping : _impl->config.signalBindings) {
        const UIFlowSignalSubscription subscription = uiFlow.subscribeSignal(
            mapping.signalId,
            [weak, intentId = mapping.intentId](
                std::string_view,
                const UIFlowPayload& payload) {
                const auto state = weak.lock();
                if (!state || !state->installed) return;
                const GameFlowDocument* document =
                    state->gameFlow->activeDocument();
                const GameFlowIntentDefinition* target = document == nullptr
                    ? nullptr : document->findIntent(intentId);
                if (target == nullptr) {
                    state->fail("Mapped GameFlow intent disappeared: '"
                        + intentId + "'.");
                    return;
                }
                const GameFlowRequestResult result = state->gameFlow->request(
                    intentId, filterForIntent(payload, *target));
                if (!result) state->fail(result.message);
            });
        if (subscription == 0) {
            return fail("Failed to subscribe to UIFlow Signal '"
                + mapping.signalId + "'.");
        }
        _impl->subscriptions.push_back(subscription);
    }

    _impl->gameReloadValidator = gameFlow.addReloadValidator(
        [weak, config = _impl->config](
            const GameFlowProgram& candidate,
            const GameFlowActionRegistry& candidateRegistry,
            std::string& error) {
            const auto state = weak.lock();
            if (!state || !state->installed) {
                error.clear();
                return true;
            }
            const UIFlowDocument* uiDocument = state->uiFlow->document();
            std::string validationError;
            if (uiDocument == nullptr) {
                validationError =
                    "UIFlow runtime is not loaded for the GameFlow bridge.";
            } else if (validateBridgeContract(candidate, candidateRegistry,
                           *uiDocument, config, validationError)) {
                error.clear();
                return true;
            }
            state->fail(
                "GameFlow UI bridge rejected GameFlow reload: "
                + std::move(validationError));
            error = state->lastError;
            return false;
        });
    if (_impl->gameReloadValidator == 0) {
        return fail("Failed to register the GameFlow reload validator.");
    }

    _impl->uiDocumentValidator = uiFlow.addDocumentValidator(
        [weak, config = _impl->config](
            const UIFlowDocument& candidate,
            std::string& error) {
            const auto state = weak.lock();
            if (!state || !state->installed) {
                error.clear();
                return true;
            }
            const GameFlowProgram* gameProgram = state->gameFlow->program();
            const GameFlowActionRegistry* registry =
                state->gameFlow->registry();
            std::string validationError;
            if (!state->gameFlow->ready()
                || gameProgram == nullptr || registry == nullptr) {
                validationError =
                    "GameFlow runtime is not ready for the UI bridge.";
            } else if (validateBridgeContract(*gameProgram, *registry,
                           candidate, config, validationError)) {
                error.clear();
                return true;
            }
            state->fail(
                "GameFlow UI bridge rejected UIFlow reload: "
                + std::move(validationError));
            error = state->lastError;
            return false;
        });
    if (_impl->uiDocumentValidator == 0) {
        return fail("Failed to register the UIFlow document validator.");
    }

    _impl->state->installed = true;
    _impl->state->lastError.clear();
    if (error != nullptr) error->clear();
    return true;
}

void GameFlowUIBridge::uninstall() noexcept
{
    if (_impl == nullptr) return;
    _impl->state->installed = false;
    if (_impl->gameReloadValidator != 0) {
        (void)_impl->state->gameFlow->removeReloadValidator(
            _impl->gameReloadValidator);
        _impl->gameReloadValidator = 0;
    }
    if (_impl->uiDocumentValidator != 0) {
        (void)_impl->state->uiFlow->removeDocumentValidator(
            _impl->uiDocumentValidator);
        _impl->uiDocumentValidator = 0;
    }
    for (const auto subscription : _impl->subscriptions) {
        (void)_impl->state->uiFlow->unsubscribeSignal(subscription);
    }
    _impl->subscriptions.clear();
    if (_impl->requestActionRegistered) {
        (void)_impl->state->uiFlow->unregisterAction(
            kUIFlowActionGameFlowRequest);
        _impl->requestActionRegistered = false;
    }
    if (_impl->applicationCommandRegistered) {
        _impl->state->uiFlow->unregisterApplicationCommandHandler();
        _impl->applicationCommandRegistered = false;
    }
    for (const auto& [id, handle] : _impl->state->activations) {
        (void)id;
        std::string ignored;
        try {
            (void)_impl->state->uiFlow->deactivateContext(handle, &ignored);
        } catch (...) {
        }
    }
    _impl->state->activations.clear();
    _impl->restoreHandlers();
}

bool GameFlowUIBridge::installed() const noexcept
{
    return _impl != nullptr && _impl->state->installed;
}

std::string_view GameFlowUIBridge::lastError() const noexcept
{
    return _impl == nullptr
        ? std::string_view{} : std::string_view(_impl->state->lastError);
}

} // namespace ayt::app
