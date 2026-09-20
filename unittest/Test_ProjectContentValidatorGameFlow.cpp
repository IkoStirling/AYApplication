#include <AYApplication/GameFlowActionRegistry.h>
#include <AYApplication/ProjectContentValidator.h>
#include <AYTest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace
{

namespace fs = std::filesystem;
using namespace ayt::app;

class TestProject
{
public:
    explicit TestProject(std::string_view name)
        : root(fs::path(AY_APPLICATION_CONTENT_VALIDATOR_OUTPUT_ROOT)
              / std::string(name))
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
        fs::create_directories(root / "Assets", ignored);
        CHECK(!ignored);
    }

    ~TestProject()
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    void write(std::string_view relative, std::string_view contents) const
    {
        const fs::path path = root / fs::path(relative);
        std::error_code ignored;
        fs::create_directories(path.parent_path(), ignored);
        CHECK(!ignored);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        CHECK(static_cast<bool>(output));
        output.write(contents.data(),
            static_cast<std::streamsize>(contents.size()));
        CHECK(static_cast<bool>(output));
    }

    fs::path root;
};

bool hasIssue(const ProjectContentValidationResult& result,
              std::string_view text)
{
    return std::any_of(result.issues.begin(), result.issues.end(),
        [text](const ProjectContentValidationIssue& issue) {
            return issue.message.find(text) != std::string::npos;
        });
}

bool hasDependency(const ProjectContentValidationResult& result,
                   ProjectContentDependencyKind kind,
                   std::string_view target)
{
    return std::any_of(result.gameFlowDependencies.begin(),
        result.gameFlowDependencies.end(),
        [kind, target](const ProjectContentDependency& dependency) {
            return dependency.kind == kind && dependency.target == target;
        });
}

bool hasDependency(const ProjectContentValidationResult& result,
                   ProjectContentDependencyKind kind,
                   std::string_view source,
                   std::string_view target)
{
    return std::any_of(result.gameFlowDependencies.begin(),
        result.gameFlowDependencies.end(),
        [kind, source, target](const ProjectContentDependency& dependency) {
            return dependency.kind == kind && dependency.source == source
                && dependency.target == target;
        });
}

constexpr std::string_view kProject = R"json({
  "schemaVersion": 1,
  "paths": { "assets": "Assets" },
  "startupFlow": "flows/root.gameflow.json",
  "worlds": [
    { "id": "LevelOne", "scene": "worlds/level-one.content" }
  ],
  "ui": { "flow": "ui/main.uiflow.json" },
  "gameFlow": { "contract": "gameflow.contract.json" }
})json";

constexpr std::string_view kRootFlow = R"json({
  "schemaVersion": 2,
  "id": "root",
  "initialState": "idle",
  "entryParameters": [],
  "result": [],
  "extensions": {},
  "intents": [{ "id": "app.start" }],
  "states": [{ "id": "idle" }, { "id": "done" }, { "id": "failed" }],
  "transitions": [{
    "id": "start-child",
    "from": "idle",
    "intent": "app.start",
    "to": "done",
    "actions": [{
      "id": "flow.enter",
      "arguments": { "subflowId": "child" }
    }],
    "onFailure": "failed"
  }]
})json";

constexpr std::string_view kChildFlow = R"json({
  "schemaVersion": 2,
  "id": "child",
  "initialState": "idle",
  "entryParameters": [],
  "result": [],
  "extensions": {},
  "intents": [{
    "id": "activate",
    "payload": [{ "id": "message", "type": "string", "required": true }]
  }],
  "states": [{ "id": "idle" }, { "id": "done" }, { "id": "failed" }],
  "transitions": [{
    "id": "activate-content",
    "from": "idle",
    "intent": "activate",
    "to": "done",
    "actions": [
      { "id": "world.replace", "arguments": { "worldId": "LevelOne" } },
      { "id": "ui.flow.start" },
      { "id": "ui.context.activate", "arguments": {
          "activationId": "menu", "contextId": "Menu"
      } },
      { "id": "ui.signal.emit", "arguments": { "signalId": "notice" } },
      { "id": "game.asset.load", "arguments": {
          "path": "data/config.json"
      } }
    ],
    "onFailure": "failed",
    "onCancel": "failed"
  }]
})json";

