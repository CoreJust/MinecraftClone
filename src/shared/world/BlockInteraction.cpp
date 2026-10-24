#include <shared/world/BlockInteraction.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace shared {

namespace {

constexpr double MAX_EXACT_VOXEL_COORDINATE = 9'007'199'254'740'000.0;
constexpr double DEGREES_TO_RADIANS = 0.017'453'292'519'943'295'769'236'907'684'89;
constexpr double RAY_TIE_EPSILON = 16.0 * std::numeric_limits<double>::epsilon();

[[nodiscard]]
constexpr uint8_t actionIndex(InteractionAction const action) noexcept
{
    return static_cast<uint8_t>(action);
}

[[nodiscard]]
constexpr bool isValidAction(InteractionAction const action) noexcept
{
    return actionIndex(action) < INTERACTION_ACTION_COUNT;
}

[[nodiscard]]
constexpr uint8_t actionMask(InteractionAction const action) noexcept
{
    return static_cast<uint8_t>(1U << actionIndex(action));
}

[[nodiscard]]
InteractionActionGrants modeDefaults(GameMode const mode, MaterialFilter& break_materials)
{
    InteractionActionGrants grants;
    switch (mode) {
    case GameMode::Spectator:
        grants.set(InteractionAction::Flight);
        grants.set(InteractionAction::CollisionBypass);
        break;
    case GameMode::Creative:
        grants.set(InteractionAction::Break);
        grants.set(InteractionAction::Place);
        grants.set(InteractionAction::Flight);
        grants.set(InteractionAction::QuickBreak);
        break;
    case GameMode::Survival:
        grants.set(InteractionAction::Break);
        grants.set(InteractionAction::Place);
        break_materials.blacklist.push_back(Block::Bedrock);
        break;
    }
    return grants;
}

[[nodiscard]]
MaterialFilter combineFilters(MaterialFilter const& defaults, MaterialFilter const& policy)
{
    MaterialFilter combined;
    if (defaults.whitelist && policy.whitelist) {
        combined.whitelist.emplace();
        for (Block const material : *defaults.whitelist) {
            if (std::find(policy.whitelist->begin(), policy.whitelist->end(), material) != policy.whitelist->end()) {
                combined.whitelist->push_back(material);
            }
        }
    } else if (defaults.whitelist) {
        combined.whitelist = defaults.whitelist;
    } else if (policy.whitelist) {
        combined.whitelist = policy.whitelist;
    }

    combined.blacklist = defaults.blacklist;
    for (Block const material : policy.blacklist) {
        if (std::find(combined.blacklist.begin(), combined.blacklist.end(), material) == combined.blacklist.end()) {
            combined.blacklist.push_back(material);
        }
    }
    return combined;
}

struct AxisTraversal final {
    int64_t cell = 0;
    int step = 0;
    double next_distance = std::numeric_limits<double>::infinity();
    double distance_per_cell = std::numeric_limits<double>::infinity();
};

[[nodiscard]]
bool isFiniteRay(VoxelRay const& ray) noexcept
{
    return std::isfinite(ray.origin_x) && std::isfinite(ray.origin_y) && std::isfinite(ray.origin_z)
        && std::isfinite(ray.direction_x) && std::isfinite(ray.direction_y) && std::isfinite(ray.direction_z);
}

[[nodiscard]]
std::optional<VoxelRay> normalizedRay(VoxelRay ray) noexcept
{
    if (!isFiniteRay(ray)) {
        return std::nullopt;
    }
    double const scale = std::max({
        std::abs(ray.direction_x),
        std::abs(ray.direction_y),
        std::abs(ray.direction_z),
    });
    if (scale == 0.0) {
        return std::nullopt;
    }
    double const scaled_x = ray.direction_x / scale;
    double const scaled_y = ray.direction_y / scale;
    double const scaled_z = ray.direction_z / scale;
    double const scaled_length = std::sqrt(
        scaled_x * scaled_x + scaled_y * scaled_y + scaled_z * scaled_z
    );
    ray.direction_x = scaled_x / scaled_length;
    ray.direction_y = scaled_y / scaled_length;
    ray.direction_z = scaled_z / scaled_length;
    return ray;
}

[[nodiscard]]
AxisTraversal makeAxisTraversal(double const origin, double const direction) noexcept
{
    double const floor_origin = std::floor(origin);
    int64_t cell = static_cast<int64_t>(floor_origin);
    if (direction < 0.0 && origin == floor_origin) {
        --cell;
    }
    if (direction == 0.0) {
        return {.cell = cell};
    }

    int const step = direction > 0.0 ? 1 : -1;
    double const distance_per_cell = 1.0 / std::abs(direction);
    double const boundary = step > 0 ? static_cast<double>(cell) + 1.0 : static_cast<double>(cell);
    double const next_distance = std::abs((boundary - origin) / direction);
    return {
        .cell = cell,
        .step = step,
        .next_distance = next_distance,
        .distance_per_cell = distance_per_cell,
    };
}

[[nodiscard]]
BlockFace initialEnteredFace(VoxelRay const& ray, double const z_acceleration = 0.0) noexcept
{
    if (ray.origin_x == std::floor(ray.origin_x) && ray.direction_x != 0.0) {
        return ray.direction_x > 0.0 ? BlockFace::NegativeX : BlockFace::PositiveX;
    }
    if (ray.origin_y == std::floor(ray.origin_y) && ray.direction_y != 0.0) {
        return ray.direction_y > 0.0 ? BlockFace::NegativeY : BlockFace::PositiveY;
    }
    if (ray.origin_z == std::floor(ray.origin_z)
        && (ray.direction_z != 0.0 || z_acceleration > 0.0)) {
        return ray.direction_z > 0.0 || (ray.direction_z == 0.0 && z_acceleration > 0.0)
            ? BlockFace::NegativeZ
            : BlockFace::PositiveZ;
    }
    return BlockFace::None;
}

[[nodiscard]]
double curvedRayZ(VoxelRay const& ray, double const distance, double const acceleration) noexcept
{
    return ray.origin_z + ray.direction_z * distance + 0.5 * acceleration * distance * distance;
}

[[nodiscard]]
double nextCurvedZBoundary(
    int64_t const cell,
    VoxelRay const& ray,
    double const acceleration,
    double const current_distance
) noexcept
{
    double const lower_boundary = static_cast<double>(cell);
    double const upper_boundary = lower_boundary + 1.0;
    double const quadratic = 0.5 * acceleration;
    double const linear = ray.direction_z;
    double earliest = std::numeric_limits<double>::infinity();

    for (double const boundary : {lower_boundary, upper_boundary}) {
        double const constant = ray.origin_z - boundary;
        if (quadratic == 0.0) {
            if (linear == 0.0) {
                continue;
            }
            double const root = -constant / linear;
            double const minimum_distance = current_distance
                + RAY_TIE_EPSILON * std::max(1.0, std::abs(current_distance));
            if (root > minimum_distance && std::isfinite(root)) {
                earliest = std::min(earliest, root);
            }
            continue;
        }

        double const discriminant = linear * linear - 4.0 * quadratic * constant;
        if (!(discriminant > 0.0) || !std::isfinite(discriminant)) {
            continue;
        }
        double const root_discriminant = std::sqrt(discriminant);
        double const q = -0.5 * (linear + std::copysign(root_discriminant, linear));
        std::array<double, 2> roots{};
        if (q == 0.0) {
            roots = { -linear / (2.0 * quadratic), -linear / (2.0 * quadratic) };
        } else {
            roots = { q / quadratic, constant / q };
        }
        double const minimum_distance = current_distance
            + RAY_TIE_EPSILON * std::max(1.0, std::abs(current_distance));
        for (double const root : roots) {
            if (root <= minimum_distance || !std::isfinite(root)) {
                continue;
            }
            double const derivative = linear + acceleration * root;
            if (derivative != 0.0) {
                earliest = std::min(earliest, root);
            }
        }
    }
    return earliest;
}

[[nodiscard]]
bool sameTraversalDistance(double const left, double const right) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right)) {
        return left == right;
    }
    return std::abs(left - right) <= RAY_TIE_EPSILON
        * std::max({1.0, std::abs(left), std::abs(right)});
}

