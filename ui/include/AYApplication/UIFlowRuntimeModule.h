#pragma once

#include <AYApplication/UIFlowRuntime.h>
#include <AYGameLoop/SubSystemModule.h>

#include <memory>
#include <string>
#include <string_view>

namespace ayt::app
{

class IEngineHost;

inline constexpr std::string_view kUIFlowRuntimeModuleId =
    "AYApplication.UIFlowRuntime";
inline constexpr std::string_view kUIFlowRuntimeSubSystemName =
    "UIFlowRuntime";

class UIFlowRuntimeModule final : public ayt::game::SubSystemModule
{
public:
    UIFlowRuntimeModule(
        IEngineHost& host,
        ayt::ui::UIFlowDocument document,
        std::unique_ptr<IUIFlowScreenHost> screenHost,
        std::string entry = {});

    void shutdown(ayt::module::IModuleContext& context) noexcept override;

private:
    IEngineHost& _host;
};

[[nodiscard]] UIFlowRuntime* uiFlowRuntime(IEngineHost& host) noexcept;

} // namespace ayt::app
