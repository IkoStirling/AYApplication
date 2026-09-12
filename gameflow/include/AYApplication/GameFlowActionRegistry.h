#pragma once

#include <AYApplication/GameFlowDocument.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

using GameFlowGeneration = std::uint64_t;
using GameFlowActionExecutionId = std::uint64_t;

struct GameFlowActionTypeDefinition
{
    std::string id;
    std::vector<GameFlowFieldDefinition> arguments;
    bool asynchronous = false;
};

struct GameFlowGuardTypeDefinition
{
    std::string id;
    std::vector<GameFlowFieldDefinition> arguments;
};

struct GameFlowActionInvocation
{
    GameFlowGeneration generation = 0;
    GameFlowActionExecutionId executionId = 0;
    std::string_view flowId;
    std::string_view transitionId;
    std::string_view intent;
    const GameFlowPayload* intentPayload = nullptr;
    const GameFlowPayload* arguments = nullptr;
    // Parameters supplied when the current subflow was entered. Empty for a
    // root flow. The result pointers describe the most recently returned
    // child while the parent transition resumes after flow.enter.
    const GameFlowPayload* flowParameters = nullptr;
    const GameFlowPayload* lastSubflowResult = nullptr;
    std::string_view returnedFlowId;
};

enum class GameFlowActionState : std::uint8_t
{
    Succeeded,
    Pending,
    Failed,
    Cancelled,
};

using GameFlowActionCancellationHandler = std::function<void()>;

struct GameFlowActionResult
{
    GameFlowActionState state = GameFlowActionState::Succeeded;
    std::string message;
    GameFlowActionCancellationHandler onCancel;

    static GameFlowActionResult succeeded();
    static GameFlowActionResult pending(
        GameFlowActionCancellationHandler onCancel = {});
    static GameFlowActionResult failed(std::string message);
    static GameFlowActionResult cancelled(std::string message = {});
};

struct GameFlowGuardInvocation
{
    std::string_view flowId;
    std::string_view transitionId;
    std::string_view intent;
    const GameFlowPayload* intentPayload = nullptr;
    const GameFlowPayload* arguments = nullptr;
    const GameFlowPayload* flowParameters = nullptr;
    const GameFlowPayload* lastSubflowResult = nullptr;
    std::string_view returnedFlowId;
};

using GameFlowActionHandler =
    std::function<GameFlowActionResult(const GameFlowActionInvocation&)>;
using GameFlowGuardHandler =
    std::function<bool(const GameFlowGuardInvocation&)>;

// The same definitions feed runtime validation and future editor authoring.
// Runtime handlers are intentionally host-owned and replaceable during setup.
class GameFlowActionRegistry
{
public:
    GameFlowActionRegistry();
    ~GameFlowActionRegistry();

    GameFlowActionRegistry(const GameFlowActionRegistry&) = delete;
    GameFlowActionRegistry& operator=(const GameFlowActionRegistry&) = delete;
    GameFlowActionRegistry(GameFlowActionRegistry&&) noexcept;
    GameFlowActionRegistry& operator=(GameFlowActionRegistry&&) noexcept;

    bool registerActionType(
        GameFlowActionTypeDefinition definition,
        bool replace = false,
        std::string* error = nullptr);
    bool registerGuardType(
        GameFlowGuardTypeDefinition definition,
        bool replace = false,
        std::string* error = nullptr);
    bool setActionHandler(
        std::string_view id,
        GameFlowActionHandler handler,
        std::string* error = nullptr);
    bool setGuardHandler(
        std::string_view id,
        GameFlowGuardHandler handler,
        std::string* error = nullptr);
    bool clearActionHandler(std::string_view id) noexcept;
    bool clearGuardHandler(std::string_view id) noexcept;

    // Convenience registration for runtime composition roots.
    bool registerAction(
        GameFlowActionTypeDefinition definition,
        GameFlowActionHandler handler,
        bool replace = false,
        std::string* error = nullptr);
    bool registerGuard(
        GameFlowGuardTypeDefinition definition,
        GameFlowGuardHandler handler,
        bool replace = false,
        std::string* error = nullptr);
    bool unregisterAction(std::string_view id);
    bool unregisterGuard(std::string_view id);
    void clear() noexcept;

    [[nodiscard]] const GameFlowActionTypeDefinition* findAction(
        std::string_view id) const noexcept;
    [[nodiscard]] const GameFlowGuardTypeDefinition* findGuard(
        std::string_view id) const noexcept;
    [[nodiscard]] const GameFlowActionHandler* findActionHandler(
        std::string_view id) const noexcept;
    [[nodiscard]] const GameFlowGuardHandler* findGuardHandler(
        std::string_view id) const noexcept;
    [[nodiscard]] std::vector<GameFlowActionTypeDefinition> actionTypes() const;
    [[nodiscard]] std::vector<GameFlowGuardTypeDefinition> guardTypes() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace ayt::app
