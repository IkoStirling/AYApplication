#include <AYApplication/GameFlowProgram.h>

#include <algorithm>
#include <set>
#include <utility>

namespace ayt::app
{
namespace
{

void appendDiagnostic(std::vector<GameFlowDiagnostic>* diagnostics,
                      std::string path,
                      std::string message)
{
    if (diagnostics == nullptr) return;
    diagnostics->push_back({GameFlowDiagnosticSeverity::Error,
        std::move(path), std::move(message)});
}

bool valueMatches(const GameFlowValue& value, GameFlowValueType type)
{
    switch (type) {
    case GameFlowValueType::Boolean:
        return std::holds_alternative<bool>(value.data);
    case GameFlowValueType::Integer:
        return std::holds_alternative<std::int64_t>(value.data);
    case GameFlowValueType::Number:
        return std::holds_alternative<std::int64_t>(value.data)
            || std::holds_alternative<double>(value.data);
    case GameFlowValueType::String:
        return std::holds_alternative<std::string>(value.data);
    }
    return false;
}

const GameFlowFieldDefinition* findField(
    const std::vector<GameFlowFieldDefinition>& fields,
    std::string_view id) noexcept
{
    const auto found = std::find_if(fields.begin(), fields.end(),
        [id](const GameFlowFieldDefinition& field) { return field.id == id; });
    return found == fields.end() ? nullptr : &*found;
}

bool validateControlPayload(
    GameFlowPayload& arguments,
    const std::vector<GameFlowFieldDefinition>& fields,
    const GameFlowIntentDefinition* sourceIntent,
    bool hasReservedSubflowId,
    std::string_view path,
    std::vector<GameFlowDiagnostic>* diagnostics)
{
    bool valid = true;
    for (const auto& [id, value] : arguments) {
        if (hasReservedSubflowId && id == kGameFlowSubflowIdArgument) continue;
        const auto* field = findField(fields, id);
        if (field == nullptr) {
            appendDiagnostic(diagnostics, std::string(path) + "." + id,
                "Unknown control payload field '" + id + "'.");
            valid = false;
        } else if (!valueMatches(value, field->type)) {
            appendDiagnostic(diagnostics, std::string(path) + "." + id,
                "Control payload field has the wrong type.");
            valid = false;
        }
    }

    for (const auto& field : fields) {
        if (arguments.contains(field.id)) continue;
        if (!std::holds_alternative<std::monostate>(field.defaultValue.data)) {
            arguments.emplace(field.id, field.defaultValue);
            continue;
        }
        const auto* source = sourceIntent == nullptr
            ? nullptr : findField(sourceIntent->payload, field.id);
        if (source != nullptr && source->type == field.type) continue;
        if (field.required) {
            appendDiagnostic(diagnostics,
                std::string(path) + "." + field.id,
                "Required control payload field cannot be supplied by the "
                "authored arguments or triggering intent.");
            valid = false;
        }
    }
    return valid;
}

std::string flowPath(std::string_view flowId,
                     std::size_t transitionIndex,
                     std::size_t actionIndex)
{
    return "$.flows['" + std::string(flowId) + "'].transitions["
        + std::to_string(transitionIndex) + "].actions["
        + std::to_string(actionIndex) + "]";
}

class ProgramBuilder
{
public:
    ProgramBuilder(const GameFlowActionRegistry& valueRegistry,
                   GameFlowDocumentResolver valueResolver,
                   GameFlowProgramBuildOptions valueOptions,
                   std::vector<GameFlowDiagnostic>* valueDiagnostics)
        : registry(valueRegistry),
          resolver(std::move(valueResolver)),
          options(valueOptions),
          diagnostics(valueDiagnostics)
    {
    }

