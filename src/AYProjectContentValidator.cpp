#include <AYApplication/ProjectContentValidator.h>

#include <AYApplication/GameFlowAssets.h>
#include <AYApplication/GameFlowContract.h>
#include <AYApplication/GameFlowStandardActions.h>
#include <AYResource/Loader/TilemapLoader.h>
#include <AYScene.h>
#include <nlohmann/json.hpp>

#ifndef AY_APPLICATION_CONTENT_VALIDATOR_HAS_UI
#define AY_APPLICATION_CONTENT_VALIDATOR_HAS_UI 0
#endif

#if AY_APPLICATION_CONTENT_VALIDATOR_HAS_UI
#include <AYUI/LayoutLoader.h>
#include <AYUI/UIFlow.h>
#include <AYUI/Widget.h>
#endif

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace ayt::app {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char byte) { return static_cast<char>(std::tolower(byte)); });
    return value;
}

bool endsWith(const std::string& value, const char* suffix)
{
    const std::size_t length = std::char_traits<char>::length(suffix);
    return value.size() >= length
        && value.compare(value.size() - length, length, suffix) == 0;
}

bool endsWithInsensitive(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size()) return false;
    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        const auto actual = static_cast<unsigned char>(value[offset + index]);
        const auto expected = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(actual) != std::tolower(expected)) return false;
    }
    return true;
}

bool hasWindowsDrivePrefix(std::string_view value) noexcept
{
    if (value.size() < 2 || value[1] != ':') return false;
    const char letter = value.front();
    return (letter >= 'A' && letter <= 'Z')
        || (letter >= 'a' && letter <= 'z');
}

bool isPortableRelativePath(std::string_view value)
{
    if (value.empty() || value.find('\\') != std::string_view::npos
        || hasWindowsDrivePrefix(value)) {
        return false;
    }
    const fs::path path(value);
    if (path.is_absolute() || path.has_root_name()
        || path.has_root_directory()) return false;
    for (const auto& part : path) {
        if (part == "..") return false;
    }
    return true;
}

bool pathComponentEqual(const fs::path& left, const fs::path& right)
{
#ifdef _WIN32
    return lower(left.string()) == lower(right.string());
#else
    return left == right;
#endif
}

bool isWithin(const fs::path& root, const fs::path& candidate)
{
    auto rootIt = root.begin();
    auto candidateIt = candidate.begin();
    for (; rootIt != root.end(); ++rootIt, ++candidateIt) {
        if (candidateIt == candidate.end()
            || !pathComponentEqual(*rootIt, *candidateIt)) return false;
    }
    return true;
}

bool resolveContainedPath(const fs::path& base, std::string_view relative,
                          fs::path& path, std::string& error)
{
    if (relative.find('\\') != std::string_view::npos) {
        error = "Path must use portable forward-slash separators: "
            + std::string(relative);
        return false;
    }
    if (!isPortableRelativePath(relative)) {
        error = "Path must be relative and stay inside the project: "
            + std::string(relative);
        return false;
    }
    std::error_code canonicalError;
    const fs::path canonicalBase = fs::canonical(base, canonicalError);
    if (canonicalError) {
        error = "Path root cannot be resolved: " + base.string();
        return false;
    }

    const fs::path candidate =
        (canonicalBase / fs::path(relative)).lexically_normal();
    if (!isWithin(canonicalBase, candidate)) {
        error = "Path escapes the project content root: "
            + std::string(relative);
        return false;
    }

    // Resolve the nearest existing ancestor instead of asking
    // weakly_canonical() to inspect a missing Windows leaf. This preserves
    // missing-file diagnostics while still exposing symlink escapes.
    fs::path probe = candidate;
    fs::path missingSuffix;
    while (true) {
        canonicalError.clear();
        const bool exists = fs::exists(probe, canonicalError);
        if (canonicalError) {
            error = "Path cannot be inspected: " + std::string(relative)
                + ": " + canonicalError.message();
            return false;
        }
        if (exists) break;
        if (probe.empty() || probe == probe.parent_path()) {
            error = "Path cannot be resolved: " + std::string(relative);
            return false;
        }
        missingSuffix = probe.filename() / missingSuffix;
        probe = probe.parent_path();
    }

    const fs::path resolvedPrefix = fs::canonical(probe, canonicalError);
    if (canonicalError) {
        error = "Path cannot be resolved: " + std::string(relative)
            + ": " + canonicalError.message();
        return false;
    }
    const fs::path resolved = missingSuffix.empty()
        ? resolvedPrefix.lexically_normal()
        : (resolvedPrefix / missingSuffix).lexically_normal();
    if (!isWithin(canonicalBase, resolved)) {
        error = "Path escapes the project content root: "
            + std::string(relative);
        return false;
    }
    path = resolved;
    error.clear();
    return true;
}

bool validateWidgetJson(const nlohmann::json& widget, std::string& error)
{
    if (!widget.is_object() || !widget.contains("type")
        || !widget["type"].is_string()
        || widget["type"].get_ref<const std::string&>().empty()) {
        error = "UI layout widget is missing a string type.";
        return false;
    }
    if (const auto children = widget.find("children"); children != widget.end()) {
        if (!children->is_array()) {
            error = "UI layout children must be an array.";
            return false;
        }
        for (const auto& child : *children) {
            if (!validateWidgetJson(child, error)) return false;
        }
    }
    for (const char* key : {"content", "bodyContent"}) {
        const auto child = widget.find(key);
        if (child != widget.end() && !child->is_null()
            && !validateWidgetJson(*child, error)) return false;
    }
    if (const auto tabs = widget.find("tabs"); tabs != widget.end()) {
        if (!tabs->is_array()) {
            error = "UI layout tabs must be an array.";
            return false;
        }
        for (const auto& tab : *tabs) {
            if (!tab.is_object()) {
                error = "UI layout tab must be an object.";
                return false;
            }
            const auto content = tab.find("content");
            if (content != tab.end() && !content->is_null()
                && !validateWidgetJson(*content, error)) return false;
        }
    }
    return true;
}

