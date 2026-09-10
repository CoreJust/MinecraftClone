#pragma once

#include <type_traits>

namespace core {

/*
 * A type is byte-serializable when its object representation is a fixed,
 * implementation-independent byte sequence (standard layout) and can be
 * copied byte-wise (trivially copyable).
 */
template<typename T>
concept ByteSerializable = true
    && std::is_standard_layout_v<std::remove_cvref_t<T>>
    && std::is_trivially_copyable_v<std::remove_cvref_t<T>>
;

} // namespace core
