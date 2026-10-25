#pragma once

#include "V9WaterModel.hpp"

#include <cstddef>
#include <string_view>

namespace generation::detail
{

enum class V9WaterFieldType : uint8_t
{
    Bool,
    UInt8,
    Int32,
    Int64,
    Float32,
    Float64,
};

struct V9WaterFieldView final
{
    std::string_view name;
    V9WaterFieldType dtype;
    std::vector<uint64_t> shape;
    std::span<std::byte const> bytes;
};

// Exactly the 45 arrays retained in the frozen v74 water-native.npz. Views
// borrow the result; no type conversion or display registration is performed.
[[nodiscard]]
std::vector<V9WaterFieldView> v9WaterNativeFields(V9WaterResult const& result);

} // namespace generation::detail
