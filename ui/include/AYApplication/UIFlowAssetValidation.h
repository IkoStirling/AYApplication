#pragma once

#include <AYUI/UIFlow.h>

#include <string>
#include <vector>

namespace ayt::app
{

// One deployable layout referenced by one or more Screens. Paths remain
// relative to the configured asset root so packaging never depends on an
// authoring machine's absolute directory layout.
struct UIFlowAssetDependency
{
    std::string asset;
    std::vector<std::string> screens;
};

struct UIFlowAssetValidationResult
{
    std::vector<UIFlowAssetDependency> dependencies;
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;

    [[nodiscard]] bool valid() const noexcept;
    explicit operator bool() const noexcept { return valid(); }
};

enum class UIFlowAssetValidationProfile
{
    StructureOnly,
    FullClient,
};

// Performs the production Screen-host checks without mounting into a
// UIManager: root containment, file existence, real UILayoutLoader decode,
// referenced animation clips, and animation track targets. The returned,
// sorted dependency list is the authoritative Flow input for a content
// packager.
UIFlowAssetValidationResult validateUIFlowAssets(
    const ayt::ui::UIFlowDocument& document,
    const std::string& assetRoot,
    UIFlowAssetValidationProfile profile =
        UIFlowAssetValidationProfile::FullClient);

} // namespace ayt::app
