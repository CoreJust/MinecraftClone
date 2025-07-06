#pragma once
#include <Core/Macro/Attributes.hpp>

namespace core::memory {
    // Has no ownership, simply provides access to a chunk of memory. All ownership (if any) can be handled from outside.
    struct RawMemory {
    protected:
        unsigned char* m_data = nullptr;
        size_t m_size = 0;

    public:
        constexpr RawMemory() noexcept = default;
        constexpr RawMemory(unsigned char* data, size_t size) noexcept : m_data(data), m_size(size) { }
        RawMemory(RawMemory&& other) noexcept = default;
        RawMemory(RawMemory const& other) noexcept = default;
        RawMemory& operator=(RawMemory&& other) noexcept = default;
        RawMemory& operator=(RawMemory const& other) noexcept = default;

        PURE static RawMemory alloc(size_t size);
        void resize(size_t newSize);
        void free();
        void reset();

        void copyFrom(RawMemory const& other);
        void repeatFrom(RawMemory const& value);
        void repeatValue(unsigned char value);
        PURE RawMemory chunk(size_t from = 0ull, size_t count = static_cast<size_t>(-1)) const;

        PURE constexpr size_t size() const noexcept { return m_size; }
        PURE constexpr unsigned char* data() noexcept { return m_data; }
        PURE constexpr unsigned char const* data() const noexcept { return m_data; }
    };
} // namespace core::memory
