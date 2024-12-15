#pragma once
#include <vector>
#include <math.h>
#include <Core/GL/Texture.hpp>
#include "EntityBase.h"
#include "../Object.h"
#include "../Model.h"
#include "../Maths/AABB.h"

class World;

enum Axis : int {
	NONE = 0,

	X = 1,
	Y = 2,
	Z = 4
};

struct TextureBox : public Object {
	glm::vec3 size;
	glm::vec3 rotCoords;
	std::vector<GLfloat> textureCoords;
};

struct EntityModel {
	std::vector<TextureBox> boxes;
	core::gl::Texture* texture;

	Model* genModel();
};

class Entity : public EntityBase {
public:
	Entity(glm::vec3 pos, glm::vec3 hitbox);

	virtual void update(World &world, float dt) override { }
	void update();
	Axis doCollisionTest(World &world, float dt);

	void setGravity(float g);
	AABB& getAABB();
	glm::vec3 getSize();

	virtual EntityModel* getEntityModel() { return nullptr; };

private:
	bool collisionTest(World &world, const glm::vec3 &velocity);

protected:
	AABB m_box;
	glm::vec3 m_velocity;

	bool m_isOnGround = true;
	bool m_isInLiquid = false;

private:
	float m_gravity = 23.f;
};