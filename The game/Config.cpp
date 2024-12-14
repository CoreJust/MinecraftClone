#include "Config.h"
#include <string>
#include <fstream>

const std::string configPath = "res/config.cfg";

Config::Config() {
	std::ifstream file(configPath);
	std::string line;

	while (std::getline(file, line)) {
		if (line == "Width") {
			file >> width;
		} else if (line == "Height") {
			file >> height;
		}
	}

	file.close();
}