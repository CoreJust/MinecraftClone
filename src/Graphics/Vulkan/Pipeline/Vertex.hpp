#pragma once
#include <Core/Meta/IsArray.hpp>
#include <Core/Meta/IsSame.hpp>
#include <Core/Meta/UnRef.hpp>
#include "Attribute.hpp"

namespace graphics::vulkan::pipeline {
    template<typename T>
    concept VertexConcept = requires (T const& vertex) {
        requires core::IsArray<core::UnRef<decltype(T::VertexLayout)>>;
        requires core::IsSame<core::UnRef<decltype(T::VertexLayout[0])>, Attribute const>;
    };

    template<typename T>
    concept PositionedVertexConcept = VertexConcept<T> && requires (T const& vertex) {
        vertex.position;
    };
    template<typename T>
    concept ColoredVertexConcept = VertexConcept<T> && requires (T const& vertex) {
        vertex.color;
    };
    template<typename T>
    concept TexturedVertexConcept = VertexConcept<T> && requires (T const& vertex) {
        vertex.texCoords;
    };
} // namespace graphics::vulkan::pipeline
