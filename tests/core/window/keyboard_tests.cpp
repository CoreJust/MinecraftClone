#include <core/window/Keyboard.hpp>

#include <gtest/gtest.h>

using namespace core;

namespace {
constexpr int KEY_COUNT = static_cast<int>(countOf<Key>());
void setKey(int const key, int const action, int const mode = 0) {
    keyCallback(nullptr, key, 0, action, mode);
}
}

TEST(KeyboardTest, PressAndReleaseStateIsConsistentForRepresentativeKeys) {
    int const keys[] {
        static_cast<int>(Key::Space), static_cast<int>(Key::A),
        static_cast<int>(Key::Escape), static_cast<int>(Key::Menu),
    };
    for (int const value : keys) {
        Key const key = static_cast<Key>(value);
        setKey(value, 1);
        EXPECT_TRUE(isKeyPressed(key));
        EXPECT_FALSE(isKeyReleased(key));
        setKey(value, 0);
        EXPECT_FALSE(isKeyPressed(key));
        EXPECT_TRUE(isKeyReleased(key));
    }
}

TEST(KeyboardTest, ModifierQueriesRequirePressedStateAndRequestedBits) {
    setKey(static_cast<int>(Key::A), 1, static_cast<int>(KeyModifierBit::Shift));
    EXPECT_TRUE(isKeyPressedWithModifer(Key::A, KeyModifierBit::Shift));
    EXPECT_FALSE(isKeyPressedWithModifer(Key::A, KeyModifierBit::Control));
    setKey(static_cast<int>(Key::A), 0);
    EXPECT_FALSE(isKeyPressedWithModifer(Key::A, KeyModifierBit::Shift));
}

TEST(KeyboardTest, InvalidCallbackKeysDoNotChangeValidState) {
    setKey(static_cast<int>(Key::A), 1);
    setKey(-1, 1);
    setKey(KEY_COUNT, 1);
    setKey(KEY_COUNT + 100, 1);
    EXPECT_TRUE(isKeyPressed(Key::A));
    setKey(static_cast<int>(Key::A), 0);
}
