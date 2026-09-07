#include <AYApplication/EngineModuleRuntime.h>

namespace ayt::app
{

EngineModuleRuntime::EngineModuleRuntime(IEngineHost& host) noexcept
    : _context(host)
{
}

EngineModuleRuntime::EngineModuleRuntime(
    IEngineHost& host,
    ayt::entity::ComponentRegistry& componentRegistry) noexcept
    : _context(host, componentRegistry)
{
}

ayt::module::ModuleManager& EngineModuleRuntime::modules() noexcept
{
    return _modules;
}

const ayt::module::ModuleManager& EngineModuleRuntime::modules() const noexcept
{
    return _modules;
}

EngineModuleContext& EngineModuleRuntime::context() noexcept
{
    return _context;
}

const EngineModuleContext& EngineModuleRuntime::context() const noexcept
{
    return _context;
}

ayt::module::ModuleResult EngineModuleRuntime::prepare()
{
    ayt::module::ModuleResult result = _modules.resolve();
    if (!result) {
        return result;
    }
    return _modules.registerTypes(_context);
}

ayt::module::ModuleResult EngineModuleRuntime::install()
{
    return _modules.install(_context);
}

ayt::module::ModuleResult EngineModuleRuntime::start()
{
    ayt::module::ModuleResult result = prepare();
    if (!result) {
        return result;
    }
    return install();
}

void EngineModuleRuntime::shutdown() noexcept
{
    _modules.shutdown(_context);
}

} // namespace ayt::app
