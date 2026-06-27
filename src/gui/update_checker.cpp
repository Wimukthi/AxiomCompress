#define NOMINMAX
#include "gui/update_checker.hpp"

#include "gui/dialog_support.hpp"

#include <bcrypt.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <winhttp.h>
#include <winver.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace axiom::gui {
namespace {

constexpr wchar_t kRegistryPath[] = L"Software\\AxiomCompress\\GUI";
constexpr wchar_t kUpdateUrlSetting[] = L"UpdateUrl";
constexpr wchar_t kUpdateChannelSetting[] = L"UpdateChannel";
constexpr wchar_t kAutoUpdateSetting[] = L"CheckForUpdates";
constexpr wchar_t kLastUpdateCheckSetting[] = L"LastUpdateCheckUtc";
constexpr DWORD kUpdateCheckIntervalSeconds = 24u * 60u * 60u;
constexpr wchar_t kVersionFallback[] = L"0.1.0.0";
constexpr wchar_t kDefaultUpdateUrl[] =
    L"https://api.github.com/repos/Wimukthi/AxiomCompress/releases/latest";
constexpr std::size_t kMaximumDownloadBytes = 512u * 1024u * 1024u;

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle) : handle_(handle) {}
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    WinHttpHandle(WinHttpHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    ~WinHttpHandle() {
        if (handle_ != nullptr) {
            WinHttpCloseHandle(handle_);
        }
    }
    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_{};
};

class BcryptAlgorithm {
public:
    ~BcryptAlgorithm() {
        if (handle_ != nullptr) BCryptCloseAlgorithmProvider(handle_, 0);
    }
    BCRYPT_ALG_HANDLE* put() { return &handle_; }
    BCRYPT_ALG_HANDLE get() const { return handle_; }

private:
    BCRYPT_ALG_HANDLE handle_{};
};

class BcryptHash {
public:
    ~BcryptHash() {
        if (handle_ != nullptr) BCryptDestroyHash(handle_);
    }
    BCRYPT_HASH_HANDLE* put() { return &handle_; }
    BCRYPT_HASH_HANDLE get() const { return handle_; }

private:
    BCRYPT_HASH_HANDLE handle_{};
};

std::optional<DWORD> read_setting_dword(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD,
                                        nullptr, &value, &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS ? std::optional<DWORD>(value) : std::nullopt;
}

