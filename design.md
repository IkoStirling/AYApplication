# AYApplication Design

> **输入子系统（2026-07-11）**：Client 侧仅注册 **`DeviceSubSystem`**（`AYDevice`：窗口 + 输入轮询 + `InputMapping`）。**无** `InputSubSystem` / **`AYInput`** 模块 — 见 [`AYDevice/design.md` §1.3](../AYDevice/design.md)。

> **引擎外壳（2026-08-02）**：装配表 + `IEngineHost` 服务面（`resources`/`physics`/`audio` + 可扩展键）— 见 [`docs/engine-host.md`](docs/engine-host.md)。新单例必须按该文档 §4 登记，禁止只扩散 `::instance()`。

## 1. 概述

AYApplication 是 AY Engine 的**应用入口层**，负责：
- 引擎初始化与关闭
- 子系统注册
- 游戏主循环启动
- 平台入口点适配（main / WinMain）
- 命令行参数解析
- 配置加载
- 日志系统初始化
- 异常与信号处理
- 多应用类型支持（Game / Editor / Server）

### 1.1 设计目标

- **跨平台入口统一**：不同平台统一 `main()` 或 `WinMain()` 调用方式
- **模块化注册**：游戏项目注册自己的子系统
- **生命周期管理**：init → run → shutdown
- **配置驱动**：通过命令行和配置文件定制行为
- **生产级错误处理**：异常捕获、断言处理、信号处理
- **日志初始化**：引擎启动时初始化日志系统
- **网络通信**：客户端与服务端通信，支持多种拓扑和协议
- **多应用类型**：Game（游戏）、Editor（编辑器）、Server（服务器）分离
- **构建时控制**：模块引入从代码转向 CMake 层

### 1.2 在引擎中的位置

```
┌─────────────────────────────────────────────────────────────────┐
│                        Game Application                          │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  main() / WinMain()                                              │
│       │                                                          │
│       ▼                                                          │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │               IApplication / AppFactory                  │   │
│  │  (应用层，引擎提供基类)                                    │   │
│  │  - create() 工厂方法                                      │   │
│  │  - registerSubSystems() 子类实现                          │   │
│  │  - run() 启动主循环                                       │   │
│  │  - BuildType 控制子系统加载                               │   │
│  └────────────────────────┬────────────────────────────────┘   │
│                           │                                      │
│       ┌───────────────────┼───────────────────┐                 │
│       │                   │                   │                 │
│       ▼                   ▼                   ▼                 │
│  ┌───────────┐  ┌───────────────┐  ┌───────────────┐             │
│  │  GameApp  │  │  EditorApp   │  │  ServerApp    │             │
│  │ (游戏)    │  │  (编辑器)    │  │  (服务器)     │             │
│  └───────────┘  └───────────────┘  └───────────────┘             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### 1.3 当前实现状态（2026-09-02）

当前代码已经形成三条互相隔离的应用装配路径：

| 路径 | 目标 | 状态 |
|---|---|---|
| 本地客户端/编辑器 | `AYApplication` | 已实现 Host、默认模块、运行时场景加载和生命周期事件桥接 |
| 在线客户端 | `AYOnlineApplication` | 已实现 Backend-aware Online Flow、内容映射、加载失败恢复和会话场景切换 |
| Dedicated 权威端 | `AYDedicatedApplication` + `AYApplication_DedicatedServer` | 已实现 allocation、可信内容解析、真实 `Scene/World`、逐玩家准入、tick、回收和 drain |

在线内容映射被单独拆为 `AYOnlineContent`，客户端桥接与 Dedicated 桥接不相互
依赖。网络协议、P2P、会话后端和 Dedicated control plane 仍由 AYNetwork
拥有；本模块不复制这些职责。

Dedicated 运行时是 headless 的：不会注册窗口、Renderer、UI 或 Audio。
`AYScene -> AYEntityCore` 不再经过完整 AYEntity facade；Application 仅在对应
CMake target 存在时编译功能分支并链接具体 integration。`windows-headless-debug`
提供最终可执行链接与启动冒烟目标来守住该边界。

---

## 2. 核心接口

### 2.1 BuildType - 构建类型

```cpp
enum class BuildType : uint8_t {
    Game,    // 游戏客户端（渲染+音频+输入）
    Editor,  // 编辑器（游戏+编辑器工具）
    Server,  // 服务器（无渲染/音频/输入）
};

// 获取当前构建类型（编译时确定）
constexpr BuildType getBuildType() {
    #if defined(AY_BUILD_TARGET_SERVER)
        return BuildType::Server;
    #elif defined(AY_BUILD_TARGET_EDITOR)
        return BuildType::Editor;
    #else
        return BuildType::Game;
    #endif
}
```

### 2.2 GameDesc

```cpp
struct GameDesc {
    const char* name = "Untitled";
    uint32_t width = 1280;
    uint32_t height = 720;
    float targetFPS = 60.0f;
    bool enableRenderThread = true;
    bool enableDebugConsole = false;        // 是否显示调试控制台
    const char* logLevel = "info";         // 日志级别: trace, debug, info, warn, error
    const char* configFile = "";           // 配置文件路径
    const char* assetRoot = "./assets";    // 资源根目录
    const char* userDataPath = "";         // 用户数据目录
    BuildType buildType = getBuildType(); // 构建类型
};
```

### 2.3 AppCommandLine

```cpp
struct AppCommandLine {
    std::vector<std::string> args;         // 原始参数列表

    // 常用选项
    bool help = false;                      // -help, --help
    bool version = false;                   // -version, --version
    bool debug = false;                     // -debug
    bool noAudio = false;                   // -no-audio
    uint32_t width = 0;                     // -width <n>
    uint32_t height = 0;                    // -height <n>
    float fps = 0.0f;                       // -fps <n>
    std::string logLevel;                   // -log <level>
    std::string configFile;                 // -config <path>
    std::string assetRoot;                  // -asset-root <path>
    std::string userDataPath;               // -user-data <path>

    // 未知参数（保留给子系统）
    std::vector<std::string> unknownArgs;
};
```

### 2.4 IApplication

```cpp
class IApplication {
public:
    virtual ~IApplication() = default;

    // 工厂方法
    static std::unique_ptr<IApplication> create(const GameDesc& desc);
    static std::unique_ptr<IApplication> create(const GameDesc& desc, const AppCommandLine& cmdLine);

    // 子类必须实现：注册子系统
    virtual void registerSubSystems() = 0;

    // 可选覆盖：生命周期钩子
    virtual void onInit() {}
    virtual void onPostUpdate(float deltaTime) {}
    virtual void onPreShutdown() {}
    virtual void onShutdown() {}

    // 启动
    virtual void run() = 0;

    // 查询
    virtual const GameDesc& getDesc() const = 0;
    virtual GameLoop& getGameLoop() = 0;
    virtual const AppCommandLine& getCommandLine() const = 0;