constexpr std::string_view kContract = R"json({
  "schemaVersion": 1,
  "actions": [{
    "id": "game.asset.load",
    "arguments": [{ "id": "path", "type": "string", "required": true }],
    "references": [{ "argument": "path", "kind": "asset" }]
  }],
  "guards": []
})json";

constexpr std::string_view kUiFlow = R"json({
  "schemaVersion": 1,
  "id": "main-ui",
  "defaultEntry": "main",
  "entries": [{ "id": "main", "contexts": ["Menu"] }],
  "contexts": [{ "id": "Menu", "priority": 0, "slots": [] }],
  "signals": [{
    "id": "notice",
    "payload": [{ "id": "message", "type": "string", "required": true }]
  }]
})json";

void writeValidProject(TestProject& project)
{
    project.write("project.ayproject.json", kProject);
    project.write("Assets/flows/root.gameflow.json", kRootFlow);
    project.write("Assets/flows/child.gameflow.json", kChildFlow);
    project.write("Assets/gameflow.contract.json", kContract);
    project.write("Assets/ui/main.uiflow.json", kUiFlow);
    project.write("Assets/worlds/level-one.content", "world");
    project.write("Assets/data/config.json", "{}");
}

} // namespace

TEST_SUITE(ProjectContentValidatorGameFlowTests)

TEST_CASE(project_content_validator_source_abi_is_explicit)
{
    CHECK(kProjectContentValidatorSourceAbiVersion == 2u);
}

TEST_CASE(startup_closure_resolves_subflows_and_typed_content_references)
{
    TestProject project("valid-closure");
    writeValidProject(project);
    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK(static_cast<bool>(result));
    CHECK(result.gameFlows == 2u);
    CHECK(hasDependency(result, ProjectContentDependencyKind::GameFlow,
        "project/startupFlow", "flows/root.gameflow.json"));
    CHECK(hasDependency(result, ProjectContentDependencyKind::GameFlow,
        "root::start-child::flow.enter.subflowId",
        "flows/child.gameflow.json"));
    CHECK(std::count_if(result.gameFlowDependencies.begin(),
        result.gameFlowDependencies.end(),
        [](const ProjectContentDependency& dependency) {
            return dependency.kind
                == ProjectContentDependencyKind::GameFlow;
        }) == 2u);
    CHECK(hasDependency(result, ProjectContentDependencyKind::World,
        "LevelOne"));
    CHECK(hasDependency(result, ProjectContentDependencyKind::UIFlowEntry,
        "main"));
    CHECK(hasDependency(result, ProjectContentDependencyKind::UIContext,
        "Menu"));
    CHECK(hasDependency(result, ProjectContentDependencyKind::UISignal,
        "notice"));
    CHECK(hasDependency(result, ProjectContentDependencyKind::Asset,
        "data/config.json"));
}

TEST_CASE(project_descriptor_can_enable_ui_action_contracts_for_headless_validation)
{
    TestProject project("descriptor-ui-actions");
    writeValidProject(project);
    std::string descriptor(kProject);
    const std::string expected =
        "\"gameFlow\": { \"contract\": \"gameflow.contract.json\" }";
    const auto position = descriptor.find(expected);
    CHECK(position != std::string::npos);
    descriptor.replace(position, expected.size(),
        "\"gameFlow\": { \"uiActions\": true, "
        "\"contract\": \"gameflow.contract.json\" }");
    project.write("project.ayproject.json", descriptor);
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK(static_cast<bool>(result));
    CHECK(hasDependency(result, ProjectContentDependencyKind::UIContext,
        "Menu"));
}

