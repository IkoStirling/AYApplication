# Host Service 编写手册 — 面向新模块作者

**Status:** AYApplication v0.8 · 互补 [`engine-host.md`](engine-host.md)
**Owner:** AYApplication / 架构组
**Related:** [`../design.md`](../design.md) · [`engine-host.md`](engine-host.md) · [`../../../../AYDocs/ARCHITECTURE.md`](../../../../AYDocs/ARCHITECTURE.md) §3 / §7 · [`../../../../AYDocs/adr/`](../../../../AYDocs/adr/)

[`engine-host.md`](engine-host.md) 是面向**游戏/脚本代码**的服务消费手册。本文件是面向**写新模块**的服务**生产**手册——什么时候 expose、怎么 expose、和谁配合。

---

## 1. 三个抽象的角色

| 抽象 | 生命周期 | 接口 | 由谁提供 |
|---|---|---|---|
| `IModule` | 启动期单次 | `registerTypes / install / shutdown` | module 仓 |
| `IEngineHost` | 进程级 Meyers 单例 | `provideService / findService / findSubSystem` | `AYApplication` |
| `ISubSystem` | 进程级(GameLoop 持有) | `tick / fixedUpdate / initialize / shutdown` | `AYGameLoop` |

**Host 是 module 跟 SubSystem 之间的桥**:module 用 `installSubSystem` 把 SubSystem 装进 GameLoop;Host 用 `provideService` 把 SubSystem 的 facade 暴露给其他 module / 游戏代码。

---

## 2. 何时需要 expose 服务

**expose 一个 facade 当且仅当**:

- 至少 2 个独立消费者(module / game / editor)需要拿到它
- 或者消费者无法通过 `findSubSystem(name)` 拿到(典型:SubSystem 内部深层 manager,例如 `PhysicsManager`)

**反例**:

- ❌ 单 module 自己用 → 直接持有 `unique_ptr`,不需要 publish
- ❌ 已经有具名 API(`host->physics()`)→ 不必再发键服务
- ❌ 一次性查询(Editor 局部 inspector)→ Editor 自己 `dynamic_cast` 或加 editor-only accessor

---

## 3. 服务 key 命名

### 3.1 内置引擎服务

格式:`ayt.<module>.<Type>`,全小写,点分隔。

```cpp
inline constexpr const char* kHostServicePhysics = "ayt.physics.PhysicsManager";
inline constexpr const char* kHostServiceAudio    = "ayt.audio.AudioEngine";
inline constexpr const char* kHostServiceResources = "ayt.resource.ResourceManager";
inline constexpr const char* kHostServiceRuntimeSceneLoader = "ayt.app.RuntimeSceneLoader";
```

### 3.2 Module-context 暴露给 module 的 key

格式同 §3.1,但语义是 `EngineModuleContext::findService` 回答:

```cpp
inline constexpr std::string_view kSubSystemModuleService =
    "ayt.game.SubSystemModuleService";
inline constexpr std::string_view kComponentRegistryModuleService =
    "ayt.entity.ComponentRegistry";
```

这两类 key **不要混用**——host service key 用于跨进程查询,module-context key 用于 module 内装配辅助。

### 3.3 项目自有服务

格式:`game.<name>` / `<studio>.<name>`,至少 3 段:

```cpp
inline constexpr const char* kGameInventory = "game.demo.inventory";
inline constexpr const char* kStudioSaveSystem = "studio.alphabet.save";
```

项目 key **不**写到 `AYApplication/IEngineHost.h`,在游戏项目自己的头文件里 `constexpr`。

### 3.4 禁忌

- ❌ 拼字符串:`provide("foo", x)`(编译期不会查重)
- ❌ `std::format`:`provide(std::format("ayt.{}.X", name), x)`(运行时拼、跨编译单元容易撞)
- ❌ 不稳定路径:`provide("physics_" + version, x)`
- ❌ 短字符串:`provide("pm", x)`(命名空间太小)

---

## 4. 发布服务的两条路径

### 4.1 在 `bindBuiltinHostServices` 里发(推荐)

适用于**已知稳定服务**:写入 [`src/AYEngineHost.cpp`](../src/AYEngineHost.cpp):

