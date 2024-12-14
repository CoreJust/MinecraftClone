#include "Console.h"
#include <stdexcept>

void printInfo(std::string str) {
	std::cout << "[INFO]: " << str << '\n';
}

void printError(std::string str) {
	std::cout << "[ERROR]: " << str << "\a\n";
}

void fatalError(std::string &&str) {
	printError(str);
	throw std::runtime_error(str);
}
