#pragma once

#include <AYApplication/GameFlowDocument.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

struct GameFlowMigrationStep
{
    std::uint32_t fromVersion = 0;
    std::uint32_t toVersion = 0;
    std::string description;
};

struct GameFlowMigrationReport
{
    std::uint32_t sourceVersion = 0;
    std::uint32_t targetVersion = kGameFlowSchemaVersion;
    bool changed = false;
    std::vector<GameFlowMigrationStep> steps;
};

// Migrates a raw JSON document without decoding it into the typed model. The
// destination is assigned only after every migration step succeeds. Calling
// this function again on its output is idempotent for the same pretty mode.
// Documents newer than this runtime are rejected instead of being downgraded.
[[nodiscard]] bool migrateGameFlowJson(
    std::string_view jsonText,
    std::string& migratedJson,
    GameFlowMigrationReport* report = nullptr,
    std::vector<GameFlowDiagnostic>* diagnostics = nullptr,
    bool pretty = true);

} // namespace ayt::app
