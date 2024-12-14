#include "EntityBase.h"

EntityBase::EntityBase(glm::vec3 pos) : m_pos(pos) { }

Object EntityBase::getObj() const {
	return Object{ m_pos, m_rot };
}

float EntityBase::getX() const {
	return m_pos.x;
}

float EntityBase::getY() const {
	return m_pos.y;
}

float EntityBase::getZ() const {
	return m_pos.z;
}
