#include <shared/net/Message.hpp>
#include <shared/world/Chunk.hpp>
#include <shared/world/SparseWorld.hpp>
#include <shared/world/TerrainWorld.hpp>
#include <shared/world/World.hpp>
#include <shared/world/WorldCoverage.hpp>

#include <generation/Generation.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

std::string terrainConfigurationIdentity()
{
    return generation::configurationIdentity(generation::GenerationConfiguration{});
}

void appendUint16(std::vector<uint8_t>& bytes, uint16_t const value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendUint32(std::vector<uint8_t>& bytes, uint32_t const value)
{
    for (uint32_t byte = 0U; byte < 4U; ++byte) {
        bytes.push_back(static_cast<uint8_t>(value >> (byte * 8U)));
    }
}

void appendUint64(std::vector<uint8_t>& bytes, uint64_t const value)
{
    for (uint32_t byte = 0U; byte < 8U; ++byte) {
        bytes.push_back(static_cast<uint8_t>(value >> (byte * 8U)));
    }
}

std::vector<uint8_t> validV20ClientStartPacket()
{
    generation::GenerationConfiguration const configuration{};
    std::string const identity = terrainConfigurationIdentity();
    std::vector<uint8_t> bytes{ shared::PROTOCOL_MAGIC, 20U, 14U };
    appendUint64(bytes, 1U);
    appendUint32(bytes, generation::TERRAIN_VERSION);
    appendUint64(bytes, 1U);
    appendUint64(bytes, configuration.seed);
    appendUint16(bytes, static_cast<uint16_t>(identity.size()));
    bytes.insert(bytes.end(), identity.begin(), identity.end());
    appendUint64(bytes, configuration.seed);
    appendUint32(bytes, configuration.resolution);
    appendUint32(bytes, configuration.epochs);
    appendUint32(bytes, configuration.erosion_steps);
    appendUint32(bytes, configuration.initial_climate_iterations);
    appendUint32(bytes, configuration.climate_iterations);
    bytes.push_back(static_cast<uint8_t>(configuration.currents));
    bytes.push_back(static_cast<uint8_t>(configuration.wind_sign));
    bytes.push_back(static_cast<uint8_t>(configuration.coastal_elaboration));
    return bytes;
}

shared::TerrainColumn terrainColumn(uint16_t const height, double const temperature = 12.0)
{
    static constexpr uint16_t SEA_LEVEL = 384U;
    bool const submerged = height < SEA_LEVEL;
    uint16_t const first = submerged
        ? 380U
        : static_cast<uint16_t>(height > 4U ? height - 4U : 1U);
    uint16_t const end = submerged
        ? std::max<uint16_t>(static_cast<uint16_t>(height + 1U), first)
        : std::max(height, first);
    return {
        .highest_solid_z = height,
        .water_height_z = 0U,
        .water_present = false,
        .submerged = submerged,
        .geological_elevation_km = -0.25,
        .temperature_celsius = temperature,
        .precipitation = 0.3,
        .top_material = submerged
            ? shared::Block::Dirt
            : (temperature <= -2.0 ? shared::Block::Snow : shared::Block::Grass),
        .ground_material = temperature <= -2.0 ? shared::Block::Ice : shared::Block::Dirt,
        .ground_first_z = first,
        .ground_end_z = end,
    };
}

void synchronizeSurface(shared::ServerHeightTileMessage& message)
{
    message.surface.coordinate = { .x = message.key.x, .y = message.key.y, .z = 0 };
    message.surface.block_sea_level = 384U;
    message.surface.max_height = generation::MAX_HEIGHT;
    message.surface.identity = {
        .terrain_version = generation::TERRAIN_VERSION,
        .revision = message.revision,
        .seed = 42U,
        .configuration_identity = terrainConfigurationIdentity(),
    };
    for (uint32_t index = 0U; index < message.heights.size(); ++index) {
        message.surface.columns[index] = terrainColumn(static_cast<uint16_t>(message.heights[index] - 1U));
    }
}

shared::ServerHeightTileDescriptorMessage heightTileDescriptorMessage()
{
    shared::ServerHeightTileDescriptorMessage message;
    message.world_identity = {
        .terrain_version = generation::TERRAIN_VERSION,
        .revision = message.world_revision,
        .seed = message.configuration.seed,
        .configuration_identity = terrainConfigurationIdentity(),
    };
    return message;
}

shared::ServerHeightTileMessage heightTileMessage()
{
    shared::ServerHeightTileMessage message{
        .key = {.x = 17, .y = 23},
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index{ 0U }; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        message.heights[index] = static_cast<uint16_t>(index + 2U);
    }
    synchronizeSurface(message);
    return message;
}

uint32_t surfaceWireSize(shared::SurfaceTile const& surface)
{
    return 8U + 22U + static_cast<uint32_t>(surface.identity.configuration_identity.size())
        + shared::HEIGHT_TILE_SAMPLE_COUNT * shared::HEIGHT_TILE_SURFACE_COLUMN_BYTES;
}

shared::ServerHeightTileMessage materialHeightTileMessage()
{
    shared::ServerHeightTileMessage message = heightTileMessage();
    message.heights.fill(391U);
    message.surface.columns.fill(terrainColumn(390U));
    message.surface.columns[1U] = terrainColumn(390U, -5.0);
    message.surface.columns[2U] = terrainColumn(380U, -5.0);
    message.heights[2U] = 381U;
    return message;
}

shared::ServerHeightTileBatchMessage heightTileBatchMessage()
{
    shared::ServerHeightTileBatchMessage batch{
        .delivery_token = 19U,
        .tiles = {},
        .removals = {},
    };
    batch.tiles.reserve(2U);
    for (uint32_t index = 0U; index < 2U; ++index) {
        batch.tiles.push_back(heightTileMessage());
    }
    auto& second = batch.tiles[1U];
    second.key = { .x = 18, .y = 23 };
    second.token = 12U;
    second.surface.coordinate = { .x = second.key.x, .y = second.key.y, .z = 0 };
    return batch;
}

shared::TerrainSessionIdentity terrainSessionIdentity(uint64_t const join_epoch = 7U)
{
    generation::GenerationConfiguration const configuration{};
    return {
        .join_epoch = join_epoch,
        .terrain = {
            .terrain_version = generation::TERRAIN_VERSION,
            .revision = 3U,
            .seed = configuration.seed,
            .configuration_identity = generation::configurationIdentity(configuration),
        },
        .configuration = configuration,
    };
}

std::shared_ptr<generation::GenerationSnapshot const> generationSnapshot256()
{
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::Tectonics;
    snapshot->resolution = 256U;
    static constexpr std::vector<float>::size_type SAMPLE_COUNT =
        static_cast<std::vector<float>::size_type>(256U) * 256U;
    snapshot->elevation.resize(SAMPLE_COUNT, 0.125F);
    snapshot->ocean_mask.resize(SAMPLE_COUNT, 0U);
    return snapshot;
}

shared::MaterializedChunkPayload uniformChunkPayload(
    shared::ChunkCoordinate const coordinate,
    shared::Block const material
)
{
    shared::Chunk::Blocks blocks{};
    blocks.fill(material);
    shared::Chunk const chunk{ coordinate, blocks };
    return {
        .coordinate = coordinate,
        .content_hash = chunk.contentIdentity().content_hash,
        .data = shared::UniformChunkData{ .material = material },
    };
}

shared::MaterializedChunkPayload denseChunkPayload(shared::ChunkCoordinate const coordinate)
{
    shared::Chunk::Blocks blocks{};
    for (uint32_t index = 0U; index < shared::Chunk::BLOCK_COUNT; ++index) {
        blocks[index] = static_cast<shared::Block>(index % 7U);
    }
    shared::Chunk const chunk{ coordinate, blocks };
    return {
        .coordinate = coordinate,
        .content_hash = chunk.contentIdentity().content_hash,
        .data = shared::DenseChunkData{ .blocks = blocks },
    };
}

shared::ServerWorldPreparationMessage worldPreparationMessage()
{
    return {
        .session = terrainSessionIdentity(),
        .sequence = 2U,
        .stage = generation::GenerationStage::Tectonics,
        .work_completed = 10U,
        .work_total = 20U,
        .server_elapsed_microseconds = 1'234'567U,
        .latest_snapshot = generationSnapshot256(),
        .refined_safe_spawn = std::nullopt,
        .failure = {},
    };
}

template<typename MessageTy>
void expectRoundTrip(MessageTy const& expected) {
    shared::Message const message = expected;
    std::vector<uint8_t> const bytes = shared::encodeMessage(message);
    auto const decoded = shared::decodeMessage(std::span<uint8_t const>{ bytes.data(), bytes.size() });
    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<MessageTy>(*decoded));
    MessageTy const actual = std::get<MessageTy>(*decoded);
    if constexpr (std::is_same_v<MessageTy, shared::JoinRequestMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
        EXPECT_EQ(actual.mode, expected.mode);
        EXPECT_EQ(actual.configuration, expected.configuration);
        EXPECT_EQ(actual.wants_previews, expected.wants_previews);
        EXPECT_EQ(actual.join_epoch, expected.join_epoch);
    } else if constexpr (std::is_same_v<MessageTy, shared::JoinResponseMessage>) {
        EXPECT_EQ(actual.accepted, expected.accepted);
        EXPECT_EQ(actual.join_epoch, expected.join_epoch);
    } else if constexpr (std::is_same_v<MessageTy, shared::ClientInputMessage>) {
        EXPECT_EQ(actual.direction.x, expected.direction.x);
        EXPECT_EQ(actual.direction.y, expected.direction.y);
        EXPECT_EQ(actual.direction.z, expected.direction.z);
        EXPECT_EQ(actual.direction.accelerated, expected.direction.accelerated);
        EXPECT_EQ(actual.direction.speedup, expected.direction.speedup);
        EXPECT_EQ(
            actual.direction.cycle_movement_capabilities,
            expected.direction.cycle_movement_capabilities
        );
        EXPECT_EQ(actual.direction.view_x, expected.direction.view_x);
        EXPECT_EQ(actual.direction.view_y, expected.direction.view_y);
        EXPECT_EQ(actual.sequence, expected.sequence);
    } else if constexpr (std::is_same_v<MessageTy, shared::ClientHeightTileCreditMessage>) {
        EXPECT_EQ(actual.world_revision, expected.world_revision);
        EXPECT_EQ(actual.delivery_token, expected.delivery_token);
        EXPECT_EQ(actual.credits, expected.credits);
        EXPECT_EQ(actual.not_resident_keys, expected.not_resident_keys);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerPlayerPositionMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
        EXPECT_EQ(actual.palette_index, expected.palette_index);
        EXPECT_EQ(actual.movement_capabilities, expected.movement_capabilities);
        EXPECT_EQ(actual.x, expected.x);
        EXPECT_EQ(actual.y, expected.y);
        EXPECT_EQ(actual.z, expected.z);
        EXPECT_EQ(actual.x_subcell, expected.x_subcell);
        EXPECT_EQ(actual.y_subcell, expected.y_subcell);
        EXPECT_EQ(actual.z_subcell, expected.z_subcell);
        EXPECT_EQ(actual.vertical_velocity_subcells, expected.vertical_velocity_subcells);
        EXPECT_EQ(actual.acknowledged_input_sequence, expected.acknowledged_input_sequence);
        EXPECT_EQ(actual.state_revision, expected.state_revision);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerRemovePlayerMessage>) {
        EXPECT_EQ(actual.ch, expected.ch);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerHeightTileDescriptorMessage>) {
        EXPECT_EQ(actual.configuration, expected.configuration);
        EXPECT_EQ(actual.world_identity, expected.world_identity);
        EXPECT_EQ(actual.world_revision, expected.world_revision);
        EXPECT_EQ(actual.max_height_tiles, expected.max_height_tiles);
        EXPECT_EQ(actual.max_height_tile_bytes, expected.max_height_tile_bytes);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerWorldRevisionMessage>) {
        EXPECT_EQ(actual.world_revision, expected.world_revision);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerHeightTileBatchMessage>) {
        EXPECT_EQ(actual.delivery_token, expected.delivery_token);
        ASSERT_EQ(actual.tiles.size(), expected.tiles.size());
        for (uint32_t index = 0U; index < actual.tiles.size(); ++index) {
            EXPECT_EQ(actual.tiles[index].key, expected.tiles[index].key);
            EXPECT_EQ(actual.tiles[index].revision, expected.tiles[index].revision);
            EXPECT_EQ(actual.tiles[index].token, expected.tiles[index].token);
            EXPECT_EQ(actual.tiles[index].heights, expected.tiles[index].heights);
        }
        ASSERT_EQ(actual.removals.size(), expected.removals.size());
        for (uint32_t index = 0U; index < actual.removals.size(); ++index) {
            EXPECT_EQ(actual.removals[index].key, expected.removals[index].key);
            EXPECT_EQ(actual.removals[index].revision, expected.removals[index].revision);
            EXPECT_EQ(actual.removals[index].token, expected.removals[index].token);
        }
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerWorldPreparationMessage>) {
        EXPECT_EQ(actual.session, expected.session);
        EXPECT_EQ(actual.sequence, expected.sequence);
        EXPECT_EQ(actual.stage, expected.stage);
        EXPECT_EQ(actual.work_completed, expected.work_completed);
        EXPECT_EQ(actual.work_total, expected.work_total);
        EXPECT_EQ(actual.server_elapsed_microseconds, expected.server_elapsed_microseconds);
        EXPECT_EQ(actual.final_atlas_complete, expected.final_atlas_complete);
        EXPECT_EQ(actual.failure, expected.failure);
        ASSERT_EQ(static_cast<bool>(actual.latest_snapshot), static_cast<bool>(expected.latest_snapshot));
        if (expected.latest_snapshot) {
            EXPECT_EQ(actual.latest_snapshot->stage, expected.latest_snapshot->stage);
            EXPECT_EQ(actual.latest_snapshot->resolution, expected.latest_snapshot->resolution);
            EXPECT_EQ(actual.latest_snapshot->elevation, expected.latest_snapshot->elevation);
            EXPECT_EQ(actual.latest_snapshot->temperature, expected.latest_snapshot->temperature);
            EXPECT_EQ(actual.latest_snapshot->precipitation, expected.latest_snapshot->precipitation);
            EXPECT_EQ(actual.latest_snapshot->water_level, expected.latest_snapshot->water_level);
            EXPECT_EQ(actual.latest_snapshot->ocean_mask, expected.latest_snapshot->ocean_mask);
            EXPECT_EQ(actual.latest_snapshot->lake_mask, expected.latest_snapshot->lake_mask);
            EXPECT_EQ(actual.latest_snapshot->river_mask, expected.latest_snapshot->river_mask);
        }
        EXPECT_EQ(
            static_cast<bool>(actual.refined_safe_spawn),
            static_cast<bool>(expected.refined_safe_spawn)
        );
        if (expected.refined_safe_spawn) {
            ASSERT_TRUE(actual.refined_safe_spawn.has_value());
            if (!actual.refined_safe_spawn) {
                return;
            }
            EXPECT_EQ(actual.refined_safe_spawn->x, expected.refined_safe_spawn->x);
            EXPECT_EQ(actual.refined_safe_spawn->y, expected.refined_safe_spawn->y);
            EXPECT_EQ(actual.refined_safe_spawn->z, expected.refined_safe_spawn->z);
            EXPECT_EQ(actual.refined_safe_spawn->x_subcell, expected.refined_safe_spawn->x_subcell);
            EXPECT_EQ(actual.refined_safe_spawn->y_subcell, expected.refined_safe_spawn->y_subcell);
            EXPECT_EQ(actual.refined_safe_spawn->z_subcell, expected.refined_safe_spawn->z_subcell);
        }
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerMaterializedChunkBatchMessage>) {
        EXPECT_EQ(actual.session, expected.session);
        EXPECT_EQ(actual.coverage_epoch, expected.coverage_epoch);
        EXPECT_EQ(actual.delivery_token, expected.delivery_token);
        ASSERT_EQ(actual.chunks.size(), expected.chunks.size());
        for (uint32_t index = 0U; index < actual.chunks.size(); ++index) {
            auto const& actual_chunk = actual.chunks[index];
            auto const& expected_chunk = expected.chunks[index];
            EXPECT_EQ(actual_chunk.coordinate, expected_chunk.coordinate);
            EXPECT_EQ(actual_chunk.content_hash, expected_chunk.content_hash);
            ASSERT_EQ(actual_chunk.data.index(), expected_chunk.data.index());
            if (auto const* const uniform = std::get_if<shared::UniformChunkData>(&expected_chunk.data)) {
                EXPECT_EQ(std::get<shared::UniformChunkData>(actual_chunk.data).material, uniform->material);
            } else {
                EXPECT_EQ(
                    std::get<shared::DenseChunkData>(actual_chunk.data).blocks,
                    std::get<shared::DenseChunkData>(expected_chunk.data).blocks
                );
            }
        }
    } else if constexpr (std::is_same_v<MessageTy, shared::ClientChunkDeliveryAckMessage>) {
        EXPECT_EQ(actual.session, expected.session);
        EXPECT_EQ(actual.coverage_epoch, expected.coverage_epoch);
        EXPECT_EQ(actual.delivery_token, expected.delivery_token);
        EXPECT_EQ(actual.credits, expected.credits);
        EXPECT_EQ(actual.not_resident_keys, expected.not_resident_keys);
    } else if constexpr (std::is_same_v<MessageTy, shared::ClientStartWorldMessage>) {
        EXPECT_EQ(actual.session, expected.session);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerStartWorldAckMessage>) {
        EXPECT_EQ(actual.session, expected.session);
        EXPECT_EQ(actual.accepted, expected.accepted);
        EXPECT_EQ(actual.failure, expected.failure);
    } else if constexpr (std::is_same_v<MessageTy, shared::ServerChunkCoverageMessage>) {
        EXPECT_EQ(actual.session, expected.session);
        EXPECT_EQ(actual.coverage_epoch, expected.coverage_epoch);
        EXPECT_EQ(actual.center.x, expected.center.x);
        EXPECT_EQ(actual.center.y, expected.center.y);
        EXPECT_EQ(actual.center.z, expected.center.z);
        EXPECT_EQ(actual.center.x_subcell, expected.center.x_subcell);
        EXPECT_EQ(actual.center.y_subcell, expected.center.y_subcell);
        EXPECT_EQ(actual.center.z_subcell, expected.center.z_subcell);
        EXPECT_EQ(actual.actual_radius_blocks, expected.actual_radius_blocks);
        EXPECT_EQ(actual.visual_lod_radius_blocks, expected.visual_lod_radius_blocks);
    } else {
        EXPECT_EQ(actual.key, expected.key);
        EXPECT_EQ(actual.revision, expected.revision);
        EXPECT_EQ(actual.token, expected.token);
        if constexpr (std::is_same_v<MessageTy, shared::ServerHeightTileMessage>) {
            EXPECT_EQ(actual.heights, expected.heights);
        }
    }
    EXPECT_EQ(shared::encodeMessage(actual), bytes);
}

} // namespace

