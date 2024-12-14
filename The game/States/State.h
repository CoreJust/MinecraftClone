#pragma once

class StatesController;

class State {
public:
	State(StatesController* controller)
		: m_controller(controller) {	}

	virtual void update(float delta, float mouseWheel) = 0;
	virtual void draw() = 0;

protected:
	StatesController* m_controller;
};