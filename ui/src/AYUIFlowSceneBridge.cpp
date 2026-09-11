#include <AYApplication/UIFlowSceneBridge.h>

#include <AYApplication/UIFlowSceneComponents.h>
#include <AYEntity.h>
#include <AYEventSystem/EventBus.h>
#include <AYEventSystem/Events/SceneEvents.h>
#include <AYScene.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ayt::app
{
namespace
{

std::string entityIdentity(const ayt::entity::Entity& entity)
{
    return "entity:" + std::to_string(entity.getId());
}

const char* sceneModeName(ayt::scene::SceneMode mode) noexcept
{
    return mode == ayt::scene::SceneMode::Play ? "play" : "edit";
}

std::uint64_t pairKey(std::uint32_t volume, std::uint32_t participant)
{
    return (static_cast<std::uint64_t>(volume) << 32u) | participant;
}

} // namespace

class UIFlowSceneBridge::Impl
{
public:
    struct VolumeSnapshot
    {
        std::uint32_t entityId = 0;
        std::string entityIdentity;
        std::string sourceId;
        std::string enterSignal;
        std::string exitSignal;
        std::string participantTag;
        ayt::math::FVector3 center{};
        ayt::math::FVector3 extent{};
        bool emitOnce = false;
    };

    struct ParticipantSnapshot
    {
        std::uint32_t entityId = 0;
        std::string entityIdentity;
        std::string tag;
        ayt::math::FVector3 position{};
    };

    struct ActivePair
    {
        std::uint32_t volumeId = 0;
        std::string exitSignal;
        UIFlowSceneSignalEvent event;
        bool allowSignals = true;
    };

    struct Presentation
    {
        UIFlowScenePresentationHandle handle = 0;
        std::string sourceId;
        ayt::ui::UIFlowScope lifetime = ayt::ui::UIFlowScope::World;
        std::string lifetimeKey;
    };

    Impl(
        UIFlowRuntime& value,
        ayt::event::EventBus& bus,
        UIFlowSceneBridgeConfig valueConfig)
        : runtime(value), eventBus(bus), config(std::move(valueConfig))
    {
    }

    void setError(std::string value, std::string* output = nullptr)
    {
        lastError = std::move(value);
        if (output != nullptr) *output = lastError;
    }

    void clearError(std::string* output = nullptr)
    {
        lastError.clear();
        if (output != nullptr) output->clear();
    }

    std::string resolveWorldKey(const ayt::scene::Scene& scene)
    {
        if (config.worldKeyResolver) {
            try {
                std::string key = config.worldKeyResolver(scene);
                if (!key.empty()) return key;
            } catch (const std::exception& exception) {
                setError(std::string("World key resolver threw: ")
                    + exception.what());
            } catch (...) {
                setError("World key resolver threw an unknown exception.");
            }
        }
        if (!scene.path().empty()) return scene.path();
        if (!scene.name().empty()) return scene.name();
        return "scene-instance:"
            + std::to_string(reinterpret_cast<std::uintptr_t>(&scene));
    }

    std::vector<const UIFlowWorldContextBinding*> findBindings(
        std::string_view key) const
    {
        std::vector<const UIFlowWorldContextBinding*> result;
        // Wildcard Contexts establish reusable per-World HUD/defaults. Exact
        // bindings are activated afterwards so equal-priority exact Contexts
        // win the normal newest-activation Slot tie-break.
        for (const auto& binding : config.worldContexts) {
            if (binding.worldKey == "*") result.push_back(&binding);
        }
        for (const auto& binding : config.worldContexts) {
            if (binding.worldKey == key) result.push_back(&binding);
        }
        return result;
    }

    bool retireScenePresentations(
        std::string_view previousWorldKey,
        bool worldScopeWillEnd,
        std::string& error)
    {
        for (auto it = presentations.begin(); it != presentations.end();) {
            if (it->lifetime == ayt::ui::UIFlowScope::Application) {
                ++it;
                continue;
            }
            // World-scope replacement/end removes these handles atomically.
            // Owner/Transient activations are bridge-owned and need an
            // explicit pop before the Scene loses its owners.
            if (it->lifetime != ayt::ui::UIFlowScope::World
                || it->lifetimeKey != previousWorldKey
                || !worldScopeWillEnd) {
                std::string localError;
                if (!runtime.deactivateContext(it->handle, &localError)) {
                    error = "Cannot retire Scene presentation '"
                        + it->sourceId + "': " + localError;
                    return false;
                }
                ++statistics.presentationPops;
            } else {
                // The following World scope replacement/end owns the actual
                // runtime deactivation, but this bridge-owned presentation is
                // retired at the same boundary and must be counted here.
                ++statistics.presentationPops;
            }
            it = presentations.erase(it);
        }
        return true;
    }

    void retireAllPresentations() noexcept
    {
        for (const Presentation& presentation : presentations) {
            try {
                std::string ignored;
                if (runtime.deactivateContext(
                        presentation.handle, &ignored)) {
                    ++statistics.presentationPops;
                }
            } catch (...) {
            }
        }
        presentations.clear();
    }

    UIFlowPayload scenePayload(const ayt::scene::Scene* scene) const
    {
        UIFlowPayload payload;
        payload["world"] = currentWorldKey;
        payload["scene"] = scene == nullptr ? std::string{} : scene->name();
        payload["path"] = scene == nullptr ? std::string{} : scene->path();
        payload["mode"] = scene == nullptr
            ? std::string("none")
            : std::string(sceneModeName(scene->mode()));
        return payload;
    }

    bool emitOptionalLifecycle(
        std::string_view signalId,
        const ayt::scene::Scene* scene,
        std::string& error)
    {
        if (signalId.empty()) return true;
        const ayt::ui::UIFlowDocument* document = runtime.document();
        if (document == nullptr || document->findSignal(signalId) == nullptr) {
            return true;
        }
        if (!runtime.emitSignal(signalId, scenePayload(scene), &error)) {
            ++statistics.rejectedSignals;
            return false;
        }
        ++statistics.emittedSignals;
        return true;
    }

    bool switchScene(
        ayt::scene::Scene* next,
        bool notify,
        std::string& error)
    {
        if (next == activeScene && scopeSynchronized) return true;

        const std::string previousKey = currentWorldKey;
        const std::string nextKey = next == nullptr
            ? std::string{}
            : resolveWorldKey(*next);
        const bool worldScopeWillEnd = next == nullptr
            || nextKey != previousKey;
        if (!retireScenePresentations(
                previousKey, worldScopeWillEnd, error)) {
            return false;
        }
        activePairs.clear();
        firedOnceVolumes.clear();
        activeScene = next;
        ++statistics.sceneChanges;

        if (next == nullptr) {
            if (!previousKey.empty()
                && runtime.scopeKey(ayt::ui::UIFlowScope::World)
                    == previousKey
                && !runtime.endScope(
                    ayt::ui::UIFlowScope::World, previousKey, &error)) {
                return false;
            }
            currentWorldKey.clear();
        } else if (nextKey != previousKey) {
            if (!runtime.beginScope(
                    ayt::ui::UIFlowScope::World, nextKey, &error)) {
                // A transactional scope replacement restores the old scope on
                // failure. That World is leaving, so explicitly retire it to
                // avoid preserving stale World UI against the new Scene.
                if (!previousKey.empty()
                    && runtime.scopeKey(ayt::ui::UIFlowScope::World)
                        == previousKey) {
                    std::string ignored;
                    (void)runtime.endScope(
                        ayt::ui::UIFlowScope::World,
                        previousKey,
                        &ignored);
                }
                currentWorldKey.clear();
                scopeSynchronized = false;
                return false;
            }
            currentWorldKey = nextKey;
            for (const UIFlowWorldContextBinding* binding
                 : findBindings(nextKey)) {
                if (binding == nullptr || binding->contextId.empty()) continue;
                UIFlowContextActivationOptions options;
                options.lifetime = {
                    ayt::ui::UIFlowScope::World, currentWorldKey};
                if (runtime.activateContext(
                        binding->contextId, std::move(options), &error) == 0) {
                    std::string ignored;
                    (void)runtime.endScope(
                        ayt::ui::UIFlowScope::World,
                        currentWorldKey,
                        &ignored);
                    currentWorldKey.clear();
                    scopeSynchronized = false;
                    return false;
                }
            }
        } else {
            currentWorldKey = nextKey;
        }
        scopeSynchronized = true;

        if (notify && !emitOptionalLifecycle(
                config.currentChangedSignal, next, error)) {
            return false;
        }
        return true;
    }

    bool emitRequired(
        std::string_view signalId,
        UIFlowSceneSignalEvent event,
        std::string& error)
    {
        UIFlowPayload payload = std::move(event.payload);
        const UIFlowPayload common = scenePayload(activeScene);
        for (const auto& pair : common) payload[pair.first] = pair.second;
        payload["sourceId"] = event.sourceId;
        payload["sourceEntity"] = event.sourceEntity;
        payload["entity"] = event.participantEntity;
        payload["participantTag"] = event.participantTag;
        if (!runtime.emitSignal(signalId, std::move(payload), &error)) {
            ++statistics.rejectedSignals;
            return false;
        }
        ++statistics.emittedSignals;
        return true;
    }

    static bool contains(
        const VolumeSnapshot& volume,
        const ParticipantSnapshot& participant) noexcept
    {
        return std::abs(participant.position.x - volume.center.x)
                <= volume.extent.x
            && std::abs(participant.position.y - volume.center.y)
                <= volume.extent.y
            && std::abs(participant.position.z - volume.center.z)
                <= volume.extent.z;
    }

    bool scanVolumes(std::string& error)
    {
        if (!config.enableSignalVolumes || activeScene == nullptr) return true;
        if (!config.processEditScenes
            && activeScene->mode() == ayt::scene::SceneMode::Edit) {
            activePairs.clear();
            return true;
        }

        ayt::entity::World& world = activeScene->world();
        std::vector<VolumeSnapshot> volumes;
        for (ayt::entity::Entity* entity : world.query<
                 ayt::entity::Transform, SceneSignalVolumeComponent>()) {
            if (entity == nullptr) continue;
            auto* transform = entity->getComponent<ayt::entity::Transform>();
            auto* volume = entity->getComponent<SceneSignalVolumeComponent>();
            if (transform == nullptr || volume == nullptr || !volume->enabled) {
                continue;
            }
            VolumeSnapshot snapshot;
            snapshot.entityId = entity->getId();
            snapshot.entityIdentity = entityIdentity(*entity);
            snapshot.sourceId = volume->sourceId.empty()
                ? snapshot.entityIdentity
                : volume->sourceId;
            snapshot.enterSignal = volume->enterSignal;
            snapshot.exitSignal = volume->exitSignal;
            snapshot.participantTag = volume->participantTag;
            snapshot.center = ayt::math::FVector3(
                transform->position.x + volume->offset.x * transform->scale.x,
                transform->position.y + volume->offset.y * transform->scale.y,
                transform->position.z + volume->offset.z * transform->scale.z);
            snapshot.extent = ayt::math::FVector3(
                std::abs(volume->halfExtents.x * transform->scale.x),
                std::abs(volume->halfExtents.y * transform->scale.y),
                std::abs(volume->halfExtents.z * transform->scale.z));
            snapshot.emitOnce = volume->emitOncePerWorld;
            volumes.push_back(std::move(snapshot));
        }

        std::vector<ParticipantSnapshot> participants;
        for (ayt::entity::Entity* entity : world.query<
                 ayt::entity::Transform,
                 SceneSignalParticipantComponent>()) {
            if (entity == nullptr) continue;
            auto* transform = entity->getComponent<ayt::entity::Transform>();
            auto* participant =
                entity->getComponent<SceneSignalParticipantComponent>();
            if (transform == nullptr || participant == nullptr
                || !participant->enabled) {
                continue;
            }
            participants.push_back(ParticipantSnapshot{
                entity->getId(),
                entityIdentity(*entity),
                participant->tag,
                transform->position});
        }

        bool valid = true;
        std::string firstError;
        std::unordered_map<std::uint64_t, ActivePair> currentPairs;
        for (const VolumeSnapshot& volume : volumes) {
            for (const ParticipantSnapshot& participant : participants) {
                if ((!volume.participantTag.empty()
                        && volume.participantTag != participant.tag)
                    || !contains(volume, participant)) {
                    continue;
                }
                const std::uint64_t key = pairKey(
                    volume.entityId, participant.entityId);
                const auto previous = activePairs.find(key);
                bool allowSignals = previous == activePairs.end()
                    ? !(volume.emitOnce
                        && firedOnceVolumes.contains(volume.entityId))
                    : previous->second.allowSignals;
                UIFlowSceneSignalEvent event;
                event.sourceId = volume.sourceId;
                event.sourceEntity = volume.entityIdentity;
                event.participantEntity = participant.entityIdentity;
                event.participantTag = participant.tag;
                currentPairs.emplace(key, ActivePair{
                    volume.entityId,
                    volume.exitSignal,
                    event,
                    allowSignals});

                if (previous != activePairs.end()) continue;
                ++statistics.volumeEnters;
                if (volume.emitOnce) {
                    firedOnceVolumes.insert(volume.entityId);
                }
                if (allowSignals && !volume.enterSignal.empty()) {
                    std::string signalError;
                    if (!emitRequired(
                            volume.enterSignal, event, signalError)) {
                        valid = false;
                        if (firstError.empty()) firstError = signalError;
                    }
                }
            }
        }

        for (const auto& pair : activePairs) {
            if (currentPairs.contains(pair.first)) continue;
            ++statistics.volumeExits;
            if (pair.second.allowSignals && !pair.second.exitSignal.empty()) {
                std::string signalError;
                if (!emitRequired(
                        pair.second.exitSignal,
                        pair.second.event,
                        signalError)) {
                    valid = false;
                    if (firstError.empty()) firstError = signalError;
                }
            }
        }
        activePairs = std::move(currentPairs);
        if (!valid) error = std::move(firstError);
        return valid;
    }

    void recordAsyncFailure(std::string value) noexcept
    {
        try {
            lastError = std::move(value);
        } catch (...) {
        }
    }

    UIFlowRuntime& runtime;
    ayt::event::EventBus& eventBus;
    UIFlowSceneBridgeConfig config;
    ayt::scene::Scene* activeScene = nullptr;
    std::string currentWorldKey;
    std::string lastError;
    std::vector<ayt::event::ConnectionId> connections;
    std::unordered_map<std::uint64_t, ActivePair> activePairs;
    std::unordered_set<std::uint32_t> firedOnceVolumes;
    std::vector<Presentation> presentations;
    std::uint64_t nextTransientPresentation = 1;
    UIFlowSceneBridgeStats statistics;
    bool started = false;
    bool scopeSynchronized = true;
};

UIFlowSceneBridge::UIFlowSceneBridge(
    UIFlowRuntime& runtime,
    ayt::event::EventBus& eventBus,
    UIFlowSceneBridgeConfig config)
    : _impl(std::make_unique<Impl>(
          runtime, eventBus, std::move(config)))
{
}

UIFlowSceneBridge::~UIFlowSceneBridge()
{
    stop();
}

bool UIFlowSceneBridge::start(
    ayt::scene::Scene* initialScene,
    std::string* error)
{
    if (_impl->started) {
        _impl->clearError(error);
        return true;
    }
    if (!_impl->runtime.isStarted()) {
        _impl->setError("UI Flow runtime must be started first.", error);
        return false;
    }
    std::unordered_set<std::string> bindings;
    const ayt::ui::UIFlowDocument* document = _impl->runtime.document();
    for (const UIFlowWorldContextBinding& binding
         : _impl->config.worldContexts) {
        if (binding.worldKey.empty() || binding.contextId.empty()) {
            _impl->setError(
                "World Context bindings require non-empty worldKey and "
                "contextId.",
                error);
            return false;
        }
        const std::string bindingKey = binding.worldKey + "\n"
            + binding.contextId;
        if (!bindings.insert(bindingKey).second) {
            _impl->setError(
                "Duplicate World Context binding for '"
                    + binding.worldKey + "' and Context '"
                    + binding.contextId + "'.",
                error);
            return false;
        }
        if (document == nullptr
            || document->findContext(binding.contextId) == nullptr) {
            _impl->setError(
                "World '" + binding.worldKey
                    + "' references unknown Context '"
                    + binding.contextId + "'.",
                error);
            return false;
        }
    }
    try {
        _impl->connections.push_back(
            _impl->eventBus.subscribe<ayt::event::SceneCurrentChangedEvent>(
                [impl = _impl.get()](const auto& event) {
                    std::string callbackError;
                    try {
                        if (!impl->switchScene(
                                event.current, true, callbackError)) {
                            impl->recordAsyncFailure(
                                std::move(callbackError));
                        }
                    } catch (const std::exception& exception) {
                        impl->recordAsyncFailure(exception.what());
                    } catch (...) {
                        impl->recordAsyncFailure(
                            "Scene current-change callback failed.");
                    }
                }));
        _impl->connections.push_back(
            _impl->eventBus.subscribe<ayt::event::SceneBeginPlayEvent>(
                [impl = _impl.get()](const auto& event) {
                    std::string callbackError;
                    try {
                        if (event.play != impl->activeScene
                            && !impl->switchScene(
                                event.play, true, callbackError)) {
                            impl->recordAsyncFailure(
                                std::move(callbackError));
                            return;
                        }
                        if (!impl->emitOptionalLifecycle(
                                impl->config.beginPlaySignal,
                                event.play,
                                callbackError)) {
                            impl->recordAsyncFailure(
                                std::move(callbackError));
                        }
                    } catch (...) {
                        impl->recordAsyncFailure(
                            "Scene begin-play callback failed.");
                    }
                }));
        _impl->connections.push_back(
            _impl->eventBus.subscribe<ayt::event::SceneEndPlayEvent>(
                [impl = _impl.get()](const auto& event) {
                    std::string callbackError;
                    try {
                        if (!impl->emitOptionalLifecycle(
                                impl->config.endPlaySignal,
                                event.edit,
                                callbackError)) {
                            impl->recordAsyncFailure(
                                std::move(callbackError));
                        }
                    } catch (...) {
                        impl->recordAsyncFailure(
                            "Scene end-play callback failed.");
                    }
                }));
        _impl->connections.push_back(
            _impl->eventBus.subscribe<UIFlowSceneSignalRequestEvent>(
                [impl = _impl.get()](const auto& request) {
                    std::string callbackError;
                    try {
                        if (!impl->emitRequired(
                                request.signalId,
                                request.event,
                                callbackError)) {
                            impl->recordAsyncFailure(
                                std::move(callbackError));
                        }
                    } catch (const std::exception& exception) {
                        impl->recordAsyncFailure(exception.what());
                    } catch (...) {
                        impl->recordAsyncFailure(
                            "Scene signal request callback failed.");
                    }
                }));
    } catch (const std::exception& exception) {
        stop();
        _impl->setError(
            std::string("Cannot subscribe Scene lifecycle: ")
                + exception.what(),
            error);
        return false;
    }
    _impl->started = true;
    std::string switchError;
    if (!_impl->switchScene(initialScene, initialScene != nullptr, switchError)) {
        stop();
        _impl->setError(std::move(switchError), error);
        return false;
    }
    _impl->clearError(error);
    return true;
}

void UIFlowSceneBridge::stop() noexcept
{
    if (!_impl) return;
    try {
        for (ayt::event::ConnectionId connection : _impl->connections) {
            _impl->eventBus.unsubscribe(connection);
        }
        _impl->connections.clear();
        _impl->retireAllPresentations();
        if (!_impl->currentWorldKey.empty()
            && _impl->runtime.scopeKey(ayt::ui::UIFlowScope::World)
                == _impl->currentWorldKey) {
            std::string ignored;
            (void)_impl->runtime.endScope(
                ayt::ui::UIFlowScope::World,
                _impl->currentWorldKey,
                &ignored);
        }
    } catch (...) {
    }
    _impl->activePairs.clear();
    _impl->firedOnceVolumes.clear();
    _impl->activeScene = nullptr;
    _impl->currentWorldKey.clear();
    _impl->started = false;
    _impl->scopeSynchronized = true;
}

bool UIFlowSceneBridge::isStarted() const noexcept
{
    return _impl->started;
}

bool UIFlowSceneBridge::update(
    ayt::scene::Scene* currentScene,
    std::string* error)
{
    if (!_impl->started) {
        _impl->setError("UI Flow Scene bridge is not started.", error);
        return false;
    }
    std::string updateError;
    if ((currentScene != _impl->activeScene
            || !_impl->scopeSynchronized)
        && !_impl->switchScene(currentScene, true, updateError)) {
        _impl->setError(std::move(updateError), error);
        return false;
    }
    if (!_impl->scanVolumes(updateError)) {
        _impl->setError(std::move(updateError), error);
        return false;
    }
    _impl->clearError(error);
    return true;
}

bool UIFlowSceneBridge::emitSceneSignal(
    std::string_view signalId,
    UIFlowSceneSignalEvent event,
    std::string* error)
{
    if (!_impl->started) {
        _impl->setError("UI Flow Scene bridge is not started.", error);
        return false;
    }
    std::string signalError;
    if (!_impl->emitRequired(signalId, std::move(event), signalError)) {
        _impl->setError(std::move(signalError), error);
        return false;
    }
    _impl->clearError(error);
    return true;
}

UIFlowScenePresentationHandle UIFlowSceneBridge::pushPresentation(
    UIFlowScenePresentationRequest request,
    std::string* error)
{
    if (!_impl->started) {
        _impl->setError("UI Flow Scene bridge is not started.", error);
        return 0;
    }
    if (request.contextId.empty() || request.sourceId.empty()) {
        _impl->setError(
            "Scene presentations require non-empty contextId and sourceId.",
            error);
        return 0;
    }

    switch (request.lifetime) {
    case ayt::ui::UIFlowScope::Application:
        if (!request.lifetimeKey.empty()
            && request.lifetimeKey != "application") {
            _impl->setError(
                "Application Scene presentations must use lifetimeKey "
                "'application'.",
                error);
            return 0;
        }
        request.lifetimeKey = "application";
        break;
    case ayt::ui::UIFlowScope::World:
        if (_impl->currentWorldKey.empty()) {
            _impl->setError(
                "World Scene presentations require an active Scene.", error);
            return 0;
        }
        if (!request.lifetimeKey.empty()
            && request.lifetimeKey != _impl->currentWorldKey) {
            _impl->setError(
                "World Scene presentation lifetimeKey does not match the "
                "active Scene.",
                error);
            return 0;
        }
        request.lifetimeKey = _impl->currentWorldKey;
        break;
    case ayt::ui::UIFlowScope::Owner:
        if (request.lifetimeKey.empty()) {
            request.lifetimeKey = request.sourceId;
        }
        break;
    case ayt::ui::UIFlowScope::Transient:
        if (request.lifetimeKey.empty()) {
            request.lifetimeKey = "scene-presentation:"
                + std::to_string(_impl->nextTransientPresentation++);
        }
        break;
    }

    UIFlowContextActivationOptions options;
    options.lifetime = {request.lifetime, request.lifetimeKey};
    std::string activationError;
    const UIFlowScenePresentationHandle handle =
        _impl->runtime.activateContext(
            request.contextId, std::move(options), &activationError);
    if (handle == 0) {
        _impl->setError(std::move(activationError), error);
        return 0;
    }

    try {
        _impl->presentations.push_back(Impl::Presentation{
            handle,
            std::move(request.sourceId),
            request.lifetime,
            std::move(request.lifetimeKey)});
    } catch (...) {
        std::string ignored;
        (void)_impl->runtime.deactivateContext(handle, &ignored);
        _impl->setError(
            "Cannot retain Scene presentation ownership.", error);
        return 0;
    }
    ++_impl->statistics.presentationPushes;
    _impl->clearError(error);
    return handle;
}

bool UIFlowSceneBridge::popPresentation(
    UIFlowScenePresentationHandle handle,
    std::string* error)
{
    if (!_impl->started) {
        _impl->setError("UI Flow Scene bridge is not started.", error);
        return false;
    }
    const auto found = std::find_if(
        _impl->presentations.begin(),
        _impl->presentations.end(),
        [handle](const Impl::Presentation& presentation) {
            return presentation.handle == handle;
        });
    if (found == _impl->presentations.end()) {
        _impl->setError("Unknown Scene presentation handle.", error);
        return false;
    }
    std::string deactivationError;
    if (!_impl->runtime.deactivateContext(handle, &deactivationError)) {
        _impl->setError(std::move(deactivationError), error);
        return false;
    }
    _impl->presentations.erase(found);
    ++_impl->statistics.presentationPops;
    _impl->clearError(error);
    return true;
}

std::size_t UIFlowSceneBridge::releasePresentations(
    std::string_view sourceId,
    std::string* error)
{
    if (!_impl->started) {
        _impl->setError("UI Flow Scene bridge is not started.", error);
        return 0;
    }
    if (sourceId.empty()) {
        _impl->setError(
            "Scene presentation sourceId cannot be empty.", error);
        return 0;
    }
    std::vector<UIFlowScenePresentationHandle> handles;
    for (const Impl::Presentation& presentation : _impl->presentations) {
        if (presentation.sourceId == sourceId) {
            handles.push_back(presentation.handle);
        }
    }
    std::size_t released = 0;
    for (UIFlowScenePresentationHandle handle : handles) {
        if (!popPresentation(handle, error)) return released;
        ++released;
    }
    _impl->clearError(error);
    return released;
}

std::size_t UIFlowSceneBridge::activePresentationCount() const noexcept
{
    return _impl->presentations.size();
}

ayt::scene::Scene* UIFlowSceneBridge::currentScene() const noexcept
{
    return _impl->activeScene;
}

std::string_view UIFlowSceneBridge::currentWorldKey() const noexcept
{
    return _impl->currentWorldKey;
}

const UIFlowSceneBridgeStats& UIFlowSceneBridge::stats() const noexcept
{
    return _impl->statistics;
}

std::string_view UIFlowSceneBridge::lastError() const noexcept
{
    return _impl->lastError;
}

} // namespace ayt::app