std::optional<std::wstring> read_setting_string(const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr, &size) !=
            ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    const LSTATUS status = RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ,
                                        nullptr, value.data(), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) return std::nullopt;
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void write_setting_dword(const wchar_t* name, DWORD value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryPath, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

DWORD unix_seconds_now() {
    FILETIME file_time{};
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value{};
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    constexpr unsigned long long epoch = 116444736000000000ull;
    return static_cast<DWORD>(std::min<unsigned long long>(
        (value.QuadPart - epoch) / 10000000ull, MAXDWORD));
}

std::wstring trim(std::wstring value) {
    const std::size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const std::size_t last = value.find_last_not_of(L" \t\r\n");
    value = value.substr(first, last - first + 1);
    if (value.size() >= 2 &&
        ((value.front() == L'"' && value.back() == L'"') ||
         (value.front() == L'\'' && value.back() == L'\''))) {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

std::wstring update_feed_url() {
    if (auto configured = read_setting_string(kUpdateUrlSetting)) {
        std::wstring url = trim(*configured);
        if (!url.empty()) {
            const DWORD channel = read_setting_dword(kUpdateChannelSetting).value_or(0);
            const std::wstring channel_name = channel == 1 ? L"preview" : L"stable";
            constexpr std::wstring_view token = L"{channel}";
            for (std::size_t pos = url.find(token); pos != std::wstring::npos;
                 pos = url.find(token, pos + channel_name.size())) {
                url.replace(pos, token.size(), channel_name);
            }
            return url;
        }
    }
    return kDefaultUpdateUrl;
}

std::wstring module_path(HINSTANCE instance) {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(instance, path.data(),
                                                 static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring format_version(DWORD high, DWORD low) {
    return std::to_wstring(HIWORD(high)) + L"." + std::to_wstring(LOWORD(high)) +
           L"." + std::to_wstring(HIWORD(low)) + L"." + std::to_wstring(LOWORD(low));
}

std::optional<std::array<int, 4>> parse_version(std::wstring_view text) {
    if (!text.empty() && (text.front() == L'v' || text.front() == L'V')) {
        text.remove_prefix(1);
    }
    std::array<int, 4> parts{};
    std::size_t index = 0;
    std::size_t start = 0;
    while (start < text.size() && index < parts.size()) {
        const std::size_t end = text.find(L'.', start);
        const std::wstring_view part = text.substr(
            start, end == std::wstring_view::npos ? text.size() - start : end - start);
        if (part.empty()) return std::nullopt;
        int value = 0;
        for (wchar_t ch : part) {
            if (ch < L'0' || ch > L'9') return std::nullopt;
            value = value * 10 + static_cast<int>(ch - L'0');
            if (value > 65535) return std::nullopt;
        }
        parts[index++] = value;
        if (end == std::wstring_view::npos) {
            start = text.size();
        } else {
            start = end + 1;
        }
    }
    return index > 0 && start >= text.size() ? std::optional(parts) : std::nullopt;
}

int compare_versions(std::wstring_view left, std::wstring_view right) {
    const auto left_parts = parse_version(left);
    const auto right_parts = parse_version(right);
    if (!left_parts || !right_parts) return 0;
    for (std::size_t i = 0; i < left_parts->size(); ++i) {
        if ((*left_parts)[i] < (*right_parts)[i]) return -1;
        if ((*left_parts)[i] > (*right_parts)[i]) return 1;
    }
    return 0;
}

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string wide_to_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        result.data(), count, nullptr, nullptr);
    return result;
}

std::optional<std::string> parse_json_string(std::string_view text, std::size_t quote) {
    if (quote >= text.size() || text[quote] != '"') return std::nullopt;
    std::string value;
    for (std::size_t i = quote + 1; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '"') return value;
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (++i >= text.size()) return std::nullopt;
        switch (text[i]) {
            case '"': case '\\': case '/': value.push_back(text[i]); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            case 'u':
                if (i + 4 >= text.size()) return std::nullopt;
                value.push_back('?');
                i += 4;
                break;
            default: return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::string> json_string_property(std::string_view text,
                                                 std::string_view name) {
    const std::string property = "\"" + std::string(name) + "\"";
    const std::size_t position = text.find(property);
    if (position == std::string_view::npos) return std::nullopt;
    const std::size_t colon = text.find(':', position + property.size());
    const std::size_t quote = colon == std::string_view::npos
        ? std::string_view::npos
        : text.find('"', colon + 1);
    return quote == std::string_view::npos ? std::nullopt : parse_json_string(text, quote);
}

std::optional<std::size_t> matching_token(std::string_view text, std::size_t start,
                                           char open, char close) {
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char ch = text[i];
        if (in_string) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') in_string = false;
            continue;
        }
        if (ch == '"') in_string = true;
        else if (ch == open) ++depth;
        else if (ch == close && --depth == 0) return i;
    }
    return std::nullopt;
}

std::optional<std::string_view> json_array_property(std::string_view text,
                                                     std::string_view name) {
    const std::string property = "\"" + std::string(name) + "\"";
    const std::size_t position = text.find(property);
    if (position == std::string_view::npos) return std::nullopt;
    const std::size_t open = text.find('[', position + property.size());
    if (open == std::string_view::npos) return std::nullopt;
    const auto close = matching_token(text, open, '[', ']');
    return close ? std::optional(text.substr(open + 1, *close - open - 1)) : std::nullopt;
}

std::vector<std::string_view> json_objects(std::string_view array) {
    std::vector<std::string_view> objects;
    std::size_t position = 0;
    while (position < array.size()) {
        const std::size_t open = array.find('{', position);
        if (open == std::string_view::npos) break;
        const auto close = matching_token(array, open, '{', '}');
        if (!close) break;
        objects.push_back(array.substr(open, *close - open + 1));
        position = *close + 1;
    }
    return objects;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool installer_asset_name(std::string_view name) {
    const std::string lower = lower_ascii(std::string(name));
    return (lower.starts_with("axiomcompresssetup-") || lower.starts_with("axiomsetup-")) &&
           lower.ends_with("-win-x64.exe");
}

std::optional<URL_COMPONENTS> crack_https_url(std::wstring_view url,
                                               std::wstring& host,
                                               std::wstring& path,
                                               std::wstring& error) {
    if (url.empty()) {
        error = L"The update feed URL is empty.";
        return std::nullopt;
    }
    if (url.size() > std::numeric_limits<DWORD>::max()) {
        error = L"The update URL is too long.";
        return std::nullopt;
    }
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0, &components)) {
        error = L"Could not parse update URL: " + last_error_text();
        return std::nullopt;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS) {
        error = L"The update URL must use HTTPS.";
        return std::nullopt;
    }
    host.assign(components.lpszHostName, components.dwHostNameLength);
    path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength > 0) {
        path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    if (path.empty()) path = L"/";
    return components;
}

std::optional<std::vector<BYTE>> http_get(std::wstring_view url, std::wstring& error) {
    std::wstring host;
    std::wstring path;
    const auto components = crack_https_url(url, host, path, error);
    if (!components) return std::nullopt;

    WinHttpHandle session(WinHttpOpen(
        L"Axiom Update Checker/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = L"Could not initialize WinHTTP: " + last_error_text();
        return std::nullopt;
    }
    WinHttpHandle connection(WinHttpConnect(session.get(), host.c_str(), components->nPort, 0));
    if (!connection) {
        error = L"Could not connect to update server: " + last_error_text();
        return std::nullopt;
    }
    WinHttpHandle request(WinHttpOpenRequest(
        connection.get(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!request) {
        error = L"Could not create update request: " + last_error_text();
        return std::nullopt;
    }
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    const wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    WinHttpAddRequestHeaders(request.get(), headers, static_cast<DWORD>(-1),
                             WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = L"Update request failed: " + last_error_text();
        return std::nullopt;
    }
    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                             WINHTTP_NO_HEADER_INDEX) ||
        status < 200 || status >= 300) {
        error = L"Update server returned HTTP status " + std::to_wstring(status) + L".";
        return std::nullopt;
    }
    std::vector<BYTE> bytes;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) {
            error = L"Could not read update response: " + last_error_text();
            return std::nullopt;
        }
        if (available == 0) break;
        if (bytes.size() + available > kMaximumDownloadBytes) {
            error = L"Update response exceeds the allowed download size.";
            return std::nullopt;
        }
        const std::size_t offset = bytes.size();
        bytes.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), bytes.data() + offset, available, &read)) {
            error = L"Could not read update response: " + last_error_text();
            return std::nullopt;
        }
        bytes.resize(offset + read);
    }
    return bytes;
}

