#include <AYApplication.h>
#include <AYTest.h>

namespace ayt::app::test
{

TEST_SUITE(ApplicationTest)

    TEST_CASE(GameDesc_Default) {
        GameDesc desc;
        CHECK(std::strcmp(desc.name, "Untitled") == 0);
        CHECK(desc.width == 1280u);
        CHECK(desc.height == 720u);
        CHECK(desc.targetFPS == 60.0f);
        CHECK(desc.enableRenderThread);
        CHECK_FALSE(static_cast<bool>(desc.configureModules));
    }

    TEST_CASE(Application_Create) {
        GameDesc desc;
        desc.name = "Test";
        desc.width = 1920;
        desc.height = 1080;

        auto app = IApplication::create(desc);
        CHECK(app != nullptr);
        CHECK(std::strcmp(app->getDesc().name, "Test") == 0);
        CHECK(app->getDesc().width == 1920u);
    }

    TEST_CASE(AppCommandLine_NullArgvIsSafe) {
        const auto commandLine = AppCommandLine::parse(1, static_cast<char**>(nullptr));
        CHECK(commandLine.args.empty());
        CHECK_FALSE(commandLine.help);
    }

    TEST_CASE(AppCommandLine_WideArgumentsBecomeUtf8) {
        wchar_t executable[] = L"BSimmerApp.exe";
        wchar_t option[] = L"-asset-root";
        wchar_t assetRoot[] = L"Assets/\u4e16\u754c";
        wchar_t unknown[] = L"--\u6d4b\u8bd5";
        wchar_t* arguments[] = {executable, option, assetRoot, unknown};

        const auto commandLine = AppCommandLine::parse(4, arguments);

        CHECK(commandLine.args.size() == 4u);
        CHECK(commandLine.assetRoot == "Assets/\xe4\xb8\x96\xe7\x95\x8c");
        CHECK(commandLine.unknownArgs.size() == 1u);
        CHECK(commandLine.unknownArgs.front() == "--\xe6\xb5\x8b\xe8\xaf\x95");
    }

    TEST_CASE(AppCommandLine_ParsesPackagedStartupValidation) {
        char executable[] = "Game.exe";
        char validate[] = "--validate-startup";
        char assetOption[] = "-asset-root";
        char assetRoot[] = "Content";
        char* arguments[] = {
            executable, validate, assetOption, assetRoot};

        const auto commandLine = AppCommandLine::parse(4, arguments);

        CHECK(commandLine.validateStartup);
        CHECK(commandLine.assetRoot == "Content");
        CHECK(commandLine.unknownArgs.empty());
    }

TEST_SUITE_END

} // namespace ayt::app::test
