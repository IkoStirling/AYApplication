# Engine Host — 装配与服务约定

**Status:** AYModule 默认装配 + **服务面**（`resources` / `physics` / `audio` / `deviceManager` / `scenes` + 可扩展键表）
**Owner:** `AYApplication`  
**Related:** [`../design.md`](../design.md) · [`../../AYGameLoop/docs/sim-present-time.md`](../../AYGameLoop/docs/sim-present-time.md) · [`../../AYScene/design.md`](../../AYScene/design.md)

本文件定义**引擎外壳**：默认 SubSystem 装配、`IEngineHost` 服务发现、以及**以后加新单例/服务时必须走的登记流程**。

---

## 1. 角色分工

| 角色 | 模块 | 职责 |
|------|------|------|
| 产品入口 | `IApplication`（Client=`ApplicationImpl`，Editor=`EditorApp`） | 选壳、CLI、生命周期钩子 |
| **引擎外壳** | `IEngineHost` + `EngineModuleRuntime` + `EngineRuntimeScope` + `configureDefault*Modules` | 模块依赖、当前 Host、SubSystem 发布、服务表与 World/Scene 生命周期 |
| 帧循环 | `AYGameLoop` | `fixedUpdate` / `update`、暂停、帧率 |
| 扩展（以后） | `AYPlugin` | 往 Host 扩展点填实现 |

---

## 2. 默认装配表（CR 用）

生产入口在模块安装前创建 `EngineRuntimeScope`，再统一执行：
`configure -> prepare -> ComponentRegistry::seal -> install -> scope.refresh()`
→ GameLoop 初始化。GameLoop 负责 SubSystem 的 `initialize/update/shutdown`；
`EngineModuleRuntime::shutdown()` 随后按模块逆序撤销仍存在的注册，最后由
scope 恢复进入本次运行前的 Host 服务、任务 hook、Scene 选择与活动 World。旧 `registerDefault*Modules()`
保留为直接注册兼容 API，不再是 `ApplicationImpl` / `EditorApp` 的默认路径。

### 2.1 Client（`configureDefaultClientModules`）

| 顺序 | 模块 / 步骤 | 说明 |
|------|------|------|
| 1 | `AYDevice.Runtime` | `DeviceSubSystem`，窗口 + 输入 |
| 2 | `AYEntity.Components` | `registerTypes()` 阶段注册组件元数据 |
| 3 | `AYEntity.Runtime` | Entity SubSystem 与 Core systems |
| 4 | `AYRenderer.Runtime`（可选） | 仅 `enablePresentation=true` |
| 5 | `AYEntity.AnimationIntegration`（可选） | Animation 组件与系统 |
| 6 | `AYEntity.RenderIntegration`（可选） | Mesh/SkinnedMesh 表现系统；依赖 Renderer + Animation integration |
| 7 | `AYEntity.2DIntegration`（可选） | Tilemap/Sprite/OrthoCamera 表现系统 |
| 8 | `AYPhysics.Runtime`（可选） | `enablePhysics` / `-no-physics`；固定步物理 |
| 9 | `AYEntity.PhysicsIntegration`（可选） | Physics 组件与 FixedPre/FixedPost ECS 桥 |
| 10 | `AYEntity.ScriptIntegration`（可选） | ScriptComponent 类型注册 |
| 11 | `AYScript.Runtime`（可选） | Logia；依赖 Script integration |
| 12 | `AYAudio.Runtime`（可选） | `-no-audio` 则跳过 |
| 13 | `AYApplication.RuntimeSceneLoader`（可选） | Egress 帧边界场景加载；初始化时创建或加载 Client Play Scene，并切换活动 World |
| 14 | `AYApplication.GameWorldRouter`（`GameProject` 客户端） | 用稳定 World ID 驱动 RuntimeSceneLoader，在 Egress 边界切换 Scene/World |
| 15 | `GameDesc::configureModules`（可选） | 项目在依赖解析前追加游戏、Video/Online 等节点 |
| 16 | `EngineRuntimeScope::refresh` | 写入 Host 服务表（含 RuntimeSceneLoader / GameWorldRouter）；安装 Scene→EventBus 观察者；安装 AYTask→`TaskCompleteEvent` hook；退出时成组恢复 |

