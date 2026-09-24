#include <AYApplication/SaveGameService.h>

#include <AYIO/File.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>

namespace ayt::app
{
namespace
{

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::string_view kSaveGameFormat = "AliyatSaveGame";
constexpr std::string_view kSlotSuffix = ".aysave.json";

struct SaveGameEnvelope
{
    std::uint32_t schemaVersion = 0u;
    Json payload;
};

bool isPortableSlot(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 64u || value == "." || value == "..") {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char byte) {
        return std::isalnum(byte) != 0 || byte == '-' || byte == '_'
            || byte == '.';
    });
}

std::string safeProjectFolder(std::string_view projectId)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(projectId.size() * 3u);
    for (const unsigned char byte : projectId) {
        if (std::isalnum(byte) != 0 || byte == '-' || byte == '_'
            || byte == '.') {
            result.push_back(static_cast<char>(byte));
        } else {
            result.push_back('%');
            result.push_back(hex[(byte >> 4u) & 0x0fu]);
            result.push_back(hex[byte & 0x0fu]);
        }
    }
    if (result.empty()) return "game";
    if (result == ".") return "%2E";
    if (result == "..") return "%2E%2E";
    return result;
}

std::string absoluteNormalized(const fs::path& path)
{
    std::error_code error;
    fs::path absolute = fs::absolute(path, error);
    return (error ? path : absolute).lexically_normal().string();
}

std::optional<std::string> environmentValue(const char* name)
{
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t size = 0u;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result(value);
    std::free(value);
    return result.empty() ? std::nullopt
                          : std::optional<std::string>(std::move(result));
#else
    const char* value = std::getenv(name);
    return value == nullptr || value[0] == '\0'
        ? std::nullopt : std::optional<std::string>(value);
#endif
}

bool decodeVersion(const Json& root, const char* name,
                   std::uint32_t& value, std::string& error)
{
    const auto found = root.find(name);
    if (found == root.end() ||
        (!found->is_number_unsigned() && !found->is_number_integer())) {
        error = std::string("SaveGame '") + name + "' must be an integer.";
        return false;
    }
    if (found->is_number_integer() && found->get<std::int64_t>() < 0) {
        error = std::string("SaveGame '") + name + "' cannot be negative.";
        return false;
    }
    const std::uint64_t decoded = found->is_number_unsigned()
        ? found->get<std::uint64_t>()
        : static_cast<std::uint64_t>(found->get<std::int64_t>());
    if (decoded > std::numeric_limits<std::uint32_t>::max()) {
        error = std::string("SaveGame '") + name + "' exceeds uint32 range.";
        return false;
    }
    value = static_cast<std::uint32_t>(decoded);
    return true;
}

bool parseEnvelope(std::string_view text, std::string_view projectId,
                   std::string_view slot, SaveGameEnvelope& envelope,
                   std::string& error)
{
    Json root;
    try {
        root = Json::parse(text.begin(), text.end());
    } catch (const std::exception& exception) {
        error = std::string("Invalid SaveGame JSON: ") + exception.what();
        return false;
    }
    if (!root.is_object()) {
        error = "SaveGame root must be an object.";
        return false;
    }
    if (root.value("format", std::string{}) != kSaveGameFormat) {
        error = "SaveGame format marker is missing or unsupported.";
        return false;
    }
    std::uint32_t envelopeVersion = 0u;
    if (!decodeVersion(root, "envelopeVersion", envelopeVersion, error)
        || envelopeVersion != kSaveGameEnvelopeVersion) {
        if (error.empty()) {
            error = "SaveGame envelope version is unsupported.";
        }
        return false;
    }
    if (root.value("projectId", std::string{}) != projectId) {
        error = "SaveGame belongs to a different project.";
        return false;
    }
    if (root.value("slot", std::string{}) != slot) {
        error = "SaveGame slot identity does not match its filename.";
        return false;
    }
    if (!decodeVersion(root, "schemaVersion", envelope.schemaVersion, error)
        || envelope.schemaVersion == 0u) {
        if (error.empty()) error = "SaveGame schemaVersion must be at least 1.";
        return false;
    }
    const auto payload = root.find("payload");
    if (payload == root.end()) {
        error = "SaveGame payload is missing.";
        return false;
    }
    envelope.payload = *payload;
    return true;
}

