#pragma once
#include "Element.h"
#include "../Model2D.h"
#include "../Textures/Texture.h"

class Background final : public Element {
public:
	Background(sf::Vector2f pos, sf::Vector2f size);

	void draw(RenderMaster &renderer) override;

	void setSize(sf::Vector2f size);
	Model2D& getModel() override;
	Texture* getTexture() override;

private:
	Model2D m_model;

	sf::Vector2f m_pos;
};