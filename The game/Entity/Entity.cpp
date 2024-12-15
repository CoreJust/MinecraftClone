#include "Entity.h"
#include "../World/World.h"

Entity::Entity(glm::vec3 pos, glm::vec3 hitbox)
	: EntityBase(pos), m_box(hitbox), m_velocity({ 0, 0, 0 }) {
	m_box.update(pos);
}

void Entity::update() {
	if (abs(m_velocity.x) < 0.006f)
		m_velocity.x = 0.f;

	if (abs(m_velocity.z) < 0.006f)
		m_velocity.z = 0.f;
}

Axis Entity::doCollisionTest(World &world, float dt) {
	if (!m_isOnGround) {
		if (m_isInLiquid) {
			m_velocity.y -= (m_gravity / 6.f) * dt;
			if (m_velocity.y < -5.f)
				m_velocity.y = -5.f;
			else if (m_velocity.y > 5.f)
				m_velocity.y = 5.f;
		} else {
			m_velocity.y -= m_gravity * dt;
		}
	} else if (m_velocity.y < 0) {
		m_velocity.y = 0;
	}

	Axis r = Axis::NONE;
	auto vel = m_velocity * dt;
	m_pos.x += vel.x;
	if (collisionTest(world, { m_velocity.x, 0, 0 }))
		r = Axis(r | Axis::X);

	m_pos.y += vel.y;
	if (collisionTest(world, { 0, m_velocity.y, 0 }))
		r = Axis(r | Axis::Y);

	m_pos.z += vel.z;
	if (collisionTest(world, { 0, 0, m_velocity.z }))
		r = Axis(r | Axis::Z);

	return r;
}

void Entity::setGravity(float g) {
	m_gravity = g;
}

AABB& Entity::getAABB() {
	return m_box;
}

glm::vec3 Entity::getSize() {
	return m_box.getDimensions();
}

bool Entity::collisionTest(World &world, const glm::vec3 &velocity) {
	auto &dims = m_box.getDimensions();
	bool r = false;

	for (int x = static_cast<int>(floor(m_pos.x)); x < m_pos.x + dims.x; x++) {
		for (int y = static_cast<int>(floor(m_pos.y)); y < m_pos.y + dims.y; y++) {
			for (int z = static_cast<int>(floor(m_pos.z)); z < m_pos.z + dims.z; z++) {
				if (ItemDatabase::isSolid(world.getBlock(x, y, z))) {
					r = true;
					if (velocity.y > 0.f) {
						m_pos.y = y - dims.y - 0.0001f;
						m_velocity.y = 0.f;
					} else if (velocity.y < 0.f) {
						m_pos.y = y + 1.f;
					}

					if (velocity.x > 0.f) {
						m_pos.x = x - dims.x - 0.0001f;
						m_velocity.x = 0.f;
					} else if (velocity.x < 0.f) {
						m_pos.x = x + 1.f;
						m_velocity.x = 0.f;
					}

					if (velocity.z > 0.f) {
						m_pos.z = z - dims.z - 0.0001f;
						m_velocity.z = 0.f;
					} else if (velocity.z < 0.f) {
						m_pos.z = z + 1.f;
						m_velocity.z = 0.f;
					}
				}
			}
		}
	}

	return r;
}

Model* EntityModel::genModel() {
	VectorGLfloat textureCoords, vertices, lights, lightsTmp, verticesTmp;
	VectorGLuint indices, indicesTmp;
	unsigned int index = 0;

	lightsTmp = Model::getBlockLights();
	for (auto &b : boxes) {
		verticesTmp = Model::getBoxVertices(b.size, b.pos, b.rot, b.rotCoords);
		indicesTmp = Model::getBoxIndices(index);

		textureCoords.insert(textureCoords.end(), b.textureCoords.begin(), b.textureCoords.end());
		vertices.insert(vertices.end(), verticesTmp.begin(), verticesTmp.end());
		indices.insert(indices.end(), indicesTmp.begin(), indicesTmp.end());
		lights.insert(lights.end(), lightsTmp.begin(), lightsTmp.end());
	}

	return new Model(vertices, textureCoords, lights, indices);
}
