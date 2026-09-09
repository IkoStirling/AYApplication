#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ayt::app {

enum class ProjectContentValidationProfile {
    Headless,
    FullClient,
};

struct ProjectContentValidationIssue {
    std::string path;
    std::string message;
};

struct ProjectContentValidationResult {
    ProjectContentValidationProfile profile =
        ProjectContentValidationProfile::Headless;
    std::size_t scenes = 0;
    std::size_t uiLayouts = 0;
    std::size_t tilemaps = 0;
    std::vector<ProjectContentValidationIssue> issues;

    explicit operator bool() const noexcept { return issues.empty(); }
    std::size_t checked() const noexcept { return scenes + uiLayouts + tilemaps; }
};

// Validates project content using the capabilities compiled into the current
// application profile. Headless builds parse UI documents without widgets;
// full-client builds additionally construct the real AYUI tree.
ProjectContentValidationResult validateProjectContent(
    const std::string& projectRoot,
    ProjectContentValidationProfile profile);

} // namespace ayt::app
