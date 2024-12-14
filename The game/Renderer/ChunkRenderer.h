#pragma once
#include <vector>
#include <glm\glm.hpp>
#include "../Maths/BlockPos.h"
#include "../Camera.h"
#include "../Model.h"
#include "../Shaders/WaterShader.h"
#include "../Shaders/SolidShader.h"
#include "../Shaders/DestroyingShader.h"
#include "../World/Chunk/Meshes.h"

class ChunkRenderer final {
private:
	struct RenderInfo {
		GLuint vao = 0;
		GLuint indicesCount = 0;
	};

public:
	ChunkRenderer();

	void draw(Meshes& meshes);
	void setDestroyingBlock(const BlockPos &pos, int destroyLevel);

	void update(const Camera& cam);

private:
	std::vector<RenderInfo> m_solidInfo;
	std::vector<RenderInfo> m_waterInfo;
	std::vector<glm::vec3> m_positions;
	SolidShader m_solidShader;
	DestroyingShader m_destroyingShader;
	WaterShader m_waterShader;

	Model m_destroyModel;
};