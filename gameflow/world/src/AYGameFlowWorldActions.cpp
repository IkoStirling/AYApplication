#include <AYApplication/GameFlowWorldActions.h>

#include <AYApplication/GameFlowActionRegistry.h>
#include <AYApplication/GameFlowCoordinator.h>
#include <AYApplication/GameProject.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RuntimeSceneLoader.h>
#include <AYEventSystem/EventBus.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace ayt::app
{
namespace
{

std::string failureMessage(const IGameWorldRouter& router,
                           const IRuntimeSceneLoader& loader)
{
    const RuntimeSceneLoadStatus status = loader.getLoadStatus();
    if (!status.message.empty()) return status.message;
    if (!router.lastError().empty()) return std::string(router.lastError());
    return "World replacement failed.";
}

} // namespace

class GameFlowWorldActionAdapter::Impl
{
public:
    struct Pending
    {
        GameFlowActionExecutionId actionExecutionId = 0;
        std::uint64_t sceneRequestId = 0;
    };

    struct State
    {
        GameFlowCoordinator* coordinator = nullptr;
        IGameWorldRouter* router = nullptr;
        IRuntimeSceneLoader* loader = nullptr;
        bool installed = false;
        std::optional<Pending> pending;

        void cancel(GameFlowActionExecutionId executionId) noexcept
        {
            if (!pending.has_value()
                || pending->actionExecutionId != executionId) return;
            const std::uint64_t requestId = pending->sceneRequestId;
            pending.reset();
            (void)loader->cancelLoad(requestId);
        }

        void finish(const RuntimeSceneLoadFinishedEvent& event)
        {
            if (!installed || !pending.has_value()
                || pending->sceneRequestId != event.requestId) return;
            const GameFlowActionExecutionId actionId =
                pending->actionExecutionId;
            pending.reset();
            GameFlowActionResult result = event.success
                ? GameFlowActionResult::succeeded()
                : GameFlowActionResult::failed(
                    failureMessage(*router, *loader));
            std::string ignored;
            (void)coordinator->completeAction(
                actionId, std::move(result), &ignored);
        }
    };

    Impl(GameFlowActionRegistry& valueRegistry,
         GameFlowCoordinator& valueCoordinator,
         IGameWorldRouter& valueRouter,
         IRuntimeSceneLoader& valueLoader,
         ayt::event::EventBus& valueEventBus)
        : registry(valueRegistry),
          eventBus(valueEventBus),
          state(std::make_shared<State>())
    {
        state->coordinator = &valueCoordinator;
        state->router = &valueRouter;
        state->loader = &valueLoader;
    }

    GameFlowActionRegistry& registry;
    ayt::event::EventBus& eventBus;
    std::shared_ptr<State> state;
    ayt::event::ConnectionId connection = ayt::event::kInvalidConnectionId;
    GameFlowActionHandler previousHandler;
};

GameFlowWorldActionAdapter::GameFlowWorldActionAdapter(
    GameFlowActionRegistry& registry,
    GameFlowCoordinator& coordinator,
    IGameWorldRouter& router,
    IRuntimeSceneLoader& loader,
    ayt::event::EventBus& eventBus)
    : _impl(std::make_unique<Impl>(
        registry, coordinator, router, loader, eventBus))
{
}

GameFlowWorldActionAdapter::~GameFlowWorldActionAdapter()
{
    uninstall();
}

GameFlowWorldActionAdapter::GameFlowWorldActionAdapter(
    GameFlowWorldActionAdapter&&) noexcept = default;
GameFlowWorldActionAdapter& GameFlowWorldActionAdapter::operator=(
    GameFlowWorldActionAdapter&& other) noexcept
{
    if (this != &other) {
        uninstall();
        _impl = std::move(other._impl);
    }
    return *this;
}

bool GameFlowWorldActionAdapter::install(std::string* error)
{
    if (_impl == nullptr) {
        if (error != nullptr) *error = "World action adapter was moved from.";
        return false;
    }
    if (_impl->state->installed) {
        if (error != nullptr) error->clear();
        return true;
    }

    if (!registerGameFlowWorldActionType(_impl->registry, error)) {
        return false;
    }

    if (const auto* handler = _impl->registry.findActionHandler(
            kGameFlowActionWorldReplace)) {
        _impl->previousHandler = *handler;
    }

    const std::weak_ptr<Impl::State> weak = _impl->state;
    _impl->state->installed = true;
    try {
        _impl->connection =
            _impl->eventBus.subscribe<RuntimeSceneLoadFinishedEvent>(
                [weak](const RuntimeSceneLoadFinishedEvent& event) {
                    if (const auto state = weak.lock()) state->finish(event);
                });
    } catch (...) {
        _impl->state->installed = false;
        if (error != nullptr) {
            *error = "Failed to subscribe to RuntimeSceneLoadFinishedEvent.";
        }
        return false;
    }

    const bool handlerInstalled = _impl->registry.setActionHandler(
        kGameFlowActionWorldReplace,
        [weak](const GameFlowActionInvocation& invocation) {
            const auto state = weak.lock();
            if (!state || !state->installed) {
                return GameFlowActionResult::failed(
                    "World action adapter is unavailable.");
            }
            if (state->pending.has_value()) {
                return GameFlowActionResult::failed(
                    "A GameFlow World replacement is already pending.");
            }
            if (invocation.arguments == nullptr) {
                return GameFlowActionResult::failed(
                    "world.replace arguments are unavailable.");
            }
            const auto argument = invocation.arguments->find("worldId");
            if (argument == invocation.arguments->end()) {
                return GameFlowActionResult::failed(
                    "world.replace requires worldId.");
            }
            const auto* worldId = std::get_if<std::string>(
                &argument->second.data);
            if (worldId == nullptr || worldId->empty()) {
                return GameFlowActionResult::failed(
                    "world.replace worldId must be a non-empty string.");
            }
            if (state->router->findWorld(*worldId) == nullptr) {
                return GameFlowActionResult::failed(
                    "Unknown World id: " + *worldId);
            }
            if (state->router->currentWorldId() == *worldId) {
                return GameFlowActionResult::succeeded();
            }
            if (!state->router->requestWorld(*worldId)) {
                const std::string message = state->router->lastError().empty()
                    ? "GameWorldRouter rejected the World replacement."
                    : std::string(state->router->lastError());
                return GameFlowActionResult::failed(message);
            }
            if (state->router->pendingWorldId().empty()) {
                return state->router->currentWorldId() == *worldId
                    ? GameFlowActionResult::succeeded()
                    : GameFlowActionResult::failed(
                        "GameWorldRouter accepted the request without pending work.");
            }

            const RuntimeSceneLoadStatus status =
                state->loader->getLoadStatus();
            if (status.state != RuntimeSceneLoadState::Queued
                || status.requestId == 0) {
                if (status.requestId != 0) {
                    (void)state->loader->cancelLoad(status.requestId);
                }
                return GameFlowActionResult::failed(
                    "RuntimeSceneLoader did not expose the queued World request.");
            }
            state->pending = Impl::Pending{
                invocation.executionId, status.requestId};
            return GameFlowActionResult::pending(
                [weak, executionId = invocation.executionId]() {
                    if (const auto locked = weak.lock()) {
                        locked->cancel(executionId);
                    }
                });
        }, error);
    if (!handlerInstalled) {
        _impl->eventBus.unsubscribe(_impl->connection);
        _impl->connection = ayt::event::kInvalidConnectionId;
        _impl->state->installed = false;
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

void GameFlowWorldActionAdapter::uninstall() noexcept
{
    if (_impl == nullptr || !_impl->state->installed) return;
    _impl->state->installed = false;
    if (_impl->connection != ayt::event::kInvalidConnectionId) {
        _impl->eventBus.unsubscribe(_impl->connection);
        _impl->connection = ayt::event::kInvalidConnectionId;
    }
    if (_impl->state->pending.has_value()) {
        const auto pending = *_impl->state->pending;
        _impl->state->pending.reset();
        (void)_impl->state->loader->cancelLoad(pending.sceneRequestId);
        std::string ignored;
        (void)_impl->state->coordinator->completeAction(
            pending.actionExecutionId,
            GameFlowActionResult::cancelled(
                "World action adapter was uninstalled."),
            &ignored);
    }
    if (_impl->previousHandler) {
        std::string ignored;
        (void)_impl->registry.setActionHandler(
            kGameFlowActionWorldReplace,
            std::move(_impl->previousHandler), &ignored);
    } else {
        (void)_impl->registry.clearActionHandler(
            kGameFlowActionWorldReplace);
    }
}

bool GameFlowWorldActionAdapter::installed() const noexcept
{
    return _impl != nullptr && _impl->state->installed;
}

std::uint64_t GameFlowWorldActionAdapter::pendingSceneRequestId() const noexcept
{
    return _impl != nullptr && _impl->state->pending.has_value()
        ? _impl->state->pending->sceneRequestId : 0;
}

std::unique_ptr<GameFlowWorldActionAdapter>
createGameFlowWorldActionAdapter(
    IEngineHost& host,
    GameFlowActionRegistry& registry,
    GameFlowCoordinator& coordinator,
    std::string* error)
{
    IGameWorldRouter* router = nullptr;
    IRuntimeSceneLoader* loader = nullptr;
    try {
        router = host.service<IGameWorldRouter>(
            kHostServiceGameWorldRouter);
        loader = host.service<IRuntimeSceneLoader>(
            kHostServiceRuntimeSceneLoader);
    } catch (...) {
        if (error != nullptr) {
            *error = "EngineHost rejected GameFlow service lookup.";
        }
        return nullptr;
    }
    if (router == nullptr || loader == nullptr) {
        if (error != nullptr) {
            *error = router == nullptr
                ? "GameWorldRouter service is unavailable."
                : "RuntimeSceneLoader service is unavailable.";
        }
        return nullptr;
    }
    auto adapter = std::make_unique<GameFlowWorldActionAdapter>(
        registry, coordinator, *router, *loader, host.eventBus());
    if (!adapter->install(error)) return nullptr;
    return adapter;
}

} // namespace ayt::app
