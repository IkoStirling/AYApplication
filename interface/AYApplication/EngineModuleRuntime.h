#pragma once

#include <AYApplication/EngineModuleContext.h>
#include <AYModule/ModuleManager.h>

namespace ayt::app
{

// Host-side owner for one startup module graph. Existing default module
// registration does not use this object yet; application/editor composition
// roots opt in explicitly as modules are migrated.
class EngineModuleRuntime
{
public:
    explicit EngineModuleRuntime(IEngineHost& host) noexcept;

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
