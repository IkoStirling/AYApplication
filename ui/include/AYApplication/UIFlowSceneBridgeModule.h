#pragma once

#include <AYApplication/UIFlowSceneBridge.h>
#include <AYGameLoop/SubSystemModule.h>

#include <string_view>

namespace ayt::app
{

class IEngineHost;

inline constexpr std::string_view kUIFlowSceneBridgeModuleId =
    "AYApplication.UIFlowSceneBridge";
inline constexpr std::string_view kUIFlowSceneBridgeSubSystemName =
    "UIFlowSceneBridge";

class UIFlowSceneBridgeModule final : public ayt::game::SubSystemModule
{
public:
    explicit UIFlowSceneBridgeModule(
        IEngineHost& host,
        UIFlowSceneBridgeConfig config = {});

    [[nodiscard]] ayt::module::ModuleResult registerTypes(
        ayt::module::IModuleContext& context) override;
    void shutdown(ayt::module::IModuleContext& context) noexcept override;

private:
    IEngineHost& _host;
};

[[nodiscard]] UIFlowSceneBridge* uiFlowSceneBridge(
    IEngineHost& host) noexcept;

} // namespace ayt::app
