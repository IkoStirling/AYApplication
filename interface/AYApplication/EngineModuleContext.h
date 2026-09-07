#pragma once

#include <AYApplication/IEngineHost.h>
#include <AYGameLoop/SubSystemModule.h>
#include <AYModule/IModuleContext.h>

#include <string_view>

namespace ayt::entity
{
class ComponentRegistry;
}

namespace ayt::app
{

// Adapts the process-scoped engine host to AYModule's dependency-neutral
// service lookup. The adapter never owns the host or any returned service.
class EngineModuleContext final
    : public ayt::module::IModuleContext,
      public ayt::game::ISubSystemModuleService
{
public:
    explicit EngineModuleContext(IEngineHost& host) noexcept;
    EngineModuleContext(
        IEngineHost& host,
        ayt::entity::ComponentRegistry& componentRegistry) noexcept;

    [[nodiscard]] void* findService(std::string_view key) const noexcept override;
    [[nodiscard]] IEngineHost& host() const noexcept;
    [[nodiscard]] ayt::entity::ComponentRegistry& componentRegistry()
        const noexcept;

    [[nodiscard]] ayt::game::ISubSystem* findSubSystem(
        std::string_view name) noexcept override;
    [[nodiscard]] bool installSubSystem(
        std::unique_ptr<ayt::game::ISubSystem> system) override;
    void uninstallSubSystem(
        std::string_view name,
        ayt::game::ISubSystem* expectedInstance) noexcept override;

private:
    IEngineHost& _host;
    ayt::entity::ComponentRegistry& _componentRegistry;
};

} // namespace ayt::app
