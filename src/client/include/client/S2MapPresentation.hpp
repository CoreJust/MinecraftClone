#pragma once

#include <client/render/GuiVector.hpp>
#include <client/render/MapImageDraw.hpp>
#include <client/render/TextRenderer.hpp>

#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <vector>

namespace client {

// Shared native terrain palette, contour, depth and ice composition for map and formation frames.
[[nodiscard]]
std::array<float, 3U> s2NativeTerrainColor(
    float elevation_m, std::array<float, 2U> height_range_m, std::array<float, 2U> neighbors_m,
    bool is_water, float water_depth_m, uint8_t ice_kind
) noexcept;

enum class S2WorldMapLayer : uint8_t {
    Plates,
    Heights,
    Temperature,
    Biomes,
    Wind,
    Currents,
    Moisture,
    Precipitation,
};

inline constexpr uint32_t S2_WORLD_MAP_LAYER_COUNT = 8U;
inline constexpr uint32_t S2_WORLD_MAP_IMAGE_SIDE = 512U;
inline constexpr uint32_t S2_MINIMAP_IMAGE_SIDE = 256U;
inline constexpr uint32_t S2_MINIMAP_WORLD_EXTENT = 16'384U;
inline constexpr std::array<std::array<uint8_t, 4U>, 3U> S2_WORLD_MAP_OVERLAY_PALETTE{{
    {21U, 39U, 47U, 255U},
    {12U, 29U, 36U, 158U},
    {12U, 29U, 36U, 214U},
}};

struct S2WorldMapState final {
    double center_x = 32'768.0;
    double center_y = 32'768.0;
    double zoom = 1.0;
    bool overview = false;
    bool legend_expanded = false;
    std::array<bool, S2_WORLD_MAP_LAYER_COUNT> layers{
        false, true, false, true, false, false, false, false,
    };
};

struct S2PlateMotion final {
    uint32_t id = 0U;
    float x = 0.0F;
    float y = 0.0F;
    float vector_x = 0.0F;
    float vector_y = 0.0F;
};

struct S2MapImage final {
    uint32_t width = 0U;
    uint32_t height = 0U;
    shared::TerrainIdentity source_identity;
    std::string_view source_model_identity = shared::TERRAIN_LOD_MODEL;
    std::array<bool, S2_WORLD_MAP_LAYER_COUNT> available_layers{};
    // Numeric source ranges are shown in the map legend; raster colors use layer-specific transforms.
    std::array<std::array<float, 2U>, S2_WORLD_MAP_LAYER_COUNT> layer_ranges{};
    std::array<bool, S2_WORLD_MAP_LAYER_COUNT> has_numeric_range{};
    uint32_t ice_snow_count = 0U;
    uint32_t ice_glacier_count = 0U;
    uint32_t ice_water_count = 0U;
    bool has_display_ice_mask = false;
    uint32_t source_resolution = 0U;
    uint32_t vector_field_resolution = 0U;
    std::vector<uint32_t> v9_plate_ids;
    std::vector<double> v9_elevation_m;
    std::vector<float> v9_temperature_mean_c;
    std::vector<float> v9_precipitation_mean_mm_year;
    std::vector<uint8_t> v9_biome_ids;
    std::vector<float> v9_moisture_mean_kg_m2;
    std::vector<float> v9_wind_u_m_s;
    std::vector<float> v9_wind_v_m_s;
    std::vector<float> v9_ocean_u_m_s;
    std::vector<float> v9_ocean_v_m_s;
    std::vector<uint8_t> v9_ocean_cells;
    std::vector<uint8_t> v9_ice_surface_kind;
    std::vector<S2PlateMotion> plate_motions;
    std::vector<std::array<uint8_t, 4U>> rgba;
};

enum class S2WorldMapAction : uint8_t {
    TogglePlates,
    ToggleHeights,
    ToggleTemperature,
    ToggleBiomes,
    ToggleWind,
    ToggleCurrents,
    ToggleMoisture,
    TogglePrecipitation,
    PanLeft,
    PanRight,
    PanUp,
    PanDown,
    ZoomIn,
    ZoomOut,
    Overview,
    Reset,
    ToggleLegend,
};

struct S2MapLayout final {
    std::array<MapImageDraw, 4U> world_draws{};
    uint32_t world_draw_count = 0U;
    MapImageDraw minimap_draw{};
    std::array<float, 4U> minimap_bounds{};
    bool show_minimap = false;
};

struct S2MapOverlayLayout final {
    struct Panel final {
        std::array<float, 4U> bounds{};
        uint8_t palette_index = 0U;
    };
    struct HeatStrip final {
        std::array<float, 4U> bounds{};
        S2WorldMapLayer layer = S2WorldMapLayer::Temperature;
        std::array<float, 2U> range{};
    };
    struct ColorChip final {
        std::array<float, 4U> bounds{};
        std::array<uint8_t, 4U> rgba{0U, 0U, 0U, 255U};
    };
    std::array<Panel, 3U> panels{};
    uint32_t panel_count = 0U;
    std::array<HeatStrip, 3U> heat_strips{};
    uint32_t heat_strip_count = 0U;
    std::array<ColorChip, 9U> color_chips{};
    uint32_t color_chip_count = 0U;
};

struct S2MinimapMarker final {
    float x = 0.0F;
    float y = 0.0F;
    double heading_degrees = 0.0;
    bool visible = false;
};

[[nodiscard]]
std::optional<S2MapImage> rasterizeS2WorldMap(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& world_lod,
    S2WorldMapState const& state
);

// Initial native-resolution crop; the local L1 request subsequently refines the same coverage.
[[nodiscard]]
std::shared_ptr<shared::TerrainLodArtifact const> buildS2MinimapPreview(
    shared::TerrainWorld const& world, shared::TerrainLodArtifact const& root,
    double player_x, double player_y);

[[nodiscard]]
// Cancellation returns no artifact so background minimap requests can stop without escaping their worker.
std::shared_ptr<shared::TerrainLodArtifact const> buildS2MinimapLod(
    shared::TerrainWorld const& world,
    double player_x,
    double player_y,
    std::stop_token const& cancellation = {}
);

[[nodiscard]]
std::optional<S2MapImage> rasterizeS2Minimap(
    shared::TerrainLodArtifact const& local_lod
);

[[nodiscard]]
S2MinimapMarker layoutS2MinimapMarker(
    shared::TerrainLodArtifact const& local_lod,
    double player_x,
    double player_y,
    double heading_degrees,
    std::array<float, 4U> const& minimap_bounds
) noexcept;

[[nodiscard]]
S2MapLayout layoutS2Map(
    S2WorldMapState const& state,
    uint32_t width,
    uint32_t height,
    bool map_open,
    bool show_minimap,
    float dpi_scale = 1.0F
) noexcept;

[[nodiscard]]
S2MapOverlayLayout buildS2WorldMapOverlay(
    S2WorldMapState const& state,
    S2MapImage const* image,
    shared::TerrainWorld const* world,
    double player_x,
    double player_y,
    uint32_t width,
    uint32_t height,
    float dpi_scale,
    TextRenderer& output,
    std::vector<GuiVectorVertex>& vector_triangles
);

void appendS2MinimapPointerTriangles(
    S2MinimapMarker const& marker,
    std::array<float, 4U> const& minimap_bounds,
    float dpi_scale,
    std::vector<GuiVectorVertex>& vector_triangles
);

[[nodiscard]]
std::optional<S2MapImage> composeS2WorldMapView(
    S2MapImage const* world_image,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t width,
    uint32_t height
);

[[nodiscard]]
bool applyS2WorldMapAction(
    S2WorldMapState& state,
    S2WorldMapAction action,
    double player_x,
    double player_y,
    bool fast_pan = false
) noexcept;

} // namespace client
