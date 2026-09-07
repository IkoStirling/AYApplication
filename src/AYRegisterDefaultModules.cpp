#include <AYApplication/RegisterDefaultModules.h>

#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RuntimeSceneLoaderModule.h>

#include <AYEntity/EntityComponentModule.h>
#include <AYEntity/EntityModule.h>
#include <AYEntity/EntityRuntimeModule.h>
#include <AYGameLoop.h>

#if AY_APPLICATION_HAS_AUDIO
#include <AYAudio/AudioBackendFactory.h>
#include <AYAudio/AudioRuntimeModule.h>
#include <AYAudio/AudioSubSystem.h>
#endif
#if AY_APPLICATION_HAS_DEVICE
#include <AYDevice/DeviceRuntimeModule.h>
#include <AYDevice/DeviceSubSystem.h>
#endif
#if AY_APPLICATION_HAS_PRESENTATION
#include <AYEntity/EntityAnimationIntegrationModule.h>
#include <AYEntity/EntityRenderIntegrationModule.h>
#include <AYRenderer/RendererRuntimeModule.h>
#include <AYRenderer/RendererSubSystem.h>
#endif
#if AY_APPLICATION_HAS_2D
#include <AYEntity/Entity2DIntegrationModule.h>
#endif
#if AY_APPLICATION_HAS_PHYSICS
#include <AYEntity/EntityPhysicsIntegrationModule.h>
#include <AYPhysics/PhysicsRuntimeModule.h>
#include <AYPhysics/PhysicsSubSystem.h>
#include <AYPhysics/PhysicsTypes.h>
#endif
#if AY_APPLICATION_HAS_SCRIPT
#include <AYEntity/EntityScriptIntegrationModule.h>
#include <AYScript/ScriptRuntimeModule.h>
#include <AYScript/ScriptSubSystem.h>
#endif

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace ayt::app
{
namespace
{

ayt::module::ModuleResult unavailableFeature(const char* feature)
{
    return ayt::module::ModuleResult::failure(
        ayt::module::ModuleErrorCode::InvalidDescriptor,
        std::string(feature) +
            " was requested but is disabled in this AYApplication build");
}

} // namespace

void registerEntityPresentationStack()
{
    ayt::entity::bootstrapEntityCore();
#if AY_APPLICATION_HAS_PRESENTATION
    ayt::render::RendererSubSystem::registerSubSystem();
    ayt::entity::registerEntityAnimationSystems();
    ayt::entity::registerEntityRenderSystems();
#if AY_APPLICATION_HAS_2D
    ayt::entity::registerEntity2DSystems();
#endif
#endif
}

void registerPhysicsModule()
{
#if AY_APPLICATION_HAS_PHYSICS
    registerPhysicsModule(ayt::physics::PhysicsBackendDescriptor{});
#endif
}

void registerPhysicsModule(const ayt::physics::PhysicsBackendDescriptor& desc)
{
#if AY_APPLICATION_HAS_PHYSICS
    if (ayt::physics::PhysicsSubSystem::findRegistered() == nullptr) {
        ayt::physics::PhysicsSubSystem::registerSubSystem(desc);
    }
    ayt::entity::registerEntityPhysicsIntegrationSubSystem();
#else
    (void)desc;
#endif
}

void registerDefaultClientModules(const ClientModuleOptions& options)
{
#if AY_APPLICATION_HAS_DEVICE
    ayt::device::DeviceConfig config{};
    config.window.title = options.windowTitle ? options.windowTitle : "Untitled";
    config.window.width = options.windowWidth;
    config.window.height = options.windowHeight;
    ayt::device::DeviceSubSystem::setBootstrapConfig(config);
    ayt::device::DeviceSubSystem::registerSubSystem();
#endif

    if (options.enablePresentation) {
#if AY_APPLICATION_HAS_PRESENTATION
        ayt::render::RendererSubSystem::setWindowProvider(
            ayt::device::DeviceSubSystem::makeWindowProvider());
        registerEntityPresentationStack();
#else
        std::fprintf(stderr, "[AYApplication] presentation is not built\n");
        ayt::entity::bootstrapEntityCore();
#endif
    } else {
        ayt::entity::bootstrapEntityCore();
    }

    if (options.enablePhysics) {
        registerPhysicsModule();
    }

#if AY_APPLICATION_HAS_SCRIPT
    if (options.enableScript) {
        ayt::game::GameLoop::instance().registerSubSystem(
            new ayt::script::ScriptSubSystem());
    }
#endif

#if AY_APPLICATION_HAS_AUDIO
    if (options.enableAudio) {
        auto audioSub = std::make_unique<ayt::audio::AudioSubSystem>();
        audioSub->setBackend(ayt::audio::makeMiniaudioBackend());
        ayt::game::GameLoop::instance().registerSubSystem(audioSub.release());
    }
#endif
}

void registerDefaultServerModules(const ServerModuleOptions& options)
{
    ayt::entity::bootstrapEntityCore();
    if (options.enablePhysics) {
        registerPhysicsModule();
    }
#if AY_APPLICATION_HAS_SCRIPT
    if (options.enableScript) {
        ayt::game::GameLoop::instance().registerSubSystem(
            new ayt::script::ScriptSubSystem());
    }
#endif
}

ayt::module::ModuleResult configureDefaultClientModules(
    EngineModuleRuntime& runtime,
    const ClientModuleOptions& options)
{
#if !AY_APPLICATION_HAS_AUDIO
    if (options.enableAudio) return unavailableFeature("Audio");
#endif
#if !AY_APPLICATION_HAS_PRESENTATION
    if (options.enablePresentation) return unavailableFeature("Presentation");
#endif
#if !AY_APPLICATION_HAS_PHYSICS
    if (options.enablePhysics) return unavailableFeature("Physics");
#endif
#if !AY_APPLICATION_HAS_SCRIPT
    if (options.enableScript) return unavailableFeature("Script");
#endif

#if AY_APPLICATION_HAS_DEVICE
    ayt::device::DeviceConfig config{};
    config.window.title = options.windowTitle ? options.windowTitle : "Untitled";
    config.window.width = options.windowWidth;
    config.window.height = options.windowHeight;
    if (auto result = runtime.modules().emplace<
            ayt::device::DeviceRuntimeModule>(std::move(config)); !result) {
        return result;
    }
#endif

    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityComponentModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityRuntimeModule>(); !result) {
        return result;
    }

#if AY_APPLICATION_HAS_PRESENTATION
    if (options.enablePresentation) {
        ayt::render::RendererSubSystem::setWindowProvider(
            ayt::device::DeviceSubSystem::makeWindowProvider());
        if (auto result = runtime.modules().emplace<
                ayt::render::RendererRuntimeModule>(); !result) {
            return result;
        }
        if (auto result = runtime.modules().emplace<
                ayt::entity::EntityAnimationIntegrationModule>(); !result) {
            return result;
        }
        if (auto result = runtime.modules().emplace<
                ayt::entity::EntityRenderIntegrationModule>(); !result) {
            return result;
        }
#if AY_APPLICATION_HAS_2D
        if (auto result = runtime.modules().emplace<
                ayt::entity::Entity2DIntegrationModule>(); !result) {
            return result;
        }
#endif
    }
#endif

#if AY_APPLICATION_HAS_PHYSICS
    if (options.enablePhysics) {
        if (auto result = runtime.modules().emplace<
                ayt::physics::PhysicsRuntimeModule>(); !result) {
            return result;
        }
        if (auto result = runtime.modules().emplace<
                ayt::entity::EntityPhysicsIntegrationModule>(); !result) {
            return result;
        }
    }
#endif

#if AY_APPLICATION_HAS_SCRIPT
    if (options.enableScript) {
        if (auto result = runtime.modules().emplace<
                ayt::entity::EntityScriptIntegrationModule>(); !result) {
            return result;
        }
        if (auto result = runtime.modules().emplace<
                ayt::script::ScriptRuntimeModule>(); !result) {
            return result;
        }
    }
#endif

#if AY_APPLICATION_HAS_AUDIO
    if (options.enableAudio) {
        if (auto result = runtime.modules().emplace<
                ayt::audio::AudioRuntimeModule>(
                    []() { return ayt::audio::makeMiniaudioBackend(); });
            !result) {
            return result;
        }
    }
#endif

    if (options.enableRuntimeSceneLoader) {
        ayt::scene::SceneManager* scenes = runtime.context().host().scenes();
        if (scenes == nullptr) {
            return ayt::module::ModuleResult::failure(
                ayt::module::ModuleErrorCode::InvalidDescriptor,
                "Client module graph requires a SceneManager service");
        }
        if (auto result = runtime.modules().emplace<RuntimeSceneLoaderModule>(
                *scenes,
                options.runtimeSceneLoaderConfig,
                &runtime.context().host().eventBus());
            !result) {
            return result;
        }
    }

    return ayt::module::ModuleResult::success();
}

