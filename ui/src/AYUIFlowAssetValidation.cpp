#include <AYApplication/UIFlowAssetValidation.h>

#include <AYUI/LayoutLoader.h>
#include <AYUI/UIAnimation.h>
#include <AYUI/Widget.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <set>
#include <string_view>
#include <system_error>

namespace ayt::app
{
namespace
{

namespace fs = std::filesystem;

bool pathEscapesRoot(const fs::path& root, const fs::path& candidate)
{
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty()) return candidate != root;
    const auto first = relative.begin();
    return first != relative.end() && *first == "..";
}

void addError(UIFlowAssetValidationResult& result,
              std::size_t screenIndex,
              std::string_view field,
              std::string message)
{
    result.diagnostics.push_back({
        ayt::ui::UIFlowDiagnosticSeverity::Error,
        "$.screens[" + std::to_string(screenIndex) + "]."
            + std::string(field),
        std::move(message),
    });
}

struct LoadedLayout
{
    bool valid = false;
    std::string error;
    std::set<std::string, std::less<>> clips;
    std::map<std::string, std::size_t, std::less<>> unresolvedTracks;
    std::set<std::string, std::less<>> eventHandlers;
};

} // namespace

bool UIFlowAssetValidationResult::valid() const noexcept
{
    return std::none_of(diagnostics.begin(), diagnostics.end(),
        [](const ayt::ui::UIFlowDiagnostic& value) {
            return value.severity
                == ayt::ui::UIFlowDiagnosticSeverity::Error;
        });
}

UIFlowAssetValidationResult validateUIFlowAssets(
    const ayt::ui::UIFlowDocument& document,
    const std::string& assetRoot,
    UIFlowAssetValidationProfile profile)
{
    UIFlowAssetValidationResult result;
    if (!ayt::ui::validateUIFlow(document, &result.diagnostics)) return result;

    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::path(assetRoot), ec);
    if (assetRoot.empty() || ec || !fs::is_directory(root, ec)) {
        result.diagnostics.push_back({
            ayt::ui::UIFlowDiagnosticSeverity::Error,
            "$.assetRoot",
            "UI Flow asset root does not exist or cannot be resolved.",
        });
        return result;
    }

    std::map<std::string, UIFlowAssetDependency, std::less<>> dependencies;
    std::map<std::string, LoadedLayout, std::less<>> loadedLayouts;
    for (std::size_t index = 0; index < document.screens.size(); ++index) {
        const ayt::ui::UIFlowScreenDefinition& screen = document.screens[index];
        const fs::path supplied(screen.layoutAsset);
        if (supplied.is_absolute()) {
            addError(result, index, "layoutAsset",
                "Absolute layoutAsset paths are not deployable.");
            continue;
        }

        ec.clear();
        const fs::path resolved = fs::weakly_canonical(root / supplied, ec);
        if (ec || pathEscapesRoot(root, resolved)) {
            addError(result, index, "layoutAsset",
                "layoutAsset escapes the configured asset root.");
            continue;
        }

        const std::string portable = supplied.lexically_normal().generic_string();
        UIFlowAssetDependency& dependency = dependencies[portable];
        dependency.asset = portable;
        dependency.screens.push_back(screen.id);

        const std::string cacheKey = resolved.generic_string();
        auto [loadedIt, inserted] = loadedLayouts.try_emplace(cacheKey);
        LoadedLayout& loaded = loadedIt->second;
        if (inserted) {
            if (!fs::is_regular_file(resolved, ec) || ec) {
                loaded.error = "Referenced UI layout does not exist.";
            } else if (profile == UIFlowAssetValidationProfile::StructureOnly) {
                loaded.valid = true;
            } else {
                ayt::ui::UILayoutLoader loader;
                ayt::ui::Widget* rootWidget = loader.loadFromFile(resolved.string());
                loader.stopHotReload();
                if (rootWidget == nullptr) {
                    loaded.error = "UILayoutLoader rejected the referenced layout.";
                } else {
                    loaded.valid = true;
                    for (const ayt::ui::UIAnimationClip& clip :
                         loader.getAnimationLibrary().clips()) {
                        loaded.clips.insert(clip.name);
                        std::size_t unresolved = 0;
                        {
                            ayt::ui::AnimationTimeline timeline =
                                loader.createAnimationTimeline(
                                    clip.name, &unresolved);
                        }
                        loaded.unresolvedTracks.emplace(clip.name, unresolved);
                    }
                    const auto& eventHandlers =
                        loader.getDeclarativeEventHandlers();
                    loaded.eventHandlers.insert(
                        eventHandlers.begin(), eventHandlers.end());
                    ayt::ui::destroyWidgetTree(rootWidget);
                }
            }
        }

        if (!loaded.valid) {
            addError(result, index, "layoutAsset", loaded.error);
            continue;
        }
        if (profile == UIFlowAssetValidationProfile::StructureOnly) continue;
        const auto validateAnimation = [&](std::string_view field,
                                           const std::string& clip) {
            if (clip.empty()) return;
            if (!loaded.clips.contains(clip)) {
                addError(result, index, field,
                    "Referenced animation clip '" + clip
                        + "' is missing from the Screen layout.");
                return;
            }
            const auto unresolved = loaded.unresolvedTracks.find(clip);
            if (unresolved != loaded.unresolvedTracks.end()
                && unresolved->second != 0u) {
                addError(result, index, field,
                    "Animation clip '" + clip + "' has "
                        + std::to_string(unresolved->second)
                        + " unresolved widget track target(s).");
            }
        };
        validateAnimation("enterAnimation", screen.enterAnimation);
        validateAnimation("exitAnimation", screen.exitAnimation);
        for (std::size_t eventIndex = 0; eventIndex < screen.events.size();
             ++eventIndex) {
            if (!loaded.eventHandlers.contains(
                    screen.events[eventIndex].handler)) {
                addError(result, index,
                    "events[" + std::to_string(eventIndex) + "].handler",
                    "Screen event handler '"
                        + screen.events[eventIndex].handler
                        + "' is not authored by the referenced layout.");
            }
        }
    }

    result.dependencies.reserve(dependencies.size());
    for (auto& [_, dependency] : dependencies) {
        std::sort(dependency.screens.begin(), dependency.screens.end());
        dependency.screens.erase(
            std::unique(dependency.screens.begin(), dependency.screens.end()),
            dependency.screens.end());
        result.dependencies.push_back(std::move(dependency));
    }
    return result;
}

} // namespace ayt::app