TEST(MessageTest, RoundTripsEveryMessageKind) {
    expectRoundTrip(shared::JoinRequestMessage{ .ch = '@', .join_epoch = 17U });
    expectRoundTrip(shared::JoinResponseMessage{ .accepted = true, .join_epoch = 17U });
    expectRoundTrip(shared::JoinResponseMessage{ .accepted = false, .join_epoch = 17U });
    expectRoundTrip(shared::ClientInputMessage{
        .direction = { .x = 129U, .y = 127U, .z = 1U, .cycle_movement_capabilities = true },
        .sequence = 0x7856'3412U,
    });
    expectRoundTrip(shared::ClientHeightTileCreditMessage{
        .world_revision = 7U,
        .delivery_token = 11U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 17, .y = 23 }, { .x = 19, .y = 29 }},
    });
    expectRoundTrip(shared::ServerPlayerPositionMessage{
        .ch = '#', .palette_index = 7U, .movement_capabilities = { .bits = 3U },
        .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
        .vertical_velocity_subcells = 5'250,
        .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
    });
    expectRoundTrip(shared::ServerRemovePlayerMessage{ .ch = '$' });
    auto descriptor = heightTileDescriptorMessage();
    expectRoundTrip(descriptor);
    descriptor.max_height_tiles = shared::heightTileInterestCount(128U);
    expectRoundTrip(descriptor);
    descriptor.max_height_tiles = shared::heightTileInterestCount(256U);
    expectRoundTrip(descriptor);
    expectRoundTrip(shared::ServerWorldRevisionMessage{});
    expectRoundTrip(heightTileMessage());
    expectRoundTrip(heightTileBatchMessage());
    expectRoundTrip(shared::ServerRemoveHeightTileMessage{
        .key = {.x = 17, .y = 23},
        .revision = 7U,
        .token = 12U,
    });
    expectRoundTrip(worldPreparationMessage());
    auto complete_preparation = worldPreparationMessage();
    complete_preparation.sequence = 3U;
    complete_preparation.stage = generation::GenerationStage::Complete;
    complete_preparation.work_completed = 100U;
    complete_preparation.work_total = 100U;
    complete_preparation.latest_snapshot.reset();
    complete_preparation.final_atlas_complete = true;
    complete_preparation.refined_safe_spawn = shared::PlayerPosition{
        .x = 8,
        .y = 12,
        .z = 400,
    };
    expectRoundTrip(complete_preparation);

    static constexpr std::array<shared::Block, 7> MATERIALS{
        shared::Block::Air,
        shared::Block::Stone,
        shared::Block::Grass,
        shared::Block::Dirt,
        shared::Block::Bedrock,
        shared::Block::Snow,
        shared::Block::Ice,
    };
    std::vector<shared::MaterializedChunkPayload> chunks;
    chunks.reserve(MATERIALS.size() + 1U);
    for (uint32_t index = 0U; index < MATERIALS.size(); ++index) {
        chunks.push_back(uniformChunkPayload(
            { .x = static_cast<int32_t>(index), .y = 0, .z = 0 },
            MATERIALS[index]
        ));
    }
    chunks.push_back(denseChunkPayload({ .x = 7, .y = 0, .z = 0 }));
    expectRoundTrip(shared::ServerMaterializedChunkBatchMessage{
        .session = terrainSessionIdentity(),
        .delivery_token = 55U,
        .chunks = chunks,
    });
    expectRoundTrip(shared::ClientChunkDeliveryAckMessage{
        .session = terrainSessionIdentity(),
        .delivery_token = 0U,
        .credits = shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW,
        .not_resident_keys = {},
    });
    expectRoundTrip(shared::ClientChunkDeliveryAckMessage{
        .session = terrainSessionIdentity(),
        .delivery_token = 55U,
        .credits = 1U,
        .not_resident_keys = { { .x = 7, .y = 0, .z = 0 } },
    });
    expectRoundTrip(shared::ClientStartWorldMessage{ .session = terrainSessionIdentity() });
    expectRoundTrip(shared::ServerStartWorldAckMessage{
        .session = terrainSessionIdentity(),
        .accepted = true,
        .failure = {},
    });
    expectRoundTrip(shared::ServerStartWorldAckMessage{
        .session = terrainSessionIdentity(),
        .accepted = false,
        .failure = "World start rejected",
    });
    expectRoundTrip(shared::ServerChunkCoverageMessage{
        .session = terrainSessionIdentity(),
        .coverage_epoch = 3U,
        .center = {
            .x = 32,
            .y = 48,
            .z = 128,
            .x_subcell = 127U,
            .y_subcell = 9U,
            .z_subcell = 43U,
        },
        .actual_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS,
        .visual_lod_radius_blocks = shared::FULL_WORLD_VISUAL_LOD_RADIUS_BLOCKS,
    });
}

