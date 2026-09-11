#include <AYApplication/UIFlowGraphExecutor.h>

#include <algorithm>
#include <cmath>
#include <deque>
#include <exception>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ayt::app
{
namespace
{

const ayt::ui::UIFlowNodeDefinition* findNode(
    const ayt::ui::UIFlowGraphDefinition& graph,
    std::string_view id)
{
    const auto found = std::find_if(graph.nodes.begin(), graph.nodes.end(),
        [id](const auto& node) { return node.id == id; });
    return found == graph.nodes.end() ? nullptr : &*found;
}

std::string diagnosticMessage(
    const std::vector<ayt::ui::UIFlowDiagnostic>& diagnostics)
{
    if (diagnostics.empty()) return "Graph node contract is invalid.";
    return diagnostics.front().path + ": " + diagnostics.front().message;
}

bool valueMatchesType(const ayt::ui::UIFlowValue& value,
                      ayt::ui::UIFlowValueType type)
{
    switch (type) {
    case ayt::ui::UIFlowValueType::Boolean:
        return std::holds_alternative<bool>(value.data);
    case ayt::ui::UIFlowValueType::Integer:
        return std::holds_alternative<std::int64_t>(value.data);
    case ayt::ui::UIFlowValueType::Number:
        return std::holds_alternative<std::int64_t>(value.data)
            || std::holds_alternative<double>(value.data);
    case ayt::ui::UIFlowValueType::String:
    case ayt::ui::UIFlowValueType::Entity:
    case ayt::ui::UIFlowValueType::Asset:
        return std::holds_alternative<std::string>(value.data);
    }
    return false;
}

} // namespace

UIFlowGraphNodeResult UIFlowGraphNodeResult::completed(
    std::string output, UIFlowPayload values)
{
    UIFlowGraphNodeResult result;
    result.flowOutput = std::move(output);
    result.outputs = std::move(values);
    return result;
}

UIFlowGraphNodeResult UIFlowGraphNodeResult::running(
    double timeout, UIFlowGraphNodeCancellationHandler cancellation)
{
    UIFlowGraphNodeResult result;
    result.state = UIFlowGraphNodeState::Running;
    result.flowOutput.clear();
    result.timeoutSeconds = std::isfinite(timeout) && timeout > 0.0
        ? timeout : 0.0;
    result.onCancel = std::move(cancellation);
    return result;
}

UIFlowGraphNodeResult UIFlowGraphNodeResult::failure(std::string message)
{
    UIFlowGraphNodeResult result;
    result.state = UIFlowGraphNodeState::Failed;
    result.flowOutput.clear();
    result.message = std::move(message);
    return result;
}

class UIFlowGraphExecutor::Impl
{
public:
    struct PendingNode
    {
        UIFlowGraphNodeExecutionId executionId = 0;
        std::string nodeId;
        double timeoutSeconds = 0.0;
        double elapsedSeconds = 0.0;
        UIFlowGraphNodeCancellationHandler onCancel;
    };

    struct Execution
    {
        UIFlowGraphExecutionRequest request;
        const ayt::ui::UIFlowGraphDefinition* graph = nullptr;
        std::deque<std::string> ready;
        std::unordered_set<std::string> scheduled;
        std::unordered_set<std::string> completed;
        std::unordered_map<std::string, std::unordered_set<std::string>>
            receivedExecutionInputs;
        std::map<std::string, UIFlowPayload, std::less<>> nodeOutputs;
        std::map<UIFlowGraphNodeExecutionId, PendingNode> pending;
    };

    enum class DriveState
    {
        Completed,
        Running,
        Failed,
    };

    struct DriveResult
    {
        DriveState state = DriveState::Completed;
        std::string message;
    };

    bool executionLink(
        const ayt::ui::UIFlowGraphDefinition& graph,
        const ayt::ui::UIFlowLinkDefinition& link) const
    {
        const auto* source = findNode(graph, link.fromNode);
        const auto* target = findNode(graph, link.toNode);
        if (source == nullptr || target == nullptr) return false;
        const auto* sourceType = registry.find(source->type);
        const auto* targetType = registry.find(target->type);
        if (sourceType == nullptr || targetType == nullptr) return false;
        const auto* sourcePin = ayt::ui::findUIFlowGraphPin(
            *sourceType, link.fromPin,
            ayt::ui::UIFlowGraphPinDirection::Output);
        const auto* targetPin = ayt::ui::findUIFlowGraphPin(
            *targetType, link.toPin,
            ayt::ui::UIFlowGraphPinDirection::Input);
        return sourcePin != nullptr && targetPin != nullptr
            && sourcePin->kind == ayt::ui::UIFlowGraphPinKind::Execution
            && targetPin->kind == ayt::ui::UIFlowGraphPinKind::Execution;
    }

    bool valueLink(
        const ayt::ui::UIFlowGraphDefinition& graph,
        const ayt::ui::UIFlowLinkDefinition& link) const
    {
        const auto* source = findNode(graph, link.fromNode);
        const auto* target = findNode(graph, link.toNode);
        if (source == nullptr || target == nullptr) return false;
        const auto* sourceType = registry.find(source->type);
        const auto* targetType = registry.find(target->type);
        if (sourceType == nullptr || targetType == nullptr) return false;
        const auto* sourcePin = ayt::ui::findUIFlowGraphPin(
            *sourceType, link.fromPin,
            ayt::ui::UIFlowGraphPinDirection::Output);
        const auto* targetPin = ayt::ui::findUIFlowGraphPin(
            *targetType, link.toPin,
            ayt::ui::UIFlowGraphPinDirection::Input);
        return sourcePin != nullptr && targetPin != nullptr
            && sourcePin->kind == ayt::ui::UIFlowGraphPinKind::Value
            && targetPin->kind == ayt::ui::UIFlowGraphPinKind::Value;
    }

    bool hasDependencyCycle(
        const ayt::ui::UIFlowGraphDefinition& graph) const
    {
        std::unordered_map<std::string, std::size_t> indegree;
        for (const auto& node : graph.nodes) indegree[node.id] = 0u;
        for (const auto& link : graph.links) {
            if (executionLink(graph, link) || valueLink(graph, link)) {
                ++indegree[link.toNode];
            }
        }
        std::deque<std::string> ready;
        for (const auto& node : graph.nodes) {
            if (indegree[node.id] == 0u) ready.push_back(node.id);
        }
        std::size_t visited = 0u;
        while (!ready.empty()) {
            const std::string nodeId = std::move(ready.front());
            ready.pop_front();
            ++visited;
            for (const auto& link : graph.links) {
                if (link.fromNode != nodeId
                    || (!executionLink(graph, link)
                        && !valueLink(graph, link))) {
                    continue;
                }
                auto found = indegree.find(link.toNode);
                if (found != indegree.end() && --found->second == 0u) {
                    ready.push_back(found->first);
                }
            }
        }
        return visited != graph.nodes.size();
    }

    std::string validateDependencies(
        const ayt::ui::UIFlowGraphDefinition& graph) const
    {
        std::unordered_set<std::string> valueTargets;
        for (const auto& link : graph.links) {
            if (!valueLink(graph, link)) continue;
            const std::string target = link.toNode + "\n" + link.toPin;
            if (!valueTargets.insert(target).second) {
                return "Value input '" + link.toNode + "." + link.toPin
                    + "' has multiple producers.";
            }
        }
        if (hasDependencyCycle(graph)) {
            return "Graph contains an execution/value dependency cycle.";
        }
        return {};
    }

    enum class ValueDependencyState
    {
        Ready,
        Waiting,
        Invalid,
    };

    struct ValueDependencyResult
    {
        ValueDependencyState state = ValueDependencyState::Ready;
        std::string message;
    };

    ValueDependencyResult valueDependenciesFor(
        const Execution& execution,
        const ayt::ui::UIFlowNodeDefinition& node) const
    {
        for (const auto& link : execution.graph->links) {
            if (link.toNode != node.id
                || !valueLink(*execution.graph, link)) {
                continue;
            }
            if (!execution.completed.contains(link.fromNode)) {
                return {ValueDependencyState::Waiting,
                    "Node '" + node.id + "' is waiting for value producer '"
                        + link.fromNode + "'."};
            }
            const auto outputNode = execution.nodeOutputs.find(link.fromNode);
            const auto output = outputNode != execution.nodeOutputs.end()
                ? outputNode->second.find(link.fromPin)
                : UIFlowPayload::const_iterator{};
            if (outputNode == execution.nodeOutputs.end()
                || output == outputNode->second.end()) {
                return {ValueDependencyState::Invalid,
                    "Node '" + link.fromNode
                        + "' completed without linked value output '"
                        + link.fromPin + "'."};
            }
        }
        return {};
    }

    std::unordered_set<std::string> requiredExecutionInputs(
        const ayt::ui::UIFlowGraphDefinition& graph,
        std::string_view nodeId) const
    {
        std::unordered_set<std::string> result;
        for (const auto& link : graph.links) {
            if (link.toNode == nodeId && executionLink(graph, link)) {
                result.insert(link.toPin);
            }
        }
        return result;
    }

    void activateNode(Execution& execution, const std::string& nodeId,
                      std::string_view inputPin = {})
    {
        if (execution.completed.contains(nodeId)
            || execution.scheduled.contains(nodeId)) return;
        if (!inputPin.empty()) {
            execution.receivedExecutionInputs[nodeId].insert(
                std::string(inputPin));
            const auto required = requiredExecutionInputs(
                *execution.graph, nodeId);
            if (required.size() > 1u) {
                const auto& received = execution.receivedExecutionInputs[nodeId];
                const bool joined = std::all_of(
                    required.begin(), required.end(),
                    [&](const std::string& pin) {
                        return received.contains(pin);
                    });
                if (!joined) return;
            }
        }
        if (execution.scheduled.insert(nodeId).second) {
            execution.ready.push_back(nodeId);
        }
    }

    std::string unresolvedJoinMessage(const Execution& execution) const
    {
        for (const auto& node : execution.graph->nodes) {
            if (execution.scheduled.contains(node.id)
                || execution.completed.contains(node.id)) {
                continue;
            }
            const auto received = execution.receivedExecutionInputs.find(
                node.id);
            if (received == execution.receivedExecutionInputs.end()
                || received->second.empty()) {
                continue;
            }
            const auto required = requiredExecutionInputs(
                *execution.graph, node.id);
            if (required.size() > received->second.size()) {
                return "Execution join '" + node.id
                    + "' did not receive every connected input.";
            }
        }
        return {};
    }

    UIFlowPayload inputsFor(
        const Execution& execution,
        const ayt::ui::UIFlowNodeDefinition& node) const
    {
        UIFlowPayload inputs = node.properties;
        const auto* nodeType = registry.find(node.type);
        if (nodeType != nullptr) {
            for (const auto& property : nodeType->properties) {
                if (!std::holds_alternative<std::monostate>(
                        property.defaultValue.data)) {
                    inputs.try_emplace(property.id, property.defaultValue);
                }
            }
        }
        for (const auto& link : execution.graph->links) {
            if (link.toNode != node.id) continue;
            const auto* sourceNode = findNode(*execution.graph, link.fromNode);
            if (sourceNode == nullptr || nodeType == nullptr) continue;
            const auto* sourceType = registry.find(sourceNode->type);
            if (sourceType == nullptr) continue;
            const auto* sourcePin = ayt::ui::findUIFlowGraphPin(
                *sourceType, link.fromPin,
                ayt::ui::UIFlowGraphPinDirection::Output);
            const auto* targetPin = ayt::ui::findUIFlowGraphPin(
                *nodeType, link.toPin,
                ayt::ui::UIFlowGraphPinDirection::Input);
            if (sourcePin == nullptr || targetPin == nullptr
                || sourcePin->kind != ayt::ui::UIFlowGraphPinKind::Value
                || targetPin->kind != ayt::ui::UIFlowGraphPinKind::Value) {
                continue;
            }
            const auto outputNode = execution.nodeOutputs.find(link.fromNode);
            if (outputNode == execution.nodeOutputs.end()) continue;
            const auto output = outputNode->second.find(link.fromPin);
            if (output != outputNode->second.end()) {
                inputs[link.toPin] = output->second;
            }
        }
        return inputs;
    }

    void appendTrace(const Execution& execution,
                     UIFlowGraphNodeExecutionId nodeExecutionId,
                     std::string nodeId,
                     std::string detail,
                     UIFlowPayload inputs = {},
                     UIFlowPayload outputs = {})
    {
        traces.push_back({nextTraceSerial++,
            execution.request.executionId, nodeExecutionId,
            execution.request.graph.graphId, std::move(nodeId),
            std::move(detail), std::move(inputs), std::move(outputs)});
        if (traces.size() > 512u) traces.erase(traces.begin());
    }

    static std::string breakpointKey(
        std::string_view graphId, std::string_view nodeId)
    {
        return std::string(graphId) + "\n" + std::string(nodeId);
    }

    bool pauseBeforeNode(Execution& execution,
                         const ayt::ui::UIFlowNodeDefinition& node,
                         UIFlowPayload inputs)
    {
        const auto resume = resumeOnce.find(execution.request.executionId);
        const bool skipPause = resume != resumeOnce.end()
            && resume->second == node.id;
        if (skipPause) resumeOnce.erase(resume);

        const bool stepBoundary = !skipPause && pauseAfterNode.erase(
            execution.request.executionId) != 0u;
        const bool breakpoint = breakpoints.contains(breakpointKey(
            execution.request.graph.graphId, node.id));
        if (skipPause || (!pauseRequested && !stepBoundary && !breakpoint)) {
            return false;
        }
        pauseRequested = false;
        paused = UIFlowGraphDebugPause{
            execution.request.executionId,
            execution.request.graph.graphId,
            node.id,
            node.type,
            stepBoundary ? "step" : breakpoint ? "breakpoint" : "manual",
            std::move(inputs)};
        execution.ready.push_front(node.id);
        appendTrace(execution, 0u, node.id,
            "paused:" + paused->reason, paused->inputs);
        return true;
    }

    void routeCompleted(Execution& execution,
                        const std::string& nodeId,
                        UIFlowGraphNodeResult result)
    {
        execution.completed.insert(nodeId);
        execution.nodeOutputs[nodeId] = std::move(result.outputs);
        const std::string output = result.flowOutput.empty()
            ? std::string("completed") : std::move(result.flowOutput);
        for (const auto& link : execution.graph->links) {
            if (link.fromNode != nodeId || link.fromPin != output
                || !executionLink(*execution.graph, link)) {
                continue;
            }
            activateNode(execution, link.toNode, link.toPin);
        }
    }

    std::string validateCompletedResult(
        const ayt::ui::UIFlowNodeDefinition& node,
        const UIFlowGraphNodeResult& result) const
    {
        const auto* type = registry.find(node.type);
        if (type == nullptr) return "Node type disappeared during execution.";
        const std::string_view flowOutput = result.flowOutput.empty()
            ? std::string_view("completed")
            : std::string_view(result.flowOutput);
        const auto* executionPin = ayt::ui::findUIFlowGraphPin(
            *type, flowOutput, ayt::ui::UIFlowGraphPinDirection::Output);
        if (executionPin == nullptr
            || executionPin->kind != ayt::ui::UIFlowGraphPinKind::Execution) {
            return "Node '" + node.id + "' returned unknown execution output '"
                + std::string(flowOutput) + "'.";
        }
        for (const auto& [id, value] : result.outputs) {
            const auto* pin = ayt::ui::findUIFlowGraphPin(
                *type, id, ayt::ui::UIFlowGraphPinDirection::Output);
            if (pin == nullptr || pin->kind != ayt::ui::UIFlowGraphPinKind::Value) {
                return "Node '" + node.id + "' returned unknown value output '"
                    + id + "'.";
            }
            if (!valueMatchesType(value, pin->valueType)) {
                return "Node '" + node.id + "' returned the wrong type for output '"
                    + id + "'.";
            }
        }
        return {};
    }

    DriveResult finish(UIFlowGraphExecutionId executionId,
                       bool succeeded,
                       std::string message,
                       bool notify)
    {
        if (paused.has_value()
            && paused->graphExecutionId == executionId) {
            paused.reset();
        }
        resumeOnce.erase(executionId);
        pauseAfterNode.erase(executionId);
        std::vector<UIFlowGraphNodeCancellationHandler> cancellations;
        const auto found = executions.find(executionId);
        if (found != executions.end()) {
            for (auto& [nodeExecutionId, pending] : found->second.pending) {
                nodeExecutions.erase(nodeExecutionId);
                if (pending.onCancel) {
                    cancellations.push_back(std::move(pending.onCancel));
                }
            }
            executions.erase(found);
        }
        for (auto& cancel : cancellations) {
            try {
                cancel(UIFlowGraphInterrupt::Cancel);
            } catch (...) {
                // Cancellation is a teardown boundary. A faulty host callback
                // cannot keep an execution alive or escape a noexcept caller.
            }
        }
        if (notify && completionHandler) {
            completionHandler(executionId, succeeded, message);
        }
        return {succeeded ? DriveState::Completed : DriveState::Failed,
                std::move(message)};
    }

    DriveResult drive(UIFlowGraphExecutionId executionId, bool notify)
    {
        for (;;) {
            auto executionIt = executions.find(executionId);
            if (executionIt == executions.end()) {
                return {DriveState::Failed, "Graph execution was interrupted."};
            }
            Execution& execution = executionIt->second;
            while (!execution.ready.empty()
                   && execution.completed.contains(execution.ready.front())) {
                execution.ready.pop_front();
            }

            std::optional<std::string> runnable;
            std::string waitingMessage;
            const std::size_t candidateCount = execution.ready.size();
            for (std::size_t i = 0; i < candidateCount; ++i) {
                std::string candidate = std::move(execution.ready.front());
                execution.ready.pop_front();
                const auto* candidateNode = findNode(
                    *execution.graph, candidate);
                if (candidateNode == nullptr) {
                    return finish(executionId, false,
                        "Graph scheduled a missing node '" + candidate + "'.",
                        notify);
                }
                const ValueDependencyResult dependency =
                    valueDependenciesFor(execution, *candidateNode);
                if (dependency.state == ValueDependencyState::Invalid) {
                    return finish(executionId, false, dependency.message,
                                  notify);
                }
                if (dependency.state == ValueDependencyState::Waiting) {
                    if (waitingMessage.empty()) {
                        waitingMessage = dependency.message;
                    }
                    execution.ready.push_back(std::move(candidate));
                    continue;
                }
                runnable = std::move(candidate);
                break;
            }
            if (!runnable.has_value()) {
                if (!execution.pending.empty()) {
                    return {DriveState::Running, {}};
                }
                if (!execution.ready.empty()) {
                    return finish(executionId, false,
                        waitingMessage.empty()
                            ? "Graph has unresolved value dependencies."
                            : std::move(waitingMessage),
                        notify);
                }
                const std::string joinError = unresolvedJoinMessage(execution);
                if (!joinError.empty()) {
                    return finish(executionId, false, joinError, notify);
                }
                return finish(executionId, true, {}, notify);
            }

            const std::string nodeId = std::move(*runnable);
            const auto* node = findNode(*execution.graph, nodeId);
            if (node == nullptr) {
                return finish(executionId, false,
                    "Graph scheduled a missing node '" + nodeId + "'.",
                    notify);
            }
            const auto handlerIt = handlers.find(node->type);
            if (handlerIt == handlers.end()) {
                return finish(executionId, false,
                    "No executor is registered for node type '"
                        + node->type + "'.",
                    notify);
            }
            UIFlowPayload resolvedInputs = inputsFor(execution, *node);
            if (pauseBeforeNode(
                    execution, *node, resolvedInputs)) {
                return {DriveState::Running, {}};
            }
            const UIFlowGraphNodeExecutionId nodeExecutionId =
                nextNodeExecutionId++;
            UIFlowGraphNodeInvocation invocation;
            invocation.graphExecutionId = executionId;
            invocation.nodeExecutionId = nodeExecutionId;
            invocation.graph = execution.graph;
            invocation.node = node;
            invocation.request = execution.request.graph;
            invocation.inputs = std::move(resolvedInputs);
            appendTrace(execution, nodeExecutionId, nodeId, "started",
                        invocation.inputs);

            UIFlowGraphNodeResult result;
            try {
                result = handlerIt->second(invocation);
            } catch (const std::exception& exception) {
                result = UIFlowGraphNodeResult::failure(exception.what());
            } catch (...) {
                result = UIFlowGraphNodeResult::failure(
                    "Node executor threw an unknown exception.");
            }

            executionIt = executions.find(executionId);
            if (executionIt == executions.end()) {
                return {DriveState::Failed, "Graph execution was interrupted."};
            }
            Execution& resumed = executionIt->second;
            if (result.state == UIFlowGraphNodeState::Running) {
                PendingNode pending;
                pending.executionId = nodeExecutionId;
                pending.nodeId = nodeId;
                pending.timeoutSeconds = result.timeoutSeconds;
                pending.onCancel = std::move(result.onCancel);
                resumed.pending.emplace(nodeExecutionId, std::move(pending));
                nodeExecutions[nodeExecutionId] = executionId;
                appendTrace(resumed, nodeExecutionId, nodeId, "running");
                continue;
            }
            if (result.state == UIFlowGraphNodeState::Failed) {
                const std::string message = result.message.empty()
                    ? "Node '" + nodeId + "' failed." : result.message;
                appendTrace(resumed, nodeExecutionId, nodeId, "failed");
                return finish(executionId, false, message, notify);
            }
            const std::string resultError = validateCompletedResult(
                *node, result);
            if (!resultError.empty()) {
                appendTrace(resumed, nodeExecutionId, nodeId,
                            "invalid-result");
                return finish(executionId, false, resultError, notify);
            }
            appendTrace(resumed, nodeExecutionId, nodeId,
                        "completed:" + result.flowOutput,
                        {}, result.outputs);
            routeCompleted(resumed, nodeId, std::move(result));
        }
    }

    const ayt::ui::UIFlowDocument* document = nullptr;
    ayt::ui::UIFlowGraphNodeRegistry registry;
    std::unordered_map<std::string, UIFlowGraphNodeHandler> handlers;
    std::unordered_map<UIFlowGraphExecutionId, Execution> executions;
    std::unordered_map<UIFlowGraphNodeExecutionId, UIFlowGraphExecutionId>
        nodeExecutions;
    UIFlowGraphCompletionHandler completionHandler;
    std::vector<UIFlowGraphExecutorTrace> traces;
    std::unordered_set<std::string> breakpoints;
    std::optional<UIFlowGraphDebugPause> paused;
    std::unordered_map<UIFlowGraphExecutionId, std::string> resumeOnce;
    std::unordered_set<UIFlowGraphExecutionId> pauseAfterNode;
    bool pauseRequested = false;
    UIFlowGraphNodeExecutionId nextNodeExecutionId = 1;
    std::uint64_t nextTraceSerial = 1;

    void abandonExecutions(UIFlowGraphInterrupt interrupt) noexcept
    {
        std::vector<UIFlowGraphNodeCancellationHandler> cancellations;
        for (auto& [executionId, execution] : executions) {
            (void)executionId;
            for (auto& [nodeExecutionId, pending] : execution.pending) {
                (void)nodeExecutionId;
                if (pending.onCancel) {
                    cancellations.push_back(std::move(pending.onCancel));
                }
            }
        }
        nodeExecutions.clear();
        executions.clear();
        paused.reset();
        resumeOnce.clear();
        pauseAfterNode.clear();
        pauseRequested = false;
        for (auto& cancel : cancellations) {
            try {
                cancel(interrupt);
            } catch (...) {
            }
        }
    }

};

UIFlowGraphExecutor::UIFlowGraphExecutor()
    : _impl(std::make_unique<Impl>())
{
}

UIFlowGraphExecutor::~UIFlowGraphExecutor()
{
    if (_impl != nullptr) {
        _impl->abandonExecutions(UIFlowGraphInterrupt::Cancel);
    }
}
UIFlowGraphExecutor::UIFlowGraphExecutor(UIFlowGraphExecutor&&) noexcept = default;
UIFlowGraphExecutor& UIFlowGraphExecutor::operator=(
    UIFlowGraphExecutor&& other) noexcept
{
    if (this == &other) return *this;
    if (_impl != nullptr) {
        _impl->abandonExecutions(UIFlowGraphInterrupt::Cancel);
    }
    _impl = std::move(other._impl);
    return *this;
}

void UIFlowGraphExecutor::setDocument(
    const ayt::ui::UIFlowDocument* document) noexcept
{
    _impl->abandonExecutions(UIFlowGraphInterrupt::Cancel);
    _impl->document = document;
}

const ayt::ui::UIFlowDocument* UIFlowGraphExecutor::document() const noexcept
{
    return _impl->document;
}

bool UIFlowGraphExecutor::registerNodeType(
    ayt::ui::UIFlowGraphNodeTypeDefinition definition,
    UIFlowGraphNodeHandler handler,
    bool replace,
    std::string* error)
{
    if (!handler) {
        if (error != nullptr) *error = "Graph node handler must not be empty.";
        return false;
    }
    if (!_impl->executions.empty()) {
        if (error != nullptr) {
            *error = "Cannot change node types while a Graph is running.";
        }
        return false;
    }
    ayt::ui::UIFlowGraphNodeRegistry probe;
    std::string localError;
    if (!probe.registerType(definition, &localError)) {
        if (error != nullptr) *error = std::move(localError);
        return false;
    }
    const bool exists = _impl->registry.find(definition.type) != nullptr;
    if (exists && !replace) {
        if (error != nullptr) *error = "Graph node type is already registered.";
        return false;
    }
    const std::string type = definition.type;
    if (exists) (void)_impl->registry.unregisterType(type);
    if (!_impl->registry.registerType(std::move(definition), &localError)) {
        if (error != nullptr) *error = std::move(localError);
        return false;
    }
    _impl->handlers[type] = std::move(handler);
    if (error != nullptr) error->clear();
    return true;
}

bool UIFlowGraphExecutor::unregisterNodeType(std::string_view type)
{
    if (!_impl->executions.empty()) return false;
    if (!_impl->registry.unregisterType(type)) return false;
    _impl->handlers.erase(std::string(type));
    return true;
}

void UIFlowGraphExecutor::clearNodeTypes() noexcept
{
    if (!_impl->executions.empty()) return;
    _impl->registry.clear();
    _impl->handlers.clear();
}

const ayt::ui::UIFlowGraphNodeRegistry& UIFlowGraphExecutor::nodeTypes()
    const noexcept
{
    return _impl->registry;
}

UIFlowGraphStartResult UIFlowGraphExecutor::start(
    const UIFlowGraphExecutionRequest& request)
{
    if (_impl->document == nullptr) {
        return UIFlowGraphStartResult::rejected(
            "Graph executor has no UI Flow document.");
    }
    if (request.executionId == 0u
        || _impl->executions.contains(request.executionId)) {
        return UIFlowGraphStartResult::rejected(
            "Graph execution ID must be unique and non-zero.");
    }
    const auto* graph = _impl->document->findGraph(request.graph.graphId);
    if (graph == nullptr) {
        return UIFlowGraphStartResult::rejected(
            "Unknown Graph '" + request.graph.graphId + "'.");
    }
    ayt::ui::UIFlowDocument validation;
    validation.graphs.push_back(*graph);
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    if (!ayt::ui::validateUIFlowGraphNodes(
            validation, _impl->registry, &diagnostics)) {
        return UIFlowGraphStartResult::rejected(
            diagnosticMessage(diagnostics));
    }
    const std::string dependencyError = _impl->validateDependencies(*graph);
    if (!dependencyError.empty()) {
        return UIFlowGraphStartResult::rejected(dependencyError);
    }

    Impl::Execution execution;
    execution.request = request;
    execution.graph = graph;
    std::unordered_set<std::string> incoming;
    for (const auto& link : graph->links) {
        if (_impl->executionLink(*graph, link)) incoming.insert(link.toNode);
    }
    for (const auto& node : graph->nodes) {
        if (!incoming.contains(node.id)) {
            _impl->activateNode(execution, node.id);
        }
    }
    _impl->executions.emplace(request.executionId, std::move(execution));
    const Impl::DriveResult result = _impl->drive(request.executionId, false);
    if (result.state == Impl::DriveState::Running) {
        return UIFlowGraphStartResult::running();
    }
    if (result.state == Impl::DriveState::Failed) {
        return UIFlowGraphStartResult::rejected(result.message);
    }
    return UIFlowGraphStartResult::completed();
}

bool UIFlowGraphExecutor::completeNode(
    UIFlowGraphNodeExecutionId nodeExecutionId,
    UIFlowGraphNodeResult result,
    std::string* error)
{
    if (result.state == UIFlowGraphNodeState::Running) {
        if (error != nullptr) *error = "A pending node requires a final result.";
        return false;
    }
    const auto executionId = _impl->nodeExecutions.find(nodeExecutionId);
    if (executionId == _impl->nodeExecutions.end()) {
        if (error != nullptr) *error = "Unknown pending node execution ID.";
        return false;
    }
    const UIFlowGraphExecutionId graphExecutionId = executionId->second;
    _impl->nodeExecutions.erase(executionId);
    auto execution = _impl->executions.find(graphExecutionId);
    if (execution == _impl->executions.end()
        || !execution->second.pending.contains(nodeExecutionId)) {
        if (error != nullptr) *error = "Pending node state is inconsistent.";
        return false;
    }
    const std::string nodeId =
        execution->second.pending.at(nodeExecutionId).nodeId;
    execution->second.pending.erase(nodeExecutionId);
    if (result.state == UIFlowGraphNodeState::Failed) {
        const std::string message = result.message.empty()
            ? "Node '" + nodeId + "' failed." : result.message;
        _impl->appendTrace(
            execution->second, nodeExecutionId, nodeId, "failed");
        (void)_impl->finish(graphExecutionId, false, message, true);
        if (error != nullptr) *error = message;
        return false;
    }
    const auto* node = findNode(*execution->second.graph, nodeId);
    if (node == nullptr) {
        const std::string message = "Pending Graph node disappeared.";
        (void)_impl->finish(graphExecutionId, false, message, true);
        if (error != nullptr) *error = message;
        return false;
    }
    const std::string resultError = _impl->validateCompletedResult(
        *node, result);
    if (!resultError.empty()) {
        _impl->appendTrace(execution->second, nodeExecutionId, nodeId,
                           "invalid-result");
        (void)_impl->finish(graphExecutionId, false, resultError, true);
        if (error != nullptr) *error = resultError;
        return false;
    }
    _impl->appendTrace(execution->second, nodeExecutionId, nodeId,
                       "completed:" + result.flowOutput,
                       {}, result.outputs);
    _impl->routeCompleted(execution->second, nodeId, std::move(result));
    const Impl::DriveResult driven = _impl->drive(graphExecutionId, true);
    if (driven.state == Impl::DriveState::Failed) {
        if (error != nullptr) *error = driven.message;
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

void UIFlowGraphExecutor::update(double deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0) return;
    std::vector<std::pair<UIFlowGraphExecutionId,
                          UIFlowGraphNodeExecutionId>> expired;
    for (auto& [executionId, execution] : _impl->executions) {
        for (auto& [nodeExecutionId, pending] : execution.pending) {
            if (pending.timeoutSeconds <= 0.0) continue;
            pending.elapsedSeconds += deltaSeconds;
            if (pending.elapsedSeconds >= pending.timeoutSeconds) {
                expired.emplace_back(executionId, nodeExecutionId);
            }
        }
    }
    std::sort(expired.begin(), expired.end());
    std::unordered_set<UIFlowGraphExecutionId> finished;
    for (const auto& [executionId, nodeExecutionId] : expired) {
        if (!finished.insert(executionId).second) continue;
        const auto execution = _impl->executions.find(executionId);
        if (execution == _impl->executions.end()) continue;
        const auto pending = execution->second.pending.find(nodeExecutionId);
        if (pending == execution->second.pending.end()) continue;
        const std::string nodeId = pending->second.nodeId;
        _impl->appendTrace(execution->second, nodeExecutionId, nodeId,
                           "timeout");
        (void)_impl->finish(executionId, false,
            "Node '" + nodeId + "' timed out.", true);
    }
}

bool UIFlowGraphExecutor::interruptGraph(
    UIFlowGraphExecutionId executionId,
    UIFlowGraphInterrupt interrupt) noexcept
{
    const auto found = _impl->executions.find(executionId);
    if (found == _impl->executions.end()) return false;
    if (_impl->paused.has_value()
        && _impl->paused->graphExecutionId == executionId) {
        _impl->paused.reset();
    }
    _impl->resumeOnce.erase(executionId);
    _impl->pauseAfterNode.erase(executionId);
    UIFlowGraphNodeExecutionId tracedNode = 0u;
    std::string tracedNodeId;
    if (!found->second.pending.empty()) {
        tracedNode = found->second.pending.begin()->first;
        tracedNodeId = found->second.pending.begin()->second.nodeId;
    }
    _impl->appendTrace(found->second,
        tracedNode, std::move(tracedNodeId),
        interrupt == UIFlowGraphInterrupt::Reverse
            ? "interrupted:reverse" : "interrupted:cancel");
    std::vector<UIFlowGraphNodeCancellationHandler> cancellations;
    for (auto& [nodeExecutionId, pending] : found->second.pending) {
        _impl->nodeExecutions.erase(nodeExecutionId);
        if (pending.onCancel) {
            cancellations.push_back(std::move(pending.onCancel));
        }
    }
    _impl->executions.erase(found);
    for (auto& cancel : cancellations) {
        try {
            cancel(interrupt);
        } catch (...) {
        }
    }
    return true;
}

void UIFlowGraphExecutor::setCompletionHandler(
    UIFlowGraphCompletionHandler handler)
{
    _impl->completionHandler = std::move(handler);
}

bool UIFlowGraphExecutor::hasPendingGraph(
    UIFlowGraphExecutionId executionId) const noexcept
{
    return _impl->executions.contains(executionId);
}

std::size_t UIFlowGraphExecutor::pendingGraphCount() const noexcept
{
    return _impl->executions.size();
}

std::size_t UIFlowGraphExecutor::pendingNodeCount() const noexcept
{
    return _impl->nodeExecutions.size();
}

bool UIFlowGraphExecutor::setBreakpoint(
    std::string graphId, std::string nodeId, bool enabled)
{
    if (graphId.empty() || nodeId.empty()) return false;
    const std::string key = Impl::breakpointKey(graphId, nodeId);
    if (enabled) {
        _impl->breakpoints.insert(key);
    } else {
        _impl->breakpoints.erase(key);
    }
    return true;
}

void UIFlowGraphExecutor::clearBreakpoints() noexcept
{
    _impl->breakpoints.clear();
}

std::size_t UIFlowGraphExecutor::breakpointCount() const noexcept
{
    return _impl->breakpoints.size();
}

void UIFlowGraphExecutor::requestPause() noexcept
{
    _impl->pauseRequested = true;
}

bool UIFlowGraphExecutor::isPaused() const noexcept
{
    return _impl->paused.has_value();
}

const UIFlowGraphDebugPause* UIFlowGraphExecutor::debugPause() const noexcept
{
    return _impl->paused.has_value() ? &*_impl->paused : nullptr;
}

bool UIFlowGraphExecutor::continueExecution(std::string* error)
{
    if (!_impl->paused.has_value()) {
        if (error != nullptr) *error = "Graph executor is not paused.";
        return false;
    }
    const UIFlowGraphExecutionId executionId =
        _impl->paused->graphExecutionId;
    const std::string nodeId = _impl->paused->nodeId;
    _impl->paused.reset();
    _impl->resumeOnce[executionId] = nodeId;
    _impl->pauseAfterNode.erase(executionId);
    const Impl::DriveResult result = _impl->drive(executionId, true);
    if (result.state == Impl::DriveState::Failed) {
        if (error != nullptr) *error = result.message;
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

bool UIFlowGraphExecutor::stepExecution(std::string* error)
{
    if (!_impl->paused.has_value()) {
        if (error != nullptr) *error = "Graph executor is not paused.";
        return false;
    }
    const UIFlowGraphExecutionId executionId =
        _impl->paused->graphExecutionId;
    const std::string nodeId = _impl->paused->nodeId;
    _impl->paused.reset();
    _impl->resumeOnce[executionId] = nodeId;
    _impl->pauseAfterNode.insert(executionId);
    const Impl::DriveResult result = _impl->drive(executionId, true);
    if (result.state == Impl::DriveState::Failed) {
        if (error != nullptr) *error = result.message;
        return false;
    }
    if (error != nullptr) error->clear();
    return true;
}

const std::vector<UIFlowGraphExecutorTrace>& UIFlowGraphExecutor::trace()
    const noexcept
{
    return _impl->traces;
}

void UIFlowGraphExecutor::clearTrace() noexcept
{
    _impl->traces.clear();
}

void UIFlowGraphExecutor::reset() noexcept
{
    _impl->abandonExecutions(UIFlowGraphInterrupt::Cancel);
}

} // namespace ayt::app
