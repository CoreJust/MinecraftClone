#pragma once
#include <iostream>
#include <vector>
#include <GL/glew.h>
#include <SFML\Graphics.hpp>
#include "State.h"
#include "../Config.h"

class StatesController final {
public:
	StatesController(Config config);
	void run();
	void pollEvents();

	sf::RenderWindow& getWindow();
	void exit();

	template<class T, class... Args>
	void pushState(Args... args) {
		m_backState = new T(args...);
		m_states.push_back(m_backState);
	}

	void popState();

	int getWidth() const;
	int getHeight() const;

private:
	const static char* const TITLE;

private:
	std::vector<State*> m_states;
	State* m_backState;
	sf::RenderWindow m_window;

	bool m_isPopState = false;
	int m_width;
	int m_height;

	// state of the app
	enum AppState {
		RUNNING,
		EXITING
	} m_state;

	float m_wheelChange = 0;
	float m_lastFrame = 0;
	float m_deltaTime = 0;
};