bool loadJson(const fs::path& path, nlohmann::json& document,
              std::string& error)
{
    try {
        std::error_code statusError;
        const bool regular = fs::is_regular_file(path, statusError);
        if (statusError || !regular) {
            error = statusError
                ? "File cannot be inspected: " + statusError.message()
                : "Path must name a regular file.";
            return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            error = "File could not be opened.";
            return false;
        }
        input >> document;
        return true;
    } catch (const std::exception& exception) {
        error = std::string("JSON is invalid: ") + exception.what();
        return false;
    }
}

bool validateHeadlessUi(const fs::path& path, std::string& error)
{
    nlohmann::json document;
    if (!loadJson(path, document, error)) return false;
    const nlohmann::json* root = &document;
    if (const auto wrapped = document.find("root"); wrapped != document.end()) {
        root = &*wrapped;
    }
    return validateWidgetJson(*root, error);
}

bool validateAuthoringTilemap(const fs::path& path, std::string& error)
{
    nlohmann::json document;
    if (!loadJson(path, document, error)) return false;
    if (!document.is_object()) {
        error = "Tilemap document must be an object.";
        return false;
    }
    const auto validPositive = [&](const char* key) {
        return document.contains(key) && document[key].is_number_unsigned()
            && document[key].get<std::uint64_t>() > 0u;
    };
    if (!validPositive("cols") || !validPositive("rows")
        || !validPositive("tileWidth") || !validPositive("tileHeight")) {
        error = "Tilemap dimensions and tile size must be positive integers.";
        return false;
    }
    const std::uint64_t columns = document["cols"].get<std::uint64_t>();
    const std::uint64_t rows = document["rows"].get<std::uint64_t>();
    constexpr std::uint64_t kMaximumCells = 4u * 1024u * 1024u;
    if (columns > kMaximumCells / rows) {
        error = "Tilemap exceeds the supported cell count.";
        return false;
    }
    const std::uint64_t cells = columns * rows;
    const auto layers = document.find("layers");
    if (layers == document.end() || !layers->is_array() || layers->empty()) {
        error = "Tilemap must contain at least one layer.";
        return false;
    }
    for (const auto& layer : *layers) {
        if (!layer.is_object() || !layer.contains("tiles")
            || !layer["tiles"].is_array()
            || layer["tiles"].size() != cells) {
            error = "Every Tilemap layer must contain exactly cols*rows tiles.";
            return false;
        }
    }
    return true;
}

bool parseValueType(std::string_view name, GameFlowValueType& type)
{
    if (name == "boolean") type = GameFlowValueType::Boolean;
    else if (name == "integer") type = GameFlowValueType::Integer;
    else if (name == "number") type = GameFlowValueType::Number;
    else if (name == "string") type = GameFlowValueType::String;
    else return false;
    return true;
}

bool parseReferenceKind(std::string_view name, GameFlowReferenceKind& kind)
{
    if (name == "asset") kind = GameFlowReferenceKind::AssetPath;
    else if (name == "world") kind = GameFlowReferenceKind::WorldId;
    else if (name == "ui-entry") kind = GameFlowReferenceKind::UIFlowEntry;
    else if (name == "ui-context") kind = GameFlowReferenceKind::UIContext;
    else if (name == "ui-signal") kind = GameFlowReferenceKind::UISignal;
    else return false;
    return true;
}

bool parseDefaultValue(const nlohmann::json& value,
                       GameFlowValueType type,
                       GameFlowValue& result)
{
    switch (type) {
    case GameFlowValueType::Boolean:
        if (!value.is_boolean()) return false;
        result = value.get<bool>();
        return true;
    case GameFlowValueType::Integer:
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) return false;
            result = static_cast<std::int64_t>(number);
            return true;
        }
        if (value.is_number_integer()) {
            result = value.get<std::int64_t>();
            return true;
        }
        return false;
    case GameFlowValueType::Number:
        if (value.is_number_unsigned()) {
            const auto number = value.get<std::uint64_t>();
            if (number > static_cast<std::uint64_t>(
                    std::numeric_limits<std::int64_t>::max())) return false;
            result = static_cast<std::int64_t>(number);
            return true;
        }
        if (value.is_number_integer()) {
            result = value.get<std::int64_t>();
            return true;
        }
        if (!value.is_number_float()) return false;
        result = value.get<double>();
        return true;
    case GameFlowValueType::String:
        if (!value.is_string()) return false;
        result = value.get<std::string>();
        return true;
    }
    return false;
}

bool hasOnlyKeys(const nlohmann::json& value,
                 std::initializer_list<std::string_view> allowed,
                 std::string_view role,
                 std::string& error)
{
    for (auto item = value.begin(); item != value.end(); ++item) {
        if (std::find(allowed.begin(), allowed.end(), item.key())
            == allowed.end()) {
            error = std::string(role) + " contains unknown field '"
                + item.key() + "'.";
            return false;
        }
    }
    return true;
}

bool parseFields(const nlohmann::json& value,
                 std::vector<GameFlowFieldDefinition>& fields,
                 std::string& error)
{
    if (!value.is_array()) {
        error = "Contract fields must be an array.";
        return false;
    }
    for (const auto& fieldJson : value) {
        if (!fieldJson.is_object()
            || !fieldJson.contains("id") || !fieldJson["id"].is_string()
            || fieldJson["id"].get_ref<const std::string&>().empty()
            || !fieldJson.contains("type")
            || !fieldJson["type"].is_string()) {
            error = "Every contract field needs non-empty id and type strings.";
            return false;
        }
        if (!hasOnlyKeys(fieldJson,
                {"id", "type", "required", "default"},
                "Contract field", error)) return false;
        GameFlowFieldDefinition field;
        field.id = fieldJson["id"].get<std::string>();
        if (!parseValueType(fieldJson["type"].get_ref<const std::string&>(),
                            field.type)) {
            error = "Unknown GameFlow field type for '" + field.id + "'.";
            return false;
        }
        if (const auto required = fieldJson.find("required");
            required != fieldJson.end()) {
            if (!required->is_boolean()) {
                error = "Contract field required must be boolean.";
                return false;
            }
            field.required = required->get<bool>();
        }
        if (const auto defaultValue = fieldJson.find("default");
            defaultValue != fieldJson.end()
            && !parseDefaultValue(*defaultValue, field.type,
                                  field.defaultValue)) {
            error = "Contract default for '" + field.id
                + "' does not match its type.";
            return false;
        }
        fields.push_back(std::move(field));
    }
    return true;
}

