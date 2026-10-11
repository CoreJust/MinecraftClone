#include <core/macro/Attributes.hpp>
#include <core/macro/Compiler.hpp>
#include <core/macro/Count.hpp>
#include <core/macro/LanguageVersion.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace {

constexpr size_t COUNT = CORE_PP_COUNT(1, 2, 3);

static_assert(COUNT == 3);
static_assert(CPP23);

} // namespace

TEST(MacroTest, CountsVariadicArguments) {
    EXPECT_EQ(COUNT, 3u);
}

TEST(MacroTest, CompilerMacrosIdentifyCurrentCompiler) {
#if defined(CLANG)
    EXPECT_GT(COMPILER_VERSION_MAJOR, 0);
#elif defined(GCC)
    EXPECT_GT(COMPILER_VERSION_MAJOR, 0);
#elif defined(MSVC)
    EXPECT_GT(COMPILER_VERSION_MAJOR, 0);
#else
    FAIL() << "Unknown compiler";
#endif
}

TEST(MacroTest, AttributesCanBeApplied) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    // A one-byte member intentionally makes ALIGNED(16) add tail padding.
    struct ALIGNED(16) AlignedValue {
        char value;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    EXPECT_EQ(alignof(AlignedValue), 16u);
}