### 2.1.1 引擎事件生产者（Host 装配后）

| 来源 | 事件 | 文档 |
|------|------|------|
| DeviceSubSystem | Window resize/close、DeviceAction | `AYEventSystem/README.md`、`AYDevice/README.md` |
| AsyncLoader | ResourceLoadComplete / Failed | `AYEventSystem/README.md` |
| TaskCompletionHook（本函数安装） | TaskComplete | `AYTask/README.md` |
| SceneLifecycleEventBridge | Scene begin/end/current | `AYEventSystem` SceneEvents |
| GameLoop | SimTick / frame events | `AYGameLoop/GameLoopEvents.h` |

**World 权威（P0）：** `SceneManager::setCurrent` 调用 `World::setActiveWorld(&scene.world())`。`EntitySubSystem::update` 走 `World::instance()`，因此 **不要** 再对同一 World 调 `scenes()->tick(dt)`（会双 tick）。`scenes()->tick` 仅给 Preview / 显式旁路用。

### 2.2 Editor（`configureDefaultEditorModules`）

| 顺序 | 模块 / 步骤 | 说明 |
|------|------|------|
| 1 | `AYEntity.Components` | Entity 类型注册 |
| 2 | `AYEditor.Components` | Editor 自有类型注册；依赖 `AYEntity.Components` |
| 3 | `AYEntity.Runtime` | Entity SubSystem 与 Core systems |
| 4 | `AYRenderer.Runtime` | Editor 合成渲染 |
| 5 | `AYEntity.AnimationIntegration` | Animation 组件与系统 |
| 6 | `AYEntity.RenderIntegration` | Mesh/SkinnedMesh 表现系统 |
| 7 | `AYEntity.2DIntegration` | Tilemap/Sprite/OrthoCamera 表现系统 |
| 8 | `AYPhysics.Runtime` + `AYEntity.PhysicsIntegration` | 编辑器 Play 物理与 ECS 桥 |
| 9 | `AYEntity.ScriptIntegration` + `AYScript.Runtime` | ScriptComponent 与 Logia |
| 10 | `AYEntity.NetworkIntegration` + `AYNetwork.Runtime` | NetworkComponent 与 Network SubSystem |
| 11 | `AYAudio.Runtime`（可选） | `-no-audio` 跳过；供 Tools → Audio Editor |
| 12 | Host 接线 | `EngineRuntimeScope::refresh`；Editor 自有 `DeviceManager` 与 Script 输入桥仍由 App 持有 |
| 13 | Play World 系统 | `beginPlay` 后为新的 Play Scene World 调用兼容 bootstrap |

### 2.3 Server（`configureDefaultServerModules` / `-server`）

| 顺序 | 模块 / 步骤 | 说明 |
|------|------|------|
| 1 | `AYEntity.Components` | 类型注册 |
| 2 | `AYEntity.Runtime` | Entity 核心，无表现系统 |
| 3 | `AYPhysics.Runtime`（可选） | 默认开；`-no-physics` 关 |
| 4 | `AYEntity.PhysicsIntegration`（可选） | Physics 组件与固定步 ECS 桥 |
| 5 | `AYEntity.ScriptIntegration`（可选） | ScriptComponent 类型注册 |
| 6 | `AYScript.Runtime`（可选） | `enableScript`；依赖 Script integration |
| 7 | `EngineRuntimeScope::refresh` | 无 Device / Audio / Renderer；退出时恢复进入前状态 |

**跳过** Device / Audio / Renderer。`ApplicationImpl`：`GameDesc::serverMode` 或 CLI `-server`。

### 2.4 项目可选模块

第三阶段提供下列节点，但默认 Client/Server/Editor 都不会自动加入它们：