bool parseReferences(const nlohmann::json& value,
                     std::vector<GameFlowActionReferenceDefinition>& references,
                     std::string& error)
{
    if (!value.is_array()) {
        error = "Action references must be an array.";
        return false;
    }
    for (const auto& referenceJson : value) {
        if (!referenceJson.is_object()
            || !referenceJson.contains("argument")
            || !referenceJson["argument"].is_string()
            || !referenceJson.contains("kind")
            || !referenceJson["kind"].is_string()) {
            error = "Every action reference needs argument and kind strings.";
            return false;
        }
        if (!hasOnlyKeys(referenceJson,
                {"argument", "kind", "allowEmpty"},
                "Action reference", error)) return false;
        GameFlowActionReferenceDefinition reference;
        reference.argumentId = referenceJson["argument"].get<std::string>();
        if (!parseReferenceKind(
                referenceJson["kind"].get_ref<const std::string&>(),
                reference.kind)) {
            error = "Unknown action reference kind for '"
                + reference.argumentId + "'.";
            return false;
        }
        if (const auto allowEmpty = referenceJson.find("allowEmpty");
            allowEmpty != referenceJson.end()) {
            if (!allowEmpty->is_boolean()) {
                error = "Action reference allowEmpty must be boolean.";
                return false;
            }
            reference.allowEmpty = allowEmpty->get<bool>();
        }
        references.push_back(std::move(reference));
    }
    return true;
}

bool registerContractManifest(const fs::path& path,
                               GameFlowActionRegistry& registry,
                               std::string& error)
{
    nlohmann::json manifest;
    if (!loadJson(path, manifest, error)) return false;
    return loadGameFlowContract(manifest.dump(), registry, &error);
}

struct UiSignalField
{
    std::string id;
    GameFlowValueType type = GameFlowValueType::String;
    bool required = false;
    bool hasDefault = false;
};

struct UiReferenceCatalog
{
    std::string defaultEntry;
    std::set<std::string, std::less<>> entries;
    std::set<std::string, std::less<>> contexts;
    std::map<std::string, std::vector<UiSignalField>, std::less<>> signals;
};

bool parseUiType(std::string_view name, GameFlowValueType& type)
{
    if (name == "entity" || name == "asset") {
        type = GameFlowValueType::String;
        return true;
    }
    return parseValueType(name, type);
}

bool loadUiReferenceCatalog(const fs::path& path,
                            UiReferenceCatalog& catalog,
                            std::string& error)
{
    try {
    nlohmann::json document;
    if (!loadJson(path, document, error)) return false;
#if AY_APPLICATION_CONTENT_VALIDATOR_HAS_UI
    ayt::ui::UIFlowDocument flow;
    std::vector<ayt::ui::UIFlowDiagnostic> diagnostics;
    const std::string jsonText = document.dump();
    if (!ayt::ui::UIFlowSerializer::deserialize(
            jsonText, flow, &diagnostics)) {
        error = "UIFlow validation failed.";
        if (!diagnostics.empty()) {
            error = diagnostics.front().path.empty()
                ? diagnostics.front().message
                : diagnostics.front().path + ": "
                    + diagnostics.front().message;
        }
        return false;
    }
    catalog.defaultEntry = flow.defaultEntry;
    for (const auto& entry : flow.entries) catalog.entries.insert(entry.id);
    for (const auto& context : flow.contexts) {
        catalog.contexts.insert(context.id);
    }
    const auto convertType = [](ayt::ui::UIFlowValueType type) {
        switch (type) {
        case ayt::ui::UIFlowValueType::Boolean:
            return GameFlowValueType::Boolean;
        case ayt::ui::UIFlowValueType::Integer:
            return GameFlowValueType::Integer;
        case ayt::ui::UIFlowValueType::Number:
            return GameFlowValueType::Number;
        case ayt::ui::UIFlowValueType::String:
        case ayt::ui::UIFlowValueType::Entity:
        case ayt::ui::UIFlowValueType::Asset:
            return GameFlowValueType::String;
        }
        return GameFlowValueType::String;
    };
    for (const auto& signal : flow.signals) {
        std::vector<UiSignalField> fields;
        fields.reserve(signal.payload.size());
        for (const auto& value : signal.payload) {
            fields.push_back({value.id, convertType(value.type),
                value.required,
                !std::holds_alternative<std::monostate>(
                    value.defaultValue.data)});
        }
        catalog.signals.emplace(signal.id, std::move(fields));
    }
    error.clear();
    return true;
#else
    bool supportedSchema = false;
    if (document.is_object()) {
        const auto version = document.find("schemaVersion");
        if (version != document.end()) {
            if (version->is_number_unsigned()) {
                supportedSchema = version->get<std::uint64_t>() == 1u;
            } else if (version->is_number_integer()) {
                supportedSchema = version->get<std::int64_t>() == 1;
            }
        }
    }
    if (!supportedSchema) {
        error = "UIFlow reference catalog requires schemaVersion 1.";
        return false;
    }
    if (!document.contains("id") || !document["id"].is_string()
        || document["id"].get_ref<const std::string&>().empty()) {
        error = "UIFlow reference catalog requires a non-empty id.";
        return false;
    }
    const auto readIds = [&](const char* key,
                             std::set<std::string, std::less<>>& values) {
        const auto array = document.find(key);
        if (array == document.end()) return true;
        if (!array->is_array()) {
            error = std::string("UIFlow '") + key + "' must be an array.";
            return false;
        }
        for (const auto& item : *array) {
            if (!item.is_object() || !item.contains("id")
                || !item["id"].is_string()
                || item["id"].get_ref<const std::string&>().empty()) {
                error = std::string("Every UIFlow ") + key
                    + " item needs a non-empty id.";
                return false;
            }
            if (!values.insert(item["id"].get<std::string>()).second) {
                error = std::string("Duplicate UIFlow ") + key + " id.";
                return false;
            }
        }
        return true;
    };
    if (!readIds("entries", catalog.entries)
        || !readIds("contexts", catalog.contexts)) return false;

    const auto defaultEntry = document.find("defaultEntry");
    if (defaultEntry != document.end()) {
        if (!defaultEntry->is_string()) {
            error = "UIFlow 'defaultEntry' must be a string.";
            return false;
        }
        catalog.defaultEntry = defaultEntry->get<std::string>();
        if (!catalog.defaultEntry.empty()
            && !catalog.entries.contains(catalog.defaultEntry)) {
            error = "UIFlow defaultEntry references an unknown entry.";
            return false;
        }
    }

    const auto signals = document.find("signals");
    if (signals == document.end()) {
        error.clear();
        return true;
    }
    if (!signals->is_array()) {
        error = "UIFlow 'signals' must be an array.";
        return false;
    }
    for (const auto& signalJson : *signals) {
        if (!signalJson.is_object() || !signalJson.contains("id")
            || !signalJson["id"].is_string()
            || signalJson["id"].get_ref<const std::string&>().empty()) {
            error = "Every UIFlow signal needs a non-empty id.";
            return false;
        }
        std::vector<UiSignalField> fields;
        const auto payload = signalJson.value(
            "payload", nlohmann::json::array());
        if (!payload.is_array()) {
            error = "UIFlow signal payload must be an array.";
            return false;
        }
        std::set<std::string, std::less<>> fieldIds;
        for (const auto& fieldJson : payload) {
            if (!fieldJson.is_object() || !fieldJson.contains("id")
                || !fieldJson["id"].is_string()
                || fieldJson["id"].get_ref<const std::string&>().empty()
                || !fieldJson.contains("type")
                || !fieldJson["type"].is_string()) {
                error = "Every UIFlow signal field needs a non-empty id and a type string.";
                return false;
            }
            UiSignalField field;
            field.id = fieldJson["id"].get<std::string>();
            if (!fieldIds.insert(field.id).second
                || !parseUiType(
                    fieldJson["type"].get_ref<const std::string&>(),
                    field.type)) {
                error = "Invalid UIFlow signal field '" + field.id + "'.";
                return false;
            }
            const auto required = fieldJson.find("required");
            if (required != fieldJson.end() && !required->is_boolean()) {
                error = "UIFlow signal field required must be boolean.";
                return false;
            }
            field.required = required != fieldJson.end()
                && required->get<bool>();
            if (const auto defaultValue = fieldJson.find("default");
                defaultValue != fieldJson.end() && !defaultValue->is_null()) {
                GameFlowValue parsedDefault;
                if (!parseDefaultValue(
                        *defaultValue, field.type, parsedDefault)) {
                    error = "UIFlow signal field default for '" + field.id
                        + "' does not match its type.";
                    return false;
                }
                field.hasDefault = true;
            }
            fields.push_back(std::move(field));
        }
        const std::string id = signalJson["id"].get<std::string>();
        if (!catalog.signals.emplace(id, std::move(fields)).second) {
            error = "Duplicate UIFlow signal id '" + id + "'.";
            return false;
        }
    }
    error.clear();
    return true;
#endif
    } catch (const std::exception& exception) {
        error = std::string("UIFlow reference catalog is invalid: ")
            + exception.what();
        return false;
    } catch (...) {
        error = "UIFlow reference catalog is invalid.";
        return false;
    }
}

