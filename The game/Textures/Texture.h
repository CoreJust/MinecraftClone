#pragma once
#include <gl\glew.h>
#include <SFML\Graphics.hpp>

class Texture final {
public:
	Texture(std::string name);
	~Texture();

	void bind(GLuint nomer);

private:
	GLuint m_textureID;
};