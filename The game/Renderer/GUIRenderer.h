#pragma once
#include <vector>
#include "../Model2D.h"
#include <Core/GL/Shader.hpp>
#include "../Textures/Texture.h"
#include "../GUI/Element.h"

class GUIRenderer {
private:
	struct RenderInfo {
		Texture* text = nullptr;
		GLuint vao = 0;
		GLuint verticesCount = 0;
	};

public:
	GUIRenderer();

	void draw(Element *elem);
	void update();

private:
	core::gl::Shader m_shader;
	std::vector<RenderInfo> m_elements;
};