const GameFlowFieldDefinition* findField(
    const std::vector<GameFlowFieldDefinition>& fields,
    std::string_view id)
{
    const auto found = std::find_if(fields.begin(), fields.end(),
        [id](const GameFlowFieldDefinition& field) {
            return field.id == id;
        });
    return found == fields.end() ? nullptr : &*found;
}

bool isNull(const GameFlowValue& value)
{
    return std::holds_alternative<std::monostate>(value.data);
}

bool validateIntentToSignal(
    const GameFlowIntentDefinition& intent,
    std::string_view signalId,
    const std::vector<UiSignalField>& fields,
    std::string& error)
{
    for (const auto& target : fields) {
        const GameFlowFieldDefinition* source = findField(
            intent.payload, target.id);
        if (source == nullptr) {
            if (target.required && !target.hasDefault) {
                error = "GameFlow intent '" + intent.id
                    + "' cannot supply required UI signal field '"
                    + target.id + "'.";
                return false;
            }
            continue;
        }
        const bool compatible = source->type == target.type
            || (source->type == GameFlowValueType::Integer
                && target.type == GameFlowValueType::Number);
        if (!compatible) {
            error = "GameFlow intent '" + intent.id + "' field '"
                + target.id + "' is incompatible with UI signal '"
                + std::string(signalId) + "'.";
            return false;
        }
        if (target.required && !target.hasDefault
            && !source->required && isNull(source->defaultValue)) {
            error = "GameFlow intent '" + intent.id
                + "' may omit required UI signal field '" + target.id + "'.";
            return false;
        }
    }
    return true;
}

void addIssue(ProjectContentValidationResult& result,
              const fs::path& path, std::string message)
{
    result.issues.push_back({path.string(), std::move(message)});
}

void sortDependencies(ProjectContentValidationResult& result)
{
    auto& dependencies = result.gameFlowDependencies;
    std::sort(dependencies.begin(), dependencies.end(),
        [](const ProjectContentDependency& left,
           const ProjectContentDependency& right) {
            if (left.kind != right.kind) return left.kind < right.kind;
            if (left.source != right.source) return left.source < right.source;
            return left.target < right.target;
        });
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end(),
        [](const ProjectContentDependency& left,
           const ProjectContentDependency& right) {
            return left.kind == right.kind && left.source == right.source
                && left.target == right.target;
        }), dependencies.end());
}

} // namespace

const char* projectContentDependencyKindName(
    ProjectContentDependencyKind kind) noexcept
{
    switch (kind) {
    case ProjectContentDependencyKind::GameFlow: return "gameflow";
    case ProjectContentDependencyKind::Asset: return "asset";
    case ProjectContentDependencyKind::World: return "world";
    case ProjectContentDependencyKind::UIFlowEntry: return "ui-entry";
    case ProjectContentDependencyKind::UIContext: return "ui-context";
    case ProjectContentDependencyKind::UISignal: return "ui-signal";
    }
    return "unknown";
}

