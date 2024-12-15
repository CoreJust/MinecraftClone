#pragma once
#include <SFML\Graphics.hpp>
#include "State.h"
#include "../Renderer/RenderMaster.h"
#include "../Entity/Player.h"
#include "../Entity/Mobs/Zombie.h"
#include "../World/World.h"

class GameState : public State {
public:
	GameState(StatesController* controller, sf::Vector2i screenSize);

	void update(float delta, float mouseWheel) override;
	void draw() override;

private:
	RenderMaster m_renderer;
	World m_world;

	Player m_player;

	sf::Font m_font;
	sf::RectangleShape m_cursor;
	std::string m_fpsString = "-";

	sf::Clock m_fpsTimer;
	int m_frames = 0;
	bool m_showDebugPanel = true;
};