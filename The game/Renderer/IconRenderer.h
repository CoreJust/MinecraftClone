#pragma once
#include <vector>
#include <gl\glew.h>
#include "../Model.h"
#include "../Shaders/IconShader.h"
#include "../World/Items/ItemIcon.h"

class IconRenderer final {
private:
	struct RenderInfo {
		sf::Vector2f pos;
		int z = 0;
		GLuint vao = 0;
		GLuint indicesCount = 0;
	};

public:
	IconRenderer();

	void draw(ItemIcon &icon, int z);
	void update();

private:
	std::vector<RenderInfo> m_renderInfos;
	IconShader m_shader;
};