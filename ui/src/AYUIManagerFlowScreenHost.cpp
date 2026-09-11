#include <AYApplication/UIManagerFlowScreenHost.h>

#include <AYUI/LayoutLoader.h>
#include <AYUI/UIManager.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ayt::app
{
namespace
{

class FlowLayerWidget final : public ayt::ui::CompoundWidget
{
public:
    ayt::ui::UIFlowInputPolicy inputPolicy =
        ayt::ui::UIFlowInputPolicy::ConsumeHandled;
    bool blocksLowerInput = false;

    bool retriesUnhandledPointerWithinChildren() const override
    {
        return !blocksLowerPointerInput();
    }

    bool allowsUnhandledPointerRetryBehind() const override
    {
        return !blocksLowerPointerInput();
    }

    bool blocksLowerPointerInput() const override
    {
        return blocksLowerInput
            || inputPolicy == ayt::ui::UIFlowInputPolicy::BlockLower;
    }

    ayt::ui::Widget* hitTest(const math::FVector2& worldPos) override
    {
        if (!isVisible() || !getWorldBounds().contains(worldPos)) {
            return nullptr;
        }

        const auto& children = getChildren();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (*it == nullptr) continue;
            if (ayt::ui::Widget* hit = (*it)->hitTest(worldPos)) {
                // A Screen root is a viewport-sized document surface. Treat
                // its otherwise inert background as transparent for a
                // pass-through Layer; interactive descendants still win.
                if (hit == *it
                    && inputPolicy
                        == ayt::ui::UIFlowInputPolicy::PassThrough
                    && !blocksLowerInput) {
                    continue;
                }
                return hit;
            }
        }

        // PassThrough and ConsumeHandled expose only actual Screen content.
        // BlockLower (or the explicit layer flag) deliberately returns this
        // transparent layer surface so UIManager does not descend into a
        // lower Flow layer or non-Flow root child.
        if (blocksLowerInput
            || inputPolicy == ayt::ui::UIFlowInputPolicy::BlockLower) {
            return this;
        }
        return nullptr;
    }
};

class FlowRootWidget final : public ayt::ui::CompoundWidget
{
public:
    bool retriesUnhandledPointerWithinChildren() const override { return true; }
    bool allowsUnhandledPointerRetryBehind() const override { return true; }

    ayt::ui::Widget* hitTest(const math::FVector2& worldPos) override
    {
        if (!isVisible() || !getWorldBounds().contains(worldPos)) {
            return nullptr;
        }
        const auto& children = getChildren();
        for (auto it = children.rbegin(); it != children.rend(); ++it) {
            if (*it == nullptr) continue;
            if (ayt::ui::Widget* hit = (*it)->hitTest(worldPos)) {
                return hit;
            }
        }
        // The orchestration root itself must never swallow input from other
        // application UI mounted below it.
        return nullptr;
    }
};

bool pathEscapesRoot(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate)
{
    const std::filesystem::path relative = candidate.lexically_relative(root);
    if (relative.empty()) return candidate != root;
    const auto first = relative.begin();
    return first != relative.end() && *first == "..";
}

} // namespace

class UIManagerFlowScreenHost::Impl
{
public:
    struct LayerRecord
    {
        std::string id;
        int order = 0;
        std::uint64_t serial = 0;
        FlowLayerWidget* widget = nullptr;
    };

    struct MountRecord
    {
        UIFlowScreenMountRequest request;
        std::unique_ptr<ayt::ui::UILayoutLoader> loader;
        ayt::ui::Widget* root = nullptr;
        LayerRecord* layer = nullptr;
        std::optional<ayt::ui::AnimationTimeline> animation;
    };

    explicit Impl(ayt::ui::UIManager& value, std::string root,
                  ayt::ui::Widget* parent)
        : manager(value), assetRoot(std::move(root)), mountParent(parent)
    {
    }

    ~Impl()
    {
        shutdown();
    }

    bool ensureRoot(std::string& error)
    {
        ayt::ui::Widget* parent = mountParent != nullptr
            ? mountParent : manager.root();
        if (parent == nullptr) {
            error = "UIManager must be initialized before UI Flow mounting.";
            return false;
        }
        if (flowRoot == nullptr) {
            flowRoot = new FlowRootWidget();
            flowRoot->setId("__ay_ui_flow_root");
            flowRoot->setLayoutPositionManaged(false);
            flowRoot->setLayoutSizeManaged(false);
            flowRoot->setPosition(math::FVector2(0.0f, 0.0f));
            flowRoot->setSize(viewportSize());
        }

        // UIManager owns and may replace its document root. The Flow host
        // owns this orchestration subtree, so attach it as an external child:
        // root teardown safely detaches it instead of deleting it. A later
        // update can then reattach the same mounted Screens to the new root.
        if (flowRoot->getParent() != parent) {
            parent->addChildExternal(flowRoot);
            lastViewport = math::FVector2(-1.0f, -1.0f);
        }
        return true;
    }

