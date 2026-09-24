#pragma once

#include <AYApplication/ProjectContentValidatorVersion.h>

#include <cstddef>
#include <string>
#include <vector>

namespace ayt::app
{

enum class ProjectMigrationAssetKind
{
    ProjectManifest,
    Scene,
    GameFlow,
    UIFlow,
    BuildProfile,
};

struct ProjectMigrationFile
{
    ProjectMigrationAssetKind kind = ProjectMigrationAssetKind::ProjectManifest;
    std::string path;
    std::string backupPath;
};

struct ProjectMigrationReport
{
    std::size_t inspectedFiles = 0u;
    std::vector<ProjectMigrationFile> migratedFiles;
    std::vector<std::string> diagnostics;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return diagnostics.empty();
    }
};

/// Preflight all supported checked-in project formats, then migrate every
/// outdated file through one entry point. No file is changed when preflight
/// finds a future version or an unsupported migration gap. Every changed file
/// receives a sibling backup before atomic replacement.
[[nodiscard]] ProjectMigrationReport migrateProjectToCurrent(
    const std::string& projectRoot);

[[nodiscard]] const char* projectMigrationAssetKindName(
    ProjectMigrationAssetKind kind) noexcept;

} // namespace ayt::app
