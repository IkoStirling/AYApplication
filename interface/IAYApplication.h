#pragma once
// IAYApplication.h - 应用接口

#include <AYCore.h>
#include <string>
#include <string_view>
#include <vector>

namespace ayt::game
{
class GameLoop;
}

namespace ayt::app
{

// =============================================================================
// GameDesc - 应用描述符
// =============================================================================
struct GameDesc {
    const char* name = "Untitled";
    uint32_t width = 1280;
    uint32_t height = 720;
    float targetFPS = 60.0f;
    bool enableRenderThread = true;
    bool enableDebugConsole = false;        // 是否显示调试控制台
    const char* logLevel = "info";          // 日志级别
    const char* configFile = "";            // 配置文件路径
    const char* assetRoot = "./assets";     // 资源根目录
    const char* userDataPath = "";          // 用户数据目录
};

// =============================================================================
// AppCommandLine - 命令行参数
// =============================================================================
struct AppCommandLine {
    std::vector<std::string> args;         // 原始参数列表

    // 常用选项
    bool help = false;
    bool version = false;
    bool debug = false;
    bool noAudio = false;
    uint32_t width = 0;
    uint32_t height = 0;
    float fps = 0.0f;
    std::string logLevel;
    std::string configFile;
    std::string assetRoot;
    std::string userDataPath;

    // 未知参数（保留给子系统）
    std::vector<std::string> unknownArgs;

    // 解析
    static AppCommandLine parse(int argc, char* argv[]);

    // 帮助信息
    void printHelp(const char* appName) const;
    void printVersion(const char* appName, const char* version) const;
};

// =============================================================================
// AppException - 应用异常
// =============================================================================
class AppException : public std::exception {
public:
    enum class Code {
        Unknown = 0,
        InitFailure = 1,
        ConfigError = 2,
        SubSystemInitFailed = 3,
        RenderInitFailed = 4,
        AssetLoadFailed = 5,
    };

    AppException(Code code, const char* message);
    AppException(Code code, const std::string& message);
    const char* what() const noexcept override;
    Code getCode() const { return _code; }

private:
    Code _code;
    std::string _message;
};

// =============================================================================
// IApplication - 应用接口
// =============================================================================
class IApplication {
public:
    virtual ~IApplication() = default;

    // 工厂方法
    static std::unique_ptr<IApplication> create(const GameDesc& desc);
    static std::unique_ptr<IApplication> create(const GameDesc& desc, const AppCommandLine& cmdLine);

    // 高级入口（自动处理命令行、配置、异常）
    template<typename AppClass>
    static int runEx(int argc, char* argv[]);

    // Optional hook for manual subsystem registration (static REGISTER_SUBSYSTEM is default).
    virtual void registerSubSystems() {}

    // 生命周期钩子
    virtual void onInit() {}
    virtual void onPostUpdate(float deltaTime) {}
    virtual void onPreShutdown() {}
    virtual void onShutdown() {}

    // 启动
    virtual void run() = 0;

    // 查询
    virtual const GameDesc& getDesc() const = 0;
    virtual ayt::game::GameLoop& getGameLoop() = 0;
    virtual const AppCommandLine& getCommandLine() const = 0;

    // 版本信息
    virtual const char* getVersion() const = 0;
    virtual const char* getEngineVersion() const = 0;
};

// =============================================================================
// 便捷宏
// =============================================================================
// 简单入口（无命令行解析）
#if defined(_WIN32)
#define AY_MAIN_DECLARE(appClass) \
    int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int, char*[]) { \
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

// 高级入口（带命令行解析、配置加载、异常处理）
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

// 异常宏
#define AY_THROW(code, msg) throw ayt::app::AppException(ayt::app::AppException::Code::code, msg)

// =============================================================================
// 模板实现
// =============================================================================
template<typename AppClass>
int IApplication::runEx(int argc, char* argv[]) {
    using namespace ayt::app;

    auto cmdLine = AppCommandLine::parse(argc, argv);

    // 显示帮助
    if (cmdLine.help) {
        cmdLine.printHelp(AppClass::getStaticName());
        return 0;
    }

    // 显示版本
    if (cmdLine.version) {
        AppClass app(GameDesc{});
        cmdLine.printVersion(AppClass::getStaticName(), app.getVersion());
        return 0;
    }

    try {
        auto app = IApplication::create(GameDesc{}, cmdLine);
        app->run();
        return 0;
    }
    catch (const AppException& e) {
        // TODO: 日志输出
        return (int)e.getCode();
    }
    catch (const std::exception& e) {
        // TODO: 日志输出
        return -1;
    }
}

} // namespace ayt::app