    // 版本信息
    virtual const char* getVersion() const = 0;
    virtual const char* getEngineVersion() const = 0;
};
```

### 2.5 Application 入口函数

```cpp
// 跨平台 main 宏（放在头文件中）
#if defined(_WIN32)
#define AY_MAIN_DECLARE(appClass) \
    int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int argc, char* argv[]) { \
        ayt::app::GameDesc desc; \
        desc.name = #appClass; \
        auto app = ayt::app::IApplication::create(desc); \
        app->run(); \
        return 0; \
    }
#else
#define AY_MAIN_DECLARE(appClass) \
    int main(int argc, char* argv[]) { \
        ayt::app::GameDesc desc; \
        desc.name = #appClass; \
        auto app = ayt::app::IApplication::create(desc); \
        app->run(); \
        return 0; \
    }
#endif

// 高级入口宏（带命令行解析、配置加载、异常处理）
#if defined(_WIN32)
#define AY_MAIN_DECLARE_EX(appClass) \
    int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int argc, char* argv[]) { \
        return ayt::app::IApplication::runEx<appClass>(argc, argv); \
    }
#else
#define AY_MAIN_DECLARE_EX(appClass) \
    int main(int argc, char* argv[]) { \
        return ayt::app::IApplication::runEx<appClass>(argc, argv); \
    }
#endif
```

---

## 3. 多应用类型

### 3.1 子系统分类

```cpp
// 子系统按构建类型分类
enum class SubSystemType : uint8_t {
    Core,     // 始终加载（FrameManager, TaskScheduler）
    Client,  // 仅客户端（渲染、音频、输入）
    Server,  // 仅服务器（AI、网络服务器）
    Shared,  // 两者都需要（物理、动画、资源）
    Editor,  // 仅编辑器（编辑器工具、场景编辑器）
};

// 子系统描述符扩展
struct SubSystemDescriptor {
    const char* name;
    std::vector<const char*> dependencies;
    int32_t basePriority;

    SubSystemType type = SubSystemType::Shared;  // 默认 Shared

    // 时间类型
    enum class TimeType : uint8_t {
        Scaled,     // 受 timeScale 影响
        Unscaled,   // 不受 timeScale 影响
        Real        // 真实时间
    };
    TimeType timeType = TimeType::Scaled;
};
```

### 3.2 各构建类型的子系统

| 子系统 | Game | Editor | Server | 说明 |
|--------|------|--------|--------|------|
| FrameManager | ✅ | ✅ | ✅ | Core - 始终需要 |
| TaskScheduler | ✅ | ✅ | ✅ | Core |
| Renderer | ✅ | ✅ | ❌ | Client |
| Audio | ✅ | ✅ | ❌ | Client |
| Device | ✅ | ✅ | ❌ | Client — **窗口 + 输入轮询 + Action 映射**（`DeviceSubSystem`）；**无**独立 `InputSubSystem` / `AYInput` 模块（见 [`AYDevice/design.md` §1.3](../AYDevice/design.md)） |
| Physics | ✅ | ✅ | ✅ | Shared |
| Animation | ✅ | ✅ | ✅ | Shared |
| Resource | ✅ | ✅ | ✅ | Shared |
| AI | ✅ | ✅ | ✅ | Server |
| Network | ✅ | ✅ | ✅ | Server |
| EditorTools | ❌ | ✅ | ❌ | Editor |
| SceneEditor | ❌ | ✅ | ❌ | Editor |

### 3.3 子系统注册过滤

```cpp
// 方式 A：代码中根据 BuildType 过滤
class GameApplication : public IApplication {
public:
    void registerSubSystems() override {
        // Core - 始终注册
        GameLoop::instance().registerSubSystem<FrameManager>();
        GameLoop::instance().registerSubSystem<TaskScheduler>();

        // Shared - 始终注册
        GameLoop::instance().registerSubSystem<PhysicsSubSystem>();
        GameLoop::instance().registerSubSystem<ResourceSubSystem>();

        // Client only
    #if !defined(AY_BUILD_TARGET_SERVER)
        GameLoop::instance().registerSubSystem<RendererSubSystem>();
        GameLoop::instance().registerSubSystem<AudioSubSystem>();
        // Runtime audio engine: see AYRuntime/AYAudio/design.md (miniaudio; not SDL audio)
        GameLoop::instance().registerSubSystem<DeviceSubSystem>();
        // Window + input poll + InputMapping — AYDevice/design.md §1.3 (no separate InputSubSystem / AYInput)
    #endif

        // Server only
    #if !defined(AY_BUILD_TARGET_GAME)
        GameLoop::instance().registerSubSystem<AISubSystem>();
    #endif

        // Editor only
    #if defined(AY_BUILD_TARGET_EDITOR)
        GameLoop::instance().registerSubSystem<EditorToolsSubSystem>();
    #endif
    }
};

// 方式 B：SubSystemRegistry 按类型过滤
class SubSystemRegistry {
public:
    void registerSubSystem(ISubSystem* system) {
        auto type = system->getDescriptor().type;
        auto buildType = getBuildType();

        // 检查子系统是否适合当前构建类型
        if (!isCompatible(type, buildType)) {
            return;  // 跳过注册
        }
        // ... 正常注册逻辑
    }

private:
    bool isCompatible(SubSystemType type, BuildType build) {
        switch (type) {
            case SubSystemType::Core:    return true;
            case SubSystemType::Shared:  return true;
            case SubSystemType::Client:  return build != BuildType::Server;
            case SubSystemType::Server:  return build != BuildType::Game;
            case SubSystemType::Editor:  return build == BuildType::Editor;
        }
        return true;
    }
};
```

---

## 4. 命令行解析

### 4.1 标准选项

| 选项 | 说明 | 示例 |
|------|------|------|
| `-help`, `--help` | 显示帮助信息 | `--help` |
| `-version`, `--version` | 显示版本信息 | `--version` |
| `-debug` | 启用调试模式 | `-debug` |
| `-width <n>` | 设置窗口宽度 | `-width 1920` |
| `-height <n>` | 设置窗口高度 | `-height 1080` |
| `-fps <n>` | 设置目标帧率 | `-fps 144` |
| `-log <level>` | 设置日志级别 | `-log debug` |
| `-config <path>` | 指定配置文件 | `-config user_settings.ini` |
| `-asset-root <path>` | 指定资源根目录 | `-asset-root /data/assets` |
| `-user-data <path>` | 指定用户数据目录 | `-user-data /tmp/mygame` |
| `-no-audio` | 禁用音频系统 | `-no-audio` |

### 4.2 解析实现

```cpp
class AppCommandLineParser {
public:
    static AppCommandLine parse(int argc, char* argv[]) {
        AppCommandLine cmd;
        cmd.args.assign(argv, argv + argc);

        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];