| 模块 ID | 依赖 | 适用范围 |
|------|------|------|
| `AYVideo.Runtime` | `AYAudio.Runtime` | 有视频播放需求的 Client/Tool |
| `AYNetwork.Online` | `AYNetwork.Runtime` | Lobby、Matchmaking、Session 服务 |
| `AYNetwork.OnlineFlow` | `AYNetwork.Online` | 登录至加载/会话退出的应用流程 |
| `AYOnlineApplication.Runtime` | `AYNetwork.OnlineFlow`、`AYApplication.RuntimeSceneLoader` | Client Online Flow 与 Scene 桥 |
| `AYApplication.UIFlowRuntime` | `AYApplicationUI`（编译期依赖 AYUI） | Client/Tool 的跨 World UI 编排；项目注入 Screen host 与 Flow document |
| `AYApplication.UIFlowSceneBridge` | `AYApplication.UIFlowRuntime`、`AYEntity.Runtime`；可选 `AYApplication.GameWorldRouter` | World Scope/Context 生命周期绑定、通用区域与外部 Scene Signal 接入 |

`GameDesc::configureModules` 在默认图配置完成后执行。Online 客户端优先调用
`configureOnlineApplicationModules()`，它会复用已有稳定 ID，补齐 Network → Online →
OnlineFlow → OnlineApplication，并在缺少 RuntimeSceneLoader 时于图冻结前报错。

### 2.5 编译期能力边界

根工程通过 `AY_ENABLE_ANIMATION/AUDIO/DEVICE/RENDERER/PHYSICS/SCRIPT/NETWORK`
以及 `AY_ENABLE_AY2D/AY_ENABLE_ENTITY_2D_SCHEMA/AYVIDEO/AYVOXEL` 决定是否创建对应 target。AYApplication
只对实际存在的 target 编译 include 与装配分支，并导出同一组
`AY_APPLICATION_HAS_*` capability；运行时请求未编译能力会返回明确失败，而不是
静默注册或留下未解析符号。

`windows-headless-debug` 关闭上述可选 Runtime，构建
`AYApplication_HeadlessSmoke` 并形成真实最终链接。该预设复用项目现有的
`out/build/vcpkg_installed`，同时关闭自身的 manifest 安装动作；依赖供应仍由
默认项目配置负责，精简验证不会重同步或卸载现有包。

`AY_ENABLE_ENTITY_2D_SCHEMA` 只编译 Sprite、Tilemap、OrthoCamera 的序列化
元数据，不编译 Renderer 或 2D presentation systems。游戏的 headless 内容验证
因此能真实加载与客户端相同的混合 2D/3D Scene。

截至第三阶段，上述默认 composition root 不再在模块安装后直接注册 GameLoop
SubSystem。Host 接线、Editor DeviceManager 和每个 Play World 的 ECS system
bootstrap 不属于 GameLoop SubSystem，因此仍由应用层持有。
---

## 3. 服务面（游戏 / 脚本怎么用）

```cpp
auto* host = ayt::app::currentEngineHost();
if (!host) { /* outside Application::run */ }

// 推荐：具名访问（演示里的 ctx.physics() 形态）
ayt::resource::ResourceManager* res = host->resources();   // 通常非空（单例回退）
ayt::physics::PhysicsManager*   phys = host->physics();  // 未 provide 则为 nullptr
ayt::audio::AudioEngine*        aud = host->audio();     // SubSystem 未 init 前可能为 nullptr
ayt::device::DeviceManager*     dev = ayt::app::deviceManager(*host); // Client/Editor 统一输入入口
// PR-6 (v0.1.3, design §10 Q-F 收口): 关卡生命周期管家
ayt::scene::SceneManager*       scenes = host->scenes(); // 永不为 null（Meyers singleton）

if (phys) {
    phys->step(dt);
}

if (scenes) {  // 防御性写法；实际不会 nullptr（Meyers singleton）
    if (scenes->canBeginPlay()) { /* enable Play button */ }
    scenes->tick(dt);
}

// 通用键（新服务 / 游戏自有服务）
host->provide("game.inventory", inventorySys);
auto* inv = host->service<InventorySystem>("game.inventory");
```

| API | 含义 |
|-----|------|
| `resources()` | 已 `provide` 的指针，否则回退 `ResourceManager::instance()` |
| `physics()` | 登记表，或惰性从 `PhysicsSubSystem::findRegistered()->manager()`（与 `audio()` 同形态） |
| `audio()` | 登记表，或惰性从名为 `"Audio"` 的 SubSystem 取 `engine()` |
| `deviceManager(host)` | 从稳定键解析非拥有型 DeviceManager；Client 与 Editor 使用同一入口 |
| `scenes()` | PR-6 (v0.1.3)：已 `provide` 的指针，否则回退 `SceneManager::instance()`（**永不为 null**） |
| `findSubSystem(name)` | 逃生口；新代码优先具名/键服务，不要靠字符串找业务 API |

