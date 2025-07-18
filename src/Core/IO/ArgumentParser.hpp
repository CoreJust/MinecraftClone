#pragma once
#include <string>
#include <unordered_map>
#include <Core/Common/Int.hpp>
#include "Logger.hpp"

namespace core {
    class ArgumentParser final {
        std::unordered_map<std::string, std::string> m_arguments;
    public:
        ArgumentParser(int argc, char** argv) {
            for (int i = 0; i < argc; ++i) {
                std::string arg = argv[i];
                usize eqPos = arg.find('=', 1ull);
                if (eqPos == std::string::npos) {
                    m_arguments[arg] = "";
                } else {
                    m_arguments[arg.substr(0, eqPos)] = arg.substr(eqPos + 1);
                }
            }
        }

        ArgumentParser const& onOption(std::string const& option, auto&& func) const {
            if (auto it = m_arguments.find(option); it != m_arguments.end()) {
                info("Option {} is set to {}", option, it->second);
                func(it->second);
            }
            return *this;
        }
    };
} // namespace core
