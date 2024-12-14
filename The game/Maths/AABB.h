#pragma once
#include <glm\glm.hpp>
#include <SFML/System/Vector3.hpp>

class AABB final {
public:
	AABB(const glm::vec3 &dim);

	void update(const glm::vec3 &pos);

	bool isColideWith(const AABB &other) const;

	glm::vec3 getVN(const glm::vec3 &normal) const;
	glm::vec3 getVP(const glm::vec3 &normal) const;

	const glm::vec3& getDimensions() const { return m_dim; }

public:
	static AABB makeBlockBox(const sf::Vector3i &pos);

private:
	glm::vec3 m_min;
	glm::vec3 m_max;
	glm::vec3 m_dim;
};