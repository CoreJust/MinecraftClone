#pragma once
#include <SFML\System\Vector3.hpp>
#include "../Block\BlockData.h"
#include "../../Camera.h"
#include "Mesh.h"
#include "Chunk.h"

class MeshBuilder final {
public:
	MeshBuilder(Chunk &chunk);
	void buildMesh(ChunkSection(&sections)[16]);
	
private:
	void tryAddFace(const VectorGLubyte &vertices, const VectorGLfloat &textureCoords, const VectorGLfloat &lights,
		BlockPosition pos, sf::Vector3i blockFacing);

	bool shouldAddFace(sf::Vector3i &blockFacing);
	bool isOpaque(ItemID block);

private:
	Chunk &m_chunk;
	Meshes* m_meshes;
	Mesh* m_activeMesh;

	const BlockData* m_data;
};