`ApplicationImpl::run` / `EditorApp::run` 内使用 `EngineHostScope` 选择当前 Host；
它们还在模块安装前创建 `EngineRuntimeScope`，装配后调用 `refresh()`。关闭时先
销毁模块，再由 scope 恢复服务表、任务 hook、Scene 选择和活动 World。

Physics 默认经模块图注册；兼容调用方也可手动：

```cpp
ayt::app::registerPhysicsModule(desc);           // 兼容直接注册
// initialize 后：
host->physics();                                 // lazy → PhysicsManager*
// 或显式：
ayt::app::providePhysics(host, mgr);
```
---

## 4. 以后添加新单例 / 新服务（必读）

引擎会不断出现新的全局能力（Save、Scene、Navigation…）。**不要**让游戏代码直接依赖又一个 `Foo::instance()` 扩散；按下列清单登记到 Host。

### 4.1 清单（CR 勾选）

1. **选稳定键名**  
   - 引擎内置：`ayt.<module>.<Type>`（与现有 `kHostService*` 同风格）  
   - 游戏项目：`game.<name>` / `<studio>.<name>`  
   - 在 `AYApplication/IEngineHost.h` 增加 `inline constexpr const char* kHostService…`（若为引擎内置）

2. **更新本文件表格**（§4.2）——键、类型、生命周期、谁负责 `provide`

3. **（可选）具名访问器**  
   - 高频引擎服务才加 `host->foo()`；低频用 `service<T>(key)` 即可  
   - 具名 API 放在 `IEngineHost` + `DefaultEngineHost`，并写清 nullptr 语义

4. **在装配点 `provide`**  
   - 进程单例：`bindBuiltinHostServices` 或模块 `register*` 末尾  
   - 会话对象（如 PhysicsManager）：`create` 之后立刻  
     `host.provide(kHostServicePhysics, mgr.get());`  
   - 关闭/销毁前：`provide(key, nullptr)` 或 `clearProvidedServices()`

5. **所有权**  
   - Host **不拥有**服务对象，只存裸指针；生命周期仍归模块 / App / unique_ptr

6. **禁止**  
   - 新玩法代码新增对 `FooManager::instance()` 的硬依赖而不登记 Host（过渡期 allowlist 除外）

### 4.2 内置服务键表（随代码更新）

