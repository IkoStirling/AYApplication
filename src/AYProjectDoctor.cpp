#include <AYApplication/ProjectDoctor.h>

#include <AYProject/ProjectContract.h>
#include <AYResource/ProjectBuild.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace ayt::app
{
namespace
{

namespace fs = std::filesystem;
using Json = nlohmann::json;

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

void addIssue(ProjectDoctorResult& result, ProjectDoctorSection section,
              ProjectDoctorSeverity severity, const fs::path& path,
              std::string message)
{
    result.issues.push_back(
        {section, severity, path.string(), std::move(message)});
}

bool loadJson(const fs::path& path, Json& root, std::string& error)
{
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "File was not found.";
            return false;
        }
        input >> root;
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

std::set<std::string, std::less<>> presetNames(
    const Json& presets, const char* key)
{
    std::set<std::string, std::less<>> names;
    const auto values = presets.find(key);
    if (values == presets.end() || !values->is_array()) return names;
    for (const auto& value : *values) {
        if (!value.is_object()) continue;
        const auto name = value.find("name");
        if (name != value.end() && name->is_string()
            && !name->get_ref<const std::string&>().empty()) {
            names.insert(name->get<std::string>());
        }
    }
    return names;
}

bool pathStaysWithin(const fs::path& root, const fs::path& candidate)
{
    const fs::path relative = candidate.lexically_relative(root);
    if (relative.empty() && candidate != root) return false;
    for (const auto& part : relative) if (part == "..") return false;
    return !relative.is_absolute();
}

} // namespace

ProjectDoctorResult::operator bool() const noexcept
{
    return std::none_of(issues.begin(), issues.end(),
        [](const ProjectDoctorIssue& issue) {
            return issue.severity == ProjectDoctorSeverity::Error;
        });
}

const char* projectDoctorSectionName(ProjectDoctorSection section) noexcept
{
    switch (section) {
    case ProjectDoctorSection::ProjectManifest: return "project-manifest";
    case ProjectDoctorSection::Content: return "content";
    case ProjectDoctorSection::BuildProfile: return "build-profile";
    case ProjectDoctorSection::CMakePresets: return "cmake-presets";
    case ProjectDoctorSection::RuntimeArtifact: return "runtime-artifact";
    }
    return "unknown";
}

const char* projectDoctorSeverityName(ProjectDoctorSeverity severity) noexcept
{
    return severity == ProjectDoctorSeverity::Error ? "error" : "warning";
}

ProjectDoctorResult diagnoseProject(
    const std::string& projectRoot, ProjectDoctorOptions options)
{
    ProjectDoctorResult result;
    result.content.profile = options.profile;
    try {
        std::error_code rootError;
        fs::path root = fs::weakly_canonical(
            fs::absolute(projectRoot.empty() ? fs::current_path()
                                             : fs::path(projectRoot)),
            rootError);
        if (rootError || !fs::is_directory(root)) {
            addIssue(result, ProjectDoctorSection::ProjectManifest,
                ProjectDoctorSeverity::Error, projectRoot, rootError
                    ? "Project root cannot be resolved: " + rootError.message()
                    : "Project root is not a directory.");
            return result;
        }

        std::string error;
        const fs::path manifestPath =
            root / ayt::project::kGameProjectManifestFile;
        std::error_code manifestError;
        const bool manifestExists = fs::exists(manifestPath, manifestError);
        if (manifestError) {
            addIssue(result, ProjectDoctorSection::ProjectManifest,
                ProjectDoctorSeverity::Error, manifestPath,
                "Project manifest cannot be inspected: "
                    + manifestError.message());
            return result;
        }
        if (manifestExists || options.requireProjectManifest) {
            const auto contract = ayt::project::inspectGameProjectContract(
                root.string(), &error);
            if (!contract) {
                if (contract.migrationRequired) {
                    error = "Project manifest schemaVersion "
                        + std::to_string(contract.schemaVersion)
                        + " requires migration before validation.";
                }
                addIssue(result, ProjectDoctorSection::ProjectManifest,
                    ProjectDoctorSeverity::Error, manifestPath,
                    error.empty() ? "Project manifest is invalid."
                                  : std::move(error));
                return result;
            }
        }

        result.content = validateProjectContent(
            root.string(), options.profile, std::move(options.content));
        for (const auto& issue : result.content.issues) {
            result.issues.push_back({ProjectDoctorSection::Content,
                ProjectDoctorSeverity::Error, issue.path, issue.message});
        }

        Json presets;
        const fs::path presetsPath = root / "CMakePresets.json";
        bool presetsReady = loadJson(presetsPath, presets, error);
        if (!presetsReady) {
            addIssue(result, ProjectDoctorSection::CMakePresets,
                ProjectDoctorSeverity::Warning, presetsPath, std::move(error));
        }
        const auto configurePresets = presetsReady
            ? presetNames(presets, "configurePresets")
            : std::set<std::string, std::less<>>{};
        const auto buildPresets = presetsReady
            ? presetNames(presets, "buildPresets")
            : std::set<std::string, std::less<>>{};

        const fs::path profilesRoot = root / "BuildProfiles";
        std::error_code scanError;
        const bool hasProfiles = fs::is_directory(profilesRoot, scanError);
        if (scanError == std::errc::no_such_file_or_directory) {
            scanError.clear();
        }
        if (!hasProfiles) {
            addIssue(result, ProjectDoctorSection::BuildProfile,
                scanError ? ProjectDoctorSeverity::Error
                          : ProjectDoctorSeverity::Warning,
                profilesRoot, scanError
                    ? "BuildProfiles cannot be inspected: " + scanError.message()
                    : "BuildProfiles directory is missing.");
            return result;
        }
        for (fs::recursive_directory_iterator it(profilesRoot,
                 fs::directory_options::skip_permission_denied, scanError), end;
             !scanError && it != end; it.increment(scanError)) {
            if (!it->is_regular_file()) continue;
            const std::string name = lower(it->path().filename().string());
            if (!endsWith(name, ".aybuild.json")) continue;
            ++result.buildProfiles;
            std::string profileError;
            const ayt::resource::ProjectBuildProfile profile =
                ayt::resource::ProjectBuildProfile::load(
                    it->path().string(), &profileError);
            if (!profile) {
                addIssue(result, ProjectDoctorSection::BuildProfile,
                    ProjectDoctorSeverity::Error, it->path(),
                    profileError.empty()
                        ? "Build Profile is invalid." : std::move(profileError));
                continue;
            }
            if (profile.code.enabled) {
                if (!presetsReady) {
                    addIssue(result, ProjectDoctorSection::CMakePresets,
                        ProjectDoctorSeverity::Error, presetsPath,
                        "Build Profile '" + profile.id
                            + "' enables CMake code build, but CMakePresets.json is unavailable.");
                } else if (!configurePresets.contains(
                               profile.code.configurePreset)) {
                    addIssue(result, ProjectDoctorSection::CMakePresets,
                        ProjectDoctorSeverity::Error, presetsPath,
                        "Build Profile '" + profile.id
                            + "' references missing configure preset '"
                            + profile.code.configurePreset + "'.");
                }
                if (presetsReady
                    && !buildPresets.contains(profile.code.buildPreset)) {
                    addIssue(result, ProjectDoctorSection::CMakePresets,
                        ProjectDoctorSeverity::Error, presetsPath,
                        "Build Profile '" + profile.id
                            + "' references missing build preset '"
                            + profile.code.buildPreset + "'.");
                }
                const fs::path artifact =
                    (root / fs::path(profile.code.artifact)).lexically_normal();
                if (!pathStaysWithin(root, artifact)) {
                    addIssue(result, ProjectDoctorSection::RuntimeArtifact,
                        ProjectDoctorSeverity::Error, artifact,
                        "Runtime artifact path escapes the project root.");
                } else {
                    std::error_code artifactError;
                    const bool artifactExists =
                        fs::exists(artifact, artifactError);
                    if (artifactError
                        == std::errc::no_such_file_or_directory) {
                        artifactError.clear();
                    }
                    if (artifactError) {
                        addIssue(result, ProjectDoctorSection::RuntimeArtifact,
                            ProjectDoctorSeverity::Error, artifact,
                            "Runtime artifact cannot be inspected: "
                                + artifactError.message());
                    } else if (artifactExists) {
                        if (fs::is_regular_file(artifact, artifactError)
                            && !artifactError) {
                            ++result.runtimeArtifacts;
                        } else {
                            addIssue(result,
                                ProjectDoctorSection::RuntimeArtifact,
                                ProjectDoctorSeverity::Error, artifact,
                                "Runtime artifact is not a regular file.");
                        }
                    } else {
                        addIssue(result, ProjectDoctorSection::RuntimeArtifact,
                            options.requireRuntimeArtifacts
                                ? ProjectDoctorSeverity::Error
                                : ProjectDoctorSeverity::Warning,
                            artifact,
                            "Runtime artifact has not been built yet for profile '"
                                + profile.id + "'.");
                    }
                }
            }
        }
        if (scanError) {
            addIssue(result, ProjectDoctorSection::BuildProfile,
                ProjectDoctorSeverity::Error, profilesRoot,
                "Build Profile scan failed: " + scanError.message());
        }
        if (result.buildProfiles == 0u) {
            addIssue(result, ProjectDoctorSection::BuildProfile,
                ProjectDoctorSeverity::Warning, profilesRoot,
                "No *.aybuild.json Build Profile was found.");
        }
        return result;
    } catch (const std::exception& exception) {
        addIssue(result, ProjectDoctorSection::ProjectManifest,
            ProjectDoctorSeverity::Error, projectRoot,
            std::string("Project Doctor failed: ") + exception.what());
        return result;
    }
}

} // namespace ayt::app
