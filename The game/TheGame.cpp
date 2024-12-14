#include <Core/Test/TestMain.hpp>
#include "States/StatesController.h"

int main() {
	TEST_MAIN();

	try {
		Config cnf;
		StatesController controller(cnf);
		controller.run();
	} catch (std::runtime_error e) {
		system("pause");
	}

	return 0;
}