            if (arg == "-help" || arg == "--help") cmd.help = true;
            else if (arg == "-version" || arg == "--version") cmd.version = true;
            else if (arg == "-debug") cmd.debug = true;
            else if (arg == "-no-audio") cmd.noAudio = true;
            else if (arg == "-width" && i + 1 < argc) cmd.width = std::stoul(argv[++i]);
            else if (arg == "-height" && i + 1 < argc) cmd.height = std::stoul(argv[++i]);
            else if (arg == "-fps" && i + 1 < argc) cmd.fps = std::stof(argv[++i]);
            else if (arg == "-log" && i + 1 < argc) cmd.logLevel = argv[++i];
            else if (arg == "-config" && i + 1 < argc) cmd.configFile = argv[++i];
            else if (arg == "-asset-root" && i + 1 < argc) cmd.assetRoot = argv[++i];
            else if (arg == "-user-data" && i + 1 < argc) cmd.userDataPath = argv[++i];
            else cmd.unknownArgs.push_back(argv[i]);
        }

        return cmd;
    }
};
```

---

## 5. 配置系统

### 5.1 ConfigFile 格式 (INI)

```ini
[Application]
Name=MyGame
Width=1920
Height=1080
TargetFPS=60

[Render]
EnableRenderThread=true
AntiAliasing=MSAA4x
ShadowQuality=High

[Audio]
EnableAudio=true
MasterVolume=0.8

[Paths]
AssetRoot=./assets
UserData=/tmp/mygame
```

### 5.2 配置加载

```cpp
class ConfigFile {
public:
    bool load(const char* path);
    bool save(const char* path);

    // 获取值
    std::string getString(const char* section, const char* key, const char* defaultValue = "") const;
    int getInt(const char* section, const char* key, int defaultValue = 0) const;
    float getFloat(const char* section, const char* key, float defaultValue = 0.0f) const;
    bool getBool(const char* section, const char* key, bool defaultValue = false) const;

private:
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> _data;
};
```

---

## 6. 错误处理

### 6.1 异常类型

```cpp
class AppException : public std::exception {
public:
    enum class Code {
        Unknown,
        InitFailure,
        ConfigError,
        SubSystemInitFailed,
        RenderInitFailed,
        AssetLoadFailed,
    };

    AppException(Code code, const char* message);
    AppException(Code code, const std::string& message);
    const char* what() const override;
    Code getCode() const { return _code; }

private:
    Code _code;
    std::string _message;
};

// 便捷宏
#define AY_THROW(code, msg) throw ayt::app::AppException(ayt::app::AppException::Code::code, msg)
```

### 6.2 信号处理

```cpp
class SignalHandler {
public:
    static void install();
    static void uninstall();

    // 信号回调
    using Handler = std::function<void(int)>;
    static void onSignal(Handler handler);

private:
    static void handle(int signum);
    static Handler _handler;
    static bool _installed;
};
```

### 6.3 全局异常捕获

```cpp
void IApplication::run() {
    try {
        // 初始化
        _gameLoop.initialize();
        onInit();

        // 主循环
        _gameLoop.run();
    }
    catch (const AppException& e) {
        AY_LOG(ERROR, "Application error: %s (code: %d)", e.what(), (int)e.getCode());
        onFatalError(e);
    }
    catch (const std::exception& e) {
        AY_LOG(ERROR, "Unhandled exception: %s", e.what());
        onFatalError(e);
    }
    catch (...) {
        AY_LOG(ERROR, "Unknown exception caught");
        onFatalError(std::current_exception());
    }
    finally {
        onPreShutdown();
        _gameLoop.shutdown();
        onShutdown();
    }
}
```

---

## 7. 日志初始化

### 7.1 日志初始化时机

日志系统应在 Application 最早阶段初始化，确保所有后续代码都能使用日志：

```cpp
void IApplication::run() {
    // 最早期：日志必须最早初始化
    ayt::log::initialize(getDesc().logLevel);

    AY_LOG(INFO, "Starting %s v%s", getDesc().name, getVersion());
    AY_LOG(INFO, "Build: %s", getBuildTypeName());

    try {
        // 初始化
        _gameLoop.initialize();
        onInit();

        // 主循环
        _gameLoop.run();
    }
    catch (...) {
        // 异常处理
    }
    finally {
        onPreShutdown();
        _gameLoop.shutdown();
        onShutdown();
        ayt::log::shutdown();  // 最后关闭日志
    }
}
```

### 7.2 日志配置

```cpp
struct GameDesc {
    // ... 其他字段
    const char* logLevel = "info";           // 日志级别: trace, debug, info, warn, error
    const char* logFile = "";                 // 日志文件路径，为空则输出到控制台
    bool logToFile = true;                    // 是否输出到文件
    size_t maxLogFileSize = 10 * 1024 * 1024;  // 单个日志文件最大大小
    uint32_t maxLogFiles = 5;                 // 保留的旧日志文件数量
};
```

### 7.3 日志级别控制

```cpp
namespace ayt::log {

// 初始化日志系统
void initialize(const char* level, const char* filePath = nullptr);

// 设置全局级别
void setLevel(LogLevel level);

// 获取当前级别
LogLevel getLevel();

// 带格式的日志
void log(LogLevel level, const char* file, int line, const char* fmt, ...);

} // namespace ayt::log

// 使用示例
AY_LOG(INFO, "Application started");
AY_LOG(WARN, "Config file not found, using defaults");
AY_LOG(ERROR, "Failed to initialize renderer: %s", errorMsg);
```

---

## 8. 通信系统

### 8.1 通信子系统架构

客户端/服务端通信属于 **NetworkSubSystem**，但由 Application 层统一管理生命周期和配置：

```
┌─────────────────────────────────────────────────────────────────┐
│                     Application Layer                            │
│  ┌─────────────────────────────────────────────────────────────┐ │
│  │ IApplication                                               │ │
│  │  - 统一管理 NetworkSubSystem 生命周期                      │ │
│  │  - 根据 BuildType 决定通信模式                              │ │
│  │  - 提供连接状态回调                                          │ │
│  └────────────────────────────┬────────────────────────────────┘ │
│                               │                                  │
└───────────────────────────────┼──────────────────────────────────┘
                                │
┌───────────────────────────────┼──────────────────────────────────┐
│                    NetworkSubSystem                               │
│  ┌────────────────┬────────────────┬────────────────┐           │
│  │ Transport      │ Protocol       │ Replication    │           │
│  │ (传输层)        │ (协议层)       │ (复制层)       │           │
│  │ TCP/UDP/WebSocket│ RPC/MessagePack│ 状态同步      │           │
│  └────────────────┴────────────────┴────────────────┘           │
└─────────────────────────────────────────────────────────────────┘
```

### 8.2 通信拓扑类型

| 拓扑 | 描述 | 适用场景 | AY 支持 |
|------|------|----------|---------|
| **Dedicated Server** | 专用服务器，客户端只表现 | MMO、竞技游戏 | ✅ Scene-backed 第一版 |
| **Listen Server** | 客户端同时作为权威主机 | 合作游戏、P2P | ✅ AYNetwork 会话层；项目装配待接入 |
| **Peer-to-Peer** | 信令后建立客户端直连 | 合作、房间制游戏 | ✅ 直连/NAT 打洞已实现并完成公网验证 |
| **Relay Server** | 中继服务器转发消息 | 无法打洞的 NAT | 规划；TURN 暂缓 |

### 8.3 NetworkDesc 配置

```cpp
struct NetworkDesc {
    // 连接模式
    enum class Mode : uint8_t {
        Client,      // 纯客户端
        Server,      // 纯服务器
        ListenServer, // 监听服务器（客户端兼服务器）
    };
    Mode mode = Mode::Client;

