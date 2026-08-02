// Test_EngineHost.cpp — Step 1 thin host (no full Application link)

#include "AYTest.h"
#include <IEngineHost.h>
#include <AYGameLoop.h>
#include <ayevent/EventBus.h>

using namespace ayt::app;

TEST_SUITE(EngineHostTests)

TEST_CASE(default_host_exposes_loop_and_bus)
{
    IEngineHost& host = defaultEngineHost();
    CHECK(&host.gameLoop() == &ayt::game::GameLoop::instance());
    CHECK(&host.eventBus() == &ayt::event::EventBus::instance());
}

TEST_CASE(engine_host_scope_sets_current)
{
    CHECK(currentEngineHost() == nullptr);
    {
        EngineHostScope scope(defaultEngineHost());
        CHECK(currentEngineHost() == &defaultEngineHost());
        CHECK(currentEngineHost()->findSubSystem("no.such.system") == nullptr);
    }
    CHECK(currentEngineHost() == nullptr);
}

TEST_SUITE_END
