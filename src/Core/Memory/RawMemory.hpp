#pragma once
#include <Core/Macro/Attributes.hpp>
#include <Core/Common/Int.hpp>

namespace core::memory {
    // Has no ownership, simply provides access to a chunk of memory. All ownership (if any) can be handled from outside.
    struct RawMemory {
    protected:
        unsigned char* m_data = nullptr;
        usize m_size = 0;

    public:
        constexpr RawMemory() noexcept = default;
        constexpr RawMemory(unsigned char* data, usize size) noexcept : m_data(data), m_size(size) { }
        constexpr RawMemory(RawMemory&& other) noexcept = default;
        constexpr RawMemory(RawMemory const& other) noexcept = default;
        constexpr RawMemory& operator=(RawMemory&& other) noexcept = default;
        constexpr RawMemory& operator=(RawMemory const& other) noexcept = default;

        PURE static RawMemory alloc(usize size);
        void resize(usize newSize);
        void free();
        constexpr void reset() noexcept {
            m_data = nullptr;
            m_size = 0;
        }

        void copyFrom(RawMemory const& other);
        void repeatFrom(RawMemory const& value);
        void repeatValue(unsigned char value);
        PURE constexpr RawMemory chunk(usize from = 0ull, usize count = static_cast<usize>(-1)) const noexcept {
            if (m_data == nullptr || m_size == 0 || from >= m_size || count == 0)
                return RawMemory();
            if (from + count >= m_size)
                return RawMemory(m_data + from, m_size - from);
            return RawMemory(m_data + from, count);
        }

        PURE constexpr usize size() const noexcept { return m_size; }
        PURE constexpr auto data(this auto&& self) noexcept { return self.m_data; }
    };
} // namespace core::memory
