# AYApplication

AYApplication 是引擎 Host 装配层，负责应用启动、子系统注册、GameLoop
接线、内建服务绑定、在线场景切换、Dedicated 权威场景托管和进程级清理。

## 目标与边界

| CMake 目标 | 用途 |
|---|---|
| `AYApplication` | 客户端/编辑器通用 Host、默认模块与场景生命周期 |
| `AYApplicationGameFlow` | 无 UI 的应用流程文档、校验、标准化计划与运行时协调器 |
| `AYApplicationGameFlowWorld` | 可选的 `world.replace` GameFlow/World 生命周期适配器 |
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
#include <AYApplicationGameFlow.h>
#include <AYApplicationGameFlowWorld.h>

#include <AYOnlineApplication/OnlineApplication.h>
#include <AYOnlineApplication/OnlineApplicationRuntimeModule.h>
#include <AYOnlineApplication/OnlineContent.h>
#include <AYOnlineApplication/DedicatedApplication.h>
```

稳定客户端接口位于 `interface/AYApplication/`，在线集成接口位于
`online/interface/AYOnlineApplication/`，实现辅助头位于
`include/AYApplication/`。GameFlow 接口位于 `gameflow/include/AYApplication/`，
由独立 `AYApplicationGameFlow` target 导出；它不依赖 AYUI、Renderer 或 Device。
需要真实 World 切换的应用再链接 `AYApplicationGameFlowWorld`，通过稳定 World ID
调用现有 `GameWorldRouter`，并由 `RuntimeSceneLoadFinishedEvent` 完成异步动作。

GameFlow 阶段一已提供 schema v1、typed intent/action、层次状态、guard、确定性
transition 选择、同步/异步 action、失败/取消/超时路径和 stale completion 隔离。
阶段二已提供 `world.replace(worldId)`，覆盖真实 `.ayscene` 成功与失败、旧 World
保留、并发拒绝、取消和迟到完成隔离。
UIFlow 与编辑器节点图均不在这个核心 target 中。完整边界与后续阶段见
[`GAMEFLOW-IMPLEMENTATION-PLAN.md`](../../AYDocs/GAMEFLOW-IMPLEMENTATION-PLAN.md)。

## AYModule 接线

`EngineModuleContext` 将 `IEngineHost::findService()` 适配到独立的
`IModuleContext`；`EngineModuleRuntime` 持有一个 `ModuleManager`，
提供 `prepare()`、`install()` 和逆序 `shutdown()`。其中
`prepare()` 只完成依赖解析和类型注册，调用方可以在 `install()` 前锁定
组件或反射注册表。`EngineModuleContext` 同时提供唯一的 `ComponentRegistry` 服务和
GameLoop SubSystem 发布服务，类型模块不得自行回退到另一份注册表，
`SubSystemModule` 因而可以把模块依赖顺序映射为 SubSystem 的注册与逆序撤销。

默认 Client、Server、Editor Host 已使用模块图装配运行时能力：

| 模块 ID | 运行时能力 |
|---|---|
| `AYEntity.Components` | Entity 组件元数据注册 |
| `AYEditor.Components` | Editor 自有组件元数据注册（仅 Editor） |
| `AYDevice.Runtime` | 窗口与输入（Client） |
| `AYEntity.Runtime` | Entity SubSystem 与 Core systems |
| `AYEntity.AnimationIntegration` | Animation 组件与系统（表现层可选） |
| `AYEntity.RenderIntegration` | Mesh/SkinnedMesh 系统（表现层可选） |
| `AYEntity.2DIntegration` | Tilemap/Sprite/OrthoCamera 系统（表现层可选） |
| `AYEntity.PhysicsIntegration` | Physics 组件与固定步 ECS 桥（物理可选） |
| `AYEntity.ScriptIntegration` | ScriptComponent 类型（脚本可选） |
| `AYEntity.NetworkIntegration` | NetworkComponent 类型（网络可选） |
| `AYRenderer.Runtime` | Renderer SubSystem |
| `AYPhysics.Runtime` | Physics SubSystem |
| `AYScript.Runtime` | Script SubSystem |
| `AYAudio.Runtime` | Audio SubSystem（可选） |
| `AYNetwork.Runtime` | Network SubSystem（Editor 默认；其他 Host 可按需加入） |
| `AYApplication.RuntimeSceneLoader` | 帧边界场景加载与切换（Client，可选） |
| `AYVideo.Runtime` | Video SubSystem（项目可选；严格依赖 Audio） |
| `AYNetwork.Online` | Online Services（项目可选；依赖 Network） |
| `AYNetwork.OnlineFlow` | 登录/大厅/匹配/加载流程（项目可选；依赖 Online） |
| `AYOnlineApplication.Runtime` | Online Flow 与场景加载桥（项目可选） |

Host 的固定顺序是 `default configure -> project configure -> prepare ->
ComponentRegistry::seal -> install`，
随后绑定内建服务并启动 GameLoop。默认 Host 在模块安装后不再直接注册额外的
GameLoop SubSystem；旧 `registerDefault*Modules()` API 仅保留作兼容入口。
Scene/EventBus 观察者、Task 完成 hook、Editor 自有 DeviceManager 与输入桥仍是
Host 接线职责。Video 与 Online 栈已经拥有模块节点，但不会进入默认 Client/Server/Editor；
项目通过 `GameDesc::configureModules` 显式选择，普通应用不会因此增加 FFmpeg 或在线后端。

```cpp
ayt::app::GameDesc desc;
desc.configureModules = [onlineConfig](
    ayt::app::EngineModuleRuntime& runtime) {
    return ayt::app::online::configureOnlineApplicationModules(
        runtime, onlineConfig);
};
auto app = ayt::app::IApplication::create(desc);
```

该回调在默认 Client/Server 图加入后、`prepare()` 前执行；Online helper 要求 Client 图
已包含 `AYApplication.RuntimeSceneLoader`。旧显式注册函数仍可用于独立 Demo 和迁移期代码。

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
- `AYScene` 与默认 Application 核心链路只依赖 `AYEntityCore`；禁用某项 CMake
  feature 后，对应 Runtime 与 Entity integration target 均不会生成或进入最终链接。
- `windows-headless-debug` 会生成 `AYApplication_HeadlessSmoke`；构建后运行它可防止
  Renderer/Audio/Physics/Script/Device/Animation/Network 重新泄漏进最小链接闭包。
- `GameFlowCoreTest` 在 headless 与完整客户端组合中运行同一套流程语义测试；两个
  preset 复用根仓 `out/build/vcpkg_installed`。
- `GameFlowWorldTest` 在相同两种组合中运行真实场景切换、失败保留、并发请求、
  取消和迟到完成测试；核心 target 仍不引入 Scene/Host 依赖。
- 最小 Application 仍使用 AYResource/AYScene，因此保留通用资产导入、存储与
  序列化依赖；本次边界不等同于“零第三方依赖”。
- 下一步是接入游戏项目的真实 authority systems，并完成生产会话后端分配到
  两客户端进入场景的端到端验证。具体验收条件见 [design.md](design.md)。

Host 服务约束与完整在线应用说明见 [docs/engine-host.md](docs/engine-host.md)
和 [docs/online-application.md](docs/online-application.md)。
