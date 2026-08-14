#include <AYRegisterDefaultModules.h>

#include <AYAudioBackendFactory.h>
#include <AYAudioSubSystem.h>
#include <AYDeviceSubSystem.h>
#include <AYEntityModule.h>
#include <AYGameLoop.h>
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

    ayt::game::GameLoop::instance().registerSubSystem(
        new ayt::script::ScriptSubSystem());

    if (!options.enableAudio) {
        return;
    }

    auto audioSub = std::make_unique<ayt::audio::AudioSubSystem>();
    audioSub->setBackend(ayt::audio::makeMiniaudioBackend());
    ayt::game::GameLoop::instance().registerSubSystem(audioSub.release());
}

} // namespace ayt::app