TEST(MessageTest, DecodesV20ClientStartAndRejectsV19AgainstItsJoinEpochAndTerrainIdentity)
{
    std::vector<uint8_t> const packet = validV20ClientStartPacket();
    auto const decoded = shared::decodeMessage(packet);

    ASSERT_TRUE(decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ClientStartWorldMessage>(*decoded));
    EXPECT_EQ(std::get<shared::ClientStartWorldMessage>(*decoded).session.join_epoch, 1U);
    EXPECT_EQ(
        std::get<shared::ClientStartWorldMessage>(*decoded).session.terrain.configuration_identity,
        terrainConfigurationIdentity()
    );

    std::vector<uint8_t> const previous_version_packet = [&packet] {
        std::vector<uint8_t> previous_version = packet;
        previous_version[1] = 19U;
        return previous_version;
    }();
    EXPECT_FALSE(shared::decodeMessage(previous_version_packet).has_value());
}

TEST(MessageTest, CarriesBoundedFullResolutionPreparationSnapshotWithoutTruncation)
{
    auto snapshot = std::make_shared<generation::GenerationSnapshot>();
    snapshot->stage = generation::GenerationStage::FinalClimate;
    snapshot->resolution = shared::MAX_GENERATION_SNAPSHOT_RESOLUTION;
    uint32_t const sample_count = shared::MAX_GENERATION_SNAPSHOT_SAMPLES;
    snapshot->elevation.resize(sample_count, 0.25F);
    snapshot->temperature.resize(sample_count, -5.0F);
    snapshot->precipitation.resize(sample_count, 0.5F);
    snapshot->water_level.resize(sample_count, 0.1F);
    snapshot->ocean_mask.resize(sample_count, 1U);
    snapshot->lake_mask.resize(sample_count, 0U);
    snapshot->river_mask.resize(sample_count, 1U);

    shared::ServerWorldPreparationMessage const preparation{
        .session = terrainSessionIdentity(),
        .sequence = 4U,
        .stage = generation::GenerationStage::FinalClimate,
        .work_completed = 9U,
        .work_total = 12U,
        .server_elapsed_microseconds = 16'000'000U,
        .latest_snapshot = snapshot,
        .refined_safe_spawn = std::nullopt,
        .failure = {},
    };
    std::vector<uint8_t> const packet = shared::encodeMessage(preparation);
    ASSERT_LE(packet.size(), shared::MAX_MESSAGE_SIZE_BYTES);
    ASSERT_GT(packet.size(), 4'980'000U);
    auto const decoded = shared::decodeMessage(packet);
    ASSERT_TRUE(decoded.has_value());
    auto const* const actual = std::get_if<shared::ServerWorldPreparationMessage>(&*decoded);
    ASSERT_NE(actual, nullptr);
    ASSERT_NE(actual->latest_snapshot, nullptr);
    EXPECT_EQ(actual->latest_snapshot->resolution, shared::MAX_GENERATION_SNAPSHOT_RESOLUTION);
    EXPECT_EQ(actual->latest_snapshot->elevation, snapshot->elevation);
    EXPECT_EQ(actual->latest_snapshot->river_mask, snapshot->river_mask);

    std::vector<uint8_t> oversized(shared::MAX_MESSAGE_SIZE_BYTES + 1U, 0U);
    EXPECT_FALSE(shared::decodeMessage(oversized).has_value());
}