TEST_CASE(layout_application_commands_are_checked_against_gameflow_intents)
{
    TestProject project("layout-application-command");
    writeValidProject(project);
    project.write("Assets/ui/main.uiflow.json", R"json({
      "schemaVersion": 1,
      "id": "main-ui",
      "defaultEntry": "main",
      "layers": [{ "id": "main", "order": 0 }],
      "slots": [{ "id": "main.content", "layer": "main" }],
      "screens": [{
        "id": "menu", "layout": "menu.ui.json", "layer": "main",
        "slot": "main.content", "scope": "application"
      }],
      "contexts": [{
        "id": "Menu", "priority": 0,
        "slots": [{
          "slot": "main.content", "operation": "present", "screen": "menu"
        }]
      }],
      "entries": [{ "id": "main", "contexts": ["Menu"] }],
      "signals": [{
        "id": "notice",
        "payload": [{ "id": "message", "type": "string", "required": true }]
      }]
    })json");
    project.write("Assets/ui/menu.ui.json", R"json({
      "type": "Button", "id": "start",
      "events": { "onClick": "app.start" }
    })json");

    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, options);
    CHECK(static_cast<bool>(result));

    project.write("Assets/ui/menu.ui.json", R"json({
      "type": "Button", "id": "start",
      "events": { "onClick": "app.strat" }
    })json");
    result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "application command 'app.strat'"));
    CHECK(hasIssue(result, "unknown GameFlow Intent"));
}

TEST_CASE(project_descriptor_rejects_non_boolean_ui_action_setting)
{
    TestProject project("descriptor-ui-actions-type");
    writeValidProject(project);
    std::string descriptor(kProject);
    const std::string expected =
        "\"gameFlow\": { \"contract\": \"gameflow.contract.json\" }";
    const auto position = descriptor.find(expected);
    CHECK(position != std::string::npos);
    descriptor.replace(position, expected.size(),
        "\"gameFlow\": { \"uiActions\": \"yes\", "
        "\"contract\": \"gameflow.contract.json\" }");
    project.write("project.ayproject.json", descriptor);

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result,
        "Project gameFlow.uiActions must be a boolean"));
}

TEST_CASE(startup_flow_requires_the_default_startup_intent)
{
    TestProject project("missing-startup-intent");
    writeValidProject(project);
    std::string rootFlow(kRootFlow);
    const std::string expected = "app.start";
    const auto intent = rootFlow.find(expected);
    CHECK(intent != std::string::npos);
    rootFlow.replace(intent, expected.size(), "manual.start");
    const auto transition = rootFlow.find(expected);
    CHECK(transition != std::string::npos);
    rootFlow.replace(transition, expected.size(), "manual.start");
    project.write("Assets/flows/root.gameflow.json", rootFlow);

    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "Startup GameFlow intent 'app.start'"));
    CHECK(hasIssue(result, "is not declared by flow 'root'"));
}

TEST_CASE(startup_flow_rejects_required_root_parameters)
{
    TestProject project("required-root-parameter");
    writeValidProject(project);
    std::string rootFlow(kRootFlow);
    const std::string expected = "\"entryParameters\": []";
    const auto entryParameters = rootFlow.find(expected);
    CHECK(entryParameters != std::string::npos);
    rootFlow.replace(entryParameters, expected.size(), R"json(
  "entryParameters": [{
    "id": "profileId", "type": "string", "required": true
  }])json");
    project.write("Assets/flows/root.gameflow.json", rootFlow);

    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "Root flow entry"));
    CHECK(hasIssue(result, "profileId"));
}

TEST_CASE(startup_flow_rejects_required_startup_intent_payload)
{
    TestProject project("required-startup-payload");
    writeValidProject(project);
    std::string rootFlow(kRootFlow);
    const std::string expected = "{ \"id\": \"app.start\" }";
    const auto intent = rootFlow.find(expected);
    CHECK(intent != std::string::npos);
    rootFlow.replace(intent, expected.size(), R"json({
    "id": "app.start",
    "payload": [{ "id": "slot", "type": "string", "required": true }]
  })json");
    project.write("Assets/flows/root.gameflow.json", rootFlow);

    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "Startup GameFlow intent 'app.start'"));
    CHECK(hasIssue(result, "slot"));
}

