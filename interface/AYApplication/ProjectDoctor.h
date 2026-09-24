#pragma once

#include <AYApplication/ProjectContentValidator.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ayt::app
{

enum class ProjectDoctorSection : std::uint8_t
{
    ProjectManifest,
    Content,
    BuildProfile,
    CMakePresets,
    RuntimeArtifact,
};

enum class ProjectDoctorSeverity : std::uint8_t
{
    Warning,
    Error,
};

struct ProjectDoctorIssue
{
    ProjectDoctorSection section = ProjectDoctorSection::ProjectManifest;
    ProjectDoctorSeverity severity = ProjectDoctorSeverity::Error;
    std::string path;
    std::string message;
};

struct ProjectDoctorOptions
{
    ProjectContentValidationProfile profile =
        ProjectContentValidationProfile::Headless;
    ProjectContentValidationOptions content;
    // CLI and CI validate complete projects by default. Editor utilities may
    // disable this when validating a content-only directory.
    bool requireProjectManifest = true;
    // Missing code artifacts are normally warnings so source projects can be
    // checked before their first build. Package/release CI can require them.
    bool requireRuntimeArtifacts = false;
};

struct ProjectDoctorResult
{
    ProjectContentValidationResult content;
    std::size_t buildProfiles = 0u;
    std::size_t runtimeArtifacts = 0u;
    std::vector<ProjectDoctorIssue> issues;

    [[nodiscard]] explicit operator bool() const noexcept;
};

/// Run the single project preflight used by CLI, Editor and CI. The report
/// covers the project contract, Scene/UI/Flow content and references, build
/// profiles, CMake preset links, and expected runtime artifacts.
[[nodiscard]] ProjectDoctorResult diagnoseProject(
    const std::string& projectRoot,
    ProjectDoctorOptions options = {});

[[nodiscard]] const char* projectDoctorSectionName(
    ProjectDoctorSection section) noexcept;
[[nodiscard]] const char* projectDoctorSeverityName(
    ProjectDoctorSeverity severity) noexcept;

} // namespace ayt::app
