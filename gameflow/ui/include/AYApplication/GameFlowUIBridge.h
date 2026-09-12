#pragma once

#include <AYApplication/GameFlowStandardActions.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

class GameFlowRuntime;
class UIFlowRuntime;

inline constexpr std::string_view kUIFlowActionGameFlowRequest =
    "gameflow.request";

struct GameFlowUISignalIntentBinding
{
    std::string signalId;
    std::string intentId;
};

struct GameFlowUIBridgeConfig
{
    std::vector<GameFlowUISignalIntentBinding> signalBindings;
    bool enableRequestAction = true;
};

// Optional process-scoped bridge between two independently usable runtimes.
// The referenced runtimes must outlive the bridge.
class GameFlowUIBridge final
{
public:
    GameFlowUIBridge(
        GameFlowRuntime& gameFlow,
        UIFlowRuntime& uiFlow,
        GameFlowUIBridgeConfig config = {});
    ~GameFlowUIBridge();

    GameFlowUIBridge(const GameFlowUIBridge&) = delete;
    GameFlowUIBridge& operator=(const GameFlowUIBridge&) = delete;
    GameFlowUIBridge(GameFlowUIBridge&&) noexcept;
    GameFlowUIBridge& operator=(GameFlowUIBridge&&) noexcept;

    bool install(std::string* error = nullptr);
    void uninstall() noexcept;

    [[nodiscard]] bool installed() const noexcept;
    [[nodiscard]] std::string_view lastError() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app
