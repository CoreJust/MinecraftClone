#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Windows SDK headers below require Windows types first.
// DONT_CHECK INCLUDE_ORDER
#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using Digest = std::array<uint8_t, 32>;
constexpr char OWNER[] = "MinecraftClone SinglePlayer cache v1\n";

struct Failure final {
    std::wstring message;
};

void require(bool const success, std::wstring const& message)
{
    if (!success) {
        throw Failure{message};
    }
}

std::wstring windowsError(std::wstring const& action)
{
    DWORD const code = GetLastError();
    return action + L" (Windows error " + std::to_wstring(code) + L").";
}


void requireWindows(bool const success, std::wstring const& action)
{
    if (!success) {
        throw Failure{windowsError(action)};
    }
}

class Handle final {
public:
    explicit Handle(HANDLE const value = INVALID_HANDLE_VALUE) noexcept : m_value(value) { }
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;
    Handle(Handle&& other) noexcept : m_value(std::exchange(other.m_value, INVALID_HANDLE_VALUE)) { }
    ~Handle() { close(); }
    void close() noexcept
    {
        if (valid()) {
            CloseHandle(m_value);
            m_value = INVALID_HANDLE_VALUE;
        }
    }
    [[nodiscard]] bool valid() const noexcept { return m_value != INVALID_HANDLE_VALUE && m_value != nullptr; }
    [[nodiscard]] HANDLE get() const noexcept { return m_value; }
private:
    HANDLE m_value;
};

