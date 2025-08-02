#pragma once
#include "ImageFile.hpp"

namespace core {
    class BmpImageFile final : public ImageFile {
    public:
        explicit BmpImageFile(StringView path);

    private:
        Image readBmpImage();
    };
} // namespace core
