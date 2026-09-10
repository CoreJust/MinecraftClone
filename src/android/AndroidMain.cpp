#include "AndroidPlayerClient.hpp"

#include <core/IO/Log.hpp>
#include <core/net/Address.hpp>
#include <core/net/Net.hpp>

#include <android_native_app_glue.h>

#include <charconv>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct LaunchOptions final {
    core::Address server = *core::Address::make("10.0.2.2", 20'040);
    char character = '@';
};

class JniEnvironment final {
public:
    explicit JniEnvironment(JavaVM& vm)
        : m_vm(vm)
    {
        void* environment = nullptr;
        jint const status = m_vm.GetEnv(&environment, JNI_VERSION_1_6);
        if (status == JNI_EDETACHED) {
            if (m_vm.AttachCurrentThread(&m_environment, nullptr) != JNI_OK) {
                throw std::runtime_error("Cannot attach the Android app thread to Java");
            }
            m_attached = true;
        } else if (status == JNI_OK) {
            m_environment = static_cast<JNIEnv*>(environment);
        } else {
            throw std::runtime_error("Cannot obtain the Android JNI environment");
        }
    }

    ~JniEnvironment()
    {
        if (m_attached) {
            m_vm.DetachCurrentThread();
        }
    }

    [[nodiscard]]
    JNIEnv& get() const noexcept { return *m_environment; }
private:
    JavaVM& m_vm;
    JNIEnv* m_environment = nullptr;
    bool m_attached = false;
};

[[nodiscard]]
std::optional<std::string> stringExtra(JNIEnv& env, jobject const activity, char const* const key)
{
    jclass const activity_class = env.GetObjectClass(activity);
    jmethodID const get_intent = env.GetMethodID(
        activity_class,
        "getIntent",
        "()Landroid/content/Intent;"
    );
    jobject const intent = env.CallObjectMethod(activity, get_intent);
    jclass const intent_class = env.GetObjectClass(intent);
    jmethodID const get_string_extra = env.GetMethodID(
        intent_class,
        "getStringExtra",
        "(Ljava/lang/String;)Ljava/lang/String;"
    );
    jstring const java_key = env.NewStringUTF(key);
    auto const java_value = static_cast<jstring>(env.CallObjectMethod(intent, get_string_extra, java_key));
    if (env.ExceptionCheck()) {
        env.ExceptionClear();
        throw std::runtime_error("Cannot read Android launch extras");
    }
    if (java_value == nullptr) {
        return std::nullopt;
    }

    char const* const utf_value = env.GetStringUTFChars(java_value, nullptr);
    if (utf_value == nullptr) {
        throw std::runtime_error("Cannot decode an Android launch extra");
    }
    std::string result{ utf_value };
    env.ReleaseStringUTFChars(java_value, utf_value);
    return result;
}

[[nodiscard]]
core::Address parseServer(std::string const& value)
{
    size_t const delimiter = value.rfind(':');
    if (delimiter == std::string::npos || delimiter == 0 || delimiter + 1 == value.size()) {
        throw std::invalid_argument("Android server extra must use IP:PORT syntax");
    }

    uint32_t port = 0;
    std::string_view const port_string{ value.data() + delimiter + 1, value.size() - delimiter - 1 };
    auto const [end, error] = std::from_chars(port_string.begin(), port_string.end(), port);
    if (
        error != std::errc{}
        || end != port_string.end()
        || port == 0
        || port > std::numeric_limits<uint16_t>::max()
    ) {
        throw std::invalid_argument("Android server extra contains an invalid port");
    }

    auto address = core::Address::make(value.substr(0, delimiter), static_cast<uint16_t>(port));
    if (!address) {
        throw std::invalid_argument("Android server extra contains an invalid IP address");
    }
    return *address;
}

[[nodiscard]]
LaunchOptions launchOptions(android_app const& app)
{
    JniEnvironment environment{ *app.activity->vm };
    JNIEnv& env = environment.get();
    LaunchOptions options;
    if (auto const server = stringExtra(env, app.activity->clazz, "server")) {
        options.server = parseServer(*server);
    }
    if (auto const character = stringExtra(env, app.activity->clazz, "character")) {
        if (character->size() != 1 || !std::string_view{ "@#$%&" }.contains(character->front())) {
            throw std::invalid_argument("Android character extra must be one of @#$%&");
        }
        options.character = character->front();
    }
    return options;
}

} // namespace

extern "C" void android_main(android_app* const app)
{
    core::Log::ensureInit();
    core::Net::ensureInit();

    try {
        LaunchOptions const options = launchOptions(*app);
        CORE_INFO("Starting Android client {} as {}", options.server, options.character);
        game_android::AndroidPlayerClient client{ *app };
        client.run(options.server, options.character);
    } catch (std::exception const& error) {
        CORE_ERROR("Android client terminated: {}", error.what());
    }
    ANativeActivity_finish(app->activity);
}
