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

class ScopedBooleanFlag
{
public:
    explicit ScopedBooleanFlag(bool& value) noexcept : _value(value)
    {
        _value = true;
    }

    ~ScopedBooleanFlag() { _value = false; }

    ScopedBooleanFlag(const ScopedBooleanFlag&) = delete;
    ScopedBooleanFlag& operator=(const ScopedBooleanFlag&) = delete;

private:
    bool& _value;
};

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

UIFlowGraphStartResult UIFlowGraphStartResult::completed()
{
    return {};
}

UIFlowGraphStartResult UIFlowGraphStartResult::running()
{
    return UIFlowGraphStartResult{UIFlowGraphStartState::Running, {}};
}

UIFlowGraphStartResult UIFlowGraphStartResult::rejected(std::string message)
{
    return UIFlowGraphStartResult{
        UIFlowGraphStartState::Rejected, std::move(message)};
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

    struct DocumentValidator
    {
        UIFlowDocumentValidatorToken token = 0;
        UIFlowDocumentValidator callback;
    };

    struct PendingGraphPipeline
    {
        std::string regionId;
        std::string transitionId;
        ayt::ui::UIFlowInterruptPolicy interruptPolicy =
            ayt::ui::UIFlowInterruptPolicy::Queue;
        std::vector<UIFlowGraphRequest> requests;
        std::size_t nextRequest = 0;
        UIFlowGraphExecutionId executionId = 0;
    };

    struct DeferredTransition
    {
        std::string regionId;
        std::string transitionId;
        QueuedSignal signal;
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

    bool validateCandidateDocument(
        const ayt::ui::UIFlowDocument& candidate,
        std::string& error) const
    {
        std::vector<UIFlowDocumentValidator> callbacks;
        callbacks.reserve(documentValidators.size());
        for (const DocumentValidator& validator : documentValidators) {
            callbacks.push_back(validator.callback);
        }
        for (const UIFlowDocumentValidator& callback : callbacks) {
            std::string validationError;
            try {
                if (callback(candidate, validationError)) continue;
            } catch (const std::exception& exception) {
                validationError =
                    "UI Flow document validator threw an exception: "
                    + std::string(exception.what());
            } catch (...) {
                validationError =
                    "UI Flow document validator threw an exception.";
            }
            error = validationError.empty()
                ? "UI Flow document was rejected by a runtime bridge."
                : std::move(validationError);
            return false;
        }
        error.clear();
        return true;
    }

    void appendTrace(std::string category, std::string id, std::string detail)
    {
        constexpr std::size_t maxTraceEntries = 512;
        if (traces.size() == maxTraceEntries) traces.erase(traces.begin());
        traces.push_back(UIFlowRuntimeTrace{
            nextTraceSerial++, std::move(category), std::move(id),
            std::move(detail)});
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

    static bool sameValue(
        const ayt::ui::UIFlowValue& lhs,
        const ayt::ui::UIFlowValue& rhs)
    {
        if (lhs.data.index() != rhs.data.index()) return false;
        if (const auto* value = std::get_if<std::monostate>(&lhs.data)) {
            (void)value;
            return true;
        }
        if (const auto* value = std::get_if<bool>(&lhs.data)) {
            return *value == std::get<bool>(rhs.data);
        }
        if (const auto* value = std::get_if<std::int64_t>(&lhs.data)) {
            return *value == std::get<std::int64_t>(rhs.data);
        }
        if (const auto* value = std::get_if<double>(&lhs.data)) {
            return *value == std::get<double>(rhs.data);
        }
        if (const auto* value = std::get_if<std::string>(&lhs.data)) {
            return *value == std::get<std::string>(rhs.data);
        }
        if (const auto* values =
                std::get_if<ayt::ui::UIFlowValue::Array>(&lhs.data)) {
            const auto& other =
                std::get<ayt::ui::UIFlowValue::Array>(rhs.data);
            if (values->size() != other.size()) return false;
            for (std::size_t index = 0; index < values->size(); ++index) {
                if (!sameValue((*values)[index], other[index])) return false;
            }
            return true;
        }
        const auto& values =
            std::get<ayt::ui::UIFlowValue::Object>(lhs.data);
        const auto& other =
            std::get<ayt::ui::UIFlowValue::Object>(rhs.data);
        if (values.size() != other.size()) return false;
        auto left = values.begin();
        auto right = other.begin();
        for (; left != values.end(); ++left, ++right) {
            if (left->first != right->first
                || !sameValue(left->second, right->second)) return false;
        }
        return true;
    }

    static bool samePayload(
        const UIFlowPayload& lhs,
        const UIFlowPayload& rhs)
    {
        if (lhs.size() != rhs.size()) return false;
        for (const auto& pair : lhs) {
            const auto found = rhs.find(pair.first);
            if (found == rhs.end() || !sameValue(pair.second, found->second)) {
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
            && mounted.layoutAsset == desired.screen->layoutAsset
            && mounted.layerId == desired.layer->id
            && mounted.scope == desired.screen->scope
            && mounted.scopeKey == desired.scopeKey
            && mounted.inputPolicy == desired.layer->inputPolicy
            && mounted.blocksLowerInput == desired.layer->blocksLowerInput
            && mounted.enterAnimation == desired.screen->enterAnimation
            && mounted.exitAnimation == desired.screen->exitAnimation
            && mounted.events == desired.screen->events
            && samePayload(mounted.parameters, desired.screen->parameters);
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
                request.enterAnimation = item.screen->enterAnimation;
                request.exitAnimation = item.screen->exitAnimation;
                request.parameters = item.screen->parameters;
                request.events = item.screen->events;
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
                appendTrace("Screen", item.screen->id,
                    "mounted on " + item.layer->id + " / " + item.slot->id);
            }
            value.activationSerial = item.activation->serial;
            value.screenId = item.screen->id;
            value.layoutAsset = item.screen->layoutAsset;
            value.layerId = item.layer->id;
            value.slotId = item.slot->id;
            value.contextId = item.activation->contextId;
            value.scope = item.screen->scope;
            value.scopeKey = item.scopeKey;
            value.layerOrder = item.layer->order;
            value.orderInLayer = item.orderInLayer;
            value.inputPolicy = item.layer->inputPolicy;
            value.blocksLowerInput = item.layer->blocksLowerInput;
            value.enterAnimation = item.screen->enterAnimation;
            value.exitAnimation = item.screen->exitAnimation;
            value.parameters = item.screen->parameters;
            value.events = item.screen->events;
            next.push_back(std::move(value));
        }

        for (std::size_t index = 0; index < mounted.size(); ++index) {
            if (!reused[index]) {
                appendTrace("Screen", mounted[index].screenId, "unmounted");
                host.unmountScreen(mounted[index].mountId);
            }
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

    const ayt::ui::UIFlowTransitionDefinition* transition(
        std::string_view id) const
    {
        const auto found = std::find_if(document.transitions.begin(),
            document.transitions.end(), [id](const auto& value) {
                return value.id == id;
            });
        return found == document.transitions.end() ? nullptr : &*found;
    }

    bool dispatchPendingPipeline(std::string_view regionId, std::string& error)
    {
        auto found = pendingPipelines.find(std::string(regionId));
        if (found == pendingPipelines.end()) return true;
        PendingGraphPipeline& pipeline = found->second;
        while (pipeline.nextRequest < pipeline.requests.size()) {
            const UIFlowGraphRequest& graph =
                pipeline.requests[pipeline.nextRequest];
            const UIFlowGraphExecutionId executionId = nextGraphExecution++;
            UIFlowGraphStartResult result;
            try {
                result = asyncGraphHandler(UIFlowGraphExecutionRequest{
                    executionId, graph});
            } catch (const std::exception& exception) {
                result = UIFlowGraphStartResult::rejected(exception.what());
            } catch (...) {
                result = UIFlowGraphStartResult::rejected(
                    "graph executor threw an unknown exception");
            }
            appendTrace("Graph", graph.graphId,
                result.state == UIFlowGraphStartState::Running
                    ? "running" : result.state == UIFlowGraphStartState::Completed
                        ? "completed" : "rejected");
            if (result.state == UIFlowGraphStartState::Rejected) {
                error = "Graph execution rejected for '" + graph.graphId
                    + "': " + (result.message.empty()
                        ? std::string("executor rejected the request")
                        : result.message);
                const std::string failedRegion = pipeline.regionId;
                pendingPipelines.erase(found);
                deferredTransitions.erase(std::remove_if(
                    deferredTransitions.begin(), deferredTransitions.end(),
                    [&failedRegion](const DeferredTransition& value) {
                        return value.regionId == failedRegion;
                    }), deferredTransitions.end());
                return false;
            }
            ++pipeline.nextRequest;
            if (result.state == UIFlowGraphStartState::Running) {
                pipeline.executionId = executionId;
                executionRegions[executionId] = pipeline.regionId;
                return true;
            }
        }
        appendTrace("Transition", pipeline.transitionId, "graph pipeline completed");
        pendingPipelines.erase(found);
        return drainDeferredTransition(regionId, error);
    }

    bool drainDeferredTransition(std::string_view regionId, std::string& error)
    {
        const auto found = std::find_if(deferredTransitions.begin(),
            deferredTransitions.end(), [regionId](const auto& value) {
                return value.regionId == regionId;
            });
        if (found == deferredTransitions.end()) return true;
        DeferredTransition deferred = std::move(*found);
        deferredTransitions.erase(found);
        const auto* definition = transition(deferred.transitionId);
        const auto* region = document.findRegion(deferred.regionId);
        if (definition == nullptr || region == nullptr) {
            error = "Deferred UI Flow transition no longer exists.";
            return false;
        }
        return applyTransitionNow(*region, *definition, deferred.signal, error);
    }

    bool beginGraphPipeline(
        const UIFlowRegionDefinition& region,
        const UIFlowTransitionDefinition& transition,
        const QueuedSignal& signal,
        const std::vector<const UIFlowStateDefinition*>& oldLineage,
        const std::vector<const UIFlowStateDefinition*>& newLineage,
        std::size_t common,
        std::string& error)
    {
        std::vector<UIFlowGraphRequest> requests;
        auto add = [&](std::string_view graphId) {
            if (graphId.empty()) return;
            UIFlowGraphRequest request;
            request.graphId = std::string(graphId);
            request.signalId = signal.id;
            request.payload = signal.payload;
            request.regionId = region.id;
            request.transitionId = transition.id;
            requests.push_back(std::move(request));
        };
        for (std::size_t index = oldLineage.size(); index > common; --index) {
            add(oldLineage[index - 1]->exitGraph);
        }
        add(transition.actionGraph);
        for (std::size_t index = common; index < newLineage.size(); ++index) {
            add(newLineage[index]->enterGraph);
        }
        if (requests.empty()) return true;
        if (!asyncGraphHandler) {
            for (const UIFlowGraphRequest& request : requests) {
                if (!requestGraph(request.graphId, &signal,
                                  region.id, transition.id, error)) {
                    return false;
                }
            }
            return true;
        }
        PendingGraphPipeline pipeline;
        pipeline.regionId = region.id;
        pipeline.transitionId = transition.id;
        pipeline.interruptPolicy = transition.interruptPolicy;
        pipeline.requests = std::move(requests);
        pendingPipelines[region.id] = std::move(pipeline);
        return dispatchPendingPipeline(region.id, error);
    }

    bool scheduleTransition(
        const UIFlowRegionDefinition& region,
        const UIFlowTransitionDefinition& transition,
        const QueuedSignal& signal,
        std::string& error)
    {
        const auto running = pendingPipelines.find(region.id);
        if (running == pendingPipelines.end()) {
            return applyTransitionNow(region, transition, signal, error);
        }
        switch (transition.interruptPolicy) {
        case ayt::ui::UIFlowInterruptPolicy::IgnoreIfRunning:
            appendTrace("Transition", transition.id, "ignored while region is running");
            return true;
        case ayt::ui::UIFlowInterruptPolicy::Queue:
            deferredTransitions.push_back(
                DeferredTransition{region.id, transition.id, signal});
            appendTrace("Transition", transition.id, "queued");
            return true;
        case ayt::ui::UIFlowInterruptPolicy::Coalesce: {
            auto queued = std::find_if(deferredTransitions.rbegin(),
                deferredTransitions.rend(), [&](const auto& value) {
                    return value.regionId == region.id;
                });
            if (queued == deferredTransitions.rend()) {
                deferredTransitions.push_back(
                    DeferredTransition{region.id, transition.id, signal});
            } else {
                queued->transitionId = transition.id;
                queued->signal = signal;
            }
            appendTrace("Transition", transition.id, "coalesced");
            return true;
        }
        case ayt::ui::UIFlowInterruptPolicy::CancelPrevious:
        case ayt::ui::UIFlowInterruptPolicy::ReversePrevious: {
            const UIFlowGraphExecutionId executionId = running->second.executionId;
            if (graphInterruptHandler && executionId != 0) {
                try {
                    graphInterruptHandler(executionId,
                        transition.interruptPolicy
                                == ayt::ui::UIFlowInterruptPolicy::ReversePrevious
                            ? UIFlowGraphInterrupt::Reverse
                            : UIFlowGraphInterrupt::Cancel);
                } catch (const std::exception& exception) {
                    error = "Graph interrupt handler threw: ";
                    error += exception.what();
                    return false;
                } catch (...) {
                    error = "Graph interrupt handler threw an unknown exception.";
                    return false;
                }
            }
            executionRegions.erase(executionId);
            appendTrace("Graph", std::to_string(executionId),
                transition.interruptPolicy
                        == ayt::ui::UIFlowInterruptPolicy::ReversePrevious
                    ? "reverse requested" : "cancel requested");
            pendingPipelines.erase(running);
            deferredTransitions.erase(std::remove_if(
                deferredTransitions.begin(), deferredTransitions.end(),
                [&](const auto& value) { return value.regionId == region.id; }),
                deferredTransitions.end());
            return applyTransitionNow(region, transition, signal, error);
        }
        }
        return true;
    }

    bool applyTransitionNow(
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

        appendTrace("Transition", transition.id,
            oldLeaf + " -> " + newLeaf);
        return beginGraphPipeline(region, transition, signal,
                                  oldLineage, newLineage, common, error);
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
                && !scheduleTransition(region, *selected, signal, error)) {
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

    bool emitSignal(std::string_view signalId, UIFlowPayload payload,
                    std::string* error)
    {
        if (!started) {
            setLastError("UI Flow is not started.", error);
            return false;
        }
        const ayt::ui::UIFlowSignalDefinition* definition =
            document.findSignal(signalId);
        if (definition == nullptr) {
            setLastError(
                "Unknown UI Flow Signal '" + std::string(signalId) + "'.",
                error);
            return false;
        }
        UIFlowPayload normalized;
        std::string payloadError;
        if (!normalizePayload(
                definition->payload, payload, normalized,
                "Signal", signalId, payloadError)) {
            setLastError(std::move(payloadError), error);
            return false;
        }
        signalQueue.push_back(
            QueuedSignal{std::string(signalId), std::move(normalized)});
        appendTrace("Signal", std::string(signalId), "accepted");
        if (!replaying) {
            replayLog.push_back(UIFlowReplaySignal{
                std::string(signalId), signalQueue.back().payload});
        }
        if (dispatchingSignals) {
            clearLastError(error);
            return true;
        }

        dispatchingSignals = true;
        bool valid = true;
        std::string dispatchError;
        std::size_t processed = 0;
        while (!signalQueue.empty() && processed < 1024u) {
            QueuedSignal signal = std::move(signalQueue.front());
            signalQueue.pop_front();
            valid = processSignal(signal, dispatchError) && valid;
            ++processed;
        }
        if (!signalQueue.empty()) {
            signalQueue.clear();
            dispatchError =
                "Signal dispatch exceeded the 1024-event reentrancy limit.";
            valid = false;
        }
        dispatchingSignals = false;
        if (!valid) {
            setLastError(
                dispatchError.empty() ? "UI Flow signal dispatch failed."
                                      : std::move(dispatchError),
                error);
            return false;
        }
        clearLastError(error);
        return true;
    }

    void unloadDocument() noexcept
    {
        if (graphInterruptHandler) {
            for (const auto& value : pendingPipelines) {
                if (value.second.executionId != 0) {
                    try {
                        graphInterruptHandler(
                            value.second.executionId,
                            UIFlowGraphInterrupt::Cancel);
                    } catch (...) {
                    }
                }
            }
        }
        unmountAll();
        document = {};
        activeContexts.clear();
        restoreFloors.clear();
        regionStates.clear();
        scopeKeys.clear();
        signalQueue.clear();
        pendingPipelines.clear();
        executionRegions.clear();
        deferredTransitions.clear();
        loaded = false;
        started = false;
        dispatchingSignals = false;
        lastError.clear();
        activeEntryId.clear();
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
    std::vector<DocumentValidator> documentValidators;
    std::unordered_map<std::string, UIFlowActionHandler> actions;
    UIFlowGuardEvaluator guardEvaluator;
    UIFlowGraphRequestHandler graphHandler;
    UIFlowAsyncGraphRequestHandler asyncGraphHandler;
    UIFlowGraphInterruptHandler graphInterruptHandler;
    UIFlowApplicationCommandHandler applicationCommandHandler;
    std::unordered_map<std::string, PendingGraphPipeline> pendingPipelines;
    std::unordered_map<UIFlowGraphExecutionId, std::string> executionRegions;
    std::deque<DeferredTransition> deferredTransitions;
    std::vector<UIFlowRuntimeTrace> traces;
    std::vector<UIFlowReplaySignal> replayLog;
    std::uint64_t nextContextHandle = 1;
    std::uint64_t nextActivationSerial = 1;
    std::uint64_t nextMountId = 1;
    std::uint64_t nextSubscription = 1;
    std::uint64_t nextDocumentValidator = 1;
    std::uint64_t nextGraphExecution = 1;
    std::uint64_t nextTraceSerial = 1;
    bool dispatchingSignals = false;
    bool replaying = false;
    bool documentMutationInProgress = false;
    std::string lastError;
    std::string activeEntryId;
};

UIFlowRuntime::UIFlowRuntime(IUIFlowScreenHost& screenHost)
    : _impl(std::make_unique<Impl>(screenHost))
{
    Impl* impl = _impl.get();
    screenHost.setSignalEmitter(
        [impl](std::string_view signalId, UIFlowPayload payload,
               std::string* error) {
            if (impl->document.findSignal(signalId) != nullptr) {
                return impl->emitSignal(signalId, std::move(payload), error);
            }
            if (!impl->applicationCommandHandler) {
                impl->setLastError(
                    "Unknown UI Flow Signal or application command '"
                        + std::string(signalId) + "'.",
                    error);
                return false;
            }
            return impl->applicationCommandHandler(
                signalId, std::move(payload), error);
        });
}

UIFlowRuntime::~UIFlowRuntime()
{
    if (_impl != nullptr) {
        _impl->host.setSignalEmitter({});
        unload();
    }
}

UIFlowRuntime::UIFlowRuntime(UIFlowRuntime&&) noexcept = default;
UIFlowRuntime& UIFlowRuntime::operator=(UIFlowRuntime&& other) noexcept
{
    if (this == &other) return *this;
    if (_impl != nullptr) {
        _impl->host.setSignalEmitter({});
        unload();
    }
    _impl = std::move(other._impl);
    return *this;
}

bool UIFlowRuntime::load(
    ayt::ui::UIFlowDocument document,
    std::string* error)
{
    if (_impl->documentMutationInProgress) {
        _impl->setLastError(
            "UI Flow document mutation is already in progress.", error);
        return false;
    }
    ScopedBooleanFlag mutationGuard(_impl->documentMutationInProgress);
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
    std::string validationError;
    if (!_impl->validateCandidateDocument(document, validationError)) {
        _impl->setLastError(validationError, error);
        _impl->appendTrace(
            "Load", document.id, "rejected: " + validationError);
        return false;
    }
    _impl->unloadDocument();
    _impl->document = std::move(document);
    _impl->loaded = true;
    _impl->scopeKeys[UIFlowScope::Application] = "application";
    _impl->clearLastError(error);
    return true;
}

bool UIFlowRuntime::reload(
    ayt::ui::UIFlowDocument document,
    std::string* error)
{
    if (!_impl->loaded) return load(std::move(document), error);
    if (_impl->documentMutationInProgress) {
        _impl->setLastError(
            "UI Flow document mutation is already in progress.", error);
        return false;
    }
    ScopedBooleanFlag mutationGuard(_impl->documentMutationInProgress);
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (!ayt::ui::validateUIFlow(document, &diagnostics)) {
        const std::string message = diagnostics.empty()
            ? "UI Flow reload validation failed."
            : (diagnostics.front().path.empty() ? std::string{}
                                                : diagnostics.front().path + ": ")
                + diagnostics.front().message;
        _impl->setLastError(message, error);
        _impl->appendTrace("Reload", _impl->document.id, "rejected: " + message);
        return false;
    }
    if (!_impl->pendingPipelines.empty()) {
        _impl->setLastError(
            "UI Flow cannot reload while an asynchronous graph is running.",
            error);
        return false;
    }
    std::string validationError;
    if (!_impl->validateCandidateDocument(document, validationError)) {
        _impl->setLastError(validationError, error);
        _impl->appendTrace(
            "Reload", document.id, "rejected: " + validationError);
        return false;
    }

    const auto oldDocument = _impl->document;
    const auto oldContexts = _impl->activeContexts;
    const auto oldFloors = _impl->restoreFloors;
    const auto oldStates = _impl->regionStates;
    const std::string oldEntryId = _impl->activeEntryId;
    const std::uint64_t oldNextHandle = _impl->nextContextHandle;
    const std::uint64_t oldNextSerial = _impl->nextActivationSerial;

    _impl->document = std::move(document);
    _impl->activeContexts.erase(std::remove_if(
        _impl->activeContexts.begin(), _impl->activeContexts.end(),
        [&](const Impl::ActiveContext& value) {
            return !value.manual || _impl->document.findContext(value.contextId) == nullptr;
        }), _impl->activeContexts.end());
    for (Impl::ActiveContext& value : _impl->activeContexts) {
        value.priority = _impl->document.findContext(value.contextId)->priority;
    }
    std::string nextEntryId = oldEntryId;
    const ayt::ui::UIFlowEntryDefinition* nextEntry = nextEntryId.empty()
        ? nullptr : _impl->document.findEntry(nextEntryId);
    if (_impl->started && nextEntry == nullptr) {
        nextEntryId = _impl->document.defaultEntry;
        nextEntry = nextEntryId.empty()
            ? nullptr : _impl->document.findEntry(nextEntryId);
    }
    if (_impl->started && nextEntry != nullptr) {
        for (const std::string& contextId : nextEntry->contexts) {
            _impl->addContext(
                contextId,
                UIFlowScopeBinding{UIFlowScope::Application, "application"},
                "entry:" + nextEntry->id,
                false);
        }
    }
    _impl->regionStates.clear();
    if (_impl->started) {
        for (const ayt::ui::UIFlowRegionDefinition& region
             : _impl->document.regions) {
            std::string leaf = descendInitialState(region, region.initialState);
            const auto preserved = oldStates.find(region.id);
            if (preserved != oldStates.end()
                && findState(region, preserved->second) != nullptr) {
                leaf = preserved->second;
            }
            _impl->regionStates[region.id] = leaf;
            _impl->addStateContexts(region, leaf);
        }
    }
    for (auto it = _impl->restoreFloors.begin();
         it != _impl->restoreFloors.end();) {
        if (_impl->document.findSlot(it->first) == nullptr) {
            it = _impl->restoreFloors.erase(it);
        } else {
            ++it;
        }
    }

    std::string reconcileError;
    if (!_impl->reconcile(reconcileError)) {
        _impl->document = oldDocument;
        _impl->activeContexts = oldContexts;
        _impl->restoreFloors = oldFloors;
        _impl->regionStates = oldStates;
        _impl->activeEntryId = oldEntryId;
        _impl->nextContextHandle = oldNextHandle;
        _impl->nextActivationSerial = oldNextSerial;
        std::string ignored;
        (void)_impl->reconcile(ignored);
        _impl->setLastError(std::move(reconcileError), error);
        _impl->appendTrace("Reload", oldDocument.id, "rolled back");
        return false;
    }
    _impl->activeEntryId = std::move(nextEntryId);
    _impl->appendTrace("Reload", _impl->document.id,
        "committed with compatible runtime state");
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
    const std::string oldEntryId = _impl->activeEntryId;
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
        _impl->activeEntryId = oldEntryId;
        _impl->nextContextHandle = oldNextHandle;
        _impl->nextActivationSerial = oldNextSerial;
        std::string ignored;
        (void)_impl->reconcile(ignored);
        _impl->setLastError(std::move(reconcileError), error);
        return false;
    }
    _impl->started = true;
    _impl->activeEntryId = entryId;
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
    if (_impl->documentMutationInProgress) return;
    _impl->unloadDocument();
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
    return _impl->emitSignal(signalId, std::move(payload), error);
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

bool UIFlowRuntime::registerApplicationCommandHandler(
    UIFlowApplicationCommandHandler handler)
{
    if (!handler || _impl->applicationCommandHandler) return false;
    _impl->applicationCommandHandler = std::move(handler);
    return true;
}

void UIFlowRuntime::unregisterApplicationCommandHandler() noexcept
{
    _impl->applicationCommandHandler = {};
}

bool UIFlowRuntime::requestApplicationCommand(
    std::string_view commandId,
    UIFlowPayload payload,
    std::string* error)
{
    if (commandId.empty()) {
        _impl->setLastError("Application command id must not be empty.", error);
        return false;
    }
    if (!_impl->applicationCommandHandler) {
        _impl->setLastError(
            "No application command router is installed for '"
                + std::string(commandId) + "'.",
            error);
        return false;
    }
    if (!_impl->applicationCommandHandler(
            commandId, std::move(payload), error)) {
        if (error != nullptr && !error->empty()) {
            _impl->lastError = *error;
        } else if (_impl->lastError.empty()) {
            _impl->setLastError(
                "Application command '" + std::string(commandId)
                    + "' was rejected.",
                error);
        }
        return false;
    }
    _impl->clearLastError(error);
    return true;
}

UIFlowDocumentValidatorToken UIFlowRuntime::addDocumentValidator(
    UIFlowDocumentValidator validator)
{
    if (!validator) return 0;
    while (_impl->nextDocumentValidator == 0
        || std::any_of(_impl->documentValidators.begin(),
            _impl->documentValidators.end(),
            [&](const Impl::DocumentValidator& value) {
                return value.token == _impl->nextDocumentValidator;
            })) {
        ++_impl->nextDocumentValidator;
    }
    const UIFlowDocumentValidatorToken token =
        _impl->nextDocumentValidator++;
    _impl->documentValidators.push_back(
        Impl::DocumentValidator{token, std::move(validator)});
    return token;
}

bool UIFlowRuntime::removeDocumentValidator(
    UIFlowDocumentValidatorToken token) noexcept
{
    const auto oldSize = _impl->documentValidators.size();
    _impl->documentValidators.erase(
        std::remove_if(_impl->documentValidators.begin(),
            _impl->documentValidators.end(),
            [token](const Impl::DocumentValidator& value) {
                return value.token == token;
            }),
        _impl->documentValidators.end());
    return _impl->documentValidators.size() != oldSize;
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

void UIFlowRuntime::setAsyncGraphRequestHandler(
    UIFlowAsyncGraphRequestHandler handler,
    UIFlowGraphInterruptHandler interruptHandler)
{
    _impl->asyncGraphHandler = std::move(handler);
    _impl->graphInterruptHandler = std::move(interruptHandler);
}

bool UIFlowRuntime::completeGraphExecution(
    UIFlowGraphExecutionId executionId,
    bool succeeded,
    std::string message,
    std::string* error)
{
    const auto regionFound = _impl->executionRegions.find(executionId);
    if (regionFound == _impl->executionRegions.end()) {
        _impl->setLastError("Unknown UI Flow graph execution.", error);
        return false;
    }
    const std::string regionId = regionFound->second;
    _impl->executionRegions.erase(regionFound);
    const auto pipeline = _impl->pendingPipelines.find(regionId);
    if (pipeline == _impl->pendingPipelines.end()
        || pipeline->second.executionId != executionId) {
        _impl->setLastError("UI Flow graph execution is no longer active.", error);
        return false;
    }
    pipeline->second.executionId = 0;
    if (!succeeded) {
        _impl->appendTrace("Graph", std::to_string(executionId),
            "failed: " + message);
        _impl->pendingPipelines.erase(pipeline);
        std::string drainError;
        if (!_impl->drainDeferredTransition(regionId, drainError)) {
            _impl->setLastError(std::move(drainError), error);
            return false;
        }
        _impl->setLastError(message.empty()
            ? "UI Flow graph execution failed." : std::move(message), error);
        return false;
    }
    _impl->appendTrace("Graph", std::to_string(executionId), "completed");
    std::string dispatchError;
    if (!_impl->dispatchPendingPipeline(regionId, dispatchError)) {
        _impl->setLastError(std::move(dispatchError), error);
        return false;
    }
    _impl->clearLastError(error);
    return true;
}

bool UIFlowRuntime::hasPendingGraphExecution(
    std::string_view regionId) const noexcept
{
    if (regionId.empty()) return !_impl->pendingPipelines.empty();
    return _impl->pendingPipelines.find(std::string(regionId))
        != _impl->pendingPipelines.end();
}

const std::vector<UIFlowRuntimeTrace>& UIFlowRuntime::trace() const noexcept
{
    return _impl->traces;
}

const std::vector<UIFlowReplaySignal>&
UIFlowRuntime::replaySignals() const noexcept
{
    return _impl->replayLog;
}

void UIFlowRuntime::clearTrace() noexcept
{
    _impl->traces.clear();
}

void UIFlowRuntime::clearReplay() noexcept
{
    _impl->replayLog.clear();
}

bool UIFlowRuntime::replay(
    const std::vector<UIFlowReplaySignal>& signals,
    std::string* error)
{
    if (!_impl->started || _impl->dispatchingSignals) {
        _impl->setLastError(!_impl->started
            ? "UI Flow is not started."
            : "UI Flow cannot replay during signal dispatch.", error);
        return false;
    }
    _impl->replaying = true;
    bool accepted = true;
    std::string replayError;
    for (const UIFlowReplaySignal& signal : signals) {
        if (!emitSignal(signal.signalId, signal.payload, &replayError)) {
            accepted = false;
            break;
        }
    }
    _impl->replaying = false;
    if (!accepted) {
        _impl->setLastError(std::move(replayError), error);
        return false;
    }
    _impl->appendTrace("Replay", std::to_string(signals.size()),
        "signals completed");
    _impl->clearLastError(error);
    return true;
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
