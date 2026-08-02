// Test_EngineHost.cpp — service registry without linking DefaultEngineHost builtins

#include "AYTest.h"
#include <IEngineHost.h>

#include <AYGameLoop.h>
#include <AYSubSystemRegistry.h>
#include <ayevent/EventBus.h>

#include <string>
#include <unordered_map>

using namespace ayt::app;

namespace {

struct FakePhysics {
    int id = 0;
};

/// Minimal host for registry tests (avoids AYResource/AYAudio link in this exe).
class FakeHost final : public IEngineHost {
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
        if (!name) {
            return nullptr;
        }
        return ayt::game::SubSystemRegistry::instance().findSubSystem(name);
    }

    void provideService(std::string_view key, void* instance) override
    {
        if (key.empty()) {
            return;
        }
        if (!instance) {
            _map.erase(std::string(key));
        } else {
            _map[std::string(key)] = instance;
        }
    }

    void* findService(std::string_view key) const override
    {
        const auto it = _map.find(std::string(key));
        return it == _map.end() ? nullptr : it->second;
    }

    void clearProvidedServices() override { _map.clear(); }

    ayt::resource::ResourceManager* resources() override { return nullptr; }
    ayt::physics::PhysicsManager* physics() override
    {
        return static_cast<ayt::physics::PhysicsManager*>(
            findService(kHostServicePhysics));
    }
    ayt::audio::AudioEngine* audio() override { return nullptr; }

private:
    std::unordered_map<std::string, void*> _map;
};

} // namespace

TEST_SUITE(EngineHostTests)

TEST_CASE(provide_and_service_roundtrip)
{
    FakeHost host;
    FakePhysics phys;
    phys.id = 7;

    CHECK(host.physics() == nullptr);
    host.provide(kHostServicePhysics, reinterpret_cast<ayt::physics::PhysicsManager*>(&phys));
    CHECK(host.physics() == reinterpret_cast<ayt::physics::PhysicsManager*>(&phys));
    CHECK(host.service<FakePhysics>("game.test.FakePhysics") == nullptr);

    host.provide("game.test.FakePhysics", &phys);
    CHECK(host.service<FakePhysics>("game.test.FakePhysics") == &phys);
    CHECK(host.service<FakePhysics>("game.test.FakePhysics")->id == 7);

    host.provide(kHostServicePhysics, static_cast<ayt::physics::PhysicsManager*>(nullptr));
    CHECK(host.physics() == nullptr);
}

TEST_CASE(engine_host_scope_sets_current)
{
    FakeHost host;
    CHECK(currentEngineHost() == nullptr);
    {
        EngineHostScope scope(host);
        CHECK(currentEngineHost() == &host);
        CHECK(currentEngineHost()->findService("missing") == nullptr);
    }
    CHECK(currentEngineHost() == nullptr);
}

TEST_SUITE_END