TEST_CASE(project_content_rejects_non_portable_authored_paths)
{
    TestProject project("non-portable-path");
    writeValidProject(project);
    project.write("project.ayproject.json", R"json({
      "schemaVersion": 1,
      "paths": { "assets": "Assets" },
      "startupFlow": "flows\\root.gameflow.json"
    })json");

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "portable forward-slash separators"));

    constexpr std::string_view drivePaths[] = {
        "C:/outside.gameflow.json",
        "C:outside.gameflow.json",
    };
    std::size_t index = 0;
    for (const std::string_view startupFlow : drivePaths) {
        TestProject driveProject("drive-path-" + std::to_string(index++));
        writeValidProject(driveProject);
        driveProject.write("project.ayproject.json",
            std::string(R"json({
              "schemaVersion": 1,
              "paths": { "assets": "Assets" },
              "startupFlow": ")json") + std::string(startupFlow)
                + R"json("
            })json");
        const auto driveResult = validateProjectContent(
            driveProject.root.string(),
            ProjectContentValidationProfile::Headless);
        CHECK_FALSE(static_cast<bool>(driveResult));
        CHECK(hasIssue(driveResult,
            "Path must be relative and stay inside the project"));
    }
}

TEST_CASE(all_drafts_are_parsed_and_duplicate_ids_are_reported)
{
    TestProject project("draft-index");
    project.write("Assets/flows/a.gameflow.json", kRootFlow);
    project.write("Assets/flows/b.gameflow.json", kRootFlow);
    project.write("Assets/flows/broken.gameflow.json", "{");
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK(result.gameFlows == 3u);
    CHECK(hasIssue(result, "Duplicate GameFlow id 'root'"));
    CHECK(hasIssue(result, "GameFlow JSON"));
}

TEST_CASE(startup_closure_rejects_unknown_action_contracts)
{
    TestProject project("strict-closure");
    project.write("project.ayproject.json", R"json({
      "paths": { "assets": "Assets" },
      "startupFlow": "root.gameflow.json"
    })json");
    project.write("Assets/root.gameflow.json", R"json({
      "schemaVersion": 2,
      "id": "strict-root",
      "initialState": "idle",
      "entryParameters": [], "result": [], "extensions": {},
      "intents": [{ "id": "go" }],
      "states": [{ "id": "idle" }, { "id": "done" }],
      "transitions": [{
        "id": "go", "from": "idle", "intent": "go", "to": "done",
        "actions": [
          { "id": "game.unknown" },
          { "id": "flow.enter", "arguments": { "subflowId": "missing" } }
        ]
      }]
    })json");
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "game.unknown' is not registered"));
}

TEST_CASE(startup_closure_reports_a_missing_subflow_without_other_errors)
{
    TestProject project("missing-subflow");
    project.write("project.ayproject.json", R"json({
      "paths": { "assets": "Assets" },
      "startupFlow": "root.gameflow.json"
    })json");
    project.write("Assets/root.gameflow.json", R"json({
      "schemaVersion": 2,
      "id": "missing-child-root",
      "initialState": "idle",
      "entryParameters": [], "result": [], "extensions": {},
      "intents": [{ "id": "go" }],
      "states": [{ "id": "idle" }, { "id": "done" }],
      "transitions": [{
        "id": "go", "from": "idle", "intent": "go", "to": "done",
        "actions": [{
          "id": "flow.enter", "arguments": { "subflowId": "missing" }
        }]
      }]
    })json");

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result,
        "GameFlow id 'missing' was not found under the project asset root"));
}

