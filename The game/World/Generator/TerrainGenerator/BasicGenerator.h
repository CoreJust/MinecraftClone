#pragma once
#include <Core/Common/Random.hpp>
#include "../../../Maths/Noise/PerlinNoise.h"
#include "../TerrainBuilder.h"

class Chunk;
class World;

class BasicGenerator {
public:
	BasicGenerator();

	void generateTerrain(Chunk &chunk);

private:
	core::common::Random m_rand;

	PerlinNoise2D m_globalNoise;
	CoherentPerlinNoise3D m_caves;
};