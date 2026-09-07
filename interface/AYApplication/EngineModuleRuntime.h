#pragma once

#include <AYApplication/EngineModuleContext.h>
#include <AYModule/ModuleManager.h>

namespace ayt::app
{

// Host-side owner for one startup module graph. ApplicationImpl and EditorApp
// use this object for the migrated default runtime modules; compatibility and
// not-yet-migrated host wiring may still run alongside the graph.
class EngineModuleRuntime
{
public:
    explicit EngineModuleRuntime(IEngineHost& host) noexcept;
    EngineModuleRuntime(
        IEngineHost& host,
        ayt::entity::ComponentRegistry& componentRegistry) noexcept;

    EngineModuleRuntime(const EngineModuleRuntime&) = delete;
    EngineModuleRuntime& operator=(const EngineModuleRuntime&) = delete;
    EngineModuleRuntime(EngineModuleRuntime&&) = delete;
    EngineModuleRuntime& operator=(EngineModuleRuntime&&) = delete;

    [[nodiscard]] ayt::module::ModuleManager& modules() noexcept;
    [[nodiscard]] const ayt::module::ModuleManager& modules() const noexcept;
    [[nodiscard]] EngineModuleContext& context() noexcept;
    [[nodiscard]] const EngineModuleContext& context() const noexcept;

    // resolve + registerTypes. The caller may seal component/reflection
    // registries after this returns and before calling install().
    [[nodiscard]] ayt::module::ModuleResult prepare();
    [[nodiscard]] ayt::module::ModuleResult install();

    // Convenience for hosts that do not need an explicit registry seal point.
    [[nodiscard]] ayt::module::ModuleResult start();

    // Explicit because the runtime does not own IEngineHost.
    void shutdown() noexcept;

private:
    EngineModuleContext _context;
    ayt::module::ModuleManager _modules;
};

} // namespace ayt::app
