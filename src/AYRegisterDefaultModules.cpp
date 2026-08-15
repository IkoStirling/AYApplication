#include <AYRegisterDefaultModules.h>

#include <AYAudioBackendFactory.h>
#include <AYAudioSubSystem.h>
#include <AYDeviceSubSystem.h>
#include <AYEntityModule.h>
#include <AYGameLoop.h>
#include <AYPhysicsSubSystem.h>
#include <AYPhysicsTypes.h>
#include <AYRendererSubSystem.h>
#include <AYScriptSubSystem.h>

#include <memory>

namespace ayt::app
{

void registerEntityPresentationStack()
{
    ayt::entity::bootstrapModule();
    ayt::render::RendererSubSystem::registerSubSystem();
}

void registerPhysicsModule()
{
    registerPhysicsModule(ayt::physics::PhysicsBackendDescriptor{});
}

void registerPhysicsModule(const ayt::physics::PhysicsBackendDescriptor& desc)
{
    if (ayt::physics::PhysicsSubSystem::findRegistered() != nullptr) {
        return;
    }
    ayt::physics::PhysicsSubSystem::registerSubSystem(desc);
}

void registerDefaultClientModules(const ClientModuleOptions& options)
{
    ayt::device::DeviceConfig config{};
    config.window.title = options.windowTitle ? options.windowTitle : "Untitled";
    config.window.width = options.windowWidth;
    config.window.height = options.windowHeight;
    ayt::device::DeviceSubSystem::setBootstrapConfig(config);
    ayt::device::DeviceSubSystem::registerSubSystem();

    if (options.enablePresentation) {
        registerEntityPresentationStack();
    } else {
        // Entity core only — no renderer/skinned systems (standalone / headless).
        ayt::entity::bootstrapEntityCore();
    }

    if (options.enablePhysics) {
        registerPhysicsModule();
    }

    ayt::game::GameLoop::instance().registerSubSystem(
        new ayt::script::ScriptSubSystem());

    if (!options.enableAudio) {
        return;
    }

    auto audioSub = std::make_unique<ayt::audio::AudioSubSystem>();
    audioSub->setBackend(ayt::audio::makeMiniaudioBackend());
    ayt::game::GameLoop::instance().registerSubSystem(audioSub.release());
}

void registerDefaultServerModules(const ServerModuleOptions& options)
{
    // No Device / Audio / Renderer — headless sim authority only.
    ayt::entity::bootstrapEntityCore();

    if (options.enablePhysics) {
        registerPhysicsModule();
    }

    if (options.enableScript) {
        ayt::game::GameLoop::instance().registerSubSystem(
            new ayt::script::ScriptSubSystem());
    }
}

} // namespace ayt::app