    bool add(GameFlowDocument document, std::size_t depth)
    {
        if (depth > options.maxCallDepth) {
            appendDiagnostic(diagnostics, "$.flows",
                "GameFlow subflow graph exceeds maxCallDepth "
                    + std::to_string(options.maxCallDepth) + ".");
            return false;
        }
        if (document.id.empty()) {
            appendDiagnostic(diagnostics, "$.id",
                "Resolved GameFlow document has an empty id.");
            return false;
        }
        if (visiting.contains(document.id)) {
            appendDiagnostic(diagnostics, "$.flows['" + document.id + "']",
                "GameFlow subflow dependency contains a cycle.");
            return false;
        }
        if (program.plans.contains(document.id)) return true;
        if (program.plans.size() >= options.maxFlows) {
            appendDiagnostic(diagnostics, "$.flows",
                "GameFlow program exceeds maxFlows "
                    + std::to_string(options.maxFlows) + ".");
            return false;
        }
        for (const auto& field : document.entryParameters) {
            if (field.id == kGameFlowSubflowIdArgument) {
                appendDiagnostic(diagnostics,
                    "$.flows['" + document.id + "'].entryParameters",
                    "Entry parameter id 'subflowId' is reserved.");
                return false;
            }
        }

        GameFlowPlan plan;
        std::vector<GameFlowDiagnostic> localDiagnostics;
        if (!buildGameFlowPlan(document, registry, plan, &localDiagnostics)) {
            if (diagnostics != nullptr) {
                for (auto& diagnostic : localDiagnostics) {
                    diagnostic.path = "$.flows['" + document.id + "']"
                        + (diagnostic.path.starts_with("$")
                            ? diagnostic.path.substr(1) : "." + diagnostic.path);
                    diagnostics->push_back(std::move(diagnostic));
                }
            }
            return false;
        }
        const std::string flowId = document.id;
        program.plans.emplace(flowId, std::move(plan));
        visiting.insert(flowId);

        auto& owner = program.plans.at(flowId);
        for (std::size_t transitionIndex = 0;
             transitionIndex < owner.document.transitions.size();
             ++transitionIndex) {
            auto& transition = owner.document.transitions[transitionIndex];
            const auto intentFound = owner.intentIndices.find(
                transition.triggerIntent);
            const GameFlowIntentDefinition* sourceIntent =
                intentFound == owner.intentIndices.end() ? nullptr
                    : &owner.document.intents[intentFound->second];
            for (std::size_t actionIndex = 0;
                 actionIndex < transition.actions.size(); ++actionIndex) {
                auto& action = transition.actions[actionIndex];
                const std::string path = flowPath(
                    flowId, transitionIndex, actionIndex);
                if (action.action == kGameFlowActionReturn) {
                    if (flowId == program.rootFlowId) {
                        appendDiagnostic(diagnostics, path + ".id",
                            "The root GameFlow cannot execute flow.return.");
                        visiting.erase(flowId);
                        return false;
                    }
                    if (actionIndex + 1u != transition.actions.size()) {
                        appendDiagnostic(diagnostics, path + ".id",
                            "flow.return must be the final transition action.");
                        visiting.erase(flowId);
                        return false;
                    }
                    if (!validateControlPayload(action.arguments,
                            owner.document.result, sourceIntent, false,
                            path + ".arguments", diagnostics)) {
                        visiting.erase(flowId);
                        return false;
                    }
                    continue;
                }
                if (action.action != kGameFlowActionEnter) continue;

                const auto idArgument = action.arguments.find(
                    std::string(kGameFlowSubflowIdArgument));
                const auto* subflowId = idArgument == action.arguments.end()
                    ? nullptr
                    : std::get_if<std::string>(&idArgument->second.data);
                if (subflowId == nullptr || subflowId->empty()) {
                    appendDiagnostic(diagnostics,
                        path + ".arguments.subflowId",
                        "flow.enter requires a non-empty string subflowId.");
                    visiting.erase(flowId);
                    return false;
                }

                if (!program.plans.contains(*subflowId)) {
                    if (!resolver) {
                        appendDiagnostic(diagnostics,
                            path + ".arguments.subflowId",
                            "No GameFlow resolver is available for subflow '"
                                + *subflowId + "'.");
                        visiting.erase(flowId);
                        return false;
                    }
                    GameFlowDocument resolved;
                    std::string error;
                    if (!resolver(*subflowId, resolved, error)) {
                        appendDiagnostic(diagnostics,
                            path + ".arguments.subflowId",
                            error.empty()
                                ? "Could not resolve subflow '" + *subflowId + "'."
                                : std::move(error));
                        visiting.erase(flowId);
                        return false;
                    }
                    if (resolved.id != *subflowId) {
                        appendDiagnostic(diagnostics,
                            path + ".arguments.subflowId",
                            "Resolved subflow id '" + resolved.id
                                + "' does not match requested id '"
                                + *subflowId + "'.");
                        visiting.erase(flowId);
                        return false;
                    }
                    if (!add(std::move(resolved), depth + 1u)) {
                        visiting.erase(flowId);
                        return false;
                    }
                } else if (visiting.contains(*subflowId)) {
                    appendDiagnostic(diagnostics,
                        path + ".arguments.subflowId",
                        "GameFlow subflow dependency contains a cycle through '"
                            + *subflowId + "'.");
                    visiting.erase(flowId);
                    return false;
                }

                const auto* child = program.findPlan(*subflowId);
                if (child == nullptr
                    || !validateControlPayload(action.arguments,
                        child->document.entryParameters, sourceIntent, true,
                        path + ".arguments", diagnostics)) {
                    visiting.erase(flowId);
                    return false;
                }
            }
        }

        visiting.erase(flowId);
        return true;
    }

    GameFlowProgram program;

private:
    const GameFlowActionRegistry& registry;
    GameFlowDocumentResolver resolver;
    GameFlowProgramBuildOptions options;
    std::vector<GameFlowDiagnostic>* diagnostics = nullptr;
    std::set<std::string, std::less<>> visiting;
};

} // namespace

bool isGameFlowControlAction(std::string_view actionId) noexcept
{
    return actionId == kGameFlowActionEnter
        || actionId == kGameFlowActionReturn;
}

const GameFlowPlan* GameFlowProgram::findPlan(
    std::string_view flowId) const noexcept
{
    const auto found = plans.find(flowId);
    return found == plans.end() ? nullptr : &found->second;
}

bool buildGameFlowProgram(
    const GameFlowDocument& root,
    const GameFlowActionRegistry& registry,
    GameFlowDocumentResolver resolver,
    GameFlowProgram& program,
    std::vector<GameFlowDiagnostic>* diagnostics,
    GameFlowProgramBuildOptions options)
{
    if (diagnostics != nullptr) diagnostics->clear();
    if (options.maxCallDepth == 0u || options.maxFlows == 0u) {
        appendDiagnostic(diagnostics, "$.options",
            "GameFlow maxCallDepth and maxFlows must be positive.");
        return false;
    }
    ProgramBuilder builder(
        registry, std::move(resolver), options, diagnostics);
    builder.program.rootFlowId = root.id;
    builder.program.maxCallDepth = options.maxCallDepth;
    if (!builder.add(root, 1u)) return false;
    program = std::move(builder.program);
    return true;
}

} // namespace ayt::app
