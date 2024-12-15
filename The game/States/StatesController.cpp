#include "StatesController.h"
#include <ctime>
#include <Core/IO/Logger.hpp>
#include <Core/GL/Debug.hpp>
#include "GameState.h"

const char* const StatesController::TITLE = "The Game v0";

StatesController::StatesController(Config config)
	: m_state(RUNNING), m_width(config.width), m_height(config.height) {
	core::io::info("Loading the initial state");

	// Initialize random
	srand(clock());

	// Set up Window
	core::io::info("Creating the window");
	sf::ContextSettings sets;
	sets.majorVersion = 3;
	sets.minorVersion = 3;
	sets.depthBits = 24;
	sets.antialiasingLevel = 4;
	m_window.create(sf::VideoMode(m_width, m_height), "The Game Classic", sf::Style::Close, sets);

	m_window.setMouseCursorVisible(false);

	// Set up the GLEW library
	core::io::info("Initializing GLEW");
	glewExperimental = GL_TRUE;
	if (auto errorCode = glewInit(); errorCode != GLEW_OK)
		core::io::fatal("Failed to initialize the GLEW library, error is {}", (const char*)glewGetErrorString(errorCode));

	core::gl::enableGLDebug();

	glViewport(0, 0, m_width, m_height);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_MULTISAMPLE);

	core::io::info("State loaded successfully!");

	// make the first state
	pushState<GameState>(this, sf::Vector2i(m_width, m_height));
}

void StatesController::run() {
	core::io::info("Running the game");
	m_state = RUNNING;

	sf::Clock clock;
	m_deltaTime = clock.getElapsedTime().asSeconds();

	// Main loop
	while (m_state == RUNNING) {
		pollEvents();

		// Calculate delta time
		auto time = clock.getElapsedTime().asSeconds();
		m_deltaTime = time - m_lastFrame;
		m_lastFrame = time;
		
		// Game actions
		if (m_window.hasFocus())
			m_backState->update(m_deltaTime, m_wheelChange);
		else
			m_window.setMouseCursorVisible(true);

		m_backState->draw();

		m_window.display();
		if (m_isPopState) {
			m_states.pop_back();
			if (m_states.size() == 0)
				exit();

			m_backState = m_states.back();
			m_isPopState = false;
		}
	}
}

void StatesController::pollEvents() {
	sf::Event e;

	m_wheelChange = 0.f;
	while (m_window.pollEvent(e)) {
		if (e.type == sf::Event::Closed || (e.type == sf::Event::KeyPressed && e.key.code == sf::Keyboard::Escape))
			exit();
		
		if (e.type == sf::Event::MouseWheelScrolled) {
			m_wheelChange = e.mouseWheelScroll.delta;
		}
	}
}

sf::RenderWindow& StatesController::getWindow() {
	return m_window;
}

void StatesController::exit() {
	m_state = EXITING;
}

void StatesController::popState() {
	m_isPopState = true;
}

int StatesController::getWidth() const {
	return m_width;
}

int StatesController::getHeight() const {
	return m_height;
}
