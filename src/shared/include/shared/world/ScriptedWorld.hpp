#pragma once

#include <shared/world/Chunk.hpp>
#include <shared/world/World.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace shared {

enum class ScriptedWorldErrorCode : uint8_t {
    InvalidOptions,
    SourceTooLarge,
    CompilationFailed,
    RuntimeFailed,
    HostCallLimitExceeded,
    InvalidHostCall,
    IncompletePublication,
};

struct ScriptedWorldError final {
    ScriptedWorldErrorCode code;
    std::string message;
};

struct ScriptedWorldOptions final {
    static constexpr uint64_t DEFAULT_MAX_SOURCE_BYTES = 8'192U;
    static constexpr uint64_t DEFAULT_MAX_HOST_CALLS = Chunk::BLOCK_COUNT * 2U + 1U;

    uint64_t seed = WorldConfiguration::SEED;
    ChunkCoordinate chunk_coordinate{};
    uint8_t chunk_width = Chunk::SIDE_LENGTH;
    uint8_t chunk_height = Chunk::SIDE_LENGTH;
    uint8_t chunk_depth = Chunk::SIDE_LENGTH;
    uint64_t max_source_bytes = DEFAULT_MAX_SOURCE_BYTES;
    uint64_t max_host_calls = DEFAULT_MAX_HOST_CALLS;
};

[[nodiscard]]
constexpr bool isValidScriptedWorldOptions(ScriptedWorldOptions const& options) noexcept
{
    return options.chunk_width == Chunk::SIDE_LENGTH
        && options.chunk_height == Chunk::SIDE_LENGTH
        && options.chunk_depth == Chunk::SIDE_LENGTH
        && options.max_source_bytes != 0U
        && options.max_host_calls != 0U;
}

class ScriptedWorld final {
public:
    [[nodiscard]]
    static std::expected<ScriptedWorld, ScriptedWorldError> load(
        std::string_view source,
        ScriptedWorldOptions options = {}
    );

    [[nodiscard]]
    Chunk const& chunk() const noexcept;
    [[nodiscard]]
    WorldConfiguration const& configuration() const noexcept;

private:
    ScriptedWorld(Chunk chunk, WorldConfiguration configuration) noexcept;

private:
    Chunk m_chunk;
    WorldConfiguration m_configuration;
};

} // namespace shared
