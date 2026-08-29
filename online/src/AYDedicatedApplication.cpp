#include <AYOnlineApplication/DedicatedApplication.h>

#include <AYScene.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace ayt::app::online
{
namespace
{

constexpr size_t kMaximumCatalogLines = 4096;
constexpr size_t kMaximumCatalogLineLength = 8192;

} // namespace

bool DedicatedSceneHostConfig::isValid() const {
    return contentResolver != nullptr && maximumWorlds != 0 &&
           std::isfinite(maximumDeltaSeconds) && maximumDeltaSeconds > 0.0f;
}

struct DedicatedSceneHost::Impl {
    struct WorldEntry {
        ::ayt::net::DedicatedAllocation allocation;
        OnlineContentResolveResult content;
        std::unique_ptr<::ayt::scene::Scene> scene;
        std::set<std::string> connectedPeers;
    };

    explicit Impl(DedicatedSceneHostConfig input)
        : config(std::move(input)) {}

    void fail(DedicatedSceneHostError error,
              ::ayt::net::DedicatedAllocationId allocationId,
              std::string message) {
        status.lastError = error;
        status.lastAllocationId = allocationId;
        status.message = std::move(message);
        refreshCounts();
    }

    void succeed(::ayt::net::DedicatedAllocationId allocationId) {
        status.lastError = DedicatedSceneHostError::None;
        status.lastAllocationId = allocationId;
        status.message.clear();
        refreshCounts();
    }

    void refreshCounts() {
        status.activeWorlds = worlds.size();
        status.connectedPlayers = 0;
        for (const auto& [id, world] : worlds) {
            (void)id;
            status.connectedPlayers += world.connectedPeers.size();
        }
    }

    void stop(WorldEntry& entry) {
        if (config.onPlayerDisconnected) {
            for (const auto& peer : entry.connectedPeers) {
                config.onPlayerDisconnected(
                    entry.allocation, *entry.scene,
                    ::ayt::net::PeerId{peer});
            }
        }
        entry.connectedPeers.clear();
        if (config.deactivateWorld) {
            config.deactivateWorld(entry.allocation, *entry.scene);
        }
    }

    DedicatedSceneHostConfig config;
    std::map<::ayt::net::DedicatedAllocationId, WorldEntry> worlds;
    DedicatedSceneHostStatus status;
};

DedicatedSceneHost::DedicatedSceneHost(DedicatedSceneHostConfig config)
    : _impl(std::make_unique<Impl>(std::move(config))) {
    if (!_impl->config.isValid()) {
        _impl->fail(DedicatedSceneHostError::InvalidConfiguration, 0,
                    "Dedicated scene host configuration is invalid");
    }
}

DedicatedSceneHost::~DedicatedSceneHost() {
    if (!_impl) return;
    std::vector<::ayt::net::DedicatedAllocationId> ids;
    ids.reserve(_impl->worlds.size());
    for (const auto& [id, world] : _impl->worlds) {
        (void)world;
        ids.push_back(id);
    }
    for (const auto id : ids) stopAuthoritativeWorld(id);
}

bool DedicatedSceneHost::startAuthoritativeWorld(
    const ::ayt::net::DedicatedAllocation& allocation) {
    if (!_impl || !_impl->config.isValid() || !allocation.isValid()) {
        if (_impl) {
            _impl->fail(DedicatedSceneHostError::InvalidConfiguration,
                        allocation.allocationId,
                        "Dedicated allocation or scene host configuration is invalid");
        }
        return false;
    }
    if (_impl->worlds.contains(allocation.allocationId)) {
        _impl->fail(DedicatedSceneHostError::DuplicateAllocation,
                    allocation.allocationId,
                    "Dedicated allocation already has an active world");
        return false;
    }
    if (_impl->worlds.size() >= _impl->config.maximumWorlds) {
        _impl->fail(DedicatedSceneHostError::CapacityReached,
                    allocation.allocationId,
                    "Dedicated scene world capacity has been reached");
        return false;
    }

    auto resolved = _impl->config.contentResolver->resolve(allocation.content);
    if (!resolved.isValid()) {
        _impl->fail(DedicatedSceneHostError::ContentUnavailable,
                    allocation.allocationId,
                    resolved.message.empty()
                        ? "Dedicated content is unavailable"
                        : std::move(resolved.message));
        return false;
    }

    const std::string sceneName = resolved.sceneName.empty()
        ? allocation.content.contentId : resolved.sceneName;
    auto scene = std::make_unique<::ayt::scene::Scene>(
        ::ayt::scene::SceneMode::Play, sceneName);
    ::ayt::serializer::SerializeError loadError;
    if (!scene->load(resolved.scenePath, &loadError)) {
        std::string message =
            "Failed to load Dedicated scene: " + resolved.scenePath;
        if (!loadError.message.empty()) message += ": " + loadError.message;
        if (!loadError.path.empty()) message += " at " + loadError.path;
        _impl->fail(DedicatedSceneHostError::SceneLoadFailed,
                    allocation.allocationId, std::move(message));
        return false;
    }

    if (_impl->config.prepareWorld) {
        std::string message;
        if (!_impl->config.prepareWorld(allocation, *scene, message)) {
            _impl->fail(DedicatedSceneHostError::WorldPreparationFailed,
                        allocation.allocationId,
                        message.empty()
                            ? "Dedicated authority world preparation failed"
                            : std::move(message));
            return false;
        }
    }

    Impl::WorldEntry entry;
    entry.allocation = allocation;
    entry.content = std::move(resolved);
    entry.scene = std::move(scene);
    _impl->worlds.emplace(allocation.allocationId, std::move(entry));
    _impl->succeed(allocation.allocationId);
    return true;
}

void DedicatedSceneHost::stopAuthoritativeWorld(
    ::ayt::net::DedicatedAllocationId allocationId) {
    if (!_impl) return;
    const auto found = _impl->worlds.find(allocationId);
    if (found == _impl->worlds.end()) return;
    _impl->stop(found->second);
    _impl->worlds.erase(found);
    _impl->succeed(allocationId);
}

void DedicatedSceneHost::playerConnected(
    ::ayt::net::DedicatedAllocationId allocationId,
    const ::ayt::net::PeerId& peerId,
    ::ayt::net::NetConnection* connection) {
    if (!_impl || !peerId.isValid()) return;
    const auto found = _impl->worlds.find(allocationId);
    if (found == _impl->worlds.end()) return;
    auto& entry = found->second;
    if (!entry.allocation.players.empty() &&
        std::find(entry.allocation.players.begin(),
                  entry.allocation.players.end(), peerId) ==
            entry.allocation.players.end()) {
        return;
    }
    if (entry.connectedPeers.size() >= entry.allocation.playerCount ||
        !entry.connectedPeers.emplace(peerId.value).second) {
        return;
    }
    if (_impl->config.onPlayerConnected) {
        _impl->config.onPlayerConnected(
            entry.allocation, *entry.scene, peerId, connection);
    }
    _impl->succeed(allocationId);
}

void DedicatedSceneHost::playerDisconnected(
    ::ayt::net::DedicatedAllocationId allocationId,
    const ::ayt::net::PeerId& peerId) {
    if (!_impl || !peerId.isValid()) return;
    const auto found = _impl->worlds.find(allocationId);
    if (found == _impl->worlds.end()) return;
    auto& entry = found->second;
    if (entry.connectedPeers.erase(peerId.value) == 0) return;
    if (_impl->config.onPlayerDisconnected) {
        _impl->config.onPlayerDisconnected(
            entry.allocation, *entry.scene, peerId);
    }
    _impl->succeed(allocationId);
}

void DedicatedSceneHost::tickAuthoritativeWorlds(float deltaTime) {
    if (!_impl || !_impl->config.isValid()) return;
    const float safeDelta = std::isfinite(deltaTime)
        ? std::clamp(deltaTime, 0.0f, _impl->config.maximumDeltaSeconds)
        : 0.0f;
    for (auto& [id, entry] : _impl->worlds) {
        (void)id;
        entry.scene->tick(safeDelta);
    }
}

DedicatedSceneHostStatus DedicatedSceneHost::getStatus() const {
    if (!_impl) return {};
    _impl->refreshCounts();
    return _impl->status;
}

::ayt::scene::Scene* DedicatedSceneHost::findScene(
    ::ayt::net::DedicatedAllocationId allocationId) {
    if (!_impl) return nullptr;
    const auto found = _impl->worlds.find(allocationId);
    return found == _impl->worlds.end() ? nullptr : found->second.scene.get();
}

const ::ayt::scene::Scene* DedicatedSceneHost::findScene(
    ::ayt::net::DedicatedAllocationId allocationId) const {
    if (!_impl) return nullptr;
    const auto found = _impl->worlds.find(allocationId);
    return found == _impl->worlds.end() ? nullptr : found->second.scene.get();
}

bool loadOnlineContentCatalog(const std::string& catalogPath,
                              OnlineContentCatalog& catalog,
                              std::string& message) {
    message.clear();
    if (catalogPath.empty()) {
        message = "Dedicated content catalog path is empty";
        return false;
    }
    std::ifstream input(catalogPath);
    if (!input) {
        message = "Unable to open Dedicated content catalog: " + catalogPath;
        return false;
    }

    std::error_code pathError;
    const auto absoluteCatalog = std::filesystem::absolute(
        std::filesystem::path{catalogPath}, pathError);
    if (pathError) {
        message = "Unable to resolve Dedicated content catalog path";
        return false;
    }
    const auto base = absoluteCatalog.parent_path();
    OnlineContentCatalog parsed;
    std::string line;
    size_t lineNumber = 0;
    size_t mappingCount = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        if (lineNumber > kMaximumCatalogLines ||
            line.size() > kMaximumCatalogLineLength) {
            message = "Dedicated content catalog exceeds its size limit";
            return false;
        }
        if (lineNumber == 1 && line.size() >= 3 &&
            static_cast<unsigned char>(line[0]) == 0xEF &&
            static_cast<unsigned char>(line[1]) == 0xBB &&
            static_cast<unsigned char>(line[2]) == 0xBF) {
            line.erase(0, 3);
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.front() == '#') continue;

        std::string fields[4];
        size_t begin = 0;
        bool malformed = false;
        for (size_t field = 0; field < 3; ++field) {
            const size_t tab = line.find('\t', begin);
            if (tab == std::string::npos) {
                malformed = true;
                break;
            }
            fields[field] = line.substr(begin, tab - begin);
            begin = tab + 1;
        }
        if (malformed || line.find('\t', begin) != std::string::npos) {
            message = "Malformed Dedicated content catalog line " +
                      std::to_string(lineNumber);
            return false;
        }
        fields[3] = line.substr(begin);

        std::filesystem::path scenePath{fields[2]};
        if (scenePath.is_relative()) scenePath = base / scenePath;
        scenePath = scenePath.lexically_normal();
        pathError.clear();
        if (!std::filesystem::is_regular_file(scenePath, pathError) ||
            pathError) {
            message = "Dedicated scene is missing at catalog line " +
                      std::to_string(lineNumber);
            return false;
        }
        OnlineContentMapping mapping{
            std::move(fields[0]), std::move(fields[1]), scenePath.string(),
            std::move(fields[3])};
        if (!parsed.addOrReplace(std::move(mapping))) {
            message = "Invalid Dedicated content mapping at line " +
                      std::to_string(lineNumber);
            return false;
        }
        ++mappingCount;
    }
    if (input.bad()) {
        message = "Failed while reading Dedicated content catalog";
        return false;
    }
    if (mappingCount == 0) {
        message = "Dedicated content catalog contains no mappings";
        return false;
    }
    catalog = std::move(parsed);
    return true;
}

} // namespace ayt::app::online
