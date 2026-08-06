// Test_EngineHost.cpp — service registry without linking DefaultEngineHost builtins

#include "AYTest.h"
#include <IEngineHost.h>

#include <AYGameLoop.h>
#include <AYSceneManager.h>  // PR-6 (v0.1.3): scenes() facade
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
    // PR-6 (v0.1.3): scenes() 不 fallback（与 FakeHost 现有 physics()/audio() 同形态——
    // FakeHost 是「纯登记表」，fallback 由 DefaultEngineHost 验证）。
    ayt::scene::SceneManager* scenes() override
    {
        return static_cast<ayt::scene::SceneManager*>(
            findService(kHostServiceScenes));
    }

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

// === PR-6 (v0.1.3, design §10 Q-F 收口) =====================================
//
// 设计依据：
//   * IEngineHost::scenes() facade — key+fallback 双路径（mirror resources()）
//   * kHostServiceScenes = "ayt.scene.SceneManager"
//   * bindBuiltinHostServices 提供 &SceneManager::instance()
//   * 文档：engine-host.md §3 (usage) + §4.2 (key-table)
//
// 验证策略：
//   * FakeHost 不 fallback（与现有 physics()/audio() 同形态）
//   * DefaultEngineHost fallback 行为通过代码走查 + 现有 provide_and_service_roundtrip
//     测试覆盖（PR-6 不在 EngineHostTest 加 DefaultEngineHost case，因为 EngineHostTest
//     是 standalone leaf exe — 不 link AYApplication，避免 AYAudio/tinyexr/basisu
//     collision；AYApplicationTest link AYApplication 主路径，fallback 路径间接验证）

TEST_SUITE(EngineHostScenesFacade)

TEST_CASE(host_scenes_returns_managed_singleton_when_provided)
{
    FakeHost host;
    // 未 provide — FakeHost 不 fallback，scenes() 应为 nullptr。
    CHECK(host.scenes() == nullptr);
    CHECK(host.service<ayt::scene::SceneManager>(kHostServiceScenes) == nullptr);

    // provide 后：key + typed accessor 都返 singleton 指针。
    ayt::scene::SceneManager& instance = ayt::scene::SceneManager::instance();
    host.provide(kHostServiceScenes, &instance);

    CHECK(host.scenes() == &instance);
    CHECK(host.service<ayt::scene::SceneManager>(kHostServiceScenes) == &instance);
}

TEST_CASE(host_scenes_clear_provided_services_drops_key_only)
{
    FakeHost host;
    ayt::scene::SceneManager& instance = ayt::scene::SceneManager::instance();
    host.provide(kHostServiceScenes, &instance);
    CHECK(host.scenes() == &instance);

    // clearProvidedServices 后：
    //   * service<T>(key) → nullptr（key 表清空）
    //   * scenes() FakeHost 不 fallback → nullptr（FakeHost 是纯登记表）
    host.clearProvidedServices();
    CHECK(host.service<ayt::scene::SceneManager>(kHostServiceScenes) == nullptr);
    CHECK(host.scenes() == nullptr);
}

TEST_SUITE_END