struct CameraBoomResult final {
    BlockTargetStatus status = BlockTargetStatus::Miss;
    double distance = MAX_INTERACTION_CAMERA_DISTANCE;
};

// Camera clipping follows the linear logical pose offset used by the client; curvature is
// applied to screen-space rays after the camera pose has been reconstructed.
[[nodiscard]]
CameraBoomResult cameraBoomDistance(
    SparseWorld const& world,
    VoxelRay const& eye_ray,
    InteractionPerspective const perspective
) noexcept
{
    double const direction_sign = perspective == InteractionPerspective::ThirdPersonRear ? -1.0 : 1.0;
    BlockTargetResult const obstruction = traceBlockTarget(
        world,
        {
            .origin_x = eye_ray.origin_x,
            .origin_y = eye_ray.origin_y,
            .origin_z = eye_ray.origin_z,
            .direction_x = eye_ray.direction_x * direction_sign,
            .direction_y = eye_ray.direction_y * direction_sign,
            .direction_z = eye_ray.direction_z * direction_sign,
        },
        MAX_INTERACTION_CAMERA_DISTANCE
    );
    switch (obstruction.status) {
    case BlockTargetStatus::Hit:
        return obstruction.target
            ? CameraBoomResult{
                  .status = BlockTargetStatus::Miss,
                  .distance = std::max(0.0, obstruction.target->distance - INTERACTION_CAMERA_OBSTRUCTION_MARGIN),
              }
            : CameraBoomResult{.status = BlockTargetStatus::InvalidRay};
    case BlockTargetStatus::Miss:
        return {.status = BlockTargetStatus::Miss, .distance = MAX_INTERACTION_CAMERA_DISTANCE};
    case BlockTargetStatus::Unknown:
    case BlockTargetStatus::Preview:
    case BlockTargetStatus::OutOfBounds:
    case BlockTargetStatus::InvalidRay:
        return {.status = obstruction.status, .distance = 0.0};
    }
    return {.status = BlockTargetStatus::InvalidRay, .distance = 0.0};
}

