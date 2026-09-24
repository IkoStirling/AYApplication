#include <AYApplication/ProjectMigration.h>

#include <AYApplication/GameFlowMigration.h>
#include <AYEntity/SceneSerializer.h>
#include <AYProject/ProjectContract.h>
#include <AYResource/ProjectBuild.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

namespace ayt::app
{
namespace
{

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::uint32_t kUIFlowSchemaVersion = 1u;

struct PendingMigration
{
    ProjectMigrationAssetKind kind;
    fs::path path;
    std::string content;
};

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    return value;
}

bool endsWith(std::string_view value, std::string_view suffix)
{
    return value.size() >= suffix.size()
        && value.substr(value.size() - suffix.size()) == suffix;
}

bool readText(const fs::path& path, std::string& text, std::string& error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "Could not read project file: " + path.string();
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(input), {});
    return true;
}

bool decodeVersion(const Json& root, const char* field,
                   std::uint32_t& version, std::string& error)
{
    const auto found = root.find(field);
    if (found == root.end()) {
        error = std::string("Missing required '") + field + "'.";
        return false;
    }
    std::uint64_t decoded = 0u;
    if (found->is_number_unsigned()) {
        decoded = found->get<std::uint64_t>();
    } else if (found->is_number_integer()) {
        const auto signedValue = found->get<std::int64_t>();
        if (signedValue < 0) {
            error = std::string("'") + field + "' must be non-negative.";
            return false;
        }
        decoded = static_cast<std::uint64_t>(signedValue);
    } else {
        error = std::string("'") + field + "' must be an integer.";
        return false;
    }
    if (decoded > std::numeric_limits<std::uint32_t>::max()) {
        error = std::string("'") + field + "' exceeds uint32 range.";
        return false;
    }
    version = static_cast<std::uint32_t>(decoded);
    return true;
}

bool parseJson(std::string_view text, Json& root, std::string& error)
{
    try {
        root = Json::parse(text.begin(), text.end());
        if (!root.is_object()) {
            error = "JSON root must be an object.";
            return false;
        }
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Invalid JSON: ") + exception.what();
        return false;
    }
}

void diagnose(ProjectMigrationReport& report, const fs::path& path,
              std::string message)
{
    report.diagnostics.push_back(path.string() + ": " + std::move(message));
}

bool inspectExactVersion(std::string_view text, const fs::path& path,
                         const char* field, std::uint32_t current,
                         std::string_view type,
                         ProjectMigrationReport& report)
{
    Json root;
    std::string error;
    std::uint32_t version = 0u;
    if (!parseJson(text, root, error)
        || !decodeVersion(root, field, version, error)) {
        diagnose(report, path, std::string(type) + " " + error);
        return false;
    }
    if (version > current) {
        diagnose(report, path, std::string(type) + " version "
            + std::to_string(version) + " is newer than supported version "
            + std::to_string(current) + "; the project was not changed.");
        return false;
    }
    if (version < current) {
        diagnose(report, path, std::string(type) + " version "
            + std::to_string(version) + " has no registered migration path.");
        return false;
    }
    return true;
}

bool preflightFile(const fs::path& path, ProjectMigrationReport& report,
                   std::vector<PendingMigration>& pending)
{
    std::error_code linkError;
    if (fs::is_symlink(fs::symlink_status(path, linkError))) {
        diagnose(report, path, "Migration refuses symbolic-link project files.");
        return false;
    }
    std::string text;
    std::string error;
    if (!readText(path, text, error)) {
        diagnose(report, path, std::move(error));
        return false;
    }
    ++report.inspectedFiles;
    const std::string name = lower(path.filename().string());
    if (endsWith(name, ".gameflow.json")) {
        std::string migrated;
        GameFlowMigrationReport migration;
        std::vector<GameFlowDiagnostic> diagnostics;
        if (!migrateGameFlowJson(text, migrated, &migration, &diagnostics)) {
            if (diagnostics.empty()) {
                diagnose(report, path, "GameFlow migration preflight failed.");
            } else {
                for (const auto& item : diagnostics) {
                    diagnose(report, path,
                        (item.path.empty() ? std::string{} : item.path + ": ")
                            + item.message);
                }
            }
            return false;
        }
        if (migration.changed) pending.push_back({
            ProjectMigrationAssetKind::GameFlow, path, migrated + "\n"});
        return true;
    }
    if (endsWith(name, ".uiflow.json")) {
        return inspectExactVersion(text, path, "schemaVersion",
            kUIFlowSchemaVersion, "UIFlow schema", report);
    }
    if (endsWith(name, ".ayscene")) {
        Json root;
        std::uint32_t version = 0u;
        if (!parseJson(text, root, error)
            || !decodeVersion(root, ayt::entity::kSceneSchemaVersionField,
                              version, error)) {
            diagnose(report, path, "Scene schema " + error);
            return false;
        }
        if (version > ayt::entity::kSceneSchemaVersion) {
            diagnose(report, path, "Scene schema version "
                + std::to_string(version) + " is newer than supported version "
                + std::to_string(ayt::entity::kSceneSchemaVersion)
                + "; the project was not changed.");
            return false;
        }
        if (version == 0u || !ayt::entity::migrateSceneSchemaToCurrent(version)) {
            diagnose(report, path, "Scene schema version "
                + std::to_string(version) + " has no usable migration path.");
            return false;
        }
        if (version < ayt::entity::kSceneSchemaVersion) {
            root[ayt::entity::kSceneSchemaVersionField] =
                ayt::entity::kSceneSchemaVersion;
            pending.push_back({ProjectMigrationAssetKind::Scene, path,
                               root.dump(2) + "\n"});
        }
        return true;
    }
    return true;
}

} // namespace

