#include "ShaderAssets.hpp"

#include <core/macro/OS.hpp>

#ifdef WINDOWS
#include <core/OS/Windows/Lean.hpp>
#else

#include <mach-o/dyld.h>
#endif

#include <array>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace client {
namespace {

std::filesystem::path executablePath()
{
#ifdef WINDOWS
    std::array<wchar_t, 32'768> buffer{};
    DWORD const length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0) {
        throw std::system_error(static_cast<int32_t>(GetLastError()), std::system_category(), "Executable path");
    }
    if (length == buffer.size()) {
        throw std::runtime_error("Executable path exceeds the Windows path limit");
    }
    return std::filesystem::canonical(std::wstring(buffer.data(), length));
#else
    std::array<char, 4'096> buffer{};
    uint32_t length = static_cast<uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &length) == 0) {
        return std::filesystem::canonical(buffer.data());
    }
    std::vector<char> expanded_buffer(length);
    if (_NSGetExecutablePath(expanded_buffer.data(), &length) != 0) {
        throw std::runtime_error("Cannot resolve executable path");
    }
    return std::filesystem::canonical(expanded_buffer.data());
#endif
}

} // namespace

std::filesystem::path shaderAssetPath(std::string_view const name)
{
    std::filesystem::path const shader_name{ name };
    if (shader_name.empty() || shader_name.has_parent_path() || shader_name.extension() != ".spv") {
        throw std::invalid_argument("Shader asset must be a .spv filename");
    }
    std::filesystem::path const asset = executablePath().parent_path() / "shaders" / shader_name;
    if (!std::filesystem::is_regular_file(asset)) {
        throw std::runtime_error("Missing shader asset: " + asset.string());
    }
    return asset;
}

} // namespace client