TEST(MessageTest, RejectsInvalidWorldPreparationBeforeClientStateCanConsumeIt)
{
    auto preparation = worldPreparationMessage();
    preparation.session.join_epoch = 0U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());

    preparation = worldPreparationMessage();
    ++preparation.session.configuration.seed;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());

    preparation = worldPreparationMessage();
    preparation.sequence = 0U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());

    preparation = worldPreparationMessage();
    preparation.work_completed = preparation.work_total + 1U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());

    preparation = worldPreparationMessage();
    auto invalid_snapshot = std::make_shared<generation::GenerationSnapshot>(*preparation.latest_snapshot);
    invalid_snapshot->elevation[0U] = std::numeric_limits<float>::infinity();
    preparation.latest_snapshot = invalid_snapshot;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());

    preparation = worldPreparationMessage();
    invalid_snapshot = std::make_shared<generation::GenerationSnapshot>(*preparation.latest_snapshot);
    invalid_snapshot->ocean_mask[0U] = 2U;
    preparation.latest_snapshot = invalid_snapshot;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());

    preparation = worldPreparationMessage();
    preparation.stage = generation::GenerationStage::Complete;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());

    preparation = worldPreparationMessage();
    preparation.failure.assign(shared::MAX_WORLD_PREPARATION_FAILURE_BYTES + 1U, 'x');
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(preparation)).has_value());
}

TEST(MessageTest, MaterializesUniformAndDenseChunksUsingCanonicalContentHashes)
{
    static constexpr std::array<shared::Block, 7> MATERIALS{
        shared::Block::Air,
        shared::Block::Stone,
        shared::Block::Grass,
        shared::Block::Dirt,
        shared::Block::Bedrock,
        shared::Block::Snow,
        shared::Block::Ice,
    };
    for (uint32_t index = 0U; index < MATERIALS.size(); ++index) {
        auto const payload = uniformChunkPayload(
            { .x = static_cast<int32_t>(index), .y = 3, .z = 2 },
            MATERIALS[index]
        );
        auto const chunk = shared::materializeChunkPayload(payload);
        ASSERT_TRUE(chunk.has_value());
        EXPECT_EQ(chunk->coordinate(), payload.coordinate);
        EXPECT_EQ(chunk->contentIdentity().content_hash, payload.content_hash);
        EXPECT_EQ(chunk->blockAt({ .x = 15U, .y = 15U, .z = 15U }), MATERIALS[index]);
    }

    std::unique_ptr<shared::MaterializedChunkPayload const> const dense_owner{
        new shared::MaterializedChunkPayload{denseChunkPayload({ .x = 7, .y = 3, .z = 2 })}
    };
    auto const& dense = *dense_owner;
    auto const materialized = shared::materializeChunkPayload(dense);
    ASSERT_TRUE(materialized.has_value());
    EXPECT_EQ(materialized->blockAt({ .x = 0U, .y = 0U, .z = 0U }), shared::Block::Air);
    EXPECT_EQ(materialized->blockAt({ .x = 6U, .y = 0U, .z = 0U }), shared::Block::Ice);
    EXPECT_EQ(materialized->blockAt({ .x = 0U, .y = 1U, .z = 0U }), shared::Block::Grass);
}

TEST(MessageTest, RejectsChunkMaterialHashesInvalidIdsCoordinatesAndBatchBounds)
{
    auto payload = uniformChunkPayload({ .x = 4, .y = 5, .z = 6 }, shared::Block::Snow);
    payload.content_hash ^= 1U;
    EXPECT_FALSE(shared::materializeChunkPayload(payload).has_value());
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ServerMaterializedChunkBatchMessage{
        .session = terrainSessionIdentity(),
        .delivery_token = 1U,
        .chunks = { payload },
    })).has_value());

    payload = uniformChunkPayload({ .x = 4, .y = 5, .z = 6 }, shared::Block::Snow);
    payload.data = shared::UniformChunkData{ .material = static_cast<shared::Block>(7U) };
    EXPECT_FALSE(shared::materializeChunkPayload(payload).has_value());

    payload = uniformChunkPayload({ .x = 4'096, .y = 5, .z = 6 }, shared::Block::Stone);
    EXPECT_FALSE(shared::materializeChunkPayload(payload).has_value());

    shared::ServerMaterializedChunkBatchMessage batch{
        .session = terrainSessionIdentity(),
        .coverage_epoch = 2U,
        .delivery_token = 1U,
        .chunks = {
            uniformChunkPayload({ .x = 4, .y = 5, .z = 6 }, shared::Block::Stone),
            uniformChunkPayload({ .x = 4, .y = 5, .z = 6 }, shared::Block::Dirt),
        },
    };
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(batch)).has_value());

    batch.chunks.clear();
    batch.chunks.resize(shared::MATERIALIZED_CHUNK_BATCH_CAPACITY + 1U);
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(batch)).has_value());

    batch.coverage_epoch = 0U;
    batch.chunks = { uniformChunkPayload({ .x = 4, .y = 5, .z = 6 }, shared::Block::Stone) };
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(batch)).has_value());
}

