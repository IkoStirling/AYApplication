# Engine Host — 装配与服务约定（Step 1）

**Status:** Step 1 — docs + thin host + default register helpers  
**Owner:** `AYApplication`  
**Related:** [`../design.md`](../design.md) · [`../../AYGameLoop/docs/sim-present-time.md`](../../AYGameLoop/docs/sim-present-time.md)

本文件定义**引擎外壳**的第一步：默认 SubSystem 装配表、薄 `IEngineHost`、以及 CR 纪律。  
完整 Plugin 扩展点 / 多 Host 实例见文末「以后」。

---

## 1. 角色分工

| 角色 | 模块 | 职责 |
|------|------|------|
| 产品入口 | `IApplication`（Client=`ApplicationImpl`，Editor=`EditorApp`） | 选壳、CLI、生命周期钩子 |
| **引擎外壳** | `IEngineHost` + `registerDefault*Modules` | 装配顺序、当前 Host、服务查找入口 |
| 帧循环 | `AYGameLoop` | `fixedUpdate` / `update`、暂停、帧率 |
| 扩展（以后） | `AYPlugin` | 往 Host 扩展点填实现 |

**今天：** `GameApp` / `ServerApp` 仍是 design 概念；可运行入口 = `ApplicationImpl`（standalone client）+ `EditorApp`。

---

## 2. 默认装配表（CR 用）

关机大致**逆序**（GameLoop `shutdown` 清 SubSystem；Host scope 在 App `run` 结束时清空）。

### 2.1 Client（`registerDefaultClientModules`）

| 顺序 | 步骤 | 说明 |
|------|------|------|
| 1 | `DeviceSubSystem` | 窗口 + 输入；priority 靠前 |
| 2 | `bootstrapEntityCore()` | Entity 核心，**不**拉渲染系统 |
| 3 | `ScriptSubSystem` | Logia；显式注册 |
| 4 | `AudioSubSystem`（可选） | `-no-audio` 则跳过 |

Client **不**默认注册：`RendererSubSystem`、`NetworkSubSystem`、完整 `bootstrapModule()`。

调用方随后可做：Script ← `DeviceInputProvider`（生命周期由 App 保证）。

### 2.2 Editor shell（`ayt::editor::registerDefaultEditorModules`）

| 顺序 | 步骤 | 说明 |
|------|------|------|
| 1 | `bootstrapModule()` | Entity + skinned/render-related ECS systems |
| 2 | `registerNetworkSubSystem()` | 联网 Play |
| 3 | `RendererSubSystem` | GPU / composite |
| 4 | `ScriptSubSystem` | Logia + 热更由 Editor 侧另开 |

Editor 的 `DeviceManager` 由 `EditorApp` **成员**持有（非 DeviceSubSystem），再桥到 Script。  
`EditorPlayRuntime` 可能在进 Play 时再次确保 Entity/Network/Renderer；以不重复破坏既有会话为前提，逐步收拢到同一 helper。

### 2.3 Server（未来）

| 步骤 | 说明 |
|------|------|
| `bootstrapEntityCore` + Script（可选） | 无窗口 |
| **跳过** Device / Audio / Renderer | 与 AYAudio/design 一致 |

---

## 3. `IEngineHost`（Step 1 表面）

```cpp
ayt::app::IEngineHost& host = ayt::app::defaultEngineHost();
// 或 App 运行期间：
ayt::app::IEngineHost* h = ayt::app::currentEngineHost();

h->gameLoop();
h->eventBus();
h->findSubSystem("ayt.script.runtime");
```

| API | Step 1 | 以后 |
|-----|--------|------|
| `gameLoop()` / `eventBus()` / `findSubSystem` | ✅ | 保留 |
| `resources()` / `physics()` / `audio()` | ❌ 仍用各模块 API / 单例 | Host v2 窄接口 |
| Plugin 扩展点表 | ❌ 文档占位 | Plugin v1 |

`ApplicationImpl::run` / `EditorApp::run` 应在主体生命周期内 `EngineHostScope`（或等价）设置 `currentEngineHost`。

---

## 4. CR 纪律（现在）

1. **新 App / demo 默认注册**优先调用 `registerDefaultClientModules` / `registerDefaultEditorModules`，避免复制粘贴三行 bootstrap。  
2. **新代码取 GameLoop / EventBus / 已注册 SubSystem**：优先 `currentEngineHost()`（非空时），过渡期允许直接 `::instance()`。  
3. **不要**在业务代码里假设「Physics 一定是某单例」——等 Host v2 再收。  
4. 改装配顺序：先改本文件表格 + helper，再改 App。  
5. Sim/Present 钩子纪律见 `AYGameLoop/docs/sim-present-time.md`。

---

## 5. 以后（不要现在做）

- Host v2：`resources()` / `physics()` / `audio()` 窄接口 + 注册表  
- Plugin：tick / 菜单 / loader / Editor 面板扩展点  
- 多 Host（测试、多世界）  
- 与 Scene / Save / `simTick` 事件对齐  

---

## 6. 一句话

**App 选壳 → Host 装配默认模块并露出查找口 → GameLoop 转起来；玩家无感，开发者少接线。**