    // 服务器地址（客户端模式）
    const char* serverAddress = "localhost";
    uint16_t serverPort = 8888;

    // 服务器配置（服务器模式）
    uint16_t listenPort = 8888;
    uint32_t maxConnections = 64;

    // 协议选择
    enum class Protocol : uint8_t {
        TCP,
        UDP,
        WebSocket,
    };
    Protocol protocol = Protocol::TCP;

    // 复制配置
    bool enableReplication = true;
    uint32_t replicationRateHz = 20;  // 复制频率

    // 心跳/超时
    uint32_t heartbeatIntervalMs = 1000;
    uint32_t connectionTimeoutMs = 10000;
};
```

### 8.4 通信接口

```cpp
class INetworkSubSystem : public ISubSystem {
public:
    // 连接管理
    virtual void connect(const NetworkDesc& desc) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;

    // 模式查询
    virtual NetworkDesc::Mode getMode() const = 0;

    // 消息发送
    virtual void send(uint16_t channel, const void* data, size_t size) = 0;

    // 消息接收回调
    using MessageHandler = std::function<void(uint16_t channel, const void* data, size_t size)>;
    virtual void onMessage(uint16_t channel, MessageHandler handler) = 0;

    // 连接状态回调
    using ConnectionHandler = std::function<void(bool connected, const char* reason)>;
    virtual void onConnectionChange(ConnectionHandler handler) = 0;

    // 服务器专用
    virtual void setAcceptCallback(std::function<bool(NetConnection*)> callback) = 0;
    virtual void kickConnection(NetConnection* conn, const char* reason) = 0;
    virtual const std::vector<NetConnection*>& getConnections() = 0;
};
```

### 8.5 消息通道

预定义消息通道：

| 通道 | ID | 描述 |
|------|----|------|
| CHANNEL_RELIABLE | 0 | 可靠有序消息（TCP语义） |
| CHANNEL_UNRELIABLE | 1 | 不可靠消息（UDP语义） |
| CHANNEL_FRAGMENTED | 2 | 大数据分片传输 |
| CHANNEL_ACK | 3 | 确认/心跳 |

### 8.6 复制系统

复制系统处理服务端与客户端间的状态同步：

```cpp
// 可复制对象接口
class IReplicable {
public:
    // 获取复制优先级（高优先级先同步）
    virtual float getReplicationPriority() const = 0;

    // 序列化状态
    virtual void replicate(ayt::net::BitStream& stream) = 0;

    // 反序列化（客户端接收）
    virtual void onReplicate(const ayt::net::BitStream& stream) = 0;

    // 获取复制频道
    virtual uint16_t getReplicationChannel() const { return CHANNEL_RELIABLE; }
};

// 复制管理器
class ReplicationManager {
public:
    // 注册可复制对象
    void registerObject(IReplicable* obj, uint32_t netId);

    // 注销
    void unregisterObject(uint32_t netId);

    // 服务端：收集待复制对象并发送
    void replicateTo(NetConnection* conn);

    // 客户端：接收并应用
    void onReceive(NetConnection* conn, BitStream& stream);

private:
    std::unordered_map<uint32_t, IReplicable*> _objects;
};
```

### 8.7 应用层集成

```cpp
// 游戏项目注册网络子系统
class MyGame : public IApplication {
public:
    void registerSubSystems() override {
        // ... 其他子系统

        if (getDesc().buildType != BuildType::Server) {
            GameLoop::instance().registerSubSystem<NetworkSubSystem>();
        }
    }

    void onInit() override {
        auto& net = GameLoop::instance().getNetwork();

        // 设置连接回调
        net.onConnectionChange([this](bool connected, const char* reason) {
            AY_LOG(INFO, "Connection %s: %s", connected ? "established" : "lost", reason);
        });

        // 设置消息处理
        net.onMessage(CHANNEL_RELIABLE, [this](auto, auto data, auto size) {
            parseGameMessage(data, size);
        });

        // 连接到服务器（客户端模式）
        if (getDesc().buildType == BuildType::Game) {
            net.connect({"localhost", 8888});
        }
    }
};
```

### 8.8 协议支持矩阵

| 特性 | TCP | UDP | WebSocket |
|------|-----|-----|-----------|
| 可靠传输 | ✅ | ✅ GNS reliable lane | ✅ |
| 有序 | ✅ | ✅ 按 lane 配置 | ✅ |
| 低延迟 | 一般 | ✅ | 一般 |
| NAT 穿透 | ❌ | ✅ GNS ICE/direct | 信令通道，不承担打洞数据面 |
| 浏览器支持 | ❌ | ❌ | ✅ |
| 大数据分片 | ✅ | ✅ GNS | ✅ |
| 加密 | TLS | GNS 会话加密 | TLS |

### 8.9 在线应用与 Dedicated 权威场景

实际在线装配链如下：

```text
Backend / Session Service
          │ logical content identity + allocation
          ▼
AYNetwork DedicatedServerRuntime
          │ IDedicatedWorldHost
          ▼
AYDedicatedApplication::DedicatedSceneHost
          │ trusted content catalog
          ▼
AYScene::Scene (Play) ── owns ──> AYEntity::World
          │
          ├─ prepareWorld: 游戏模式、脚本、物理、复制、authority spawn
          ├─ playerConnected/playerDisconnected: 玩家与场景实体绑定
          └─ tick/deactivate: 权威模拟与确定性清理
```

安全边界：后端不得提供可直接访问的场景文件路径。`contentId + version`
必须先通过服务器本地可信 catalog 或项目实现的 `IOnlineContentResolver`，然后
才能加载 `.ayscene`。通用 TSV loader 对行数、行长、字段数量和目标普通文件
进行限制，并以临时 catalog 完整解析成功后再替换现有映射。

多世界边界：`DedicatedSceneHostConfig::maximumWorlds` 默认是 1。通用服务器
入口默认将其提高到注册的玩家容量，使后端可以在同一进程放置多个小型比赛；
任何仍调用 `World::instance()` 的游戏系统都必须通过
`AY_DEDICATED_MAX_WORLDS=1` 保持单世界。

---

## 9. 子系统注册

### 7.1 游戏项目实现示例

```cpp
// MyGame.cpp
class MyGame : public IApplication {
public:
    void registerSubSystems() override {
        // 注册引擎子系统
        GameLoop::instance().registerSubSystem<DeviceSubSystem>();
        GameLoop::instance().registerSubSystem<PhysicsSubSystem>();
        GameLoop::instance().registerSubSystem<RendererSubSystem>();
        GameLoop::instance().registerSubSystem<ResourceSubSystem>();
        GameLoop::instance().registerSubSystem<AnimationSubSystem>();

        // 注册游戏子系统
        GameLoop::instance().registerSubSystem<MyGameLogicSubSystem>();
        GameLoop::instance().registerSubSystem<MyAISubSystem>();
    }
};

