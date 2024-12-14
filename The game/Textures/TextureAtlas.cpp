#include "TextureAtlas.h"

std::vector<GLfloat> Atlas::getTextureCoords(sf::Vector2i coords) {
	float x1 = coords.x * unitSize;
	float y1 = coords.y * unitSize;

	float x2 = x1 + unitSize;
	float y2 = y1 + unitSize;

	return {
		x1, y2,
		x2, y2,
		x2, y1,
		x1, y1
	};
}

std::vector<GLfloat> Atlas::getTextureBoxCoords(sf::Vector2i coords, int size, int bX, int bY, int bZ) {
	static auto addBoxFace = [](std::vector<GLfloat> &vec, float x1, float y1, float x2, float y2) {
		vec.insert(vec.end(), {
			x1, y2,
			x2, y2,
			x2, y1,
			x1, y1
		});
	};

	float cX = coords.x / (float)size;
	float cY = coords.y / (float)size;
	float sX = bX / (float)size;
	float sY = bY / (float)size;
	float sZ = bZ / (float)size;

	float
		topX1 = cX + sZ,
		topX2 = cX + sZ + sX,
		topY1 = cY,
		topY2 = cY + sZ;

	float
		botX1 = topX2,
		botX2 = topX2 + sX,
		botY1 = cY,
		botY2 = topY2;

	float
		rigX1 = cX,
		rigX2 = cX + sZ,
		rigY1 = topY2,
		rigY2 = topY2 + sY;

	float
		froX1 = rigX2,
		froX2 = rigX2 + sX,
		froY1 = topY2,
		froY2 = topY2 + sY;
	
	float
		lefX1 = froX2,
		lefX2 = froX2 + sZ,
		lefY1 = topY2,
		lefY2 = topY2 + sY;

	float
		bacX1 = lefX2,
		bacX2 = lefX2 + sX,
		bacY1 = topY2,
		bacY2 = topY2 + sY;

	std::vector<GLfloat> r;

	addBoxFace(r, bacX1, bacY1, bacX2, bacY2);
	addBoxFace(r, rigX1, rigY1, rigX2, rigY2);
	addBoxFace(r, froX1, froY1, froX2, froY2);
	addBoxFace(r, lefX1, lefY1, lefX2, lefY2);
	addBoxFace(r, topX1, topY1, topX2, topY2);
	addBoxFace(r, botX1, botY1, botX2, botY2);

	return r;
}

Texture* Atlas::getAtlas() {
	static Texture *atlas = new Texture(TEXTURE_SIZE == 8 ? "texture_atlas.png" : "texture_atlas16.png");
	return atlas;
}