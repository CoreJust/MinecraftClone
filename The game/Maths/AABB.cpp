#include "AABB.h"

AABB::AABB(const glm::vec3 &dim) : m_dim(dim) { }

void AABB::update(const glm::vec3 &pos) {
	m_min = pos;
	m_max = m_min + m_dim;
}

bool AABB::isColideWith(const AABB &other) const {
	return  (m_min.x < other.m_max.x && m_max.x > other.m_min.x) &&
		(m_min.y < other.m_max.y && m_max.y > other.m_min.y) &&
		(m_min.z < other.m_max.z && m_max.z > other.m_min.z);
}

glm::vec3 AABB::getVN(const glm::vec3 &normal) const {
	glm::vec3 res = m_min;

	if (normal.x < 0) {
		res.x += m_dim.x;
	} if (normal.y < 0) {
		res.y += m_dim.y;
	} if (normal.z < 0) {
		res.z += m_dim.z;
	}

	return res;
}

glm::vec3 AABB::getVP(const glm::vec3 &normal) const {
	glm::vec3 res = m_min;

	if (normal.x > 0) {
		res.x += m_dim.x;
	} if (normal.y > 0) {
		res.y += m_dim.y;
	} if (normal.z > 0) {
		res.z += m_dim.z;
	}

	return res;
}

AABB AABB::makeBlockBox(const sf::Vector3i &pos) {
	AABB r({ 1.f, 1.f, 1.f });
	r.update({ pos.x, pos.y, pos.z });
	return r;
}