// 入口点
AY_MAIN_DECLARE_EX(MyGame);
```

### 7.2 简化注册（静态注册宏）

```cpp
// 游戏项目定义自己的子系统
class MyGameLogicSubSystem : public ISubSystem {
public:
    const char* getName() const override { return "MyGameLogic"; }
    const SubSystemDescriptor& getDescriptor() const override {
        static SubSystemDescriptor desc = {
            .name = "MyGameLogic",
            .dependencies = {"Physics"},
            .basePriority = 500
        };
        return desc;
    }

    bool initialize() override;
    void update(float dt) override;
    void fixedUpdate(float dt) override {}
    void shutdown() override;
};

// 自动注册（编译时）
REGISTER_SUBSYSTEM(MyGameLogicSubSystem, {"Physics"}, 500);

// 游戏项目只需声明
AY_MAIN_DECLARE_EX(MyGame);
```

---

## 10. 目录结构

```
AYApplication/
├── README.md
├── design.md
├── CMakeLists.txt
├── docs/
│   ├── engine-host.md
│   └── online-application.md
├── interface/
│   └── AYApplication/                         # Host 稳定接口
├── include/
│   └── AYApplication/                         # 实现辅助接口
├── src/
│   ├── AYApplicationImpl.cpp
│   ├── AYEngineHost.cpp
│   └── AYRuntimeSceneLoader.cpp
├── online/
│   ├── interface/AYOnlineApplication/
│   │   ├── OnlineContent.h
│   │   ├── OnlineApplication.h
│   │   └── DedicatedApplication.h
│   ├── src/
│   │   ├── AYOnlineContent.cpp
│   │   ├── AYOnlineApplication.cpp
│   │   └── AYDedicatedApplication.cpp
│   └── tools/dedicated_server/main.cpp
└── unittest/
    ├── Test_OnlineApplicationBridge.cpp
    ├── Test_OnlineVerticalSlice.cpp
    └── Test_DedicatedOnlineVerticalSlice.cpp
```

---

## 11. 构建系统集成

### 9.1 CMake 构建类型控制

```cmake
# AYRuntime/CMakeLists.txt

# ============================================
# 构建类型选项
# ============================================
set(AY_BUILD_TARGET "game" CACHE STRING "Build target: game|server|editor")
set_property(CACHE AY_BUILD_TARGET PROPERTY STRINGS "game" "server" "editor")

# ============================================
# 根据构建类型排除子系统
# ============================================
set(AY_CLIENT_SUBSYSTEMS "Renderer;Audio;Device")
set(AY_SERVER_SUBSYSTEMS "AI;Network;ServerSpecific")
set(AY_EDITOR_SUBSYSTEMS "EditorTools;SceneEditor;Inspector")

if(AY_BUILD_TARGET STREQUAL "server")
    # Server 构建排除 Client 特有模块
    list(APPEND AY_EXCLUDED_SUBSYSTEMS ${AY_CLIENT_SUBSYSTEMS} ${AY_EDITOR_SUBSYSTEMS})
elseif(AY_BUILD_TARGET STREQUAL "editor")
    # Editor 构建排除 Server 特有模块
    list(APPEND AY_EXCLUDED_SUBSYSTEMS ${AY_SERVER_SUBSYSTEMS})
else()
    # Game 构建排除 Editor 特有模块
    list(APPEND AY_EXCLUDED_SUBSYSTEMS ${AY_EDITOR_SUBSYSTEMS})
endif()

# ============================================
# 导出构建类型定义
# ============================================
add_library(AYBuildSettings INTERFACE)
target_compile_definitions(AYBuildSettings INTERFACE
    AY_BUILD_TARGET_${AY_BUILD_TARGET}
)
```

### 9.2 子系统 CMakeLists.txt 过滤

```cmake
# AYRuntime/AYRender/CMakeLists.txt

# Server 构建跳过渲染子系统
if("Renderer" IN_LIST AY_EXCLUDED_SUBSYSTEMS)
    return()
endif()

add_library(AYRender SUBSYSTEM)
target_sources(AYRender PRIVATE RenderSubSystem.cpp)
target_link_libraries(AYRender PRIVATE AYCore AYGameLoop)
target_compile_definitions(AYRender PRIVATE WITH_RENDER)
```

### 9.3 游戏项目使用

```bash
# 游戏构建（默认）
cmake -B build .
cmake --build build

# 服务器构建
cmake -B build-server -DAY_BUILD_TARGET=server .
cmake --build build-server

# 编辑器构建
cmake -B build-editor -DAY_BUILD_TARGET=editor .
cmake --build build-editor
```

### 9.4 模块引入演进

```
阶段 1: 代码控制（当前）
┌─────────────────────────────────────────┐
│ 代码中 #ifdef 过滤子系统                 │
│ if (buildType == Game) register XXX;    │
│ #ifndef AY_BUILD_TARGET_SERVER          │
│     register YYY;                       │
│ #endif                                 │
└─────────────────────────────────────────┘
         │
         ▼ 演进
阶段 2: CMake 声明 + 代码注册（过渡）
┌─────────────────────────────────────────┐
│ CMake 定义 AY_BUILD_TARGET_*            │
│ 代码按类型注册子系统                     │
│ SubSystemRegistry 读取编译定义          │
└─────────────────────────────────────────┘
         │
         ▼ 演进
阶段 3: CMake 完全控制（目标）
┌─────────────────────────────────────────┐
│ CMake 按类型过滤源文件                  │
│ 编译时完全排除不需要的子系统             │
│ 代码无需条件编译                        │
│ SubSystemRegistry 只看到兼容子系统       │
└─────────────────────────────────────────┘
```

### 9.5 当前状态与目标

| 阶段 | 机制 | 状态 |
|------|------|------|
| 1 | 代码中 `#ifdef` 过滤 | ✅ 已实现 |
| 2 | CMake 定义类型 + Registry 过滤 | 规划 |
| 3 | CMake 过滤源文件 + 代码无感知 | 规划 |

---

## 12. 实现优先级

### Phase 1: 核心
- [x] IApplication 接口
- [x] GameDesc 结构
- [x] 工厂方法 create()
- [x] 基础 run() 实现

### Phase 2: 平台适配
- [x] WinMain 宏
- [x] main() 宏
- [ ] 命令行参数解析 (AppCommandLine)
- [ ] ConfigFile 配置系统

### Phase 3: 生命周期
- [ ] onPostUpdate() 钩子
- [ ] onPreShutdown() 钩子
- [ ] 全局异常捕获
- [ ] 信号处理 (SignalHandler)
- [x] 日志初始化 (ayt::log::initialize)

