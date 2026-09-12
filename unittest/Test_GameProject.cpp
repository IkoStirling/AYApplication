#include <AYApplication/GameProject.h>
#include <AYTest.h>

#include <filesystem>
#include <string>

namespace
{

ayt::app::GameProject validProject()
{
    ayt::app::GameProject project;
    project.id = "sample";
    project.displayName = "Sample Game";
    project.assetRoot = "assets";
    project.startupWorld = "menu";
    project.worlds = {
        {.id = "menu", .scenePath = "worlds/menu.ayscene"},
        {.id = "game", .scenePath = "worlds/game.ayscene"},
    };
    return project;
}

std::string resolvedPath(
    std::string_view root,
    std::string_view relative)
{
    return (std::filesystem::path(root) / std::filesystem::path(relative))
        .lexically_normal().string();
}

} // namespace

TEST_SUITE(GameProjectTests)

TEST_CASE(valid_project_uses_stable_world_ids)
{
    auto project = validProject();
    std::string error;
    CHECK(ayt::app::validateGameProject(project, error));
    CHECK(error.empty());
}

TEST_CASE(startup_selection_uses_scene_then_command_flow_then_project_flow)
{
    using ayt::app::GameProjectStartupSource;

    auto project = validProject();
    project.startupFlow = "flows/project.gameflow.json";
    ayt::app::AppCommandLine commandLine;
    commandLine.scenePath = "debug/direct.ayscene";
    commandLine.flowPath = "flows/debug.gameflow.json";

    ayt::app::GameProjectStartupSelection selection;
    std::string error;
    CHECK(ayt::app::resolveGameProjectStartup(
        project, commandLine, selection, error));
    CHECK(selection.source == GameProjectStartupSource::CommandLineScene);
    CHECK(selection.worldId == "__command_line__");
    CHECK(selection.scenePath == "debug/direct.ayscene");
    CHECK(selection.flowPath.empty());

    commandLine.scenePath.clear();
    CHECK(ayt::app::resolveGameProjectStartup(
        project, commandLine, selection, error));
    CHECK(selection.source == GameProjectStartupSource::CommandLineFlow);
    CHECK(selection.flowPath
          == resolvedPath(project.assetRoot, commandLine.flowPath));
    CHECK(selection.scenePath.empty());

    commandLine.flowPath.clear();
    CHECK(ayt::app::resolveGameProjectStartup(
        project, commandLine, selection, error));
    CHECK(selection.source == GameProjectStartupSource::ProjectFlow);
    CHECK(selection.flowPath
          == resolvedPath(project.assetRoot, project.startupFlow));
    CHECK(error.empty());
}

TEST_CASE(startup_world_remains_the_compatible_fallback)
{
    using ayt::app::GameProjectStartupSource;

    const auto project = validProject();
    ayt::app::GameProjectStartupSelection selection;
    std::string error;
    CHECK(ayt::app::resolveGameProjectStartup(
        project, {}, selection, error));
    CHECK(selection.source == GameProjectStartupSource::ProjectWorld);
    CHECK(selection.worldId == "menu");
    CHECK(selection.scenePath
          == resolvedPath(project.assetRoot, "worlds/menu.ayscene"));
    CHECK(selection.flowPath.empty());
    CHECK(error.empty());
}

TEST_CASE(command_line_asset_root_resolves_flow_and_world_assets)
{
    using ayt::app::GameProjectStartupSource;

    auto project = validProject();
    ayt::app::AppCommandLine commandLine;
    commandLine.assetRoot = "staging/../packaged_assets";
    commandLine.flowPath = "flows/../flow/start.gameflow.json";

    ayt::app::GameProjectStartupSelection selection;
    std::string error;
    CHECK(ayt::app::resolveGameProjectStartup(
        project, commandLine, selection, error));
    CHECK(selection.source == GameProjectStartupSource::CommandLineFlow);
    CHECK(selection.flowPath == resolvedPath(
        commandLine.assetRoot, commandLine.flowPath));

    commandLine.flowPath.clear();
    CHECK(ayt::app::resolveGameProjectStartup(
        project, commandLine, selection, error));
    CHECK(selection.source == GameProjectStartupSource::ProjectWorld);
    CHECK(selection.scenePath == resolvedPath(
        commandLine.assetRoot, "worlds/menu.ayscene"));
}

