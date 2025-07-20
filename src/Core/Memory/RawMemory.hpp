#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>

namespace core {
    using byte = unsigned char;

    // Has no ownership, simply provides access to a chunk of memory. All ownership (if any) can be handled from outside.
    struct RawMemory {
        byte* data = nullptr;
        usize size = 0;

        PURE static RawMemory ofObject(auto& obj) noexcept {
            return {
                .data = reinterpret_cast<byte*>(&obj),
                .size = sizeof(obj),
            };
        }

        PURE static RawMemory alloc(usize size); // Note: guaranteed to be aligned at least for a pointer size
        void resize(usize newSize);
        void free();
        constexpr void reset() noexcept {
            data = nullptr;
            size = 0;
        }

        void copyFrom(RawMemory const& other);
        void repeatFrom(RawMemory const& value);
        void repeatValue(byte value);
        PURE constexpr RawMemory chunk(usize from = 0ull, usize count = static_cast<usize>(-1)) const noexcept {
            if (data == nullptr || size == 0 || from >= size || count == 0)
                return RawMemory();
            if (from + count >= size)
                return RawMemory(data + from, size - from);
            return RawMemory(data + from, count);
        }
    };
} // namespace core
