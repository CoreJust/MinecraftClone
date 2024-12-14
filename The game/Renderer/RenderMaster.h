#pragma once
#include "ChunkRenderer.h"
#include "EntityRenderer.h"
#include "HitBoxRenderer.h"
#include "GUIRenderer.h"
#include "IconRenderer.h"
#include "SFMLRenderer.h"
#include "../FBO.h"
#include "../World/Chunk/Chunk.h"
#include "../World/Items/ItemIcon.h"

class RenderMaster {
public:
	RenderMaster(sf::Font *font, sf::Vector2i screenSize);

	void draw(ChunkSection &section);
	void draw(Entity *entity);
	void draw(sf::Vector3i &pos);
	void draw(Element *element);
	void draw(ItemIcon &icon, int z = 1);
	void draw(sf::Drawable &drawable);
	void drawDestroying(BlockPos pos, int level);
	
	void setWaterShader(bool a);

	void clear(const sf::Vector3f &color);
	void update(Camera &cam, sf::RenderWindow &window);

	static sf::Font *getFont();
	static sf::Vector2i glCoords2sfmlCoords(sf::Vector2f coords);
	static sf::Vector2f sfmlCoords2glCoords(sf::Vector2i coords);

private:
	ChunkRenderer m_chunkRenderer;
	EntityRenderer m_entityRenderer;
	HitBoxRenderer m_hitBoxRenderer;
	GUIRenderer m_guiRenderer;
	IconRenderer m_iconRenderer;
	SFMLRenderer m_sfmlRenderer;

	core::gl::Shader m_waterShader;
	core::gl::Shader m_simpleShader;
	FBO m_fbo;
	Model m_quad;

	bool m_useWaterShader = false;

	static sf::Font *m_font;
	static sf::Vector2i m_screenSize;
};