TEST_CASE(command_line_parser_accepts_flow_override)
{
    char executable[] = "SampleGame.exe";
    char option[] = "-flow";
    char value[] = "flows/debug.gameflow.json";
    char* arguments[] = {executable, option, value};

    const auto commandLine = ayt::app::AppCommandLine::parse(3, arguments);
    CHECK(commandLine.flowPath == value);
    CHECK(commandLine.unknownArgs.empty());
}

TEST_CASE(rejects_duplicate_world_ids)
{
    auto project = validProject();
    project.worlds.push_back(
        {.id = "menu", .scenePath = "worlds/other.ayscene"});
    std::string error;
    CHECK(!ayt::app::validateGameProject(project, error));
    CHECK(error.find("Duplicate World id") != std::string::npos);
}

TEST_CASE(rejects_unknown_startup_world)
{
    auto project = validProject();
    project.startupWorld = "missing";
    std::string error;
    CHECK(!ayt::app::validateGameProject(project, error));
    CHECK(error.find("startupWorld") != std::string::npos);
}

TEST_CASE(flow_startup_is_valid_without_a_startup_world)
{
    auto project = validProject();
    project.startupWorld.clear();
    project.startupFlow = "flow/application.gameflow.json";
    std::string error;
    CHECK(ayt::app::validateGameProject(project, error));
    CHECK(error.empty());
}

TEST_CASE(rejects_startup_flow_without_the_gameflow_json_suffix)
{
    auto project = validProject();
    project.startupFlow = "flow/application.json";
    std::string error;
    CHECK(!ayt::app::validateGameProject(project, error));
    CHECK(error.find("startupFlow") != std::string::npos);
    CHECK(error.find(".gameflow.json") != std::string::npos);
}

TEST_CASE(accepts_case_insensitive_startup_flow_suffix)
{
    auto project = validProject();
    project.startupFlow = "flow/Application.GAMEFLOW.JSON";
    std::string error;
    CHECK(ayt::app::validateGameProject(project, error));
    CHECK(error.empty());
}

TEST_CASE(rejects_startup_flow_paths_outside_the_asset_root)
{
    auto project = validProject();
    project.startupFlow = "../outside.gameflow.json";
    std::string error;
    CHECK(!ayt::app::validateGameProject(project, error));
    CHECK(error.find("inside assetRoot") != std::string::npos);

    project.startupFlow =
        (std::filesystem::temp_directory_path() / "outside.gameflow.json")
            .string();
    CHECK(!ayt::app::validateGameProject(project, error));
    CHECK(error.find("relative to assetRoot") != std::string::npos);
}

TEST_CASE(rejects_non_portable_startup_flow_separators)
{
    constexpr std::string_view invalidPaths[] = {
        "flow\\application.gameflow.json",
        "..\\outside.gameflow.json",
        "C:\\outside.gameflow.json",
        "C:/outside.gameflow.json",
        "C:outside.gameflow.json",
    };
    for (const std::string_view path : invalidPaths) {
        auto project = validProject();
        project.startupFlow = path;
        std::string error;
        CHECK_FALSE(ayt::app::validateGameProject(project, error));
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE(rejects_command_line_flow_paths_outside_the_asset_root)
{
    const auto project = validProject();
    ayt::app::AppCommandLine commandLine;
    commandLine.flowPath = "../../outside.gameflow.json";
    ayt::app::GameProjectStartupSelection selection;
    std::string error;
    CHECK(!ayt::app::resolveGameProjectStartup(
        project, commandLine, selection, error));
    CHECK(error.find("inside assetRoot") != std::string::npos);

    commandLine.flowPath =
        (std::filesystem::temp_directory_path() / "outside.gameflow.json")
            .string();
    CHECK(!ayt::app::resolveGameProjectStartup(
        project, commandLine, selection, error));
    CHECK(error.find("relative to assetRoot") != std::string::npos);
}

TEST_CASE(headless_project_can_omit_world_catalog)
{
    ayt::app::GameProject project;
    project.id = "server";
    project.displayName = "Server";
    project.serverMode = true;
    std::string error;
    CHECK(ayt::app::validateGameProject(project, error));
}

TEST_SUITE_END
