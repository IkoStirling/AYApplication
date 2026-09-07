#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RegisterDefaultModules.h>

int main()
{
    static_assert(!ayt::app::kApplicationHasAudio);
    static_assert(!ayt::app::kApplicationHasDevice);
    static_assert(!ayt::app::kApplicationHasPresentation);
    static_assert(!ayt::app::kApplicationHas2D);
    static_assert(!ayt::app::kApplicationHasPhysics);
    static_assert(!ayt::app::kApplicationHasScript);
    static_assert(!ayt::app::kApplicationHasNetwork);

    {
        ayt::app::EngineModuleRuntime unavailableRuntime(
            ayt::app::defaultEngineHost());
        ayt::app::ClientModuleOptions unavailableOptions{};
        unavailableOptions.enablePresentation = true;
        unavailableOptions.enableRuntimeSceneLoader = false;
        if (ayt::app::configureDefaultClientModules(
                unavailableRuntime, unavailableOptions)) {
            return 1;
        }
    }

    ayt::app::EngineModuleRuntime runtime(ayt::app::defaultEngineHost());
    ayt::app::ServerModuleOptions options{};
    options.enablePhysics = false;
    options.enableScript = false;

    if (!ayt::app::configureDefaultServerModules(runtime, options)) {
        return 2;
    }
    if (!runtime.start()) {
        runtime.shutdown();
        return 3;
    }

    runtime.shutdown();
    return 0;
}
