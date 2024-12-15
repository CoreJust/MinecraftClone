#include "Zombie.h"
#include "../../World/World.h"
#include "../../Textures/TextureAtlas.h"

const glm::vec3 ZOMBIE_HITBOX = { 0.6f, 1.76f, 0.6f };
const float LOOK_DISTANCE = 24.f;

Zombie::Zombie(glm::vec3 pos) : Entity(pos, ZOMBIE_HITBOX) {
	static core::gl::Texture ZOMBIE_TEXTURE = core::gl::Texture("entities/zombie.png");

	std::vector<TextureBox> boxes;
	TextureBox box;

	// body
	box.pos = { 0.f, 0.66f, 0.f };
	box.rot = { 0.f, 0.f, 0.f };
	box.size = { 0.44f, 0.66f, 0.22f };
	box.rotCoords = { 0.f, 0.f, 0.f };
	box.textureCoords = Atlas::getTextureBoxCoords({ 16, 16 }, 64, 8, 12, 4);

	boxes.push_back(box);

	// head
	box.pos = { 0.f, 1.32f, -0.11f };
	box.rot = { 0.f, 0.f, 0.f };
	box.size = { 0.44f, 0.44f, 0.44f };
	box.rotCoords = box.size * 0.5f;
	box.textureCoords = Atlas::getTextureBoxCoords({ 0, 0 }, 64, 8, 8, 8);

	boxes.push_back(box);

	// right hand
	box.pos = { -0.22f, 1.21f, 0.f };
	box.rot = { 80.f, 0.f, 0.f };
	box.size = { 0.22f, 0.66f, 0.22f };
	box.rotCoords = { 0.f, 0.055f, 0.f };
	box.textureCoords = Atlas::getTextureBoxCoords({ 40, 16 }, 64, 4, 12, 4);

	boxes.push_back(box);

	// left hand
	box.pos = { 0.44f, 1.21f, 0.f };
	box.rot = { 80.f, 0.f, 0.f };
	box.size = { 0.22f, 0.66f, 0.22f };
	box.rotCoords = { 0.f, 0.055f, 0.f };
	box.textureCoords = Atlas::getTextureBoxCoords({ 40, 16 }, 64, 4, 12, 4);

	boxes.push_back(box);

	// right leg
	box.pos = { 0.f, 0.7f, 0.22f };
	box.rot = { 180.f, 0.f, 0.f };
	box.size = { 0.22f, 0.66f, 0.22f };
	box.rotCoords = { 0.f, 0.f, 0.f };
	box.textureCoords = Atlas::getTextureBoxCoords({ 0, 16 }, 64, 4, 12, 4);

	boxes.push_back(box);

	// left leg
	box.pos = { 0.22f, 0.7f, 0.22f };
	box.rot = { 180.f, 0.f, 0.f };
	box.size = { 0.22f, 0.66f, 0.22f };
	box.rotCoords = { 0.f, 0.f, 0.f };
	box.textureCoords = Atlas::getTextureBoxCoords({ 0, 16 }, 64, 4, 12, 4);

	boxes.push_back(box);

	m_model = new EntityModel{ boxes, &ZOMBIE_TEXTURE };
}

