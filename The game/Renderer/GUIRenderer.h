#pragma once
#include <vector>
#include <Core/GL/Shader.hpp>
#include "../Model2D.h"
#include "../GUI/Element.h"

namespace core::gl {
	class Texture;
}

class GUIRenderer {
private:
	struct RenderInfo {
		core::gl::Texture* text = nullptr;
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