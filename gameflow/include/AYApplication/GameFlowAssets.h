#pragma once

#include <AYApplication/GameFlowProgram.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

struct GameFlowAssetIssue
{
    std::string path;
    std::string message;
};

struct GameFlowAssetDocument
{
    // Portable path relative to the scanned asset root.
    std::string assetPath;
    std::string absolutePath;
    GameFlowDocument document;
};

// A deterministic, read-only index used by both runtime composition and
// project validation. Invalid documents remain in issues and are not exposed
// through the resolver; duplicate stable ids are rejected as ambiguous.
struct GameFlowAssetCatalog
{
    std::string assetRoot;
    std::size_t scannedFiles = 0;
    std::vector<GameFlowAssetDocument> documents;
    std::vector<GameFlowAssetIssue> issues;

    [[nodiscard]] const GameFlowAssetDocument* findByAssetPath(
        std::string_view path) const;
    [[nodiscard]] bool resolve(
        std::string_view flowId,
        GameFlowDocument& document,
        std::string& error) const;
};

// Returns false only when the asset root itself cannot be scanned. Individual
// malformed files are reported in catalog.issues so callers can decide whether
// unrelated authoring drafts should block runtime startup.
[[nodiscard]] bool scanGameFlowAssets(
    const std::string& assetRoot,
    GameFlowAssetCatalog& catalog,
    std::string* error = nullptr);

[[nodiscard]] GameFlowDocumentResolver makeGameFlowAssetResolver(
    std::shared_ptr<const GameFlowAssetCatalog> catalog);

} // namespace ayt::app
