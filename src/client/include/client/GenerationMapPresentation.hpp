#pragma once

#include <generation/Generation.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace client
{

using GenerationMapRgba8 = std::array<uint8_t, 4U>;

inline constexpr uint32_t GENERATION_MAP_ANIMATION_RESOLUTION = 256U;
inline constexpr uint32_t GENERATION_MAP_ANIMATION_SCALE = 2U;
inline constexpr uint32_t GENERATION_MAP_ANIMATION_RENDER_RESOLUTION =
    GENERATION_MAP_ANIMATION_RESOLUTION * GENERATION_MAP_ANIMATION_SCALE;
inline constexpr uint8_t GENERATION_MAP_ICE_NONE = 0U;
inline constexpr uint8_t GENERATION_MAP_ICE_SNOW = 1U;
inline constexpr uint8_t GENERATION_MAP_ICE_WATER = 2U;

// Periodic material grain in finite source-cell coordinates; both axes tile every 256 cells.
[[nodiscard]]
float generationMapMaterialGrain(float x, float y) noexcept;

struct GenerationMapImage final
{
    uint32_t width = 0U;
    uint32_t height = 0U;
    std::vector<GenerationMapRgba8> rgba;
};

using GenerationMapImageHandle = std::shared_ptr<GenerationMapImage const>;

struct GenerationMapDisplayIceMask final
{
    // 0 = none, 1 = land snow, 2 = ocean/lake ice.
    std::vector<uint8_t> surface_kind;
    std::vector<float> temperature;
};

using GenerationMapDisplayIceMaskHandle = std::shared_ptr<GenerationMapDisplayIceMask const>;

struct GenerationMapAnimationClock final
{
    // Elapsed for one world identity; the caller pauses it while the loading view is hidden.
    std::chrono::milliseconds visible_elapsed{};
    bool reduced_motion = false;
    // Authoritative readiness does not advance the desktop illustrative timeline.
    bool ready = false;
    // Baseline five-phase duration; presentation uses 1.5x pacing independently of readiness.
    std::chrono::milliseconds formation_duration{25'000};
    // Finite thermal budget from the first geographic relief; provisional fields
    // remain incandescent while evolving. Rain follows full crust and a short pause.
    std::chrono::milliseconds cooling_duration{8'000};
};

enum class GenerationMapAnimationPhase : uint8_t
{
    Tectonics,
    Relief,
    Water,
    Ice,
    Climate,
    Arrival,
    Creation,
};

struct GenerationMapAnimationFrame final
{
    GenerationMapAnimationPhase phase = GenerationMapAnimationPhase::Tectonics;
    float phase_progress = 0.0F;
    GenerationMapImageHandle image;
    // Native surface before weather strokes; exact final-map parity is independent of overlays.
    GenerationMapImageHandle surface_image;
};

// Values from the current native surface, for deterministic thermal inspection.
struct GenerationMapThermalSample final
{
    float potential = 0.0F;
    float progress = 0.0F;
    float elevation_m = 0.0F;
};

struct GenerationMapAnimationCache;
struct GenerationMapNativeCache;
struct S2MapImage;

// Retains immutable frames at 24 visual ticks per visible second. The typed native
// path follows live scalar fields and adopts later fields within the same playback.
class GenerationMapAnimator final
{
public:
    GenerationMapAnimator();
    ~GenerationMapAnimator();
    GenerationMapAnimator(GenerationMapAnimator const&) = delete;
    GenerationMapAnimator& operator=(GenerationMapAnimator const&) = delete;
    GenerationMapAnimator(GenerationMapAnimator&&) noexcept;
    GenerationMapAnimator& operator=(GenerationMapAnimator&&) noexcept;

    [[nodiscard]]
    GenerationMapAnimationFrame update(
        std::shared_ptr<generation::GenerationSnapshot const> snapshot,
        std::shared_ptr<generation::GlobalAtlas const> matching_atlas,
        GenerationMapImageHandle final_image,
        GenerationMapDisplayIceMaskHandle final_display_ice_mask,
        GenerationMapAnimationClock const& clock
    );

    // Begins with the first real progress snapshot before readiness. Later scalar
    // fields converge from the current surface without restarting the visible clock.
    [[nodiscard]]
    GenerationMapAnimationFrame updateNative(
        std::shared_ptr<generation::GenerationSnapshot const> snapshot,
        std::shared_ptr<generation::GlobalAtlas const> atlas,
        std::shared_ptr<S2MapImage const> final_source,
        GenerationMapAnimationClock const& clock
    );

    // Completed-source convenience entry; ordinary loading uses live snapshots.
    [[nodiscard]]
    GenerationMapAnimationFrame updateNative(
        std::shared_ptr<generation::GlobalAtlas const> atlas,
        std::shared_ptr<S2MapImage const> final_source,
        GenerationMapAnimationClock const& clock
    );

    [[nodiscard]]
    std::optional<GenerationMapThermalSample> thermalSample(uint32_t x, uint32_t y) const noexcept;

    void reset() noexcept;

private:
    std::unique_ptr<GenerationMapAnimationCache> cache_;
    std::unique_ptr<GenerationMapNativeCache> native_cache_;
};


[[nodiscard]]
std::optional<GenerationMapDisplayIceMask> makeGenerationMapDisplayIceMask(
    std::span<uint8_t const> climate_zone,
    std::span<uint8_t const> ocean_mask,
    std::span<uint8_t const> lake_mask,
    std::span<float const> temperature
);

// Uses one elevation palette across stages and applies display ice only from the
// shared final-atlas mask. Missing masks leave the map ice-free.
[[nodiscard]]
std::optional<GenerationMapImage> rasterizeGenerationMap(
    generation::GenerationSnapshot const& snapshot,
    std::span<uint8_t const> display_ice_surface_kind = {}
);

// Rasters the completed atlas without copying its field vectors.
[[nodiscard]]
std::optional<GenerationMapImage> rasterizeGenerationMap(
    generation::GlobalAtlas const& atlas,
    std::span<uint8_t const> display_ice_surface_kind = {},
    bool include_climate_tint = true
);

// Retained for the Android presentation path; desktop uses the cached animator.
GenerationMapImageHandle animateGenerationMap(
    generation::GenerationSnapshot const& snapshot,
    GenerationMapImageHandle final_image,
    GenerationMapDisplayIceMaskHandle final_display_ice_mask,
    GenerationMapAnimationClock const& clock
);

} // namespace client
