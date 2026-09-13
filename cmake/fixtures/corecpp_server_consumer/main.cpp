#include <core/common/ByteWriter.hpp>
#include <core/IO/Log.hpp>

#include <cstdint>

int main()
{
    core::Log::ensureInit();
    core::ByteWriter writer;
    writer.write(uint32_t{ 48 });
    return writer.size() == sizeof(uint32_t) ? 0 : 1;
}