bool parsePayload(std::string_view payloadJson, Json& payload,
                  std::string& error)
{
    try {
        payload = Json::parse(payloadJson.begin(), payloadJson.end());
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Invalid SaveGame payload JSON: ")
            + exception.what();
        return false;
    }
}

std::string serializeEnvelope(std::string_view projectId,
                              std::string_view slot,
                              std::uint32_t schemaVersion,
                              const Json& payload)
{
    return Json{
        {"format", kSaveGameFormat},
        {"envelopeVersion", kSaveGameEnvelopeVersion},
        {"projectId", projectId},
        {"slot", slot},
        {"schemaVersion", schemaVersion},
        {"payload", payload},
    }.dump(2) + "\n";
}

bool atomicWriteText(const std::string& path, const std::string& text)
{
    return ayt::io::File::atomicWrite(path, text.data(), text.size());
}

SaveGameLoadResult loadFile(const std::string& path,
                            std::string_view projectId,
                            std::string_view slot)
{
    SaveGameLoadResult result;
    if (!ayt::io::File::exists(path)) {
        result.status = SaveGameStatus::NotFound;
        return result;
    }
    const std::string text = ayt::io::File::readAllText(path);
    SaveGameEnvelope envelope;
    if (!parseEnvelope(text, projectId, slot, envelope, result.error)) {
        result.status = SaveGameStatus::Corrupt;
        return result;
    }
    result.status = SaveGameStatus::Ok;
    result.schemaVersion = envelope.schemaVersion;
    result.payloadJson = envelope.payload.dump();
    return result;
}

bool applyMigrations(SaveGameLoadResult& result,
                     std::uint32_t targetSchemaVersion,
                     const std::vector<SaveGameMigration>& migrations)
{
    if (result.schemaVersion > targetSchemaVersion) {
        result.status = SaveGameStatus::MigrationFailed;
        result.error = "SaveGame schema is newer than this game build.";
        return false;
    }
    while (result.schemaVersion < targetSchemaVersion) {
        const SaveGameMigration* selected = nullptr;
        for (const auto& migration : migrations) {
            if (migration.fromVersion != result.schemaVersion) continue;
            if (selected != nullptr) {
                result.status = SaveGameStatus::MigrationFailed;
                result.error = "SaveGame migration path is ambiguous at version "
                    + std::to_string(result.schemaVersion) + ".";
                return false;
            }
            selected = &migration;
        }
        if (selected == nullptr || selected->toVersion <= result.schemaVersion
            || selected->toVersion > targetSchemaVersion || !selected->apply) {
            result.status = SaveGameStatus::MigrationFailed;
            result.error = "No valid SaveGame migration from version "
                + std::to_string(result.schemaVersion) + " to version "
                + std::to_string(targetSchemaVersion) + ".";
            return false;
        }
        std::string migrationError;
        bool migrated = false;
        try {
            migrated = selected->apply(result.payloadJson, migrationError);
        } catch (const std::exception& exception) {
            migrationError = std::string("SaveGame migration threw: ")
                + exception.what();
        } catch (...) {
            migrationError = "SaveGame migration threw an unknown exception.";
        }
        if (!migrated) {
            result.status = SaveGameStatus::MigrationFailed;
            result.error = migrationError.empty()
                ? "SaveGame migration callback failed."
                : std::move(migrationError);
            return false;
        }
        Json migratedPayload;
        if (!parsePayload(result.payloadJson, migratedPayload, result.error)) {
            result.status = SaveGameStatus::MigrationFailed;
            return false;
        }
        result.payloadJson = migratedPayload.dump();
        result.schemaVersion = selected->toVersion;
        result.migrated = true;
    }
    return true;
}

} // namespace

