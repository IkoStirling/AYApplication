#include <AYApplication/ProjectContentValidator.h>

#include <AYResource/Loader/TilemapLoader.h>
#include <AYScene.h>
#include <nlohmann/json.hpp>

#if AY_APPLICATION_HAS_DEVICE
#include <AYUI/LayoutLoader.h>
#include <AYUI/Widget.h>
#endif

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace ayt::app {
namespace {

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

bool loadJson(const std::filesystem::path& path, nlohmann::json& document,
              std::string& error)
{
    try {
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

bool validateHeadlessUi(const std::filesystem::path& path, std::string& error)
{
    nlohmann::json document;
    if (!loadJson(path, document, error)) return false;
    const nlohmann::json* root = &document;
    if (const auto wrapped = document.find("root"); wrapped != document.end()) {
        root = &*wrapped;
    }
    return validateWidgetJson(*root, error);
}

bool validateAuthoringTilemap(const std::filesystem::path& path,
                              std::string& error)
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
    const std::uint64_t cells = document["cols"].get<std::uint64_t>()
        * document["rows"].get<std::uint64_t>();
    if (cells > 4u * 1024u * 1024u) {
        error = "Tilemap exceeds the supported cell count.";
        return false;
    }
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

} // namespace

ProjectContentValidationResult validateProjectContent(
    const std::string& projectRoot,
    ProjectContentValidationProfile profile)
{
    ProjectContentValidationResult result;
    result.profile = profile;
    const std::filesystem::path root = std::filesystem::absolute(projectRoot)
        .lexically_normal();
    const std::filesystem::path descriptorPath = root / "project.ayproject.json";
    nlohmann::json descriptor;
    if (std::filesystem::is_regular_file(descriptorPath)) {
        std::string descriptorError;
        if (!loadJson(descriptorPath, descriptor, descriptorError)) {
            result.issues.push_back({descriptorPath.string(), descriptorError});
            return result;
        }
    }
    std::string assetRoot = "Assets";
    if (descriptor.is_object()) {
        if (const auto paths = descriptor.find("paths");
            paths != descriptor.end() && paths->is_object()) {
            assetRoot = paths->value("assets", assetRoot);
        } else {
            assetRoot = descriptor.value("assetRoot", assetRoot);
        }
    }
    const std::filesystem::path assets = root / assetRoot;
    if (!std::filesystem::is_directory(assets)) {
        result.issues.push_back({assets.string(), "Project asset root is missing."});
        return result;
    }
    if (const auto worlds = descriptor.find("worlds");
        descriptor.is_object() && worlds != descriptor.end()
        && worlds->is_array()) {
        for (const auto& world : *worlds) {
            if (!world.is_object()) continue;
            const auto require = [&](const std::string& relative,
                                     const char* role) {
                if (!relative.empty() && !std::filesystem::is_regular_file(
                        assets / relative)) {
                    result.issues.push_back({(assets / relative).string(),
                        std::string("Project World references a missing ")
                            + role + "."});
                }
            };
            require(world.value("scene", std::string{}), "Scene");
            require(world.value("ui", std::string{}), "UI layout");
            if (const auto maps = world.find("tilemaps"); maps != world.end()
                && maps->is_array()) {
                for (const auto& map : *maps) {
                    if (map.is_string()) require(map.get<std::string>(), "Tilemap");
                }
            }
        }
    }

    std::error_code scanError;
    for (std::filesystem::recursive_directory_iterator it(assets,
             std::filesystem::directory_options::skip_permission_denied,
             scanError), end;
         !scanError && it != end; it.increment(scanError)) {
        if (!it->is_regular_file(scanError)) continue;
        const std::string name = lower(it->path().filename().string());
        if (endsWith(name, ".ayscene")) {
            ++result.scenes;
            ayt::scene::Scene scene(ayt::scene::SceneMode::Edit, "validation");
            ayt::serializer::SerializeError error;
            if (!scene.load(it->path().string(), &error)) {
                result.issues.push_back({it->path().string(), error.message});
            }
        } else if (endsWith(name, ".ui.json")) {
            ++result.uiLayouts;
            std::string error;
            if (!validateHeadlessUi(it->path(), error)) {
                result.issues.push_back({it->path().string(), error});
                continue;
            }
            if (profile == ProjectContentValidationProfile::FullClient) {
#if AY_APPLICATION_HAS_DEVICE
                ayt::ui::UILayoutLoader loader;
                ayt::ui::Widget* widget = loader.loadFromFile(it->path().string());
                loader.stopHotReload();
                if (widget == nullptr) {
                    result.issues.push_back({it->path().string(),
                        "UI layout loader rejected the file."});
                } else {
                    ayt::ui::destroyWidgetTree(widget);
                }
#else
                result.issues.push_back({it->path().string(),
                    "Full-client UI validation is unavailable in this headless build."});
#endif
            }
        } else if (endsWith(name, ".aytilemap.json")) {
            ++result.tilemaps;
            std::string error;
            if (!validateAuthoringTilemap(it->path(), error)) {
                result.issues.push_back({it->path().string(), error});
            }
        } else if (endsWith(name, ".aytilemap")) {
            ++result.tilemaps;
            ayt::resource::TilemapLoader loader;
            if (loader.load(it->path().string()) == nullptr) {
                result.issues.push_back({it->path().string(),
                    "Runtime Tilemap loader rejected the cooked file."});
            }
        }
    }
    if (scanError) {
        result.issues.push_back({assets.string(),
            "Project scan failed: " + scanError.message()});
    }
    return result;
}

} // namespace ayt::app