```cpp
// 伪代码 — 实际参见现有 physics / audio / resources 分支
void bindBuiltinHostServices(IEngineHost& host) {
    // 1) ResourceManager 单例,几乎总是存在
    if (auto* rm = ResourceManager::instance()) {
        host.provide(kHostServiceResources, rm);
    }

    // 2) PhysicsManager:从 SubSystem 拿 manager()
    if (auto* sub = PhysicsSubSystem::findRegistered()) {
        host.provide(kHostServicePhysics, sub->manager());
        if (auto* query = sub->manager()->query()) {
            host.provide(kHostServicePhysicsQuery, query);
        }
    }

    // 3) AudioEngine:从 SubSystem 拿 engine()
    if (auto* sub = AudioSubSystem::findRegistered()) {
        host.provide(kHostServiceAudio, sub->engine());
    }

    // 4) SceneManager(Meyers singleton,deprecated facade)
    if (auto* sm = SceneManager::instance()) {
        host.provide(kHostServiceScenes, sm);
    }

    // 5) RuntimeSceneLoader(来自 AYApplication.RuntimeSceneLoader 模块)
    if (auto* ldr = findRegisteredRuntimeSceneLoader()) {
        host.provide(kHostServiceRuntimeSceneLoader, ldr);
    }
}
```

**调用时机**:`runtime.start()` 之后、`gameLoop.run()` 之前。**严格**这时——早了 SubSystem 没 initialize,晚了 SubSystem 已经在跑 phase。

### 4.2 在 module 的 install hook 里发

适用于**模块自带**(不是从 SubSystem 拿)的服务。

```cpp
// AYRuntime/AYApplication/src/RuntimeSceneLoaderModule.cpp(概念示例)
ayt::module::ModuleResult RuntimeSceneLoaderModule::install(
    ayt::module::IModuleContext& ctx)
{
    auto& appCtx = static_cast<ayt::app::EngineModuleContext&>(ctx);

    // 1) 构造子系统
    auto sub = std::make_unique<RuntimeSceneLoaderSubSystem>(
        appCtx.host().scenes(), appCtx.host().eventBus(), _config);

    // 2) 装到 GameLoop
    if (!ctx.installSubSystem(std::move(sub))) {
        return ayt::module::ModuleResult::failure(
            ayt::module::ModuleErrorCode::InstallationFailed,
            "RuntimeSceneLoader install failed");
    }

    // 3) 不要在这里直接 provide — 等 bindBuiltinHostServices
    return ayt::module::ModuleResult::success();
}
```

**约束**:

- module install hook **不**直接调 `host.provide`,留给 `bindBuiltinHostServices` 统一发
- 例外:模块必须提供**非 SubSystem**的 facade(如 `AYApplication.RuntimeSceneLoader` 的 `IRuntimeSceneLoader*` 是它自有的 facade,SubSystem 只是实现细节)→ `install` hook 里 provide,但要先把 facade 准备好

---

## 5. 消费服务的四条路径

| 路径 | 何时 | noexcept | 类型 |
|---|---|---|---|
| `host.service<T>(key)` | 游戏代码 / 调试 | 否 | 类型安全 |
| `host.findService(key)` | 通用裸指针查询 | 否 | `void*` |
| `ctx.findService(key)` | module 内(IModuleContext) | **是** | `void*` |
| `ctx.findSubSystem(name)` | module 内找 SubSystem | **是** | `ISubSystem*` |
| `host.findSubSystem(name)` | 通用找 SubSystem | 否 | `ISubSystem*` |

**rule of thumb**:

- module 代码用 `ctx.findService`(noexcept)
- 游戏 / 脚本用 `host.service<T>`(允许抛、可调试)
- 跨 module 找对方 manager → `host.findService` + `static_cast`

**消费写法**:

```cpp
// 推荐(module 内,noexcept)
if (auto* phys = ctx.findServiceAs<ayt::physics::PhysicsManager>(
        ayt::app::kHostServicePhysics)) {
    phys->step(dt);
}

// 推荐(游戏代码,可抛)
if (auto* phys = host.service<ayt::physics::PhysicsManager>(
        ayt::app::kHostServicePhysics)) {
    phys->step(dt);
}

// 反例 — 永远 nullptr
if (auto* x = host.service<XManager>("missing.key")) { ... }
```

---

## 6. 生命周期保证

### 6.1 不取所有权

Host **不**拥有 service 指针:

```cpp
// ❌ 反例
class IEngineHost {
    virtual void provide(std::string_view key, std::unique_ptr<void> svc) = 0;
};

// ✅ 正例
class IEngineHost {
    virtual void provide(std::string_view key, void* svc) noexcept = 0;
};
```

provider 自己保证生命周期:典型 `unique_ptr` 由 SubSystem 持有,host 仅存裸指针。
生产入口用 `EngineRuntimeScope` 对内置服务、任务 hook、Scene 选择与活动 World
做成组快照和恢复；scope 必须先于模块安装创建，并晚于模块关闭释放。