TEST(MessageTest, EnforcesPerJoinChunkCreditAcknowledgementBounds)
{
    shared::TerrainSessionIdentity const session = terrainSessionIdentity();
    EXPECT_TRUE(shared::decodeMessage(shared::encodeMessage(shared::ClientChunkDeliveryAckMessage{
        .session = session,
        .delivery_token = 0U,
        .credits = shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW,
        .not_resident_keys = {},
    })).has_value());

    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ClientChunkDeliveryAckMessage{
        .session = session,
        .delivery_token = 0U,
        .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
        .not_resident_keys = {},
    })).has_value());

    auto ack = shared::ClientChunkDeliveryAckMessage{
        .session = session,
        .coverage_epoch = 4U,
        .delivery_token = 0U,
        .credits = 1U,
        .not_resident_keys = { { .x = 1, .y = 2, .z = 3 } },
    };
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)).has_value());

    ack.delivery_token = 12U;
    ack.credits = 2U;
    ack.not_resident_keys.clear();
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)).has_value());

    ack.credits = 1U;
    ack.not_resident_keys = { { .x = 1, .y = 2, .z = 3 }, { .x = 1, .y = 2, .z = 3 } };
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)).has_value());

    ack.not_resident_keys = { { .x = 1, .y = 2, .z = 64 } };
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)).has_value());

    ack.not_resident_keys.clear();
    ack.credits = shared::MATERIALIZED_CHUNK_DELIVERY_WINDOW + 1U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)).has_value());

    ack.coverage_epoch = 0U;
    ack.credits = 1U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)).has_value());
}

TEST(MessageTest, KeepsHeightTileCreditWindowAtEight)
{
    auto credit = shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 0U,
        .credits = shared::HEIGHT_TILE_DELIVERY_WINDOW,
        .not_resident_keys = {},
    };
    EXPECT_TRUE(shared::decodeMessage(shared::encodeMessage(credit)).has_value());

    credit.credits = shared::HEIGHT_TILE_DELIVERY_WINDOW + 1U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(credit)).has_value());
}

TEST(MessageTest, RejectsInvalidChunkCoverageEpochAndCenter)
{
    auto coverage = shared::ServerChunkCoverageMessage{
        .session = terrainSessionIdentity(),
        .coverage_epoch = 1U,
        .center = { .x = 0, .y = 0, .z = 0 },
    };
    EXPECT_TRUE(shared::decodeMessage(shared::encodeMessage(coverage)).has_value());

    coverage.coverage_epoch = 0U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(coverage)).has_value());

    coverage.coverage_epoch = 2U;
    coverage.center.z = 65'536;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(coverage)).has_value());

    coverage.center.z = 65'535;
    EXPECT_TRUE(shared::decodeMessage(shared::encodeMessage(coverage)).has_value());

    coverage.center.z = 0;
    coverage.center.x_subcell = shared::SUBCELLS_PER_CELL;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(coverage)).has_value());

    coverage.center.x_subcell = 0U;
    coverage.actual_radius_blocks = 320U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(coverage)).has_value());

    coverage.actual_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS;
    coverage.visual_lod_radius_blocks = shared::ACTUAL_TERRAIN_RADIUS_BLOCKS;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(coverage)).has_value());
}

TEST(MessageTest, PlayerPositionCodecAcceptsEntityHeightBeyondFiniteTerrain)
{
    shared::ServerPlayerPositionMessage position{
        .ch = '@',
        .x = 32'768,
        .y = 32'768,
        .z = 65'535,
        .x_subcell = 0U,
        .y_subcell = 0U,
        .z_subcell = 0U,
        .acknowledged_input_sequence = 1U,
        .state_revision = 1U,
    };

    EXPECT_TRUE(shared::decodeMessage(shared::encodeMessage(position)).has_value());

    position.z = 65'536;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(position)).has_value());
}

TEST(MessageTest, PreservesEveryStableBlockIdInSingleAndBatchTileCodecs)
{
    static constexpr std::array<shared::Block, 7> MATERIALS{
        shared::Block::Air,
        shared::Block::Stone,
        shared::Block::Grass,
        shared::Block::Dirt,
        shared::Block::Bedrock,
        shared::Block::Snow,
        shared::Block::Ice,
    };
    auto verify_materials = [](shared::ServerHeightTileMessage const& source) {
        auto const packet = shared::encodeMessage(source);
        auto const decoded = shared::decodeMessage(packet);
        ASSERT_TRUE(decoded.has_value());
        auto const* const tile = std::get_if<shared::ServerHeightTileMessage>(&*decoded);
        ASSERT_NE(tile, nullptr);
        EXPECT_EQ(tile->surface.identity, source.surface.identity);
        EXPECT_EQ(tile->surface.coordinate, source.surface.coordinate);
        EXPECT_EQ(tile->surface.block_sea_level, source.surface.block_sea_level);
        EXPECT_EQ(tile->surface.max_height, source.surface.max_height);
        EXPECT_EQ(tile->surface.columns[1U].top_material, shared::Block::Snow);
        EXPECT_EQ(tile->surface.columns[1U].ground_material, shared::Block::Ice);
        std::array<shared::Block, 7> const observed{
            tile->surface.columns[0U].materialAt(391U, tile->surface.block_sea_level),
            tile->surface.columns[0U].materialAt(1U, tile->surface.block_sea_level),
            tile->surface.columns[0U].materialAt(390U, tile->surface.block_sea_level),
            tile->surface.columns[0U].materialAt(389U, tile->surface.block_sea_level),
            tile->surface.columns[0U].materialAt(0U, tile->surface.block_sea_level),
            tile->surface.columns[1U].materialAt(390U, tile->surface.block_sea_level),
            tile->surface.columns[1U].materialAt(389U, tile->surface.block_sea_level),
        };
        EXPECT_EQ(observed, MATERIALS);
    };

    shared::ServerHeightTileMessage const tile = materialHeightTileMessage();
    verify_materials(tile);

    shared::ServerHeightTileMessage second = tile;
    second.key = { .x = tile.key.x + 1, .y = tile.key.y };
    second.surface.coordinate = { .x = second.key.x, .y = second.key.y, .z = 0 };
    shared::ServerHeightTileBatchMessage const batch{
        .delivery_token = 33U,
        .tiles = { tile, second },
        .removals = {},
    };
    auto const packet = shared::encodeMessage(batch);
    auto const decoded = shared::decodeMessage(packet);
    ASSERT_TRUE(decoded.has_value());
    auto const* const decoded_batch = std::get_if<shared::ServerHeightTileBatchMessage>(&*decoded);
    ASSERT_NE(decoded_batch, nullptr);
    ASSERT_EQ(decoded_batch->tiles.size(), 2U);
    verify_materials(decoded_batch->tiles[0U]);
    verify_materials(decoded_batch->tiles[1U]);
}

TEST(MessageTest, RejectsTruncatedAndUnknownPayloads) {
    std::array<shared::Message, 11> const messages{
        shared::JoinRequestMessage{ .ch = '@' },
        shared::JoinResponseMessage{ .accepted = true },
        shared::ClientInputMessage{ .direction = { 129, 127, 1 }, .sequence = 0x7856'3412U },
        shared::ClientHeightTileCreditMessage{
            .world_revision = 7U,
            .delivery_token = 11U,
            .credits = 1U,
            .not_resident_keys = {},
        },
        shared::ServerPlayerPositionMessage{
            .ch = '#', .palette_index = 7U, .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
            .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
        },
        shared::ServerRemovePlayerMessage{ .ch = '$' },
        heightTileDescriptorMessage(),
        shared::ServerWorldRevisionMessage{},
        heightTileMessage(),
        heightTileBatchMessage(),
        shared::ServerRemoveHeightTileMessage{
            .key = {.x = 17, .y = 23},
            .revision = 7U,
            .token = 12U,
        },
    };
    for (auto const& message : messages) {
        auto const bytes = shared::encodeMessage(message);
        for (uint64_t size = 0; size < bytes.size(); ++size) {
            EXPECT_FALSE(shared::decodeMessage(std::span{ bytes.data(), size }).has_value());
        }
    }
    std::array<uint8_t, 4> unknown{ shared::PROTOCOL_MAGIC, shared::PROTOCOL_VERSION, 255, 0 };
    EXPECT_FALSE(shared::decodeMessage(unknown).has_value());
}

TEST(MessageTest, RejectsTruncatedCreditAcknowledgementToken)
{
    std::vector<uint8_t> bytes = shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 7U,
        .delivery_token = 0x1234'5678'9ABC'DEF0U,
        .credits = 1U,
        .not_resident_keys = {},
    });
    bytes.erase(bytes.end() - 5, bytes.end() - 1);

    EXPECT_FALSE(shared::decodeMessage(bytes).has_value());
}

