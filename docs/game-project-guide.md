# 独立游戏快速装配

`GameProject` 是独立游戏的 composition root。应用只描述自己需要的引擎能力、
游戏模块和 World 路由；窗口、渲染、音频、物理、Scene/World、主循环与关闭顺序
继续由 `AYApplication` 装配。独立游戏不需要经过 Editor。

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
"AY_VCPKG_INSTALLED_DIR": "${sourceDir}/../AliyatEngine/out/build/vcpkg_installed"
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
初始化后会自动排队该 intent，首个 Ingress 更新再执行启动动作。最小启动流程如下：

```json
{
  "schemaVersion": 1,
  "id": "application",
  "initialState": "boot",
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

路径必须是项目根目录或资产根目录内的相对路径。`.ayeditor/run.json` 只作为个人或
临时运行覆盖；团队共享配置写在项目清单。Tilemap 编辑器保存
`.aytilemap.json` 时会在 `Assets/tilemaps` 同步生成运行时 `.aytilemap`，Scene 中
引用后者。当前 UI 文件会被清单和验证器关联到 World；独立客户端的 World UI
Overlay 生命周期尚未接入 `AYApplication`，在该能力完成前游戏代码不能假定它会
随 World 自动显示。
