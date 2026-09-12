#include <AYApplication/GameFlowMigration.h>

#include <nlohmann/json.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace ayt::app
{
namespace
{

using json = nlohmann::json;

void addError(std::vector<GameFlowDiagnostic>* diagnostics,
              std::string path,
              std::string message)
{
    if (diagnostics == nullptr) return;
    diagnostics->push_back({GameFlowDiagnosticSeverity::Error,
        std::move(path), std::move(message)});
}

std::uint32_t decodeSchemaVersion(const json& root)
{
    if (!root.contains("schemaVersion")) {
        throw std::runtime_error("Missing required 'schemaVersion'.");
    }
    const auto& value = root["schemaVersion"];
    std::uint64_t version = 0;
    if (value.is_number_unsigned()) {
        version = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signedVersion = value.get<std::int64_t>();
        if (signedVersion < 0) {
            throw std::runtime_error("'schemaVersion' must be non-negative.");
        }
        version = static_cast<std::uint64_t>(signedVersion);
    } else {
        throw std::runtime_error("'schemaVersion' must be an integer.");
    }
    if (version > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("'schemaVersion' exceeds uint32 range.");
    }
    return static_cast<std::uint32_t>(version);
}

void migrateVersion1To2(json& root, GameFlowMigrationReport& report)
{
    if (!root.contains("entryParameters")) {
        root["entryParameters"] = json::array();
    }
    if (!root.contains("result")) root["result"] = json::array();
    if (!root.contains("extensions")) root["extensions"] = json::object();
    root["schemaVersion"] = 2u;
    report.steps.push_back({1u, 2u,
        "Added subflow entry/result schemas and the namespaced extensions container."});
    report.changed = true;
}

} // namespace

bool migrateGameFlowJson(
    std::string_view jsonText,
    std::string& migratedJson,
    GameFlowMigrationReport* report,
    std::vector<GameFlowDiagnostic>* diagnostics,
    bool pretty)
{
    if (diagnostics != nullptr) diagnostics->clear();
    GameFlowMigrationReport builtReport;
    try {
        json root = json::parse(jsonText.begin(), jsonText.end());
        if (!root.is_object()) {
            throw std::runtime_error("Root must be an object.");
        }

        const std::uint32_t sourceVersion = decodeSchemaVersion(root);
        builtReport.sourceVersion = sourceVersion;
        if (sourceVersion > kGameFlowSchemaVersion) {
            addError(diagnostics, "$.schemaVersion",
                "GameFlow schema version " + std::to_string(sourceVersion)
                    + " is newer than the supported version "
                    + std::to_string(kGameFlowSchemaVersion) + ".");
            if (report != nullptr) *report = std::move(builtReport);
            return false;
        }
        if (sourceVersion == 0u) {
            addError(diagnostics, "$.schemaVersion",
                "GameFlow schema version 0 has no registered migration path.");
            if (report != nullptr) *report = std::move(builtReport);
            return false;
        }

        std::uint32_t version = sourceVersion;
        while (version < kGameFlowSchemaVersion) {
            switch (version) {
            case 1u:
                migrateVersion1To2(root, builtReport);
                version = 2u;
                break;
            default:
                addError(diagnostics, "$.schemaVersion",
                    "GameFlow schema version " + std::to_string(version)
                        + " has no registered migration path.");
                if (report != nullptr) *report = std::move(builtReport);
                return false;
            }
        }

        std::string encoded = root.dump(pretty ? 2 : -1);
        migratedJson = std::move(encoded);
        if (report != nullptr) *report = std::move(builtReport);
        return true;
    } catch (const std::exception& exception) {
        addError(diagnostics, "$",
            std::string("GameFlow JSON migration failed: ")
                + exception.what());
        if (report != nullptr) *report = std::move(builtReport);
        return false;
    }
}

} // namespace ayt::app
