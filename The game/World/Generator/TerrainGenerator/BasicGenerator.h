#pragma once
#include <vector>
#include <unordered_map>
#include "../../../Maths/VectorXZ.h"
#include "../../../Utils/Random.h"
#include "../../../Maths/Noise/PerlinNoise.h"
#include "../TerrainBuilder.h"

class Chunk;
class World;

class BasicGenerator {
public:
	BasicGenerator();

	void generateTerrain(Chunk &chunk);

private:
	Random<std::mt19937> m_rand;

	PerlinNoise2D m_globalNoise;
	CoherentPerlinNoise3D m_caves;
};