### Phase 4: 多应用类型
- [ ] BuildType 枚举
- [ ] SubSystemType 分类
- [ ] SubSystemRegistry 按类型过滤
- [ ] CMake 构建类型选项

### Phase 5: 网络通信
- [x] AYNetwork 提供 `INetworkSubSystem`、GNS 传输、连接管理和复制协议
- [x] 客户端 Backend-aware Online Flow 与场景加载桥接
- [x] 精确版本内容目录与可信本地场景解析
- [x] Dedicated allocation 到真实 `Scene/World` 的权威端桥接
- [x] 通用 `AYApplication_DedicatedServer` 入口与 drain 生命周期
- [x] 真实 listener + 两客户端准入/离开/世界释放纵向测试
- [ ] 游戏项目 authority systems 接入（脚本、物理、复制、实体生成）
- [ ] 生产会话后端分配到两客户端入场的跨进程 E2E
- [ ] AYEntity Core / Render Systems 构建目标拆分

### Phase 6: 版本与帮助
- [ ] 版本信息 (getVersion, getEngineVersion)
- [ ] -help, -version 支持

---

## 13. 与工业级引擎对比

### 11.1 功能对比

| 功能 | AYApplication | O3DE | Unreal | Unity |
|------|--------------|------|--------|-------|
| 命令行解析 | 基础 | ✅ 完整 | ✅ 完整 | ✅ 完整 |
| 配置文件 | INI (规划) | JSON/YAML | INI/JSON | JSON |
| 子系统注册 | ✅ | ✅ | ✅ | ✅ |
| 生命周期钩子 | 基础 | 丰富 | 丰富 | 丰富 |
| 异常处理 | 基础 | ✅ | ✅ | ✅ |
| 信号处理 | ❌ | ✅ | ✅ | ❌ |
| 多平台入口 | ✅ | ✅ | ✅ | ✅ |
| 多应用类型 | ✅ 规划 | ✅ Game/Editor | ✅ | ✅ |
| 日志初始化 | ✅ 基础 | ✅ | ✅ | ✅ |
| 网络通信 | ✅ AYNetwork + 应用桥接 | ✅ | ✅ | ✅ |
| Replication 复制 | ✅ AYNetwork；游戏绑定待接入 | ✅ | ✅ | ✅ |
| 版本管理 | ❌ | ✅ | ✅ | ✅ |
| 用户数据路径 | ❌ | ✅ | ✅ | ✅ |
| Crash Handler | ❌ | ✅ | ✅ | ✅ |
| CMake 集成 | 基础 | ✅ Gem | ✅ Target | ✅ |

### 11.2 代码规模对比

| 引擎 | Application 代码行数 | 复杂度 |
|------|---------------------|--------|
| AYApplication | ~300 | 简单 |
| O3DE Application | ~3000 | 复杂 (多应用类型) |
| Unreal Launch | ~1500 | 中等 |
| Unity PlayerLoop | ~800 | 中等 |

### 11.3 差距分析

**核心差距**：
1. **命令行解析** - O3DE/Unreal 有完整的命令行系统，支持子系统的自定义参数
2. **配置系统** - O3DE 使用 JSON/YAML，Unreal 使用IniParser，功能更丰富
3. **多应用类型** - O3DE 有 Game/Editor/Server，Unreal 有 Player/Editor
4. **Crash Handler** - 生产环境必需
5. **日志集成** - 应在 Application 层初始化
6. **模块完全 CMake 化** - 当前代码控制，尚未迁移到 CMake

**当前优先级**：
1. 在游戏项目中实现 authority bootstrap，并通过 `prepareWorld` 注册真实游戏模式、脚本、物理和复制系统
2. 完成“生产 Session Backend 分配 → Dedicated 加载指定内容 → 两客户端准入 → 场景复制”的跨进程 E2E
3. 增加失败验收：内容版本缺失、场景加载失败、第二世界超限、后台 fencing、drain 超时
4. 拆分 AYEntity Core / Render Systems，缩小 Dedicated 链接面与部署包

### 13.4 下一阶段验收条件

下一阶段不是继续扩展通用网络协议，而是把当前通用权威宿主接到一个真实游戏
项目。完成标准：

1. 同一份 `contentId + version + contentSeed` 在 Dedicated 端加载确定的游戏场景。
2. `prepareWorld` 注册项目 authority systems，至少生成一个可复制玩家实体和一个权威物理对象。
3. 两个独立客户端经生产 Session Backend 获得逐玩家 admission token 并进入同一 allocation。
4. 客户端收到初始快照和后续状态变化；第一名玩家离开不卸载世界，最后一名玩家离开后释放 allocation。
5. 内容不匹配、准入失败、backend fencing 和进程 drain 都产生可检索日志并确定性清理场景。
6. 测试至少覆盖本机跨进程；公网/云端验证复用相同命令与内容 catalog，不另设测试专用协议。

---

## 14. AYModule 启动装配边界

AYApplication 通过 `EngineModuleContext` 和 `EngineModuleRuntime` 接入独立
AYModule。默认 Client、Server 与 Editor composition root 已切换到模块图；
旧 `registerDefault*Modules()` 函数只保留给兼容调用方。

生命周期分为两个显式入口：

1. `prepare()`：冻结模块集合、解析依赖，并让所有模块完成
   `registerTypes()`。
2. `install()`：在 Host 锁定组件或反射注册表后安装服务与 SubSystem。

`EngineModuleRuntime` 持有模块，不持有 `IEngineHost`。因此关闭必须由 Host
在自身及其服务仍然有效时显式调用 `shutdown()`。安装失败的逆序回滚由
AYModule 负责；类型注册不提供通用回滚。

当前已接入的模块节点为：

- 类型阶段：`AYEntity.Components` 与按能力选择的
  `AYEntity.*Integration`；
- 第一阶段 SubSystem：`AYDevice.Runtime`、`AYEntity.Runtime`、
  `AYRenderer.Runtime`、`AYPhysics.Runtime`、`AYScript.Runtime`、
  `AYAudio.Runtime`；
- 第二阶段桥接与 SubSystem：`AYEntity.PhysicsIntegration`、
  `AYNetwork.Runtime`、
  `AYApplication.RuntimeSceneLoader`。
- 第三阶段可选扩展：`AYVideo.Runtime`、`AYNetwork.Online`、
  `AYNetwork.OnlineFlow`、`AYOnlineApplication.Runtime`。

`SubSystemModule` 通过 `EngineModuleContext` 发布到当前 GameLoop；GameLoop
仍负责 `initialize/update/shutdown`，模块只负责启动期注册、重复实例收养和
自己所注册实例的撤销。Client/Server/Editor 分别通过
`configureDefaultClientModules()`、`configureDefaultServerModules()`、
`configureDefaultEditorModules()` 选择模块集合。

当前边界与后续项：

- `ComponentRegistry` 仍由 AYEntity 实现，并由 Host 在
  `prepare() -> seal -> install()` 边界显式封存；
