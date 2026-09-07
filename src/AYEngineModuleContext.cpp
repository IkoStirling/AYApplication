#include <AYApplication/EngineModuleContext.h>

#include <AYEntity/ComponentRegistry.h>

#include <string>

namespace ayt::app
{

EngineModuleContext::EngineModuleContext(IEngineHost& host) noexcept
    : EngineModuleContext(
          host,
          ayt::entity::ComponentRegistry::instance())
{
}

EngineModuleContext::EngineModuleContext(
    IEngineHost& host,
    ayt::entity::ComponentRegistry& componentRegistry) noexcept
    : _host(host),
      _componentRegistry(componentRegistry)
{
}

void* EngineModuleContext::findService(std::string_view key) const noexcept
{
    if (key == ayt::game::kSubSystemModuleService) {
        return static_cast<ayt::game::ISubSystemModuleService*>(
            const_cast<EngineModuleContext*>(this));
    }
    if (key == ayt::entity::kComponentRegistryModuleService) {
        return &_componentRegistry;
    }
    // AYModule guarantees noexcept lookup while the legacy host interface does
    // not yet express that guarantee. Treat an exceptional host lookup as an
    // unavailable service rather than allowing it across the module boundary.
    try {
        return _host.findService(key);
    } catch (...) {
        return nullptr;
    }
}

IEngineHost& EngineModuleContext::host() const noexcept
{
    return _host;
}

ayt::entity::ComponentRegistry& EngineModuleContext::componentRegistry()
    const noexcept
{
    return _componentRegistry;
}

ayt::game::ISubSystem* EngineModuleContext::findSubSystem(
    std::string_view name) noexcept
{
    if (name.empty()) {
        return nullptr;
    }
    try {
        const std::string ownedName(name);
        return _host.findSubSystem(ownedName.c_str());
    } catch (...) {
        return nullptr;
    }
}

bool EngineModuleContext::installSubSystem(
    std::unique_ptr<ayt::game::ISubSystem> system)
{
    if (!system) {
        return false;
    }
    const char* rawName = system->getName();
    if (rawName == nullptr || rawName[0] == '\0') {
        return false;
    }

    const std::string name(rawName);
    if (findSubSystem(name) != nullptr) {
        return false;
    }

    ayt::game::ISubSystem* expected = system.get();
    _host.gameLoop().registerSubSystem(system.release());
    return findSubSystem(name) == expected;
}

void EngineModuleContext::uninstallSubSystem(
    std::string_view name,
    ayt::game::ISubSystem* expectedInstance) noexcept
{
    if (name.empty() || expectedInstance == nullptr
        || findSubSystem(name) != expectedInstance) {
        return;
    }
    try {
        const std::string ownedName(name);
        _host.gameLoop().unregisterSubSystem(ownedName.c_str());
    } catch (...) {
        // Module shutdown is noexcept. A host that refuses teardown keeps
        // ownership; it must finish cleanup as part of its own shutdown.
    }
}

} // namespace ayt::app