ProjectContentValidationResult validateProjectContent(
    const std::string& projectRoot,
    ProjectContentValidationProfile profile)
{
    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions =
        profile == ProjectContentValidationProfile::FullClient;
    return validateProjectContent(projectRoot, profile, std::move(options));
}

ProjectContentValidationResult validateProjectContent(
    const std::string& projectRoot,
    ProjectContentValidationProfile profile,
    ProjectContentValidationOptions options)
{
    ProjectContentValidationResult result;
    result.profile = profile;
    try {
    std::error_code rootError;
    fs::path root = fs::absolute(fs::path(projectRoot), rootError);
    if (rootError) {
        result.issues.push_back({projectRoot,
            "Project root cannot be resolved: " + rootError.message()});
        return result;
    }
    root = fs::weakly_canonical(root, rootError);
    if (rootError) {
        result.issues.push_back({projectRoot,
            "Project root cannot be resolved: " + rootError.message()});
        return result;
    }
    fs::path descriptorPath;
    std::string pathError;
    if (!resolveContainedPath(root, "project.ayproject.json",
            descriptorPath, pathError)) {
        addIssue(result, root, std::move(pathError));
        return result;
    }
    nlohmann::json descriptor;
    std::error_code descriptorStatusError;
    const bool descriptorExists = fs::exists(
        descriptorPath, descriptorStatusError);
    if (descriptorStatusError) {
        addIssue(result, descriptorPath,
            "Project descriptor cannot be inspected: "
                + descriptorStatusError.message());
        return result;
    }
    const bool hasDescriptor = descriptorExists
        && fs::is_regular_file(descriptorPath, descriptorStatusError);
    if (descriptorStatusError || (descriptorExists && !hasDescriptor)) {
        addIssue(result, descriptorPath, descriptorStatusError
            ? "Project descriptor cannot be inspected: "
                + descriptorStatusError.message()
            : "Project descriptor must be a regular file.");
        return result;
    }
    if (hasDescriptor) {
        std::string descriptorError;
        if (!loadJson(descriptorPath, descriptor, descriptorError)) {
            addIssue(result, descriptorPath, std::move(descriptorError));
            return result;
        }
        if (!descriptor.is_object()) {
            addIssue(result, descriptorPath,
                "Project descriptor root must be an object.");
            return result;
        }
    }

    std::string assetRoot = "Assets";
    if (descriptor.is_object()) {
        if (const auto paths = descriptor.find("paths");
            paths != descriptor.end()) {
            if (!paths->is_object()
                || !paths->contains("assets")
                || !(*paths)["assets"].is_string()) {
                addIssue(result, descriptorPath,
                    "Project paths.assets must be a string.");
                return result;
            }
            assetRoot = (*paths)["assets"].get<std::string>();
        } else if (const auto legacy = descriptor.find("assetRoot");
                   legacy != descriptor.end()) {
            if (!legacy->is_string()) {
                addIssue(result, descriptorPath,
                    "Project assetRoot must be a string.");
                return result;
            }
            assetRoot = legacy->get<std::string>();
        }
    }
    if (!isPortableRelativePath(assetRoot)) {
        addIssue(result, descriptorPath,
            "Project asset root must be a relative path inside the project.");
        return result;
    }
    fs::path assets;
    if (!resolveContainedPath(root, assetRoot, assets, pathError)) {
        addIssue(result, descriptorPath, std::move(pathError));
        return result;
    }
    std::error_code assetsError;
    const bool hasAssets = fs::is_directory(assets, assetsError);
    if (assetsError || !hasAssets) {
        addIssue(result, assets, assetsError
            ? "Project asset root cannot be inspected: "
                + assetsError.message()
            : "Project asset root is missing.");
        return result;
    }

    std::set<std::string, std::less<>> worldIds;
    const auto worlds = descriptor.is_object()
        ? descriptor.find("worlds") : descriptor.end();
    if (descriptor.is_object() && worlds != descriptor.end()) {
        if (!worlds->is_array()) {
            addIssue(result, descriptorPath,
                "Project worlds must be an array.");
        } else {
            std::size_t worldIndex = 0;
            for (const auto& world : *worlds) {
                const std::string source = "$.worlds["
                    + std::to_string(worldIndex++) + "]";
                if (!world.is_object() || !world.contains("id")
                    || !world["id"].is_string()
                    || world["id"].get_ref<const std::string&>().empty()) {
                    result.issues.push_back({descriptorPath.string() + " " + source,
                        "Project World requires a non-empty id."});
                    continue;
                }
                const std::string worldId = world["id"].get<std::string>();
                if (!worldIds.insert(worldId).second) {
                    result.issues.push_back({descriptorPath.string() + " " + source,
                        "Duplicate project World id '" + worldId + "'."});
                }
                const auto require = [&](const nlohmann::json& value,
                                         const char* role) {
                    if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
                        result.issues.push_back({descriptorPath.string() + " " + source,
                            std::string("Project World ") + role
                                + " must be a non-empty relative path."});
                        return;
                    }
                    fs::path resolved;
                    std::string error;
                    const std::string relative = value.get<std::string>();
                    if (!resolveContainedPath(assets, relative, resolved, error)) {
                        result.issues.push_back({descriptorPath.string() + " " + source,
                            std::move(error)});
                    } else {
                        std::error_code fileError;
                        const bool exists = fs::is_regular_file(
                            resolved, fileError);
                        if (fileError) {
                            addIssue(result, resolved,
                                std::string("Project World ") + role
                                    + " cannot be inspected: "
                                    + fileError.message());
                        } else if (!exists) {
                            addIssue(result, resolved,
                                std::string("Project World references a missing ")
                                    + role + ".");
                        }
                    }
                };
                if (const auto scene = world.find("scene"); scene != world.end()) {
                    require(*scene, "Scene");
                } else {
                    result.issues.push_back({descriptorPath.string() + " " + source,
                        "Project World requires a Scene path."});
                }
                if (const auto ui = world.find("ui"); ui != world.end()
                    && !(ui->is_string()
                         && ui->get_ref<const std::string&>().empty())) {
                    require(*ui, "UI layout");
                }
                if (const auto maps = world.find("tilemaps");
                    maps != world.end()) {
                    if (!maps->is_array()) {
                        result.issues.push_back({descriptorPath.string() + " " + source,
                            "Project World tilemaps must be an array."});
                    } else {
                        for (const auto& map : *maps) require(map, "Tilemap");
                    }
                }
            }
        }
    }

    std::string uiFlowAsset;
    if (descriptor.is_object()) {
        if (const auto ui = descriptor.find("ui"); ui != descriptor.end()) {
            if (!ui->is_object()) {
                addIssue(result, descriptorPath,
                    "Project ui settings must be an object.");
            } else if (const auto flow = ui->find("flow"); flow != ui->end()) {
                if (!flow->is_string()) {
                    addIssue(result, descriptorPath,
                        "Project ui.flow must be a string.");
                } else {
                    uiFlowAsset = flow->get<std::string>();
                }
            }
        }
    }

    GameFlowAssetCatalog flowCatalog;
    std::string flowScanError;
    if (!scanGameFlowAssets(assets.string(), flowCatalog, &flowScanError)) {
        addIssue(result, assets, std::move(flowScanError));
    } else {
        result.gameFlows = flowCatalog.scannedFiles;
        for (const auto& issue : flowCatalog.issues) {
            result.issues.push_back({issue.path, issue.message});
        }
    }

    std::string startupFlow;
    if (descriptor.is_object()) {
        if (const auto value = descriptor.find("startupFlow");
            value != descriptor.end()) {
            if (!value->is_string()) {
                addIssue(result, descriptorPath,
                    "Project startupFlow must be a string.");
            } else {
                startupFlow = value->get<std::string>();
            }
        }
    }

    if (!startupFlow.empty()) {
        fs::path startupPath;
        pathError.clear();
        if (!endsWithInsensitive(startupFlow, ".gameflow.json")
            || !resolveContainedPath(
                assets, startupFlow, startupPath, pathError)) {
            addIssue(result, descriptorPath,
                pathError.empty()
                    ? "Project startupFlow must reference a relative "
                        "*.gameflow.json asset."
                    : std::move(pathError));
        } else {
            const GameFlowAssetDocument* rootFlow =
                flowCatalog.findByAssetPath(startupFlow);
            if (rootFlow == nullptr) {
                addIssue(result, startupPath,
                    "Project startupFlow is missing or structurally invalid.");
            } else {
                GameFlowActionRegistry registry;
                std::string registryError;
                bool registryReady = true;
                if (!registerGameFlowWorldActionType(registry, &registryError)) {
                    addIssue(result, descriptorPath, std::move(registryError));
                    registryReady = false;
                }

                const bool enableUiActions = options.enableGameFlowUIActions
                    || profile == ProjectContentValidationProfile::FullClient;
                if (enableUiActions) {
                    if (!registerGameFlowUIActionTypes(
                            registry, &registryError)) {
                        addIssue(result, descriptorPath,
                            std::move(registryError));
                        registryReady = false;
                    }
                }

                std::string contractAsset = options.gameFlowContractPath;
                if (contractAsset.empty() && descriptor.is_object()) {
                    if (const auto gameFlow = descriptor.find("gameFlow");
                        gameFlow != descriptor.end()) {
                        if (!gameFlow->is_object()) {
                            addIssue(result, descriptorPath,
                                "Project gameFlow settings must be an object.");
                            registryReady = false;
                        } else if (const auto contract =
                                   gameFlow->find("contract");
                                   contract != gameFlow->end()) {
                            if (!contract->is_string()) {
                                addIssue(result, descriptorPath,
                                    "Project gameFlow.contract must be a string.");
                                registryReady = false;
                            } else {
                                contractAsset = contract->get<std::string>();
                            }
                        }
                    }
                }
                if (!contractAsset.empty()) {
                    fs::path contractPath;
                    if (!resolveContainedPath(
                            assets, contractAsset, contractPath, pathError)) {
                        addIssue(result, descriptorPath, std::move(pathError));
                        registryReady = false;
                    } else if (!registerContractManifest(
                                   contractPath, registry, registryError)) {
                        addIssue(result, contractPath, std::move(registryError));
                        registryReady = false;
                    }
                }
                if (options.configureGameFlow) {
                    try {
                        if (!options.configureGameFlow(
                                registry, registryError)) {
                            addIssue(result, descriptorPath,
                                registryError.empty()
                                    ? "GameFlow registry configuration failed."
                                    : std::move(registryError));
                            registryReady = false;
                        }
                    } catch (const std::exception& exception) {
                        addIssue(result, descriptorPath,
                            std::string("GameFlow registry configuration threw: ")
                                + exception.what());
                        registryReady = false;
                    } catch (...) {
                        addIssue(result, descriptorPath,
                            "GameFlow registry configuration threw an unknown exception.");
                        registryReady = false;
                    }
                }

                // Project contracts may use replace=true. Re-check the
                // engine-owned contracts after every extension is applied so
                // content validation cannot accept a registry that the live
                // World/UI adapters will reject during installation.
                if (registryReady
                    && !registerGameFlowWorldActionType(
                        registry, &registryError)) {
                    addIssue(result, descriptorPath, std::move(registryError));
                    registryReady = false;
                }
                if (registryReady && enableUiActions
                    && !registerGameFlowUIActionTypes(
                        registry, &registryError)) {
                    addIssue(result, descriptorPath, std::move(registryError));
                    registryReady = false;
                }

                GameFlowProgram program;
                std::vector<GameFlowDiagnostic> diagnostics;
                GameFlowProgramBuildOptions buildOptions;
                buildOptions.maxCallDepth = options.maxGameFlowCallDepth;
                buildOptions.maxFlows = options.maxGameFlows;
                if (registryReady && !buildGameFlowProgram(
                        rootFlow->document, registry,
                        [&flowCatalog](std::string_view id,
                                       GameFlowDocument& document,
                                       std::string& error) {
                            return flowCatalog.resolve(id, document, error);
                        },
                        program, &diagnostics, buildOptions)) {
                    for (const auto& diagnostic : diagnostics) {
                        result.issues.push_back({startupPath.string()
                            + (diagnostic.path.empty() ? std::string{}
                                : " " + diagnostic.path),
                            diagnostic.message});
                    }
                    if (diagnostics.empty()) {
                        addIssue(result, startupPath,
                            "GameFlow program normalization failed.");
                    }
                } else if (registryReady) {
                    // Match GameProject's fixed startup contract before the
                    // editor or CLI reports the project as runnable. The
                    // project descriptor currently has no root-parameter or
                    // startup-payload source, so both inputs are empty here
                    // exactly as they are in runGameProject().
                    GameFlowCoordinator startupPreflight;
                    std::string startupError;
                    if (!startupPreflight.setProgram(
                            &program, &registry, {}, &startupError)) {
                        addIssue(result, startupPath,
                            startupError.empty()
                                ? "GameFlow root parameters are invalid."
                                : std::move(startupError));
                    } else {
                        GameFlowRequestResult startupRequest =
                            startupPreflight.request(
                                kGameFlowDefaultStartupIntent);
                        if (!startupRequest) {
                            addIssue(result, startupPath,
                                "Startup GameFlow intent '"
                                    + std::string(
                                        kGameFlowDefaultStartupIntent)
                                    + "' was rejected: "
                                    + std::move(startupRequest.message));
                        }
                    }

                    result.gameFlowDependencies.push_back({
                        ProjectContentDependencyKind::GameFlow,
                        "project/startupFlow", rootFlow->assetPath});

                    std::map<std::string,
                             const GameFlowAssetDocument*, std::less<>>
                        flowAssetsById;
                    for (const auto& candidate : flowCatalog.documents) {
                        const auto [found, inserted] = flowAssetsById.emplace(
                            candidate.document.id, &candidate);
                        if (!inserted) found->second = nullptr;
                    }

                    std::optional<UiReferenceCatalog> uiReferenceCatalog;
                    std::string uiReferenceCatalogError;
                    fs::path uiFlowPath;
                    bool uiReferenceCatalogAttempted = false;
                    const auto loadUiCatalog = [&]()
                        -> const UiReferenceCatalog* {
                        if (!uiReferenceCatalogAttempted) {
                            uiReferenceCatalogAttempted = true;
                            if (uiFlowAsset.empty()) {
                                uiReferenceCatalogError =
                                    "Project ui.flow is not configured.";
                            } else if (!resolveContainedPath(
                                           assets, uiFlowAsset, uiFlowPath,
                                           uiReferenceCatalogError)) {
                                // The path resolver supplies the diagnostic.
                            } else {
                                UiReferenceCatalog candidate;
                                if (loadUiReferenceCatalog(
                                        uiFlowPath, candidate,
                                        uiReferenceCatalogError)) {
                                    uiReferenceCatalog = std::move(candidate);
                                }
                            }
                        }
                        return uiReferenceCatalog
                            ? &*uiReferenceCatalog : nullptr;
                    };
                    for (const auto& [flowId, plan] : program.plans) {
                        for (const auto& transition : plan.document.transitions) {
                            const auto* intent = plan.document.findIntent(
                                transition.triggerIntent);
                            for (const auto& action : transition.actions) {
                                std::string semanticError;
                                if (!validateGameFlowStandardActionSemantics(
                                        action, &semanticError)) {
                                    result.issues.push_back({startupPath.string(),
                                        flowId + "::" + transition.id + "::"
                                            + action.action + ": "
                                            + semanticError});
                                }
                                if (action.action == kGameFlowActionEnter) {
                                    const auto argument = action.arguments.find(
                                        std::string(kGameFlowSubflowIdArgument));
                                    const auto* subflowId =
                                        argument == action.arguments.end()
                                        ? nullptr
                                        : std::get_if<std::string>(
                                            &argument->second.data);
                                    const auto child = subflowId == nullptr
                                        ? flowAssetsById.end()
                                        : flowAssetsById.find(*subflowId);
                                    if (child != flowAssetsById.end()
                                        && child->second != nullptr) {
                                        result.gameFlowDependencies.push_back({
                                            ProjectContentDependencyKind::GameFlow,
                                            flowId + "::" + transition.id + "::"
                                                + action.action + ".subflowId",
                                            child->second->assetPath});
                                    }
                                }
                                const auto* definition =
                                    registry.findAction(action.action);
                                if (definition == nullptr) continue;
                                for (const auto& reference :
                                     definition->references) {
                                    const auto argument = action.arguments.find(
                                        reference.argumentId);
                                    const auto* target =
                                        argument == action.arguments.end()
                                        ? nullptr
                                        : std::get_if<std::string>(
                                            &argument->second.data);
                                    const std::string source = flowId + "::"
                                        + transition.id + "::" + action.action
                                        + "." + reference.argumentId;
                                    if (target == nullptr
                                        && !reference.allowEmpty) {
                                        result.issues.push_back({startupPath.string(),
                                            source
                                                + " requires a non-empty reference."});
                                        continue;
                                    }
                                    std::string resolvedTarget = target == nullptr
                                        ? std::string{} : *target;
                                    if (resolvedTarget.empty()
                                        && !reference.allowEmpty) {
                                        result.issues.push_back({startupPath.string(),
                                            source
                                                + " requires a non-empty reference."});
                                        continue;
                                    }
                                    if (resolvedTarget.empty()
                                        && reference.kind
                                            != GameFlowReferenceKind::UIFlowEntry) {
                                        continue;
                                    }

                                    if (reference.kind
                                        == GameFlowReferenceKind::WorldId) {
                                        if (!worldIds.contains(resolvedTarget)) {
                                            result.issues.push_back({startupPath.string(),
                                                source + " references unknown World '"
                                                    + resolvedTarget + "'."});
                                        } else {
                                            result.gameFlowDependencies.push_back({
                                                ProjectContentDependencyKind::World,
                                                source, resolvedTarget});
                                        }
                                        continue;
                                    }
                                    if (reference.kind
                                        == GameFlowReferenceKind::AssetPath) {
                                        fs::path referencedPath;
                                        if (!resolveContainedPath(assets, resolvedTarget,
                                                referencedPath, pathError)) {
                                            result.issues.push_back({startupPath.string(),
                                                source + ": " + pathError});
                                        } else {
                                            std::error_code fileError;
                                            const bool exists = fs::is_regular_file(
                                                referencedPath, fileError);
                                            if (fileError) {
                                                addIssue(result, referencedPath,
                                                    source
                                                        + " cannot inspect the referenced asset: "
                                                        + fileError.message());
                                            } else if (!exists) {
                                                addIssue(result, referencedPath,
                                                    source
                                                        + " references a missing asset.");
                                            } else {
                                                result.gameFlowDependencies.push_back({
                                                    ProjectContentDependencyKind::Asset,
                                                    source, resolvedTarget});
                                            }
                                        }
                                        continue;
                                    }

                                    const UiReferenceCatalog* uiCatalog =
                                        loadUiCatalog();
                                    if (uiCatalog == nullptr) {
                                        result.issues.push_back({startupPath.string(),
                                            source
                                                + " requires a valid project ui.flow"
                                                + (uiReferenceCatalogError.empty()
                                                    ? std::string(".")
                                                    : ": "
                                                        + uiReferenceCatalogError)});
                                        continue;
                                    }
                                    if (resolvedTarget.empty()) {
                                        resolvedTarget = uiCatalog->defaultEntry;
                                        if (resolvedTarget.empty()) {
                                            // UIFlowRuntime::start("") is a
                                            // valid no-entry start when the
                                            // document has no defaultEntry.
                                            continue;
                                        }
                                    }
                                    bool exists = false;
                                    ProjectContentDependencyKind dependencyKind =
                                        ProjectContentDependencyKind::UIFlowEntry;
                                    if (reference.kind
                                        == GameFlowReferenceKind::UIFlowEntry) {
                                        exists = uiCatalog->entries.contains(
                                            resolvedTarget);
                                    } else if (reference.kind
                                        == GameFlowReferenceKind::UIContext) {
                                        dependencyKind =
                                            ProjectContentDependencyKind::UIContext;
                                        exists = uiCatalog->contexts.contains(
                                            resolvedTarget);
                                    } else {
                                        dependencyKind =
                                            ProjectContentDependencyKind::UISignal;
                                        const auto signal =
                                            uiCatalog->signals.find(resolvedTarget);
                                        exists = signal != uiCatalog->signals.end();
                                        if (exists && intent != nullptr
                                            && !validateIntentToSignal(
                                                *intent, resolvedTarget,
                                                signal->second, pathError)) {
                                            result.issues.push_back({
                                                startupPath.string(),
                                                source + ": " + pathError});
                                        }
                                    }
                                    if (!exists) {
                                        result.issues.push_back({startupPath.string(),
                                            source + " references unknown "
                                                + gameFlowReferenceKindName(
                                                    reference.kind)
                                                + " '" + resolvedTarget + "'."});
                                    } else {
                                        result.gameFlowDependencies.push_back({
                                            dependencyKind, source,
                                            resolvedTarget});
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    std::error_code scanError;
    for (fs::recursive_directory_iterator it(assets,
             fs::directory_options::none, scanError), end;
         !scanError && it != end; it.increment(scanError)) {
        const fs::path logicalPath = it->path().lexically_normal();
        std::error_code entryError;
        const fs::path resolvedPath = fs::weakly_canonical(
            logicalPath, entryError);
        if (entryError) {
            result.issues.push_back({logicalPath.string(),
                "Project asset path cannot be resolved: "
                    + entryError.message()});
            continue;
        }
        if (!isWithin(assets, resolvedPath)) {
            result.issues.push_back({logicalPath.string(),
                "Project asset resolves outside the project asset root '"
                    + assets.string() + "' as '" + resolvedPath.string()
                    + "'."});
            continue;
        }
        if (!fs::is_regular_file(resolvedPath, entryError)) {
            if (entryError) {
                result.issues.push_back({logicalPath.string(),
                    "Project asset cannot be inspected: "
                        + entryError.message()});
            }
            continue;
        }
        const std::string name = lower(logicalPath.filename().string());
        if (endsWith(name, ".ayscene")) {
            ++result.scenes;
            ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "validation");
            ayt::serializer::SerializeError error;
            if (!scene.load(resolvedPath.string(), &error)) {
                result.issues.push_back({logicalPath.string(), error.message});
            }
        } else if (endsWith(name, ".ui.json")) {
            ++result.uiLayouts;
            std::string error;
            if (!validateHeadlessUi(resolvedPath, error)) {
                result.issues.push_back({logicalPath.string(), error});
                continue;
            }
            if (profile == ProjectContentValidationProfile::FullClient) {
#if AY_APPLICATION_CONTENT_VALIDATOR_HAS_UI
                ayt::ui::UILayoutLoader loader;
                ayt::ui::Widget* widget = loader.loadFromFile(
                    resolvedPath.string());
                loader.stopHotReload();
                if (widget == nullptr) {
                    result.issues.push_back({logicalPath.string(),
                        "UI layout loader rejected the file."});
                } else {
                    ayt::ui::destroyWidgetTree(widget);
                }
#else
                result.issues.push_back({logicalPath.string(),
                    "Full-client UI validation is unavailable in this headless build."});
#endif
            }
        } else if (endsWith(name, ".aytilemap.json")) {
            ++result.tilemaps;
            std::string error;
            if (!validateAuthoringTilemap(resolvedPath, error)) {
                result.issues.push_back({logicalPath.string(), error});
            }
        } else if (endsWith(name, ".aytilemap")) {
            ++result.tilemaps;
            ayt::resource::TilemapLoader loader;
            if (loader.load(resolvedPath.string()) == nullptr) {
                result.issues.push_back({logicalPath.string(),
                    "Runtime Tilemap loader rejected the cooked file."});
            }
        }
    }
    if (scanError) {
        result.issues.push_back({assets.string(),
            "Project scan failed: " + scanError.message()});
    }
    sortDependencies(result);
    return result;
    } catch (const std::exception& exception) {
        result.issues.push_back({projectRoot,
            std::string("Project validation failed: ") + exception.what()});
        return result;
    } catch (...) {
        result.issues.push_back({projectRoot,
            "Project validation failed with an unknown error."});
        return result;
    }
}

} // namespace ayt::app