[[nodiscard]]
BlockTargetStatus traceFailure(BlockTargetStatus const status) noexcept
{
    switch (status) {
    case BlockTargetStatus::Unknown:
    case BlockTargetStatus::Preview:
    case BlockTargetStatus::OutOfBounds:
    case BlockTargetStatus::InvalidRay:
        return status;
    case BlockTargetStatus::Hit:
    case BlockTargetStatus::Miss:
        return BlockTargetStatus::Miss;
    }
    return BlockTargetStatus::InvalidRay;
}

} // namespace

bool InteractionActionGrants::allows(InteractionAction const action) const noexcept
{
    return isValidAction(action) && (bits & actionMask(action)) != 0U;
}

void InteractionActionGrants::set(InteractionAction const action, bool const enabled) noexcept
{
    if (!isValidAction(action)) {
        return;
    }
    uint8_t const mask = actionMask(action);
    bits = enabled ? static_cast<uint8_t>(bits | mask) : static_cast<uint8_t>(bits & ~mask);
}

void InteractionActionOverrides::set(InteractionAction const action, bool const enabled) noexcept
{
    if (isValidAction(action)) {
        values[actionIndex(action)] = enabled;
    }
}

bool MaterialFilter::allows(Block const material) const noexcept
{
    if (!isValidBlock(material) || material == Block::Air) {
        return false;
    }
    if (whitelist && std::find(whitelist->begin(), whitelist->end(), material) == whitelist->end()) {
        return false;
    }
    return std::find(blacklist.begin(), blacklist.end(), material) == blacklist.end();
}

bool EffectiveInteractionRights::allows(InteractionAction const action) const noexcept
{
    return actions.allows(action);
}

bool EffectiveInteractionRights::canBreak(Block const material) const noexcept
{
    return allows(InteractionAction::Break) && break_materials.allows(material);
}

bool EffectiveInteractionRights::canPlace(Block const material) const noexcept
{
    return allows(InteractionAction::Place) && place_materials.allows(material);
}

EffectiveInteractionRights effectiveInteractionRights(
    GameMode const mode,
    InteractionPolicy const& policy
)
{
    if (!isValidGameMode(mode)) {
        return {.mode = mode};
    }
    MaterialFilter mode_break_materials;
    InteractionActionGrants actions = modeDefaults(mode, mode_break_materials);
    for (uint8_t index = 0U; index < INTERACTION_ACTION_COUNT; ++index) {
        InteractionAction const action = static_cast<InteractionAction>(index);
        if (policy.soft_actions.values[index]) {
            actions.set(action, *policy.soft_actions.values[index]);
        }
        if (policy.hard_vetoes.allows(action)) {
            actions.set(action, false);
        }
    }
    return {
        .mode = mode,
        .actions = actions,
        .break_materials = combineFilters(mode_break_materials, policy.break_materials),
        .place_materials = policy.place_materials,
    };
}