TEST_CASE(headless_ui_reference_catalog_rejects_malformed_signal_fields)
{
    TestProject emptyIdProject("ui-signal-empty-field");
    writeValidProject(emptyIdProject);
    emptyIdProject.write("Assets/ui/main.uiflow.json", R"json({
      "schemaVersion": 1,
      "id": "main-ui",
      "defaultEntry": "main",
      "entries": [{ "id": "main" }],
      "contexts": [{ "id": "Menu" }],
      "signals": [{
        "id": "notice",
        "payload": [{ "id": "", "type": "string", "required": true }]
      }]
    })json");
    ProjectContentValidationOptions emptyIdOptions;
    emptyIdOptions.enableGameFlowUIActions = true;
    const auto emptyIdResult = validateProjectContent(
        emptyIdProject.root.string(),
        ProjectContentValidationProfile::Headless,
        std::move(emptyIdOptions));
    CHECK_FALSE(static_cast<bool>(emptyIdResult));
    CHECK(hasIssue(emptyIdResult, "non-empty id and a type string")
        || hasIssue(emptyIdResult, "ID must not be empty")
        || hasIssue(emptyIdResult, "Required string must not be empty"));

    TestProject invalidDefaultProject("ui-signal-invalid-default");
    writeValidProject(invalidDefaultProject);
    invalidDefaultProject.write("Assets/ui/main.uiflow.json", R"json({
      "schemaVersion": 1,
      "id": "main-ui",
      "defaultEntry": "main",
      "entries": [{ "id": "main" }],
      "contexts": [{ "id": "Menu" }],
      "signals": [{
        "id": "notice",
        "payload": [{
          "id": "message", "type": "integer", "required": true,
          "default": "not-an-integer"
        }]
      }]
    })json");
    ProjectContentValidationOptions invalidDefaultOptions;
    invalidDefaultOptions.enableGameFlowUIActions = true;
    const auto invalidDefaultResult = validateProjectContent(
        invalidDefaultProject.root.string(),
        ProjectContentValidationProfile::Headless,
        std::move(invalidDefaultOptions));
    CHECK_FALSE(static_cast<bool>(invalidDefaultResult));
    CHECK(hasIssue(invalidDefaultResult, "does not match its type")
        || hasIssue(invalidDefaultResult,
            "Default value does not match field type"));
}

TEST_CASE(headless_ui_reference_catalog_requires_an_exact_schema_version)
{
    const std::array<std::string_view, 3> invalidVersions = {
        "true", "1.5", "4294967297"};
    std::size_t index = 0;
    for (const std::string_view version : invalidVersions) {
        TestProject project("ui-schema-" + std::to_string(index++));
        writeValidProject(project);
        std::string uiFlow(kUiFlow);
        const std::string expected = "\"schemaVersion\": 1";
        const auto position = uiFlow.find(expected);
        CHECK(position != std::string::npos);
        uiFlow.replace(position, expected.size(),
            "\"schemaVersion\": " + std::string(version));
        project.write("Assets/ui/main.uiflow.json", uiFlow);

        ProjectContentValidationOptions options;
        options.enableGameFlowUIActions = true;
        const auto result = validateProjectContent(project.root.string(),
            ProjectContentValidationProfile::Headless,
            std::move(options));
        CHECK_FALSE(static_cast<bool>(result));
        CHECK(hasIssue(result, "schemaVersion"));
    }
}

TEST_CASE(gameflow_asset_scan_rejects_a_symlink_outside_the_asset_root)
{
    TestProject project("symlink-escape");
    project.write("outside.gameflow.json", R"json({
      "schemaVersion": 2,
      "id": "outside",
      "initialState": "idle",
      "entryParameters": [], "result": [], "extensions": {},
      "intents": [], "states": [{ "id": "idle" }], "transitions": []
    })json");
    std::error_code linkError;
    fs::create_symlink(project.root / "outside.gameflow.json",
        project.root / "Assets/linked.gameflow.json", linkError);
    if (linkError) return;

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result,
        "GameFlow asset resolves outside the scanned asset root"));
}

TEST_CASE(project_asset_scan_rejects_a_symlink_outside_the_asset_root)
{
    TestProject project("content-symlink-escape");
    project.write("outside.ui.json", R"json({
      "type": "Panel"
    })json");
    std::error_code linkError;
    fs::create_symlink(project.root / "outside.ui.json",
        project.root / "Assets/linked.ui.json", linkError);
    if (linkError) return;

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result,
        "Project asset resolves outside the project asset root"));
}

TEST_CASE(project_descriptor_cannot_be_a_symlink_outside_the_project_root)
{
    TestProject project("descriptor-symlink-escape");
    TestProject outside("descriptor-symlink-target");
    outside.write("project.ayproject.json", "{}");
    std::error_code linkError;
    fs::create_symlink(outside.root / "project.ayproject.json",
        project.root / "project.ayproject.json", linkError);
    if (linkError) return;

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "Path escapes the project content root"));
}

