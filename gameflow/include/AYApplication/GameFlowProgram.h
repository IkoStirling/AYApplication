#pragma once

#include <AYApplication/GameFlowCoordinator.h>

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::app
{

inline constexpr std::string_view kGameFlowActionEnter = "flow.enter";
inline constexpr std::string_view kGameFlowActionReturn = "flow.return";
inline constexpr std::string_view kGameFlowSubflowIdArgument = "subflowId";
inline constexpr std::string_view kGameFlowDefaultStartupIntent = "app.start";

[[nodiscard]] bool isGameFlowControlAction(
    std::string_view actionId) noexcept;

using GameFlowDocumentResolver = std::function<bool(
    std::string_view flowId,
    GameFlowDocument& document,
    std::string& error)>;

struct GameFlowProgramBuildOptions
{
    std::size_t maxCallDepth = 16u;
    std::size_t maxFlows = 256u;
};

// An immutable set of normalized flow plans. References between plans use
// stable document ids; resolving files or project assets remains a host job.
struct GameFlowProgram
{
    std::string rootFlowId;
    std::map<std::string, GameFlowPlan, std::less<>> plans;
    std::size_t maxCallDepth = 16u;

    [[nodiscard]] const GameFlowPlan* findPlan(
        std::string_view flowId) const noexcept;
};

// Recursively resolves every flow.enter reference and validates the complete
// call graph. The destination is changed only after the full graph succeeds.
[[nodiscard]] bool buildGameFlowProgram(
    const GameFlowDocument& root,
    const GameFlowActionRegistry& registry,
    GameFlowDocumentResolver resolver,
    GameFlowProgram& program,
    std::vector<GameFlowDiagnostic>* diagnostics = nullptr,
    GameFlowProgramBuildOptions options = {});

} // namespace ayt::app
