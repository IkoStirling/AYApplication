#include <AYApplication/ProjectDoctor.h>
#include <AYApplication/ProjectMigration.h>
#include <AYProject/ProjectScaffold.h>
#include <AYTest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace ayt::app;
using namespace ayt::project;

namespace
{
namespace fs = std::filesystem;

struct DoctorSandbox
{
    DoctorSandbox()
    {
        root = fs::path(AY_APPLICATION_CONTENT_VALIDATOR_OUTPUT_ROOT)
            / ("project-doctor-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        engine = root / "AliyatEngine";
        project = root / "Game";
        fs::create_directories(engine / "cmake");
        std::ofstream(engine / "cmake/AYGameApplication.cmake")
            << "# doctor fixture\n";
    }
    ~DoctorSandbox()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
    fs::path root;
    fs::path engine;
    fs::path project;
};

std::string readText(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void writeText(const fs::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
}

bool hasIssue(const ProjectDoctorResult& result,
              ProjectDoctorSection section, std::string_view message)
{
    for (const auto& issue : result.issues) {
        if (issue.section == section
            && issue.message.find(message) != std::string::npos) return true;
    }
    return false;
}

GameProjectScaffoldPlan makeProject(DoctorSandbox& sandbox)
{
    const auto plan = planGameProjectScaffold({
        .destination = sandbox.project.string(),
        .engineSource = sandbox.engine.string(),
        .displayName = "Doctor Game",
        .projectId = "doctor-game",
        .profile = GameProjectTemplateProfile::Client3D,
    });
    std::string error;
    CHECK(static_cast<bool>(plan));
    const bool written = writeGameProjectScaffold(plan, &error);
    if (!written) {
        std::fprintf(stderr, "[ProjectDoctorTests] %s\n", error.c_str());
    }
    CHECK(written);
    CHECK(error.empty());
    return plan;
}
}

TEST_SUITE(ProjectDoctorTests)

TEST_CASE(generated_project_passes_shared_doctor_before_first_build)
{
    DoctorSandbox sandbox;
    makeProject(sandbox);
    ProjectDoctorOptions options;
    options.content.enableGameFlowUIActions = true;
    const auto result = diagnoseProject(sandbox.project.string(), options);
    CHECK(static_cast<bool>(result));
    CHECK(result.buildProfiles == 1u);
    CHECK(result.runtimeArtifacts == 0u);
    CHECK(hasIssue(result, ProjectDoctorSection::RuntimeArtifact,
                   "has not been built yet"));
}

TEST_CASE(doctor_rejects_missing_presets_referenced_by_build_profile)
{
    DoctorSandbox sandbox;
    makeProject(sandbox);
    std::string presets = readText(sandbox.project / "CMakePresets.json");
    const std::string needle = "\"windows-client-release\"";
    std::size_t position = 0u;
    while ((position = presets.find(needle, position)) != std::string::npos) {
        presets.replace(position, needle.size(), "\"renamed-release\"");
        position += 17u;
    }
    writeText(sandbox.project / "CMakePresets.json", presets);
    const auto result = diagnoseProject(sandbox.project.string());
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, ProjectDoctorSection::CMakePresets,
                   "missing configure preset"));
}

TEST_CASE(unified_migration_preflights_then_backs_up_manifest_flow_and_scene)
{
    DoctorSandbox sandbox;
    makeProject(sandbox);
    fs::path manifest = sandbox.project / "project.ayproject.json";
    std::string descriptor = readText(manifest);
    descriptor.replace(descriptor.find("\"schemaVersion\": 2"),
                       std::string("\"schemaVersion\": 2").size(),
                       "\"schemaVersion\": 1");
    const auto compatibility = descriptor.find("  \"templateVersion\"");
    const auto id = descriptor.find("  \"id\"");
    CHECK(compatibility != std::string::npos);
    CHECK(id != std::string::npos);
    descriptor.erase(compatibility, id - compatibility);
    writeText(manifest, descriptor);

    const fs::path flow = sandbox.project
        / "Assets/flow/application.gameflow.json";
    std::string flowText = readText(flow);
    flowText.replace(flowText.find("\"schemaVersion\": 2"),
        std::string("\"schemaVersion\": 2").size(),
        "\"schemaVersion\": 1");
    writeText(flow, flowText);

    const fs::path scene = sandbox.project / "Assets/worlds/main_menu.ayscene";
    std::string sceneText = readText(scene);
    sceneText.replace(sceneText.find("\"__schemaVersion\": 3"),
        std::string("\"__schemaVersion\": 3").size(),
        "\"__schemaVersion\": 2");
    writeText(scene, sceneText);

    const auto report = migrateProjectToCurrent(sandbox.project.string());
    CHECK(static_cast<bool>(report));
    CHECK(report.migratedFiles.size() == 3u);
    for (const auto& file : report.migratedFiles) {
        CHECK(fs::is_regular_file(file.backupPath));
    }
    CHECK(readText(manifest).find("\"schemaVersion\": 2")
        != std::string::npos);
    CHECK(readText(flow).find("\"schemaVersion\": 2")
        != std::string::npos);
    CHECK(readText(scene).find("\"__schemaVersion\": 3")
        != std::string::npos);
}

TEST_CASE(unified_migration_does_not_mutate_when_any_future_format_is_found)
{
    DoctorSandbox sandbox;
    makeProject(sandbox);
    const fs::path manifest = sandbox.project / "project.ayproject.json";
    std::string descriptor = readText(manifest);
    descriptor.replace(descriptor.find("\"schemaVersion\": 2"),
        std::string("\"schemaVersion\": 2").size(),
        "\"schemaVersion\": 1");
    const auto compatibility = descriptor.find("  \"templateVersion\"");
    const auto id = descriptor.find("  \"id\"");
    CHECK(compatibility != std::string::npos);
    CHECK(id != std::string::npos);
    descriptor.erase(compatibility, id - compatibility);
    writeText(manifest, descriptor);

    const fs::path uiFlow = sandbox.project
        / "Assets/ui/application.uiflow.json";
    std::string uiFlowText = readText(uiFlow);
    uiFlowText.replace(uiFlowText.find("\"schemaVersion\": 1"),
        std::string("\"schemaVersion\": 1").size(),
        "\"schemaVersion\": 2");
    writeText(uiFlow, uiFlowText);

    const auto report = migrateProjectToCurrent(sandbox.project.string());
    CHECK_FALSE(static_cast<bool>(report));
    CHECK(report.migratedFiles.empty());
    CHECK(readText(manifest).find("\"schemaVersion\": 1")
        != std::string::npos);
    CHECK_FALSE(fs::exists(
        fs::path(manifest.string() + ".before-migration.bak")));
    CHECK_FALSE(report.diagnostics.empty());
    CHECK(report.diagnostics.front().find("newer") != std::string::npos);
}

TEST_SUITE_END
