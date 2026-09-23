# 独立游戏快速装配

`GameProject` 是独立游戏的 composition root。应用只描述自己需要的引擎能力、
游戏模块和 World 路由；窗口、渲染、音频、物理、Scene/World、主循环与关闭顺序
继续由 `AYApplication` 装配。独立游戏不需要经过 Editor。

## 一条命令创建项目基线

`project_init_tool` 调用 `AYProject` 的共享模板核心，一次生成本指南中的完整目录、
CMake、`GameProject`、项目清单、初始 Scene、GameFlow、UIFlow、主菜单/HUD 和构建
Profile。Editor 的“文件 → 新建项目…”窗口与 CLI 使用同一份生成规则；创建完成后
Editor 会直接打开新项目。

```powershell
project_init_tool --output D:\Games\MyGame --name "My Game" `
  --id my-game --profile client-3d
```

目标目录必须尚不存在；工具不会覆盖或拼接旧项目。可先加 `--dry-run` 查看确定性文件
清单。`client-2d` 与 `client-3d` 选择编译能力；两者都会生成 Headless/Full Client 内容
验证测试。生成的 preset 复用引擎 `out/build/vcpkg_installed`，仍需在终端设置同一
`VCPKG_ROOT`。

## 推荐目录

```text
MyGame/
├── CMakeLists.txt              # 选择 CLIENT_2D / CLIENT_3D / HEADLESS
├── CMakePresets.json           # 指向共享 vcpkg_installed
├── app/
│   └── MyGameApp.cpp           # 薄入口，只调用 runGameProject
├── game/
│   ├── MyGame.cpp              # 唯一运行时装配清单
│   └── MyGame.h
├── src/
│   ├── logic/                  # 与渲染、窗口无关的游戏规则
│   ├── data/                   # 游戏数据结构与加载
│   └── systems/                # 游戏 SubSystem / ECS system
├── Assets/
│   ├── flow/                   # 应用级 .gameflow.json
│   ├── ui/                     # .uiflow.json 与 Screen .ui.json
│   └── worlds/                 # .ayscene 文件
├── project.ayproject.json      # Editor/工具读取的项目、World 与运行清单
└── tests/                      # 游戏规则和装配验证
```

日常开发只需要记住三个入口：

- 改编译进来的引擎能力：根 `CMakeLists.txt` 的 `ay_add_engine_for_game`。
- 改窗口、启动 World、World 清单和游戏模块：`game/MyGame.cpp`。
- 写玩法：`src/logic`、`src/data`、`src/systems`；`app` 不放玩法。

## 构建期能力

在游戏根 CMake 中引入一次引擎 helper：

```cmake
include("${AY_ENGINE_SOURCE_DIR}/cmake/AYGameApplication.cmake")
ay_add_engine_for_game(
    SOURCE_DIR "${AY_ENGINE_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/_ay/engine"
    PROFILE CLIENT_2D
    DISABLE NETWORK AYVOXEL AYVIDEO
)
```

可选 profile 是 `CLIENT_2D`、`CLIENT_3D` 和 `HEADLESS`。`ENABLE` / `DISABLE`
用于少量项目差异，应用仓库不再逐个 `add_subdirectory` 引擎模块。

所有应用 preset 应把 `VCPKG_INSTALLED_DIR` 指向引擎统一安装目录，例如：

```json
"VCPKG_INSTALLED_DIR": "${sourceDir}/../AliyatEngine/out/build/vcpkg_installed"
```

程序和资产用同一个 helper 声明：

```cmake
ay_add_game_executable(
    TARGET MyGameApp
    SOURCES MyGameApp.cpp
    LIBRARIES my_game
    ASSET_DIR "${CMAKE_SOURCE_DIR}/Assets"
    ASSET_OUTPUT_DIR "Assets"
)
```

## 运行时装配

`game/MyGame.cpp` 返回一份 `GameProject`：

```cpp
ayt::app::GameProject makeGameProject()
{
    ayt::app::GameProject game;
    game.id = "my_game";
    game.displayName = "My Game";
    game.assetRoot = "Assets";
    game.startupFlow = "flow/application.gameflow.json";
    game.worlds = {
        {.id = "main_menu", .scenePath = "worlds/main_menu.ayscene"},
        {.id = "level_01", .scenePath = "worlds/level_01.ayscene"},
    };
    game.configureModules = &configureGameModules;
    return game;
}
```

`configureGameModules` 把游戏自己的 SubSystem 模块加入同一张依赖图。模块通过稳定
ID 声明依赖；引擎在启动前统一检查缺失依赖和依赖环，再按拓扑顺序安装和关闭。

`startupFlow` 是相对 `assetRoot` 的应用级流程入口。GameFlow 运行时由 GameLoop
持有，因此替换 World 时不会被销毁。启动文档应声明 `app.start` intent；运行时完成
初始化后会自动排队该 intent，首个 Ingress 更新再执行启动动作。当前项目描述符不提供
根流程参数或启动 intent 载荷，因此两者都必须能从空输入完成校验（必填字段需提供默认
值）。内容验证器会按这条真实启动路径执行预检。最小启动流程如下：

```json
{
  "schemaVersion": 2,
  "id": "application",
  "initialState": "boot",
  "entryParameters": [],
  "result": [],
  "extensions": {},
  "intents": [
    { "id": "app.start" }
  ],
  "states": [
    { "id": "boot" },
    { "id": "main_menu" },
    { "id": "startup_error" }
  ],
  "transitions": [
    {
      "id": "open_main_menu",
      "from": "boot",
      "intent": "app.start",
      "to": "main_menu",
      "actions": [
        { "id": "world.replace", "arguments": { "worldId": "main_menu" } }
      ],
      "onFailure": "startup_error"
    }
  ]
}
```

`world.replace` 由客户端装配自动注册，并且只接受 `worlds` 清单里的稳定 ID。游戏
自定义 action/guard 通过 `configureGameFlow` 注册；回调在流程文档归一化和校验前
执行。声明自定义类型时包含 `AYApplicationGameFlow.h`，不要让 JSON 直接依赖 C++
类名或 Scene 文件名。

```cpp
game.configureGameFlow = [](
    ayt::app::GameFlowActionRegistry& registry,
    std::string& error) {
    return registry.registerAction(
        {"save.load_slot",
         {{"slot", ayt::app::GameFlowValueType::Integer, true, {}}},
         false},
        &loadSlotAction,
        false,
        &error);
};
```

动作实现放在 `src/systems`，`game/MyGame.cpp` 只登记稳定 action ID、参数契约和
处理器。这样以后增加编辑器节点图时，可以直接从同一份注册表生成节点端口和参数
Inspector。

### Subflow 资产

大型流程使用 `flow.enter(subflowId, ...)` 调用另一个独立的
`*.gameflow.json`，使用 `flow.return(...)` 把 typed result 返回给调用者。`subflowId`
引用文档中的稳定 `id`，不引用文件名；运行时和内容验证器会在 `assetRoot` 下建立同一份
确定性目录，拒绝重复 ID、递归调用、超过配置上限的调用深度和不匹配的参数/返回值。

```json
{
  "id": "flow.enter",
  "arguments": {
    "subflowId": "new_game_setup",
    "difficulty": "normal"
  }
}
```

编辑器坐标保存在 `extensions` 命名空间内，不参与运行时语义和确定性程序指纹。移动节点
不会使 replay 失效；修改状态、transition、action、guard、默认值或 subflow 内容会改变
程序指纹，并在回放开始时立即拒绝不匹配的记录。

### 纯数据动作契约

CI 和 Editor 无法调用游戏可执行文件中的 `configureGameFlow` 时，可在资产目录放置
`gameflow.contract.json`，并在项目描述符的 `gameFlow.contract` 中登记。manifest 只描述
动作/guard 的 typed 参数和资源引用，不包含 C++ 处理器：

```json
{
  "schemaVersion": 1,
  "actions": [
    {
      "id": "inventory.load_table",
      "arguments": [
        { "id": "path", "type": "string", "required": true }
      ],
      "references": [
        { "argument": "path", "kind": "asset" }
      ]
    }
  ],
  "guards": []
}
```

reference kind 支持 `asset`、`world`、`ui-entry`、`ui-context` 和 `ui-signal`。
被标记参数必须是 string；验证器会拒绝绝对路径、`..`、符号链接越界、缺失文件和未知
稳定 ID。manifest 与运行时 C++ 注册必须保持相同的 ID、字段、默认值、异步标志和引用
metadata。

## 可选的 UIFlow 桥接

客户端可以链接 `AYApplicationGameFlowUI` 将 UIFlow 和 GameFlow 组装起来，并包含
`AYApplicationGameFlowUI.h`。项目先在 `configureModules` 中注册
`UIFlowRuntimeModule`，再调用 `enableGameFlowUIBridge`；该 helper 会保留已有的模块
配置回调，并在其后追加桥接模块。

```cpp
#include <AYApplicationGameFlowUI.h>

