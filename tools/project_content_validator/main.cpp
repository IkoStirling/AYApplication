#include <AYApplication/ProjectContentValidator.h>
#include <AYEntity/EntityModule.h>

#include <iostream>
#include <string>
#include <utility>

int main(int argc, char** argv)
{
    if ((argc != 4 && argc != 6) || std::string(argv[2]) != "--profile"
        || (argc == 6
            && std::string(argv[4]) != "--gameflow-contract")) {
        std::cerr << "Usage: AYProjectContentValidator <project-root> "
                     "--profile <headless|full-client> "
                     "[--gameflow-contract <asset-relative-path>]\n";
        return 2;
    }
    ayt::app::ProjectContentValidationProfile profile;
    const std::string profileName = argv[3];
    if (profileName == "headless") {
        profile = ayt::app::ProjectContentValidationProfile::Headless;
    } else if (profileName == "full-client") {
        profile = ayt::app::ProjectContentValidationProfile::FullClient;
    } else {
        std::cerr << "Unknown validation profile: " << profileName << '\n';
        return 2;
    }
    ayt::entity::registerEntityComponents();
    ayt::app::ProjectContentValidationOptions options;
    if (argc == 6) options.gameFlowContractPath = argv[5];
    const auto result = ayt::app::validateProjectContent(
        argv[1], profile, std::move(options));
    std::cout << profileName << ": " << result.checked() << " file(s): "
              << result.scenes << " scene(s), " << result.uiLayouts
              << " UI layout(s), " << result.tilemaps << " tilemap file(s), "
              << result.gameFlows << " GameFlow file(s)\n";
    for (const auto& dependency : result.gameFlowDependencies) {
        std::cout << "dependency["
                  << ayt::app::projectContentDependencyKindName(
                      dependency.kind)
                  << "]: " << dependency.source << " -> "
                  << dependency.target << '\n';
    }
    for (const auto& issue : result.issues) {
        std::cerr << issue.path << ": " << issue.message << '\n';
    }
    return result ? 0 : 1;
}