std::optional<BreakTiming> breakTiming(
    EffectiveInteractionRights const& rights,
    Block const material
) noexcept
{
    if (!rights.canBreak(material)) {
        return std::nullopt;
    }
    if (rights.allows(InteractionAction::QuickBreak)) {
        return BreakTiming{.required_full_intervals_after_start = 0U, .quick = true};
    }

    switch (material) {
    case Block::Snow:
        return BreakTiming{.required_full_intervals_after_start = 10U};
    case Block::Dirt:
    case Block::Grass:
        return BreakTiming{.required_full_intervals_after_start = 15U};
    case Block::Ice:
        return BreakTiming{.required_full_intervals_after_start = 20U};
    case Block::Stone:
        return BreakTiming{.required_full_intervals_after_start = 30U};
    case Block::Air:
    case Block::Bedrock:
        return std::nullopt;
    }
    return std::nullopt;
}

bool isBreakComplete(BreakTiming const timing, uint16_t const full_intervals_after_start) noexcept
{
    return timing.quick
        || (timing.required_full_intervals_after_start != 0U
            && full_intervals_after_start >= timing.required_full_intervals_after_start);
}

BlockTargetResult traceBlockTarget(
    SparseWorld const& world,
    VoxelRay const& ray,
    double const max_distance,
    bool const curved_world
) noexcept
{
    if (!std::isfinite(max_distance) || max_distance < 0.0
        || std::abs(ray.origin_x) + max_distance > MAX_EXACT_VOXEL_COORDINATE
        || std::abs(ray.origin_y) + max_distance > MAX_EXACT_VOXEL_COORDINATE) {
        return {.status = BlockTargetStatus::InvalidRay};
    }

    std::optional<VoxelRay> const normalized = normalizedRay(ray);
    if (!normalized) {
        return {.status = BlockTargetStatus::InvalidRay};
    }
    VoxelRay const normalized_ray = *normalized;
    double const curvature_acceleration = curved_world
        ? (normalized_ray.direction_x * normalized_ray.direction_x
            + normalized_ray.direction_y * normalized_ray.direction_y)
            / INTERACTION_WORLD_CURVATURE_RADIUS
        : 0.0;
    double const vertical_extent = max_distance + 0.5 * curvature_acceleration * max_distance * max_distance;
    if (!std::isfinite(vertical_extent)
        || std::abs(normalized_ray.origin_z) + vertical_extent > MAX_EXACT_VOXEL_COORDINATE) {
        return {.status = BlockTargetStatus::InvalidRay};
    }

    AxisTraversal x = makeAxisTraversal(normalized_ray.origin_x, normalized_ray.direction_x);
    AxisTraversal y = makeAxisTraversal(normalized_ray.origin_y, normalized_ray.direction_y);
    AxisTraversal straight_z = makeAxisTraversal(normalized_ray.origin_z, normalized_ray.direction_z);
    int64_t z_cell = straight_z.cell;
    double next_z_distance = curved_world
        ? nextCurvedZBoundary(z_cell, normalized_ray, curvature_acceleration, 0.0)
        : straight_z.next_distance;
    BlockTargetResult result;

    auto inspectCell = [&](double const distance, BlockFace const entered_face) noexcept -> bool {
        auto const normalized = WorldBounds::normalize({x.cell, y.cell, z_cell});
        if (!normalized) {
            result = {.status = BlockTargetStatus::OutOfBounds};
            return false;
        }
        SparseBlockQuery const query = world.queryBlock(*normalized);
        if (query.state == GenerationState::Preview) {
            result = {.status = BlockTargetStatus::Preview};
            return false;
        }
        if (query.state != GenerationState::Materialized || !query.block || !isValidBlock(*query.block)) {
            result = {.status = BlockTargetStatus::Unknown};
            return false;
        }
        if (*query.block != Block::Air) {
            result = {
                .status = BlockTargetStatus::Hit,
                .target = BlockTarget{
                    .cell = *normalized,
                    .block = *query.block,
                    .entered_face = entered_face,
                    .distance = distance,
                },
            };
            return false;
        }
        return true;
    };

    if (!inspectCell(0.0, initialEnteredFace(normalized_ray, curvature_acceleration))) {
        return result;
    }

    while (true) {
        double const next_distance = std::min({x.next_distance, y.next_distance, next_z_distance});
        if (!std::isfinite(next_distance) || next_distance > max_distance) {
            return {.status = BlockTargetStatus::Miss};
        }

        bool const crosses_x = sameTraversalDistance(x.next_distance, next_distance);
        bool const crosses_y = sameTraversalDistance(y.next_distance, next_distance);
        bool const crosses_z = sameTraversalDistance(next_z_distance, next_distance);
        BlockFace face = BlockFace::None;
        if (crosses_x) {
            x.cell += x.step;
            x.next_distance += x.distance_per_cell;
            face = x.step > 0 ? BlockFace::NegativeX : BlockFace::PositiveX;
        }
        if (crosses_y) {
            y.cell += y.step;
            y.next_distance += y.distance_per_cell;
            if (face == BlockFace::None) {
                face = y.step > 0 ? BlockFace::NegativeY : BlockFace::PositiveY;
            }
        }
        if (crosses_z) {
            int const z_step = normalized_ray.direction_z + curvature_acceleration * next_distance > 0.0
                ? 1 : -1;
            z_cell += z_step;
            if (!curved_world) {
                straight_z.cell = z_cell;
                straight_z.next_distance += straight_z.distance_per_cell;
                next_z_distance = straight_z.next_distance;
            } else {
                next_z_distance = nextCurvedZBoundary(
                    z_cell,
                    normalized_ray,
                    curvature_acceleration,
                    next_distance
                );
            }
            if (face == BlockFace::None) {
                face = z_step > 0 ? BlockFace::NegativeZ : BlockFace::PositiveZ;
            }
        }
        straight_z.cell = z_cell;
        if (!inspectCell(next_distance, face)) {
            return result;
        }
    }
}

