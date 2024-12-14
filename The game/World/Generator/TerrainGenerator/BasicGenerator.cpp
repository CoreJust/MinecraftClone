#include "BasicGenerator.h"
#include "../../../Maths/Maths.h"
#include "../../../Utils/Hasher.h"
#include "../../Chunk/Chunk.h"
#include "../../World.h"
#include "../Structures/OakTree.h"

#include "../../../Maths/Noise/Noise.h"

BasicGenerator::BasicGenerator() : m_globalNoise(1, 0.005), m_caves(0, 0.2) { }

void BasicGenerator::generateTerrain(Chunk &chunk) {
	static unsigned char hMap[16][16];

	ChunkTerrainBuilder builder(chunk);
	/*memset(chunk.treeMap, 0, 256);
	builder.fillBlocks(ItemID::BEDROCK, { 0, 0, 0 }, { 15, 0, 15 });
	builder.fillBlocks(ItemID::GRASS, { 0, 1, 0 }, { 15, 1, 15 });
	return;*/

	// Generator parameters
	int waterLevel = 28;
	int halfWaterLevel = -(waterLevel / 2);
	int treesFrequency = 30;
	int ironFrequency = 32;


	int cX = chunk.getPos().x;
	int cZ = chunk.getPos().z;

	if (cX == 0) {
		builder.fillBlocks(ItemID::BEDROCK, { 0, 0, 0 }, { 15, 0, 15 });
		return;
	}
	
	// Generate height map
	for (int x = 0; x < 16; ++x) {
		for (int z = 0; z < 16; ++z) {
			float global_noise = m_globalNoise.octave_noise(cX * 16 + x, cZ * 16 + z, 4) * 200 - 34;
			if (global_noise < -5) {
				global_noise *= 0.6f;
				if (global_noise < halfWaterLevel)
					global_noise = halfWaterLevel;
			}

			int noise = global_noise + waterLevel;

			chunk.heightMap[x][z] = hMap[x][z] = noise;
			if (noise > chunk.maxHeight)
				chunk.maxHeight = noise;
		}
	}

	// Generate terrain based on the height map
	builder.fillBlocks(ItemID::BEDROCK, { 0, 0, 0 }, { 15, 0, 15 });

	for (int x = 0; x < 16; ++x) {
		for (int z = 0; z < 16; ++z) {
			int h = hMap[x][z];
			int cobblestone_height = max(h - 3, 1);

			builder.makeColumn(ItemID::DIRT, { x, 1, z }, h - 1);
			builder.makeColumn(ItemID::STONE, { x, 1, z }, cobblestone_height);
			for (int y = 1; y < cobblestone_height; ++y) {
				// underground
				float noise = m_caves.noise(cX * 16 + x, y, cZ * 16 + z);
				noise -= (y - waterLevel) * 0.0045f;

				float chance = 0.14f;
				if (y <= 5) {
					switch (y) {
						case 1: chance += 0.2f; break;
						case 2: chance += 0.09f; break;
						case 3: chance += 0.042f; break;
						case 4: chance += 0.016f; break;
						case 5: chance += 0.01f; break;
					}
				} else if ((y - (y % 3)) % 11 == 0) {
					chance += 0.3f;
				}

				if (noise > chance) // caves
					builder.setBlock(ItemID::AIR, { x, y, z });
				else if (m_rand.random(0, ironFrequency) == 0) // ore gen
					builder.setBlock(ItemID::IRON_ORE, { x, y, z });
			}

			if (h >= waterLevel) { // grass
				builder.setBlock(ItemID::GRASS, { x, h, z });
			} else { // water
				builder.makeColumn(ItemID::WATER, { x, h + 1, z }, waterLevel - h);
				builder.makeColumn(ItemID::SAND, { x, h - 1, z }, 2);
			}
		}
	}
}
