#include "Texture.hpp"
#include <GL/glew.h>
#include <SFML/Graphics/Image.hpp>
#include <Core/Common/Assert.hpp>

constexpr const char* const TEXTURES_PATH = "res/textures/";

core::gl::Texture::Texture(const std::string& file_name) {
	glGenTextures(1, &m_id);
	sf::Image img;
	if (!img.loadFromFile(TEXTURES_PATH + file_name))
		ASSERT(false, "failed to load texture from " + file_name);

	bind();
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img.getSize().x, img.getSize().y, 0, GL_RGBA, GL_UNSIGNED_BYTE, img.getPixelsPtr());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	auto aniso = 16.f;
	glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &aniso);
	glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, aniso);

	glGenerateMipmap(GL_TEXTURE_2D);
}

core::gl::Texture::~Texture() {
	if (m_id) {
		glDeleteTextures(1, &m_id);
		m_id = 0;
	}
}

void core::gl::Texture::setTextureBlock(Enum block) {
	glActiveTexture(block);
}

void core::gl::Texture::bind() const {
	ASSERT(m_id, "Texture must be initialized to bind");
	glBindTexture(GL_TEXTURE_2D, m_id);
}

void core::gl::Texture::unbind() const {
	glBindTexture(GL_TEXTURE_2D, 0);
}
