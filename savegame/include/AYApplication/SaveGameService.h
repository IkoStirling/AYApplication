#pragma once

#include <AYApplication/SaveGameVersion.h>

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

inline constexpr std::uint32_t kSaveGameEnvelopeVersion = 1u;

enum class SaveGameStatus : std::uint8_t
{
    Ok,
    RecoveredFromBackup,
    NotFound,
    InvalidArgument,
    Corrupt,
    MigrationFailed,
    IoError,
};

/// Result shared by save and remove operations.
struct SaveGameOperationResult
{
    SaveGameStatus status = SaveGameStatus::Ok;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == SaveGameStatus::Ok;
    }
};

/// One explicit, game-owned payload migration step.
///
/// The callback receives JSON text for the payload only. It must replace the
/// text with valid JSON for toVersion or return false with an actionable error.
struct SaveGameMigration
{
    std::uint32_t fromVersion = 0u;
    std::uint32_t toVersion = 0u;
    std::function<bool(std::string& payloadJson, std::string& error)> apply;
};

struct SaveGameLoadResult
{
    SaveGameStatus status = SaveGameStatus::NotFound;
    std::uint32_t schemaVersion = 0u;
    std::string payloadJson;
    std::string error;
    bool migrated = false;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return status == SaveGameStatus::Ok
            || status == SaveGameStatus::RecoveredFromBackup;
    }

    [[nodiscard]] bool recoveredFromBackup() const noexcept
    {
        return status == SaveGameStatus::RecoveredFromBackup;
    }
};

struct SaveGameServiceConfig
{
    /// Final project-specific user-data directory. Save files are stored in
    /// its SaveGames child directory and never in the asset tree.
    std::string userDataPath;
    std::string projectId;
};

/// Crash-safe, slot-oriented persistence for game-owned JSON data.
///
/// SaveGameService writes a versioned envelope through AYIO atomic replace,
/// retains the previous valid primary as a backup, and falls back to that
/// backup when the primary is missing or corrupt. It does not serialize a
/// World or ECS snapshot; the game chooses the durable data and schema.
/// Calls on one instance must be serialized by its owner.
class SaveGameService
{
public:
    explicit SaveGameService(SaveGameServiceConfig config);

    /// Atomically replace a slot while retaining the previous valid primary
    /// as `<slot>.aysave.json.bak`.
    [[nodiscard]] SaveGameOperationResult save(
        std::string_view slot,
        std::uint32_t schemaVersion,
        std::string_view payloadJson) const;

    /// Load and migrate a slot in memory. Migrated data is not written back
    /// automatically; the game decides when a successful load becomes a save.
    [[nodiscard]] SaveGameLoadResult load(
        std::string_view slot,
        std::uint32_t targetSchemaVersion,
        const std::vector<SaveGameMigration>& migrations = {}) const;

    /// Remove both the primary and recovery backup for a slot.
    [[nodiscard]] SaveGameOperationResult remove(
        std::string_view slot) const;

    /// Enumerate primary slots in deterministic name order.
    [[nodiscard]] std::vector<std::string> listSlots(
        std::string* error = nullptr) const;

    [[nodiscard]] std::string slotPath(std::string_view slot) const;
    [[nodiscard]] std::string backupPath(std::string_view slot) const;
    [[nodiscard]] const SaveGameServiceConfig& config() const noexcept
    {
        return _config;
    }

private:
    SaveGameServiceConfig _config;
};

/// Resolve a project-specific user-data directory. commandLineOverride wins
/// over configuredPath; when both are empty the operating system's per-user
/// application-data root is used.
[[nodiscard]] std::string resolveGameUserDataPath(
    std::string_view projectId,
    std::string_view configuredPath = {},
    std::string_view commandLineOverride = {});

} // namespace ayt::app
