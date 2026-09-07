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
├── assets/
│   └── worlds/                 # .ayscene 文件
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
    ASSET_DIR "${CMAKE_SOURCE_DIR}/assets"
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
    game.assetRoot = "assets";
    game.startupWorld = "main_menu";
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

1. 在 `assets/worlds` 添加 `.ayscene`。
2. 在 `game/MyGame.cpp` 的 `worlds` 中登记稳定 ID 和相对路径。
3. 从游戏系统调用 `requestWorld("stable_id")`。

命令行 `-scene <path>` 可临时覆盖客户端启动 World；`-server` 选择无窗口的 Server
装配。正式流程仍应使用项目内声明的稳定 World ID。