void Zombie::update(World &world, float dt) {
	static float mob_speed = 0.28f;
	static float time = 0.f;
	bool isSeeing = false;
	bool isMoving = false;
	float rotation = 0.f;
	glm::vec3 position_change = { 0, 0, 0 };

	time += dt;
	m_isOnGround = world.isOnGround(m_pos, m_box.getDimensions());
	m_isInLiquid = world.isInLiquid(m_pos, m_box.getDimensions());
	Entity::update();

	Player *player = world.getPlayer();
	float px = player->getX();
	float pz = player->getZ();
	float distance = sqrt(pow(px - m_pos.x, 2) + pow(pz - m_pos.z, 2));
	if (distance <= LOOK_DISTANCE)
		isSeeing = true;

	// Try to jump
	if (m_hasObstacle) {
		if (m_isInLiquid)
			m_velocity.y += (m_isOnGround ? 1.5f : 0.15f);
		else if (m_isOnGround)
			m_velocity.y += (m_velocity.y < 0 ? fminf(-m_velocity.y, 3.f) : 9.f);
	}

	if (isSeeing) {
		// Rotate to player
		float dx = px - m_pos.x;
		float dz = pz - m_pos.z;
		rotation = glm::degrees(atan2(dx, dz)) - m_rot.y;
		if (rotation > 180.f)
			rotation -= 360.f;
		else if (rotation < -180.f)
			rotation += 360.f;

		if (abs(rotation) > 15.f) {
			float change = 100.f * dt * (rotation < 0 ? -1 : 1);

			if (abs(change) > abs(rotation))
				change = rotation;
			else if (abs(rotation) < 10.f)
				change *= 0.15f;
			else if (abs(rotation) < 25.f)
				change *= 0.6f;
			else if (abs(rotation) > 60.f)
				change *= 1.5f;

			m_rot.y += change;
		}

		// Move to player
		if (abs(rotation) < 65.0) {
			isMoving = true;
			position_change.x -= cos(glm::radians(m_rot.y + 90));
			position_change.z += sin(glm::radians(m_rot.y + 90));
		}
	} else {
		// Passive mode
		static Random<> r;

		static float aim_rot = 0.f;
		if (time >= 2.8f) {
			aim_rot = r.random<float>(0, 360);
			time = 0.f;
		}

		rotation = aim_rot - m_rot.y;
		if (rotation > 180.f)
			rotation -= 360.f;
		else if (rotation < -180.f)
			rotation += 360.f;

		float change = 140.f * dt * (rotation < 0 ? -1 : 1);
		if (abs(change) > abs(rotation))
			change = rotation;

		m_rot.y += change;

		if (change <= 0.5f && time < 2.f) {
			isMoving = true;
			position_change.x -= cos(glm::radians(m_rot.y + 90));
			position_change.z += sin(glm::radians(m_rot.y + 90));
		}
	}

	// Update animation
	// Head
	if (rotation) {
		float head_rot = (abs(rotation) > 25.f ? 25.f : abs(rotation)) * (rotation < 0 ? -1 : 1);
		m_model->boxes[1].rot.y = head_rot;
	}

	if (isMoving) {
		// Move
		float speed = mob_speed;
		if (m_isInLiquid)
			speed *= 0.6f;
		else if (!m_isOnGround)
			speed *= 0.2f;

		m_velocity += position_change * speed;

		// Update position
		auto obstacles = doCollisionTest(world, dt);
		m_hasObstacle = obstacles & (Axis::X | Axis::Z);

		m_box.update(m_pos);

		static float accT = 0;
		accT += dt;

		while (accT >= 0.0055f) {
			float m = (m_isOnGround ? 0.93 : 0.975);
			m_velocity.x *= m;
			m_velocity.z *= m;
			accT -= 0.0055f;
		}

		// Legs animation
		static bool moveForward = true;
		float &right_rot = m_model->boxes[4].rot.x;
		float &left_rot = m_model->boxes[5].rot.x;

		if (moveForward) {
			if (right_rot < 160.f) {
				moveForward = false;
			} else {
				right_rot -= 30.f * dt;
				left_rot += 30.f * dt;
			}
		} else {
			if (right_rot > 200.f) {
				moveForward = true;
			} else {
				right_rot += 30.f * dt;
				left_rot -= 30.f * dt;
			}
		}
	} else {
		doCollisionTest(world, dt);
		m_hasObstacle = false;

		// Legs
		m_model->boxes[4].rot.x = 180.f;
		m_model->boxes[5].rot.x = 180.f;
	}
}

EntityModel* Zombie::getEntityModel() {
	return m_model;
}
