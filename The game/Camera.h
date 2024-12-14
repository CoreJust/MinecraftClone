#pragma once
#include <SFML\System\Vector2.hpp>
#include "Object.h"
#include "Maths\Frustum.h"

class Camera : public Object {
public:
	Camera(glm::vec3 pos, sf::Vector2i screenSize);
	
	void input(glm::vec3 &res, bool isOnGround, bool isInLiquid);
	void update();

	void setScreenCenter(sf::Vector2i screenSize);

	const Frustum& getFrustum() const;
	const glm::mat4x4 getViewMatrix() const;
	const glm::mat4x4 getProjectionMatrix() const;

private:
	sf::Vector2i m_screenCenter;
	float m_widthToHeight;
	Frustum m_frustum;

	glm::mat4x4 m_viewMatrix;
	glm::mat4x4 m_projectionMatrix;

public:
	static float cam_speed;
};