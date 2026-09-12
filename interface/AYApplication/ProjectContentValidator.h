#pragma once

#include <AYApplication/ProjectContentValidatorVersion.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ayt::app {

class GameFlowActionRegistry;

enum class ProjectContentValidationProfile {
    Headless,
    FullClient,
};

struct ProjectContentValidationIssue {
    std::string path;
    std::string message;
};

enum class ProjectContentDependencyKind : std::uint8_t {
    GameFlow,
    Asset,
    World,
    UIFlowEntry,
    UIContext,
    UISignal,
};

struct ProjectContentDependency {
    ProjectContentDependencyKind kind =
        ProjectContentDependencyKind::Asset;
    std::string source;
    std::string target;
};

using ProjectGameFlowRegistryConfigurator = std::function<bool(
    GameFlowActionRegistry& registry, std::string& error)>;

struct ProjectContentValidationOptions {
    // Called after built-in action types and an optional manifest are loaded.
    // This is the programmatic path for game-owned C++ action contracts.
    ProjectGameFlowRegistryConfigurator configureGameFlow;
    // Optional asset-root-relative override. When empty, the
    // descriptor's gameFlow.contract value is used.
    std::string gameFlowContractPath;
    // Editor/headless authoring can validate UI references structurally
    // without constructing widgets. FullClient enables this automatically.
    bool enableGameFlowUIActions = false;
    std::size_t maxGameFlowCallDepth = 16u;
    std::size_t maxGameFlows = 256u;
};

struct ProjectContentValidationResult {
    ProjectContentValidationProfile profile =
        ProjectContentValidationProfile::Headless;
    std::size_t scenes = 0;
    std::size_t uiLayouts = 0;
    std::size_t tilemaps = 0;
    std::size_t gameFlows = 0;
    std::vector<ProjectContentDependency> gameFlowDependencies;
    std::vector<ProjectContentValidationIssue> issues;

    explicit operator bool() const noexcept { return issues.empty(); }
    std::size_t checked() const noexcept {
        return scenes + uiLayouts + tilemaps + gameFlows;
    }
};

// Validates project content using the capabilities compiled into the current
// application profile. Headless builds parse UI documents without widgets;
// full-client builds additionally construct the real AYUI tree.
ProjectContentValidationResult validateProjectContent(
    const std::string& projectRoot,
    ProjectContentValidationProfile profile);
ProjectContentValidationResult validateProjectContent(
    const std::string& projectRoot,
    ProjectContentValidationProfile profile,
    ProjectContentValidationOptions options);

[[nodiscard]] const char* projectContentDependencyKindName(
    ProjectContentDependencyKind kind) noexcept;

} // namespace ayt::app
