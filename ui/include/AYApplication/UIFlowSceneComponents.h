#pragma once

#include <AYCore.h>
#include <AYEntity/IEntity.h>
#include <AYMath/MathTypes.h>

#include <string>

namespace ayt::entity
{
class ComponentRegistry;
class ComponentRegistryResult;
}

namespace ayt::app
{

// An authored, axis-aligned Scene region which emits generic Flow Signals.
// It deliberately carries no gameplay category or Widget reference. Rotation
// is ignored in Stage 3; scale is applied to halfExtents by the bridge.
#define AY_CURRENT_CLASS SceneSignalVolumeComponent
struct SceneSignalVolumeComponent final : public ayt::entity::IComponent
{
    const char* getName() const override
    {
        return "SceneSignalVolumeComponent";
    }

    AY_PROPERTY(std::string, sourceId, ayt::entity::kAttrSerialize)
    AY_PROPERTY(std::string, enterSignal, ayt::entity::kAttrSerialize)
    AY_PROPERTY(std::string, exitSignal, ayt::entity::kAttrSerialize)
    AY_PROPERTY(std::string, participantTag, ayt::entity::kAttrSerialize)
    AY_PROPERTY(
        ayt::math::FVector3, halfExtents, ayt::entity::kAttrSerialize)
    AY_PROPERTY(ayt::math::FVector3, offset, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, enabled, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, emitOncePerWorld, ayt::entity::kAttrSerialize)

    SceneSignalVolumeComponent()
    {
        halfExtents = ayt::math::FVector3(1.0f, 1.0f, 1.0f);
        offset = ayt::math::FVector3(0.0f, 0.0f, 0.0f);
        enabled = true;
        emitOncePerWorld = false;
    }
};
#undef AY_CURRENT_CLASS

// Marks an Entity as a point tested against SceneSignalVolumeComponent.
// The tag is an optional exact-match channel such as "player" or "camera".
#define AY_CURRENT_CLASS SceneSignalParticipantComponent
struct SceneSignalParticipantComponent final : public ayt::entity::IComponent
{
    const char* getName() const override
    {
        return "SceneSignalParticipantComponent";
    }

    AY_PROPERTY(std::string, tag, ayt::entity::kAttrSerialize)
    AY_PROPERTY(bool, enabled, ayt::entity::kAttrSerialize)

    SceneSignalParticipantComponent() { enabled = true; }
};
#undef AY_CURRENT_CLASS

[[nodiscard]] ayt::entity::ComponentRegistryResult
registerUIFlowSceneComponents(ayt::entity::ComponentRegistry& registry);

} // namespace ayt::app
