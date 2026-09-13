#include <shared/world/CanonicalWorld.hpp>

#include <shared/CanonicalWorldScript.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace shared {

ScriptedWorld const& canonicalWorld()
{
    static ScriptedWorld const world = [] {
        std::expected<ScriptedWorld, ScriptedWorldError> loaded = ScriptedWorld::load(
            detail::CANONICAL_WORLD_SCRIPT
        );
        if (!loaded) {
            throw std::runtime_error("failed to load canonical world script: " + loaded.error().message);
        }
        return std::move(*loaded);
    }();
    return world;
}

} // namespace shared
