#pragma once
#include <vector>
#include <gl\glew.h>
#include <SFML\System\Vector2.hpp>
#include "Texture.h"

#define TEXTURE_SIZE 16

class Atlas {
public:
	constexpr static int atlasSize = 32 * TEXTURE_SIZE;
	constexpr static int textureSize = TEXTURE_SIZE;
	constexpr static int texturesInRow = atlasSize / textureSize;
	constexpr static float unitSize = 1.f / texturesInRow;
	constexpr static float pixelSize = 1.f / atlasSize;

	static std::vector<GLfloat> getTextureCoords(sf::Vector2i coords);
	static std::vector<GLfloat> getTextureBoxCoords(sf::Vector2i coords, int size, int bX, int bY, int bZ);

	static Texture* getAtlas();
};