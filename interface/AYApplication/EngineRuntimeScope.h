#pragma once

#include <memory>

namespace ayt::app
{

class IEngineHost;

/// Owns the process-wide bindings installed for one engine runtime.
///
/// The scope snapshots the host service table, SceneManager/active-World
/// selection, and process hooks before installing the built-in bindings. On
/// reset it ends any remaining Play scene and restores that complete snapshot.
/// Module shutdown must run first so subsystems can still resolve services
/// while they release their state.
class EngineRuntimeScope final
{
public:
    explicit EngineRuntimeScope(IEngineHost& host);
    ~EngineRuntimeScope();

    EngineRuntimeScope(const EngineRuntimeScope&) = delete;
    EngineRuntimeScope& operator=(const EngineRuntimeScope&) = delete;
    EngineRuntimeScope(EngineRuntimeScope&&) noexcept;
    EngineRuntimeScope& operator=(EngineRuntimeScope&&) noexcept;

    /// Re-publish services whose instances become available after initialize.
    void refresh();

    /// Idempotent teardown. Restores services, hooks, Scene selection and the
    /// active World captured by the constructor.
    void reset() noexcept;

    [[nodiscard]] bool active() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app