TEST(MessageTest, BoundsAndValidatesTileResidencyReceipts)
{
    static constexpr uint8_t RECEIPT_CAPACITY = shared::HEIGHT_TILE_RESIDENCY_RECEIPT_KEY_CAPACITY;
    std::vector<shared::HeightTileKey> keys;
    keys.reserve(RECEIPT_CAPACITY);
    for (uint8_t index = 0U; index < RECEIPT_CAPACITY; ++index) {
        keys.push_back({ .x = index, .y = 0 });
    }
    shared::ClientHeightTileCreditMessage const maximum_receipt{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = keys,
    };
    std::vector<uint8_t> const maximum_bytes = shared::encodeMessage(maximum_receipt);
    EXPECT_EQ(maximum_bytes.size(), 3U + 18U + 8U * RECEIPT_CAPACITY);
    EXPECT_TRUE(shared::decodeMessage(maximum_bytes).has_value());

    keys.push_back({ .x = 100, .y = 0 });
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = keys,
    })).has_value());

    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 1, .y = 2 }, { .x = 1, .y = 2 }},
    })).has_value());
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 4'096, .y = 0 }},
    })).has_value());
    std::vector<uint8_t> truncated_receipt = shared::encodeMessage(shared::ClientHeightTileCreditMessage{
        .world_revision = 1U,
        .delivery_token = 1U,
        .credits = 1U,
        .not_resident_keys = {{ .x = 1, .y = 2 }},
    });
    truncated_receipt.pop_back();
    EXPECT_FALSE(shared::decodeMessage(truncated_receipt).has_value());
}

TEST(MessageTest, UsesVersionedLittleEndianFixedWidthPayloads) {
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' }),
        (std::vector<uint8_t>{
            0x4D, 20, 0, '@', 0,
            1, 0, 0, 0,
            42, 0, 0, 0, 0, 0, 0, 0,
            16, 16, 16,
            101, 252, 205, 108, 74, 88, 174, 176, 0,
            1, 0, 0, 0, 0, 0, 0, 0,
        })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = true }),
        (std::vector<uint8_t>{ 0x4D, 20, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::JoinResponseMessage{ .accepted = false }),
        (std::vector<uint8_t>{ 0x4D, 20, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ClientInputMessage{ .direction = { 129, 127, 1 }, .sequence = 0x7856'3412U }),
        (std::vector<uint8_t>{ 0x4D, 20, 2, 129, 127, 1, 0, 5, 0, 0, 0, 127, 18, 52, 86, 120 })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerPlayerPositionMessage{
            .ch = '#', .palette_index = 7U, .x = 30, .y = 2, .z = 12, .x_subcell = 9'999, .y_subcell = 500, .z_subcell = 1,
            .vertical_velocity_subcells = 5'250,
            .acknowledged_input_sequence = 0x7856'3412U, .state_revision = 0x1234'5678U,
        }),
        (std::vector<uint8_t>{
            0x4D, 20, 4, '#', 7, 0,
            30, 0, 0, 0,
            2, 0, 0, 0,
            12, 0, 0, 0,
            15, 39, 244, 1, 1, 0,
            0x82, 0x14, 0, 0,
            18, 52, 86, 120, 120, 86, 52, 18,
        })
    );
    EXPECT_EQ(
        shared::encodeMessage(shared::ServerRemovePlayerMessage{ .ch = '$' }),
        (std::vector<uint8_t>{ 0x4D, 20, 5, '$' })
    );
    auto const descriptor = heightTileDescriptorMessage();
    auto const descriptor_packet = shared::encodeMessage(descriptor);
    EXPECT_EQ(
        descriptor_packet.size(),
        3U + 61U + descriptor.world_identity.configuration_identity.size()
    );
    auto const height_tile = shared::encodeMessage(heightTileMessage());
    EXPECT_EQ(height_tile.size(), 3U + shared::encodedHeightTileSize(heightTileMessage()));
    EXPECT_EQ(height_tile[0], shared::PROTOCOL_MAGIC);
    EXPECT_EQ(height_tile[1], shared::PROTOCOL_VERSION);
    EXPECT_EQ(height_tile[2], 8U);
    auto const height_tile_batch = shared::encodeMessage(heightTileBatchMessage());
    EXPECT_EQ(
        height_tile_batch.size(),
        3U + 10U + 2U * shared::encodedHeightTileSize(heightTileMessage())
    );
    EXPECT_EQ(height_tile_batch[2], 10U);
}

TEST(MessageTest, EncodesHeightTilesWithCanonicalLosslessBitPacking)
{
    shared::ServerHeightTileMessage constant{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    constant.heights.fill(1021U);
    synchronizeSurface(constant);
    auto const constant_packet = shared::encodeMessage(constant);
    EXPECT_EQ(constant_packet[0], shared::PROTOCOL_MAGIC);
    EXPECT_EQ(constant_packet[1], shared::PROTOCOL_VERSION);
    EXPECT_EQ(constant_packet[2], 8U);
    EXPECT_EQ(constant_packet[27], 0U);
    EXPECT_EQ(constant_packet[28], 0xFDU);
    EXPECT_EQ(constant_packet[29], 0x03U);
    EXPECT_EQ(constant_packet.size(), 3U + 27U + surfaceWireSize(constant.surface));
    EXPECT_EQ(shared::encodedHeightTileSize(constant), constant_packet.size() - 3U);
    auto const constant_decoded = shared::decodeMessage(constant_packet);
    ASSERT_TRUE(constant_decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ServerHeightTileMessage>(*constant_decoded));
    EXPECT_EQ(std::get<shared::ServerHeightTileMessage>(*constant_decoded).heights, constant.heights);

    shared::ServerHeightTileMessage alternating{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index = 0U; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        alternating.heights[index] = static_cast<uint16_t>(200U + (index & 1U));
    }
    synchronizeSurface(alternating);
    auto const alternating_packet = shared::encodeMessage(alternating);
    EXPECT_EQ(alternating_packet[27], 1U);
    EXPECT_EQ(alternating_packet[28], 200U);
    EXPECT_EQ(alternating_packet[29], 0U);
    EXPECT_TRUE(std::all_of(alternating_packet.begin() + 30, alternating_packet.begin() + 62,
        [](uint8_t const byte) { return byte == 0xAAU; }));
    EXPECT_EQ(alternating_packet.size(), 3U + 59U + surfaceWireSize(alternating.surface));
    EXPECT_EQ(shared::encodedHeightTileSize(alternating), alternating_packet.size() - 3U);
    auto const alternating_decoded = shared::decodeMessage(alternating_packet);
    ASSERT_TRUE(alternating_decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ServerHeightTileMessage>(*alternating_decoded));
    EXPECT_EQ(std::get<shared::ServerHeightTileMessage>(*alternating_decoded).heights, alternating.heights);

    shared::ServerHeightTileMessage valid_range{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index = 0U; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        valid_range.heights[index] = static_cast<uint16_t>(index * 4U + 2U);
    }
    valid_range.heights.back() = 1021U;
    synchronizeSurface(valid_range);
    auto const valid_range_packet = shared::encodeMessage(valid_range);
    EXPECT_EQ(valid_range_packet[27], 10U);
    EXPECT_EQ(valid_range_packet[28], 2U);
    EXPECT_EQ(valid_range_packet[29], 0U);
    EXPECT_EQ(valid_range_packet.size(), 3U + 27U + 320U + surfaceWireSize(valid_range.surface));
    EXPECT_EQ(shared::encodedHeightTileSize(valid_range), valid_range_packet.size() - 3U);
    auto const valid_range_decoded = shared::decodeMessage(valid_range_packet);
    ASSERT_TRUE(valid_range_decoded.has_value());
    ASSERT_TRUE(std::holds_alternative<shared::ServerHeightTileMessage>(*valid_range_decoded));
    EXPECT_EQ(std::get<shared::ServerHeightTileMessage>(*valid_range_decoded).heights, valid_range.heights);

    expectRoundTrip(heightTileMessage());
}

TEST(MessageTest, RejectsMalformedHeightTileEncodings)
{
    shared::ServerHeightTileMessage alternating{
        .key = { .x = 17, .y = 23 },
        .revision = 7U,
        .token = 11U,
    };
    for (uint32_t index = 0U; index < shared::HEIGHT_TILE_SAMPLE_COUNT; ++index) {
        alternating.heights[index] = static_cast<uint16_t>(200U + (index & 1U));
    }
    synchronizeSurface(alternating);
    std::vector<uint8_t> const valid_packet = shared::encodeMessage(alternating);

    auto invalid_width = valid_packet;
    invalid_width[27] = 17U;
    EXPECT_FALSE(shared::decodeMessage(invalid_width).has_value());

    auto truncated = valid_packet;
    truncated.pop_back();
    EXPECT_FALSE(shared::decodeMessage(truncated).has_value());

    auto overflowing_base = valid_packet;
    overflowing_base[28] = 0xFFU;
    overflowing_base[29] = 0xFFU;
    EXPECT_FALSE(shared::decodeMessage(overflowing_base).has_value());

    auto trailing = valid_packet;
    trailing.push_back(0U);
    EXPECT_FALSE(shared::decodeMessage(trailing).has_value());
}

TEST(MessageTest, RejectsInvalidSurfaceIdsBoundsAndIdentity)
{
    auto tile = materialHeightTileMessage();
    tile.surface.columns[0U].top_material = static_cast<shared::Block>(7U);
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    tile = materialHeightTileMessage();
    ++tile.heights[0U];
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    tile = materialHeightTileMessage();
    tile.key.x = -1;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    tile = materialHeightTileMessage();
    tile.surface.columns[0U].highest_solid_z = generation::MAX_HEIGHT + 1U;
    tile.heights[0U] = generation::MAX_HEIGHT + 2U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    tile = materialHeightTileMessage();
    tile.surface.max_height--;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    tile = materialHeightTileMessage();
    tile.surface.columns[0U].temperature_celsius = std::numeric_limits<double>::infinity();
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    tile = materialHeightTileMessage();
    tile.surface.identity.revision++;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    tile = materialHeightTileMessage();
    tile.surface.identity.configuration_identity.assign(
        shared::TERRAIN_CONFIGURATION_IDENTITY_MAX_BYTES + 1U,
        'x'
    );
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(tile)).has_value());

    auto descriptor = heightTileDescriptorMessage();
    descriptor.world_identity.revision++;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(descriptor)).has_value());

    shared::ServerHeightTileBatchMessage batch{
        .delivery_token = 1U,
        .tiles = { materialHeightTileMessage() },
        .removals = {},
    };
    batch.tiles[0U].surface.columns[0U].ground_material = static_cast<shared::Block>(255U);
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(batch)).has_value());
}

