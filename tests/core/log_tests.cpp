#include <core/IO/Log.hpp>

#include <gtest/gtest.h>

#include <spdlog/sinks/base_sink.h>

#include <mutex>
#include <string>
#include <vector>

namespace {

class TestSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> messages;

protected:
    void sink_it_(spdlog::details::log_msg const& message) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(message, formatted);
        messages.emplace_back(formatted.data(), formatted.size());
    }

    void flush_() override { }
};

} // namespace

TEST(LogTest, EnsureInitIsIdempotent) {
    core::Log::ensureInit();
    EXPECT_NO_THROW(core::Log::ensureInit());
    ASSERT_NE(core::Log::getLogger(), nullptr);
}

TEST(LogTest, LevelFiltersAndEmitsMessages) {
    core::Log::ensureInit();
    auto sink = std::make_shared<TestSink>();
    core::Log::getLogger()->sinks().push_back(sink);

    core::Log::setLogLevel(spdlog::level::warn);
    CORE_INFO("filtered");
    CORE_WARN("emitted");
    core::Log::getLogger()->flush();

    ASSERT_EQ(sink->messages.size(), 1u);
    EXPECT_NE(sink->messages.front().find("emitted"), std::string::npos);
    core::Log::getLogger()->sinks().pop_back();
    core::Log::setLogLevel(spdlog::level::info);
}

TEST(LogTest, SetAndGetLogLevelRoundTrip) {
    core::Log::ensureInit();
    core::Log::setLogLevel(spdlog::level::trace);
    EXPECT_EQ(core::Log::getLogLevel(), spdlog::level::trace);
    core::Log::setLogLevel(spdlog::level::err);
    EXPECT_EQ(core::Log::getLogLevel(), spdlog::level::err);
    core::Log::setLogLevel(spdlog::level::info);
}