### 6.2 清理协议

**SubSystem 析构前**必须清掉自己 provide 过的所有 key:

```cpp
// XSubSystem.cpp
XSubSystem::~XSubSystem() {
    if (auto* host = currentEngineHost()) {
        host.provide(kHostServiceXFacade, nullptr);
    }
    // ... 自己的资源清理
}
```

兼容路径若绕过 `EngineRuntimeScope`，必须在 shutdown 时清掉对应 key：

```cpp
host.provide(kHostServicePhysics, nullptr);
host.provide(kHostServicePhysicsQuery, nullptr);
// ...
```

**违约**:野指针 + 任何残留消费者 → 段错误 / UB。

### 6.3 shutdown 顺序

```
游戏退出
    → gameLoop.run() 退出
    → EngineModuleRuntime::shutdown()  ← 逆序调 module.shutdown
        → module.shutdown 调 uninstallSubSystem (noexcept)
            → GameLoop unregister SubSystem
            → ~SubSystem() 跑 → 清 provide 表
    → EngineRuntimeScope::reset()  ← 恢复服务、hook、Scene 与 World
    → ~DefaultEngineHost (Meyers 单例逆序析构)
    → 全局 statics 析构
```

**SubSystem 的析构**发生在 `EngineModuleRuntime::shutdown()` 内,host 析构之前。如果 SubSystem 没清 provide,host 表里就留野指针。

---

## 7. 错误语义对照

| 场景 | host 端 | ctx 端 |
|---|---|---|
| `findService` 不存在 | 抛 `std::out_of_range` / 返回 nullptr(看实现) | **永远返回 nullptr**(noexcept 边界吞掉异常) |
| `findSubSystem` 不存在 | 抛 / 返回 nullptr | **永远返回 nullptr** |
| `installSubSystem` 已存在同名 | GameLoop 行为(可能拒绝) | **`installSubSystem` 返回 false** |
| 服务对象已析构但 host 未清 | UB | UB |

**Module 端永远防御性写**:

```cpp
if (auto* svc = ctx.findServiceAs<X>(key)) {
    // ... use svc
} else {
    AY_LOG_WARN("[ModuleX] service not available yet");
}
```

---

## 8. 测试多 host

`EngineHostScope` 是 RAII,可以临时换掉 `currentEngineHost`:

```cpp
TEST(MyModule, WithoutHost) {
    ayt::app::EngineHostScope scope(host);  // 内部 push,析构 pop
    EXPECT_NE(ayt::app::currentEngineHost(), nullptr);
    // ...
}
```

测试多 host 同时存在时,**每个测试用独立 `EngineHostScope`**,不要跨测试共享。

---

## 9. 新建服务的清单(CR 用)

每加一个新 host service,**必须**做下列七项:

```text
□ 选稳定 key(§3),inline constexpr 字符串字面量
□ 在 AYApplication/IEngineHost.h 顶部新增 constexpr(若是引擎内置)
□ 提供者(SubSystem / module)在适当位置调 host.provide
□ 消费者文档(本文件 §5 + engine-host.md §4.2)
□ DefaultEngineHost.cpp / AYEngineHost.cpp 清理逻辑(若 SubSystem 自己 provide)
□ bindBuiltinHostServices 的反向 publish(若是 SubSystem 派生)
□ 单元测试(本文件 §10)
□ 同步更新:
    - AYDocs/ARCHITECTURE.md §3.3 / §7 表
    - AYDocs/adr/(若涉及架构变更)
```

漏一项 → CR 时打回。

---

## 10. 单元测试模板

```cpp
#include <gtest/gtest.h>
#include <AYApplication/IEngineHost.h>
#include <AYApplication/DefaultEngineHost.h>

TEST(HostServiceGuide, ProvideFindRoundtrip) {
    ayt::app::DefaultEngineHost host;
    ayt::app::EngineHostScope scope(host);

    int dummy = 42;
    host.provide("test.dummy", &dummy);

    EXPECT_EQ(host.service<int>("test.dummy"), &dummy);
    EXPECT_EQ(host.findService("test.dummy"), &dummy);
}

TEST(HostServiceGuide, MissingKeyReturnsNullOrThrows) {
    ayt::app::DefaultEngineHost host;
    ayt::app::EngineHostScope scope(host);

    // 文档约定:host 端可抛,ctx 端 noexcept
    EXPECT_THROW(host.findService("missing.key"), std::exception);
    // ctx 端(用 EngineModuleContext)永远不抛,返回 nullptr
}
```

