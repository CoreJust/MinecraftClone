#include "MeshBuilder.h"
#include "../../Textures/TextureAtlas.h"
#include "../ItemDatabase.h"
#include "../../Maths/Maths.h"

#include <iostream>

struct BlockFacing {
	void update(int x, int y, int z) {
		up = { x, y + 1, z };
		down = { x, y - 1, z };
		left = { x - 1, y, z };
		right = { x + 1, y, z };
		front = { x, y, z + 1 };
		back = { x, y, z - 1 };
	}

	sf::Vector3i up;
	sf::Vector3i down;
	sf::Vector3i left;
	sf::Vector3i right;
	sf::Vector3i front;
	sf::Vector3i back;
};

VectorGLubyte frontFace {
	0, 0, 1,
	1, 0, 1,
	1, 1, 1,
	0, 1, 1
};

VectorGLubyte backFace {
	1, 0, 0,
	0, 0, 0,
	0, 1, 0,
	1, 1, 0
};

VectorGLubyte rightFace {
	1, 0, 1,
	1, 0, 0,
	1, 1, 0,
	1, 1, 1
};

VectorGLubyte leftFace {
	0, 0, 0,
	0, 0, 1,
	0, 1, 1,
	0, 1, 0
};

VectorGLubyte topFace {
	0, 1, 1,
	1, 1, 1,
	1, 1, 0,
	0, 1, 0
};

VectorGLubyte bottomFace {
	0, 0, 0,
	1, 0, 0,
	1, 0, 1,
	0, 0, 1
};

VectorGLfloat topLight {
	16.f, 16.f, 16.f, 16.f
};

VectorGLfloat sideLight {
	14.f, 14.f, 14.f, 14.f
};

VectorGLfloat bottomLight {
	11.f, 11.f, 11.f, 11.f
};

MeshBuilder::MeshBuilder(Chunk &chunk) : m_chunk(chunk) { }

void MeshBuilder::buildMesh(ChunkSection(&sections)[16]) {
	BlockFacing facing;
	int sectionsCount = m_chunk.maxHeight / 16 + 1;
	for (int i = 0; i < sectionsCount; ++i) {
		ChunkSection &section = sections[i];
		section.isMade = true;
		section.isBuffered = false;
		section.meshes.reset();
		m_meshes = &section.meshes;

		int y_base = section.index * 16;
		for (int x = 0; x < 16; ++x) {
			for (int z = 0; z < 16; ++z) {
				int y_max = min(y_base + 16, int(m_chunk.heightMap[x][z]));
				for (int y = y_base; y <= y_max; ++y) {
					ItemID block = m_chunk.at(x, y, z);
					if (block == ItemID::AIR)
						continue;

					BlockPosition pos{ x, y, z };
					m_data = &ItemDatabase::get().getData(block);

					facing.update(x, y, z);
					if (block == ItemID::WATER)
						m_activeMesh = &m_meshes->waterMesh;
					else
						m_activeMesh = &m_meshes->solidMesh;

					if (y != 0) // do not make bottom faces for low blocks
						tryAddFace(bottomFace, m_data->bottomTextureCoords_vec, bottomLight, pos, facing.down);

					tryAddFace(topFace, m_data->topTextureCoords_vec, topLight, pos, facing.up);
					tryAddFace(rightFace, m_data->sideTextureCoords_vec, sideLight, pos, facing.right);
					tryAddFace(leftFace, m_data->sideTextureCoords_vec, sideLight, pos, facing.left);
					tryAddFace(frontFace, m_data->frontTextureCoords_vec, sideLight, pos, facing.front);
					tryAddFace(backFace, m_data->sideTextureCoords_vec, sideLight, pos, facing.back);
				}
			}
		}
	}
}

void MeshBuilder::tryAddFace(const VectorGLubyte &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights,
	BlockPosition pos, sf::Vector3i blockFacing) {
	if (shouldAddFace(blockFacing)) {
		m_activeMesh->addFace(vertices, textureCoords, lights, pos);
	}
}

bool MeshBuilder::shouldAddFace(sf::Vector3i &blockFacing) {
	auto block = m_chunk.getBlock(blockFacing.x, blockFacing.y, blockFacing.z);
	if (isOpaque(block))
		return false;

	return block != m_data->blockID;
}

bool MeshBuilder::isOpaque(ItemID block) {
	static auto &bd = ItemDatabase::get();
	return bd.getData(block).isOpaque;
}