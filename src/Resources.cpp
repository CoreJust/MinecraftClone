#include "Resources.hpp"
#include <cstring>

#define RESOURCES_PATH "res/"

#define SHADER_PATH RESOURCES_PATH "shaders/"
constexpr size_t SHADER_PATH_SIZE = sizeof(SHADER_PATH);

char const* makeShaderPath(char const* const shaderPath) {
    static char s_buffer[1024] = SHADER_PATH;
    strcpy_s(s_buffer + SHADER_PATH_SIZE, 1024, shaderPath);
    return s_buffer;
}