std::optional<std::string> sha256_hex(const std::vector<BYTE>& bytes,
                                       std::wstring& error) {
    BcryptAlgorithm algorithm;
    if (BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        error = L"Could not initialize SHA-256.";
        return std::nullopt;
    }
    DWORD object_length = 0;
    DWORD hash_length = 0;
    DWORD result_length = 0;
    if (BCryptGetProperty(algorithm.get(), BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
                          &result_length, 0) < 0 ||
        BCryptGetProperty(algorithm.get(), BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hash_length), sizeof(hash_length),
                          &result_length, 0) < 0) {
        error = L"Could not query SHA-256 properties.";
        return std::nullopt;
    }
    std::vector<BYTE> object(object_length);
    BcryptHash hash;
    if (BCryptCreateHash(algorithm.get(), hash.put(), object.data(), object_length,
                         nullptr, 0, 0) < 0) {
        error = L"Could not create SHA-256 hash.";
        return std::nullopt;
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ULONG count = static_cast<ULONG>(std::min<std::size_t>(
            bytes.size() - offset, 1024u * 1024u));
        if (BCryptHashData(hash.get(), const_cast<PUCHAR>(bytes.data() + offset), count, 0) < 0) {
            error = L"Could not hash downloaded installer.";
            return std::nullopt;
        }
        offset += count;
    }
    std::vector<BYTE> digest(hash_length);
    if (BCryptFinishHash(hash.get(), digest.data(), hash_length, 0) < 0) {
        error = L"Could not finish SHA-256 hash.";
        return std::nullopt;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (BYTE value : digest) {
        result.push_back(hex[(value >> 4) & 0x0f]);
        result.push_back(hex[value & 0x0f]);
    }
    return result;
}

