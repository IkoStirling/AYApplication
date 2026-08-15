#include <AYApplication/IEngineHost.h>

namespace ayt::app
{

namespace {
IEngineHost* g_currentHost = nullptr;
} // namespace

IEngineHost* currentEngineHost()
{
    return g_currentHost;
}

void setCurrentEngineHost(IEngineHost* host)
{
    g_currentHost = host;
}

EngineHostScope::EngineHostScope(IEngineHost& host)
    : _previous(g_currentHost)
{
    g_currentHost = &host;
}

EngineHostScope::~EngineHostScope()
{
    g_currentHost = _previous;
}

} // namespace ayt::app
