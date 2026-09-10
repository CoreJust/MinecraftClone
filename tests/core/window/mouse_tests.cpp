#include <core/window/Mouse.hpp>

#include <gtest/gtest.h>

using namespace core;

namespace {
constexpr int MOUSE_KEY_COUNT = static_cast<int>(countOf<MouseKey>());
void setMouse(int const button, int const action, int const mode = 0) {
    mouseKeyCallback(nullptr, button, action, mode);
}
}

TEST(MouseTest, PressAndReleaseStateIsConsistentForEveryButton) {
    for (int i = 0; i < MOUSE_KEY_COUNT; ++i) {
        MouseKey const key = static_cast<MouseKey>(i);
        setMouse(i, 1);
        EXPECT_TRUE(isMouseKeyPressed(key));
        EXPECT_FALSE(isMouseKeyReleased(key));
        setMouse(i, 0);
        EXPECT_FALSE(isMouseKeyPressed(key));
        EXPECT_TRUE(isMouseKeyReleased(key));
    }
}

TEST(MouseTest, ModifierQueriesRequirePressedStateAndRequestedBits) {
    setMouse(0, 1, static_cast<int>(KeyModifierBit::Shift) | static_cast<int>(KeyModifierBit::Control));
    EXPECT_TRUE(isMouseKeyPressedWithModifer(MouseKey::Left, KeyModifierBit::Shift));
    EXPECT_TRUE(isMouseKeyPressedWithModifer(MouseKey::Left, KeyModifierBit::Control));
    EXPECT_FALSE(isMouseKeyPressedWithModifer(MouseKey::Left, KeyModifierBit::Alt));
    setMouse(0, 0);
    EXPECT_FALSE(isMouseKeyPressedWithModifer(MouseKey::Left, KeyModifierBit::Shift));
}

TEST(MouseTest, InvalidCallbackButtonsDoNotChangeValidState) {
    setMouse(0, 1);
    setMouse(-1, 1);
    setMouse(MOUSE_KEY_COUNT, 1);
    setMouse(999, 1);
    EXPECT_TRUE(isMouseKeyPressed(MouseKey::Left));
    setMouse(0, 0);
}
