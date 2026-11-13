#pragma once

#include <client/render/GuiVector.hpp>
#include <client/render/MapImageDraw.hpp>
#include <client/render/TextRenderer.hpp>

#include <shared/world/TerrainLod.hpp>
#include <shared/world/TerrainWorld.hpp>

#include <array>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

namespace client {

// Three rigid line segments (shaft and two wings), shared by CPU formation and map overlays.
[[nodiscard]] std::array<std::array<float, 2U>, 6U> s2FlowArrowSegments(
    std::array<float, 2U> center, std::array<float, 2U> direction, float length
) noexcept;

// Shared native terrain palette, contour, depth and ice composition for map and formation frames.
[[nodiscard]]
std::array<float, 3U> s2NativeTerrainColor(
    float elevation_m, std::array<float, 2U> height_range_m, std::array<float, 2U> neighbors_m,
    bool is_water, float water_depth_m, uint8_t ice_kind
) noexcept;

// Annual cold-dominant freshwater map projection, not a physical cover enum or thickness.
inline constexpr uint8_t S2_MAP_PROJECTED_INLAND_ICE = 4U;
[[nodiscard]] uint8_t s2ProjectedInlandIceKind(
    std::array<double, 8U> const& seasonal_temperature_c, bool ocean, bool lake, bool river
) noexcept;

// Canonical native biome composition, shared by the final map and its history.
[[nodiscard]]
std::array<float, 3U> s2NativeBiomeColor(
    std::array<float, 3U> color, uint8_t biome_id, float coverage = 1.0F
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
inline constexpr uint32_t S2_WORLD_MAP_MAXIMUM_DRAWS = 32U;
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

    // Weather overlays use fields already present in the raster, independent of visibility.
    [[nodiscard]] constexpr auto rasterLayers() const noexcept
    {
        return std::array{
            layers[static_cast<uint8_t>(S2WorldMapLayer::Plates)],
            layers[static_cast<uint8_t>(S2WorldMapLayer::Heights)],
            layers[static_cast<uint8_t>(S2WorldMapLayer::Temperature)],
            layers[static_cast<uint8_t>(S2WorldMapLayer::Biomes)],
            layers[static_cast<uint8_t>(S2WorldMapLayer::Moisture)],
        };
    }
};

struct S2PlateMotion final {
    uint32_t id = 0U;
    float x = 0.0F;
    float y = 0.0F;
    float vector_x = 0.0F;
    float vector_y = 0.0F;
};

struct S2PlateBoundarySegment final {
    std::array<float, 2U> start{};
    std::array<float, 2U> end{};
};

struct S2RiverMapGeometry;

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
    uint32_t ocean_mask_resolution = 0U;
    std::shared_ptr<S2RiverMapGeometry const> v10_river_geometry;
    std::vector<std::array<uint8_t, 4U>> v10_river_base_rgba;
    // Coverage returned by rasterizeS2RiverMap; excludes standing-water owners.
    std::vector<float> v10_river_coverage;
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
    // Native actual-ocean mask; its resolution is independent of the 32x32 vector fields.
    std::vector<uint8_t> v9_ocean_cells;
    std::vector<uint8_t> v9_ice_surface_kind;
    // Fine surface climate at native vertices: eight-phase means and explicit phase-zero stocks.
    std::vector<float> v10_land_snow_mean_m;
    std::vector<float> v10_glacier_mean_m;
    std::vector<float> v10_sea_ice_mean_m;
    std::vector<float> v10_land_snow_phase0_m;
    std::vector<float> v10_glacier_phase0_m;
    std::vector<float> v10_sea_ice_phase0_m;
    std::vector<uint32_t> v10_plate_ids;
    std::vector<double> v10_elevation_m;
    std::vector<float> v10_temperature_mean_c;
    std::vector<float> v10_precipitation_mean_mm_year;
    std::vector<uint8_t> v10_biome_ids;
    std::vector<float> v10_moisture_mean_kg_m2;
    std::vector<float> v10_wind_u_m_s;
    std::vector<float> v10_wind_v_m_s;
    std::vector<float> v10_ocean_u_m_s;
    std::vector<float> v10_ocean_v_m_s;
    // Native actual-ocean mask; its resolution is independent of the 32x32 vector fields.
    std::vector<uint8_t> v10_ocean_cells;
    std::vector<uint8_t> v10_ice_surface_kind;
    // Separate map-only projection; actual physical cover/stocks above remain unchanged.
    std::vector<uint8_t> v10_projected_inland_ice_kind;
    std::vector<uint32_t> v21_plate_ids;
    std::vector<double> v21_elevation_m;
    std::vector<float> v21_temperature_mean_c;
    std::vector<float> v21_precipitation_mean_mm_year;
    std::vector<uint8_t> v21_biome_ids;
    std::vector<float> v21_moisture_mean_kg_m2;
    std::vector<float> v21_wind_u_m_s;
    std::vector<float> v21_wind_v_m_s;
    std::vector<float> v21_ocean_u_m_s;
    std::vector<float> v21_ocean_v_m_s;
    // Native actual-ocean mask; its resolution is independent of the 32x32 vector fields.
    std::vector<uint8_t> v21_ocean_cells;
    std::vector<uint8_t> v21_ice_surface_kind;
    std::vector<S2PlateMotion> plate_motions;
    // Cached source-space runs keep plate borders stable while the viewport pans and zooms.
    std::vector<S2PlateBoundarySegment> plate_boundaries;
    std::vector<std::array<uint8_t, 4U>> rgba;
};

