#include <AYApplication/ProjectContentValidator.h>
#include <AYProject/ProjectScaffold.h>
#include <AYTest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ayt::app;
using namespace ayt::project;

namespace
{

namespace fs = std::filesystem;

class GeneratedProjectSandbox
{
public:
    GeneratedProjectSandbox()
    {
        const auto nonce = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        root = fs::path(AY_APPLICATION_CONTENT_VALIDATOR_OUTPUT_ROOT)
            / ("generated-golden-path-" + std::to_string(nonce));
        engine = root / "AliyatEngine";
        project = root / "Game";
        fs::create_directories(engine / "cmake");
        std::ofstream helper(engine / "cmake/AYGameApplication.cmake");
        helper << "# validation fixture\n";
    }

    ~GeneratedProjectSandbox()
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root;
    fs::path engine;
    fs::path project;
};

void printIssues(const ProjectContentValidationResult& result)
{
    for (const auto& issue : result.issues) {
        std::fprintf(stderr, "[GeneratedProject] %s: %s\n",
                     issue.path.c_str(), issue.message.c_str());
    }
}

} // namespace

TEST_SUITE(ProjectScaffoldGoldenPathTests)

TEST_CASE(generated_project_passes_independent_content_validation)
{
    GeneratedProjectSandbox sandbox;
    const GameProjectScaffoldPlan plan = planGameProjectScaffold({
        .destination = sandbox.project.string(),
        .engineSource = sandbox.engine.string(),
        .displayName = "Generated Golden Path",
        .projectId = "generated-golden-path",
        .profile = GameProjectTemplateProfile::Client3D,
    });
    CHECK(static_cast<bool>(plan));

    std::string error;
    CHECK(writeGameProjectScaffold(plan, &error));
    CHECK(error.empty());

    const auto headless = validateProjectContent(
        sandbox.project.string(),
        ProjectContentValidationProfile::Headless);
    if (!headless) printIssues(headless);
    CHECK(static_cast<bool>(headless));
    CHECK(headless.scenes == 2u);
    CHECK(headless.uiLayouts == 2u);
    CHECK(headless.gameFlows == 1u);

#if AY_APPLICATION_CONTENT_VALIDATOR_HAS_UI
    const auto fullClient = validateProjectContent(
        sandbox.project.string(),
        ProjectContentValidationProfile::FullClient);
    if (!fullClient) printIssues(fullClient);
    CHECK(static_cast<bool>(fullClient));
    CHECK(fullClient.scenes == headless.scenes);
    CHECK(fullClient.uiLayouts == headless.uiLayouts);
    CHECK(fullClient.gameFlows == headless.gameFlows);
#endif
}

TEST_SUITE_END
