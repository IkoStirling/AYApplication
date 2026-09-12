#ifndef UNICODE
#  define UNICODE
#endif
#include <Windows.h>

#include <AYApplication/UIFlowRuntime.h>
#include <AYApplication/UIFlowSceneBridge.h>
#include <AYApplication/UIManagerFlowScreenHost.h>
#include <AYDevice/DeviceManager.h>
#include <AYEntity.h>
#include <AYMath/MathTransform.h>
#include <AYRenderer.h>
#include <AYRenderer/RenderScene.h>
#include <AYRenderer/RenderTypes.h>
#include <AYRenderer/UIRenderBackend.h>
#include <AYScene.h>
#include <AYUI/DeviceInputBridge.h>
#include <AYUI/Theme.h>
#include <AYUI/UIManager.h>
#include <AYUI/Widget.h>
#include <AYEventSystem/EventBus.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef AY_UI_VERTICAL_SLICE_INTERACTIVE_DEFAULT
#  define AY_UI_VERTICAL_SLICE_INTERACTIVE_DEFAULT 0
#endif

namespace
{

namespace fs = std::filesystem;
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

constexpr std::string_view kSceneProbePhoskia = R"(
material UIFlowSceneProbe {
    uniform vec3 lightDir
    uniform vec3 lightColor

    vertex {
        in pos : position
        in nrm : normal
        out worldNormal : normal = (modelMatrix * vec4(nrm, 0.0)).xyz
        return modelViewProjection * vec4(pos, 1.0)
    }
    fragment {
        in worldNormal : normal
        let ndotl = max(dot(normalize(worldNormal), normalize(lightDir)), 0.18)
        return vec4(vec3(0.18, 0.58, 0.92) * lightColor * ndotl, 1.0)
    }
}
)";

struct SceneProbeItem {
    ayt::entity::Entity* entity = nullptr;
    ayt::math::FVector3 spin{};
};

class SceneProbe {
public:
    bool initialize(ayt::scene::Scene& scene)
    {
        ayt::entity::World::registerComponentType<ayt::entity::Transform>(
            "Transform");
        return add(scene, "floor", {0.0f, -1.35f, 0.0f},
                   {7.0f, 0.18f, 7.0f}, {})
            && add(scene, "hero-cube", {0.0f, 0.0f, 0.0f},
                   {1.25f, 1.25f, 1.25f}, {0.35f, 0.65f, 0.12f})
            && add(scene, "left-tower", {-2.7f, -0.25f, 1.4f},
                   {0.72f, 1.55f, 0.72f}, {0.0f, 0.22f, 0.0f})
            && add(scene, "right-tower", {2.5f, -0.42f, 0.35f},
                   {0.92f, 1.25f, 0.92f}, {0.0f, -0.18f, 0.0f})
            && add(scene, "gate", {0.0f, -0.55f, 3.1f},
                   {2.4f, 0.72f, 0.35f}, {});
    }

    void update(float elapsedSeconds)
    {
        for (const SceneProbeItem& item : _items) {
            if (item.entity == nullptr) continue;
            ayt::entity::Transform* transform =
                item.entity->getComponent<ayt::entity::Transform>();
            if (transform == nullptr) continue;
            transform->rotation = ayt::math::FQuaternion::fromEulerAngles({
                item.spin.x * elapsedSeconds,
                item.spin.y * elapsedSeconds,
                item.spin.z * elapsedSeconds,
            });
        }
    }

    void populate(ayt::render::RenderScene& renderScene,
                  ayt::render::MeshHandle mesh,
                  ayt::render::MaterialHandle material) const
    {
        renderScene.clear();
        for (const SceneProbeItem& item : _items) {
            if (item.entity == nullptr) continue;
            const ayt::entity::Transform* transform =
                item.entity->getComponent<ayt::entity::Transform>();
            if (transform == nullptr) continue;
            renderScene.add(mesh, material, ayt::math::Transform::getMatrix(
                transform->position, transform->rotation, transform->scale));
        }
    }

private:
    bool add(ayt::scene::Scene& scene,
             const char* name,
             const ayt::math::FVector3& position,
             const ayt::math::FVector3& scale,
             const ayt::math::FVector3& spin)
    {
        ayt::entity::Entity* entity = scene.world().createEntity();
        if (entity == nullptr) return false;
        entity->setName(name);
        ayt::entity::Transform* transform =
            entity->addComponent<ayt::entity::Transform>();
        if (transform == nullptr) return false;
        transform->position = position;
        transform->scale = scale;
        _items.push_back({entity, spin});
        return true;
    }

