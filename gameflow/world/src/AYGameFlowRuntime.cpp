#include <AYApplication/GameFlowRuntime.h>

#include <AYApplication/GameFlowWorldActions.h>
#include <AYApplication/IEngineHost.h>

#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
#include <map>
#include <utility>

namespace ayt::app
{
namespace
{

std::string diagnosticMessage(
    const std::vector<GameFlowDiagnostic>& diagnostics,
    std::string fallback)
{
    if (diagnostics.empty()) return fallback;
    const auto& first = diagnostics.front();
    if (first.path.empty()) return first.message;
    return first.path + ": " + first.message;
}

const ayt::game::SubSystemDescriptor& descriptor(bool worldActions)
{
    static const ayt::game::SubSystemDescriptor withWorld{
        .name = "GameFlowRuntime",
        .basePriority = 100,
        .timeType = ayt::game::SubSystemDescriptor::TimeType::Real,
        .phases = ayt::game::phaseBit(ayt::game::FramePhase::Ingress),
        .clock = ayt::game::ClockDomain::RealWall,
        .initializeAfter = {"RuntimeSceneLoader", "GameWorldRouter"},
        .phasePriority = 100,
        .reads = {"Application.GameFlowIntent"},
        .writes = {"Application.GameFlowState"},
    };
    static const ayt::game::SubSystemDescriptor standalone{
        .name = "GameFlowRuntime",
        .basePriority = 100,
        .timeType = ayt::game::SubSystemDescriptor::TimeType::Real,
        .phases = ayt::game::phaseBit(ayt::game::FramePhase::Ingress),
        .clock = ayt::game::ClockDomain::RealWall,
        .phasePriority = 100,
        .reads = {"Application.GameFlowIntent"},
        .writes = {"Application.GameFlowState"},
    };
    return worldActions ? withWorld : standalone;
}

bool compatibleActionDefinition(
    const GameFlowActionTypeDefinition& left,
    const GameFlowActionTypeDefinition& right)
{
    if (left.id != right.id
        || left.asynchronous != right.asynchronous
        || left.arguments.size() != right.arguments.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.arguments.size(); ++index) {
        const auto& leftField = left.arguments[index];
        const auto& rightField = right.arguments[index];
        if (leftField.id != rightField.id
            || leftField.type != rightField.type
            || leftField.required != rightField.required
            || leftField.defaultValue != rightField.defaultValue) {
            return false;
        }
    }
    return true;
}

} // namespace

class GameFlowRuntimePreparation::Impl
{
public:
    std::string documentPath;
    std::string startupIntent;
    bool enableWorldActions = true;
    GameFlowActionRegistry registry;
    GameFlowProgram program;
    GameFlowPayload rootParameters;
    std::vector<GameFlowDiagnostic> diagnostics;
};

GameFlowRuntimePreparation::GameFlowRuntimePreparation()
    : _impl(std::make_unique<Impl>())
{
}

GameFlowRuntimePreparation::~GameFlowRuntimePreparation() = default;
GameFlowRuntimePreparation::GameFlowRuntimePreparation(
    GameFlowRuntimePreparation&&) noexcept = default;
GameFlowRuntimePreparation& GameFlowRuntimePreparation::operator=(
    GameFlowRuntimePreparation&&) noexcept = default;

std::string_view GameFlowRuntimePreparation::documentPath() const noexcept
{
    return _impl->documentPath;
}

std::string_view GameFlowRuntimePreparation::startupIntent() const noexcept
{
    return _impl->startupIntent;
}

bool GameFlowRuntimePreparation::worldActionsEnabled() const noexcept
{
    return _impl->enableWorldActions;
}

const GameFlowDocument& GameFlowRuntimePreparation::document() const noexcept
{
    return _impl->program.findPlan(_impl->program.rootFlowId)->document;
}

const std::vector<GameFlowDiagnostic>&
GameFlowRuntimePreparation::diagnostics() const noexcept
{
    return _impl->diagnostics;
}

std::unique_ptr<GameFlowRuntimePreparation> prepareGameFlowRuntime(
    GameFlowRuntimeConfig config,
    std::string* error)
{
    auto prepared = std::make_unique<GameFlowRuntimePreparation>();
    prepared->_impl->documentPath = std::move(config.documentPath);
    prepared->_impl->startupIntent = std::move(config.startupIntent);
    prepared->_impl->enableWorldActions = config.enableWorldActions;
    prepared->_impl->rootParameters = std::move(config.rootParameters);

    auto fail = [&](std::string message)
        -> std::unique_ptr<GameFlowRuntimePreparation> {
        if (error != nullptr) *error = std::move(message);
        return nullptr;
    };

    if (prepared->_impl->documentPath.empty()) {
        return fail("GameFlow document path is empty.");
    }
    if (prepared->_impl->enableWorldActions
        && !registerGameFlowWorldActionType(
            prepared->_impl->registry, error)) {
        return nullptr;
    }

    if (config.configureRegistry) {
        try {
            std::string callbackError;
            if (!config.configureRegistry(
                    prepared->_impl->registry, callbackError)) {
                return fail(callbackError.empty()
                    ? "GameFlow registry configuration failed."
                    : std::move(callbackError));
            }
        } catch (const std::exception& exception) {
            return fail(std::string(
                "GameFlow registry configuration threw: ")
                + exception.what());
        } catch (...) {
            return fail(
                "GameFlow registry configuration threw an unknown exception.");
        }
    }

    std::ifstream input(prepared->_impl->documentPath, std::ios::binary);
    if (!input) {
        return fail("Cannot open GameFlow document: "
            + prepared->_impl->documentPath);
    }
    const std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    GameFlowDocument document;
    if (!GameFlowSerializer::deserialize(
            text, document, &prepared->_impl->diagnostics)) {
        return fail(diagnosticMessage(
            prepared->_impl->diagnostics,
            "GameFlow document could not be parsed."));
    }
    if (!buildGameFlowProgram(
            document,
            prepared->_impl->registry,
            std::move(config.resolveDocument),
            prepared->_impl->program,
            &prepared->_impl->diagnostics,
            config.programOptions)) {
        return fail(diagnosticMessage(
            prepared->_impl->diagnostics,
            "GameFlow program could not be normalized."));
    }
    const GameFlowPlan* root = prepared->_impl->program.findPlan(
        prepared->_impl->program.rootFlowId);
    if (!prepared->_impl->startupIntent.empty()
        && root->document.findIntent(
            prepared->_impl->startupIntent) == nullptr) {
        return fail("Startup GameFlow intent '"
            + prepared->_impl->startupIntent
            + "' is not declared by the document.");
    }
    GameFlowCoordinator preflight;
    std::string parameterError;
    if (!preflight.setProgram(&prepared->_impl->program,
            &prepared->_impl->registry,
            prepared->_impl->rootParameters,
            &parameterError)) {
        return fail(parameterError.empty()
            ? "GameFlow root parameters are invalid."
            : std::move(parameterError));
    }

    if (error != nullptr) error->clear();
    return prepared;
}

class GameFlowRuntime::Impl
{
public:
    struct BoundActionHandler
    {
        GameFlowActionTypeDefinition definition;
        GameFlowActionHandler handler;
    };

