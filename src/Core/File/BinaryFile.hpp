#pragma once
#include "File.hpp"

namespace core {
    class BinaryFile : public File {
        DynArray<byte> m_data;

    public:
        explicit BinaryFile(StringView path)
            : File(path)
            , m_data(open().readAll())
        { }

        PURE DynArray<byte> extractData()        noexcept { return core::move(m_data); }
        PURE DynArray<byte>      & data()       &noexcept { return m_data; }
        PURE DynArray<byte> const& data() const &noexcept { return m_data; }
    };
} // namespace core
