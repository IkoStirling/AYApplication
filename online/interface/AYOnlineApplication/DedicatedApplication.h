#pragma once
// AYApplication-side bridge from Dedicated allocations to real AYScene worlds.

#include <AYNetwork/Session/DedicatedServerRuntime.h>
#include <AYOnlineApplication/OnlineContent.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ayt::scene { class Scene; }

namespace ayt::app::online
{

enum class DedicatedSceneHostError : uint8_t {
    None = 0,
    InvalidConfiguration,
    CapacityReached,
    DuplicateAllocation,
    ContentUnavailable,
    SceneLoadFailed,
    WorldPreparationFailed,
};

struct DedicatedSceneHostStatus {
    size_t activeWorlds = 0;
    size_t connectedPlayers = 0;
    DedicatedSceneHostError lastError = DedicatedSceneHostError::None;
    ::ayt::net::DedicatedAllocationId lastAllocationId = 0;
    std::string message;
};

struct DedicatedSceneHostConfig {
    std::shared_ptr<IOnlineContentResolver> contentResolver;

    // A Scene owns one independent World. Keep this at one for game stacks
    // that still resolve process-global World::instance(); projects whose
    // systems are world-local may opt into multiple allocations per process.
    size_t maximumWorlds = 1;
    float maximumDeltaSeconds = 0.25f;

    // Called after the scene file is loaded but before the allocation becomes
    // visible. Register authority-only systems and apply contentSeed here.
    std::function<bool(const ::ayt::net::DedicatedAllocation&,
                       ::ayt::scene::Scene&,
                       std::string&)> prepareWorld;
    std::function<void(const ::ayt::net::DedicatedAllocation&,
                       ::ayt::scene::Scene&)> deactivateWorld;
    std::function<void(const ::ayt::net::DedicatedAllocation&,
                       ::ayt::scene::Scene&,
                       const ::ayt::net::PeerId&,
                       ::ayt::net::NetConnection*)> onPlayerConnected;
    std::function<void(const ::ayt::net::DedicatedAllocation&,
                       ::ayt::scene::Scene&,
                       const ::ayt::net::PeerId&)> onPlayerDisconnected;

    bool isValid() const;
};

// Main-thread world host used by DedicatedServerRuntime. Backend content IDs
// are resolved through a trusted local catalog; no remote filesystem path is
// accepted. Each active allocation owns one Play Scene and its World.
class DedicatedSceneHost final : public ::ayt::net::IDedicatedWorldHost {
public:
    explicit DedicatedSceneHost(DedicatedSceneHostConfig config);
    ~DedicatedSceneHost() override;

    DedicatedSceneHost(const DedicatedSceneHost&) = delete;
    DedicatedSceneHost& operator=(const DedicatedSceneHost&) = delete;

    bool startAuthoritativeWorld(
        const ::ayt::net::DedicatedAllocation& allocation) override;
    void stopAuthoritativeWorld(
        ::ayt::net::DedicatedAllocationId allocationId) override;
    void playerConnected(::ayt::net::DedicatedAllocationId allocationId,
                         const ::ayt::net::PeerId& peerId,
                         ::ayt::net::NetConnection* connection) override;
    void playerDisconnected(::ayt::net::DedicatedAllocationId allocationId,
                            const ::ayt::net::PeerId& peerId) override;
    void tickAuthoritativeWorlds(float deltaTime) override;

    DedicatedSceneHostStatus getStatus() const;
    ::ayt::scene::Scene* findScene(
        ::ayt::net::DedicatedAllocationId allocationId);
    const ::ayt::scene::Scene* findScene(
        ::ayt::net::DedicatedAllocationId allocationId) const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

// Loads a trusted, local tab-separated catalog:
//   content-id<TAB>version<TAB>scene-path<TAB>scene-name
// Relative scene paths are resolved against the catalog directory. Blank
// lines and lines beginning with '#' are ignored.
bool loadOnlineContentCatalog(const std::string& catalogPath,
                              OnlineContentCatalog& catalog,
                              std::string& message);

} // namespace ayt::app::online
