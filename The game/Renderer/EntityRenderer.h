#pragma once
#include <vector>
#include <glm\glm.hpp>
#include "../Camera.h"
#include "../Model.h"
#include "../Shaders/EntityShader.h"

class Entity;
namespace core::gl {
	class Texture;
}

class EntityRenderer final {
private:
	struct RenderInfo {
		core::gl::Texture *texture;
		Model *model;
		Object obj;
		glm::vec3 size;
	};

public:
	EntityRenderer();

	void draw(Entity *entity);
	void update(const Camera& cam);

private:
	std::vector<RenderInfo> m_renderInfos;
	EntityShader m_entityShader;
};