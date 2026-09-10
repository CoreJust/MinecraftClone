#pragma once

namespace core {

template<typename FieldPtr>
struct FieldTraits{ };

template<typename StructTy, typename FieldTy>
struct FieldTraits<FieldTy StructTy::*> {
    using Struct = StructTy;
    using Field = FieldTy;
};

template<typename FieldPtr>
using StructOfFieldType = FieldTraits<FieldPtr>::Struct;
template<auto FieldPtr>
using StructOfField = FieldTraits<decltype(FieldPtr)>::Struct;
template<typename FieldPtr>
using TypeOfFieldType = FieldTraits<FieldPtr>::Field;
template<auto FieldPtr>
using TypeOfField = FieldTraits<decltype(FieldPtr)>::Field;

template<typename FieldPtr>
concept FieldPtrType = requires { typename FieldTraits<FieldPtr>::Struct; };

} // namespace core
