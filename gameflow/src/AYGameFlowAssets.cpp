#include <AYApplication/GameFlowAssets.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>
#include <utility>

namespace ayt::app
{
namespace
{
namespace fs = std::filesystem;

bool endsWithInsensitive(std::string_view value, std::string_view suffix)
{
    if (value.size() < suffix.size()) return false;
    const std::size_t offset = value.size() - suffix.size();
    for (std::size_t index = 0; index < suffix.size(); ++index) {
        const auto left = static_cast<unsigned char>(value[offset + index]);
        const auto right = static_cast<unsigned char>(suffix[index]);
        if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
}

std::string portablePath(const fs::path& path)
{
    return path.lexically_normal().generic_string();
}

bool pathComponentEqual(const fs::path& left, const fs::path& right)
{
#ifdef _WIN32
    const std::string leftText = left.string();
    const std::string rightText = right.string();
    return leftText.size() == rightText.size()
        && std::equal(leftText.begin(), leftText.end(), rightText.begin(),
            [](char lhs, char rhs) {
                return std::tolower(static_cast<unsigned char>(lhs))
                    == std::tolower(static_cast<unsigned char>(rhs));
            });
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

struct ScannedFile
{
    fs::path logicalPath;
    fs::path resolvedPath;
    std::string assetPath;
};

std::string diagnosticText(const GameFlowDiagnostic& diagnostic)
{
    return diagnostic.path.empty()
        ? diagnostic.message
        : diagnostic.path + ": " + diagnostic.message;
}

} // namespace

const GameFlowAssetDocument* GameFlowAssetCatalog::findByAssetPath(
    std::string_view path) const
{
    const std::string normalized = portablePath(fs::path(path));
    const auto found = std::find_if(documents.begin(), documents.end(),
        [&](const GameFlowAssetDocument& value) {
            return value.assetPath == normalized;
        });
    return found == documents.end() ? nullptr : &*found;
}

bool GameFlowAssetCatalog::resolve(
    std::string_view flowId,
    GameFlowDocument& document,
    std::string& error) const
{
    const GameFlowAssetDocument* match = nullptr;
    for (const auto& candidate : documents) {
        if (candidate.document.id != flowId) continue;
        if (match != nullptr) {
            error = "GameFlow id '" + std::string(flowId)
                + "' is ambiguous under the project asset root.";
            return false;
        }
        match = &candidate;
    }
    if (match == nullptr) {
        error = "GameFlow id '" + std::string(flowId)
            + "' was not found under the project asset root.";
        return false;
    }
    document = match->document;
    error.clear();
    return true;
}

bool scanGameFlowAssets(
    const std::string& assetRoot,
    GameFlowAssetCatalog& catalog,
    std::string* error)
{
    const auto reject = [error](std::string message) {
        if (error != nullptr) *error = std::move(message);
        return false;
    };

    try {
    GameFlowAssetCatalog candidate;
    std::error_code filesystemError;
    const fs::path absoluteRoot = fs::absolute(
        fs::path(assetRoot), filesystemError);
    if (filesystemError) {
        return reject("GameFlow asset root cannot be resolved: "
            + filesystemError.message());
    }
    const fs::path root = fs::weakly_canonical(
        absoluteRoot, filesystemError);
    if (filesystemError) {
        return reject("GameFlow asset root cannot be resolved: "
            + filesystemError.message());
    }
    if (!fs::is_directory(root, filesystemError)) {
        return reject(filesystemError
            ? "GameFlow asset root cannot be inspected: "
                + filesystemError.message()
            : "GameFlow asset root is missing: " + root.string());
    }
    candidate.assetRoot = root.string();

    std::vector<ScannedFile> files;
    for (fs::recursive_directory_iterator it(root,
             fs::directory_options::none, filesystemError),
             end;
         !filesystemError && it != end; it.increment(filesystemError)) {
        const fs::path logicalPath = it->path().lexically_normal();
        if (!endsWithInsensitive(
                logicalPath.filename().string(), ".gameflow.json")) {
            continue;
        }

        std::error_code entryError;
        const fs::path resolvedPath = fs::weakly_canonical(
            logicalPath, entryError);
        if (entryError) {
            candidate.issues.push_back({logicalPath.string(),
                "GameFlow asset path cannot be resolved: "
                    + entryError.message()});
            continue;
        }
        if (!isWithin(root, resolvedPath)) {
            candidate.issues.push_back({logicalPath.string(),
                "GameFlow asset resolves outside the scanned asset root."});
            continue;
        }
        if (!fs::is_regular_file(resolvedPath, entryError)) {
            if (entryError) {
                candidate.issues.push_back({logicalPath.string(),
                    "GameFlow asset cannot be inspected: "
                        + entryError.message()});
            }
            continue;
        }

        const fs::path relative = logicalPath.lexically_relative(root);
        const std::string assetPath = portablePath(relative);
        if (assetPath.empty() || relative.is_absolute()
            || (!relative.empty() && *relative.begin() == "..")) {
            candidate.issues.push_back({logicalPath.string(),
                "GameFlow asset path is not relative to the scanned root."});
            continue;
        }
        files.push_back({logicalPath, resolvedPath, assetPath});
    }
    if (filesystemError) {
        return reject("GameFlow asset scan failed: "
            + filesystemError.message());
    }
    std::sort(files.begin(), files.end(), [](const ScannedFile& left,
                                             const ScannedFile& right) {
        return left.assetPath < right.assetPath;
    });
    candidate.scannedFiles = files.size();

    for (const ScannedFile& file : files) {
        try {
            std::ifstream input(file.resolvedPath, std::ios::binary);
            if (!input) {
                candidate.issues.push_back({file.logicalPath.string(),
                    "GameFlow asset could not be opened."});
                continue;
            }
            const std::string text{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            GameFlowDocument document;
            std::vector<GameFlowDiagnostic> diagnostics;
            if (!GameFlowSerializer::deserialize(
                    text, document, &diagnostics)) {
                if (diagnostics.empty()) {
                    candidate.issues.push_back({file.logicalPath.string(),
                        "GameFlow serializer rejected the asset."});
                } else {
                    for (const auto& diagnostic : diagnostics) {
                        candidate.issues.push_back({file.logicalPath.string(),
                            diagnosticText(diagnostic)});
                    }
                }
                continue;
            }
            candidate.documents.push_back({file.assetPath,
                file.resolvedPath.string(), std::move(document)});
        } catch (const std::exception& exception) {
            candidate.issues.push_back({file.logicalPath.string(),
                std::string("GameFlow asset could not be inspected: ")
                    + exception.what()});
        } catch (...) {
            candidate.issues.push_back({file.logicalPath.string(),
                "GameFlow asset could not be inspected."});
        }
    }

    std::map<std::string, std::vector<const GameFlowAssetDocument*>,
             std::less<>> byId;
    for (const auto& document : candidate.documents) {
        byId[document.document.id].push_back(&document);
    }
    for (const auto& [id, matches] : byId) {
        if (matches.size() < 2u) continue;
        for (const auto* match : matches) {
            candidate.issues.push_back({match->absolutePath,
                "Duplicate GameFlow id '" + id + "'."});
        }
    }

    catalog = std::move(candidate);
    if (error != nullptr) error->clear();
    return true;
    } catch (const std::exception& exception) {
        return reject(std::string("GameFlow asset scan failed: ")
            + exception.what());
    } catch (...) {
        return reject("GameFlow asset scan failed.");
    }
}

GameFlowDocumentResolver makeGameFlowAssetResolver(
    std::shared_ptr<const GameFlowAssetCatalog> catalog)
{
    return [catalog = std::move(catalog)](
               std::string_view flowId,
               GameFlowDocument& document,
               std::string& error) {
        if (!catalog) {
            error = "GameFlow asset catalog is unavailable.";
            return false;
        }
        return catalog->resolve(flowId, document, error);
    };
}

} // namespace ayt::app
