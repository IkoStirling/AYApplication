#include <AYSceneLifecycleEventBridge.h>

#include <IEngineHost.h>

#include <ayevent/EventBus.h>
#include <ayevent/Events/SceneEvents.h>

namespace ayt::app
{

SceneLifecycleEventBridge& SceneLifecycleEventBridge::instance()
{
    static SceneLifecycleEventBridge bridge;
    return bridge;
}

void SceneLifecycleEventBridge::onCurrentChanged(ayt::scene::Scene* current)
{
    ayt::event::SceneCurrentChangedEvent ev;
    ev.current = current;
    resolveEventBus().emit(ev);
}

void SceneLifecycleEventBridge::onBeginPlay(ayt::scene::Scene* play)
{
    ayt::event::SceneBeginPlayEvent ev;
    ev.play = play;
    resolveEventBus().emit(ev);
}

void SceneLifecycleEventBridge::onEndPlay(ayt::scene::Scene* editOrNull)
{
    ayt::event::SceneEndPlayEvent ev;
    ev.edit = editOrNull;
    resolveEventBus().emit(ev);
}

} // namespace ayt::app
