#include "ImageFile.hpp"
#include <string_view>
#include <Core/IO/Logger.hpp>
#include "BmpImageFile.hpp"

namespace core {
    UniquePtr<ImageFile> ImageFile::readImage(StringView path) {
        if (path.endsWith(".bmp") || path.endsWith(".dib")) {
            return makeUP<BmpImageFile>(path);
        }
        
        core::error("Unknown image file format of file {}", std::string_view { path.data(), path.size() });
        return nullptr;
    }
} // namespace core
