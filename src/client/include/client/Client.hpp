#pragma once

#include <core/net/Host.hpp>

namespace client {

class Client final {
public:
    void run();
private:
    bool pollServer();
private:
    core::Host m_host{ std::nullopt, 1, 1 };
};

} // namespace client