std::optional<std::array<double, 3>> interactionLookDirection(
    double const yaw_degrees,
    double const pitch_degrees
) noexcept
{
    if (!std::isfinite(yaw_degrees) || !std::isfinite(pitch_degrees)) {
        return std::nullopt;
    }
    double yaw = std::fmod(yaw_degrees, 360.0);
    if (yaw < 0.0) {
        yaw += 360.0;
    }
    double const pitch = std::clamp(pitch_degrees, -89.0, 89.0);
    yaw *= DEGREES_TO_RADIANS;
    double const pitch_radians = pitch * DEGREES_TO_RADIANS;
    double const cosine_pitch = std::cos(pitch_radians);
    return std::array{
        std::sin(yaw) * cosine_pitch,
        std::cos(yaw) * cosine_pitch,
        std::sin(pitch_radians),
    };
}

std::optional<VoxelRay> reconstructInteractionCameraRay(
    VoxelRay eye_ray,
    InteractionPerspective const perspective,
    double const maximum_unobstructed_distance
) noexcept
{
    if (!isValidInteractionPerspective(perspective)) {
        return std::nullopt;
    }
    std::optional<VoxelRay> const normalized_eye_ray = normalizedRay(eye_ray);
    if (!normalized_eye_ray) {
        return std::nullopt;
    }
    double const camera_distance = perspective == InteractionPerspective::FirstPerson
        || !std::isfinite(maximum_unobstructed_distance)
        || maximum_unobstructed_distance <= 0.0
        ? 0.0
        : std::min(maximum_unobstructed_distance, MAX_INTERACTION_CAMERA_DISTANCE);
    double const offset_sign = perspective == InteractionPerspective::ThirdPersonRear ? -1.0 : 1.0;
    double const direction_sign = perspective == InteractionPerspective::ThirdPersonFront ? -1.0 : 1.0;
    VoxelRay camera_ray = *normalized_eye_ray;
    camera_ray.origin_x += normalized_eye_ray->direction_x * camera_distance * offset_sign;
    camera_ray.origin_y += normalized_eye_ray->direction_y * camera_distance * offset_sign;
    camera_ray.origin_z += normalized_eye_ray->direction_z * camera_distance * offset_sign;
    camera_ray.direction_x *= direction_sign;
    camera_ray.direction_y *= direction_sign;
    camera_ray.direction_z *= direction_sign;
    if (!isFiniteRay(camera_ray)) {
        return std::nullopt;
    }
    return camera_ray;
}

