#pragma once

#include <core/net/Host.hpp>

namespace server {

class Server final {
public:
    void run();
private:
    core::Host m_host{ core::Address::localhost(20040), 1, 1 };
};

} // namespace server
