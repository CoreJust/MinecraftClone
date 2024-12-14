#pragma once
#include <glm/glm.hpp>
#include "../Object.h"

class World;

class EntityBase {
public:
	EntityBase(glm::vec3 pos);

	virtual void update(World &world, float dt) { };

	Object getObj() const;
	float getX() const;
	float getY() const;
	float getZ() const;

protected:
	glm::vec3 m_pos;
	glm::vec3 m_rot;
};