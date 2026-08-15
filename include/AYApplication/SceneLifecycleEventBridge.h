#pragma once
// AYApplication/SceneLifecycleEventBridge.h — SceneManager observer → EventBus Scene* events

#include <AYScene/SceneLifecycleObserver.h>

namespace ayt::app
{

/// Process-wide bridge installed by bindBuiltinHostServices.
/// Forwards SceneManager lifecycle to EventBus Scene* events.
class SceneLifecycleEventBridge final : public ayt::scene::ISceneLifecycleObserver {
public:
    static SceneLifecycleEventBridge& instance();

    void onCurrentChanged(ayt::scene::Scene* current) override;
    void onBeginPlay(ayt::scene::Scene* play) override;
    void onEndPlay(ayt::scene::Scene* editOrNull) override;
};

} // namespace ayt::app
