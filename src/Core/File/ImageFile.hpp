#pragma once
#include <Core/Container/UniquePtr.hpp>
#include <Core/Container/Image.hpp>
#include "File.hpp"

namespace core {
    class ImageFile : public File {
    protected:
        Image m_image;

    protected:
        ImageFile(StringView path) noexcept : File(path), m_image() { }

    public:
        ImageFile(StringView path, Image image) noexcept : File(path), m_image(core::move(image)) { }
        
        static UniquePtr<ImageFile> readImage(StringView path);

        PURE Image      & image()       noexcept { return m_image; }
        PURE Image const& image() const noexcept { return m_image; }
    };
} // namespace core
