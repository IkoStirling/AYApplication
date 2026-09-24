#include <AYApplication/ProjectDoctor.h>
#include <AYApplication/ProjectMigration.h>
#include <AYEntity/EntityModule.h>

#include <iostream>
#include <string>

namespace
{

void usage()
{
    std::cerr << "Usage: AYProjectDoctor <project-root> "
                 "[--profile headless|full-client] [--migrate] "
                 "[--require-artifacts]\n";
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    ayt::app::ProjectDoctorOptions options;
    bool migrate = false;
    for (int index = 2; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--migrate") {
            migrate = true;
        } else if (argument == "--require-artifacts") {
            options.requireRuntimeArtifacts = true;
        } else if (argument == "--profile" && index + 1 < argc) {
            const std::string profile = argv[++index];
            if (profile == "headless") {
                options.profile =
                    ayt::app::ProjectContentValidationProfile::Headless;
            } else if (profile == "full-client") {
                options.profile =
                    ayt::app::ProjectContentValidationProfile::FullClient;
            } else {
                std::cerr << "Unknown profile: " << profile << '\n';
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }

    if (migrate) {
        const auto migration = ayt::app::migrateProjectToCurrent(argv[1]);
        for (const auto& diagnostic : migration.diagnostics) {
            std::cerr << diagnostic << '\n';
        }
        if (!migration) return 1;
        for (const auto& file : migration.migratedFiles) {
            std::cout << "migrated["
                      << ayt::app::projectMigrationAssetKindName(file.kind)
                      << "]: " << file.path << "\n  backup: "
                      << file.backupPath << '\n';
        }
    }

    ayt::entity::registerEntityComponents();
    options.content.enableGameFlowUIActions = true;
    const auto result = ayt::app::diagnoseProject(argv[1], std::move(options));
    std::cout << "Project Doctor: " << result.content.checked()
              << " content file(s), " << result.buildProfiles
              << " Build Profile(s), " << result.runtimeArtifacts
              << " runtime artifact(s)\n";
    for (const auto& issue : result.issues) {
        std::ostream& output = issue.severity
            == ayt::app::ProjectDoctorSeverity::Error
            ? std::cerr : std::cout;
        output << ayt::app::projectDoctorSeverityName(issue.severity)
               << '[' << ayt::app::projectDoctorSectionName(issue.section)
               << "] " << issue.path << ": " << issue.message << '\n';
    }
    return result ? 0 : 1;
}