**测试要点**:

- provide → find 一致
- 缺失 key → 文档约定行为(host 抛 / ctx nullptr)
- cleanup:析构 host 后 provider 对象不能再被引用
- 多 host 隔离:每个 scope 独立

---

## 11. 陷阱与反模式

### 11.1 SubSystem 内存泄漏

```cpp
// ❌ SubSystem 不知道自己被谁接管
class BadSubSystem : public ISubSystem {
    std::unique_ptr<PhysicsManager> _mgr;
};

// ~ISubSystem 默认是 = delete 但 BadSubSystem 没声明 virtual dtor
```

修复:每个 SubSystem 类加 `~ISubSystem() override = default;`。

### 11.2 provide 栈指针

```cpp
void bindBuiltinHostServices(IEngineHost& host) {
    ayt::scene::SceneManager scenes;  // 栈上!
    host.provide(kHostServiceScenes, &scenes);
}   // ← scenes 析构,host 表里野指针
```

修复:栈对象只在 host 析构之前活着 — 用 Meyers singleton / static / heap unique_ptr。

### 11.3 provide 临时对象

```cpp
host.provide("foo", new XManager());  // 谁 delete?
```

修复:永远先构造 `unique_ptr`,然后 `host.provide(key, ptr.get())`,ptr 由 SubSystem / module 持有。

### 11.4 跨 phase 调 service

```cpp
class GameLogicSubSystem : public ISubSystem {
    void tick(const FrameContext& ctx) override {
        // ❌ 在 tick 里 provide / unregister
        if (auto* h = currentEngineHost()) {
            h->provide("some.transient", this);
        }
    }
};
```

修复:服务 publish 只在 `install / bindBuiltinHostServices` / `shutdown`,**不**在 tick 里。

### 11.5 用 string key 找业务 API

```cpp
// ❌ 新代码不应再用
auto* phys = ayt::game::GameLoop::instance().findSubSystem("Physics");
phys->manager()->step(dt);
```

修复:用 `host.service<PhysicsManager>(kHostServicePhysics)` 或 `host->physics()`。

### 11.6 多个 `EngineHostScope` 嵌套

```cpp
{
    EngineHostScope a(hostA);  // push hostA
    {
        EngineHostScope b(hostB);  // push hostB
        // current = hostB
    }   // pop → current = hostA
}   // pop → current = default
```

这没问题,但**记得每个 scope 都对应自己的 host**——不要共享同一 host 给多个 scope,否则 pop 顺序乱了 current 会乱跳。

---

## 12. 调试技巧

| 现象 | 排查 |
|---|---|
| `findService(key)` 总是 nullptr | key 是否完全一致(字符串字面量比较);host 是否被 `EngineHostScope` 换过;provide 顺序(晚了 SubSystem 已 destroy) |
| `bindBuiltinHostServices` 里看到 SubSystem 已 destroy | SubSystem 的 initialize 失败,`findRegistered` 返回 nullptr;查 module log |
| 进程退出段错误 | SubSystem 析构时没清 provide 表;查 ~XSubSystem 是否有清 host |
| `host.service<T>(key)` 返回错的指针 | key 撞名;查 `kHostServiceXxx` 全集 |
| 编辑器刷新场景后 host 不一致 | EditorApp 多 viewport 各持有 host;确认 EditorApp::run 内 scope 嵌套 |

---

## 13. 阅读路径

| 想了解 | 读 |
|---|---|
| 模块框架本身 | [`../../../../AYDocs/ARCHITECTURE.md`](../../../../AYDocs/ARCHITECTURE.md) §3 · [AYModule/docs/architecture/module-system.md](../../../../AYModule/docs/architecture/module-system.md) |
| AYApplication 装配总览 | [`engine-host.md`](engine-host.md) · [`../design.md`](../design.md) |
| SubSystem 编程 | [`../../AYGameLoop/design.md`](../../AYGameLoop/design.md) · [`../../AYGameLoop/docs/frame-stage-model.md`](../../AYGameLoop/docs/frame-stage-model.md) |
| 架构决策 | [`../../../../AYDocs/adr/`](../../../../AYDocs/adr/) |
| 引擎默认模块入图 | [`../src/AYRegisterDefaultModules.cpp`](../src/AYRegisterDefaultModules.cpp) |

---

*本文件随新服务上线同步维护。变更 key / 新增 facade → 同步更新 [`engine-host.md` §4.2](engine-host.md) 与 [`ARCHITECTURE.md` §3.3](../../../../AYDocs/ARCHITECTURE.md)。*