BlockTargetResult resolveInteractionTarget(
    SparseWorld const& world,
    VoxelRay const& accepted_eye_ray,
    InteractionPerspective const perspective,
    bool const curved_world
) noexcept
{
    if (!isValidInteractionPerspective(perspective)) {
        return {.status = BlockTargetStatus::InvalidRay};
    }
    std::optional<VoxelRay> const normalized_eye_ray = normalizedRay(accepted_eye_ray);
    if (!normalized_eye_ray) {
        return {.status = BlockTargetStatus::InvalidRay};
    }

    double camera_distance = 0.0;
    if (perspective != InteractionPerspective::FirstPerson) {
        CameraBoomResult const boom = cameraBoomDistance(world, *normalized_eye_ray, perspective);
        if (boom.status != BlockTargetStatus::Miss) {
            return {.status = boom.status};
        }
        camera_distance = boom.distance;
    }

    std::optional<VoxelRay> const camera_ray = reconstructInteractionCameraRay(
        *normalized_eye_ray,
        perspective,
        camera_distance
    );
    if (!camera_ray) {
        return {.status = BlockTargetStatus::InvalidRay};
    }
    BlockTargetResult candidate = traceBlockTarget(
        world,
        *camera_ray,
        camera_distance + DEFAULT_BLOCK_REACH + 1e-6,
        curved_world
    );
    if (candidate.status != BlockTargetStatus::Hit || !candidate.target) {
        return candidate;
    }

    double const candidate_z = curved_world
        ? curvedRayZ(
            *camera_ray,
            candidate.target->distance,
            (camera_ray->direction_x * camera_ray->direction_x
                + camera_ray->direction_y * camera_ray->direction_y)
                / INTERACTION_WORLD_CURVATURE_RADIUS
        )
        : camera_ray->origin_z + camera_ray->direction_z * candidate.target->distance;
    double const candidate_x = camera_ray->origin_x + camera_ray->direction_x * candidate.target->distance;
    double const candidate_y = camera_ray->origin_y + camera_ray->direction_y * candidate.target->distance;
    double const eye_delta_x = candidate_x - normalized_eye_ray->origin_x;
    double const eye_delta_y = candidate_y - normalized_eye_ray->origin_y;
    double const eye_delta_z = candidate_z - normalized_eye_ray->origin_z;
    double const eye_distance = std::hypot(eye_delta_x, eye_delta_y, eye_delta_z);
    if (!std::isfinite(eye_distance) || eye_distance > DEFAULT_BLOCK_REACH + 1e-9) {
        return {.status = BlockTargetStatus::Miss};
    }
    if (eye_distance == 0.0) {
        return candidate;
    }

    double visibility_direction_z = eye_delta_z;
    double visibility_distance = eye_distance;
    if (curved_world) {
        double const horizontal_distance_squared = eye_delta_x * eye_delta_x + eye_delta_y * eye_delta_y;
        // traceBlockTarget applies the renderer inverse curvature, so convert the logical
        // endpoint back to a straight render-space ray before asking it to trace the path.
        visibility_direction_z -= horizontal_distance_squared
            / (2.0 * INTERACTION_WORLD_CURVATURE_RADIUS);
        visibility_distance = std::hypot(eye_delta_x, eye_delta_y, visibility_direction_z);
    }
    double const visibility_limit = std::nextafter(visibility_distance, 0.0);
    BlockTargetResult const visibility = traceBlockTarget(
        world,
        {
            .origin_x = normalized_eye_ray->origin_x,
            .origin_y = normalized_eye_ray->origin_y,
            .origin_z = normalized_eye_ray->origin_z,
            .direction_x = eye_delta_x,
            .direction_y = eye_delta_y,
            .direction_z = visibility_direction_z,
        },
        visibility_limit,
        curved_world
    );
    if (visibility.status != BlockTargetStatus::Miss) {
        return {.status = traceFailure(visibility.status)};
    }
    return candidate;
}

} // namespace shared
