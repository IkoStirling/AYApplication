# AYApplication

AYApplication 是引擎 Host 装配层，负责应用启动、子系统注册、GameLoop
接线、内建服务绑定、在线场景切换、Dedicated 权威场景托管和进程级清理。

## 目标与边界

| CMake 目标 | 用途 |
|---|---|
| `AYApplication` | 客户端/编辑器通用 Host、默认模块与场景生命周期 |
| `AYOnlineContent` | 后端内容身份到可信本地场景的精确版本映射 |
| `AYOnlineApplication` | 客户端 Online Flow 与运行时场景加载桥接 |
| `AYDedicatedApplication` | Dedicated allocation 与真实 `Scene/World` 的权威端桥接 |
| `AYApplication_DedicatedServer` | 可运行的通用 Dedicated Server 入口 |

网络协议、会话后端、P2P、Dedicated allocation 和准入凭据由 AYNetwork
负责；AYApplication 只负责把这些结果接入实际应用、场景和游戏世界。

## 公开接口

```cpp
#include <AYApplication.h>
#include <AYApplication/EngineModuleContext.h>
#include <AYApplication/EngineModuleRuntime.h>
#include <AYApplication/IApplication.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/RegisterDefaultModules.h>

#include <AYOnlineApplication/OnlineApplication.h>
#include <AYOnlineApplication/OnlineContent.h>
#include <AYOnlineApplication/DedicatedApplication.h>
```

稳定客户端接口位于 `interface/AYApplication/`，在线集成接口位于
`online/interface/AYOnlineApplication/`，实现辅助头位于
`include/AYApplication/`。

## AYModule 接线

`EngineModuleContext` 将 `IEngineHost::findService()` 适配到独立的
`IModuleContext`；`EngineModuleRuntime` 持有一个 `ModuleManager`，
提供 `prepare()`、`install()` 和逆序 `shutdown()`。其中
`prepare()` 只完成依赖解析和类型注册，调用方可以在 `install()` 前锁定
组件或反射注册表。

AYEntity 当前提供首个注册阶段试点 `EntityComponentModule`。Host 使用它时在
`prepare()` 后封存 `ComponentRegistry`，再调用 `install()`。默认 Client、
Server、Editor 装配仍未替换，也未迁移 GameLoop SubSystem 所有权。

## Scene-backed Dedicated Server

`DedicatedSceneHost` 为每个 allocation 创建并加载一个 Play `Scene`，持有其
独立 `World`，在 `DedicatedServerRuntime` 主循环中 tick，并在 allocation
被回收或最后一名玩家离开后卸载。后端只传递逻辑内容身份；文件路径只能来自
服务器本地可信目录。

目录格式为制表符分隔的四列：

```text
# content-id<TAB>version<TAB>scene-path<TAB>scene-name
maps/arena	content-1	Content/Scenes/Arena.ayscene	ArenaAuthority
```

启动示例：

```powershell
$env:AY_ONLINE_SERVER_TOKEN = "<fleet-token>"
$env:AY_ONLINE_BACKEND_TLS = "true"
$env:AY_DEDICATED_MAX_WORLDS = "1" # 仍使用 World::instance() 的项目必须为 1

.\AYApplication_DedicatedServer.exe `
  api.example.com 443 ds-sg-01 asia build-42 `
  203.0.113.20 7777 16 .\content-catalog.tsv
```

游戏项目应复用 `DedicatedSceneHost`，并在 `prepareWorld` 中注册游戏模式、
脚本、物理、复制与权威实体生成逻辑；通用入口只注册场景反序列化所需的组件
工厂。

## 构建与验证

VS CMake/Ninja 环境下使用单线程构建：

```powershell
cmake --build --preset windows-debug-vs2026-insider `
  --target AYApplication_DedicatedServer DedicatedOnlineVerticalSliceTest `
  --parallel 1

ctest --test-dir out/build/windows-debug-vs2026-insider `
  -R "^(OnlineApplicationTest|OnlineVerticalSliceTest|DedicatedOnlineVerticalSliceTest)$" `
  --output-on-failure -j 1
```

`DedicatedOnlineVerticalSliceTest` 使用真实 GNS listener 和两个客户端，覆盖
allocation、逐玩家准入、真实场景 tick、首名玩家离开后的席位保持，以及最后
一名玩家离开后的世界释放。

## 当前限制与下一步

- Dedicated 进程不会注册窗口、渲染、UI 或音频子系统。
- `AYScene` 当前仍依赖单体 `AYEntity`，因此最终链接命令可能包含表现层静态库；
  运行时 DLL 依赖不包含渲染/UI/音频后端。
- 下一步是接入游戏项目的真实 authority systems，并完成生产会话后端分配到
  两客户端进入场景的端到端验证。具体验收条件见 [design.md](design.md)。

Host 服务约束与完整在线应用说明见 [docs/engine-host.md](docs/engine-host.md)
和 [docs/online-application.md](docs/online-application.md)。
