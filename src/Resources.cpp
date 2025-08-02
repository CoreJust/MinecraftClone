#include "Resources.hpp"
#include <Core/Common/Assert.hpp>
#include <Core/Common/Int.hpp>

#define RESOURCES_PATH "res/"
#define ASSETS_PATH "res/assets/"

#define SHADER_PATH RESOURCES_PATH "shaders/"
constexpr usize SHADER_PATH_SIZE = sizeof(SHADER_PATH) - 1;

#define IMAGE_PATH ASSETS_PATH "images/"
constexpr usize IMAGES_PATH_SIZE = sizeof(IMAGE_PATH) - 1;

core::StringView makeShaderPath(char const* const shaderPath) {
    static char s_buffer[1024] = SHADER_PATH;
    char const* src = shaderPath;
    char* dst = s_buffer + SHADER_PATH_SIZE;
    [[maybe_unused]] char const* end = s_buffer + 1024;
    while (*src) {
        *(dst++) = *(src++);
        ASSERT(dst < end);
    }
    ASSERT(dst < end - 5);
    *(dst++) = '.';
    *(dst++) = 's';
    *(dst++) = 'p';
    *(dst++) = 'v';
    *dst = '\0';
    return { &s_buffer[0], static_cast<usize>(dst - s_buffer) };
}

core::StringView makeImagePath(char const* const imagePath) {
    static char s_buffer[1024] = IMAGE_PATH;
    char const* src = imagePath;
    char* dst = s_buffer + IMAGES_PATH_SIZE;
    [[maybe_unused]] char const* end = s_buffer + 1024;
    while (*src) {
        *(dst++) = *(src++);
        ASSERT(dst < end);
    }
    ASSERT(dst < end);
    *dst = '\0';
    return { &s_buffer[0], static_cast<usize>(dst - s_buffer) };
}
