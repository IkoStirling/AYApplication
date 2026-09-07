#include <AYApplication/GameProject.h>
#include <AYTest.h>

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

} // namespace

TEST_SUITE(GameProjectTests)

TEST_CASE(valid_project_uses_stable_world_ids)
{
    auto project = validProject();
    std::string error;
    CHECK(ayt::app::validateGameProject(project, error));
    CHECK(error.empty());
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
