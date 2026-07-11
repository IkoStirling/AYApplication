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

TEST_SUITE_END

} // namespace ayt::app::test