- `AYEntity.Runtime` 只安装 Core；Animation/Render/2D/Physics/Script/Network
  组件与行为必须由对应 integration node 显式加入；
- CMake feature 关闭时不会创建对应 Runtime/integration target，Application
  使用同一组 build-capability 宏裁剪 include、代码分支和链接依赖；
- Editor 自有 `DeviceManager` 不作为 GameLoop SubSystem；
- 默认 Client/Server/Editor composition root 中的 GameLoop SubSystem 已全部由
  模块图装配；
- `GameDesc::configureModules` 在默认图完成后、依赖解析前接收项目模块；Video 和
  Online 栈由项目显式选择，不污染普通 Client/Server/Editor 的链接面；
- Scene/EventBus 观察者、Task 完成 hook、Editor 输入桥属于 Host adapter，
  不作为 `IModule` 节点；
- Video 与 Online 仍保留显式注册函数，供独立 Demo 和旧 composition root 兼容；
- 动态插件加载、热卸载和 C ABI 不属于本阶段。

## 15. Application UI Flow Runtime

阶段二把跨 World 的 UI 编排实现为可选静态目标 `AYApplicationUI`，而不是向核心
`AYApplication` 增加 AYUI 硬依赖。Server/headless 构建图保持不变；Client 或工具宿主在
`TARGET AYUI` 时可使用以下三层：

- `UIFlowRuntime`：持有 Entry、并行 Region/State、Context activation、Slot restore floor、
  Scope key、Signal queue 与 Action registry；不直接持有 Widget。
- `IUIFlowScreenHost` / `UIManagerFlowScreenHost`：把逻辑 Screen mount 映射成独立
  `UILayoutLoader` 和有序 Widget Layer，负责视口同步与 Screen 文件热重载。
- `UIFlowRuntimeModule`：作为 Unscaled/Presentation SubSystem 进入模块图，并以
  `kHostServiceUIFlowRuntime` 发布非 owning `UIFlowRuntime*`。

换屏采用 mount-new-before-unmount-old：任一新布局加载失败时撤销本轮新增 mount，旧 UI
保持可见。Context 以 priority、activation serial 排序；Slot 支持 capacity、Hide 和
restorePrevious。Application Scope 永久存在，World/Owner Scope 结束时清除绑定 Context 并只
卸载对应 Screen。Signal 在 UI 线程同步串行，重入进入有界队列；Guard、Graph request、Action
handler 都通过回调/registry 扩展，异常不得越过 runtime 边界。

项目通过 `GameDesc::configureModules` 显式加入 `UIFlowRuntimeModule`，并传入已经初始化的
`UIManagerFlowScreenHost`。Flow 子树由该 Host 持有并以 external child 接入 `UIManager`；宿主替换
根布局时，子树先安全脱离，再在下一次 update 重挂，不会丢失当前 Screen。

阶段三在同一可选目标中增加 `UIFlowSceneBridge`、`UIFlowSceneBridgeModule` 和两种通用 Scene 组件。
Bridge 订阅现有 Scene 生命周期事件，在 `FramePhase::World` 同步 World Scope 和 World→Context 映射，
并把声明过的生命周期/交互信号送入 `UIFlowRuntime`。如果安装了 `GameWorldRouter`，模块图保证 Bridge
在 Router 初始化之后运行，World 切换优先使用稳定的 `pendingWorldId`。Bridge 以
`kHostServiceUIFlowSceneBridge` 发布 non-owning 服务，物理、脚本和任务系统可通过
`emitSceneSignal()` 复用同一入口。

`SceneSignalVolumeComponent` 与 `SceneSignalParticipantComponent` 是 editor-addable、scene-serializable
的 Entity 组件，不持有 Widget、Screen 或游戏类型。内建检测应用 Transform position/scale，忽略旋转，
并以 O(volume × participant) 的确定性扫描覆盖少量 authored UI region；大规模区域由物理 broadphase
检测后调用显式入口。Scene scope 挂载失败时旧 UI 保持不变，Bridge 在后续帧自动重试同步。

阶段五补齐生产门禁：`UIFlowRuntime` 支持异步 Graph execution ID 和每 Region 的 queue/coalesce/
ignore/cancel/reverse 中断策略；`completeGraphExecution()` 驱动 exit → transition → enter pipeline。
`reload()` 在完整验证后保留兼容的手动 Context handle 与活动 Region，布局变化才触发事务式 remount；
运行中的异步 Graph 会明确拒绝 reload。Signal replay 与有界 trace 用于问题复现，不改变正常分发契约。
`UIManagerFlowScreenHost` 使用 Screen 自有动画库完成 enter/exit 交接，并自然遵循 AYUI reduced-motion
设置；`consumeHandled` 通过显式容器能力重试下层目标，`blockLower` 仍是硬边界。

阶段六补齐生产资产闭包。`validateUIFlowAssets()` 在不启动 Runtime 的情况下复用生产
`UILayoutLoader`，检查 Screen 路径必须位于项目 asset root 内、布局可构造、enter/exit clip
存在且动画轨道能解析到 Widget；相同布局只加载一次。结果同时返回按 portable path 排序、
去重并带反向 Screen 引用的 `UIFlowAssetDependency`，供编辑器诊断和内容打包直接消费。
`StructureOnly` profile 只做契约、路径和文件闭包检查，保持 headless 验证不构造 Widget；
`FullClient` profile 才执行真实布局和动画验证。

阶段七闭合 Widget 到 Flow 的生产交互路径。Screen 定义可声明 `handler -> Signal` 映射；
`UIManagerFlowScreenHost` 为每个独立 Loader 安装动态事件 resolver，把布局中的 `onClick` 等语义
处理器转成空动态 payload 的 Flow Signal。显式注册的 controller/global handler 仍优先，未知处理器
继续保持无行为。Runtime 在构造时把稳定的 emitter 注入 Screen Host，析构和 move-assignment 前清除，
避免 Host 或热重载 Loader 持有悬空 Runtime；Screen 映射变化属于 mount identity，会触发事务式重挂。
Full Client 资产审计同时递归收集布局事件处理器，提前报告映射到不存在 handler 的 Screen。

阶段八增加 `UIFlowGraphExecutor`，把阶段五的异步 Graph request 从生命周期协议推进为可执行
command graph。宿主以 `UIFlowGraphNodeTypeDefinition + UIFlowGraphNodeHandler` 注册能力；执行器先用
AYUI 的严格 Pin contract 校验图，再按 execution link 确定性串行运行。节点 property/default 和已完成
上游的 value output 合并为 invocation inputs；`Running` 节点通过稳定 node execution ID 在后续
`completeNode()` 继续，Graph 完成回调再调用 `UIFlowRuntime::completeGraphExecution()` 推进状态机。
执行环、重复 execution ID、未知类型/Pin、handler 异常和失败结果都在边界收敛；cancel/reverse 会使旧
continuation 失效。Completed result 的 flow output、value output 名称和实际值类型也会对照 registry
复核，错误 host handler 不能把不匹配的数据继续传播。执行器位于可选 `AYApplicationUI`，因此
headless 核心和 AYUI 数据层不获得游戏语义。