| Key 常量 | 字符串 | 类型 | 谁 provide | 空指针含义 |
|----------|--------|------|------------|------------|
| `kHostServiceResources` | `ayt.resource.ResourceManager` | `ResourceManager*` | `bindBuiltinHostServices`；`resources()` 另有 instance 回退 | 几乎不应为空 |
| `kHostServicePhysics` | `ayt.physics.PhysicsManager` | `PhysicsManager*` | `AYPhysics.Runtime` / 兼容 `registerPhysicsModule` / `providePhysics`；`physics()` 另有 SubSystem 惰性回退 | 未装配物理或尚未 initialize |
| `kHostServicePhysicsQuery` | `ayt.physics.IPhysicsQuery` | `IPhysicsQuery*` | `providePhysicsQuery` / SubSystem `query()` | 未装配物理或尚未 initialize |
| `kHostServiceAudio` | `ayt.audio.AudioEngine` | `AudioEngine*` | bind 时若已 init；否则 `audio()` 惰性查 SubSystem | 无 Audio 模块或尚未 initialize |
| `kHostServiceDeviceManager` | `ayt.device.DeviceManager` | `DeviceManager*` | Client 由 `bindBuiltinHostServices` 映射 DeviceSubSystem；Editor 由 composition root 调用 `provideDeviceManager` | Headless、Device 模块未安装或尚未 initialize |
| `kHostServiceScenes` | `ayt.scene.SceneManager` | `SceneManager*` | `bindBuiltinHostServices`（PR-6 v0.1.3，Meyers singleton）；`scenes()` 另有 instance 回退 | 几乎不应为空（单例） |
| `kHostServiceRuntimeSceneLoader` | `ayt.app.RuntimeSceneLoader` | `IRuntimeSceneLoader*` | `AYApplication.RuntimeSceneLoader` 安装，`bindBuiltinHostServices` 发布 | Client 禁用或未安装 RuntimeSceneLoader |
| `kHostServiceGameWorldRouter` | `ayt.app.GameWorldRouter` | `IGameWorldRouter*` | `runGameProject` 添加的 `AYApplication.GameWorldRouter` | Server、非 GameProject 应用或尚未初始化 |
| `kHostServiceUIFlowRuntime` | `ayt.app.UIFlowRuntime` | `UIFlowRuntime*` | `AYApplication.UIFlowRuntime` 安装；模块 shutdown 前清除 | 未选择 UI Flow、headless/server 或模块尚未安装 |
| `kHostServiceUIFlowSceneBridge` | `ayt.app.UIFlowSceneBridge` | `UIFlowSceneBridge*` | `AYApplication.UIFlowSceneBridge` 安装；模块 shutdown 前清除 | 未接入 Scene Bridge 或模块尚未安装 |
| `kHostServiceOnlineFlow` | `ayt.net.OnlineFlowCoordinator` | `OnlineFlowCoordinator*` | `AYOnlineApplication.Runtime` 初始化 | 未选择 Online 栈或尚未初始化 |
| `kHostServiceOnlineApplication` | `ayt.app.online.OnlineApplication` | `IOnlineApplicationSubSystem*` | `AYOnlineApplication.Runtime` 初始化 | 未选择 Online 栈或尚未初始化 |

**新增行时：** 改代码键常量 + 改本表 +（若有）具名 API，同一 PR。

### 4.3 示例：假设新增 `AYSave::SaveService`

```cpp
// AYApplication/IEngineHost.h
inline constexpr const char* kHostServiceSave = "ayt.save.SaveService";

// 装配后
host.provide(kHostServiceSave, saveService);

// 游戏
if (auto* save = host.service<ayt::save::SaveService>(kHostServiceSave)) {
    save->writeSlot(0);
}
```

若 Save 变成一等公民再考虑 `host->save()` 糖衣。

---

## 5. CR 纪律

1. 默认 Host 走 `EngineRuntimeScope -> configureDefault*Modules -> GameDesc::configureModules -> prepare -> seal -> install -> scope.refresh()`；`registerDefault*Modules` 仅作兼容。
2. 业务取资源/音频/物理：`currentEngineHost()->resources()/audio()/physics()`。  
3. 新全局能力：走 §4，不要只加单例。  
4. 改装配顺序：先改 §2 表再改 helper。  
5. Sim/Present：`AYGameLoop/docs/sim-present-time.md`。

---

## 6. Host 扩展与事件对齐（已落地）

| 能力 | API | 说明 |
|------|-----|------|
| **多 Host（测试注入）** | `EngineHostScope` + 自定义 `IEngineHost` | `currentEngineHost()` / `resolveEventBus()` 走当前 Host |
| **窄物理查询** | `host->physicsQuery()` / `IPhysicsQuery` | raycast / overlap；勿为查询拉全 `PhysicsManager` |
| **Plugin 扩展点** | `host->registerPlugin(IHostPluginHooks*)` | `onAttach` 里 `provide` + 订阅 `SimTickEvent` / Scene 事件 |
| **Sim 时间锚点** | `SimTickEvent` + `IGameLoop::getSimTick()` | 每个 `fixedUpdate` 步发射一次 |
| **Scene 生命周期** | `SceneBeginPlayEvent` / `SceneEndPlayEvent` / `SceneCurrentChangedEvent` | SceneManager observer → EventBus（AYScene 不依赖 EventSystem） |

业务代码优先：`currentEngineHost()->eventBus().subscribe<SimTickEvent>(...)`，避免新的 `::instance()` 扩散。

---

## 7. 一句话

**App 选壳 → 装配模块 → `provide` 进 Host → 游戏用 `host->physics()` / `physicsQuery()` / 键服务 / EventBus；新单例先登记键表，再考虑具名 API。**
