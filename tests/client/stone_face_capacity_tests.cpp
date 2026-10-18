#include <client/render/StoneFaceCapacity.hpp>

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

TEST(StoneFaceCapacityTest,
     DeferredGrowthRetainsTargetAndOldCapacityUntilRetry) {
  using Clock = client::detail::PendingStoneFaceGrowth::Clock;
  static constexpr uint32_t INITIAL_CAPACITY = 8U;
  static constexpr uint32_t REQUIRED_CAPACITY = 9U;
  static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
  static constexpr Clock::time_point STARTED_AT{Clock::duration{100}};
  static constexpr Clock::time_point FIRST_DEADLINE{Clock::duration{200}};
  static constexpr Clock::time_point RETRY_STARTED_AT =
      STARTED_AT + std::chrono::seconds{1};
  static constexpr Clock::time_point RETRY_DEADLINE =
      RETRY_STARTED_AT + std::chrono::milliseconds{16};

  client::detail::PendingStoneFaceGrowth pending;
  client::StoneFaceGrowthDiagnostics diagnostics;
  uint32_t capacity = INITIAL_CAPACITY;
  uint32_t presented_frames = 0U;
  bool defer_first_attempt = true;
  std::vector<Clock::time_point> recreation_deadlines;
  recreation_deadlines.reserve(3U);
  pending.coalesce(REQUIRED_CAPACITY, STARTED_AT);

  auto grow = [&](uint32_t const target_capacity,
                  Clock::time_point const deadline) {
    auto recreate = [&](Clock::time_point const actual_deadline) {
      EXPECT_EQ(actual_deadline, deadline);
      recreation_deadlines.push_back(actual_deadline);
      if (defer_first_attempt) {
        defer_first_attempt = false;
        throw client::detail::StoneFaceRecreationDeferred{};
      }
    };
    static_cast<void>(client::detail::tryGrowStoneFaceCapacity(
        capacity, target_capacity, MAXIMUM_CAPACITY, deadline, recreate,
        &diagnostics));
    return capacity;
  };
  auto present = [&] {
    ++presented_frames;
    return true;
  };

  EXPECT_FALSE(
      pending.render(capacity, FIRST_DEADLINE, grow, STARTED_AT, present));
  EXPECT_EQ(capacity, INITIAL_CAPACITY);
  EXPECT_EQ(pending.targetCapacity(), REQUIRED_CAPACITY);
  EXPECT_EQ(presented_frames, 0U);
  EXPECT_EQ(recreation_deadlines.size(), 2U);
  EXPECT_EQ(recreation_deadlines[0], FIRST_DEADLINE);
  EXPECT_EQ(recreation_deadlines[1], FIRST_DEADLINE);
  EXPECT_EQ(diagnostics.deferred_recreation_count, 1U);
  EXPECT_EQ(diagnostics.growth_success_count, 0U);
  EXPECT_EQ(diagnostics.rollback_count, 1U);

  EXPECT_TRUE(pending.render(capacity, RETRY_DEADLINE, grow, RETRY_STARTED_AT,
                             present));
  EXPECT_EQ(capacity, MAXIMUM_CAPACITY);
  EXPECT_EQ(pending.targetCapacity(), 0U);
  EXPECT_EQ(presented_frames, 1U);
  EXPECT_EQ(recreation_deadlines.size(), 3U);
  EXPECT_EQ(recreation_deadlines[2], RETRY_DEADLINE);
  EXPECT_EQ(diagnostics.growth_success_count, 1U);
}

TEST(StoneFaceCapacityTest,
     AllocationFailureIsNotClassifiedAsDeferredRecreation) {
  using Clock = std::chrono::steady_clock;
  static constexpr uint32_t INITIAL_CAPACITY = 8U;
  static constexpr uint32_t REQUIRED_CAPACITY = 9U;
  static constexpr uint32_t MAXIMUM_CAPACITY = 16U;
  static constexpr Clock::time_point DEADLINE{Clock::duration{200}};

  uint32_t capacity = INITIAL_CAPACITY;
  client::StoneFaceGrowthDiagnostics diagnostics;
  std::vector<uint32_t> recreated_capacities;
  recreated_capacities.reserve(2U);
  auto recreate = [&](Clock::time_point const) {
    recreated_capacities.push_back(capacity);
    if (capacity != INITIAL_CAPACITY) {
      throw std::runtime_error("simulated allocation failure");
    }
  };

  EXPECT_FALSE(client::detail::tryGrowStoneFaceCapacity(
      capacity, REQUIRED_CAPACITY, MAXIMUM_CAPACITY, DEADLINE, recreate,
      &diagnostics));

  EXPECT_EQ(capacity, INITIAL_CAPACITY);
  EXPECT_EQ(recreated_capacities,
            (std::vector<uint32_t>{MAXIMUM_CAPACITY, INITIAL_CAPACITY}));
  EXPECT_EQ(diagnostics.deferred_recreation_count, 0U);
  EXPECT_EQ(diagnostics.recreation_exception_count, 1U);
  EXPECT_EQ(diagnostics.recreation_exception_before_hook_count, 1U);
  EXPECT_EQ(diagnostics.rollback_count, 1U);
  EXPECT_EQ(
      std::string_view{diagnostics.first_recreation_exception_reason.data()},
      "simulated allocation failure");
}
