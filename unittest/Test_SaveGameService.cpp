#include <AYApplicationSaveGame.h>
#include <AYTest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{

namespace fs = std::filesystem;
using namespace ayt::app;

class ScopedSaveRoot
{
public:
    explicit ScopedSaveRoot(std::string_view name)
        : path(fs::path(AY_APPLICATION_SAVEGAME_TEST_ROOT) / name)
    {
        std::error_code error;
        fs::remove_all(path, error);
        fs::create_directories(path, error);
    }

    ~ScopedSaveRoot()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }

    fs::path path;
};

SaveGameService makeService(const fs::path& root)
{
    return SaveGameService({root.string(), "savegame-test"});
}

} // namespace

TEST_SUITE(SaveGameServiceTests)

TEST_CASE(savegame_source_abi_is_explicit)
{
    CHECK(kSaveGameSourceAbiVersion == 1u);
}

TEST_CASE(slot_roundtrip_list_and_remove)
{
    ScopedSaveRoot root("roundtrip");
    auto service = makeService(root.path);

    CHECK(service.save("slot-1", 2u, R"({"day":7,"room":"kitchen"})"));
    const auto loaded = service.load("slot-1", 2u);
    CHECK(loaded);
    CHECK(loaded.status == SaveGameStatus::Ok);
    CHECK(loaded.schemaVersion == 2u);
    CHECK(loaded.payloadJson.find("kitchen") != std::string::npos);

    std::string listError;
    const auto slots = service.listSlots(&listError);
    CHECK(listError.empty());
    CHECK(slots.size() == 1u);
    CHECK(slots.front() == "slot-1");

    CHECK(service.remove("slot-1"));
    CHECK(service.load("slot-1", 2u).status == SaveGameStatus::NotFound);
}

TEST_CASE(previous_valid_save_recovers_a_corrupt_primary)
{
    ScopedSaveRoot root("recovery");
    auto service = makeService(root.path);
    CHECK(service.save("autosave", 1u, R"({"day":1})"));
    CHECK(service.save("autosave", 1u, R"({"day":2})"));

    std::ofstream corrupt(service.slotPath("autosave"),
                          std::ios::binary | std::ios::trunc);
    corrupt << "{broken";
    corrupt.close();

    const auto recovered = service.load("autosave", 1u);
    CHECK(recovered);
    CHECK(recovered.recoveredFromBackup());
    CHECK(recovered.payloadJson.find("1") != std::string::npos);

    // A corrupt primary never replaces the last known-good backup.
    CHECK(service.save("autosave", 1u, R"({"day":3})"));
    std::ofstream corruptAgain(service.slotPath("autosave"),
                               std::ios::binary | std::ios::trunc);
    corruptAgain << "{broken-again";
    corruptAgain.close();
    const auto recoveredAgain = service.load("autosave", 1u);
    CHECK(recoveredAgain);
    CHECK(recoveredAgain.payloadJson.find("1") != std::string::npos);
}

TEST_CASE(migrations_are_explicit_and_run_in_memory)
{
    ScopedSaveRoot root("migration");
    auto service = makeService(root.path);
    CHECK(service.save("manual", 1u, R"({"coins":3})"));

    SaveGameMigration migration;
    migration.fromVersion = 1u;
    migration.toVersion = 2u;
    migration.apply = [](std::string& payload, std::string&) {
        payload = R"({"coins":3,"inventory":[]})";
        return true;
    };
    const auto migrated = service.load("manual", 2u, {migration});
    CHECK(migrated);
    CHECK(migrated.migrated);
    CHECK(migrated.schemaVersion == 2u);
    CHECK(migrated.payloadJson.find("inventory") != std::string::npos);

    const auto unchangedOnDisk = service.load("manual", 1u);
    CHECK(unchangedOnDisk);
    CHECK(unchangedOnDisk.schemaVersion == 1u);

    const auto noPath = service.load("manual", 2u);
    CHECK(!noPath);
    CHECK(noPath.status == SaveGameStatus::MigrationFailed);
}

TEST_CASE(slot_names_cannot_escape_the_save_root)
{
    ScopedSaveRoot root("path-safety");
    auto service = makeService(root.path);
    const auto result = service.save("../outside", 1u, R"({})");
    CHECK(!result);
    CHECK(result.status == SaveGameStatus::InvalidArgument);
    CHECK(service.slotPath("../outside").empty());
}

TEST_CASE(project_path_precedence_and_serializer_payload_adapter)
{
    ScopedSaveRoot root("project-path");
    GameProject project;
    project.id = "sample-game";
    project.userDataPath = (root.path / "project").string();
    AppCommandLine commandLine;
    commandLine.userDataPath = (root.path / "override").string();
    CHECK(resolveGameUserDataPath(project, commandLine)
        == fs::absolute(root.path / "override").lexically_normal().string());

    std::int32_t written = 42;
    std::string payload;
    std::string error;
    CHECK(encodeSaveGamePayload(written, payload, error));
    CHECK(error.empty());
    std::int32_t read = 0;
    CHECK(decodeSaveGamePayload(payload, read, error));
    CHECK(read == written);
}

TEST_SUITE_END
