#pragma once

#include <AYApplication/GameFlowCoordinator.h>
#include <AYGameLoop/IGameLoop.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

class IEngineHost;

inline constexpr std::string_view kGameFlowRuntimeModuleId =
    "AYApplication.GameFlowRuntime";
inline constexpr std::string_view kGameFlowRuntimeSubSystemName =
    "GameFlowRuntime";

using ConfigureGameFlowRegistry =
    std::function<bool(GameFlowActionRegistry&, std::string&)>;

struct GameFlowRuntimeConfig
{
    std::string documentPath;
    ConfigureGameFlowRegistry configureRegistry;
    bool enableWorldActions = true;
    std::string startupIntent = "app.start";
};

// Immutable startup product built before the Application creates platform
// services. It owns the registry and normalized plan used by the runtime.
class GameFlowRuntimePreparation final
{
public:
    GameFlowRuntimePreparation();
    ~GameFlowRuntimePreparation();
    GameFlowRuntimePreparation(GameFlowRuntimePreparation&&) noexcept;
    GameFlowRuntimePreparation& operator=(
        GameFlowRuntimePreparation&&) noexcept;

    GameFlowRuntimePreparation(const GameFlowRuntimePreparation&) = delete;
    GameFlowRuntimePreparation& operator=(
        const GameFlowRuntimePreparation&) = delete;

    [[nodiscard]] std::string_view documentPath() const noexcept;
    [[nodiscard]] std::string_view startupIntent() const noexcept;
    [[nodiscard]] bool worldActionsEnabled() const noexcept;
    [[nodiscard]] const GameFlowDocument& document() const noexcept;
    [[nodiscard]] const std::vector<GameFlowDiagnostic>& diagnostics()
        const noexcept;

private:
    friend std::unique_ptr<GameFlowRuntimePreparation>
        prepareGameFlowRuntime(GameFlowRuntimeConfig, std::string*);
    friend class GameFlowRuntime;

    class Impl;
    std::unique_ptr<Impl> _impl;
};

// Parses, validates, and normalizes a startup Flow without creating a window
// or touching Host services. A null result always includes an actionable
// message in error when one is supplied.
[[nodiscard]] std::unique_ptr<GameFlowRuntimePreparation>
prepareGameFlowRuntime(
    GameFlowRuntimeConfig config,
    std::string* error = nullptr);

// Process-scoped runtime owned by the GameLoop. It keeps the normalized plan
// and coordinator alive while transient World instances are replaced.
class GameFlowRuntime final : public ayt::game::ISubSystem
{
public:
    GameFlowRuntime(IEngineHost& host, GameFlowRuntimeConfig config);
    GameFlowRuntime(
        IEngineHost& host,
        std::unique_ptr<GameFlowRuntimePreparation> preparation);
    ~GameFlowRuntime() override;

    GameFlowRuntime(const GameFlowRuntime&) = delete;
    GameFlowRuntime& operator=(const GameFlowRuntime&) = delete;

    const char* getName() const override;
    const ayt::game::SubSystemDescriptor& getDescriptor() const override;
    bool initialize() override;
    void update(float deltaTime) override;
    void fixedUpdate(float fixedDeltaTime) override;
    void shutdown() override;

    GameFlowRequestResult request(
        std::string_view intent,
        GameFlowPayload payload = {});

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] std::string_view currentState() const noexcept;
    [[nodiscard]] std::string_view lastError() const noexcept;
    [[nodiscard]] std::string_view documentPath() const noexcept;
    [[nodiscard]] const std::vector<GameFlowDiagnostic>& diagnostics()
        const noexcept;
    [[nodiscard]] const GameFlowDocument* document() const noexcept;
    [[nodiscard]] GameFlowCoordinator& coordinator() noexcept;
    [[nodiscard]] const GameFlowCoordinator& coordinator() const noexcept;
    [[nodiscard]] const GameFlowActionRegistry* registry() const noexcept;

    // Runtime bridges may bind handlers only for types already accepted by
    // preflight. The normalized contract therefore cannot drift at runtime.
    bool bindActionHandler(
        std::string_view actionId,
        GameFlowActionHandler handler,
        std::string* error = nullptr);
    bool unbindActionHandler(std::string_view actionId) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

class GameFlowRuntimeModule;

[[nodiscard]] GameFlowRuntime* gameFlowRuntime(IEngineHost& host) noexcept;

} // namespace ayt::app