game.configureModules = [](ayt::app::EngineModuleRuntime& runtime) {
    auto uiDocument = loadApplicationUIFlow();       // 项目内加载并解析 .uiflow.json
    auto screenHost = makeApplicationScreenHost();   // 项目内绑定已初始化的 UIManager
    return runtime.modules().emplace<ayt::app::UIFlowRuntimeModule>(
        runtime.context().host(),
        std::move(uiDocument),
        std::move(screenHost));
};

ayt::app::enableGameFlowUIBridge(game);
```

`loadApplicationUIFlow` 和 `makeApplicationScreenHost` 是示例中的项目 helper：前者使用
`UIFlowSerializer` 读取项目的 `.uiflow.json`，后者通常返回绑定已初始化
`UIManager` 的 `UIManagerFlowScreenHost`。它们需要由客户端组装层实现。

UI 按钮不应直接打开 Scene。普通游戏流程按钮只需要在 Layout 中选择一个
GameFlow Intent 作为应用命令槽位；UI Designer 的 `On Click` 下拉框会列出项目内的
Intent。运行时若没有找到显式 Screen 事件映射，就把该 ID 直接交给 GameFlow：

`main_menu.ui.json` 中的按钮片段：

```json
{ "events": { "onClick": "menu.start" } }
```

此时 `.uiflow.json` 的 Screen 不需要重复声明 handler/Signal，应用组装层也不需要
`signalBindings`。Full Client 与 Headless 内容验证都会检查命名空间形式的命令 ID
（如 `menu.start`）是否对应已编译 GameFlow 程序中的 Intent。

显式 UIFlow Signal 仍用于 UI 内部状态、带类型 payload 的高级交互或第三方 UI 适配。
旧格式继续兼容：

```json
{
  "screens": [
    {
      "id": "main_menu",
      "events": [
        { "handler": "startGame", "signal": "ui.start_game" }
      ]
    }
  ],
  "signals": [
    { "id": "ui.start_game" }
  ]
}
```

`signalBindings` 明确指定 `signalId -> intentId`；桥接会在安装时同时校验 Signal、
intent 和 payload schema。因此重命 UI Signal 或 GameFlow intent 时会在启动阶段失败，
不会在按钮点击后静默路由到错误目标。直接按钮命令当前不携带动态 payload；
这类 Signal 的必填字段需要在 UIFlow 中提供默认值，或由游戏系统显式发出带参 Signal。

UIFlow 也可以通过 host action 主动请求 GameFlow intent。默认桥接会为
`gameflow.request` 安装处理器，但 UIFlow 仍必须在文档的 `actions` 中声明该
action 及其输入契约，否则 `invokeAction` 会拒绝调用：

```json
"actions": [
  {
    "id": "gameflow.request",
    "inputs": [
      { "id": "intent", "type": "string", "required": true }
    ]
  }
]
```

额外输入会按目标 intent 的 payload schema 过滤和校验；应同时在 action 的
`inputs` 中声明它们，以便 UIFlow 完成类型检查和节点端口生成。只需要 Signal 映射的项目可以
在桥接配置中设置 `enableRequestAction = false`，并省略该 action 声明。

GameFlow 通过四个稳定 action 控制 UIFlow：

| Action | 用途 |
|---|---|
| `ui.flow.start` | 从可选 `entry` 启动 UIFlow。 |
| `ui.context.activate` | 使用稳定 `activationId` 激活 `contextId`，可选指定 `scope` 和 `scopeKey`。 |
| `ui.context.deactivate` | 通过 `activationId` 撤销之前的 Context 激活。 |
| `ui.signal.emit` | 发出 `signalId`；数据来自触发当前 transition 的 intent payload，并按 UI Signal schema 过滤。 |

`ui.flow.start` 的空 `entry` 使用 UIFlow 的 `defaultEntry`；两者都为空时只启动持久
UIFlow runtime，不触发入口动作。这与运行时和内容验证器的语义一致。

这些 UI action 会在 GameFlow 启动文档预检前注册，但处理器只在 UIFlow 运行时
存在时安装。`AYApplicationGameFlowUI` 不在 headless 配置中生成；无窗口/服务器
项目仍只依赖 GameFlow 核心，不会间接引入 AYUI。

bridge 安装期间，单侧热重载必须继续满足当前另一侧的契约。若一次修改同时改变
GameFlow 和 UIFlow 的共享 intent/context/signal 契约，当前版本没有成对原子提交 API；
工具应先卸载 bridge，分别完成两侧 reload，再重新安装 bridge 进行整体验证。后续若
编辑器需要无中断地提交这类成对修改，再增加 bridge 级 prepare/commit 事务。

平台入口保持很薄：

```cpp
int main(int argc, char* argv[])
{
    return ayt::app::runGameProject(makeGameProject(), argc, argv);
}
```

## 跨 World 配置

`GameWorld::id` 是游戏代码和存档使用的稳定 ID，文件名可以调整。运行时通过 Host
取得路由服务：

```cpp
auto* host = ayt::app::currentEngineHost();
auto* worlds = host ? ayt::app::gameWorldRouter(*host) : nullptr;
if (worlds && !worlds->requestWorld("level_01")) {
    // worlds->lastError() 给出未知 ID、并发切换或加载失败原因。
}
```

切换请求在 GameLoop 的 Egress 阶段提交给 `RuntimeSceneLoader`。新 Scene 会先在
暂存 World 中加载并执行可选的 `prepareActivation`；全部成功后才替换当前 World，
失败时旧 World 继续运行。

World 保存本关的实体、组件和临时状态。跨 World 仍需保留的进度、存档、背包、
会话和叙事标记应由游戏 SubSystem/Host service 持有。这样切换 World 不会隐式
销毁全局进度，也不会让关卡对象泄漏到下一关。

新增关卡只需完成三件事：

1. 在 `Assets/worlds` 添加 `.ayscene`。
2. 在 `game/MyGame.cpp` 的 `worlds` 中登记稳定 ID 和相对路径。
3. 从游戏系统调用 `requestWorld("stable_id")`。

启动入口按以下顺序选择：

1. 客户端 `-scene <path>`：直接启动指定 Scene，绕过 GameFlow，适合关卡调试。
2. `-flow <path>`：覆盖项目流程，路径相对当前有效的资产根目录。
3. `GameProject::startupFlow`：正式应用流程入口。
4. `GameProject::startupWorld`：旧项目的兼容启动路径。

`-asset-root <path>` 同时覆盖 flow 和 World 相对路径的解析根目录。`-server` 选择
无窗口的 Server 装配；Server 不使用 `-scene`，但可运行不含 `world.replace` 的
GameFlow。正式流程应使用项目内声明的稳定 World ID。

现有项目无需立即迁移：只设置 `startupWorld` 时行为保持不变。迁移时先新增包含
`app.start -> world.replace(startupWorld)` 的流程文件，再设置 `startupFlow`；两者同时
存在时优先使用 `startupFlow`，保留 `startupWorld` 可作为旧版本配置的兼容信息。

## Editor 项目清单

项目根目录提交 `project.ayproject.json`。它是工具入口，记录资产目录、游戏代码位置、
可运行程序以及各 World 关联的 Scene、UI 和 Tilemap；带回调的模块装配仍以
`game/MyGame.cpp` 中的 `GameProject` 为准。

```json
{
  "schemaVersion": 1,
  "id": "my_game",
  "displayName": "My Game",
  "engineProfile": "CLIENT_2D",
  "paths": {
    "assets": "Assets",
    "gameAssembly": "game/MyGame.cpp",
    "gameCode": "src"
  },
  "startupFlow": "flow/application.gameflow.json",
  "startupWorld": "main_menu",
  "gameFlow": {
    "uiActions": true,
    "contract": "gameflow.contract.json"
  },
  "worlds": [
    {
      "id": "main_menu",
      "scene": "worlds/main_menu.ayscene",
      "ui": "ui/main_menu.ui.json",
      "tilemaps": []
    }
  ],
  "run": {
    "executable": "out/build/windows-debug/bin/MyGameApp.exe",
    "workingDirectory": ".",
    "arguments": []
  }
}
```

`gameFlow.uiActions` 表示项目会安装 GameFlow/UIFlow 桥。headless 内容验证会据此加载
标准 UI 动作契约，但不会创建窗口、Widget 或渲染器；因此同一份流程可在完整客户端和
无图形 CI 中验证。它应与游戏装配中的 `enableGameFlowUIBridge(...)` 保持一致。

路径必须是项目根目录或资产根目录内的相对路径。`.ayeditor/run.json` 只作为个人或
临时运行覆盖；团队共享配置写在项目清单。Tilemap 编辑器保存
`.aytilemap.json` 时会在 `Assets/tilemaps` 同步生成运行时 `.aytilemap`，Scene 中
引用后者。独立客户端可通过 `enableGameProjectUIFlow(...)` 挂载项目级 UIFlow；
GameFlow 再通过稳定 context 控制主菜单、HUD 等界面的显示生命周期。

## 独立内容验证

`AYProjectContentValidator` 使用与运行时相同的 GameFlow migration、registry 和 normalized
program。它会扫描全部 GameFlow 草稿以发现损坏文件和重复 ID，并严格验证从
`startupFlow` 可达的 subflow、action、guard、World、UIFlow entry/context/signal 和
项目资产引用。输出的 dependency 行是确定性的，可供 CI 定位闭包来源。

```powershell
AYProjectContentValidator.exe <project-root> --profile headless
AYProjectContentValidator.exe <project-root> --profile full-client
AYProjectContentValidator.exe <project-root> --profile headless `
  --gameflow-contract gameflow.contract.json
```

`headless` profile 不创建窗口或 widget。未编入 AYUI 的纯 headless 构建使用轻量 UIFlow
契约读取；Editor 和完整客户端构建因具备 AYUI，会对两个 profile 都使用正式
`UIFlowSerializer`。只有 `full-client` profile 会进一步构造 UI layout widget 树。两种
profile 对 ID、payload type、默认值和路径边界给出一致结论。

## 确定性录制与回放

`AYApplicationGameFlow` 只依赖 `IGameFlowDeterminismExchange`，因此服务器和内容工具不会
引入回放持久化模块。需要文件录制时链接可选的 `AYApplicationGameFlowReplay`，用
`GameFlowReplayCaptureExchange` 或 `GameFlowReplayPlaybackExchange` 连接 AYReplay。
记录包含程序指纹、intent/update 顺序、guard/action 结果、异步完成、取消和 subflow
进出；回放时不再次调用有副作用的 action/guard/onCancel handler。

当前适配器面向一段 AYReplay session。它不提供跨轮转文件 playlist，也没有单独的
“必须消费到流尾” finalize API；调用方应保持外部 intent 与 update 的顺序/内容一致，并在
完整 session 边界结束验证。程序指纹不覆盖原生 C++ handler 的实现变化。