SaveGameService::SaveGameService(SaveGameServiceConfig config)
    : _config(std::move(config))
{
    if (!_config.userDataPath.empty()) {
        _config.userDataPath = absoluteNormalized(_config.userDataPath);
    }
}

SaveGameOperationResult SaveGameService::save(
    std::string_view slot, std::uint32_t schemaVersion,
    std::string_view payloadJson) const
{
    SaveGameOperationResult result;
    if (_config.userDataPath.empty() || _config.projectId.empty()
        || !isPortableSlot(slot) || schemaVersion == 0u) {
        result.status = SaveGameStatus::InvalidArgument;
        result.error = "SaveGame requires a user-data path, project id, portable "
            "slot name, and schemaVersion >= 1.";
        return result;
    }

    Json payload;
    if (!parsePayload(payloadJson, payload, result.error)) {
        result.status = SaveGameStatus::InvalidArgument;
        return result;
    }

    const std::string primary = slotPath(slot);
    const std::string backup = backupPath(slot);
    if (ayt::io::File::exists(primary)) {
        const std::string previous = ayt::io::File::readAllText(primary);
        SaveGameEnvelope previousEnvelope;
        std::string ignoredError;
        if (parseEnvelope(previous, _config.projectId, slot,
                          previousEnvelope, ignoredError)
            && !atomicWriteText(backup, previous)) {
            result.status = SaveGameStatus::IoError;
            result.error = "Could not update the SaveGame recovery backup.";
            return result;
        }
    }

    const std::string envelope = serializeEnvelope(
        _config.projectId, slot, schemaVersion, payload);
    if (!atomicWriteText(primary, envelope)) {
        result.status = SaveGameStatus::IoError;
        result.error = "Could not atomically write the SaveGame slot.";
        return result;
    }
    return result;
}

SaveGameLoadResult SaveGameService::load(
    std::string_view slot, std::uint32_t targetSchemaVersion,
    const std::vector<SaveGameMigration>& migrations) const
{
    SaveGameLoadResult result;
    if (_config.userDataPath.empty() || _config.projectId.empty()
        || !isPortableSlot(slot) || targetSchemaVersion == 0u) {
        result.status = SaveGameStatus::InvalidArgument;
        result.error = "SaveGame load requires a configured service, portable "
            "slot name, and targetSchemaVersion >= 1.";
        return result;
    }

    result = loadFile(slotPath(slot), _config.projectId, slot);
    const bool primaryUnavailable = result.status == SaveGameStatus::NotFound
        || result.status == SaveGameStatus::Corrupt;
    if (primaryUnavailable) {
        const std::string primaryError = result.error;
        auto backup = loadFile(backupPath(slot), _config.projectId, slot);
        if (backup) {
            backup.status = SaveGameStatus::RecoveredFromBackup;
            result = std::move(backup);
        } else if (result.status == SaveGameStatus::NotFound
                   && backup.status == SaveGameStatus::NotFound) {
            return result;
        } else {
            result.status = SaveGameStatus::Corrupt;
            result.error = "Primary SaveGame is unavailable";
            if (!primaryError.empty()) result.error += ": " + primaryError;
            if (!backup.error.empty()) {
                result.error += "; recovery backup is invalid: " + backup.error;
            } else {
                result.error += "; no usable recovery backup exists.";
            }
            return result;
        }
    }

    const SaveGameStatus loadedStatus = result.status;
    if (!applyMigrations(result, targetSchemaVersion, migrations)) {
        return result;
    }
    result.status = loadedStatus;
    return result;
}

