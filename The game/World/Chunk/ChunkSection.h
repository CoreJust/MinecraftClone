#pragma once
#include "../../Maths/AABB.h"
#include "Meshes.h"

struct ChunkSection {
	ChunkSection();
	void init(int x, int y, int z);

	void buffer();

	Meshes meshes;
	AABB aabb;
	int index;
	bool isMade = false;
	bool isBuffered = false;
};