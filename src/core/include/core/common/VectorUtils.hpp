#pragma once

#include <type_traits>
#include <vector>

namespace core {

template<typename T, typename Element>
concept RangeLike = requires(T t) {
    *std::begin(t);
    *std::end(t);
    requires std::is_same_v<std::remove_cvref_t<decltype(*std::begin(t))>, Element>;
    requires std::is_same_v<std::remove_cvref_t<decltype(*std::end(t))>, Element>;
};

template<typename T, RangeLike<T> Range>
std::vector<T>& appendRange(std::vector<T>& vec, Range&& range) {
    vec.insert(vec.end(), std::begin(range), std::end(range));
    return vec;
}

} // namespace core
