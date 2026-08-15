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
| **Dedicated Server** | 专用服务器，客户端只渲染 | MMO、竞技游戏 | ✅ 规划 |
| **Listen Server** | 客户端同时作为服务器 | 合作游戏、P2P | ✅ 规划 |
| **Peer-to-Peer** | 无服务器，客户端直连 | 街机游戏 | 规划 |
| **Relay Server** | 中继服务器转发消息 | NAT 穿透 | 规划 |

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
| 可靠传输 | ✅ | ❌ | ✅ |
| 有序 | ✅ | ❌ | ✅ |
| 低延迟 | ❌ | ✅ | ❌ |
| NAT 穿透 | ❌ | ❌ | ✅ |
| 浏览器支持 | ❌ | ❌ | ✅ |
| 大数据分片 | ✅ | 规划 | ✅ |
| 加密 (TLS) | ✅ | ❌ | ✅ |

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
├── design.md
├── CMakeLists.txt
├── interface/
│   └── AYApplication/IApplication.h          # 应用接口
│   └── AppCommandLine.h           # 命令行结构
│
├── include/
│   ├── AYApplication.h            # 主入口
│   └── ConfigFile.h               # 配置加载器
│
├── src/
│   ├── AYApplicationImpl.cpp      # 实现
│   ├── AppCommandLine.cpp         # 命令行解析
│   └── ConfigFile.cpp             # 配置解析
│
└── unittest/
    ├── CMakeLists.txt
    ├── AYApplicationTest.cpp
    └── TestMain.cpp
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
- [ ] NetworkDesc 配置结构
- [ ] INetworkSubSystem 接口
- [ ] TCP/UDP 传输层
- [ ] 连接管理 (connect/disconnect)
- [ ] 消息通道 (CHANNEL_RELIABLE/UNRELIABLE)
- [ ] ReplicationManager 复制管理器
- [ ] 复制协议 (状态同步)

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
| 网络通信 | 规划 | ✅ | ✅ | ✅ |
| Replication 复制 | 规划 | ✅ | ✅ | ✅ |
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
1. 完善命令行解析 (AppCommandLine)
2. 添加 ConfigFile 支持
3. 多应用类型支持（BuildType + SubSystemType）
4. CMake 构建类型控制

---

## 14. 参考

- [O3DE Application](https://docs.o3de.org/)
- [Unreal Engine Launch](https://docs.unrealengine.com/en-US/Programming/Development/Architecture/UnrealArchitecture/)
- [Unity PlayerLoop](https://docs.unity3d.com/Manual/ExecutionOrder.html)
- [Glenn Fiedler - Game Engine Architecture](https://gameprogrammingpatterns.com/)
- [CMake Modern Best Practices](https://cmake.org/cmake/help/latest/guide/tutorial/)
- [Unreal Build System](https://docs.unrealengine.com/en-US/ProductionPipelines/BuildTools/UnrealBuildSystem/)