    std::vector<SceneProbeItem> _items;
};

bool hasSwitch(std::wstring_view commandLine, std::wstring_view name)
{
    return commandLine.find(name) != std::wstring_view::npos;
}

std::string readText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

const ayt::app::UIFlowMountedScreen* mountedScreen(
    const ayt::app::UIFlowRuntime& runtime, const std::string& screenId)
{
    for (const ayt::app::UIFlowMountedScreen& screen :
         runtime.mountedScreens()) {
        if (screen.screenId == screenId) return &screen;
    }
    return nullptr;
}

bool clickWidget(ayt::ui::DeviceInputBridge& input,
                 ayt::ui::UIManager& manager,
                 ayt::app::UIManagerFlowScreenHost& host,
                 const ayt::app::UIFlowRuntime& runtime,
                 const std::string& screenId,
                 const std::string& widgetId)
{
    const ayt::app::UIFlowMountedScreen* screen =
        mountedScreen(runtime, screenId);
    if (screen == nullptr) return false;
    manager.layout();
    ayt::ui::Widget* widget = host.findWidget(screen->mountId, widgetId);
    if (widget == nullptr) return false;
    const ayt::math::FRectangle bounds = widget->getWorldBounds();
    const ayt::math::FVector2 physical = manager.logicalToPhysical({
        (bounds.minX + bounds.maxX) * 0.5f,
        (bounds.minY + bounds.maxY) * 0.5f,
    });
    ayt::device::DeviceInputEvent event{};
    event.type = ayt::device::DeviceInputEventType::MouseMove;
    event.x = physical.x;
    event.y = physical.y;
    (void)input.dispatch(event);
    event.type = ayt::device::DeviceInputEventType::MouseButton;
    event.mouseButton = ayt::device::MouseButton::Left;
    event.pressed = true;
    const bool down = input.dispatch(event);
    event.pressed = false;
    return input.dispatch(event) || down;
}

