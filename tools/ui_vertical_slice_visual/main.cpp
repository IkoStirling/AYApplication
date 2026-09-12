#ifndef UNICODE
#  define UNICODE
#endif
#include <Windows.h>

#include <AYApplication/UIFlowRuntime.h>
#include <AYApplication/UIFlowSceneBridge.h>
#include <AYApplication/UIManagerFlowScreenHost.h>
#include <AYDevice/DeviceManager.h>
#include <AYRenderer.h>
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

namespace
{

namespace fs = std::filesystem;
constexpr int kWidth = 1280;
constexpr int kHeight = 720;

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
             int frame)
{
    const std::string base = (outputRoot / name).string();
    const bool queued = renderer.captureScreenshot(base);
    std::ofstream metrics(base + ".metrics.txt",
                          std::ios::binary | std::ios::trunc);
    metrics << "scenario=" << name << '\n'
            << "frame=" << frame << '\n'
            << "framebuffer=" << kWidth << 'x' << kHeight << '\n'
            << "backend=d3d11\n"
            << "drawCalls=" << uiBackend.getDrawCallCount() << '\n'
            << "queued=" << (queued ? "yes" : "no") << '\n';
    return queued;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
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
    deviceConfig.window.title = "AYUI Production Vertical Slice";
    deviceConfig.window.width = kWidth;
    deviceConfig.window.height = kHeight;
    deviceConfig.window.hidden = true;
    if (!devices.initialize(deviceConfig)) return 11;

    ayt::render::Renderer renderer;
    ayt::render::InitDesc renderConfig{};
    renderConfig.windowHandle = devices.window().getWindowHandle();
    renderConfig.width = kWidth;
    renderConfig.height = kHeight;
    renderConfig.vsync = false;
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
            ayt::app::UIFlowSceneBridgeConfig bridgeConfig;
            bridgeConfig.worldKeyResolver = [](const ayt::scene::Scene& scene) {
                return scene.name();
            };
            bridgeConfig.worldContexts.push_back({"town", "WorldHud"});
            ayt::app::UIFlowSceneBridge sceneBridge(
                runtime, eventBus, std::move(bridgeConfig));
            ayt::ui::DeviceInputBridge input(manager);
            std::string error;
            if (!runtime.load(std::move(document), &error)
                || !runtime.start({}, &error)) {
                result = 15;
            } else {
                bool allCapturesQueued = true;
                for (int frame = 1; frame <= 72; ++frame) {
                    devices.pollEvents();
                    if (frame == 20) {
                        if (!clickWidget(input, manager, host, runtime,
                                         "menu", "menu_start")
                            || !sceneBridge.start(&town, &error)) {
                            result = 16;
                            break;
                        }
                    } else if (frame == 44) {
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

                    constexpr float deltaSeconds = 1.0f / 60.0f;
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
                    manager.flushFrame();
                    if (frame == 12) {
                        allCapturesQueued = capture(
                            renderer, uiBackend, outputRoot,
                            "vertical_boot", frame) && allCapturesQueued;
                    } else if (frame == 36) {
                        allCapturesQueued = capture(
                            renderer, uiBackend, outputRoot,
                            "vertical_gameplay", frame) && allCapturesQueued;
                    } else if (frame == 64) {
                        allCapturesQueued = capture(
                            renderer, uiBackend, outputRoot,
                            "vertical_parallel_modal", frame)
                            && allCapturesQueued;
                    }
                    renderer.endFrame();
                }
                if (result == 0 && !allCapturesQueued) result = 18;
                sceneBridge.stop();
                runtime.unload();
            }
        }
    }
    manager.shutdown();
    uiBackend.shutdown();
    renderer.shutdown();
    devices.shutdown();
    return result;
}
