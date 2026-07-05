#include <AYApplication/AYApplication.h>
#include <AYTest/AYTest.h>

namespace ayt::app::test
{

TEST_CASE("GameDesc_Default") {
    GameDesc desc;
    ASSERT_STREQ(desc.name, "Untitled");
    ASSERT_EQ(desc.width, 1280u);
    ASSERT_EQ(desc.height, 720u);
    ASSERT_EQ(desc.targetFPS, 60.0f);
    ASSERT_TRUE(desc.enableRenderThread);
}

TEST_CASE("Application_Create") {
    GameDesc desc;
    desc.name = "Test";
    desc.width = 1920;
    desc.height = 1080;

    auto app = IApplication::create(desc);
    ASSERT_NE(app.get(), nullptr);
    ASSERT_STREQ(app->getDesc().name, "Test");
    ASSERT_EQ(app->getDesc().width, 1920u);
}

} // namespace ayt::app::test