TEST(MessageTest, RejectsNoncanonicalAcceptanceByte) {
    for (uint16_t value = 2; value <= 255; ++value) {
        std::array<uint8_t, 12> const bytes{
            shared::PROTOCOL_MAGIC,
            shared::PROTOCOL_VERSION,
            1U,
            static_cast<uint8_t>(value),
            1U,
            0U,
            0U,
            0U,
            0U,
            0U,
            0U,
            0U,
        };
        EXPECT_FALSE(shared::decodeMessage(bytes).has_value());
    }
}

TEST(MessageTest, RejectsTrailingBytes) {
    auto bytes = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    bytes.push_back(0);
    auto const decoded = shared::decodeMessage(std::span<uint8_t const>{ bytes.data(), bytes.size() });
    EXPECT_FALSE(decoded.has_value());
}

TEST(MessageTest, RejectsEmptyOversizedMalformedAndTrailingHeightTileBatches)
{
    shared::ServerHeightTileBatchMessage empty;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(empty)).has_value());

    shared::ServerHeightTileBatchMessage oversized;
    oversized.tiles.resize(shared::HEIGHT_TILE_BATCH_CAPACITY + 1U, heightTileMessage());
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(oversized)).has_value());

    auto malformed = heightTileBatchMessage();
    malformed.tiles[1].heights[0] = std::numeric_limits<uint16_t>::max();
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(malformed)).has_value());

    auto trailing = shared::encodeMessage(heightTileBatchMessage());
    trailing.push_back(0U);
    EXPECT_FALSE(shared::decodeMessage(trailing).has_value());
}

TEST(MessageTest, RejectsInvalidPayloadValues) {
    auto input = shared::encodeMessage(shared::ClientInputMessage{ .direction = { 127, 127 }, .sequence = 1U });
    input[3] = 128;
    EXPECT_FALSE(shared::decodeMessage(input).has_value());

    input = shared::encodeMessage(shared::ClientInputMessage{
        .direction = { .x = 127, .y = 127, .accelerated = true, .speedup = 5U },
        .sequence = 1U,
    });
    input[7] = 4U;
    EXPECT_FALSE(shared::decodeMessage(input).has_value());

    auto position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
        .acknowledged_input_sequence = 0U, .state_revision = 1U,
    });
    position[4] = shared::PLAYER_PALETTE_COUNT;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    position = shared::encodeMessage(shared::ServerPlayerPositionMessage{
        .ch = '@', .x = 1, .y = 1, .x_subcell = 0, .y_subcell = 0,
        .acknowledged_input_sequence = 0U, .state_revision = 1U,
    });
    position[17] = 16;
    position[18] = 39;
    EXPECT_FALSE(shared::decodeMessage(position).has_value());

    auto join = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '\n' });
    EXPECT_FALSE(shared::decodeMessage(join).has_value());

    auto invalid_tile = heightTileMessage();
    invalid_tile.heights.fill(1025U);
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(invalid_tile)).has_value());
}

TEST(MessageTest, RejectsOldAndMixedProtocolVersions) {
    std::array<uint8_t, 2> const old_packet{ 0, '@' };
    EXPECT_FALSE(shared::decodeMessage(old_packet).has_value());

    auto packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[0] = 0x4C;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[1] = 16U;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[1] = 11U;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet[1] = 10U;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[4] = 2;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());
}

TEST(MessageTest, MapsExactDiskCardinalitiesToEverySupportedRadius)
{
    static constexpr uint32_t MINIMUM_RADIUS = 1U;
    static constexpr uint32_t MAXIMUM_RADIUS = 256U;
    static constexpr uint32_t DEFAULT_COUNT = shared::HEIGHT_TILE_INTEREST_COUNT;
    static constexpr std::array<uint32_t, 4> INVALID_COUNTS{
        0U,
        DEFAULT_COUNT - 1U,
        DEFAULT_COUNT + 1U,
        shared::heightTileInterestCount(257U),
    };

    EXPECT_EQ(shared::PROTOCOL_VERSION, 20U);
    EXPECT_EQ(shared::HEIGHT_TILE_BATCH_CAPACITY, 64U);
    EXPECT_EQ(shared::HEIGHT_TILE_RESIDENCY_RECEIPT_KEY_CAPACITY, 128U);
    EXPECT_EQ(
        shared::encodedHeightTileSize(heightTileMessage()),
        283U + surfaceWireSize(heightTileMessage().surface)
    );
    EXPECT_FALSE(shared::isValidHeightTileInterestRadius(0U));
    EXPECT_FALSE(shared::isValidHeightTileInterestRadius(257U));
    for (uint32_t radius = MINIMUM_RADIUS; radius <= MAXIMUM_RADIUS; ++radius) {
        ASSERT_TRUE(shared::isValidHeightTileInterestRadius(radius));
        auto const count = shared::heightTileInterestCount(radius);
        ASSERT_EQ(shared::heightTileInterestRadiusForCount(count), std::optional{radius});
        if (radius > MINIMUM_RADIUS) {
            EXPECT_GT(count, shared::heightTileInterestCount(radius - 1U));
        }
    }
    for (uint32_t const count : INVALID_COUNTS) {
        EXPECT_EQ(shared::heightTileInterestRadiusForCount(count), std::nullopt);
    }
}

