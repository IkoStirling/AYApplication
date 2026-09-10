#include <AYApplication/UIFlowRuntimeModule.h>

#include <AYApplication/IEngineHost.h>

#include <memory>
#include <utility>

namespace ayt::app
{
namespace
{

class UIFlowSubSystem final : public ayt::game::ISubSystem
{
public:
    UIFlowSubSystem(
        ayt::ui::UIFlowDocument document,
        std::unique_ptr<IUIFlowScreenHost> screenHost,
        std::string entry)
        : _document(std::move(document)),
          _screenHost(std::move(screenHost)),
          _runtime(*_screenHost),
          _entry(std::move(entry))
    {
    }

    const char* getName() const override
    {
        return kUIFlowRuntimeSubSystemName.data();
    }

    const ayt::game::SubSystemDescriptor& getDescriptor() const override
    {
        static const ayt::game::SubSystemDescriptor descriptor{
            .name = kUIFlowRuntimeSubSystemName.data(),
            .basePriority = 0,
            .timeType = ayt::game::SubSystemDescriptor::TimeType::Unscaled,
            .phases = ayt::game::phaseBit(
                ayt::game::FramePhase::Presentation),
            .clock = ayt::game::ClockDomain::Unscaled,
            .phasePriority = -100,
            .reads = {"Application.UIFlowCommands"},
            .writes = {"Application.UIFlowPresentation"},
        };
        return descriptor;
    }

    bool initialize() override
    {
        if (_initialized) return true;
        std::string error;
        if (!_runtime.load(_document, &error)
            || !_runtime.start(_entry, &error)) {
            _runtime.unload();
            return false;
        }
        _initialized = true;
        return true;
    }

    void update(float deltaTime) override
    {
        if (_initialized) _screenHost->update(deltaTime);
    }

    void fixedUpdate(float fixedDeltaTime) override
    {
        (void)fixedDeltaTime;
    }

    void shutdown() override
    {
        if (!_initialized) return;
        _runtime.unload();
        _initialized = false;
    }

    UIFlowRuntime& runtime() noexcept { return _runtime; }

private:
    ayt::ui::UIFlowDocument _document;
    std::unique_ptr<IUIFlowScreenHost> _screenHost;
    UIFlowRuntime _runtime;
    std::string _entry;
    bool _initialized = false;
};

struct UIFlowModuleSeed
{
    ayt::ui::UIFlowDocument document;
    std::unique_ptr<IUIFlowScreenHost> screenHost;
    std::string entry;
};

} // namespace

UIFlowRuntimeModule::UIFlowRuntimeModule(
    IEngineHost& host,
    ayt::ui::UIFlowDocument document,
    std::unique_ptr<IUIFlowScreenHost> screenHost,
    std::string entry)
    : SubSystemModule(
          ayt::module::ModuleDescriptor{
              .id = std::string(kUIFlowRuntimeModuleId),
              .displayName = "AYApplication UI Flow Runtime",
              .version = "0.2.0",
          },
          std::string(kUIFlowRuntimeSubSystemName),
          [seed = std::make_shared<UIFlowModuleSeed>(UIFlowModuleSeed{
               std::move(document), std::move(screenHost), std::move(entry)})]()
              -> std::unique_ptr<ayt::game::ISubSystem> {
              if (!seed->screenHost) return {};
              return std::make_unique<UIFlowSubSystem>(
                  std::move(seed->document),
                  std::move(seed->screenHost),
                  std::move(seed->entry));
          },
          [&host](ayt::module::IModuleContext&,
                  ayt::game::ISubSystem& system) {
              auto* typed = dynamic_cast<UIFlowSubSystem*>(&system);
              if (typed == nullptr) {
                  return ayt::module::ModuleResult::failure(
                      ayt::module::ModuleErrorCode::InstallationFailed,
                      "UIFlowRuntime module received an incompatible subsystem");
              }
              host.provide(kHostServiceUIFlowRuntime, &typed->runtime());
              return ayt::module::ModuleResult::success();
          }),
      _host(host)
{
}

void UIFlowRuntimeModule::shutdown(
    ayt::module::IModuleContext& context) noexcept
{
    UIFlowRuntime* published = uiFlowRuntime(_host);
    auto* installed = dynamic_cast<UIFlowSubSystem*>(installedSubSystem());
    if (installed != nullptr && published == &installed->runtime()) {
        try {
            _host.provideService(kHostServiceUIFlowRuntime, nullptr);
        } catch (...) {
            // Module teardown must remain noexcept. A host that rejects the
            // clear owns final service-table cleanup.
        }
    }
    SubSystemModule::shutdown(context);
}

UIFlowRuntime* uiFlowRuntime(IEngineHost& host) noexcept
{
    try {
        return host.service<UIFlowRuntime>(kHostServiceUIFlowRuntime);
    } catch (...) {
        return nullptr;
    }
}

} // namespace ayt::app
