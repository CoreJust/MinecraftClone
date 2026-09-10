#include <core/IO/StacktraceFmt.hpp>

#if CORE_HAS_STACKTRACE

#include <sstream>

namespace fmt {

context::iterator formatter<std::stacktrace>::format(std::stacktrace const& st, format_context& ctx) const {
    std::ostringstream oss;
    oss << st;
    std::string str = oss.str();
    if (!str.empty() && str.back() == '\n') {
        str.pop_back();
    }
    return formatter<std::string_view>::format(std::string_view(str), ctx);
}

} // namespace fmt

#endif
