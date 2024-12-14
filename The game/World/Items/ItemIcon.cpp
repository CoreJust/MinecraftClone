#include "ItemIcon.h"
#include "../../Textures/TextureAtlas.h"
#include "../ItemDatabase.h"
#include "../../Renderer/RenderMaster.h"

constexpr GLfloat Bx = 0.04f;
constexpr GLfloat By = Bx * (1920 / 1080.f);
constexpr GLfloat Bz = Bx * 1.35f;

static VectorGLfloat verticesBlock {
	// top
	0,  By, 0,
	Bx, By, 0,
	Bx, By, Bz,
	0,  By, Bz,

	// right
	Bx, 0,  0,
	Bx, 0,  Bz,
	Bx, By, Bz,
	Bx, By, 0,

	// front
	0,  0,  0,
	Bx, 0,  0,
	Bx, By, 0,
	0,  By, 0
};

constexpr GLfloat Ix = 0.07f;
constexpr GLfloat Iy = Ix * 2.12f;

static VectorGLfloat verticesItem {
	0,  0,  0,
	Ix, 0,  0,
	Ix, Iy, 0,
	0,  Iy, 0
};

static VectorGLfloat lightsBlock {
	// top
	18.f, 18.f, 18.f, 18.f,

	// right
	12.5f, 12.5f, 12.5f, 12.5f,

	// front
	15.f, 15.f, 15.f, 15.f
};

static VectorGLfloat lightsItem {
	16.f, 16.f, 16.f, 16.f
};

static VectorGLuint eboBlock {
	0, 1, 2,
	2, 3, 0,

	4, 5, 6,
	6, 7, 4,

	8, 9, 10,
	10, 11, 8
};

static VectorGLuint eboItem {
	0, 1, 2,
	2, 3, 0
};

ItemIcon::ItemIcon(int ID) {
	setID(m_ID);
}

void ItemIcon::draw(RenderMaster &renderer) {
	renderer.draw(*this, m_z);
}

void ItemIcon::setID(int ID) {
	if (m_ID == ID)
		return;

	m_ID = ID;
	if (m_ID == 0)
		return;

	VectorGLfloat textureCoords;
	const auto &data = ItemDatabase::get().getData((ItemID)m_ID);

	if (isItemBlock(ID)) {
		textureCoords.insert(textureCoords.end(), data.topTextureCoords_vec.begin(), data.topTextureCoords_vec.end());
		textureCoords.insert(textureCoords.end(), data.sideTextureCoords_vec.begin(), data.sideTextureCoords_vec.end());
		textureCoords.insert(textureCoords.end(), data.frontTextureCoords_vec.begin(), data.frontTextureCoords_vec.end());

		m_model = Model(verticesBlock, textureCoords, lightsBlock, eboBlock);
	} else {
		textureCoords.insert(textureCoords.end(), data.topTextureCoords_vec.begin(), data.topTextureCoords_vec.end());

		m_model = Model(verticesItem, textureCoords, lightsItem, eboItem);
	}
}

void ItemIcon::setPosition(sf::Vector2f pos) {
	m_pos = pos;
}

void ItemIcon::setZ(int z) {
	m_z = z;
}

Model& ItemIcon::getModel() {
	return m_model;
}

sf::Vector2f ItemIcon::getPosition() {
	return m_pos + (isItemBlock(m_ID) ? sf::Vector2f{ 0.f, 0.f } : sf::Vector2f{ 0.001f, -0.0082f });
}

int ItemIcon::getID() {
	return m_ID;
}