bool capture(ayt::render::Renderer& renderer,
             ayt::render::UIRenderBackend& uiBackend,
             const fs::path& outputRoot,
             const char* name,
             int frame,
             std::size_t sceneItems)
{
    const std::string base = (outputRoot / name).string();
    const bool queued = renderer.captureScreenshot(base);
    std::ofstream metrics(base + ".metrics.txt",
                          std::ios::binary | std::ios::trunc);
    metrics << "scenario=" << name << '\n'
            << "frame=" << frame << '\n'
             << "framebuffer=" << kWidth << 'x' << kHeight << '\n'
             << "backend=d3d11\n"
             << "sceneItems=" << sceneItems << '\n'
             << "drawCalls=" << uiBackend.getDrawCallCount() << '\n'
            << "queued=" << (queued ? "yes" : "no") << '\n';
    return queued;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    const bool interactive = AY_UI_VERTICAL_SLICE_INTERACTIVE_DEFAULT != 0
        || hasSwitch(::GetCommandLineW(), L"--interactive");
    fs::path outputRoot(AY_UI_VERTICAL_SLICE_CAPTURE_ROOT);
    char supplied[32768] = {};
    const DWORD suppliedLength = ::GetEnvironmentVariableA(
        "AY_UI_VERTICAL_CAPTURE_DIR", supplied,
        static_cast<DWORD>(sizeof(supplied)));
    if (suppliedLength > 0u && suppliedLength < sizeof(supplied)) {
        outputRoot = supplied;
    }
    std::error_code directoryError;
    fs::create_directories(outputRoot, directoryError);
    if (directoryError) return 10;

    ayt::device::DeviceManager devices;
    ayt::device::DeviceConfig deviceConfig{};
    deviceConfig.window.title = interactive
        ? "AYUI Scene + Flow Integration (F1/F2/F3, F9, Esc)"
        : "AYUI Production Vertical Slice";
    deviceConfig.window.width = kWidth;
    deviceConfig.window.height = kHeight;
    deviceConfig.window.resizable = false;
    deviceConfig.window.hidden = !interactive;
    if (!devices.initialize(deviceConfig)) return 11;

    ayt::render::Renderer renderer;
    ayt::render::InitDesc renderConfig{};
    renderConfig.windowHandle = devices.window().getWindowHandle();
    renderConfig.width = kWidth;
    renderConfig.height = kHeight;
    renderConfig.vsync = interactive;
    renderConfig.backend = ayt::render::Backend::Direct3D11;
    renderConfig.msaa = 0;
    if (!renderer.initialize(renderConfig)) {
        devices.shutdown();
        return 12;
    }

    ayt::render::UIRenderBackend uiBackend;
    if (!uiBackend.initialize(renderer)) {
        renderer.shutdown();
        devices.shutdown();
        return 13;
    }
    uiBackend.setFramebufferSize(kWidth, kHeight);

    ayt::render::MeshHandle sceneMesh = renderer.createUnitCube();
    ayt::render::MaterialHandle sceneMaterial =
        renderer.createMaterialFromPhoskia(
            std::string(kSceneProbePhoskia), "ui-flow-scene-probe");
    if (!sceneMesh.isValid() || !sceneMaterial.isValid()) {
        if (sceneMesh.isValid()) renderer.destroyMesh(sceneMesh);
        if (sceneMaterial.isValid()) renderer.destroyMaterial(sceneMaterial);
        uiBackend.shutdown();
        renderer.shutdown();
        devices.shutdown();
        return 19;
    }
    renderer.setViewportRect(0, 0, kWidth, kHeight);
    renderer.setMainCameraLookAtPerspective(
        {6.2f, 4.2f, 8.4f}, {0.0f, -0.15f, 0.6f}, {0.0f, 1.0f, 0.0f},
        50.0f, static_cast<float>(kWidth) / static_cast<float>(kHeight),
        0.1f, 100.0f);
    renderer.setDirectionalLight(
        {0.35f, -0.85f, -0.40f}, {1.0f, 0.96f, 0.88f});

    int result = 0;
    ayt::ui::UIManager manager;
    manager.initialize(&uiBackend);
    manager.setClientSize(static_cast<float>(kWidth),
                          static_cast<float>(kHeight));
    ayt::ui::ThemeManager::get().ensureDefaultThemes();
    ayt::ui::ThemeManager::get().setActiveTheme("dark");
    {
        const fs::path assetRoot(AY_UI_VERTICAL_SLICE_ASSET_ROOT);
        ayt::ui::UIFlowDocument document;
        std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
        const std::string encoded = readText(assetRoot / "production.uiflow.json");
        if (encoded.empty()
            || !ayt::ui::UIFlowSerializer::deserialize(
                encoded, document, &diagnostics)) {
            result = 14;
        } else {
            ayt::app::UIManagerFlowScreenHost host(manager, assetRoot.string());
            ayt::app::UIFlowRuntime runtime(host);
            ayt::event::EventBus eventBus;
            ayt::scene::Scene town(ayt::scene::SceneMode::Play, "town");
            SceneProbe sceneProbe;
            ayt::app::UIFlowSceneBridgeConfig bridgeConfig;
            bridgeConfig.worldKeyResolver = [](const ayt::scene::Scene& scene) {
                return scene.name();
            };
            bridgeConfig.worldContexts.push_back({"town", "WorldHud"});
            ayt::app::UIFlowSceneBridge sceneBridge(
                runtime, eventBus, std::move(bridgeConfig));
            ayt::ui::DeviceInputBridge input(manager);
            std::string error;
            if (!sceneProbe.initialize(town)) {
                result = 20;
            } else if (!runtime.load(std::move(document), &error)
                || !runtime.start({}, &error)) {
                result = 15;
            } else {
                bool allCapturesQueued = true;
                bool running = true;
                bool sceneStarted = false;
                bool interactiveCaptureRequested = false;
                ayt::device::DeviceInputListenerId shortcutListener = 0;
                if (interactive) {
                    input.connect(devices);
                    input.bindTextInputFocus(manager);
                    shortcutListener = devices.addInputListener(
                        [&](const ayt::device::DeviceInputEvent& event) {
                            if (event.type != ayt::device::DeviceInputEventType::Key
                                || !event.pressed || event.repeat) {
                                return;
                            }
                            bool commandOk = true;
                            switch (event.key) {
                            case ayt::device::KeyCode::F1:
                                if (sceneStarted) {
                                    commandOk = sceneBridge.emitSceneSignal(
                                        runtime.activeState("story") == "visible"
                                            ? "zone.exit" : "zone.enter",
                                        {}, &error);
                                }
                                break;
                            case ayt::device::KeyCode::F2:
                                if (sceneStarted) {
                                    commandOk = runtime.emitSignal(
                                        runtime.activeState("notice") == "visible"
                                            ? "notice.hide" : "notice.show",
                                        {}, &error);
                                }
                                break;
                            case ayt::device::KeyCode::F3:
                                if (sceneStarted) {
                                    commandOk = runtime.emitSignal(
                                        runtime.activeState("modal") == "paused"
                                            ? "pause.close" : "pause.open",
                                        {}, &error);
                                }
                                break;
                            case ayt::device::KeyCode::F9:
                                interactiveCaptureRequested = true;
                                break;
                            case ayt::device::KeyCode::Escape:
                                running = false;
                                break;
                            default:
                                break;
                            }
                            if (!commandOk) {
                                std::fprintf(stderr,
                                    "[UI scene integration] command failed: %s\n",
                                    error.c_str());
                            }
                        });
                }

                ayt::render::RenderScene renderScene;
                int frame = 0;
                while (running && result == 0 && (interactive || frame < 72)) {
                    ++frame;
                    devices.pollEvents();
                    if (devices.window().consumeCloseRequested()) {
                        running = false;
                        continue;
                    }

                    if (!interactive && frame == 20) {
                        if (!clickWidget(input, manager, host, runtime,
                                         "menu", "menu_start")
                            || !sceneBridge.start(&town, &error)) {
                            result = 16;
                            break;
                        }
                        sceneStarted = true;
                    } else if (!interactive && frame == 44) {
                        if (!sceneBridge.emitSceneSignal(
                                "zone.enter", {}, &error)
                            || !runtime.emitSignal(
                                "notice.show", {}, &error)
                            || !runtime.emitSignal(
                                "pause.open", {}, &error)) {
                            result = 17;
                            break;
                        }
                    }

                    if (interactive && !sceneStarted
                        && runtime.activeState("application") == "game") {
                        if (!sceneBridge.start(&town, &error)) {
                            result = 16;
                            break;
                        }
                        sceneStarted = true;
                    }

                    constexpr float deltaSeconds = 1.0f / 60.0f;
                    if (sceneStarted) {
                        town.tick(deltaSeconds);
                        sceneProbe.update(static_cast<float>(frame) * deltaSeconds);
                        sceneProbe.populate(renderScene, sceneMesh, sceneMaterial);
                    } else {
                        renderScene.clear();
                    }
                    host.update(deltaSeconds);
                    manager.update(deltaSeconds);
                    manager.layout();
                    ayt::render::ClearDesc clear{};
                    clear.r = 0.055f;
                    clear.g = 0.065f;
                    clear.b = 0.085f;
                    clear.a = 1.0f;
                    renderer.beginCompositeFrame(
                        clear, static_cast<std::uint16_t>(kWidth),
                        static_cast<std::uint16_t>(kHeight));
                    manager.populateFrame();
                    renderer.render(renderScene);
                    manager.flushFrame();
                    if (!interactive && frame == 12) {
                        allCapturesQueued = capture(
                            renderer, uiBackend, outputRoot,
                            "vertical_boot", frame, renderScene.items().size())
                            && allCapturesQueued;
                    } else if (!interactive && frame == 36) {
                        allCapturesQueued = capture(
                            renderer, uiBackend, outputRoot,
                            "vertical_gameplay", frame, renderScene.items().size())
                            && allCapturesQueued;
                    } else if (!interactive && frame == 64) {
                        allCapturesQueued = capture(
                            renderer, uiBackend, outputRoot,
                            "vertical_parallel_modal", frame,
                            renderScene.items().size())
                            && allCapturesQueued;
                    } else if (interactive && interactiveCaptureRequested) {
                        (void)capture(renderer, uiBackend, outputRoot,
                                      "interactive_scene_ui", frame,
                                      renderScene.items().size());
                        interactiveCaptureRequested = false;
                    }
                    renderer.endFrame();
                }
                if (result == 0 && !allCapturesQueued) result = 18;
                if (shortcutListener != 0) {
                    devices.removeInputListener(shortcutListener);
                }
                input.disconnect();
                sceneBridge.stop();
                runtime.unload();
            }
        }
    }
    manager.shutdown();
    renderer.destroyMesh(sceneMesh);
    renderer.destroyMaterial(sceneMaterial);
    uiBackend.shutdown();
    renderer.shutdown();
    devices.shutdown();
    return result;
}
