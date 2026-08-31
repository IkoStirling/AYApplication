#include <AYApplication/EngineModuleContext.h>

namespace ayt::app
{

EngineModuleContext::EngineModuleContext(IEngineHost& host) noexcept
    : _host(host)
{
}

void* EngineModuleContext::findService(std::string_view key) const noexcept
{
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

} // namespace ayt::app