const char* projectMigrationAssetKindName(
    ProjectMigrationAssetKind kind) noexcept
{
    switch (kind) {
    case ProjectMigrationAssetKind::ProjectManifest: return "project-manifest";
    case ProjectMigrationAssetKind::Scene: return "scene";
    case ProjectMigrationAssetKind::GameFlow: return "gameflow";
    case ProjectMigrationAssetKind::UIFlow: return "uiflow";
    case ProjectMigrationAssetKind::BuildProfile: return "build-profile";
    }
    return "unknown";
}

ProjectMigrationReport migrateProjectToCurrent(const std::string& projectRoot)
{
    ProjectMigrationReport report;
    try {
        std::error_code rootError;
        fs::path root = fs::weakly_canonical(
            fs::absolute(projectRoot.empty() ? fs::current_path()
                                             : fs::path(projectRoot)),
            rootError);
        if (rootError || !fs::is_directory(root)) {
            diagnose(report, projectRoot, rootError
                ? "Project root cannot be resolved: " + rootError.message()
                : "Project root is not a directory.");
            return report;
        }

        const fs::path manifest = root / ayt::project::kGameProjectManifestFile;
        std::string manifestText;
        std::string error;
        if (!readText(manifest, manifestText, error)) {
            diagnose(report, manifest, std::move(error));
            return report;
        }
        ++report.inspectedFiles;
        std::string migratedManifest;
        ayt::project::GameProjectContractMigrationReport manifestReport;
        if (!ayt::project::migrateGameProjectContractJson(
                manifestText, migratedManifest, &manifestReport, &error)) {
            diagnose(report, manifest, std::move(error));
            return report;
        }

        Json descriptor;
        if (!parseJson(migratedManifest, descriptor, error)) {
            diagnose(report, manifest, std::move(error));
            return report;
        }
        std::string assetRoot = "Assets";
        if (const auto paths = descriptor.find("paths"); paths != descriptor.end()) {
            if (!paths->is_object() || !paths->contains("assets")
                || !(*paths)["assets"].is_string()) {
                diagnose(report, manifest,
                    "Project paths.assets must be a string.");
                return report;
            }
            assetRoot = (*paths)["assets"].get<std::string>();
        }

        std::vector<PendingMigration> pending;
        if (manifestReport.changed) pending.push_back({
            ProjectMigrationAssetKind::ProjectManifest,
            manifest, std::move(migratedManifest)});

        const fs::path assets = (root / fs::path(assetRoot)).lexically_normal();
        std::error_code scanError;
        if (fs::is_directory(assets, scanError)) {
            for (fs::recursive_directory_iterator it(assets,
                     fs::directory_options::skip_permission_denied, scanError), end;
                 !scanError && it != end; it.increment(scanError)) {
                if (!it->is_regular_file()) continue;
                const std::string name = lower(it->path().filename().string());
                if (endsWith(name, ".gameflow.json")
                    || endsWith(name, ".uiflow.json")
                    || endsWith(name, ".ayscene")) {
                    preflightFile(it->path(), report, pending);
                }
            }
        }
        if (scanError == std::errc::no_such_file_or_directory) {
            scanError.clear();
        }
        if (scanError) diagnose(report, assets,
            "Asset migration scan failed: " + scanError.message());

        const fs::path profiles = root / "BuildProfiles";
        scanError.clear();
        const bool hasProfiles = fs::is_directory(profiles, scanError);
        if (scanError == std::errc::no_such_file_or_directory) {
            scanError.clear();
        }
        if (hasProfiles) {
            for (fs::recursive_directory_iterator it(profiles,
                     fs::directory_options::skip_permission_denied, scanError), end;
                 !scanError && it != end; it.increment(scanError)) {
                if (!it->is_regular_file()) continue;
                const std::string name = lower(it->path().filename().string());
                if (!endsWith(name, ".aybuild.json")) continue;
                std::string text;
                if (!readText(it->path(), text, error)) {
                    diagnose(report, it->path(), std::move(error));
                    continue;
                }
                ++report.inspectedFiles;
                inspectExactVersion(text, it->path(), "schemaVersion",
                    ayt::resource::kProjectBuildProfileSchemaVersion,
                    "Build Profile schema", report);
            }
        }

        // Preflight is deliberately complete before the first mutation.
        if (!report.diagnostics.empty()) return report;
        for (const PendingMigration& item : pending) {
            std::string backup;
            if (!ayt::project::publishProjectFileMigration(
                    item.path.string(), item.content, backup, &error)) {
                diagnose(report, item.path, std::move(error));
                return report;
            }
            report.migratedFiles.push_back(
                {item.kind, item.path.string(), std::move(backup)});
        }
        return report;
    } catch (const std::exception& exception) {
        diagnose(report, projectRoot,
            std::string("Project migration failed: ") + exception.what());
        return report;
    }
}

} // namespace ayt::app