SaveGameOperationResult SaveGameService::remove(std::string_view slot) const
{
    SaveGameOperationResult result;
    if (_config.userDataPath.empty() || !isPortableSlot(slot)) {
        result.status = SaveGameStatus::InvalidArgument;
        result.error = "SaveGame remove requires a configured service and "
            "portable slot name.";
        return result;
    }
    const std::string primary = slotPath(slot);
    const std::string backup = backupPath(slot);
    const bool hadPrimary = ayt::io::File::exists(primary);
    const bool hadBackup = ayt::io::File::exists(backup);
    if (!hadPrimary && !hadBackup) {
        result.status = SaveGameStatus::NotFound;
        return result;
    }
    const bool removedPrimary = !hadPrimary || ayt::io::File::remove(primary);
    const bool removedBackup = !hadBackup || ayt::io::File::remove(backup);
    if (!removedPrimary || !removedBackup) {
        result.status = SaveGameStatus::IoError;
        result.error = "Could not remove the complete SaveGame slot.";
    }
    return result;
}

std::vector<std::string> SaveGameService::listSlots(std::string* error) const
{
    std::vector<std::string> slots;
    if (error != nullptr) error->clear();
    if (_config.userDataPath.empty()) {
        if (error != nullptr) *error = "SaveGame service is not configured.";
        return slots;
    }
    const fs::path directory = fs::path(_config.userDataPath) / "SaveGames";
    std::error_code scanError;
    const bool exists = fs::exists(directory, scanError);
    if (scanError) {
        if (error != nullptr) {
            *error = "Could not inspect the SaveGame directory: "
                + scanError.message();
        }
        return slots;
    }
    if (!exists) return slots;
    for (fs::directory_iterator it(directory, scanError), end;
         !scanError && it != end; it.increment(scanError)) {
        std::error_code typeError;
        const bool regular = it->is_regular_file(typeError);
        if (typeError) {
            scanError = typeError;
            break;
        }
        if (!regular) continue;
        const std::string name = it->path().filename().string();
        if (name.size() <= kSlotSuffix.size()
            || !std::equal(kSlotSuffix.rbegin(), kSlotSuffix.rend(),
                           name.rbegin())) {
            continue;
        }
        std::string slot = name.substr(0, name.size() - kSlotSuffix.size());
        if (isPortableSlot(slot)) slots.push_back(std::move(slot));
    }
    if (scanError && error != nullptr) {
        *error = "Could not enumerate SaveGame slots: " + scanError.message();
        slots.clear();
        return slots;
    }
    std::sort(slots.begin(), slots.end());
    return slots;
}

std::string SaveGameService::slotPath(std::string_view slot) const
{
    if (!isPortableSlot(slot) || _config.userDataPath.empty()) return {};
    return (fs::path(_config.userDataPath) / "SaveGames"
        / (std::string(slot) + std::string(kSlotSuffix))).string();
}

std::string SaveGameService::backupPath(std::string_view slot) const
{
    const std::string primary = slotPath(slot);
    return primary.empty() ? std::string{} : primary + ".bak";
}

std::string resolveGameUserDataPath(
    std::string_view projectId,
    std::string_view configuredPath,
    std::string_view commandLineOverride)
{
    if (!commandLineOverride.empty()) {
        return absoluteNormalized(std::string(commandLineOverride));
    }
    if (!configuredPath.empty()) {
        return absoluteNormalized(std::string(configuredPath));
    }

    fs::path base;
#if defined(_WIN32)
    if (const auto localAppData = environmentValue("LOCALAPPDATA")) {
        base = *localAppData;
    }
#elif defined(__APPLE__)
    if (const auto home = environmentValue("HOME")) {
        base = fs::path(*home) / "Library" / "Application Support";
    }
#else
    if (const auto xdgData = environmentValue("XDG_DATA_HOME")) {
        base = *xdgData;
    } else if (const auto home = environmentValue("HOME")) {
        base = fs::path(*home) / ".local" / "share";
    }
#endif
    if (base.empty()) {
        std::error_code error;
        base = fs::current_path(error);
        if (error) base = ".";
        base /= ".aliyat-user";
    }
    return absoluteNormalized(base / "Aliyat" / safeProjectFolder(projectId));
}

} // namespace ayt::app
