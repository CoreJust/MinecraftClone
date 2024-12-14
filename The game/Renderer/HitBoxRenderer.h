#pragma once
#include <SFML\System\Vector3.hpp>
#include "../Camera.h"
#include "../Model.h"
#include "../Shaders/HitBoxShader.h"

class HitBoxRenderer final {
public:
	HitBoxRenderer();

	void draw(sf::Vector3i &pos);
	void update(Camera &cam);

private:
	HitBoxShader m_shader;
	Model m_model;

	sf::Vector3i m_pos;
};