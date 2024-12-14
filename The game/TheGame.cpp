#include "States/StatesController.h"

// todo: moon and sun, light, trees, biomes, fix physics of player, fix caves, optimization, greedy meshing with repeated textures, PCH
// average fps: 38-44. Max - 60. Least - 34.

int main() {
	try {
		Config cnf;
		StatesController controller(cnf);
		controller.run();
	} catch (std::runtime_error e) {
		system("pause");
	}

	return 0;
}