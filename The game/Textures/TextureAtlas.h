#pragma once
#include <vector>
#include <SFML\System\Vector2.hpp>
#include <Core/GL/Texture.hpp>

constexpr size_t TEXTURE_SIZE = 16;

class Atlas final {
public:
	constexpr static int atlasSize = 32 * TEXTURE_SIZE;
	constexpr static int textureSize = TEXTURE_SIZE;
	constexpr static int texturesInRow = atlasSize / textureSize;
	constexpr static float unitSize = 1.f / texturesInRow;
	constexpr static float pixelSize = 1.f / atlasSize;

	static std::vector<float> getTextureCoords(sf::Vector2i coords);
	static std::vector<float> getTextureBoxCoords(sf::Vector2i coords, int size, int bX, int bY, int bZ);

	static core::gl::Texture* getAtlas();
};