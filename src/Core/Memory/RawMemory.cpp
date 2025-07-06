#include "RawMemory.hpp"
#include <cstdlib>
#include <cstring>
#include <Core/Common/Assert.hpp>
#include <Core/IO/Logger.hpp>
#include "OutOfMemoryException.hpp"

namespace core::memory {
    RawMemory RawMemory::alloc(size_t size) {
        unsigned char* data = reinterpret_cast<unsigned char*>(malloc(size));
        if (!data) {
            core::io::error("Malloc({}) returned nullptr; Probably, out of memory", size);
            throw OutOfMemoryException { };
        }
        return RawMemory(data, size);
    }

    void RawMemory::resize(size_t newSize) {
        if (m_size == newSize)
            return;
        unsigned char* data = reinterpret_cast<unsigned char*>(realloc(m_data, newSize));
        if (!data) {
            core::io::error("Realloc({}) returned nullptr; Probably, out of memory", newSize);
            throw OutOfMemoryException { };
        }
        m_data = data;
        m_size = newSize;
    }
    
    void RawMemory::free() {
        if (m_data == nullptr)
            return;
        ::free(m_data);
        reset();
    }
    
    void RawMemory::reset() {
        m_data = nullptr;
        m_size = 0;
    }

    void RawMemory::copyFrom(RawMemory const& other) {
        ASSERT(other.m_size == m_size);
        ASSERT(m_data != nullptr);
        memcpy(m_data, other.m_data, m_size);
    }

    void RawMemory::repeatFrom(RawMemory const& value) {
        ASSERT(m_data != nullptr);
        unsigned char* dest = m_data;
        unsigned char* end = dest + m_size;
        unsigned char* src = value.m_data;
        while (end - dest >= static_cast<long long>(value.m_size)) {
            memcpy(dest, src, value.m_size);
            dest += value.m_size;
        }
        if (dest < end)
            memcpy(dest, src, static_cast<size_t>(dest - end));
    }

    void RawMemory::repeatValue(unsigned char value) {
        ASSERT(m_data != nullptr);
        memset(m_data, value, m_size);
    }

    RawMemory RawMemory::chunk(size_t from, size_t count) const {
        if (m_data == nullptr || m_size == 0 || from >= m_size || count == 0)
            return RawMemory();
        if (from + count >= m_size)
            return RawMemory(m_data + from, m_size - from);
        return RawMemory(m_data + from, count);
    }
} // namespace core::memory
