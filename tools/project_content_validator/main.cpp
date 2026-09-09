#include <AYApplication/ProjectContentValidator.h>
#include <AYEntity/EntityModule.h>

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 4 || std::string(argv[2]) != "--profile") {
        std::cerr << "Usage: AYProjectContentValidator <project-root> "
                     "--profile <headless|full-client>\n";
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
    const auto result = ayt::app::validateProjectContent(
        std::filesystem::absolute(argv[1]).lexically_normal().string(), profile);
    std::cout << profileName << ": " << result.checked() << " file(s): "
              << result.scenes << " scene(s), " << result.uiLayouts
              << " UI layout(s), " << result.tilemaps << " tilemap file(s)\n";
    for (const auto& issue : result.issues) {
        std::cerr << issue.path << ": " << issue.message << '\n';
    }
    return result ? 0 : 1;
}