    math::FVector2 viewportSize() const
    {
        return mountParent != nullptr
            ? mountParent->getSize() : manager.getClientSize();
    }

    std::filesystem::path resolveAsset(
        const std::string& asset,
        std::string& error) const
    {
        namespace fs = std::filesystem;
        if (asset.empty()) {
            error = "Screen layoutAsset must not be empty.";
            return {};
        }

        std::error_code ec;
        fs::path supplied(asset);
        if (assetRoot.empty()) {
            const fs::path resolved = fs::weakly_canonical(supplied, ec);
            if (ec) {
                error = "Cannot resolve Screen layoutAsset.";
                return {};
            }
            return resolved;
        }
        if (supplied.is_absolute()) {
            error = "Absolute layoutAsset paths are not allowed with an asset root.";
            return {};
        }

        const fs::path root = fs::weakly_canonical(fs::path(assetRoot), ec);
        if (ec) {
            error = "Cannot resolve the UI Flow asset root.";
            return {};
        }
        const fs::path candidate = fs::weakly_canonical(root / supplied, ec);
        if (ec || pathEscapesRoot(root, candidate)) {
            error = "layoutAsset escapes the configured UI Flow asset root.";
            return {};
        }
        return candidate;
    }

    LayerRecord* ensureLayer(
        const UIFlowScreenMountRequest& request,
        std::string& error)
    {
        if (!ensureRoot(error)) return nullptr;
        auto found = layers.find(request.layerId);
        if (found != layers.end()) {
            found->second->order = request.layerOrder;
            found->second->widget->inputPolicy = request.inputPolicy;
            found->second->widget->blocksLowerInput = request.blocksLowerInput;
            sortLayers();
            return found->second.get();
        }
        auto value = std::make_unique<LayerRecord>();
        value->id = request.layerId;
        value->order = request.layerOrder;
        value->serial = nextLayerSerial++;
        value->widget = new FlowLayerWidget();
        value->widget->setId("__ay_ui_flow_layer_" + request.layerId);
        value->widget->inputPolicy = request.inputPolicy;
        value->widget->blocksLowerInput = request.blocksLowerInput;
        value->widget->setLayoutPositionManaged(false);
        value->widget->setLayoutSizeManaged(false);
        value->widget->setPosition(math::FVector2(0.0f, 0.0f));
        value->widget->setSize(viewportSize());
        flowRoot->addChild(value->widget);
        LayerRecord* raw = value.get();
        layers.emplace(request.layerId, std::move(value));
        sortLayers();
        return raw;
    }

