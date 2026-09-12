#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ayt::app
{

inline constexpr std::uint32_t kGameFlowSchemaVersion = 2u;

enum class GameFlowValueType : std::uint8_t
{
    Boolean,
    Integer,
    Number,
    String,
};

struct GameFlowValue
{
    using Array = std::vector<GameFlowValue>;
    using Object = std::map<std::string, GameFlowValue, std::less<>>;
    using Storage = std::variant<
        std::monostate, bool, std::int64_t, double, std::string, Array, Object>;

    GameFlowValue() = default;
    GameFlowValue(std::monostate value) : data(value) {}
    GameFlowValue(bool value) : data(value) {}
    GameFlowValue(std::int64_t value) : data(value) {}
    GameFlowValue(double value) : data(value) {}
    GameFlowValue(std::string value) : data(std::move(value)) {}
    GameFlowValue(const char* value) : data(std::string(value)) {}
    GameFlowValue(Array value) : data(std::move(value)) {}
    GameFlowValue(Object value) : data(std::move(value)) {}

    Storage data;

    friend bool operator==(const GameFlowValue&, const GameFlowValue&) = default;
};

using GameFlowPayload =
    std::map<std::string, GameFlowValue, std::less<>>;

struct GameFlowFieldDefinition
{
    std::string id;
    GameFlowValueType type = GameFlowValueType::String;
    bool required = false;
    GameFlowValue defaultValue;
};

struct GameFlowIntentDefinition
{
    std::string id;
    std::vector<GameFlowFieldDefinition> payload;
};

struct GameFlowActionCall
{
    std::string action;
    GameFlowPayload arguments;
};

struct GameFlowGuardCall
{
    std::string guard;
    GameFlowPayload arguments;
};

// States remain flat for stable ids. parent and initialChild describe an
// optional hierarchy without coupling the asset to editor graph nesting.
struct GameFlowStateDefinition
{
    std::string id;
    std::string parent;
    std::string initialChild;
};

struct GameFlowTransitionDefinition
{
    std::string id;
    std::string fromState;
    std::string triggerIntent;
    std::string toState;
    GameFlowGuardCall guard;
    std::vector<GameFlowActionCall> actions;
    std::string onFailureState;
    std::string onCancelState;
    double timeoutSeconds = 0.0;
    std::int32_t priority = 0;
};

struct GameFlowDocument
{
    std::uint32_t schemaVersion = kGameFlowSchemaVersion;
    std::string id;
    std::string initialState;
    std::vector<GameFlowIntentDefinition> intents;
    std::vector<GameFlowStateDefinition> states;
    std::vector<GameFlowTransitionDefinition> transitions;
    // Kept at the tail so existing aggregate initialization remains source
    // compatible. Phase 6 subflows use these schemas for call input/result.
    std::vector<GameFlowFieldDefinition> entryParameters;
    std::vector<GameFlowFieldDefinition> result;
    // Engine and project extensions must live under this explicit namespace;
    // arbitrary unknown root keys are not part of the preservation contract.
    GameFlowValue::Object extensions;

    [[nodiscard]] const GameFlowIntentDefinition* findIntent(
        std::string_view id) const noexcept;
    [[nodiscard]] const GameFlowStateDefinition* findState(
        std::string_view id) const noexcept;
};

enum class GameFlowDiagnosticSeverity : std::uint8_t
{
    Warning,
    Error,
};

struct GameFlowDiagnostic
{
    GameFlowDiagnosticSeverity severity = GameFlowDiagnosticSeverity::Error;
    std::string path;
    std::string message;
};

class GameFlowActionRegistry;
struct GameFlowMigrationReport;

// Structural validation is shared by serialization, runtime normalization,
// headless tooling, and the future editor. Supplying a registry additionally
// validates every referenced guard/action and its authored arguments.
[[nodiscard]] bool validateGameFlow(
    const GameFlowDocument& document,
    const GameFlowActionRegistry* registry = nullptr,
    std::vector<GameFlowDiagnostic>* diagnostics = nullptr);

class GameFlowSerializer
{
public:
    static bool deserialize(
        std::string_view jsonText,
        GameFlowDocument& document,
        std::vector<GameFlowDiagnostic>* diagnostics = nullptr);
    static bool deserialize(
        std::string_view jsonText,
        GameFlowDocument& document,
        std::vector<GameFlowDiagnostic>* diagnostics,
        GameFlowMigrationReport* migrationReport);
    static bool serialize(
        const GameFlowDocument& document,
        std::string& jsonText,
        std::vector<GameFlowDiagnostic>* diagnostics = nullptr,
        bool pretty = true);
};

[[nodiscard]] const char* gameFlowValueTypeName(GameFlowValueType value) noexcept;

} // namespace ayt::app