// Shared metric coverage for the world view and creation-stage composition.
// Rendering only reads prepared geometry; panning never evaluates the field.
// A requested stop aborts rasterization before the cache is updated.
[[nodiscard]]
std::shared_ptr<S2MapImage const> rasterizeS2RiverMap(
    std::shared_ptr<S2RiverMapGeometry const> const& geometry,
    std::span<MapImageDraw const> draws, uint32_t screen_width, uint32_t screen_height,
    uint32_t output_width, uint32_t output_height, std::stop_token cancellation = {}
);

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
    std::array<MapImageDraw, S2_WORLD_MAP_MAXIMUM_DRAWS> world_draws{};
    uint32_t world_draw_count = 0U;
    MapImageDraw minimap_draw{};
    std::array<float, 4U> minimap_bounds{};
    bool show_minimap = false;
};

// Owned by the presentation thread. A pending or failed request leaves the baked map available.
class S2WorldMapRiverWorker final {
    struct Request final {
        std::shared_ptr<S2RiverMapGeometry const> geometry;
        S2MapLayout layout;
        uint32_t width;
        uint32_t height;
    };

public:
    S2WorldMapRiverWorker() = default;
    ~S2WorldMapRiverWorker();

    [[nodiscard]]
    std::shared_ptr<S2MapImage const> update(
        std::shared_ptr<S2RiverMapGeometry const> geometry,
        S2MapLayout const& layout,
        uint32_t width,
        uint32_t height
    );

    void invalidate() noexcept;

private:
    std::optional<Request> m_request;
    std::shared_ptr<S2MapImage const> m_result;
    std::stop_source m_cancellation{std::nostopstate};
    std::future<std::shared_ptr<S2MapImage const>> m_job;
};

// One continuous field renderer is used by creation and the interactive world map.
struct S2MapWeatherFields final {
    uint32_t resolution = 0U;
    uint32_t ocean_resolution = 0U;
    uint32_t precipitation_resolution = 0U;
    std::span<float const> wind_u;
    std::span<float const> wind_v;
    std::span<float const> current_u;
    std::span<float const> current_v;
    std::span<float const> precipitation;
    std::span<uint8_t const> ocean;
    float precipitation_maximum = 0.0F;
};

// Rain uses periodic map anchors with zoom-dependent density and bounded pixel size.
// Time is elapsed weather seconds; precipitation and its maximum share source units.
void appendS2MapRainTriangles(
    std::vector<GuiVectorVertex>& output, S2MapLayout const& layout,
    S2MapWeatherFields const& fields, float weather_seconds, float opacity = 1.0F
);

// Phase is elapsed seconds; precipitation and its maximum use the same source units.
void appendS2MapWeatherTriangles(
    std::vector<GuiVectorVertex>& output, S2MapLayout const& layout,
    S2MapWeatherFields const& fields, float phase, float dpi_scale,
    bool wind, bool currents, bool rain, float opacity = 1.0F
);

struct S2MapOverlayLayout final {
    struct ActionRegion final {
        // Physical pixels: left, top, width, height; right/bottom are exclusive.
        std::array<float, 4U> bounds{};
        S2WorldMapAction action = S2WorldMapAction::TogglePlates;
    };
    std::array<ActionRegion, 11U> action_regions{};
    uint32_t action_region_count = 0U;
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

// Converts the camera's +Y-at-zero forward vector to clockwise minimap degrees.
[[nodiscard]]
double minimapHeadingDegreesFromCameraForward(double forward_x, double forward_y) noexcept;

[[nodiscard]]
// A requested stop aborts the map build without returning a partial image.
std::optional<S2MapImage> rasterizeS2WorldMap(
    shared::TerrainWorld const& world,
    shared::TerrainLodArtifact const& world_lod,
    S2WorldMapState const& state,
    std::stop_token cancellation = {}
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

// Keeps the player glyph at the minimap center independently of the loaded LOD window.
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
    std::vector<GuiVectorVertex>& vector_triangles,
    float flow_phase = 0.0F,
    bool touch_controls = false
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
    uint32_t height,
    std::stop_token cancellation = {}
);

// Composes with a prepared viewport layer, or samples the baked map while that layer is pending.
[[nodiscard]]
std::optional<S2MapImage> composeS2WorldMapViewWithRiverLayer(
    S2MapImage const* world_image,
    S2MapLayout const& map_layout,
    S2MapOverlayLayout const& overlay_layout,
    uint32_t width,
    uint32_t height,
    S2MapImage const* river_layer,
    std::stop_token cancellation = {}
);

[[nodiscard]]
float minimumS2WorldMapZoom(uint32_t width, uint32_t height) noexcept;

[[nodiscard]]
bool applyS2WorldMapAction(
    S2WorldMapState& state,
    S2WorldMapAction action,
    double player_x,
    double player_y,
    bool fast_pan,
    uint32_t viewport_width,
    uint32_t viewport_height
) noexcept;

} // namespace client
