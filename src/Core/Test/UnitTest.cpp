#include "UnitTest.hpp"
#include <iostream>
#include <vector>
#include <algorithm>
#include <Core/Common/Assert.hpp>
#include <Core/Common/DurationToString.hpp>
#include <Core/Common/Timer.hpp>
#include <Core/IO/ConsoleColor.hpp>

namespace core {
namespace {
	struct UnitTest {
		char const* name;
		char const* file;
		usize line;
		UnitTestFunction test;
	};

	void printTestsFileHeader(UnitTest const& test) {
		std::cout << White << "\n=== Tests file " << test.file << " ===\n";
	}

	void printTestInfo(UnitTest const& test, usize const testIndex) {
		std::cout << White << "Test[" << testIndex << "] " << test.name << ": \n";
	}

	void printTestsResults(usize const successes, usize const failures, Duration const duration) {
		if (!failures) {
			std::cout << Green << "All " << successes << " tests have passed!" << std::endl;
		} else if (!successes) {
			std::cout << Red << "All " << failures << " tests have failed." << std::endl;
		} else {
			std::cout << Green << successes << " tests have passed!" << std::endl;
			std::cout << Red << failures << " tests have failed." << std::endl;
		}
		std::cout << Gray << "Overall tests time: " << durationToString(duration) << std::endl;
	}

	std::vector<Duration> g_testIterationDurations;
	bool g_isSuccess;

	// Returns true if this is the last iteration
	bool onTestIterationEnd(Duration const duration, UnitTestHelper::BenchmarkingOptions const& options) {
		g_testIterationDurations.emplace_back(duration);
		if (g_testIterationDurations.size() < options.window)
			return false;
		if (g_testIterationDurations.size() >= options.maxIterations) {
			std::cout << Yellow << "\tUnstable test iteration time!" << std::endl;
			return true;
		}

		Duration minDuration = duration, maxDuration = duration;
		for (usize i = g_testIterationDurations.size(); i-- > g_testIterationDurations.size() - options.window;) {
			Duration iterationDuration = g_testIterationDurations[i];
			if (iterationDuration < minDuration) {
				minDuration = iterationDuration;
			} else if (iterationDuration > maxDuration) {
				maxDuration = iterationDuration;
			}
		}

		usize const minNs = minDuration.ns + options.timeAddition;
		usize const maxNs = maxDuration.ns + options.timeAddition;
		double const diff = static_cast<double>(maxNs) / static_cast<double>(minNs) - 1.0;
		return diff < options.maxDeviation;
	}

	std::string_view averageIterationTime(UnitTestHelper::BenchmarkingOptions const& options) {
		usize i = g_testIterationDurations.size() - options.window;
		usize sumNs = 0;
		while (i < g_testIterationDurations.size()) {
			sumNs += g_testIterationDurations[i].ns;
			++i;
		}
		Duration avgDuration { sumNs / options.window };
		return durationToString(avgDuration);
	}

	void onTestEnd(Duration const duration, bool success, UnitTestHelper::BenchmarkingOptions const& options) {
		g_isSuccess = success;
		if (success) {
			if (g_testIterationDurations.empty()) {
				std::cout << Green << "\tPassed in " << durationToString(duration) << std::endl;
			} else {
				std::cout << Green << "\tPassed in " << durationToString(duration) 
					<< "; average time " << averageIterationTime(options) << std::endl;
			}
		} else {
			if (g_testIterationDurations.empty()) {
				std::cout << Red << "\tFailed in " << durationToString(duration) << std::endl;
			} else {
				std::cout << Red << "\tFailed in " << durationToString(duration)
					<< "; average time " << averageIterationTime(options) << std::endl;
			}
		}
	}

	bool runUnitTest(UnitTest const& test, usize const testIndex, bool const shallPrintHeader) {
		if (shallPrintHeader)
			printTestsFileHeader(test);
		printTestInfo(test, testIndex);
		try {
			g_testIterationDurations.clear();
			(*test.test)(UnitTestHelper{ });
			return g_isSuccess;
		} catch (...) {
			std::cout << Red << "\tUncaught exception occured during test." << std::endl;
			return false;
		}
	}

	class UnitTestRunner final {
		std::vector<UnitTest> m_unitTests;

	public:
		static inline UnitTestRunner* s_instance = nullptr;

		void addTest(UnitTestNote note, UnitTestFunction unitTest) {
			s_instance = this;
			m_unitTests.emplace_back(note.name, note.file, note.line, unitTest);
		}

		void runAll() {
			std::sort(m_unitTests.begin(), m_unitTests.end(), [](auto const& a, auto const& b) { return a.file < b.file; });

			usize index = 0;
			usize successCount = 0;
			char const* prevTestFile = nullptr;
			Timer allTestsTimer;
			for (const auto& test : m_unitTests) {
				successCount += runUnitTest(test, index++, prevTestFile != test.file);
				prevTestFile = test.file;
			}

			printTestsResults(successCount, m_unitTests.size() - successCount, allTestsTimer.elapsed());
		}
	};
}

	UnitTestHelper::UnitTestIteratedObject::~UnitTestIteratedObject() {
		if (onTestIterationEnd(timer.elapsed(), pIter->options)) 
			pIter->isEnd = true;
	}

	UnitTestHelper::UnitTestIterator::value_type UnitTestHelper::UnitTestIterator::operator*() const {
		return UnitTestIteratedObject { this, {} };
	}

	UnitTestHelper::UnitTestIterator::pointer UnitTestHelper::UnitTestIterator::operator->() const {
		return nullptr;
	}

	UnitTestHelper::UnitTestIterator& UnitTestHelper::UnitTestIterator::operator++() {
		return *this;
	}

	UnitTestHelper::UnitTestIterator UnitTestHelper::UnitTestIterator::operator++(int) {
		return *this;
	}

	bool UnitTestHelper::UnitTestIterator::operator==(UnitTestIterator const& other) const noexcept {
		return isEnd == other.isEnd;
	}

	UnitTestHelper::UnitTestHelper(UnitTestHelper&& other) noexcept 
		: m_timer(std::exchange(other.m_timer, Timer(Time{ 0 })))
		, m_success(other.m_success) {
	}

	UnitTestHelper::~UnitTestHelper() {
		if (m_timer.startTime().ns)
			onTestEnd(m_timer.elapsed(), m_success, m_options);
	}
		
	UnitTestHelper& UnitTestHelper::setBenchmarkingOptions(BenchmarkingOptions options) &noexcept {
		ASSERT(options.window > 0, "Benchmarking options: window cannot be null");
		ASSERT(options.maxDeviation > 0.00001, "Benchmarking options: maxDeviation cannot be less than 0.00001");
		m_options = options;
		return *this;
	}

	void UnitTestHelper::checkFailure(char const* const message, std::source_location const location) {
		std::cout << Red << "\tTest assertion failed: " << message << " in " << location.file_name() << ":" << location.line() << std::endl;
		m_success = false;
	}

	UnitTestHelper::UnitTestIterator UnitTestHelper::begin() &noexcept {
		return { false, m_options };
	}

	UnitTestHelper::UnitTestIterator UnitTestHelper::end() &noexcept {
		return { true, m_options };
	}

	void registerUnitTest(UnitTestNote note, UnitTestFunction unitTest) {
		static UnitTestRunner s_unitTestRunner;
		s_unitTestRunner.addTest(std::move(note), unitTest);
	}

	void runAll() {
		if(UnitTestRunner::s_instance)
			UnitTestRunner::s_instance->runAll();
	}
} // namespace core
