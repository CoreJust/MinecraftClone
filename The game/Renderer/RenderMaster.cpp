#include "RenderMaster.h"
#include <Core/GL/ShaderBuilder.hpp>

namespace {
	std::vector<GLfloat> quadVerticies {
		 1, -1, 0,
		 1,  1, 0,
		-1,  1, 0,
		-1, -1, 0,
	};

	std::vector<GLfloat> quadTextureCoords {
		1, 0,
		1, 1,
		0, 1,
		0, 0,
	};

	std::vector<GLuint> quadIndices {
		0, 1, 2,
		2, 3, 0
	};

	core::gl::Shader buildWaterFrameShader() {
		return core::gl::ShaderBuilder("water_frame_shader")
			.buildFromFiles("basic_frameVertex", "water_frameFragment");
	}

	core::gl::Shader buildSimpleFrameShader() {
		return core::gl::ShaderBuilder("simple_frame_shader")
			.buildFromFiles("basic_frameVertex", "basic_frameFragment");
	}
}

sf::Font *RenderMaster::m_font = nullptr;
sf::Vector2i RenderMaster::m_screenSize = sf::Vector2i{ 0, 0 };

RenderMaster::RenderMaster(sf::Font *font, sf::Vector2i screenSize)
	: m_waterShader(buildWaterFrameShader())
	, m_simpleShader(buildSimpleFrameShader())
	, m_fbo(screenSize.x, screenSize.y), m_quad(quadVerticies, quadTextureCoords, quadIndices) {
	m_screenSize = screenSize;
	m_font = font;
}

void RenderMaster::draw(ChunkSection &section) {
	m_chunkRenderer.draw(section.meshes);
}

void RenderMaster::draw(Entity *entity) {
	m_entityRenderer.draw(entity);
}

void RenderMaster::draw(sf::Vector3i &pos) {
	m_hitBoxRenderer.draw(pos);
}

void RenderMaster::draw(Element *element) {
	m_guiRenderer.draw(element);
}

void RenderMaster::draw(ItemIcon &icon, int z) {
	m_iconRenderer.draw(icon, z);
}

void RenderMaster::draw(sf::Drawable &drawable) {
	m_sfmlRenderer.draw(drawable);
}

void RenderMaster::drawDestroying(BlockPos pos, int level) {
	m_chunkRenderer.setDestroyingBlock(pos, level);
}

void RenderMaster::setWaterShader(bool a) {
	m_useWaterShader = a;
}

void RenderMaster::clear(const sf::Vector3f &color) {
	m_fbo.clear();

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glClearColor(color.x, color.y, color.z, 1.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void RenderMaster::update(Camera &cam, sf::RenderWindow &window) {
	static sf::Clock clock;

	// Draw the scene
	if (m_useWaterShader)
		m_fbo.bindBuffer();

	glEnable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);

	// Draw the world
	m_entityRenderer.update(cam);
	m_hitBoxRenderer.update(cam);
	m_chunkRenderer.update(cam);

	if (m_useWaterShader) { // Water shader
		glDisable(GL_DEPTH_TEST);
		m_waterShader.bind();
		m_quad.bind();
		m_fbo.bindTexture();
		glDrawElements(GL_TRIANGLES, m_quad.getIndicesCount(), GL_UNSIGNED_INT, nullptr);

		m_fbo.unbind();
		m_simpleShader.bind();
		glDrawElements(GL_TRIANGLES, m_quad.getIndicesCount(), GL_UNSIGNED_INT, nullptr);
		glEnable(GL_DEPTH_TEST);
	}
	

	// Draw GUI
	glDisable(GL_DEPTH_TEST);
	m_guiRenderer.update();

	glEnable(GL_DEPTH_TEST);
	m_iconRenderer.update();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);

	m_sfmlRenderer.update(window);
}

sf::Font* RenderMaster::getFont() {
	return m_font;
}

sf::Vector2i RenderMaster::glCoords2sfmlCoords(sf::Vector2f coords) {
	return sf::Vector2i(int(m_screenSize.x * (coords.x + 1) / 2), int(m_screenSize.y - m_screenSize.y * (coords.y + 1) / 2));
}

sf::Vector2f RenderMaster::sfmlCoords2glCoords(sf::Vector2i coords) {
	return sf::Vector2f(coords.x / (float)m_screenSize.x * 2 - 1.f, 1.f - coords.y / (float)m_screenSize.y * 2);
}