    Impl(IEngineHost& valueHost, GameFlowRuntimeConfig valueConfig)
        : host(valueHost),
          pendingConfig(std::move(valueConfig)),
          enableWorldActions(pendingConfig.enableWorldActions)
    {
    }

    Impl(
        IEngineHost& valueHost,
        std::unique_ptr<GameFlowRuntimePreparation> valuePreparation)
        : host(valueHost),
          preparation(std::move(valuePreparation)),
          enableWorldActions(
              preparation && preparation->worldActionsEnabled())
    {
    }

    IEngineHost& host;
    GameFlowRuntimeConfig pendingConfig;
    std::unique_ptr<GameFlowRuntimePreparation> preparation;
    std::unique_ptr<GameFlowRuntimePreparation> pendingReload;
    GameFlowCoordinator coordinator;
    std::unique_ptr<GameFlowWorldActionAdapter> worldAdapter;
    std::map<std::string, BoundActionHandler, std::less<>> boundHandlers;
    std::vector<GameFlowDiagnostic> failureDiagnostics;
    std::string lastError;
    std::string lastReloadError;
    bool enableWorldActions = true;
    bool ready = false;

    GameFlowReloadResult applyReload(
        std::unique_ptr<GameFlowRuntimePreparation> candidate)
    {
        if (!candidate) {
            return {GameFlowReloadState::Rejected,
                "GameFlow reload candidate is unavailable."};
        }
        auto& next = *candidate->_impl;
        if (next.enableWorldActions != enableWorldActions) {
            return {GameFlowReloadState::Rejected,
                "GameFlow reload cannot change World action assembly."};
        }

        std::string error;
        for (const auto& [actionId, binding] : boundHandlers) {
            const auto* definition = next.registry.findAction(actionId);
            if (definition == nullptr
                || !compatibleActionDefinition(
                    *definition, binding.definition)) {
                return {GameFlowReloadState::Rejected,
                    "GameFlow reload changed the bound action contract for '"
                        + actionId + "'."};
            }
            if (!next.registry.setActionHandler(
                    actionId, binding.handler, &error)) {
                return {GameFlowReloadState::Rejected,
                    "GameFlow reload could not restore action handler '"
                        + actionId + "': " + error};
            }
        }

        std::unique_ptr<GameFlowWorldActionAdapter> nextWorldAdapter;
        if (enableWorldActions) {
            nextWorldAdapter = createGameFlowWorldActionAdapter(
                host, next.registry, coordinator, &error);
            if (!nextWorldAdapter) {
                return {GameFlowReloadState::Rejected,
                    error.empty()
                        ? "GameFlow reload could not bind World actions."
                        : std::move(error)};
            }
        }

        if (!coordinator.replaceProgram(&next.program, &next.registry,
                next.rootParameters, &error)) {
            return {GameFlowReloadState::Rejected,
                error.empty()
                    ? "GameFlow reload could not replace the active program."
                    : std::move(error)};
        }

        worldAdapter = std::move(nextWorldAdapter);
        preparation = std::move(candidate);
        return {GameFlowReloadState::Applied, {}};
    }
};

GameFlowRuntime::GameFlowRuntime(
    IEngineHost& host,
    GameFlowRuntimeConfig config)
    : _impl(std::make_unique<Impl>(host, std::move(config)))
{
}

GameFlowRuntime::GameFlowRuntime(
    IEngineHost& host,
    std::unique_ptr<GameFlowRuntimePreparation> preparation)
    : _impl(std::make_unique<Impl>(host, std::move(preparation)))
{
}

GameFlowRuntime::~GameFlowRuntime()
{
    shutdown();
    try {
        if (_impl->host.findService(kHostServiceGameFlowRuntime) == this) {
            _impl->host.provideService(kHostServiceGameFlowRuntime, nullptr);
        }
    } catch (...) {
    }
}

const char* GameFlowRuntime::getName() const
{
    return kGameFlowRuntimeSubSystemName.data();
}

const ayt::game::SubSystemDescriptor& GameFlowRuntime::getDescriptor() const
{
    return descriptor(_impl->enableWorldActions);
}

bool GameFlowRuntime::initialize()
{
    if (_impl->ready) return true;
    _impl->lastError.clear();
    _impl->failureDiagnostics.clear();

    if (!_impl->preparation) {
        _impl->preparation = prepareGameFlowRuntime(
            std::move(_impl->pendingConfig), &_impl->lastError);
        if (!_impl->preparation) return false;
    }
    auto& prepared = *_impl->preparation->_impl;

    if (_impl->enableWorldActions) {
        _impl->worldAdapter = createGameFlowWorldActionAdapter(
            _impl->host,
            prepared.registry,
            _impl->coordinator,
            &_impl->lastError);
        if (!_impl->worldAdapter) return false;
    }
    if (!_impl->coordinator.setProgram(
            &prepared.program,
            &prepared.registry,
            prepared.rootParameters,
            &_impl->lastError)) {
        _impl->worldAdapter.reset();
        return false;
    }
    if (!prepared.startupIntent.empty()) {
        const GameFlowRequestResult request = _impl->coordinator.request(
            prepared.startupIntent);
        if (!request) {
            _impl->lastError = "Startup GameFlow intent '"
                + prepared.startupIntent + "' was rejected: "
                + request.message;
            _impl->coordinator.reset();
            _impl->worldAdapter.reset();
            return false;
        }
    }

    _impl->ready = true;
    return true;
}

void GameFlowRuntime::update(float deltaTime)
{
    if (!_impl->ready) return;
    const double seconds = std::isfinite(deltaTime) && deltaTime > 0.0f
        ? static_cast<double>(deltaTime) : 0.0;
    _impl->coordinator.update(seconds);
    if (_impl->pendingReload && _impl->coordinator.reloadSafePoint()) {
        GameFlowReloadResult result = _impl->applyReload(
            std::move(_impl->pendingReload));
        if (!result) _impl->lastReloadError = std::move(result.message);
    }
}

void GameFlowRuntime::fixedUpdate(float)
{
}

void GameFlowRuntime::shutdown()
{
    if (!_impl) return;
    _impl->coordinator.reset();
    _impl->worldAdapter.reset();
    _impl->pendingReload.reset();
    _impl->ready = false;
}

GameFlowRequestResult GameFlowRuntime::request(
    std::string_view intent,
    GameFlowPayload payload)
{
    if (!_impl->ready) {
        return {GameFlowRequestState::NotReady,
            _impl->lastError.empty()
                ? "GameFlow runtime is not ready."
                : _impl->lastError};
    }
    return _impl->coordinator.request(intent, std::move(payload));
}

bool GameFlowRuntime::ready() const noexcept
{
    return _impl->ready;
}

std::string_view GameFlowRuntime::currentState() const noexcept
{
    return _impl->coordinator.currentState();
}

std::string_view GameFlowRuntime::lastError() const noexcept
{
    return _impl->lastError;
}

std::string_view GameFlowRuntime::documentPath() const noexcept
{
    if (_impl->preparation) return _impl->preparation->documentPath();
    return _impl->pendingConfig.documentPath;
}

const std::vector<GameFlowDiagnostic>& GameFlowRuntime::diagnostics()
    const noexcept
{
    return _impl->preparation
        ? _impl->preparation->diagnostics()
        : _impl->failureDiagnostics;
}

const GameFlowDocument* GameFlowRuntime::document() const noexcept
{
    return _impl->preparation
        ? &_impl->preparation->document()
        : nullptr;
}

GameFlowCoordinator& GameFlowRuntime::coordinator() noexcept
{
    return _impl->coordinator;
}

const GameFlowCoordinator& GameFlowRuntime::coordinator() const noexcept
{
    return _impl->coordinator;
}

const GameFlowActionRegistry* GameFlowRuntime::registry() const noexcept
{
    return _impl->preparation
        ? &_impl->preparation->_impl->registry
        : nullptr;
}

bool GameFlowRuntime::bindActionHandler(
    std::string_view actionId,
    GameFlowActionHandler handler,
    std::string* error)
{
    if (!_impl->preparation) {
        if (error != nullptr) {
            *error = "GameFlow runtime has not completed preflight.";
        }
        return false;
    }
    if (_impl->enableWorldActions
        && actionId == kGameFlowActionWorldReplace) {
        if (error != nullptr) {
            *error = "world.replace is owned by the GameFlow World adapter.";
        }
        return false;
    }
    const auto* definition =
        _impl->preparation->_impl->registry.findAction(actionId);
    if (definition == nullptr) {
        if (error != nullptr) {
            *error = "GameFlow action type '" + std::string(actionId)
                + "' is not registered.";
        }
        return false;
    }
    if (!_impl->preparation->_impl->registry.setActionHandler(
            actionId, handler, error)) {
        return false;
    }
    _impl->boundHandlers[std::string(actionId)] = {
        *definition, std::move(handler)};
    return true;
}

bool GameFlowRuntime::unbindActionHandler(std::string_view actionId) noexcept
{
    if (_impl->enableWorldActions
        && actionId == kGameFlowActionWorldReplace) return false;
    if (!_impl->preparation
        || !_impl->preparation->_impl->registry.clearActionHandler(actionId)) {
        return false;
    }
    _impl->boundHandlers.erase(std::string(actionId));
    return true;
}

GameFlowReloadResult GameFlowRuntime::reload(GameFlowRuntimeConfig config)
{
    if (!_impl->ready) {
        return {GameFlowReloadState::Rejected,
            "GameFlow runtime is not ready."};
    }
    // A newer authoring request supersedes an older deferred candidate even
    // when the newer document turns out to be invalid.
    _impl->pendingReload.reset();
    std::string error;
    auto candidate = prepareGameFlowRuntime(std::move(config), &error);
    if (!candidate) {
        _impl->lastReloadError = error.empty()
            ? "GameFlow reload preflight failed." : std::move(error);
        return {GameFlowReloadState::Rejected, _impl->lastReloadError};
    }
    if (candidate->worldActionsEnabled() != _impl->enableWorldActions) {
        _impl->lastReloadError =
            "GameFlow reload cannot change World action assembly.";
        return {GameFlowReloadState::Rejected, _impl->lastReloadError};
    }
    _impl->lastReloadError.clear();
    if (!_impl->coordinator.reloadSafePoint()) {
        _impl->pendingReload = std::move(candidate);
        return {GameFlowReloadState::Deferred,
            "GameFlow reload is waiting for an idle root safe point."};
    }
    GameFlowReloadResult result = _impl->applyReload(std::move(candidate));
    if (!result) _impl->lastReloadError = result.message;
    return result;
}

bool GameFlowRuntime::reloadPending() const noexcept
{
    return _impl->pendingReload != nullptr;
}

std::string_view GameFlowRuntime::lastReloadError() const noexcept
{
    return _impl->lastReloadError;
}

GameFlowRuntime* gameFlowRuntime(IEngineHost& host) noexcept
{
    try {
        return host.service<GameFlowRuntime>(kHostServiceGameFlowRuntime);
    } catch (...) {
        return nullptr;
    }
}

} // namespace ayt::app
