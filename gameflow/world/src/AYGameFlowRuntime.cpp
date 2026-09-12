#include <AYApplication/GameFlowRuntime.h>

#include <AYApplication/GameFlowWorldActions.h>
#include <AYApplication/IEngineHost.h>

#include <cmath>
#include <exception>
#include <fstream>
#include <iterator>
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

} // namespace

class GameFlowRuntimePreparation::Impl
{
public:
    std::string documentPath;
    std::string startupIntent;
    bool enableWorldActions = true;
    GameFlowActionRegistry registry;
    GameFlowPlan plan;
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
    return _impl->plan.document;
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
    if (!buildGameFlowPlan(
            document,
            prepared->_impl->registry,
            prepared->_impl->plan,
            &prepared->_impl->diagnostics)) {
        return fail(diagnosticMessage(
            prepared->_impl->diagnostics,
            "GameFlow document could not be normalized."));
    }
    if (!prepared->_impl->startupIntent.empty()
        && prepared->_impl->plan.document.findIntent(
            prepared->_impl->startupIntent) == nullptr) {
        return fail("Startup GameFlow intent '"
            + prepared->_impl->startupIntent
            + "' is not declared by the document.");
    }

    if (error != nullptr) error->clear();
    return prepared;
}

class GameFlowRuntime::Impl
{
public:
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
    GameFlowCoordinator coordinator;
    std::unique_ptr<GameFlowWorldActionAdapter> worldAdapter;
    std::vector<GameFlowDiagnostic> failureDiagnostics;
    std::string lastError;
    bool enableWorldActions = true;
    bool ready = false;
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
    if (!_impl->coordinator.setPlan(
            &prepared.plan,
            &prepared.registry,
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
}

void GameFlowRuntime::fixedUpdate(float)
{
}

void GameFlowRuntime::shutdown()
{
    if (!_impl) return;
    _impl->coordinator.reset();
    _impl->worldAdapter.reset();
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
    return _impl->preparation->_impl->registry.setActionHandler(
        actionId, std::move(handler), error);
}

bool GameFlowRuntime::unbindActionHandler(std::string_view actionId) noexcept
{
    return _impl->preparation
        && _impl->preparation->_impl->registry.clearActionHandler(actionId);
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
