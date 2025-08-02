#include "BmpImageFile.hpp"
#include <Core/IO/Logger.hpp>

namespace core {
#pragma pack(push, 1)
    struct BmpHeader final {
        u16 file_type;
        u32 file_size;
        u16 reserved1;
        u16 reserved2;
        u32 offset_data; 
    };
    
    struct DIBHeader final {
        uint32_t size;
        int32_t width;
        int32_t height;
        uint16_t planes;
        uint16_t bit_count;
        uint32_t compression;
        uint32_t size_image;
        int32_t x_pixels_per_meter;
        int32_t y_pixels_per_meter;
        uint32_t colors_used;
        uint32_t colors_important;
    };
    
    struct ColorHeader {
        uint32_t red_mask         { 0x00ff0000 };
        uint32_t green_mask       { 0x0000ff00 };
        uint32_t blue_mask        { 0x000000ff };
        uint32_t alpha_mask       { 0xff000000 };
        uint32_t color_space_type { 0x73524742 };
        uint32_t unused[16]       { 0 };
    };
#pragma pack(pop)

    BmpImageFile::BmpImageFile(StringView path)
        : ImageFile(path) {
        m_image = readBmpImage();
    }
        
    Image BmpImageFile::readBmpImage() {
        FileHandle file = open();

        BmpHeader bmp_header;
        file.readTo(RawMemory::ofObject(bmp_header));
        if (bmp_header.file_type != 0x4D42) {
            core::error("Incorrect file type: BMP files must begin with 0x4D42, bit it begins with {}", bmp_header.file_type);
            return { };
        } if (bmp_header.reserved1 != 0 || bmp_header.reserved2 != 0) {
            core::error("Non-zero reserved parts in BMP file header");
            return { };
        }

        DIBHeader dib_header;
        file.readTo(RawMemory::ofObject(dib_header));
        if (dib_header.planes != 1) {
            core::error("Incorrect number of planes: BMP files can only have single plane, but got {}", dib_header.planes);
            return { };
        } if (dib_header.width < 1 || dib_header.height < 1) {
            core::error("Only positive width and height are currently supported for BMP files, got {} and {}", dib_header.width, dib_header.height);
            return { };
        } if (dib_header.compression != 3 && dib_header.compression != 0) {
            core::error("Compression for BMP files is not currently supported, compression: {}", dib_header.compression);
            return { };
        } if (dib_header.width % 4 != 0) {
            core::error("Only images with width being a multiply of 4 are currently supported");
            return { };
        }

        ColorHeader color_header;
        if (dib_header.bit_count == 32 && dib_header.size >= sizeof(DIBHeader) + sizeof(ColorHeader)) {
            file.readTo(RawMemory::ofObject(color_header));
            // TODO: support color header values
        }

        file.seek(bmp_header.offset_data);

        Image result({ static_cast<usize>(dib_header.width), static_cast<usize>(dib_header.height) });
        file.readTo(result.data().raw());

        return result;
    }
} // namespace core
