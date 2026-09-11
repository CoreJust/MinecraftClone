#include <client/render/DebugHud.hpp>

#include <gtest/gtest.h>
#if defined(_WIN32)
#include <malloc.h>
#endif

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <string_view>

namespace allocation_test {

std::atomic_bool tracking = false;
std::atomic_size_t allocations = 0;

void* allocate(size_t const size)
{
    void* const result = std::malloc(size == 0 ? 1 : size);
    if (result == nullptr) {
        throw std::bad_alloc{};
    }
    if (tracking.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

void* allocateAligned(size_t const size, size_t const alignment)
{
#if defined(_WIN32)
    void* const result = _aligned_malloc(size == 0 ? alignment : size, alignment);
    if (result == nullptr) {
        throw std::bad_alloc{};
    }
#else
    void* result = nullptr;
    if (posix_memalign(&result, alignment, size == 0 ? alignment : size) != 0) {
        throw std::bad_alloc{};
    }
#endif
    if (tracking.load(std::memory_order_relaxed)) {
        allocations.fetch_add(1, std::memory_order_relaxed);
    }
    return result;
}

void deallocateAligned(void* const pointer) noexcept
{
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

} // namespace allocation_test

void* operator new(size_t const size)
{
    return allocation_test::allocate(size);
}

void* operator new[](size_t const size)
{
    return allocation_test::allocate(size);
}

void operator delete(void* const pointer) noexcept
{
    std::free(pointer);
}

void operator delete[](void* const pointer) noexcept
{
    std::free(pointer);
}

void operator delete(void* const pointer, size_t) noexcept
{
    std::free(pointer);
}

void operator delete[](void* const pointer, size_t) noexcept
{
    std::free(pointer);
}

void* operator new(size_t const size, std::nothrow_t const&) noexcept
{
    try {
        return allocation_test::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](size_t const size, std::nothrow_t const&) noexcept
{
    try {
        return allocation_test::allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* const pointer, std::nothrow_t const&) noexcept
{
    std::free(pointer);
}

void operator delete[](void* const pointer, std::nothrow_t const&) noexcept
{
    std::free(pointer);
}

void* operator new(size_t const size, std::align_val_t const alignment)
{
    return allocation_test::allocateAligned(size, static_cast<size_t>(alignment));
}

void* operator new[](size_t const size, std::align_val_t const alignment)
{
    return allocation_test::allocateAligned(size, static_cast<size_t>(alignment));
}

void operator delete(void* const pointer, std::align_val_t) noexcept
{
    allocation_test::deallocateAligned(pointer);
}

void operator delete[](void* const pointer, std::align_val_t) noexcept
{
    allocation_test::deallocateAligned(pointer);
}

void operator delete(void* const pointer, size_t, std::align_val_t) noexcept
{
    allocation_test::deallocateAligned(pointer);
}

void operator delete[](void* const pointer, size_t, std::align_val_t) noexcept
{
    allocation_test::deallocateAligned(pointer);
}

void* operator new(
    size_t const size,
    std::align_val_t const alignment,
    std::nothrow_t const&
) noexcept
{
    try {
        return allocation_test::allocateAligned(size, static_cast<size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](
    size_t const size,
    std::align_val_t const alignment,
    std::nothrow_t const&
) noexcept
{
    try {
        return allocation_test::allocateAligned(size, static_cast<size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}

void operator delete(
    void* const pointer,
    std::align_val_t const,
    std::nothrow_t const&
) noexcept
{
    allocation_test::deallocateAligned(pointer);
}

void operator delete[](
    void* const pointer,
    std::align_val_t const,
    std::nothrow_t const&
) noexcept
{
    allocation_test::deallocateAligned(pointer);
}

namespace {

size_t writeMarker(
    char* const destination,
    size_t const capacity,
    double,
    int,
    void*
) noexcept {
    if (capacity == 0) {
        return 0;
    }
    destination[0] = 'X';
    return 1;
}

double readClock(void* const context) noexcept
{
    return *static_cast<double*>(context);
}

} // namespace

TEST(DebugHudTest, PacksFourSanitizedAsciiBytesInLittleEndianOrder)
{
    EXPECT_EQ(
        client::packDebugHudAscii('A', 'B', 'C', 'D'),
        0x4443'4241U
    );
    EXPECT_EQ(
        client::packDebugHudAscii('A', 0, 'C', 'D'),
        0x4443'0041U
    );
    EXPECT_EQ(
        client::packDebugHudAscii(0x01, 0x80, '~', 0x7f),
        0x7f7e'3f3fU
    );
}

TEST(DebugHudTest, RendererDefaultIsDisabledUntilGameplayEnablesIt)
{
    client::DebugHudState hud;
    client::DebugHudText text;
    client::DebugHudBatch batch;

    EXPECT_FALSE(hud.snapshot().enabled);
    EXPECT_FALSE(hud.buildBatch(text, batch));
    hud.setEnabled(true);
    EXPECT_TRUE(hud.buildBatch(text, batch));
}

TEST(DebugHudTest, F1ToggleLatchDebouncesPressesAndCanBeReset)
{
    client::DebugHudToggleLatch latch;
    client::DebugHudState hud;

    if (latch.update(true)) {
        hud.toggle();
    }
    EXPECT_TRUE(hud.snapshot().enabled);
    EXPECT_FALSE(latch.update(true));
    EXPECT_TRUE(hud.snapshot().enabled);
    EXPECT_FALSE(latch.update(false));
    if (latch.update(true)) {
        hud.toggle();
    }
    EXPECT_FALSE(hud.snapshot().enabled);

    latch.reset();
    EXPECT_TRUE(latch.update(true));
}

TEST(DebugHudTest, PadsIncompleteWordsAndHonorsNulGlyphs)
{
    client::DebugHudText text;
    text.bytes[0] = 'A';
    text.bytes[1] = 0;
    text.bytes[2] = 'C';
    text.size = 3;
    client::DebugHudBatch batch;

    client::packDebugHudText(text, batch);

    ASSERT_EQ(batch.size, 1U);
    EXPECT_EQ(batch.instances[0].packed_ascii, 0x0043'0041U);
}

TEST(DebugHudTest, PackingIsBoundedByFixedCapacity)
{
    client::DebugHudText text;
    text.bytes.fill('A');
    text.size = client::DEBUG_HUD_MAX_TEXT_BYTES + 100;
    client::DebugHudBatch batch;

    client::packDebugHudText(text, batch);

    EXPECT_EQ(batch.size, client::DEBUG_HUD_WORDS_PER_LINE);
    EXPECT_EQ(
        batch.instances[client::DEBUG_HUD_WORDS_PER_LINE - 1U].packed_ascii,
        0x4141'4141U
    );
}

TEST(DebugHudTest, UsesPresentedFramesInAOneSecondSlidingWindow)
{
    client::DebugHudState hud;
    client::DebugHudInput input{ .presented = true };

    hud.updateAt(0.0, input);
    hud.updateAt(0.5, input);
    hud.updateAt(1.0, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 2.0);

    input.presented = false;
    hud.updateAt(2.1, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 0.0);
}

TEST(DebugHudTest, ExcludesDroppedFramesFromPresentedFps)
{
    client::DebugHudState hud;
    client::DebugHudInput input{ .presented = true };

    hud.updateAt(0.0, input);
    input.presented = false;
    hud.updateAt(0.25, input);
    input.presented = true;
    hud.updateAt(0.5, input);
    input.presented = false;
    hud.updateAt(0.75, input);

    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 2.0);

    hud.updateAt(1.1, input);
    EXPECT_DOUBLE_EQ(hud.snapshot().presented_fps, 0.0);
}

TEST(DebugHudTest, ClocksAreInjectableAndUptimeNeverRewinds)
{
    double clock_time = 4.0;
    client::DebugHudState hud(
        client::DebugHudClock{ .now = &readClock, .context = &clock_time }
    );

    hud.update({});
    clock_time = 2.0;
    hud.update({});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 0.0);

    clock_time = 5.5;
    hud.update({});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 1.5);
}

TEST(DebugHudTest, IgnoresNonFiniteClockValues)
{
    client::DebugHudState hud;
    hud.updateAt(std::numeric_limits<double>::quiet_NaN(), {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 0.0);

    hud.updateAt(2.0, {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 2.0);

    hud.updateAt(std::numeric_limits<double>::infinity(), {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 2.0);
    hud.updateAt(3.0, {});
    EXPECT_DOUBLE_EQ(hud.snapshot().uptime_seconds, 3.0);
}

TEST(DebugHudTest, FormatsStableGoldenValuesAndDegreeLabels)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    hud.updateAt(
        10.0,
        client::DebugHudInput{
            .presented = false,
            .player_x = 1.25F,
            .player_y = -2.3F,
            .player_z = 4.56F,
            .camera_yaw_degrees = 45.0F,
            .camera_pitch_degrees = -10.0F,
            .camera_roll_degrees = 3.0F,
        }
    );
    client::DebugHudText text;

    ASSERT_TRUE(hud.formatText(text));
    EXPECT_EQ(
        std::string_view(text.bytes.data(), text.size),
        "FPS:0.0\nUPTIME:0.0s\nXYZ:1.2,-2.3,4.6\nY/P/R(deg):45.0,-10.0,3.0"
    );
}

TEST(DebugHudTest, PacksFourLinesIntoFixedShaderRows)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    hud.updateAt(
        10.0,
        client::DebugHudInput{
            .player_x = 1.25F,
            .player_y = -2.3F,
            .player_z = 4.56F,
            .camera_yaw_degrees = 45.0F,
            .camera_pitch_degrees = -10.0F,
        }
    );
    client::DebugHudText text;
    client::DebugHudBatch batch;

    ASSERT_TRUE(hud.buildBatch(text, batch));
    ASSERT_EQ(batch.size, client::DEBUG_HUD_MAX_INSTANCES);
    EXPECT_EQ(batch.instances[0].packed_ascii, client::packDebugHudAscii('F', 'P', 'S', ':'));
    EXPECT_EQ(
        batch.instances[client::DEBUG_HUD_WORDS_PER_LINE].packed_ascii,
        client::packDebugHudAscii('U', 'P', 'T', 'I')
    );
    EXPECT_EQ(
        batch.instances[2U * client::DEBUG_HUD_WORDS_PER_LINE].packed_ascii,
        client::packDebugHudAscii('X', 'Y', 'Z', ':')
    );
    EXPECT_EQ(
        batch.instances[3U * client::DEBUG_HUD_WORDS_PER_LINE].packed_ascii,
        client::packDebugHudAscii('Y', '/', 'P', '/')
    );
}

TEST(DebugHudTest, SupportsInjectedFormattingAndDpiToggleState)
{
    client::DebugHudState hud(
        {},
        client::DebugHudNumberFormatter{ .format = &writeMarker }
    );
    hud.setEnabled(true);
    hud.setDpiScale(100.0F);
    EXPECT_FLOAT_EQ(hud.snapshot().dpi_scale, 8.0F);
    hud.setDpiScale(0.0F);
    EXPECT_FLOAT_EQ(hud.snapshot().dpi_scale, 0.25F);

    hud.updateAt(2.0, {});
    client::DebugHudText text;
    ASSERT_TRUE(hud.formatText(text));
    EXPECT_EQ(
        std::string_view(text.bytes.data(), text.size),
        "FPS:X\nUPTIME:Xs\nXYZ:X,X,X\nY/P/R(deg):X,X,X"
    );

    hud.toggle();
    EXPECT_FALSE(hud.snapshot().enabled);
    EXPECT_FALSE(hud.formatText(text));
}

TEST(DebugHudTest, BoundsSignedCameraAnglesToTheFourthLayoutRow)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    hud.updateAt(
        10.0,
        client::DebugHudInput{
            .camera_yaw_degrees = 359.0F,
            .camera_pitch_degrees = -89.0F,
            .camera_roll_degrees = 359.0F,
        }
    );
    client::DebugHudText text;

    ASSERT_TRUE(hud.formatText(text));
    std::string_view const formatted{ text.bytes.data(), text.size };
    size_t const last_line = formatted.rfind('\n');
    ASSERT_NE(last_line, std::string_view::npos);
    EXPECT_LE(
        formatted.size() - last_line - 1U,
        client::DEBUG_HUD_MAX_LINE_BYTES
    );
    EXPECT_EQ(
        formatted.substr(last_line + 1U),
        "Y/P/R(deg):359.0,-89.0,359.0"
    );
}

TEST(DebugHudTest, ReusesFixedBuffersWithoutSteadyFrameAllocations)
{
    client::DebugHudState hud;
    hud.setEnabled(true);
    client::DebugHudText text;
    client::DebugHudBatch batch;

    allocation_test::allocations.store(0, std::memory_order_relaxed);
    allocation_test::tracking.store(true, std::memory_order_relaxed);
    for (size_t frame = 0; frame < 120; ++frame) {
        hud.updateAt(
            static_cast<double>(frame) / 120.0,
            client::DebugHudInput{ .presented = true }
        );
        static_cast<void>(hud.buildBatch(text, batch));
    }
    allocation_test::tracking.store(false, std::memory_order_relaxed);

    EXPECT_EQ(allocation_test::allocations.load(std::memory_order_relaxed), 0U);
    EXPECT_GT(batch.size, 0U);
}
