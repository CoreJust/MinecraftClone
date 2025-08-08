#include "RawMemory.hpp"
#include <Core/Common/Assert.hpp>

namespace core {
    template<usize ElementSize_>
    struct RawArray : public RawMemory {
        inline constexpr static usize ElementSize = ElementSize_;

        constexpr RawArray() noexcept = default;
        constexpr RawArray(RawArray&&) noexcept = default;
        constexpr RawArray(RawArray const&) noexcept = default;
        constexpr RawArray(RawMemory memory) noexcept : RawMemory(memory) { ASSERT(memory.size % ElementSize == 0); }
        constexpr RawArray& operator=(RawArray&&) noexcept = default;
        constexpr RawArray& operator=(RawArray const&) noexcept = default;

        PURE static RawArray alloc(usize size) { return RawMemory::alloc(size * ElementSize); }
        void resize(usize newSize) { RawMemory::resize(newSize * ElementSize); }

        void copyFrom(RawArray const& other) { RawMemory::copyFrom(other); }
        void repeatFrom(RawArray const& value)  { RawMemory::repeatFrom(value); }
        PURE constexpr RawArray chunk(usize from = 0ull, usize count = static_cast<usize>(-1) / ElementSize) const noexcept {
            return RawMemory::chunk(from * ElementSize, count * ElementSize);
        }

        PURE constexpr byte* ptrAt(usize idx) {
            ASSERT(idx * ElementSize_ + (ElementSize_ - 1) < size);
            return &data[idx * ElementSize]; 
        }
        PURE constexpr byte const* ptrAt(usize idx) const {
            ASSERT(idx * ElementSize_ + (ElementSize_ - 1) < size);
            return &data[idx * ElementSize]; 
        }
        PURE constexpr usize count() const noexcept { return size / ElementSize; }
    };
} // namespace core
