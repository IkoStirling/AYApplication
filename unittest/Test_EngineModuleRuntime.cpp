#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYModule/IModule.h>
#include <AYTest.h>

#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ayt::app::test
{
namespace
{

inline constexpr std::string_view kProbeService = "test.module.ProbeService";

class FakeHost final : public IEngineHost
{
public:
    ayt::game::IGameLoop& gameLoop() override
    {
        std::abort();
    }

    ayt::event::EventBus& eventBus() override
    {
        std::abort();
    }

    ayt::game::ISubSystem* findSubSystem(const char*) override
    {
        return nullptr;
    }

    void provideService(std::string_view key, void* instance) override
    {
        if (key.empty()) {
            return;
        }
        if (instance == nullptr) {
            _services.erase(std::string(key));
        } else {
            _services[std::string(key)] = instance;
        }
    }

    void* findService(std::string_view key) const override
    {
        if (_throwOnLookup) {
            throw std::runtime_error("planned host lookup failure");
        }
        const auto found = _services.find(std::string(key));
        return found == _services.end() ? nullptr : found->second;
    }

    void clearProvidedServices() override
    {
        _services.clear();
    }

    ayt::resource::ResourceManager* resources() override { return nullptr; }
    ayt::physics::PhysicsManager* physics() override { return nullptr; }
    ayt::physics::IPhysicsQuery* physicsQuery() override { return nullptr; }
    ayt::audio::AudioEngine* audio() override { return nullptr; }
    ayt::scene::SceneManager* scenes() override { return nullptr; }

    void setThrowOnLookup(bool enabled) noexcept
    {
        _throwOnLookup = enabled;
    }

private:
    std::unordered_map<std::string, void*> _services;
    bool _throwOnLookup = false;
};

class RecordingModule final : public ayt::module::IModule
{
public:
    RecordingModule(
        ayt::module::ModuleDescriptor descriptor,
        std::vector<std::string>& events,
        bool failInstall = false)
        : _descriptor(std::move(descriptor)),
          _events(events),
          _failInstall(failInstall)
    {
    }

    const ayt::module::ModuleDescriptor& descriptor() const noexcept override
    {
        return _descriptor;
    }

    ayt::module::ModuleResult registerTypes(
        ayt::module::IModuleContext& context) override
    {
        _events.push_back("types:" + _descriptor.id);
        if (context.findServiceAs<int>(kProbeService) == nullptr) {
            return ayt::module::ModuleResult::failure(
                ayt::module::ModuleErrorCode::TypeRegistrationFailed,
                "probe service is unavailable");
        }
        return ayt::module::ModuleResult::success();
    }

    ayt::module::ModuleResult install(
        ayt::module::IModuleContext&) override
    {
        _events.push_back("install:" + _descriptor.id);
        if (_failInstall) {
            return ayt::module::ModuleResult::failure(
                ayt::module::ModuleErrorCode::InstallationFailed,
                "planned install failure");
        }
        return ayt::module::ModuleResult::success();
    }

    void shutdown(ayt::module::IModuleContext&) noexcept override
    {
        _events.push_back("shutdown:" + _descriptor.id);
    }

private:
    ayt::module::ModuleDescriptor _descriptor;
    std::vector<std::string>& _events;
    bool _failInstall = false;
};

ayt::module::ModuleDescriptor descriptor(
    std::string id,
    std::vector<ayt::module::ModuleDependency> dependencies = {})
{
    return {
        .id = std::move(id),
        .displayName = {},
        .version = "1.0.0",
        .dependencies = std::move(dependencies)
    };
}

} // namespace

TEST_SUITE(EngineModuleRuntimeTests)

TEST_CASE(context_forwards_engine_host_services)
{
    FakeHost host;
    int probe = 42;
    host.provideService(kProbeService, &probe);

    EngineModuleContext context(host);
    CHECK(&context.host() == &host);
    CHECK(context.findServiceAs<int>(kProbeService) == &probe);
    CHECK(context.findService("test.module.Missing") == nullptr);

    host.setThrowOnLookup(true);
    CHECK(context.findService(kProbeService) == nullptr);
}

TEST_CASE(prepare_keeps_registry_seal_boundary_before_install)
{
    FakeHost host;
    int probe = 42;
    host.provideService(kProbeService, &probe);

    EngineModuleRuntime runtime(host);
    std::vector<std::string> events;

    CHECK_TRUE(runtime.modules().add(std::make_unique<RecordingModule>(
        descriptor(
            "Feature",
            {ayt::module::ModuleDependency::required("Core")}),
        events)).succeeded());
    CHECK_TRUE(runtime.modules().add(std::make_unique<RecordingModule>(
        descriptor("Core"),
        events)).succeeded());

    CHECK_TRUE(runtime.prepare().succeeded());
    CHECK(runtime.modules().phase() ==
        ayt::module::ModuleManagerPhase::TypesRegistered);
    CHECK(events == std::vector<std::string>({"types:Core", "types:Feature"}));

    // A real host seals ComponentRegistry/ReflectRegistry at this boundary.
    CHECK_TRUE(runtime.install().succeeded());
    CHECK(events == std::vector<std::string>({
        "types:Core",
        "types:Feature",
        "install:Core",
        "install:Feature"
    }));

    runtime.shutdown();
    runtime.shutdown();
    CHECK(events == std::vector<std::string>({
        "types:Core",
        "types:Feature",
        "install:Core",
        "install:Feature",
        "shutdown:Feature",
        "shutdown:Core"
    }));
}

TEST_CASE(start_rolls_back_a_partial_install)
{
    FakeHost host;
    int probe = 42;
    host.provideService(kProbeService, &probe);

    EngineModuleRuntime runtime(host);
    std::vector<std::string> events;

    CHECK_TRUE(runtime.modules().add(std::make_unique<RecordingModule>(
        descriptor("Core"),
        events)).succeeded());
    CHECK_TRUE(runtime.modules().add(std::make_unique<RecordingModule>(
        descriptor(
            "Feature",
            {ayt::module::ModuleDependency::required("Core")}),
        events,
        true)).succeeded());

    const ayt::module::ModuleResult result = runtime.start();
    CHECK_FALSE(result.succeeded());
    CHECK(result.code() == ayt::module::ModuleErrorCode::InstallationFailed);
    CHECK(runtime.modules().phase() == ayt::module::ModuleManagerPhase::Failed);
    CHECK(events == std::vector<std::string>({
        "types:Core",
        "types:Feature",
        "install:Core",
        "install:Feature",
        "shutdown:Feature",
        "shutdown:Core"
    }));
}

TEST_SUITE_END

} // namespace ayt::app::test
