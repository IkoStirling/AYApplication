#include <IEngineHost.h>

#include <AYGameLoop.h>
#include <AYSubSystemRegistry.h>
#include <ayevent/EventBus.h>

namespace ayt::app
{

namespace {

class DefaultEngineHost final : public IEngineHost {
public:
    ayt::game::IGameLoop& gameLoop() override
    {
        return ayt::game::GameLoop::instance();
    }

    ayt::event::EventBus& eventBus() override
    {
        return ayt::event::EventBus::instance();
    }

    ayt::game::ISubSystem* findSubSystem(const char* name) override
    {
        if (name == nullptr || name[0] == '\0') {
            return nullptr;
        }
        return ayt::game::SubSystemRegistry::instance().findSubSystem(name);
    }
};

DefaultEngineHost g_defaultHost;
IEngineHost* g_currentHost = nullptr;

} // namespace

IEngineHost& defaultEngineHost()
{
    return g_defaultHost;
}

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
