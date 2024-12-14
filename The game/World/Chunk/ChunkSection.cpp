#include "ChunkSection.h"

ChunkSection::ChunkSection()
	: aabb({ 16, 16, 16 }) { }

void ChunkSection::init(int x, int y, int z) {
	aabb.update({ x * 16, y * 16, z * 16 });
	meshes.position = { x * 16, y * 16, z * 16 };
	index = y;
}

void ChunkSection::buffer() {
	meshes.buffer();
	isBuffered = true;
}
