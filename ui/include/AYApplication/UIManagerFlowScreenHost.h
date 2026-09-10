#pragma once

#include <AYApplication/UIFlowRuntime.h>

#include <memory>
#include <string>

namespace ayt::ui
{
class UIManager;
class Widget;
}

namespace ayt::app
{

// Production Widget-tree adapter. The referenced UIManager must be initialized
// before mounting and must outlive this host. The adapter owns its Flow subtree
// and attaches it externally to the current UIManager root, so replacing that
// root detaches rather than destroys mounted Screens; update() reattaches them.
// Each mounted Screen receives its own UILayoutLoader, preserving screen-local
// IDs and animation libraries.
class UIManagerFlowScreenHost final : public IUIFlowScreenHost
{
public:
    explicit UIManagerFlowScreenHost(
        ayt::ui::UIManager& manager,
        std::string assetRoot = {});
    ~UIManagerFlowScreenHost() override;

    UIManagerFlowScreenHost(const UIManagerFlowScreenHost&) = delete;
    UIManagerFlowScreenHost& operator=(const UIManagerFlowScreenHost&) = delete;

    bool mountScreen(
        const UIFlowScreenMountRequest& request,
        std::string& error) override;
    void unmountScreen(std::uint64_t mountId) noexcept override;
    void setScreenOrder(
        std::uint64_t mountId,
        int layerOrder,
        std::uint32_t orderInLayer) noexcept override;
    void update(float deltaSeconds) override;

    [[nodiscard]] ayt::ui::Widget* screenRoot(
        std::uint64_t mountId) const noexcept;
    [[nodiscard]] ayt::ui::Widget* findWidget(
        std::uint64_t mountId,
        const std::string& widgetId) const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app
