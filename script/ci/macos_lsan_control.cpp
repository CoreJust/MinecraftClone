#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <thread>

namespace {

static constexpr std::chrono::seconds CALLBACK_DEADLINE{ 5 };
static constexpr std::chrono::milliseconds CALLBACK_POLL_INTERVAL{ 10 };
static constexpr uint32_t LEAK_SIZE = 4096;

std::atomic<bool> callback_observed{ false };
std::atomic<bool> callback_succeeded{ false };
std::atomic<bool> callback_leak_created{ false };

[[gnu::noinline]] void createApplicationLeak()
{
    void* allocation = std::malloc(LEAK_SIZE);
    if (allocation == nullptr) {
        std::fprintf(stderr, "malloc failed\n");
        std::abort();
    }
    std::memset(allocation, 0x5a, LEAK_SIZE);
    asm volatile("" : : "r"(allocation) : "memory");
}

[[nodiscard]] bool reportStatus(char const* operation, OSStatus status)
{
    bool const succeeded = status == noErr;
    std::fprintf(
        stderr,
        "%s: OSStatus=%d (%s)\n",
        operation,
        status,
        succeeded ? "success" : "failure");
    return succeeded;
}

void reportSkipped(char const* operation, char const* reason)
{
    std::fprintf(stderr, "%s: skipped (%s)\n", operation, reason);
}

[[gnu::noinline]] OSStatus renderAndLeak(
    void*,
    AudioUnitRenderActionFlags*,
    AudioTimeStamp const*,
    UInt32,
    UInt32,
    AudioBufferList* output)
{
    if (!callback_leak_created.exchange(true, std::memory_order_relaxed)) {
        createApplicationLeak();
    }
    if (output == nullptr) {
        callback_observed.store(true, std::memory_order_relaxed);
        return kAudio_ParamError;
    }
    for (UInt32 index = 0; index < output->mNumberBuffers; ++index) {
        AudioBuffer const& buffer = output->mBuffers[index];
        if (buffer.mData != nullptr) {
            std::memset(buffer.mData, 0, buffer.mDataByteSize);
        }
    }
    callback_succeeded.store(true, std::memory_order_relaxed);
    callback_observed.store(true, std::memory_order_relaxed);
    return noErr;
}

bool teardownAudioUnit(AudioUnit audio_unit, bool initialize_attempted, bool start_attempted)
{
    bool succeeded = true;
    if (start_attempted) {
        succeeded = reportStatus("AudioOutputUnitStop", AudioOutputUnitStop(audio_unit))
            && succeeded;
    } else {
        reportSkipped("AudioOutputUnitStop", "output unit was not started");
    }
    if (initialize_attempted) {
        succeeded = reportStatus("AudioUnitUninitialize", AudioUnitUninitialize(audio_unit))
            && succeeded;
    } else {
        reportSkipped("AudioUnitUninitialize", "output unit was not initialized");
    }
    succeeded = reportStatus("AudioComponentInstanceDispose", AudioComponentInstanceDispose(audio_unit))
        && succeeded;
    return succeeded;
}

int32_t runAudioCallback()
{
    AudioComponentDescription const description{
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_DefaultOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0,
    };
    AudioComponent const component = AudioComponentFindNext(nullptr, &description);
    if (component == nullptr) {
        std::fprintf(stderr, "default output component unavailable\n");
        return 2;
    }
    std::fprintf(stderr, "default output component: available\n");

    AudioUnit audio_unit = nullptr;
    bool const instance_created = reportStatus(
        "AudioComponentInstanceNew",
        AudioComponentInstanceNew(component, &audio_unit));
    if (!instance_created) {
        if (audio_unit != nullptr) {
            teardownAudioUnit(audio_unit, false, false);
        }
        return 2;
    }
    if (audio_unit == nullptr) {
        std::fprintf(stderr, "AudioComponentInstanceNew returned no instance\n");
        return 2;
    }

    bool initialize_attempted = false;
    bool start_attempted = false;
    AURenderCallbackStruct const callback{
        .inputProc = renderAndLeak,
        .inputProcRefCon = nullptr,
    };
    bool setup_succeeded = reportStatus(
        "AudioUnitSetProperty",
        AudioUnitSetProperty(
            audio_unit,
            kAudioUnitProperty_SetRenderCallback,
            kAudioUnitScope_Input,
            0,
            &callback,
            static_cast<UInt32>(sizeof(callback))));
    if (setup_succeeded) {
        initialize_attempted = true;
        setup_succeeded = reportStatus("AudioUnitInitialize", AudioUnitInitialize(audio_unit));
    }
    if (setup_succeeded) {
        start_attempted = true;
        setup_succeeded = reportStatus("AudioOutputUnitStart", AudioOutputUnitStart(audio_unit));
    }

    if (setup_succeeded) {
        auto const deadline = std::chrono::steady_clock::now() + CALLBACK_DEADLINE;
        while (!callback_observed.load(std::memory_order_relaxed)
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(CALLBACK_POLL_INTERVAL);
        }
        bool const observed = callback_observed.load(std::memory_order_relaxed);
        bool const callback_ok = callback_succeeded.load(std::memory_order_relaxed);
        std::fprintf(stderr, "audio callback observed: %s\n", observed ? "yes" : "no");
        std::fprintf(stderr, "audio callback succeeded: %s\n", callback_ok ? "yes" : "no");
        if (!observed || !callback_ok) {
            setup_succeeded = false;
        }
    }

    bool const teardown_succeeded = teardownAudioUnit(
        audio_unit,
        initialize_attempted,
        start_attempted);
    if (!setup_succeeded || !teardown_succeeded) {
        return 2;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: macos_lsan_control <clean|ordinary-leak|audio-callback-leak>\n");
        return 64;
    }
    std::string_view const mode{ argv[1] };
    if (mode == "clean") {
        std::fprintf(stderr, "mode: clean no-allocation control\n");
        return 0;
    }
    if (mode == "ordinary-leak") {
        std::fprintf(stderr, "mode: intentional 4096-byte application leak\n");
        createApplicationLeak();
        return 0;
    }
    if (mode == "audio-callback-leak") {
        std::fprintf(stderr, "mode: intentional 4096-byte default-output callback leak\n");
        return runAudioCallback();
    }
    std::fprintf(stderr, "unknown mode\n");
    return 64;
}