    void sortLayers()
    {
        if (flowRoot == nullptr) return;
        std::vector<LayerRecord*> ordered;
        ordered.reserve(layers.size());
        for (auto& pair : layers) ordered.push_back(pair.second.get());
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const LayerRecord* lhs, const LayerRecord* rhs) {
                if (lhs->order != rhs->order) return lhs->order < rhs->order;
                return lhs->serial < rhs->serial;
            });
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            flowRoot->moveChildToIndex(ordered[index]->widget, index);
        }
    }

    void sortScreens(LayerRecord& layer)
    {
        std::vector<MountRecord*> ordered;
        for (auto& pair : mounts) {
            if (pair.second.layer == &layer) ordered.push_back(&pair.second);
        }
        std::stable_sort(ordered.begin(), ordered.end(),
            [](const MountRecord* lhs, const MountRecord* rhs) {
                if (lhs->request.orderInLayer
                    != rhs->request.orderInLayer) {
                    return lhs->request.orderInLayer
                        < rhs->request.orderInLayer;
                }
                return lhs->request.mountId < rhs->request.mountId;
            });
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            layer.widget->moveChildToIndex(ordered[index]->root, index);
        }
    }

    void resizeWidgetTree(ayt::ui::Widget* root)
    {
        if (root == nullptr) return;
        const math::FVector2 viewport = viewportSize();
        // A mounted document root has the same viewport contract as a root
        // loaded directly by UIManager: authored root position/size are a
        // design-time canvas, while runtime Screens always fill the host.
        root->setPosition(math::FVector2(0.0f, 0.0f));
        root->setSize(viewport);
        root->performLayout();
    }

    void syncViewport()
    {
        if (flowRoot == nullptr
            || (mountParent == nullptr && manager.root() == nullptr)) return;
        std::string error;
        if (!ensureRoot(error)) return;
        const math::FVector2 viewport = viewportSize();
        if (viewport.x == lastViewport.x && viewport.y == lastViewport.y) {
            return;
        }
        lastViewport = viewport;
        flowRoot->setSize(viewport);
        for (auto& pair : layers) pair.second->widget->setSize(viewport);
        for (auto& pair : mounts) resizeWidgetTree(pair.second.root);
        for (MountRecord& retiring : retiringMounts) {
            resizeWidgetTree(retiring.root);
        }
        manager.invalidateLayout();
    }

    void removeUnusedLayer(LayerRecord* layer)
    {
        if (layer == nullptr) return;
        for (const auto& pair : mounts) {
            if (pair.second.layer == layer) return;
        }
        for (const MountRecord& retiring : retiringMounts) {
            if (retiring.layer == layer) return;
        }
        manager.clearTransientStateForSubtree(layer->widget);
        ayt::ui::destroyWidgetTree(layer->widget);
        layers.erase(layer->id);
    }

    void shutdown() noexcept
    {
        while (!mounts.empty()) {
            unmount(mounts.begin()->first);
        }
        if (flowRoot != nullptr) {
            // Detach first so addChildExternal's host-owned flag is cleared;
            // destroyWidgetTree can then delete the host-owned root itself.
            manager.clearTransientStateForSubtree(flowRoot);
            flowRoot->detachFromParent();
            ayt::ui::destroyWidgetTree(flowRoot);
            flowRoot = nullptr;
        }
        retiringMounts.clear();
        layers.clear();
    }

    void unmount(std::uint64_t mountId) noexcept
    {
        const auto found = mounts.find(mountId);
        if (found == mounts.end()) return;
        LayerRecord* layer = found->second.layer;
        if (!found->second.request.exitAnimation.empty()) {
            ayt::ui::AnimationTimeline timeline =
                found->second.loader->createAnimationTimeline(
                    found->second.request.exitAnimation);
            timeline.play();
            if (timeline.isRunning()) {
                found->second.animation.emplace(std::move(timeline));
                retiringMounts.push_back(std::move(found->second));
                mounts.erase(found);
                manager.invalidateLayout();
                return;
            }
        }
        manager.clearTransientStateForSubtree(found->second.root);
        ayt::ui::destroyWidgetTree(found->second.root);
        mounts.erase(found);
        removeUnusedLayer(layer);
        manager.invalidateLayout();
    }

    ayt::ui::UIManager& manager;
    std::string assetRoot;
    ayt::ui::Widget* mountParent = nullptr;
    FlowRootWidget* flowRoot = nullptr;
    math::FVector2 lastViewport{-1.0f, -1.0f};
    std::unordered_map<std::string, std::unique_ptr<LayerRecord>> layers;
    std::unordered_map<std::uint64_t, MountRecord> mounts;
    std::vector<MountRecord> retiringMounts;
    std::uint64_t nextLayerSerial = 1;
};

UIManagerFlowScreenHost::UIManagerFlowScreenHost(
    ayt::ui::UIManager& manager,
    std::string assetRoot,
    ayt::ui::Widget* mountParent)
    : _impl(std::make_unique<Impl>(
          manager, std::move(assetRoot), mountParent))
{
}

UIManagerFlowScreenHost::~UIManagerFlowScreenHost() = default;