bool verify_digest(const std::vector<BYTE>& bytes, std::wstring_view expected,
                   std::wstring& error) {
    if (expected.empty()) return true;
    std::string normalized = lower_ascii(wide_to_utf8(expected));
    constexpr std::string_view prefix = "sha256:";
    if (!normalized.starts_with(prefix)) {
        error = L"Release asset digest uses an unsupported format.";
        return false;
    }
    normalized.erase(0, prefix.size());
    const auto actual = sha256_hex(bytes, error);
    if (!actual) return false;
    if (*actual != normalized) {
        error = L"Downloaded installer failed SHA-256 verification.";
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> update_directory(std::wstring& error) {
    PWSTR local_app_data = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                    nullptr, &local_app_data)) ||
        local_app_data == nullptr) {
        error = L"Could not locate the local application data folder.";
        return std::nullopt;
    }
    std::filesystem::path path(local_app_data);
    CoTaskMemFree(local_app_data);
    path /= L"AxiomCompress";
    path /= L"Updates";
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        error = L"Could not create the update download folder.";
        return std::nullopt;
    }
    return path;
}

bool write_download(const std::filesystem::path& path, const std::vector<BYTE>& bytes,
                    std::wstring& error) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = L"Could not create the downloaded installer file.";
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        error = L"Could not write the downloaded installer file.";
        return false;
    }
    return true;
}

std::optional<UpdateInfo> parse_release(std::string_view json, HINSTANCE instance,
                                        std::wstring& message) {
    const auto tag = json_string_property(json, "tag_name");
    if (!tag) {
        message = L"Latest release response did not include a tag name.";
        return std::nullopt;
    }
    const std::wstring latest = utf8_to_wide(*tag);
    if (compare_versions(latest, current_executable_version(instance)) <= 0) {
        return UpdateInfo{};
    }
    const auto assets = json_array_property(json, "assets");
    if (!assets) {
        message = L"Latest release response did not include installer assets.";
        return std::nullopt;
    }
    for (std::string_view asset : json_objects(*assets)) {
        const auto name = json_string_property(asset, "name");
        const auto url = json_string_property(asset, "browser_download_url");
        if (!name || !url || !installer_asset_name(*name)) continue;
        UpdateInfo update;
        update.version = latest;
        update.asset_name = utf8_to_wide(*name);
        update.download_url = utf8_to_wide(*url);
        if (const auto digest = json_string_property(asset, "digest")) {
            update.digest = utf8_to_wide(*digest);
        }
        if (const auto release_url = json_string_property(json, "html_url")) {
            update.release_url = utf8_to_wide(*release_url);
        }
        return update;
    }
    message = L"A newer release exists, but it does not include an Axiom x64 installer asset.";
    return std::nullopt;
}