class Hash final {
public:
    Hash()
    {
        try {
            require(BCryptOpenAlgorithmProvider(&m_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
                L"Windows could not initialize file verification. Please restart the game.");
            DWORD bytes = 0;
            DWORD size = 0;
            require(BCryptGetProperty(m_algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size),
                        sizeof(size), &bytes, 0) >= 0,
                L"Windows could not initialize file verification.");
            m_object.resize(size);
            require(BCryptCreateHash(m_algorithm, &m_hash, m_object.data(), size, nullptr, 0, 0) >= 0,
                L"Windows could not initialize file verification.");
        } catch (...) {
            if (m_hash != nullptr) {
                BCryptDestroyHash(m_hash);
            }
            if (m_algorithm != nullptr) {
                BCryptCloseAlgorithmProvider(m_algorithm, 0);
            }
            throw;
        }
    }
    Hash(Hash const&) = delete;
    Hash& operator=(Hash const&) = delete;
    ~Hash()
    {
        BCryptDestroyHash(m_hash);
        BCryptCloseAlgorithmProvider(m_algorithm, 0);
    }
    void add(std::span<uint8_t const> bytes)
    {
        while (!bytes.empty()) {
            auto const part = bytes.first(std::min<size_t>(bytes.size(), 65'536U));
            require(BCryptHashData(m_hash, const_cast<PUCHAR>(part.data()), static_cast<ULONG>(part.size()), 0) >= 0,
                L"Windows could not verify the game files.");
            bytes = bytes.subspan(part.size());
        }
    }
    [[nodiscard]] Digest finish()
    {
        Digest result{};
        require(BCryptFinishHash(m_hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0,
            L"Windows could not finish game file verification.");
        return result;
    }
private:
    BCRYPT_ALG_HANDLE m_algorithm = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    std::vector<uint8_t> m_object;
};

Digest digest(std::span<uint8_t const> const bytes)
{
    Hash hash;
    hash.add(bytes);
    return hash.finish();
}

std::wstring hexadecimal(Digest const& hash)
{
    constexpr wchar_t DIGITS[] = L"0123456789abcdef";
    std::wstring result;
    for (uint8_t const byte : hash) {
        result += DIGITS[byte >> 4U];
        result += DIGITS[byte & 15U];
    }
    return result;
}

std::wstring utf16(std::string const& text)
{
    require(text.size() <= static_cast<size_t>(std::numeric_limits<int>::max()), L"A game path is too long.");
    int const length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    require(length > 0, L"The embedded game contains an invalid path. Download the game again.");
    std::wstring result(static_cast<size_t>(length), L'\0');
    require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                result.data(), length) == length,
        L"Windows could not read a game path.");
    return result;
}

struct Entry final {
    fs::path path;
    Digest hash;
    std::span<uint8_t const> data;
};

class Reader final {
public:
    explicit Reader(std::span<uint8_t const> const bytes) : m_bytes(bytes) { }
    std::span<uint8_t const> take(size_t const count)
    {
        require(count <= m_bytes.size(), L"The game download is incomplete. Download this executable again.");
        auto const result = m_bytes.first(count);
        m_bytes = m_bytes.subspan(count);
        return result;
    }
    template <std::unsigned_integral T>
    T number()
    {
        T result{};
        auto const bytes = take(sizeof(T));
        std::memcpy(&result, bytes.data(), sizeof(T));
        return result;
    }
    [[nodiscard]] bool empty() const noexcept { return m_bytes.empty(); }
private:
    std::span<uint8_t const> m_bytes;
};

std::vector<Entry> entries(std::span<uint8_t const> const bundle)
{
    Reader reader{bundle};
    auto const magic = reader.take(8U);
    require(std::memcmp(magic.data(), "MCSINGLE", 8U) == 0 && reader.number<uint32_t>() == 1U,
        L"This game package is not supported. Download the current executable.");
    uint32_t const count = reader.number<uint32_t>();
    require(count > 0U && count <= 4'096U, L"The embedded game file list is invalid.");
    std::vector<Entry> result;
    result.reserve(count);
    std::set<std::wstring> names;
    for (uint32_t index = 0; index < count; ++index) {
        uint32_t const length = reader.number<uint32_t>();
        uint64_t const size = reader.number<uint64_t>();
        Digest hash{};
        auto const encoded_hash = reader.take(hash.size());
        std::copy(encoded_hash.begin(), encoded_hash.end(), hash.begin());
        require(length > 0U && length <= 1'024U && size <= std::numeric_limits<size_t>::max(),
            L"An embedded game file has an invalid size.");
        auto const encoded_name = reader.take(length);
        std::string const name{reinterpret_cast<char const*>(encoded_name.data()), encoded_name.size()};
        require(name.find('\\') == std::string::npos && name.find(':') == std::string::npos
                && name.find('\0') == std::string::npos,
            L"An embedded game path is invalid.");
        fs::path const relative{utf16(name)};
        require(!relative.has_root_path(), L"An embedded game path is not relative.");
        for (auto const& part : relative) {
            require(part != L".." && part != L"." && !part.empty(), L"An embedded game path is unsafe.");
        }
        std::wstring folded = relative.generic_wstring();
        CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
        require(names.insert(folded).second, L"The embedded game contains conflicting file names.");
        auto const data = reader.take(static_cast<size_t>(size));
        require(digest(data) == hash, L"The game download is damaged. Download this executable again.");
        result.push_back({relative, hash, data});
    }
    require(reader.empty() && names.contains(L"mc_main.exe"), L"The embedded game is incomplete.");
    return result;
}

bool plain(fs::path const& path)
{
    DWORD const attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
}

void directories(fs::path const& path)
{
    if (!fs::exists(path)) {
        directories(path.parent_path());
        requireWindows(
            CreateDirectoryW(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS,
            L"Cannot create the game folder " + path.wstring()
        );
    }
    require(plain(path) && fs::is_directory(path), L"The game folder is redirected or unavailable: " + path.wstring());
}

Handle cacheLock(fs::path const& path)
{
    for (uint32_t attempt = 0; attempt < 300U; ++attempt) {
        Handle lock{CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (lock.valid()) {
            return lock;
        }
        requireWindows(GetLastError() == ERROR_SHARING_VIOLATION, L"Cannot open the game cache lock");
        Sleep(100U);
    }
    throw Failure{L"Another game launch is still unpacking. Wait a moment and try again."};
}

bool owned(fs::path const& path)
{
    if (!plain(path) || !plain(path / L".owner") || fs::file_size(path / L".owner") != sizeof(OWNER) - 1U) {
        return false;
    }
    std::ifstream stream{path / L".owner", std::ios::binary};
    std::string const marker{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    return marker == OWNER;
}

bool removeUnused(fs::path const& path)
{
    if (!owned(path)) {
        return false;
    }
    fs::path const lease_path = path / L".lease";
    DWORD const attributes = GetFileAttributesW(lease_path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) {
            return false;
        }
    } else {
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
            return false;
        }
        Handle lease{CreateFileW(lease_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (!lease.valid()) {
            return false;
        }
    }
    for (auto const& child : fs::recursive_directory_iterator(path)) {
        if (!plain(child.path())) {
            return false;
        }
    }
    std::error_code error;
    fs::remove_all(path, error);
    return !error;
}

Digest fileDigest(fs::path const& path)
{
    Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    requireWindows(file.valid(), L"Cannot verify " + path.wstring());
    Hash hash;
    std::array<uint8_t, 65'536> buffer{};
    DWORD read = 0;
    do {
        requireWindows(
            ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) != FALSE,
            L"Cannot read " + path.wstring()
        );
        hash.add(std::span<uint8_t const>{buffer}.first(read));
    } while (read != 0U);
    return hash.finish();
}

bool validCache(fs::path const& path, std::vector<Entry> const& files)
{
    if (!owned(path) || !plain(path / L".lease")) {
        return false;
    }
    for (auto const& entry : files) {
        fs::path target = path;
        for (auto const& component : entry.path) {
            target /= component;
            if (!plain(target)) {
                return false;
            }
        }
        if (!fs::is_regular_file(target) || fs::file_size(target) != entry.data.size()
            || fileDigest(target) != entry.hash) {
            return false;
        }
    }
    return true;
}

void writeFile(fs::path const& path, std::span<uint8_t const> bytes)
{
    directories(path.parent_path());
    Handle file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    requireWindows(file.valid(), L"Cannot unpack " + path.wstring() + L". Check free disk space");
    while (!bytes.empty()) {
        DWORD written = 0;
        DWORD const count = static_cast<DWORD>(std::min<size_t>(bytes.size(), 65'536U));
        requireWindows(
            WriteFile(file.get(), bytes.data(), count, &written, nullptr) != FALSE && written == count,
            L"Cannot write game files. Free disk space and try again"
        );
        bytes = bytes.subspan(written);
    }
    requireWindows(FlushFileBuffers(file.get()) != FALSE, L"Cannot finish writing game files");
}

fs::path prepareCache(fs::path const& root, std::wstring const& key, std::vector<Entry> const& files)
{
    fs::path const cache = root / key;
    if (validCache(cache, files)) {
        return cache;
    }
    if (fs::exists(cache)) {
        require(removeUnused(cache), L"Close other MinecraftClone windows, then try again to repair the game cache.");
    }
    fs::path const temporary = root / (key + L".stage-" + std::to_wstring(GetCurrentProcessId()));
    if (fs::exists(temporary)) {
        require(removeUnused(temporary), L"The temporary game folder is unavailable: " + temporary.wstring());
    }
    directories(temporary);
    writeFile(temporary / L".owner", {reinterpret_cast<uint8_t const*>(OWNER), sizeof(OWNER) - 1U});
    writeFile(temporary / L".lease", {});
    try {
        for (auto const& entry : files) {
            writeFile(temporary / entry.path, entry.data);
        }
        require(validCache(temporary, files), L"Unpacked files did not pass verification. Check your disk and try again.");
        requireWindows(
            MoveFileExW(temporary.c_str(), cache.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE,
            L"Cannot finish preparing the local game folder"
        );
    } catch (...) {
        removeUnused(temporary);
        throw;
    }
    return cache;
}

DWORD launch(fs::path const& root, fs::path const& cache, Handle root_lock)
{
    directories(root / L"logs");
    fs::path const log = root / L"logs/latest.log";
    MoveFileExW(log.c_str(), (root / L"logs/previous.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
        &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
    requireWindows(output.valid(), L"Cannot open the game log " + log.wstring());
    root_lock.close();
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    requireWindows(input.valid(), L"Cannot initialize the game input stream");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdOutput = output.get();
    startup.StartupInfo.hStdError = output.get();
    startup.StartupInfo.hStdInput = input.get();
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attributes(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    requireWindows(
        InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) != FALSE,
        L"Cannot initialize the game process"
    );
    std::array<HANDLE, 2> inherited{output.get(), input.get()};
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited.data(), sizeof(inherited), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        throw Failure{windowsError(L"Cannot prepare the game log stream")};
    }
    fs::path const executable = cache / L"mc_main.exe";
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    PROCESS_INFORMATION process{};
    BOOL const started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, cache.c_str(), &startup.StartupInfo, &process);
    DWORD const start_error = GetLastError();
    DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (!started) {
        SetLastError(start_error);
        throw Failure{windowsError(L"Cannot start the game. Download this executable again")};
    }
    Handle child{process.hProcess};
    Handle thread{process.hThread};
    requireWindows(WaitForSingleObject(child.get(), INFINITE) == WAIT_OBJECT_0, L"Cannot observe the game process");
    DWORD exit = 1;
    requireWindows(GetExitCodeProcess(child.get(), &exit) != FALSE, L"Cannot read the game exit status");
    if (exit != 0U) {
        throw Failure{L"The game stopped unexpectedly (exit code " + std::to_wstring(exit)
            + L"). Try starting it again. If it repeats, report this message and the log at:\n\n" + log.wstring()};
    }
    return exit;
}

int run(HINSTANCE const instance)
{
    HRSRC const resource = FindResourceW(instance, MAKEINTRESOURCEW(101), RT_RCDATA);
    require(resource != nullptr, L"This executable has no embedded game. Download the complete game executable.");
    HGLOBAL const loaded = LoadResource(instance, resource);
    auto const* data = static_cast<uint8_t const*>(LockResource(loaded));
    DWORD const size = SizeofResource(instance, resource);
    require(data != nullptr && size > 0U, L"The embedded game could not be read. Download the executable again.");
    std::span<uint8_t const> const bundle{data, size};
    auto const files = entries(bundle);
    std::wstring const key = hexadecimal(digest(bundle));
    PWSTR local = nullptr;
    require(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &local) == S_OK,
        L"Windows could not locate your local application folder.");
    fs::path const root = fs::path{local} / L"MinecraftClone/SinglePlayer";
    CoTaskMemFree(local);
    directories(root);
    auto lock = cacheLock(root / L"cache.lock");
    fs::path const cache = prepareCache(root, key, files);
    // Lease under the root lock so another version cannot prune this launch's files.
    Handle active{CreateFileW((cache / L".lease").c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
    require(active.valid(), L"Another launch is repairing the game cache. Please try again.");
    for (auto const& item : fs::directory_iterator(root)) {
        if (item.path() != cache && owned(item.path()) && item.is_directory()) {
            removeUnused(item.path());
        }
    }
    return static_cast<int>(launch(root, cache, std::move(lock)));
}

} // namespace

int WINAPI wWinMain(HINSTANCE const instance, HINSTANCE, PWSTR, int)
{
    try {
        return run(instance);
    } catch (Failure const& failure) {
        MessageBoxW(nullptr, failure.message.c_str(), L"MinecraftClone could not start", MB_OK | MB_ICONERROR);
    } catch (std::exception const& error) {
        std::wstring message = L"The local game files are unavailable. Check free disk space and try again.\n\n";
        try {
            message += utf16(error.what());
        } catch (...) {
            message += L"Windows could not access the local game folder.";
        }
        MessageBoxW(nullptr, message.c_str(), L"MinecraftClone could not start", MB_OK | MB_ICONERROR);
    }
    return 1;
}