ayt::module::ModuleResult configureDefaultServerModules(
    EngineModuleRuntime& runtime,
    const ServerModuleOptions& options)
{
#if !AY_APPLICATION_HAS_PHYSICS
    if (options.enablePhysics) return unavailableFeature("Physics");
#endif
#if !AY_APPLICATION_HAS_SCRIPT
    if (options.enableScript) return unavailableFeature("Script");
#endif

    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityComponentModule>(); !result) {
        return result;
    }
    if (auto result = runtime.modules().emplace<
            ayt::entity::EntityRuntimeModule>(); !result) {
        return result;
    }

#if AY_APPLICATION_HAS_PHYSICS
    if (options.enablePhysics) {
        if (auto result = runtime.modules().emplace<
                ayt::physics::PhysicsRuntimeModule>(); !result) {
            return result;
        }
        if (auto result = runtime.modules().emplace<
                ayt::entity::EntityPhysicsIntegrationModule>(); !result) {
            return result;
        }
    }
#endif

#if AY_APPLICATION_HAS_SCRIPT
    if (options.enableScript) {
        if (auto result = runtime.modules().emplace<
                ayt::entity::EntityScriptIntegrationModule>(); !result) {
            return result;
        }
        if (auto result = runtime.modules().emplace<
                ayt::script::ScriptRuntimeModule>(); !result) {
            return result;
        }
    }
#endif
    return ayt::module::ModuleResult::success();
}

} // namespace ayt::app