bool UIManagerFlowScreenHost::mountScreen(
    const UIFlowScreenMountRequest& request,
    std::string& error)
{
    error.clear();
    if (request.mountId == 0 || request.layerId.empty()) {
        error = "Screen mount requires a non-zero ID and a Layer.";
        return false;
    }
    if (_impl->mounts.find(request.mountId) != _impl->mounts.end()) {
        error = "Screen mount ID is already active.";
        return false;
    }
    const std::filesystem::path path = _impl->resolveAsset(
        request.layoutAsset, error);
    if (!error.empty() || path.empty()) return false;

    auto loader = std::make_unique<ayt::ui::UILayoutLoader>();
    ayt::ui::Widget* root = loader->loadFromFile(path.string());
    if (root == nullptr) {
        error = "Cannot load layout '" + path.string() + "'.";
        return false;
    }
    const auto& animations = loader->getAnimationLibrary();
    if ((!request.enterAnimation.empty()
            && animations.findClip(request.enterAnimation) == nullptr)
        || (!request.exitAnimation.empty()
            && animations.findClip(request.exitAnimation) == nullptr)) {
        error = "Screen '" + request.screenId
            + "' references a missing enter/exit animation clip.";
        ayt::ui::destroyWidgetTree(root);
        return false;
    }

    Impl::LayerRecord* layer = _impl->ensureLayer(request, error);
    if (layer == nullptr) {
        ayt::ui::destroyWidgetTree(root);
        return false;
    }
    root->setId(root->getId().empty()
        ? "__ay_ui_flow_screen_" + std::to_string(request.mountId)
        : root->getId());
    layer->widget->addChild(root);
    Impl::MountRecord value;
    value.request = request;
    value.loader = std::move(loader);
    value.root = root;
    value.layer = layer;
    _impl->mounts.emplace(request.mountId, std::move(value));
    if (!request.enterAnimation.empty()) {
        auto& mounted = _impl->mounts.at(request.mountId);
        mounted.animation.emplace(
            mounted.loader->createAnimationTimeline(request.enterAnimation));
        mounted.animation->play();
        if (!mounted.animation->isRunning()) mounted.animation.reset();
    }
    _impl->resizeWidgetTree(root);
    _impl->sortScreens(*layer);
    _impl->syncViewport();
    _impl->manager.invalidateLayout();
    return true;
}

void UIManagerFlowScreenHost::unmountScreen(std::uint64_t mountId) noexcept
{
    _impl->unmount(mountId);
}

void UIManagerFlowScreenHost::setScreenOrder(
    std::uint64_t mountId,
    int layerOrder,
    std::uint32_t orderInLayer) noexcept
{
    const auto found = _impl->mounts.find(mountId);
    if (found == _impl->mounts.end()) return;
    found->second.request.layerOrder = layerOrder;
    found->second.request.orderInLayer = orderInLayer;
    found->second.layer->order = layerOrder;
    _impl->sortLayers();
    _impl->sortScreens(*found->second.layer);
}

void UIManagerFlowScreenHost::update(float deltaSeconds)
{
    _impl->syncViewport();

    for (auto& pair : _impl->mounts) {
        if (!pair.second.animation.has_value()) continue;
        pair.second.animation->tick(deltaSeconds);
        if (!pair.second.animation->isRunning()) pair.second.animation.reset();
    }
    for (auto it = _impl->retiringMounts.begin();
         it != _impl->retiringMounts.end();) {
        if (it->animation.has_value()) it->animation->tick(deltaSeconds);
        if (it->animation.has_value() && it->animation->isRunning()) {
            ++it;
            continue;
        }
        Impl::LayerRecord* layer = it->layer;
        _impl->manager.clearTransientStateForSubtree(it->root);
        ayt::ui::destroyWidgetTree(it->root);
        it = _impl->retiringMounts.erase(it);
        _impl->removeUnusedLayer(layer);
        _impl->manager.invalidateLayout();
    }

    // Each Screen owns an independent loader and watcher. Reload into a new
    // tree first, then replace the old child, preserving the active Screen
    // if parsing/loading fails.
    for (auto& pair : _impl->mounts) {
        Impl::MountRecord& mount = pair.second;
        if (!mount.loader->isReloadNeeded()) continue;
        ayt::ui::Widget* replacement = mount.loader->tryReload();
        if (replacement == nullptr) continue;
        ayt::ui::Widget* old = mount.root;
        mount.animation.reset();
        mount.layer->widget->addChild(replacement);
        mount.root = replacement;
        _impl->resizeWidgetTree(replacement);
        _impl->sortScreens(*mount.layer);
        _impl->manager.clearTransientStateForSubtree(old);
        ayt::ui::destroyWidgetTree(old);
        _impl->manager.invalidateLayout();
    }
}

ayt::ui::Widget* UIManagerFlowScreenHost::screenRoot(
    std::uint64_t mountId) const noexcept
{
    const auto found = _impl->mounts.find(mountId);
    return found == _impl->mounts.end() ? nullptr : found->second.root;
}

ayt::ui::Widget* UIManagerFlowScreenHost::findWidget(
    std::uint64_t mountId,
    const std::string& widgetId) const noexcept
{
    const auto found = _impl->mounts.find(mountId);
    return found == _impl->mounts.end()
        ? nullptr
        : found->second.loader->findWidgetById(widgetId);
}

bool UIManagerFlowScreenHost::isScreenRetiring(
    std::uint64_t mountId) const noexcept
{
    return std::any_of(_impl->retiringMounts.begin(),
        _impl->retiringMounts.end(), [mountId](const auto& value) {
            return value.request.mountId == mountId;
        });
}

} // namespace ayt::app
