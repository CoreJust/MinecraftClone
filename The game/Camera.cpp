#include "Camera.h"
#include <gl\glew.h>
#include <SFML\Window\Keyboard.hpp>
#include <SFML\Window\Mouse.hpp>
#include "Maths\Matrix.h"

float Camera::cam_speed = 0.6f;

Camera::Camera(glm::vec3 pos, sf::Vector2i screenSize)
	: Object{ pos, glm::vec3() } {
	setScreenCenter(screenSize);
	m_projectionMatrix = Maths::genProjectionMatrix(90.f, m_widthToHeight);
}

void Camera::input(glm::vec3 &res, bool isOnGround, bool isInLiquid) {
	// Keyboard input
	// Control
	GLfloat speed = cam_speed;
	bool isSpacePressed = sf::Keyboard::isKeyPressed(sf::Keyboard::Space);

	if (isSpacePressed && isOnGround && !isInLiquid) // when we are jumping our speed is increasing
		speed *= 3.f;
	else if (isInLiquid) // when we are swimming our speed is decreasing
		speed *= 0.6f;
	else if (!isOnGround)
		speed *= 0.3f;

	if (sf::Keyboard::isKeyPressed(sf::Keyboard::W)) {
		float m = (sf::Keyboard::isKeyPressed(sf::Keyboard::LControl) ? 2.4f : 1.3f);
		res.x -= cos(glm::radians(rot.y + 90)) * speed * m;
		res.z -= sin(glm::radians(rot.y + 90)) * speed * m;
	} if (sf::Keyboard::isKeyPressed(sf::Keyboard::S)) {
		res.x += cos(glm::radians(rot.y + 90)) * speed;
		res.z += sin(glm::radians(rot.y + 90)) * speed;
	} if (sf::Keyboard::isKeyPressed(sf::Keyboard::A)) {
		res.x -= cos(glm::radians(rot.y)) * speed;
		res.z -= sin(glm::radians(rot.y)) * speed;
	} if (sf::Keyboard::isKeyPressed(sf::Keyboard::D)) {
		res.x += cos(glm::radians(rot.y)) * speed;
		res.z += sin(glm::radians(rot.y)) * speed;
	} if (isSpacePressed) {
		if (isInLiquid)
			res.y += (isOnGround ? 2.2f : 0.15f);
		else if (isOnGround)
			res.y += (res.y < 0 ? fminf(-res.y, 3.f) : 10.5f);
	} if (sf::Keyboard::isKeyPressed(sf::Keyboard::F)) {
		if (res.y < 15.f)
			res.y += 1.1f;
	}

	// Other
	static bool isCPressed = false;
	if (isCPressed != sf::Keyboard::isKeyPressed(sf::Keyboard::C)) {
		isCPressed = !isCPressed;
		if (isCPressed) {
			m_projectionMatrix = Maths::genProjectionMatrix(20.f, m_widthToHeight);
		} else {
			m_projectionMatrix = Maths::genProjectionMatrix(90.f, m_widthToHeight);
		}
	}

	// Mouse input
	static sf::Vector2i lastMouse = sf::Mouse::getPosition();
	auto mouseChange = sf::Mouse::getPosition() - lastMouse;

	// for cases when we closed inventory and mouseChange is a very big number.
	mouseChange.x = fmaxf(fminf(mouseChange.x, 30), -30);
	mouseChange.y = fmaxf(fminf(mouseChange.y, 30), -30);

	rot.x += mouseChange.y * 0.21f;
	rot.y += mouseChange.x * 0.21f;

	if (rot.x > 90) {
		rot.x = 90;
	} else if (rot.x < -90) {
		rot.x = -90;
	}

	sf::Mouse::setPosition(m_screenCenter);
	lastMouse = sf::Mouse::getPosition();
}

void Camera::update() {
	m_viewMatrix = Maths::genViewMatrix(*this);
	m_frustum.update(m_projectionMatrix * m_viewMatrix);
}

void Camera::setScreenCenter(sf::Vector2i screenSize) {
	m_screenCenter.x = screenSize.x / 2;
	m_screenCenter.y = screenSize.y / 2;
	m_widthToHeight = float(screenSize.x) / screenSize.y;
}

const Frustum& Camera::getFrustum() const {
	return m_frustum;
}

const glm::mat4x4 Camera::getViewMatrix() const {
	return m_viewMatrix;
}

const glm::mat4x4 Camera::getProjectionMatrix() const {
	return m_projectionMatrix;
}
