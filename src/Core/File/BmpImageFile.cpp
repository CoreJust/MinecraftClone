#include "BmpImageFile.hpp"
#include <Core/IO/Logger.hpp>

namespace core {
namespace {
#pragma pack(push, 1)
    struct BmpHeader final {
        u16 file_type;
        u32 file_size;
        u16 reserved1;
        u16 reserved2;
        u32 offset_data;

        bool verify() const {
            if (file_type != 0x4D42) {
                core::error("Incorrect file type: BMP files must begin with 0x4D42, bit it begins with {}", file_type);
                return false;
            } if (reserved1 != 0 || reserved2 != 0) {
                core::error("Non-zero reserved parts in BMP file header");
                return false;
            }

            return true;
        }
    };
    
    struct DIBHeader final {
        u32 size;
        i32 width;
        i32 height;
        u16 planes;
        u16 bit_count;
        u32 compression;
        u32 size_image;
        i32 x_pixels_per_meter;
        i32 y_pixels_per_meter;
        u32 colors_used;
        u32 colors_important;

        bool verify() const {    
            if (planes != 1) {
                core::error("Incorrect number of planes: BMP files can only have single plane, but got {}", planes);
                return false;
            } if (width < 1 || height < 1) {
                core::error("Only positive width and height are currently supported for BMP files, got {} and {}", width, height);
                return false;
            } if (compression != 3 && compression != 0) {
                core::error("Compression for BMP files is not currently supported, compression: {}", compression);
                return false;
            } if (width % 4 != 0) {
                core::error("Only images with width being a multiply of 4 are currently supported");
                return false;
            }
            return true;
        }
    };
    
    struct ColorHeader {
        u32 red_mask         { 0x00ff0000 };
        u32 green_mask       { 0x0000ff00 };
        u32 blue_mask        { 0x000000ff };
        u32 alpha_mask       { 0xff000000 };
        u32 color_space_type { 0x73524742 };
        u32 unused[16]       { 0 };

        bool verify() const {
            return color_space_type == 0x73524742;
        }
    };
#pragma pack(pop)

    usize rowUnpaddedSize(i32 width, u16 bit_count) noexcept {
        return static_cast<usize>(width) * static_cast<usize>(bit_count >> 3);
    }

    usize biRGBSize(i32 width, i32 height, u16 bit_count) noexcept {
        usize const row_unpadded_size = rowUnpaddedSize(width, bit_count);
        return (row_unpadded_size + (4 - row_unpadded_size % 4)) * static_cast<usize>(height);
    }

    u8 offset_from_mask(u32 component_mask) noexcept {
        switch (component_mask) {
            case 0x000000ff: return 0;
            case 0x0000ff00: return 8;
            case 0x00ff0000: return 16;
            case 0xff000000: return 24;
        default:
            core::error("Unknown component {}, cannot evaluate offset", component_mask);
            return 255;
        }
    }

    struct BMPDecoder final {
        DynArray<byte> raw_data;
        Vec2<usize> size;
        usize row_unpadded_size;
        u16 bitness;
        u8 r_offset;
        u8 g_offset;
        u8 b_offset;
        u8 a_offset;

        Image decode() {
            if (bitness == 32 && r_offset == 0 && g_offset == 1 && b_offset == 2 && a_offset == 3)
                return Image { size, raw_data.reinterpret<Pixel>() };

            Image result { size };
            usize const row_size = row_unpadded_size + (4 - row_unpadded_size % 4);
            for (usize row = 0; row < size.height(); ++row)
                decode_row(result[row], raw_data.subview(row * row_size, row_unpadded_size));
            return result;
        }

    private:
        void decode_row(ArrayView<Pixel> image_row, ArrayView<byte const> raw_data_row) noexcept {
            byte const* raw_pixel = raw_data_row.begin();
            usize const offset = bitness / 8;
            for (Pixel& pixel : image_row) {
                decode_pixel(pixel, *reinterpret_cast<u32 const*>(raw_pixel));
                raw_pixel += offset;
            }
        }

        void decode_pixel(Pixel& pixel, u32 raw_pixel) noexcept {
#define DECODE_COMPONENT(c) pixel.c() = static_cast<u8>((raw_pixel & (0xffu << c##_offset)) >> c##_offset)
            pixel.a() = 255u;
            if (a_offset != 255u) // Has alpha component
                DECODE_COMPONENT(a);
            DECODE_COMPONENT(r);
            DECODE_COMPONENT(g);
            DECODE_COMPONENT(b);
#undef DECODE_COMPONENT
        }
    };
} // namespace

    BmpImageFile::BmpImageFile(StringView path)
        : ImageFile(path) {
        m_image = readBmpImage();
    }
        
    Image BmpImageFile::readBmpImage() {
        FileHandle file = open();

        BmpHeader bmp_header;
        file.readTo(RawMemory::ofObject(bmp_header));
        if (!bmp_header.verify())
            return { };

        DIBHeader dib_header;
        file.readTo(RawMemory::ofObject(dib_header));
        if (!dib_header.verify())
            return { };

        ColorHeader color_header;
        if (dib_header.bit_count == 32 && dib_header.size >= sizeof(DIBHeader) + sizeof(ColorHeader)) {
            file.readTo(RawMemory::ofObject(color_header));
            if (!color_header.verify()) 
                return { };
        }

        file.seek(bmp_header.offset_data);

        usize raw_size = dib_header.size_image
            ? dib_header.size_image
            : biRGBSize(dib_header.width, dib_header.height, dib_header.bit_count);
        DynArray<byte> raw_data = DynArray<byte>::uninitialized(raw_size);
        file.readTo(raw_data.raw());

        BMPDecoder decoder {
            .raw_data = core::move(raw_data),
            .size     = { static_cast<usize>(dib_header.width), static_cast<usize>(dib_header.height) },
            .row_unpadded_size = rowUnpaddedSize(dib_header.width, dib_header.bit_count),
            .bitness  = dib_header.bit_count,
            .r_offset = offset_from_mask(color_header.red_mask),
            .g_offset = offset_from_mask(color_header.green_mask),
            .b_offset = offset_from_mask(color_header.blue_mask),
            .a_offset = dib_header.bit_count == 32 ? offset_from_mask(color_header.alpha_mask) : static_cast<u8>(255),
        };

        return decoder.decode();
    }
} // namespace core
