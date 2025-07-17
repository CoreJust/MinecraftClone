#include "RawMemory.hpp"
#include <cstdlib>
#include <cstring>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "OutOfMemoryException.hpp"

namespace core {
    RawMemory RawMemory::alloc(usize size) {
        byte* data = reinterpret_cast<byte*>(malloc(size));
        if (!data) {
            error("Malloc({}) returned nullptr; Probably, out of memory", size);
            throw OutOfMemoryException { };
        }
        return RawMemory(data, size);
    }

    void RawMemory::resize(usize newSize) {
        if (size == newSize)
            return;
        byte* newData = reinterpret_cast<byte*>(realloc(data, newSize));
        if (!newData) {
            error("Realloc({}) returned nullptr; Probably, out of memory", newSize);
            throw OutOfMemoryException { };
        }
        data = newData;
        size = newSize;
    }
    
    void RawMemory::free() {
        if (data == nullptr)
            return;
        ::free(data);
        reset();
    }

    void RawMemory::copyFrom(RawMemory const& other) {
        ASSERT(other.size == size);
        ASSERT(data != nullptr);
        memcpy(data, other.data, size);
    }

    void RawMemory::repeatFrom(RawMemory const& value) {
        ASSERT(data != nullptr);
        byte* dest = data;
        byte* end = dest + size;
        byte* src = value.data;
        while (end - dest >= static_cast<i64>(value.size)) {
            memcpy(dest, src, value.size);
            dest += value.size;
        }
        if (dest < end)
            memcpy(dest, src, static_cast<usize>(dest - end));
    }

    void RawMemory::repeatValue(byte value) {
        ASSERT(data != nullptr);
        memset(data, value, size);
    }
} // namespace core