TEST(MessageTest, RejectsMalformedHeightTileDescriptorCardinalities)
{
    static constexpr uint32_t DEFAULT_COUNT = shared::HEIGHT_TILE_INTEREST_COUNT;
    static constexpr std::array<uint32_t, 4> INVALID_COUNTS{
        0U,
        DEFAULT_COUNT - 1U,
        DEFAULT_COUNT + 1U,
        shared::heightTileInterestCount(257U),
    };

    for (uint32_t const count : INVALID_COUNTS) {
        auto descriptor = heightTileDescriptorMessage();
        descriptor.max_height_tiles = count;
        auto const packet = shared::encodeMessage(descriptor);
        EXPECT_FALSE(shared::decodeMessage(packet).has_value()) << "count " << count;
    }
}

TEST(MessageTest, RejectsInvalidWorldConfiguration) {
    auto packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[5] = 0;
    packet[6] = 0;
    packet[7] = 0;
    packet[8] = 0;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());

    packet = shared::encodeMessage(shared::JoinRequestMessage{ .ch = '@' });
    packet[17] = 0;
    EXPECT_FALSE(shared::decodeMessage(packet).has_value());
}

TEST(MessageTest, OrdersSequencesAcrossWrapButRejectsTheHalfRangeTie)
{
    EXPECT_TRUE(shared::isNewerSequence(1U, 0U));
    EXPECT_TRUE(shared::isNewerSequence(0U, UINT32_MAX));
    EXPECT_FALSE(shared::isNewerSequence(0U, 0U));
    EXPECT_FALSE(shared::isNewerSequence(0x8000'0000U, 0U));
    EXPECT_FALSE(shared::isNewerSequence(0U, 0x8000'0000U));
}

TEST(MessageTest, AuthoritativeProfilesRoundTripExactEightByteColumnsWithSessionAndEpoch)
{
    auto const session = terrainSessionIdentity();
    shared::AuthoritativeTerrainTile tile{
        .coordinate = { .x = 4, .y = 5, .z = 0 }, .identity = session.terrain,
        .block_sea_level = 64U, .max_height = 1'023U, .revision = 7U,
    };
    tile.columns.fill({ 100U, 97U, 100U, shared::Block::Grass, shared::Block::Dirt });
    shared::ServerAuthoritativeTerrainBatchMessage batch{
        .session = session, .coverage_epoch = 13U, .delivery_token = 17U, .tiles = { tile },
    };
    auto const single = shared::encodeMessage(batch);
    auto const decoded = shared::decodeMessage(single);
    ASSERT_TRUE(decoded);
    auto const& result = std::get<shared::ServerAuthoritativeTerrainBatchMessage>(*decoded);
    EXPECT_EQ(result.session, session);
    EXPECT_EQ(result.coverage_epoch, 13U);
    EXPECT_EQ(result.delivery_token, 17U);
    ASSERT_EQ(result.tiles.size(), 1U);
    EXPECT_EQ(result.tiles.front().coordinate, tile.coordinate);
    EXPECT_EQ(result.tiles.front().revision, 7U);
    EXPECT_EQ(result.tiles.front().columns.back().ground_first_z, 97U);
    EXPECT_EQ(result.tiles.front().columns.back().top_material, shared::Block::Grass);
    ++tile.coordinate.x;
    batch.tiles.push_back(tile);
    EXPECT_EQ(shared::encodeMessage(batch).size() - single.size(), 28U + 256U * 8U);
    auto truncated = single;
    truncated.pop_back();
    EXPECT_FALSE(shared::decodeMessage(truncated));
    batch.tiles.front().columns.front().ground_end_z = 102U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(batch)));
}

TEST(MessageTest, AuthoritativeReceiptRoundTripsOnlyBoundedSessionEpochDeliveryKeys)
{
    shared::ClientAuthoritativeTerrainAckMessage ack{
        .session = terrainSessionIdentity(), .coverage_epoch = 3U, .delivery_token = 19U,
        .not_resident_keys = { { 4, 5, 0 } },
    };
    auto decoded = shared::decodeMessage(shared::encodeMessage(ack));
    ASSERT_TRUE(decoded);
    auto const& result = std::get<shared::ClientAuthoritativeTerrainAckMessage>(*decoded);
    EXPECT_EQ(result.session, ack.session);
    EXPECT_EQ(result.coverage_epoch, ack.coverage_epoch);
    EXPECT_EQ(result.delivery_token, ack.delivery_token);
    EXPECT_EQ(result.not_resident_keys, ack.not_resident_keys);
    ack.not_resident_keys.push_back(ack.not_resident_keys.front());
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)));
    ack.not_resident_keys.clear();
    ack.coverage_epoch = 0U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(ack)));
}

TEST(MessageTest, TerrainDeliverySizesMatchTheCompleteWireFrames)
{
    shared::ServerMaterializedChunkBatchMessage dense{
        .session = terrainSessionIdentity(), .coverage_epoch = 1U, .delivery_token = 1U,
        .chunks = {},
    };
    for (int32_t x = 0; x < shared::MATERIALIZED_CHUNK_BATCH_CAPACITY; ++x) {
        dense.chunks.push_back(x % 2 == 0
            ? uniformChunkPayload({x, 1, 0}, shared::Block::Stone)
            : denseChunkPayload({x, 1, 0}));
        EXPECT_EQ(shared::encodedMaterializedChunkBatchSize(dense), shared::encodeMessage(dense).size());
    }
    shared::ServerAuthoritativeTerrainBatchMessage profiles{
        .session = terrainSessionIdentity(), .coverage_epoch = 1U, .delivery_token = 1U,
        .tiles = {},
    };
    for (int32_t x = 0; x < shared::AUTHORITATIVE_TERRAIN_BATCH_CAPACITY; ++x) {
        shared::AuthoritativeTerrainTile tile{
            .coordinate = {x, 1, 0}, .identity = profiles.session.terrain,
            .block_sea_level = 64U, .max_height = 1'023U, .revision = 1U,
        };
        tile.columns.fill({100U, 97U, 100U, shared::Block::Grass, shared::Block::Dirt});
        profiles.tiles.push_back(tile);
        EXPECT_EQ(shared::encodedAuthoritativeTerrainBatchSize(profiles), shared::encodeMessage(profiles).size());
    }
}

TEST(MessageTest, AuthoritativeDeepOceanKeepsCanonicalEmptyGroundIntervalAboveSolidHeight)
{
    auto const session = terrainSessionIdentity();
    shared::AuthoritativeTerrainTile tile{
        .coordinate = { 4, 5, 0 }, .identity = session.terrain,
        .block_sea_level = 64U, .max_height = 1'023U, .revision = 1U,
    };
    tile.columns.fill({ 10U, 60U, 60U, shared::Block::Dirt, shared::Block::Dirt });
    shared::ServerAuthoritativeTerrainBatchMessage batch{
        .session = session, .coverage_epoch = 1U, .delivery_token = 1U, .tiles = { tile },
    };
    auto decoded = shared::decodeMessage(shared::encodeMessage(batch));
    ASSERT_TRUE(decoded);
    auto const& result = std::get<shared::ServerAuthoritativeTerrainBatchMessage>(*decoded);
    EXPECT_EQ(result.tiles.front().columns.front().ground_first_z, 60U);
    EXPECT_EQ(result.tiles.front().columns.front().ground_end_z, 60U);
    EXPECT_EQ(result.tiles.front().columns.front().materialAt(10U, 64U), shared::Block::Stone);
    batch.tiles.front().columns.front().ground_end_z = 61U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(batch)));
    batch.tiles.front().columns.front().ground_first_z = 1'025U;
    batch.tiles.front().columns.front().ground_end_z = 1'025U;
    EXPECT_FALSE(shared::decodeMessage(shared::encodeMessage(batch)));
}
