#pragma once

#include <AYUI/UIFlow.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

using UIFlowContextHandle = std::uint64_t;
using UIFlowSignalSubscription = std::uint64_t;
using UIFlowPayload = std::map<std::string, ayt::ui::UIFlowValue>;

struct UIFlowScopeBinding
{
    ayt::ui::UIFlowScope scope = ayt::ui::UIFlowScope::Application;
    std::string key;
};

struct UIFlowContextActivationOptions
{
    // Controls the activation's lifetime. Screen lifetime remains defined by
    // UIFlowScreenDefinition::scope.
    UIFlowScopeBinding lifetime{};
};

struct UIFlowScreenMountRequest
{
    std::uint64_t mountId = 0;
    std::string screenId;
    std::string layoutAsset;
    std::string layerId;
    std::string slotId;
    std::string contextId;
    ayt::ui::UIFlowScope scope = ayt::ui::UIFlowScope::Transient;
    std::string scopeKey;
    int layerOrder = 0;
    ayt::ui::UIFlowInputPolicy inputPolicy =
        ayt::ui::UIFlowInputPolicy::ConsumeHandled;
    bool blocksLowerInput = false;
    std::uint32_t orderInLayer = 0;
    std::string enterAnimation;
    std::string exitAnimation;
    UIFlowPayload parameters;
    std::vector<ayt::ui::UIFlowScreenEventBinding> events;
};

using UIFlowScreenSignalEmitter = std::function<bool(
    std::string_view signalId,
    UIFlowPayload payload,
    std::string* error)>;

// Presentation adapter. The runtime owns logical orchestration and mount IDs;
// the host owns actual Widget trees, native views, or a headless test model.
class IUIFlowScreenHost
{
public:
    virtual ~IUIFlowScreenHost() = default;

    virtual bool mountScreen(
        const UIFlowScreenMountRequest& request,
        std::string& error) = 0;
    virtual void unmountScreen(std::uint64_t mountId) noexcept = 0;
    virtual void setScreenOrder(
        std::uint64_t mountId,
        int layerOrder,
        std::uint32_t orderInLayer) noexcept = 0;
    virtual void setSignalEmitter(UIFlowScreenSignalEmitter emitter) {
        (void)emitter;
    }
    virtual void update(float deltaSeconds) { (void)deltaSeconds; }
};

struct UIFlowMountedScreen
{
    std::uint64_t mountId = 0;
    std::uint64_t activationSerial = 0;
    std::string screenId;
    std::string layoutAsset;
    std::string layerId;
    std::string slotId;
    std::string contextId;
    ayt::ui::UIFlowScope scope = ayt::ui::UIFlowScope::Transient;
    std::string scopeKey;
    int layerOrder = 0;
    std::uint32_t orderInLayer = 0;
    ayt::ui::UIFlowInputPolicy inputPolicy =
        ayt::ui::UIFlowInputPolicy::ConsumeHandled;
    bool blocksLowerInput = false;
    std::string enterAnimation;
    std::string exitAnimation;
    UIFlowPayload parameters;
    std::vector<ayt::ui::UIFlowScreenEventBinding> events;
};

struct UIFlowActionInvocation
{
    std::string actionId;
    UIFlowPayload inputs;
};

struct UIFlowActionResult
{
    bool accepted = true;
    std::string message;

    static UIFlowActionResult success();
    static UIFlowActionResult failure(std::string message);
};

struct UIFlowGraphRequest
{
    std::string graphId;
    std::string signalId;
    UIFlowPayload payload;
    std::string regionId;
    std::string transitionId;
};

using UIFlowGraphExecutionId = std::uint64_t;

enum class UIFlowGraphStartState : std::uint8_t
{
    Completed,
    Running,
    Rejected,
};

struct UIFlowGraphExecutionRequest
{
    UIFlowGraphExecutionId executionId = 0;
    UIFlowGraphRequest graph;
};

struct UIFlowGraphStartResult
{
    UIFlowGraphStartState state = UIFlowGraphStartState::Completed;
    std::string message;

    static UIFlowGraphStartResult completed();
    static UIFlowGraphStartResult running();
    static UIFlowGraphStartResult rejected(std::string message);
};

enum class UIFlowGraphInterrupt : std::uint8_t
{
    Cancel,
    Reverse,
};

struct UIFlowRuntimeTrace
{
    std::uint64_t serial = 0;
    std::string category;
    std::string id;
    std::string detail;
};

struct UIFlowReplaySignal
{
    std::string signalId;
    UIFlowPayload payload;
};

using UIFlowActionHandler =
    std::function<UIFlowActionResult(const UIFlowActionInvocation&)>;
