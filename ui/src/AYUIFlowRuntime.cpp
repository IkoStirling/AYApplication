#include <AYApplication/UIFlowRuntime.h>

#include <algorithm>
#include <deque>
#include <exception>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ayt::app
{
namespace
{

using ayt::ui::UIFlowContextDefinition;
using ayt::ui::UIFlowFieldDefinition;
using ayt::ui::UIFlowGraphDefinition;
using ayt::ui::UIFlowLayerDefinition;
using ayt::ui::UIFlowRegionDefinition;
using ayt::ui::UIFlowScope;
using ayt::ui::UIFlowScreenDefinition;
using ayt::ui::UIFlowSlotAssignment;
using ayt::ui::UIFlowSlotDefinition;
using ayt::ui::UIFlowSlotOperation;
using ayt::ui::UIFlowStateDefinition;
using ayt::ui::UIFlowTransitionDefinition;
using ayt::ui::UIFlowValue;
using ayt::ui::UIFlowValueType;

void writeError(std::string* output, const std::string& value)
{
    if (output != nullptr) *output = value;
}

bool isNull(const UIFlowValue& value)
{
    return std::holds_alternative<std::monostate>(value.data);
}

bool valueMatchesType(const UIFlowValue& value, UIFlowValueType type)
{
    if (isNull(value)) return true;
    switch (type) {
    case UIFlowValueType::Boolean:
        return std::holds_alternative<bool>(value.data);
    case UIFlowValueType::Integer:
        return std::holds_alternative<std::int64_t>(value.data);
    case UIFlowValueType::Number:
        return std::holds_alternative<std::int64_t>(value.data)
            || std::holds_alternative<double>(value.data);
    case UIFlowValueType::String:
    case UIFlowValueType::Entity:
    case UIFlowValueType::Asset:
        return std::holds_alternative<std::string>(value.data);
    }
    return false;
}

bool normalizePayload(
    const std::vector<UIFlowFieldDefinition>& fields,
    const UIFlowPayload& input,
    UIFlowPayload& output,
    std::string_view kind,
    std::string_view id,
    std::string& error)
{
    output = input;
    for (const UIFlowFieldDefinition& field : fields) {
        const auto found = output.find(field.id);
        if (found == output.end()) {
            if (!isNull(field.defaultValue)) {
                output[field.id] = field.defaultValue;
                continue;
            }
            if (field.required) {
                error = std::string(kind) + " '" + std::string(id)
                    + "' requires field '" + field.id + "'.";
                return false;
            }
            continue;
        }
        if ((field.required && isNull(found->second))
            || !valueMatchesType(found->second, field.type)) {
            error = std::string(kind) + " '" + std::string(id)
                + "' field '" + field.id + "' must be "
                + ayt::ui::uiFlowValueTypeName(field.type) + ".";
            return false;
        }
    }
    return true;
}

const UIFlowStateDefinition* findState(
    const UIFlowRegionDefinition& region,
    std::string_view id)
{
    const auto found = std::find_if(
        region.states.begin(), region.states.end(),
        [id](const UIFlowStateDefinition& state) { return state.id == id; });
    return found == region.states.end() ? nullptr : &*found;
}

std::string descendInitialState(
    const UIFlowRegionDefinition& region,
    std::string stateId)
{
    std::unordered_set<std::string> visited;
    while (!stateId.empty() && visited.insert(stateId).second) {
        const UIFlowStateDefinition* state = findState(region, stateId);
        if (state == nullptr || state->initialChild.empty()) break;
        stateId = state->initialChild;
    }
    return stateId;
}

std::vector<const UIFlowStateDefinition*> stateLineage(
    const UIFlowRegionDefinition& region,
    std::string_view leafId)
{
    std::vector<const UIFlowStateDefinition*> result;
    const UIFlowStateDefinition* state = findState(region, leafId);
    while (state != nullptr) {
        result.push_back(state);
        state = state->parent.empty() ? nullptr : findState(region, state->parent);
    }
    std::reverse(result.begin(), result.end());
    return result;
}

std::string contextSourcePrefix(std::string_view regionId)
{
    return "state:" + std::string(regionId) + ":";
}

} // namespace

UIFlowActionResult UIFlowActionResult::success()
{
    return {};
}

UIFlowActionResult UIFlowActionResult::failure(std::string message)
{
    return UIFlowActionResult{false, std::move(message)};
}

class UIFlowRuntime::Impl
{
public:
    explicit Impl(IUIFlowScreenHost& value) : host(value) {}

    struct ActiveContext
    {
        UIFlowContextHandle handle = 0;
        std::uint64_t serial = 0;
        std::string contextId;
        int priority = 0;
        UIFlowScopeBinding lifetime;
        std::string source;
        bool manual = false;
    };

    struct DesiredScreen
    {
        const UIFlowScreenDefinition* screen = nullptr;
        const UIFlowLayerDefinition* layer = nullptr;
        const UIFlowSlotDefinition* slot = nullptr;
        const ActiveContext* activation = nullptr;
        std::string scopeKey;
        std::size_t layerIndex = 0;
        std::size_t slotIndex = 0;
        std::size_t stackIndex = 0;
        std::uint32_t orderInLayer = 0;
    };

    struct QueuedSignal
    {
        std::string id;
        UIFlowPayload payload;
    };

    struct Subscription
    {
        UIFlowSignalSubscription id = 0;
        std::string signalId;
        UIFlowSignalHandler handler;
    };

    void setLastError(std::string value, std::string* output = nullptr)
    {
        lastError = std::move(value);
        writeError(output, lastError);
    }

    void clearLastError(std::string* output = nullptr)
    {
        lastError.clear();
        if (output != nullptr) output->clear();
    }

    const UIFlowContextDefinition* context(std::string_view id) const
    {
        return document.findContext(id);
    }

    ActiveContext* addContext(
        std::string_view contextId,
        UIFlowScopeBinding lifetime,
        std::string source,
        bool manual)
    {
        const UIFlowContextDefinition* definition = context(contextId);
        if (definition == nullptr) return nullptr;
        ActiveContext value;
        value.handle = nextContextHandle++;
        value.serial = nextActivationSerial++;
        value.contextId = definition->id;
        value.priority = definition->priority;
        value.lifetime = std::move(lifetime);
        value.source = std::move(source);
        value.manual = manual;
        activeContexts.push_back(std::move(value));
        return &activeContexts.back();
    }

    std::optional<std::string> screenScopeKey(
        const UIFlowScreenDefinition& screen,
        const ActiveContext& activation) const
    {
        switch (screen.scope) {
        case UIFlowScope::Application:
            return std::string("application");
        case UIFlowScope::World: {
            const auto found = scopeKeys.find(UIFlowScope::World);
            if (found == scopeKeys.end() || found->second.empty()) {
                return std::nullopt;
            }
            return found->second;
        }
        case UIFlowScope::Owner:
            if (activation.lifetime.scope == UIFlowScope::Owner
                && !activation.lifetime.key.empty()) {
                return activation.lifetime.key;
            } else {
                const auto found = scopeKeys.find(UIFlowScope::Owner);
                if (found == scopeKeys.end() || found->second.empty()) {
                    return std::nullopt;
                }
                return found->second;
            }
        case UIFlowScope::Transient:
            return "activation:" + std::to_string(activation.serial);
        }
        return std::nullopt;
    }

    bool buildDesired(std::vector<DesiredScreen>& output, std::string& error)
    {
        output.clear();
        std::unordered_map<std::string, std::size_t> layerIndices;
        for (std::size_t index = 0; index < document.layers.size(); ++index) {
            layerIndices[document.layers[index].id] = index;
        }

        struct Candidate
        {
            const ActiveContext* activation = nullptr;
            const UIFlowSlotAssignment* assignment = nullptr;
        };

        for (std::size_t slotIndex = 0; slotIndex < document.slots.size();
             ++slotIndex) {
            const UIFlowSlotDefinition& slot = document.slots[slotIndex];
            std::vector<Candidate> candidates;
            const std::uint64_t floor = restoreFloors[slot.id];
            for (const ActiveContext& active : activeContexts) {
                if (active.serial <= floor) continue;
                const UIFlowContextDefinition* definition =
                    context(active.contextId);
                if (definition == nullptr) continue;
                for (const UIFlowSlotAssignment& assignment : definition->slots) {
                    if (assignment.slot == slot.id) {
                        candidates.push_back(Candidate{&active, &assignment});
                    }
                }
            }
            std::stable_sort(candidates.begin(), candidates.end(),
                [](const Candidate& lhs, const Candidate& rhs) {
                    if (lhs.activation->priority != rhs.activation->priority) {
                        return lhs.activation->priority > rhs.activation->priority;
                    }
                    return lhs.activation->serial > rhs.activation->serial;
                });
            if (candidates.empty()
                || candidates.front().assignment->operation
                    == UIFlowSlotOperation::Hide) {
                continue;
            }

            std::vector<DesiredScreen> selected;
            std::set<std::pair<std::string, std::string>> identities;
            for (const Candidate& candidate : candidates) {
                if (candidate.assignment->operation == UIFlowSlotOperation::Hide) {
                    continue;
                }
                const UIFlowScreenDefinition* screen =
                    document.findScreen(candidate.assignment->screen);
                if (screen == nullptr) continue;
                const std::optional<std::string> key =
                    screenScopeKey(*screen, *candidate.activation);
                if (!key.has_value()) continue;
                if (!identities.insert({screen->id, *key}).second) continue;
                const UIFlowLayerDefinition* layer =
                    document.findLayer(screen->layer);
                if (layer == nullptr) continue;
                selected.push_back(DesiredScreen{
                    screen,
                    layer,
                    &slot,
                    candidate.activation,
                    *key,
                    layerIndices[layer->id],
                    slotIndex,
                    selected.size(),
                    0u,
                });
                if (selected.size() >= slot.capacity) break;
            }
            // Candidates are highest-first. Widget children render last-on-top,
            // so append the selected stack from lowest to highest.
            std::reverse(selected.begin(), selected.end());
            for (std::size_t index = 0; index < selected.size(); ++index) {
                selected[index].stackIndex = index;
                output.push_back(std::move(selected[index]));
            }
        }

        std::stable_sort(output.begin(), output.end(),
            [](const DesiredScreen& lhs, const DesiredScreen& rhs) {
                if (lhs.layer->order != rhs.layer->order) {
                    return lhs.layer->order < rhs.layer->order;
                }
                if (lhs.layerIndex != rhs.layerIndex) {
                    return lhs.layerIndex < rhs.layerIndex;
                }
                if (lhs.slotIndex != rhs.slotIndex) {
                    return lhs.slotIndex < rhs.slotIndex;
                }
                return lhs.stackIndex < rhs.stackIndex;
            });

        std::unordered_map<std::string, std::uint32_t> layerCounts;
        for (DesiredScreen& desired : output) {
            desired.orderInLayer = layerCounts[desired.layer->id]++;
        }
        for (const UIFlowLayerDefinition& layer : document.layers) {
            const std::uint32_t count = layerCounts[layer.id];
            if (layer.maxActiveScreens != 0u
                && count > layer.maxActiveScreens) {
                error = "Layer '" + layer.id + "' exceeds maxActiveScreens.";
                return false;
            }
        }
        return true;
    }

    static bool sameMountIdentity(
        const UIFlowMountedScreen& mounted,
        const DesiredScreen& desired)
    {
        return mounted.slotId == desired.slot->id
            && mounted.screenId == desired.screen->id
            && mounted.scope == desired.screen->scope
            && mounted.scopeKey == desired.scopeKey;
    }

    bool reconcile(std::string& error)
    {
        std::vector<DesiredScreen> desired;
        if (!buildDesired(desired, error)) return false;

        std::vector<bool> reused(mounted.size(), false);
        std::vector<UIFlowMountedScreen> next;
        next.reserve(desired.size());
        std::vector<std::uint64_t> newlyMounted;

        for (const DesiredScreen& item : desired) {
            auto found = mounted.end();
            for (auto it = mounted.begin(); it != mounted.end(); ++it) {
                const std::size_t index = static_cast<std::size_t>(
                    std::distance(mounted.begin(), it));
                if (!reused[index] && sameMountIdentity(*it, item)) {
                    found = it;
                    reused[index] = true;
                    break;
                }
            }

            UIFlowMountedScreen value;
            if (found != mounted.end()) {
                value = *found;
            } else {
                value.mountId = nextMountId++;
                UIFlowScreenMountRequest request;
                request.mountId = value.mountId;
                request.screenId = item.screen->id;
                request.layoutAsset = item.screen->layoutAsset;
                request.layerId = item.layer->id;
                request.slotId = item.slot->id;
                request.contextId = item.activation->contextId;
                request.scope = item.screen->scope;
                request.scopeKey = item.scopeKey;
                request.layerOrder = item.layer->order;
                request.inputPolicy = item.layer->inputPolicy;
                request.blocksLowerInput = item.layer->blocksLowerInput;
                request.orderInLayer = item.orderInLayer;
                request.parameters = item.screen->parameters;
                std::string mountError;
                bool mountedSuccessfully = false;
                try {
                    mountedSuccessfully = host.mountScreen(request, mountError);
                } catch (const std::exception& exception) {
                    mountError = exception.what();
                } catch (...) {
                    mountError = "screen host threw an unknown exception";
                }
                if (!mountedSuccessfully) {
                    for (std::uint64_t mountId : newlyMounted) {
                        host.unmountScreen(mountId);
                    }
                    error = "Failed to mount Screen '" + item.screen->id
                        + "': " + (mountError.empty()
                            ? std::string("screen host rejected the request")
                            : mountError);
                    return false;
                }
                newlyMounted.push_back(value.mountId);
            }
            value.activationSerial = item.activation->serial;
            value.screenId = item.screen->id;
            value.layerId = item.layer->id;
            value.slotId = item.slot->id;
            value.contextId = item.activation->contextId;
            value.scope = item.screen->scope;
            value.scopeKey = item.scopeKey;
            value.layerOrder = item.layer->order;
            value.orderInLayer = item.orderInLayer;
            next.push_back(std::move(value));
        }

        for (std::size_t index = 0; index < mounted.size(); ++index) {
            if (!reused[index]) host.unmountScreen(mounted[index].mountId);
        }
        mounted = std::move(next);
        for (const UIFlowMountedScreen& value : mounted) {
            host.setScreenOrder(
                value.mountId, value.layerOrder, value.orderInLayer);
        }
        return true;
    }

    void unmountAll() noexcept
    {
        for (const UIFlowMountedScreen& value : mounted) {
            host.unmountScreen(value.mountId);
        }
        mounted.clear();
    }

    void markRestoreFloors(
        const std::unordered_set<std::uint64_t>& activationSerials)
    {
        for (const UIFlowMountedScreen& value : mounted) {
            if (activationSerials.find(value.activationSerial)
                == activationSerials.end()) {
                continue;
            }
            const UIFlowSlotDefinition* slot = document.findSlot(value.slotId);
            if (slot != nullptr && !slot->restorePrevious) {
                restoreFloors[slot->id] = std::max(
                    restoreFloors[slot->id], value.activationSerial);
            }
        }
    }

    void removeHandles(const std::unordered_set<UIFlowContextHandle>& handles)
    {
        activeContexts.erase(
            std::remove_if(activeContexts.begin(), activeContexts.end(),
                [&handles](const ActiveContext& value) {
                    return handles.find(value.handle) != handles.end();
                }),
            activeContexts.end());
    }

    void addStateContexts(
        const UIFlowRegionDefinition& region,
        std::string_view leaf)
    {
        const UIFlowScopeBinding lifetime{
            UIFlowScope::Application, "application"};
        for (const UIFlowStateDefinition* state : stateLineage(region, leaf)) {
            for (const std::string& contextId : state->contexts) {
                addContext(contextId, lifetime,
                    contextSourcePrefix(region.id) + state->id, false);
            }
        }
    }

    std::unordered_set<UIFlowContextHandle> handlesForSources(
        const std::unordered_set<std::string>& sources) const
    {
        std::unordered_set<UIFlowContextHandle> result;
        for (const ActiveContext& value : activeContexts) {
            if (sources.find(value.source) != sources.end()) {
                result.insert(value.handle);
            }
        }
        return result;
    }

    bool requestGraph(
        std::string_view graphId,
        const QueuedSignal* signal,
        std::string_view region,
        std::string_view transition,
        std::string& error)
    {
        if (graphId.empty() || !graphHandler) return true;
        UIFlowGraphRequest request;
        request.graphId = std::string(graphId);
        if (signal != nullptr) {
            request.signalId = signal->id;
            request.payload = signal->payload;
        }
        request.regionId = std::string(region);
        request.transitionId = std::string(transition);
        try {
            graphHandler(request);
            return true;
        } catch (const std::exception& exception) {
            error = "Graph request handler threw for '" + request.graphId
                + "': " + exception.what();
        } catch (...) {
            error = "Graph request handler threw for '" + request.graphId
                + "'.";
        }
        return false;
    }

    bool applyTransition(
        const UIFlowRegionDefinition& region,
        const UIFlowTransitionDefinition& transition,
        const QueuedSignal& signal,
        std::string& error)
    {
        const auto stateFound = regionStates.find(region.id);
        if (stateFound == regionStates.end()) return true;
        const std::string oldLeaf = stateFound->second;
        const std::string newLeaf = descendInitialState(region, transition.toState);
        const auto oldLineage = stateLineage(region, oldLeaf);
        const auto newLineage = stateLineage(region, newLeaf);
        std::size_t common = 0;
        while (common < oldLineage.size() && common < newLineage.size()
               && oldLineage[common]->id == newLineage[common]->id) {
            ++common;
        }

        const std::vector<ActiveContext> oldContexts = activeContexts;
        const auto oldStates = regionStates;
        const auto oldFloors = restoreFloors;
        const std::uint64_t oldNextHandle = nextContextHandle;
        const std::uint64_t oldNextSerial = nextActivationSerial;

        std::unordered_set<std::string> exitingSources;
        for (std::size_t index = common; index < oldLineage.size(); ++index) {
            exitingSources.insert(
                contextSourcePrefix(region.id) + oldLineage[index]->id);
        }
        const auto handles = handlesForSources(exitingSources);
        std::unordered_set<UIFlowContextHandle> mountedSerials;
        for (const ActiveContext& value : activeContexts) {
            if (handles.find(value.handle) != handles.end()) {
                mountedSerials.insert(value.serial);
            }
        }
        markRestoreFloors(mountedSerials);
        removeHandles(handles);
        regionStates[region.id] = newLeaf;
        const UIFlowScopeBinding lifetime{
            UIFlowScope::Application, "application"};
        for (std::size_t index = common; index < newLineage.size(); ++index) {
            for (const std::string& contextId : newLineage[index]->contexts) {
                addContext(
                    contextId,
                    lifetime,
                    contextSourcePrefix(region.id) + newLineage[index]->id,
                    false);
            }
        }
        if (!reconcile(error)) {
            activeContexts = oldContexts;
            regionStates = oldStates;
            restoreFloors = oldFloors;
            nextContextHandle = oldNextHandle;
            nextActivationSerial = oldNextSerial;
            std::string ignored;
            (void)reconcile(ignored);
            return false;
        }

        for (std::size_t index = oldLineage.size(); index > common; --index) {
            if (!requestGraph(oldLineage[index - 1]->exitGraph, &signal,
                              region.id, transition.id, error)) {
                return false;
            }
        }
        if (!requestGraph(transition.actionGraph, &signal,
                          region.id, transition.id, error)) {
            return false;
        }
        for (std::size_t index = common; index < newLineage.size(); ++index) {
            if (!requestGraph(newLineage[index]->enterGraph, &signal,
                              region.id, transition.id, error)) {
                return false;
            }
        }
        return true;
    }

    bool processSignal(const QueuedSignal& signal, std::string& error)
    {
        bool valid = true;
        for (const UIFlowRegionDefinition& region : document.regions) {
            const auto activeFound = regionStates.find(region.id);
            if (activeFound == regionStates.end()) continue;
            const auto lineage = stateLineage(region, activeFound->second);
            std::unordered_set<std::string> activeIds;
            for (const UIFlowStateDefinition* state : lineage) {
                activeIds.insert(state->id);
            }

            const UIFlowTransitionDefinition* selected = nullptr;
            for (const UIFlowTransitionDefinition& transition
                 : document.transitions) {
                if (transition.region != region.id
                    || transition.triggerSignal != signal.id
                    || (transition.fromState != "*"
                        && activeIds.find(transition.fromState)
                            == activeIds.end())) {
                    continue;
                }
                bool guardPassed = true;
                if (!transition.guardExpression.empty()) {
                    if (!guardEvaluator) {
                        error = "Transition '" + transition.id
                            + "' requires a guard evaluator.";
                        valid = false;
                        continue;
                    }
                    std::string guardError;
                    try {
                        guardPassed = guardEvaluator(
                            transition.guardExpression,
                            signal.payload,
                            guardError);
                    } catch (const std::exception& exception) {
                        guardError = exception.what();
                        guardPassed = false;
                    } catch (...) {
                        guardError = "guard evaluator threw an unknown exception";
                        guardPassed = false;
                    }
                    if (!guardError.empty()) {
                        error = "Transition '" + transition.id
                            + "' guard failed: " + guardError;
                        valid = false;
                        continue;
                    }
                }
                if (!guardPassed) continue;
                if (selected == nullptr
                    || transition.priority > selected->priority) {
                    selected = &transition;
                }
            }
            if (selected != nullptr
                && !applyTransition(region, *selected, signal, error)) {
                valid = false;
            }
        }

        std::vector<UIFlowSignalHandler> callbacks;
        for (const Subscription& value : subscriptions) {
            if (value.signalId == signal.id || value.signalId == "*") {
                callbacks.push_back(value.handler);
            }
        }
        for (const UIFlowSignalHandler& callback : callbacks) {
            if (!callback) continue;
            try {
                callback(signal.id, signal.payload);
            } catch (...) {
                error = "Signal listener threw while handling '"
                    + signal.id + "'.";
                valid = false;
            }
        }
        return valid;
    }

    IUIFlowScreenHost& host;
    ayt::ui::UIFlowDocument document;
    bool loaded = false;
    bool started = false;
    std::vector<ActiveContext> activeContexts;
    std::vector<UIFlowMountedScreen> mounted;
    std::map<UIFlowScope, std::string> scopeKeys;
    std::unordered_map<std::string, std::uint64_t> restoreFloors;
    std::unordered_map<std::string, std::string> regionStates;
    std::deque<QueuedSignal> signalQueue;
    std::vector<Subscription> subscriptions;
    std::unordered_map<std::string, UIFlowActionHandler> actions;
    UIFlowGuardEvaluator guardEvaluator;
    UIFlowGraphRequestHandler graphHandler;
    std::uint64_t nextContextHandle = 1;
    std::uint64_t nextActivationSerial = 1;
    std::uint64_t nextMountId = 1;
    std::uint64_t nextSubscription = 1;
    bool dispatchingSignals = false;
    std::string lastError;
};

UIFlowRuntime::UIFlowRuntime(IUIFlowScreenHost& screenHost)
    : _impl(std::make_unique<Impl>(screenHost))
{
}

UIFlowRuntime::~UIFlowRuntime()
{
    unload();
}

UIFlowRuntime::UIFlowRuntime(UIFlowRuntime&&) noexcept = default;
UIFlowRuntime& UIFlowRuntime::operator=(UIFlowRuntime&& other) noexcept
{
    if (this == &other) return *this;
    unload();
    _impl = std::move(other._impl);
    return *this;
}

bool UIFlowRuntime::load(
    ayt::ui::UIFlowDocument document,
    std::string* error)
{
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (!ayt::ui::validateUIFlow(document, &diagnostics)) {
        if (diagnostics.empty()) {
            _impl->setLastError("UI Flow validation failed.", error);
        } else {
            const auto& first = diagnostics.front();
            _impl->setLastError(
                (first.path.empty() ? std::string{} : first.path + ": ")
                    + first.message,
                error);
        }
        return false;
    }
    unload();
    _impl->document = std::move(document);
    _impl->loaded = true;
    _impl->scopeKeys[UIFlowScope::Application] = "application";
    _impl->clearLastError(error);
    return true;
}

bool UIFlowRuntime::start(std::string_view entry, std::string* error)
{
    if (!_impl->loaded) {
        _impl->setLastError("UI Flow is not loaded.", error);
        return false;
    }

    const std::string entryId = entry.empty()
        ? _impl->document.defaultEntry
        : std::string(entry);
    const ayt::ui::UIFlowEntryDefinition* entryDefinition = nullptr;
    if (!entryId.empty()) {
        entryDefinition = _impl->document.findEntry(entryId);
        if (entryDefinition == nullptr) {
            _impl->setLastError(
                "Unknown UI Flow entry '" + entryId + "'.", error);
            return false;
        }
    }

    // Build the new logical presentation transactionally. reconcile() mounts
    // replacements before retiring old screens; preserving the old model here
    // means an invalid asset or host rejection cannot blank a running UI.
    const auto oldContexts = _impl->activeContexts;
    const auto oldFloors = _impl->restoreFloors;
    const auto oldStates = _impl->regionStates;
    const auto oldSignals = _impl->signalQueue;
    const bool oldStarted = _impl->started;
    const std::uint64_t oldNextHandle = _impl->nextContextHandle;
    const std::uint64_t oldNextSerial = _impl->nextActivationSerial;

    _impl->activeContexts.clear();
    _impl->restoreFloors.clear();
    _impl->regionStates.clear();
    _impl->signalQueue.clear();
    _impl->started = false;

    if (entryDefinition != nullptr) {
        for (const std::string& contextId : entryDefinition->contexts) {
            _impl->addContext(
                contextId,
                UIFlowScopeBinding{UIFlowScope::Application, "application"},
                "entry:" + entryDefinition->id,
                false);
        }
    }

    for (const UIFlowRegionDefinition& region : _impl->document.regions) {
        const std::string leaf = descendInitialState(
            region, region.initialState);
        _impl->regionStates[region.id] = leaf;
        _impl->addStateContexts(region, leaf);
    }

    std::string reconcileError;
    if (!_impl->reconcile(reconcileError)) {
        _impl->activeContexts = oldContexts;
        _impl->restoreFloors = oldFloors;
        _impl->regionStates = oldStates;
        _impl->signalQueue = oldSignals;
        _impl->started = oldStarted;
        _impl->nextContextHandle = oldNextHandle;
        _impl->nextActivationSerial = oldNextSerial;
        std::string ignored;
        (void)_impl->reconcile(ignored);
        _impl->setLastError(std::move(reconcileError), error);
        return false;
    }
    _impl->started = true;
    std::string graphError;
    if (entryDefinition != nullptr) {
        if (!_impl->requestGraph(
                entryDefinition->actionGraph, nullptr, {}, {}, graphError)) {
            _impl->setLastError(std::move(graphError), error);
            return false;
        }
    }
    for (const UIFlowRegionDefinition& region : _impl->document.regions) {
        for (const UIFlowStateDefinition* state : stateLineage(
                 region, _impl->regionStates[region.id])) {
            if (!_impl->requestGraph(
                    state->enterGraph, nullptr, region.id, {}, graphError)) {
                _impl->setLastError(std::move(graphError), error);
                return false;
            }
        }
    }
    _impl->clearLastError(error);
    return true;
}

void UIFlowRuntime::unload() noexcept
{
    if (!_impl) return;
    _impl->unmountAll();
    _impl->document = {};
    _impl->activeContexts.clear();
    _impl->restoreFloors.clear();
    _impl->regionStates.clear();
    _impl->scopeKeys.clear();
    _impl->signalQueue.clear();
    _impl->loaded = false;
    _impl->started = false;
    _impl->dispatchingSignals = false;
    _impl->lastError.clear();
}

bool UIFlowRuntime::isLoaded() const noexcept
{
    return _impl->loaded;
}

bool UIFlowRuntime::isStarted() const noexcept
{
    return _impl->started;
}

const ayt::ui::UIFlowDocument* UIFlowRuntime::document() const noexcept
{
    return _impl->loaded ? &_impl->document : nullptr;
}

UIFlowContextHandle UIFlowRuntime::activateContext(
    std::string_view contextId,
    UIFlowContextActivationOptions options,
    std::string* error)
{
    if (!_impl->started) {
        _impl->setLastError("UI Flow is not started.", error);
        return 0;
    }
    if (_impl->context(contextId) == nullptr) {
        _impl->setLastError(
            "Unknown UI Flow Context '" + std::string(contextId) + "'.",
            error);
        return 0;
    }
    if (options.lifetime.scope == UIFlowScope::Application
        && options.lifetime.key.empty()) {
        options.lifetime.key = "application";
    } else if (options.lifetime.key.empty()) {
        const std::string_view active = scopeKey(options.lifetime.scope);
        if (!active.empty()) options.lifetime.key = std::string(active);
    }
    if (options.lifetime.scope != UIFlowScope::Application
        && options.lifetime.key.empty()) {
        _impl->setLastError(
            "Non-application Context lifetime requires an active scope key.",
            error);
        return 0;
    }

    Impl::ActiveContext* active = _impl->addContext(
        contextId, std::move(options.lifetime), "manual", true);
    const UIFlowContextHandle handle = active->handle;
    std::string reconcileError;
    if (!_impl->reconcile(reconcileError)) {
        _impl->activeContexts.erase(
            std::remove_if(_impl->activeContexts.begin(),
                           _impl->activeContexts.end(),
                [handle](const Impl::ActiveContext& value) {
                    return value.handle == handle;
                }),
            _impl->activeContexts.end());
        _impl->setLastError(std::move(reconcileError), error);
        return 0;
    }
    _impl->clearLastError(error);
    return handle;
}

bool UIFlowRuntime::deactivateContext(
    UIFlowContextHandle handle,
    std::string* error)
{
    const auto found = std::find_if(
        _impl->activeContexts.begin(), _impl->activeContexts.end(),
        [handle](const Impl::ActiveContext& value) {
            return value.handle == handle && value.manual;
        });
    if (found == _impl->activeContexts.end()) {
        _impl->setLastError("Unknown manual Context handle.", error);
        return false;
    }

    const auto oldContexts = _impl->activeContexts;
    const auto oldFloors = _impl->restoreFloors;
    const std::unordered_set<UIFlowContextHandle> handles{handle};
    const std::unordered_set<UIFlowContextHandle> serials{found->serial};
    _impl->markRestoreFloors(serials);
    _impl->removeHandles(handles);
    std::string reconcileError;
    if (!_impl->reconcile(reconcileError)) {
        _impl->activeContexts = oldContexts;
        _impl->restoreFloors = oldFloors;
        std::string ignored;
        (void)_impl->reconcile(ignored);
        _impl->setLastError(std::move(reconcileError), error);
        return false;
    }
    _impl->clearLastError(error);
    return true;
}

bool UIFlowRuntime::beginScope(
    UIFlowScope scope,
    std::string key,
    std::string* error)
{
    if (!_impl->loaded || key.empty()) {
        _impl->setLastError(
            !_impl->loaded ? "UI Flow is not loaded."
                           : "Scope key must not be empty.",
            error);
        return false;
    }
    if (scope == UIFlowScope::Application && key != "application") {
        _impl->setLastError(
            "The Application scope is permanent and has the key 'application'.",
            error);
        return false;
    }
    const auto found = _impl->scopeKeys.find(scope);
    if (found != _impl->scopeKeys.end() && found->second == key) {
        _impl->clearLastError(error);
        return true;
    }

    const auto oldScopes = _impl->scopeKeys;
    const auto oldContexts = _impl->activeContexts;
    const auto oldFloors = _impl->restoreFloors;
    if (found != _impl->scopeKeys.end() && !found->second.empty()) {
        std::unordered_set<UIFlowContextHandle> handles;
        std::unordered_set<UIFlowContextHandle> serials;
        for (const Impl::ActiveContext& value : _impl->activeContexts) {
            if (value.lifetime.scope == scope
                && value.lifetime.key == found->second) {
                handles.insert(value.handle);
                serials.insert(value.serial);
            }
        }
        _impl->markRestoreFloors(serials);
        _impl->removeHandles(handles);
    }
    _impl->scopeKeys[scope] = std::move(key);
    std::string reconcileError;
    if (!_impl->reconcile(reconcileError)) {
        _impl->scopeKeys = oldScopes;
        _impl->activeContexts = oldContexts;
        _impl->restoreFloors = oldFloors;
        std::string ignored;
        (void)_impl->reconcile(ignored);
        _impl->setLastError(std::move(reconcileError), error);
        return false;
    }
    _impl->clearLastError(error);
    return true;
}

bool UIFlowRuntime::endScope(
    UIFlowScope scope,
    std::string_view key,
    std::string* error)
{
    if (scope == UIFlowScope::Application) {
        _impl->setLastError(
            "The Application scope cannot be ended.", error);
        return false;
    }
    const auto found = _impl->scopeKeys.find(scope);
    if (found == _impl->scopeKeys.end() || found->second != key) {
        _impl->setLastError("Scope key is not active.", error);
        return false;
    }
    const auto oldScopes = _impl->scopeKeys;
    const auto oldContexts = _impl->activeContexts;
    const auto oldFloors = _impl->restoreFloors;
    std::unordered_set<UIFlowContextHandle> handles;
    std::unordered_set<UIFlowContextHandle> serials;
    for (const Impl::ActiveContext& value : _impl->activeContexts) {
        if (value.lifetime.scope == scope && value.lifetime.key == key) {
            handles.insert(value.handle);
            serials.insert(value.serial);
        }
    }
    _impl->markRestoreFloors(serials);
    _impl->removeHandles(handles);
    _impl->scopeKeys.erase(found);
    std::string reconcileError;
    if (!_impl->reconcile(reconcileError)) {
        _impl->scopeKeys = oldScopes;
        _impl->activeContexts = oldContexts;
        _impl->restoreFloors = oldFloors;
        std::string ignored;
        (void)_impl->reconcile(ignored);
        _impl->setLastError(std::move(reconcileError), error);
        return false;
    }
    _impl->clearLastError(error);
    return true;
}

std::string_view UIFlowRuntime::scopeKey(UIFlowScope scope) const noexcept
{
    const auto found = _impl->scopeKeys.find(scope);
    return found == _impl->scopeKeys.end()
        ? std::string_view{}
        : std::string_view(found->second);
}

bool UIFlowRuntime::emitSignal(
    std::string_view signalId,
    UIFlowPayload payload,
    std::string* error)
{
    if (!_impl->started) {
        _impl->setLastError("UI Flow is not started.", error);
        return false;
    }
    const ayt::ui::UIFlowSignalDefinition* definition =
        _impl->document.findSignal(signalId);
    if (definition == nullptr) {
        _impl->setLastError(
            "Unknown UI Flow Signal '" + std::string(signalId) + "'.",
            error);
        return false;
    }
    UIFlowPayload normalized;
    std::string payloadError;
    if (!normalizePayload(
            definition->payload, payload, normalized,
            "Signal", signalId, payloadError)) {
        _impl->setLastError(std::move(payloadError), error);
        return false;
    }
    _impl->signalQueue.push_back(
        Impl::QueuedSignal{std::string(signalId), std::move(normalized)});
    if (_impl->dispatchingSignals) {
        _impl->clearLastError(error);
        return true;
    }

    _impl->dispatchingSignals = true;
    bool valid = true;
    std::string dispatchError;
    std::size_t processed = 0;
    while (!_impl->signalQueue.empty() && processed < 1024u) {
        Impl::QueuedSignal signal = std::move(_impl->signalQueue.front());
        _impl->signalQueue.pop_front();
        valid = _impl->processSignal(signal, dispatchError) && valid;
        ++processed;
    }
    if (!_impl->signalQueue.empty()) {
        _impl->signalQueue.clear();
        dispatchError = "Signal dispatch exceeded the 1024-event reentrancy limit.";
        valid = false;
    }
    _impl->dispatchingSignals = false;
    if (!valid) {
        _impl->setLastError(
            dispatchError.empty() ? "UI Flow signal dispatch failed."
                                  : std::move(dispatchError),
            error);
        return false;
    }
    _impl->clearLastError(error);
    return true;
}

UIFlowSignalSubscription UIFlowRuntime::subscribeSignal(
    std::string signalId,
    UIFlowSignalHandler handler)
{
    if (signalId.empty() || !handler) return 0;
    const UIFlowSignalSubscription id = _impl->nextSubscription++;
    _impl->subscriptions.push_back(
        Impl::Subscription{id, std::move(signalId), std::move(handler)});
    return id;
}

bool UIFlowRuntime::unsubscribeSignal(UIFlowSignalSubscription subscription)
{
    const auto oldSize = _impl->subscriptions.size();
    _impl->subscriptions.erase(
        std::remove_if(_impl->subscriptions.begin(), _impl->subscriptions.end(),
            [subscription](const Impl::Subscription& value) {
                return value.id == subscription;
            }),
        _impl->subscriptions.end());
    return _impl->subscriptions.size() != oldSize;
}

bool UIFlowRuntime::registerAction(
    std::string actionId,
    UIFlowActionHandler handler,
    bool replace)
{
    if (actionId.empty() || !handler) return false;
    const auto found = _impl->actions.find(actionId);
    if (found != _impl->actions.end() && !replace) return false;
    _impl->actions[std::move(actionId)] = std::move(handler);
    return true;
}

bool UIFlowRuntime::unregisterAction(std::string_view actionId)
{
    return _impl->actions.erase(std::string(actionId)) != 0u;
}

UIFlowActionResult UIFlowRuntime::invokeAction(
    std::string_view actionId,
    UIFlowPayload inputs) const
{
    if (!_impl->loaded) {
        return UIFlowActionResult::failure("UI Flow is not loaded.");
    }
    const ayt::ui::UIFlowActionDefinition* definition =
        _impl->document.findAction(actionId);
    if (definition == nullptr) {
        return UIFlowActionResult::failure(
            "Unknown UI Flow Action '" + std::string(actionId) + "'.");
    }
    const auto handler = _impl->actions.find(std::string(actionId));
    if (handler == _impl->actions.end()) {
        return UIFlowActionResult::failure(
            "No handler is registered for Action '"
                + std::string(actionId) + "'.");
    }
    UIFlowPayload normalized;
    std::string validationError;
    if (!normalizePayload(
            definition->inputs, inputs, normalized,
            "Action", actionId, validationError)) {
        return UIFlowActionResult::failure(std::move(validationError));
    }
    try {
        return handler->second(UIFlowActionInvocation{
            std::string(actionId), std::move(normalized)});
    } catch (...) {
        return UIFlowActionResult::failure(
            "Action handler threw for '" + std::string(actionId) + "'.");
    }
}

void UIFlowRuntime::setGuardEvaluator(UIFlowGuardEvaluator evaluator)
{
    _impl->guardEvaluator = std::move(evaluator);
}

void UIFlowRuntime::setGraphRequestHandler(UIFlowGraphRequestHandler handler)
{
    _impl->graphHandler = std::move(handler);
}

std::string_view UIFlowRuntime::activeState(
    std::string_view regionId) const noexcept
{
    const auto found = _impl->regionStates.find(std::string(regionId));
    return found == _impl->regionStates.end()
        ? std::string_view{}
        : std::string_view(found->second);
}

const std::vector<UIFlowMountedScreen>& UIFlowRuntime::mountedScreens()
    const noexcept
{
    return _impl->mounted;
}

std::string_view UIFlowRuntime::lastError() const noexcept
{
    return _impl->lastError;
}

} // namespace ayt::app