#if AY_APPLICATION_CONTENT_VALIDATOR_HAS_UI
TEST_CASE(full_client_profile_constructs_real_ui_layouts)
{
    TestProject project("full-client-layout");
    project.write("Assets/ui/main.ui.json", R"json({
      "type": "Panel",
      "id": "root",
      "size": { "w": 640, "h": 480 },
      "children": [{
        "type": "TextLabel",
        "id": "title",
        "text": "GameFlow",
        "size": { "w": 160, "h": 24 }
      }]
    })json");

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::FullClient);
    CHECK(static_cast<bool>(result));
    CHECK(result.uiLayouts == 1u);
}
#endif

TEST_CASE(contract_defaults_reject_unsigned_integers_outside_int64_range)
{
    TestProject project("contract-integer-range");
    writeValidProject(project);
    project.write("Assets/gameflow.contract.json", R"json({
      "schemaVersion": 1,
      "actions": [{
        "id": "game.asset.load",
        "arguments": [{
          "id": "path",
          "type": "integer",
          "required": false,
          "default": 18446744073709551615
        }],
        "references": []
      }],
      "guards": []
    })json");

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "does not match its type"));
}

TEST_CASE(contract_manifest_must_be_a_regular_file)
{
    TestProject project("contract-regular-file");
    writeValidProject(project);
    std::error_code filesystemError;
    fs::remove(project.root / "Assets/gameflow.contract.json",
        filesystemError);
    CHECK(!filesystemError);
    fs::create_directory(project.root / "Assets/gameflow.contract.json",
        filesystemError);
    CHECK(!filesystemError);

    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless);
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "regular file"));
}

TEST_CASE(contract_references_cannot_escape_or_name_unknown_worlds)
{
    TestProject project("reference-errors");
    writeValidProject(project);
    std::string invalidChild(kChildFlow);
    const auto asset = invalidChild.find("data/config.json");
    CHECK(asset != std::string::npos);
    invalidChild.replace(asset, std::string("data/config.json").size(),
                         "../outside.json");
    const auto world = invalidChild.find("LevelOne");
    CHECK(world != std::string::npos);
    invalidChild.replace(world, std::string("LevelOne").size(), "MissingWorld");
    project.write("Assets/flows/child.gameflow.json", invalidChild);

    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "unknown World 'MissingWorld'"));
    CHECK(hasIssue(result, "Path must be relative and stay inside the project"));
}

TEST_CASE(standard_ui_action_value_domains_match_the_live_bridge)
{
    TestProject project("standard-action-semantics");
    writeValidProject(project);
    std::string invalidChild(kChildFlow);
    const std::string original =
        "\"activationId\": \"menu\", \"contextId\": \"Menu\"";
    const auto position = invalidChild.find(original);
    CHECK(position != std::string::npos);
    invalidChild.replace(position, original.size(),
        original + ", \"scope\": \"planet\"");
    project.write("Assets/flows/child.gameflow.json", invalidChild);

    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "ui.context.activate has an invalid scope"));
}

TEST_CASE(ui_flow_start_allows_no_entry_when_the_document_has_no_default)
{
    TestProject project("ui-start-without-entry");
    writeValidProject(project);
    std::string uiFlow(kUiFlow);
    const std::string expected = "\"defaultEntry\": \"main\"";
    const auto position = uiFlow.find(expected);
    CHECK(position != std::string::npos);
    uiFlow.replace(position, expected.size(), "\"defaultEntry\": \"\"");
    project.write("Assets/ui/main.uiflow.json", uiFlow);

    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK(static_cast<bool>(result));
    CHECK_FALSE(hasDependency(
        result, ProjectContentDependencyKind::UIFlowEntry, ""));
}

TEST_CASE(project_registry_cannot_replace_engine_owned_contracts)
{
    TestProject project("standard-contract-override");
    writeValidProject(project);
    ProjectContentValidationOptions options;
    options.enableGameFlowUIActions = true;
    options.configureGameFlow = [](GameFlowActionRegistry& registry,
                                   std::string& error) {
        return registry.registerActionType(
            {"ui.flow.start", {}, false}, true, &error);
    };
    const auto result = validateProjectContent(project.root.string(),
        ProjectContentValidationProfile::Headless, std::move(options));
    CHECK_FALSE(static_cast<bool>(result));
    CHECK(hasIssue(result, "incompatible with the standard UI contract"));
}

TEST_SUITE_END