阶段九把 command graph 推进为依赖感知的生产调度器。Value Link 同时是数据依赖：消费者即使排在
producer 前面也会等待其完成，链接存在但 producer 未执行或未返回对应输出时会得到明确失败，而不是
静默使用默认值。执行边与数据边组成统一 DAG，循环依赖和同一输入连接多个 producer 在启动前拒绝。
单个执行输入 Pin 保持原有 merge/OR 语义；节点声明多个且均已连接的执行输入 Pin 时形成显式
all-input Join，缺少任一已激活分支会报告未完成 Join。

执行器仍以稳定顺序调用 handler，但不会因第一个 `Running` 节点冻结整张图：独立 ready 节点会继续
启动，一张图可同时持有多个异步 continuation。`UIFlowGraphNodeResult::running()` 可声明可选超时和
取消回调；`update()` 驱动超时，Graph failure、cancel/reverse、`setDocument()`、`reset()` 与析构都会
先使 continuation 失效，再至多一次通知每个异步宿主。因而 World/Flow 文档卸载不会留下可回写旧图的
后台节点。Graph 完成仍通过原 completion handler 回到 `UIFlowRuntime`，保持阶段五的状态机边界。

阶段十补齐 Scene 与 UI Flow 之间的稳定控制面。一个 World 可同时绑定多个 Context，`*` 绑定为
每个 World 建立通用展示，精确 World 绑定随后激活并保留正常的优先级/activation-serial 仲裁。
物理、脚本和任务系统既可以直接调用 `emitSceneSignal()`，也可以发布
`UIFlowSceneSignalRequestEvent`，无需持有 Bridge 指针。

剧情、交互区域和临时游戏模式可通过 `pushPresentation()` 投射一个 Context，并用稳定 handle 或
`sourceId` 成组释放。请求只描述 Context、来源和 Application/World/Owner/Transient 生命周期，不接触
Widget 或布局资产。World、Owner 与 Transient presentation 在 Scene 实例切换时自动退休，包括两个
Scene 使用相同 World key 的情况；Application presentation 跨 Scene 保留，直到显式 pop 或 Bridge
停止。由此 Scene 代码控制“何时出现哪组 UI 能力”，而 Flow 继续独占 Layer/Slot/Screen 的组合细节。

阶段十一为生产 Graph Executor 增加可选调试控制。断点在 node handler 调用前命中，快照携带已经合并
property、default 与 value link 的最终 inputs；started/completed trace 分别保留 inputs/outputs。
`requestPause()`、`stepExecution()` 与 `continueExecution()` 只操纵现有 execution/ready queue，不复制
运行逻辑。单步遇到异步节点时等待真实 continuation，再在下一个 ready 节点暂停；interrupt、reset、
document replacement 和 graph finish 都清除关联暂停状态，避免悬空调试快照。

阶段十二建立生产 UI 纵向验收门禁。`UIProductionVerticalSliceTest` 在独立进程中加载真实 Flow/Layout
资产，通过 `DeviceInputBridge` 回放 AYDevice 输入，覆盖菜单进入、Scene bridge 挂载 World HUD、区域
驱动的动画中断与重入、五个 Screen 并行、Modal 阻断及恢复、IME composition/UTF-8 commit、Scene
替换清理和 150% DPI 物理坐标点击。循环内只累计失败，末尾统一断言，并输出包含 Region、Screen、
World 与 effective scale 的确定性 trace。

Windows 视觉门禁由 `UIProductionVerticalSliceVisual` 在隐藏的 D3D11 窗口中使用同一资产和输入路径绘制，
`UIProductionVerticalSliceGoldenRegression` 比较 boot、gameplay、parallel-modal 三个未压缩 TGA。覆盖层
夹具使用透明根容器，使并行 Screen 可在同一帧被观察；Modal 的输入硬边界由功能测试单独验证。基线
容许单通道 2 级量化误差且变化像素上限为 0.05%。测试工具在 bgfx 原始 TGA 落盘后自行生成 PNG 预览，
不依赖异步 PNG sidecar 的时序。常规 CI/本机复测无需人工操作；仅有意更新基线及视觉设计评审需人工确认。

完整格式与运行语义见 [`../AYUI/docs/UIFlow.md`](../AYUI/docs/UIFlow.md)。

## 16. GameFlow 应用流程核心

`AYApplicationGameFlow` 是独立静态目标，负责应用级流程的数据与执行语义。它不链接
`AYApplicationUI`、AYUI、Renderer 或 Device，因此 Dedicated/headless 与完整客户端可以
运行同一个流程计划。阶段一包含：

- `GameFlowDocument` / `GameFlowSerializer`：schema v1 的 typed intent、平面存储的
  层次 state、transition、guard 与 action call；
- `validateGameFlow()` / `buildGameFlowPlan()`：检查重复 ID、引用、parent cycle、字段类型、
  host action/guard 注册，并固化 default argument 与确定性 transition 顺序；
- `GameFlowActionRegistry`：同时向运行时和未来编辑器暴露 action/guard 参数契约，handler
  仍由应用 composition root 注入；
- `GameFlowCoordinator`：跨 World 存活的状态协调器，按叶状态到父状态、priority 降序、
  文档顺序选择 transition，并串行执行 action；
- 异步 action 使用单调 execution ID 和 generation。取消、超时或新迁移后，旧 completion
  不能再写回当前状态；失败与取消只进入文档明确声明的路由。

协调器当前由宿主显式 `update()`，尚未注册 GameLoop module/service。阶段二先增加
`world.replace(worldId)` 适配并通过 `RuntimeSceneLoadFinishedEvent` 完成 pending action；
项目 `startupFlow`、UIFlow 桥接和 AYEditor 节点图依次后置。节点图只能编辑同一
`GameFlowDocument` 并复用同一 validator/normalized plan，不建立第二套运行语义。

实施状态、验证矩阵和后续切片见
[`GAMEFLOW-IMPLEMENTATION-PLAN.md`](../../AYDocs/GAMEFLOW-IMPLEMENTATION-PLAN.md)，架构决策见
[`ADR-0009`](../../AYDocs/adr/0009-gameflow-application-orchestration.md)。

## 17. 参考

- [O3DE Application](https://docs.o3de.org/)
- [Unreal Engine Launch](https://docs.unrealengine.com/en-US/Programming/Development/Architecture/UnrealArchitecture/)
- [Unity PlayerLoop](https://docs.unity3d.com/Manual/ExecutionOrder.html)
- [Glenn Fiedler - Game Engine Architecture](https://gameprogrammingpatterns.com/)
- [CMake Modern Best Practices](https://cmake.org/cmake/help/latest/guide/tutorial/)
- [Unreal Build System](https://docs.unrealengine.com/en-US/ProductionPipelines/BuildTools/UnrealBuildSystem/)
