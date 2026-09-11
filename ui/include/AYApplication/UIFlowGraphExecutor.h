#pragma once

#include <AYApplication/UIFlowRuntime.h>
#include <AYUI/UIFlowGraphNodeRegistry.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

using UIFlowGraphNodeExecutionId = std::uint64_t;

enum class UIFlowGraphNodeState : std::uint8_t
{
    Completed,
    Running,
    Failed,
};

struct UIFlowGraphNodeInvocation
{
    UIFlowGraphExecutionId graphExecutionId = 0;
    UIFlowGraphNodeExecutionId nodeExecutionId = 0;
    const ayt::ui::UIFlowGraphDefinition* graph = nullptr;
    const ayt::ui::UIFlowNodeDefinition* node = nullptr;
    UIFlowGraphRequest request;
    UIFlowPayload inputs;
};

struct UIFlowGraphNodeResult
{
    UIFlowGraphNodeState state = UIFlowGraphNodeState::Completed;
    std::string flowOutput = "completed";
    UIFlowPayload outputs;
    std::string message;

    static UIFlowGraphNodeResult completed(
        std::string flowOutput = "completed",
        UIFlowPayload outputs = {});
    static UIFlowGraphNodeResult running();
    static UIFlowGraphNodeResult failure(std::string message);
};

using UIFlowGraphNodeHandler =
    std::function<UIFlowGraphNodeResult(const UIFlowGraphNodeInvocation&)>;
using UIFlowGraphCompletionHandler = std::function<void(
    UIFlowGraphExecutionId executionId,
    bool succeeded,
    std::string message)>;

struct UIFlowGraphExecutorTrace
{
    std::uint64_t serial = 0;
    UIFlowGraphExecutionId graphExecutionId = 0;
    UIFlowGraphNodeExecutionId nodeExecutionId = 0;
    std::string graphId;
    std::string nodeId;
    std::string detail;
};

// Executes host-registered command nodes while AYUI remains an open wire and
// authoring contract. Execution is deterministic and serial per Graph. A node
// may suspend the Graph and later resume it through completeNode().
class UIFlowGraphExecutor
{
public:
    UIFlowGraphExecutor();
    ~UIFlowGraphExecutor();

    UIFlowGraphExecutor(const UIFlowGraphExecutor&) = delete;
    UIFlowGraphExecutor& operator=(const UIFlowGraphExecutor&) = delete;
    UIFlowGraphExecutor(UIFlowGraphExecutor&&) noexcept;
    UIFlowGraphExecutor& operator=(UIFlowGraphExecutor&&) noexcept;

    // The document must outlive every execution. Replacing it cancels pending
    // work without invoking completion callbacks.
    void setDocument(const ayt::ui::UIFlowDocument* document) noexcept;
    [[nodiscard]] const ayt::ui::UIFlowDocument* document() const noexcept;

    bool registerNodeType(
        ayt::ui::UIFlowGraphNodeTypeDefinition definition,
        UIFlowGraphNodeHandler handler,
        bool replace = false,
        std::string* error = nullptr);
    bool unregisterNodeType(std::string_view type);
    void clearNodeTypes() noexcept;
    [[nodiscard]] const ayt::ui::UIFlowGraphNodeRegistry& nodeTypes()
        const noexcept;

    UIFlowGraphStartResult start(const UIFlowGraphExecutionRequest& request);
    bool completeNode(
        UIFlowGraphNodeExecutionId nodeExecutionId,
        UIFlowGraphNodeResult result,
        std::string* error = nullptr);
    bool interruptGraph(
        UIFlowGraphExecutionId executionId,
        UIFlowGraphInterrupt interrupt) noexcept;

    void setCompletionHandler(UIFlowGraphCompletionHandler handler);
    [[nodiscard]] bool hasPendingGraph(
        UIFlowGraphExecutionId executionId) const noexcept;
    [[nodiscard]] std::size_t pendingGraphCount() const noexcept;
    [[nodiscard]] std::size_t pendingNodeCount() const noexcept;

    [[nodiscard]] const std::vector<UIFlowGraphExecutorTrace>& trace()
        const noexcept;
    void clearTrace() noexcept;
    void reset() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app
