#pragma once

#include <AYApplication/GameFlowRuntime.h>
#include <AYGameLoop/SubSystemModule.h>

namespace ayt::app
{

class GameFlowRuntimeModule final : public ayt::game::SubSystemModule
{
public:
    GameFlowRuntimeModule(
        IEngineHost& host,
        GameFlowRuntimeConfig config);
    GameFlowRuntimeModule(
        IEngineHost& host,
        std::unique_ptr<GameFlowRuntimePreparation> preparation);

    void shutdown(ayt::module::IModuleContext& context) noexcept override;

private:
    IEngineHost& _host;
};

} // namespace ayt::app
