#include "Frustum.h"

namespace Planes {
	enum {
		Near,
		Far,
		Left,
		Right,
		Top,
		Bottom
	};
}

float Plane::distanceTo(const glm::vec3 &point) const {
	return glm::dot(point, normal) + distance;
}

void Frustum::update(const glm::mat4x4 &mat) {
	// left
	m_planes[Planes::Left].normal.x = mat[0][3] + mat[0][0];
	m_planes[Planes::Left].normal.y = mat[1][3] + mat[1][0];
	m_planes[Planes::Left].normal.z = mat[2][3] + mat[2][0];
	m_planes[Planes::Left].distance = mat[3][3] + mat[3][0];

	// right
	m_planes[Planes::Right].normal.x = mat[0][3] - mat[0][0];
	m_planes[Planes::Right].normal.y = mat[1][3] - mat[1][0];
	m_planes[Planes::Right].normal.z = mat[2][3] - mat[2][0];
	m_planes[Planes::Right].distance = mat[3][3] - mat[3][0];

	// bottom
	m_planes[Planes::Bottom].normal.x = mat[0][3] + mat[0][1];
	m_planes[Planes::Bottom].normal.y = mat[1][3] + mat[1][1];
	m_planes[Planes::Bottom].normal.z = mat[2][3] + mat[2][1];
	m_planes[Planes::Bottom].distance = mat[3][3] + mat[3][1];

	// top
	m_planes[Planes::Top].normal.x = mat[0][3] - mat[0][1];
	m_planes[Planes::Top].normal.y = mat[1][3] - mat[1][1];
	m_planes[Planes::Top].normal.z = mat[2][3] - mat[2][1];
	m_planes[Planes::Top].distance = mat[3][3] - mat[3][1];

	// near
	m_planes[Planes::Near].normal.x = mat[0][3] + mat[0][2];
	m_planes[Planes::Near].normal.y = mat[1][3] + mat[1][2];
	m_planes[Planes::Near].normal.z = mat[2][3] + mat[2][2];
	m_planes[Planes::Near].distance = mat[3][3] + mat[3][2];

	// far
	m_planes[Planes::Far].normal.x = mat[0][3] - mat[0][2];
	m_planes[Planes::Far].normal.y = mat[1][3] - mat[1][2];
	m_planes[Planes::Far].normal.z = mat[2][3] - mat[2][2];
	m_planes[Planes::Far].distance = mat[3][3] - mat[3][2];

	for (int i = 0; i < 6; ++i) {
		auto &plane = m_planes[i];
		float length = glm::length(plane.normal);
		plane.normal /= length;
		plane.distance /= length;
	}
}

bool Frustum::boxInFrustum(const AABB &box) const {
	bool r = true;

	for (int i = 0; i < 6; ++i) {
		auto &plane = m_planes[i];
		if (plane.distanceTo(box.getVP(plane.normal)) < 0) {
			return false;
		} else if (plane.distanceTo(box.getVN(plane.normal)) < 0) {
			r = true;
		}
	}

	return r;
}