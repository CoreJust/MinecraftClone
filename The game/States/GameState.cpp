#include "GameState.h"
#include <ctime>
#include <Core/IO/Logger.hpp>
#include "../Utils/Random.h"
#include "../Maths/Maths.h"
#include "../Textures/TextureAtlas.h"
#include "StatesController.h"

GameState::GameState(StatesController* controller, sf::Vector2i screenSize)
	: State(controller), m_player(0, 190, 0,
		screenSize), m_renderer(&m_font, screenSize) {
	// Load the app data
	m_font.loadFromFile("res/fonts/font.ttf");

	// Create the cursor
	sf::Vector2f cursorSize{ 20, 20 };
	m_cursor.setFillColor(sf::Color::White);
	m_cursor.setSize(cursorSize);
	m_cursor.setPosition(m_controller->getWidth() / 2 - 10.f, m_controller->getHeight() / 2 - 10.f);

	// Generate the world
	core::io::info("Starting world generation");
	int pX = (int)m_player.getX();
	int pY = (int)m_player.getY();
	int pZ = (int)m_player.getZ();
	m_world.generate(pX, pZ);
	m_world.setPlayer(&m_player);

	// Generate the spawn house
	core::io::info("Preparing spawn area");

	// floor and roof
	m_world.fillBlocks(ItemID::COBBLESTONE, pX - 14, 29, pY - 1, 1, pZ - 14, 29);
	m_world.fillBlocks(ItemID::COBBLESTONE, pX - 4, 9, pY + 4, 1, pZ - 4, 10);

	// walls
	m_world.fillBlocks(ItemID::COBBLESTONE, pX - 5, 11, pY - 1, 6, pZ - 5, 1);
	m_world.fillBlocks(ItemID::COBBLESTONE, pX - 5, 11, pY - 1, 6, pZ + 5, 1);

	m_world.fillBlocks(ItemID::COBBLESTONE, pX - 5, 1,  pY - 1, 6, pZ - 4, 10);
	m_world.fillBlocks(ItemID::COBBLESTONE, pX + 5, 1,  pY - 1, 6, pZ - 4, 10);

	// door
	m_world.destroyBlocks(pX - 1, 3, pY, 3, pZ + 5, 1);

	// windows
	m_world.destroyBlocks(pX - 5, 1, pY + 1, 2, pZ - 2, 5);
	m_world.destroyBlocks(pX + 5, 1, pY + 1, 2, pZ - 2, 5);

	m_player.addItem(ItemDatabase::get().getItem(ItemID::STONE_PICKAXE));
	m_player.addItem(ItemDatabase::get().getItem(ItemID::STONE_SHOVEL));

	core::io::info("World generation and player spawn finished");
}

void GameState::update(float delta, float mouseWheel) {
	static float tickTime = 0.f;
	tickTime += delta;

	// Update the player
	m_player.playerInput(m_world, mouseWheel);

	if (!m_player.isInventoryOpened()) {
		m_controller->getWindow().setMouseCursorVisible(false);
		m_player.update(m_world, delta);
		m_renderer.setWaterShader(m_player.isHeadInLiquid(m_world));

		// Update the world
		if (tickTime >= 0.1f) {
			m_world.tick();
			tickTime -= 0.1f;
		}


		m_world.update(m_player.getCamera().pos, delta);

		// Check if F3 pressed and if true, update debug panel
		static bool isFirstPress = true;
		if (sf::Keyboard::isKeyPressed(sf::Keyboard::F3)) {
			if (isFirstPress) {
				m_showDebugPanel = !m_showDebugPanel;
				isFirstPress = false;
			}
		} else {
			isFirstPress = true;
		}

		// FPS(frames per second)
		++m_frames;
		if (m_fpsTimer.getElapsedTime().asSeconds() >= 1.f) {
			m_fpsString = std::to_string(m_frames);

			m_fpsTimer.restart();
			m_frames = 0;
		}

		// Draw the cursor
		m_renderer.draw(m_cursor);
	} else {
		m_controller->getWindow().setMouseCursorVisible(true);
	}
}

void GameState::draw() {
	Camera &cam = m_player.getCamera();

	// Draw the world
	float chunksDrawn = m_world.draw(m_renderer, cam) / 16.f;

	// Draw the hitbox on the player look and player UI
	m_player.draw(m_renderer);

	// Draw the debug panel
	if (m_showDebugPanel) {
		static sf::Text text("", m_font, 36);
		int chunksCount = m_world.getChunks().size();
		int playerX = toChunkPos(m_player.getX());
		int playerZ = toChunkPos(m_player.getZ());
		
		text.setString("XYZ: " + std::to_string(m_player.getX()) + "  " + std::to_string(m_player.getY()) + "  " +
			std::to_string(m_player.getZ()) + "\nChunk XZ: " + std::to_string(playerX) + "  " +
			std::to_string(playerZ) + "\nFPS: " + m_fpsString + "\nTime: " + std::to_string(m_world.getWorldTime()) +
			"\nChunks loaded: " + std::to_string(chunksCount) + "\nChunks drawn: " + std::to_string(chunksDrawn));
		text.setPosition(15, 15);
		m_renderer.draw(text);
	}

	// Render
	const static sf::Vector3f dayColor(0.2f, 0.3f, 0.9f);
	const static sf::Vector3f nightColor(0.15f, 0.f, 0.3f);

	sf::Vector3f color = dayColor;
	if (!m_world.isDay()) {
		auto time = m_world.getWorldTime();
		if (m_world.isNight()) { // Night
			color = nightColor;
		} else if (m_world.isEvening()) {
			constexpr auto EVENING_LENGTH = NIGHT_START - DAY_END;
			auto interpolation = (time - DAY_END) / EVENING_LENGTH;

			color = nightColor * interpolation + dayColor * (1.f - interpolation);
		} else if (m_world.isMorning()) {
			constexpr auto MORNING_LENGTH = (DAY_SIZE - NIGHT_END) + DAY_START;
			auto interpolation = (time <= DAY_START ? time + (DAY_SIZE - NIGHT_END) : time - NIGHT_END) / MORNING_LENGTH;

			color = dayColor * interpolation + nightColor * (1.f - interpolation);
		}
	}

	m_renderer.clear(color);
	m_renderer.update(cam, m_controller->getWindow());
}
