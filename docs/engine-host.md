# Engine Host — 装配与服务约定

**Status:** Step 1（装配）+ **服务面**（`resources` / `physics` / `audio` / `scenes` + 可扩展键表）  
**Owner:** `AYApplication`  
**Related:** [`../design.md`](../design.md) · [`../../AYGameLoop/docs/sim-present-time.md`](../../AYGameLoop/docs/sim-present-time.md) · [`../../AYScene/design.md`](../../AYScene/design.md)

本文件定义**引擎外壳**：默认 SubSystem 装配、`IEngineHost` 服务发现、以及**以后加新单例/服务时必须走的登记流程**。

---

## 1. 角色分工

| 角色 | 模块 | 职责 |
|------|------|------|
| 产品入口 | `IApplication`（Client=`ApplicationImpl`，Editor=`EditorApp`） | 选壳、CLI、生命周期钩子 |
| **引擎外壳** | `IEngineHost` + `registerDefault*Modules` | 装配顺序、当前 Host、**服务表** |
| 帧循环 | `AYGameLoop` | `fixedUpdate` / `update`、暂停、帧率 |
| 扩展（以后） | `AYPlugin` | 往 Host 扩展点填实现 |

---

## 2. 默认装配表（CR 用）

关机大致**逆序**（GameLoop `shutdown` 清 SubSystem；Host scope 在 App `run` 结束时清空）。

### 2.1 Client（`registerDefaultClientModules`）

| 顺序 | 步骤 | 说明 |
|------|------|------|
| 1 | `DeviceSubSystem` | 窗口 + 输入 |
| 2a | `bootstrapEntityCore()` | 默认：Entity 核心，不拉渲染 |
| 2b | `registerEntityPresentationStack()` | `enablePresentation=true`：`bootstrapModule` + `RendererSubSystem` |
| 3 | `ScriptSubSystem` | Logia |
| 4 | `PhysicsSubSystem`（可选） | `enablePhysics` / `-no-physics`；`fixedUpdate` → `step`+`fetchResults` |
| 5 | `AudioSubSystem`（可选） | `-no-audio` 则跳过 |
| 6 | `bindBuiltinHostServices` | 写入 Host 服务表（`physics()` 也可惰性从 SubSystem 取） |
| 7 | Client Play Scene | `ApplicationImpl` 创建 `Scene(Play)`，可选 `-scene` / `GameDesc::scenePath` load，`setCurrent` → `World::instance()` 指向 Scene World |

**World 权威（P0）：** `SceneManager::setCurrent` 调用 `World::setActiveWorld(&scene.world())`。`EntitySubSystem::update` 走 `World::instance()`，因此 **不要** 再对同一 World 调 `scenes()->tick(dt)`（会双 tick）。`scenes()->tick` 仅给 Preview / 显式旁路用。

### 2.2 Editor（`registerDefaultEditorModules`）

| 顺序 | 步骤 |
|------|------|
| 1 | `registerEntityPresentationStack()`（与 Client `enablePresentation` 共用） |
| 2 | `registerPhysicsModule()` |
| 3 | Network + Script |
| 4 | `bindBuiltinHostServices`（+ Editor 自有 Device 桥接仍在 App 内） |
| 5 | Play：`beginPlay` 后再次 `bootstrapModule()`（系统落到 Play Scene World） |

### 2.3 Server（`registerDefaultServerModules` / `-server`）

| 顺序 | 步骤 | 说明 |
|------|------|------|
| 1 | `bootstrapEntityCore()` | Entity 核心 |
| 2 | `PhysicsSubSystem`（可选） | 默认开；`-no-physics` 关 |
| 3 | `ScriptSubSystem`（可选） | `enableScript` |
| 4 | `bindBuiltinHostServices` | 无 Device / Audio / Renderer |

**跳过** Device / Audio / Renderer。`ApplicationImpl`：`GameDesc::serverMode` 或 CLI `-server`。
---

## 3. 服务面（游戏 / 脚本怎么用）

```cpp
auto* host = ayt::app::currentEngineHost();
if (!host) { /* outside Application::run */ }

// 推荐：具名访问（演示里的 ctx.physics() 形态）
ayt::resource::ResourceManager* res = host->resources();   // 通常非空（单例回退）
ayt::physics::PhysicsManager*   phys = host->physics();  // 未 provide 则为 nullptr
ayt::audio::AudioEngine*        aud = host->audio();     // SubSystem 未 init 前可能为 nullptr
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
| `scenes()` | PR-6 (v0.1.3)：已 `provide` 的指针，否则回退 `SceneManager::instance()`（**永不为 null**） |
| `findSubSystem(name)` | 逃生口；新代码优先具名/键服务，不要靠字符串找业务 API |

`ApplicationImpl::run` / `EditorApp::run` 内使用 `EngineHostScope`；装配后调用 `bindBuiltinHostServices`。

Physics 默认经装配表注册；也可手动：

```cpp
ayt::app::registerPhysicsModule(desc);           // GameLoop SubSystem
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
   - 在 `IEngineHost.h` 增加 `inline constexpr const char* kHostService…`（若为引擎内置）

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
| `kHostServicePhysics` | `ayt.physics.PhysicsManager` | `PhysicsManager*` | `registerPhysicsModule` / `providePhysics`；`physics()` 另有 SubSystem 惰性回退 | 未装配物理或尚未 initialize |
| `kHostServicePhysicsQuery` | `ayt.physics.IPhysicsQuery` | `IPhysicsQuery*` | `providePhysicsQuery` / SubSystem `query()` | 未装配物理或尚未 initialize |
| `kHostServiceAudio` | `ayt.audio.AudioEngine` | `AudioEngine*` | bind 时若已 init；否则 `audio()` 惰性查 SubSystem | 无 Audio 模块或尚未 initialize |
| `kHostServiceScenes` | `ayt.scene.SceneManager` | `SceneManager*` | `bindBuiltinHostServices`（PR-6 v0.1.3，Meyers singleton）；`scenes()` 另有 instance 回退 | 几乎不应为空（单例） |

**新增行时：** 改代码键常量 + 改本表 +（若有）具名 API，同一 PR。

### 4.3 示例：假设新增 `AYSave::SaveService`

```cpp
// IEngineHost.h
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

1. 默认注册走 `registerDefault*Modules`，随后 `bindBuiltinHostServices`。  
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
