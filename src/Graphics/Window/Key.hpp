#pragma once

namespace graphics::window {
    enum class Key {
        Space = 32,
        Apostrophe = 39,
        Comma = 44,
        Minus,
        Period,
        Slash,

        N0 = 48,
        N1,
        N2,
        N3,
        N4,
        N5,
        N6,
        N7,
        N8,
        N9,

        Semicolon = 59,
        Equal = 61,

        A = 65,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        p,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,

        LeftBracket = 91,
        Backslash,
        RightBracket,
        GraveAccent = 96,

        World1 = 161,
        World2,

        Escape = 256,
        Enter,
        Tab,
        Backspace,
        Insert,
        Delete,
        Right,
        Left,
        Down,
        Up,
        PageUp,
        PageDown,
        Home,
        End,

        CapsLock = 280,
        ScrollLock,
        NumLock,
        PrintScreen,
        Pause,

        F1 = 290,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        F13,
        F14,
        F15,
        F16,
        F17,
        F18,
        F19,
        F20,
        F21,
        F22,
        F23,
        F24,
        F25,

        KP0 = 320,
        KP1,
        KP2,
        KP3,
        KP4,
        KP5,
        KP6,
        KP7,
        KP8,
        KP9,
        KPDecimal,
        KPDivide,
        KPMultiply,
        KPSubtract,
        KPAdd,
        KPEnter,
        KPEqual,

        LeftShift = 340,
        LeftControl,
        LeftAlt,
        LeftSuper,
        RightShift,
        RightControl,
        RightAlt,
        RightSuper,
        Menu,

        KeysCount,
    };

    enum class KeyAction {
        Release = 0,
        Press,
        Repeat,

        KeyActionsCount,
    };

    enum class KeyModifierBit {
        None = 0,
        Shift = 1,
        Control = 2,
        Alt = 4,
        Super = 8,
        CapsLock = 16,
        NumLock = 32,
    };

    constexpr KeyModifierBit operator|(KeyModifierBit lhs, KeyModifierBit rhs) noexcept {
        return static_cast<KeyModifierBit>(static_cast<unsigned int>(lhs) | static_cast<unsigned int>(rhs));
    }

    constexpr KeyModifierBit operator&(KeyModifierBit lhs, KeyModifierBit rhs) noexcept {
        return static_cast<KeyModifierBit>(static_cast<unsigned int>(lhs) & static_cast<unsigned int>(rhs));
    }
} // namespace graphics::window