using UIFlowSignalHandler = std::function<void(
    std::string_view signalId,
    const UIFlowPayload& payload)>;
using UIFlowGuardEvaluator = std::function<bool(
    std::string_view expression,
    const UIFlowPayload& payload,
    std::string& error)>;
using UIFlowGraphRequestHandler =
    std::function<void(const UIFlowGraphRequest& request)>;
using UIFlowAsyncGraphRequestHandler = std::function<UIFlowGraphStartResult(
    const UIFlowGraphExecutionRequest& request)>;
using UIFlowGraphInterruptHandler = std::function<void(
    UIFlowGraphExecutionId executionId,
    UIFlowGraphInterrupt interrupt)>;

// Persistent application-level orchestration. It is deliberately independent
// from Scene and Entity; bridges publish dynamic signals and scope keys.
class UIFlowRuntime
{
public:
    explicit UIFlowRuntime(IUIFlowScreenHost& screenHost);
    ~UIFlowRuntime();

    UIFlowRuntime(const UIFlowRuntime&) = delete;
    UIFlowRuntime& operator=(const UIFlowRuntime&) = delete;
    UIFlowRuntime(UIFlowRuntime&&) noexcept;
    UIFlowRuntime& operator=(UIFlowRuntime&&) noexcept;

    bool load(ayt::ui::UIFlowDocument document, std::string* error = nullptr);
    // Transactionally swaps a validated document while preserving compatible
    // scopes, manual Context handles and region states. Mounted Screens whose
    // layout or transition metadata changed are replaced before old trees are
    // retired, so a failed reload leaves the running presentation untouched.
    bool reload(ayt::ui::UIFlowDocument document,
                std::string* error = nullptr);
    bool start(std::string_view entry = {}, std::string* error = nullptr);
    void unload() noexcept;

    [[nodiscard]] bool isLoaded() const noexcept;
    [[nodiscard]] bool isStarted() const noexcept;
    [[nodiscard]] const ayt::ui::UIFlowDocument* document() const noexcept;

    UIFlowContextHandle activateContext(
        std::string_view contextId,
        UIFlowContextActivationOptions options = {},
        std::string* error = nullptr);
    bool deactivateContext(
        UIFlowContextHandle handle,
        std::string* error = nullptr);

    bool beginScope(
        ayt::ui::UIFlowScope scope,
        std::string key,
        std::string* error = nullptr);
    bool endScope(
        ayt::ui::UIFlowScope scope,
        std::string_view key,
        std::string* error = nullptr);
    [[nodiscard]] std::string_view scopeKey(
        ayt::ui::UIFlowScope scope) const noexcept;

    bool emitSignal(
        std::string_view signalId,
        UIFlowPayload payload = {},
        std::string* error = nullptr);
    UIFlowSignalSubscription subscribeSignal(
        std::string signalId,
        UIFlowSignalHandler handler);
    bool unsubscribeSignal(UIFlowSignalSubscription subscription);

    bool registerAction(
        std::string actionId,
        UIFlowActionHandler handler,
        bool replace = false);
    bool unregisterAction(std::string_view actionId);
    UIFlowActionResult invokeAction(
        std::string_view actionId,
        UIFlowPayload inputs = {}) const;

    void setGuardEvaluator(UIFlowGuardEvaluator evaluator);
    void setGraphRequestHandler(UIFlowGraphRequestHandler handler);
    // A Running result installs the execution as pending. Complete it from a
    // later host callback; do not synchronously call completeGraphExecution()
    // from inside the start handler before that pending record exists.
    void setAsyncGraphRequestHandler(
        UIFlowAsyncGraphRequestHandler handler,
        UIFlowGraphInterruptHandler interruptHandler = {});
    bool completeGraphExecution(UIFlowGraphExecutionId executionId,
                                bool succeeded = true,
                                std::string message = {},
                                std::string* error = nullptr);
    [[nodiscard]] bool hasPendingGraphExecution(
        std::string_view regionId = {}) const noexcept;

    [[nodiscard]] const std::vector<UIFlowRuntimeTrace>& trace() const noexcept;
    [[nodiscard]] const std::vector<UIFlowReplaySignal>& replaySignals()
        const noexcept;
    void clearTrace() noexcept;
    void clearReplay() noexcept;
    bool replay(const std::vector<UIFlowReplaySignal>& signals,
                std::string* error = nullptr);

    [[nodiscard]] std::string_view activeState(
        std::string_view regionId) const noexcept;
    [[nodiscard]] const std::vector<UIFlowMountedScreen>& mountedScreens()
        const noexcept;
    [[nodiscard]] std::string_view lastError() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app
