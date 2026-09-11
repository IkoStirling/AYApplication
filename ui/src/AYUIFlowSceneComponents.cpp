#include <AYApplication/UIFlowSceneComponents.h>

#include <AYEntity/ComponentRegistration.h>
#include <AYEntity/ComponentRegistry.h>

namespace ayt::app
{

AY_FINALIZE_REGISTRATION_METADATA(SceneSignalVolumeComponent)
AY_FINALIZE_REGISTRATION_METADATA(SceneSignalParticipantComponent)

ayt::entity::ComponentRegistryResult registerUIFlowSceneComponents(
    ayt::entity::ComponentRegistry& registry)
{
    using namespace ayt::entity;
    if (ComponentRegistryResult result =
            registerSceneComponent<SceneSignalVolumeComponent>(
                registry,
                "SceneSignalVolumeComponent",
                "Scene Signal Volume",
                "Flow & Interaction");
        !result) {
        return result;
    }
    return registerSceneComponent<SceneSignalParticipantComponent>(
        registry,
        "SceneSignalParticipantComponent",
        "Scene Signal Participant",
        "Flow & Interaction");
}

} // namespace ayt::app
