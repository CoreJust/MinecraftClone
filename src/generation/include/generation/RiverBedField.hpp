#pragma once

// Imported S2 river-bed field contract. Channel geometry does not imply wetness.

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace river_field_candidate
{

struct WorldXY final
{
    double x_m;
    double y_m;
};

struct NativePointSample final
{
    WorldXY xy;
    double foundation_m;
    double standing_stage_m;
    int32_t standing_owner;
};

struct RiverReach final
{
    int32_t start_vertex;
    int32_t target_vertex;
    WorldXY start_xy;
    WorldXY delta_xy;
    double grade0_m;
    double grade1_m;
    double discharge_m3_s;
    bool shore_truncated;
    double stage0_m;
    double stage1_m;
};

struct ProtectedContact final
{
    WorldXY xy;
    double radius_m;
};

struct VisualCalibration final
{
    double width_base_m = 2.0;
    double width_sqrt_m_per_sqrt_m3_s = 5.4;
    double depth_base_m = 1.6;
    double depth_sqrt_m_per_sqrt_m3_s = 0.96;
};

struct ProbeOptions final
{
    double gradient_step_m = 16.0;
    double outlet_search_m = 160.0;
    double outlet_step_m = 1.0;
    double receiver_extension_m = 32.0;
};

struct RiverFieldIdentity final
{
    std::string field_model_id;
    std::string field_source_sha256;
    std::string native_model_identity;
    std::string native_source_manifest_sha256;
    std::string native_prepared_state_identity;
    std::string native_sampler_identity;
    std::string reach_catalog_sha256;
    std::string contact_catalog_sha256;
    uint32_t seed;
    double period_m;
};

enum class FailureCode
{
    InvalidInput,
    NativeQueryFailed,
    NativeSampleMismatch,
    OutletUnresolved,
    ConstraintBudgetExceeded,
};

struct Failure final
{
    FailureCode code;
    std::string message;
};

// Synchronous: returns one exact-XY row per input, in the same order.
using NativeBatchSampler = std::function<std::expected<std::vector<NativePointSample>, Failure>(
    std::span<WorldXY const>
)>;

struct RiverBedSample final
{
    WorldXY xy;
    double foundation_m;
    double bed_m;
    double signed_delta_m;
    double corridor_support;
};

// The consumer binds this to its native sampler and sampleRiverBed().
using RiverHeightCallback = std::function<std::expected<double, Failure>(WorldXY)>;

class PreparedRiverField;

// Copies catalog inputs; consumes but never retains the sampler or its output.
[[nodiscard]] std::expected<std::shared_ptr<PreparedRiverField const>, Failure> prepareRiverField(
    RiverFieldIdentity const& identity,
    std::span<RiverReach const> reaches,
    std::span<ProtectedContact const> contacts,
    NativeBatchSampler const& native_sampler,
    VisualCalibration const& calibration,
    ProbeOptions const& probe_options
);

// Empty input succeeds with empty output. No native query or cache mutation.
[[nodiscard]] std::expected<std::vector<RiverBedSample>, Failure> sampleRiverBed(
    PreparedRiverField const& field, std::span<NativePointSample const> native_samples
);

enum class ConsumerKind
{
    L0,
    L1,
    L2,
    Columns1m,
};

struct ConstraintRequest final
{
    WorldXY crop_min;
    WorldXY crop_max;
    ConsumerKind consumer;
    double base_spacing_m;
    // Horizontal feature subdivision; the V2-tested setting is 1.0 m.
    double max_xy_spacing_m = 1.0;
};

enum class FeatureRole
{
    ValleyLeft,
    ChannelBankLeft,
    ChannelInnerLeft,
    Centreline,
    ChannelInnerRight,
    ChannelBankRight,
    ValleyRight,
    Transverse,
};

struct ConstraintVertex final
{
    NativePointSample native;
    double bed_m;
    double signed_delta_m;
    double corridor_support;
};

struct RiverConstraintPolyline final
{
    std::string feature_id;
    uint64_t reach_index;
    uint64_t course_index;
    std::string start_node_key;
    std::string target_node_key;
    bool receiver_extension;
    FeatureRole role;
    WorldXY periodic_image_offset;
    std::vector<ConstraintVertex> vertices;
};

struct RiverConstraintExport final
{
    RiverFieldIdentity identity;
    VisualCalibration calibration;
    ProbeOptions probe_options;
    ConstraintRequest request;
    std::vector<RiverConstraintPolyline> polylines;
    double max_xy_segment_m;
    double max_xyz_chord_m;
    double max_native_midpoint_height_error_m;
    double max_bed_midpoint_height_error_m;
};

// Owns returned data. Proposed success checks requested XY spacing; XYZ chord
// length and midpoint errors are reported without an accepted bound.
// Adaptive height-error refinement has no accepted budget in this proposal.
[[nodiscard]] std::expected<RiverConstraintExport, Failure> exportRiverConstraints(
    PreparedRiverField const& field,
    ConstraintRequest const& request,
    NativeBatchSampler const& native_sampler
);

} // namespace river_field_candidate