template <typename Result>
void post_owned_result(HWND window, UINT message, std::unique_ptr<Result> result) {
    if (PostMessageW(window, message, 0, reinterpret_cast<LPARAM>(result.get()))) {
        result.release();
    }
}

}  // namespace

bool automatic_update_checks_enabled() {
    if (auto value = read_setting_dword(kAutoUpdateSetting)) return *value != 0;
    return false;
}

void set_automatic_update_checks_enabled(bool enabled) {
    write_setting_dword(kAutoUpdateSetting, enabled ? 1u : 0u);
}

bool automatic_update_check_due() {
    if (!automatic_update_checks_enabled() || update_feed_url().empty()) return false;
    const DWORD now = unix_seconds_now();
    if (auto last = read_setting_dword(kLastUpdateCheckSetting)) {
        return now < *last || now - *last >= kUpdateCheckIntervalSeconds;
    }
    return true;
}

std::wstring current_executable_version(HINSTANCE instance) {
    const std::wstring path = module_path(instance);
    if (path.empty()) return kVersionFallback;
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) return kVersionFallback;
    std::vector<BYTE> info(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, info.data())) return kVersionFallback;
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT fixed_size = 0;
    if (!VerQueryValueW(info.data(), L"\\", reinterpret_cast<LPVOID*>(&fixed),
                        &fixed_size) ||
        fixed == nullptr || fixed_size < sizeof(VS_FIXEDFILEINFO) ||
        fixed->dwSignature != 0xfeef04bd) {
        return kVersionFallback;
    }
    return format_version(fixed->dwFileVersionMS, fixed->dwFileVersionLS);
}

void start_update_check(HWND notify_window, HINSTANCE instance, UpdateCheckKind kind) {
    const std::wstring url = update_feed_url();
    write_setting_dword(kLastUpdateCheckSetting, unix_seconds_now());
    std::thread([notify_window, instance, kind, url] {
        auto result = std::make_unique<UpdateCheckResult>();
        result->kind = kind;
        std::wstring error;
        const auto bytes = http_get(url, error);
        if (!bytes) {
            result->message = std::move(error);
            post_owned_result(notify_window, kUpdateCheckCompleteMessage, std::move(result));
            return;
        }
        const std::string json(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        auto update = parse_release(json, instance, result->message);
        if (!update) {
            post_owned_result(notify_window, kUpdateCheckCompleteMessage, std::move(result));
            return;
        }
        result->success = true;
        if (!update->version.empty()) {
            result->update_available = true;
            result->update = std::move(*update);
        }
        post_owned_result(notify_window, kUpdateCheckCompleteMessage, std::move(result));
    }).detach();
}

void start_update_download(HWND notify_window, UpdateInfo update) {
    std::thread([notify_window, update = std::move(update)]() mutable {
        auto result = std::make_unique<UpdateDownloadResult>();
        result->update = std::move(update);
        std::wstring error;
        const auto bytes = http_get(result->update.download_url, error);
        if (!bytes) {
            result->message = std::move(error);
            post_owned_result(notify_window, kUpdateDownloadCompleteMessage, std::move(result));
            return;
        }
        if (!verify_digest(*bytes, result->update.digest, error)) {
            result->message = std::move(error);
            post_owned_result(notify_window, kUpdateDownloadCompleteMessage, std::move(result));
            return;
        }
        const auto directory = update_directory(error);
        if (!directory) {
            result->message = std::move(error);
            post_owned_result(notify_window, kUpdateDownloadCompleteMessage, std::move(result));
            return;
        }
        const std::filesystem::path installer = *directory / result->update.asset_name;
        if (!write_download(installer, *bytes, error)) {
            result->message = std::move(error);
            post_owned_result(notify_window, kUpdateDownloadCompleteMessage, std::move(result));
            return;
        }
        result->success = true;
        result->installer_path = installer.wstring();
        post_owned_result(notify_window, kUpdateDownloadCompleteMessage, std::move(result));
    }).detach();
}

}  // namespace axiom::gui
