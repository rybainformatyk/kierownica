#include "updater.h"

#include <windows.h>

#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <functional>
#include <memory>

#include "json.h"
#include "util.h"
#include "version.h"

namespace fs = std::filesystem;

namespace wb::update {

namespace {

constexpr DWORD TIMEOUT_MS = 15000;

struct HandleCloser {
    void operator()(HINTERNET h) const {
        if (h) WinHttpCloseHandle(h);
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

// Zapytanie GET; kazdy kawalek odpowiedzi idzie do `sink` (false = przerwij).
// `total` dostaje Content-Length albo 0. Przekierowania (github.com ->
// serwer plikow) WinHTTP robi sam, tylko HTTPS -> HTTPS.
bool http_get(const std::string& url, const std::function<bool(const char*, size_t)>& sink, uint64_t& total,
              std::string& error, bool api = false) {
    total = 0;
    std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "nieprawidłowy adres: " + url;
        return false;
    }

    std::wstring agent = widen(std::string("KierownicaPanel/") + APP_VERSION_TEXT);
    Handle session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        error = "WinHTTP: " + win_error_text(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.get(), TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS, TIMEOUT_MS);
    Handle connect(WinHttpConnect(session.get(), host, parts.nPort, 0));
    Handle request(connect ? WinHttpOpenRequest(connect.get(), L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr);
    if (!request) {
        error = "brak połączenia: " + win_error_text(GetLastError());
        return false;
    }
    const wchar_t* headers = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28" : WINHTTP_NO_ADDITIONAL_HEADERS;
    if (!WinHttpSendRequest(request.get(), headers, api ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        error = "brak połączenia z GitHubem: " + win_error_text(GetLastError());
        return false;
    }
    DWORD status = 0, size = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) {
        error = std::format("HTTP {}", status);
        return false;
    }
    wchar_t length[32] = {};
    DWORD length_size = sizeof(length);
    if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, length, &length_size, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(length, nullptr, 10);

    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.get(), buf.data(), static_cast<DWORD>(buf.size()), &got)) {
            error = "przerwane pobieranie: " + win_error_text(GetLastError());
            return false;
        }
        if (got == 0) return true;
        if (!sink(buf.data(), got)) {
            error = "przerwano";
            return false;
        }
    }
}

bool http_text(const std::string& url, std::string& out, std::string& error, bool api = false) {
    out.clear();
    uint64_t total = 0;
    return http_get(url, [&](const char* d, size_t n) {
        out.append(d, n);
        return out.size() < 4 * 1024 * 1024;  // odpowiedz API to kilka kB
    }, total, error, api);
}

std::string str(const json::Value* v) { return (v && v->is_string()) ? v->as_string() : std::string(); }

} // namespace

const Asset* Release::asset(std::string_view name) const {
    for (const Asset& a : assets)
        if (a.name == name) return &a;
    return nullptr;
}

const Asset* Release::installer() const {
    for (const Asset& a : assets)
        if (a.name.starts_with("Kierownica-") && a.name.ends_with("-instalator.exe")) return &a;
    return nullptr;
}

int compare_versions(std::string_view a, std::string_view b) {
    auto numbers = [](std::string_view s) {
        std::vector<long> out;
        size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
        while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) {
            long n = 0;
            while (i < s.size() && isdigit(static_cast<unsigned char>(s[i])) && n < 100000) n = n * 10 + (s[i++] - '0');
            out.push_back(n);
            if (i < s.size() && s[i] == '.') ++i;
            else break;  // "-beta" i reszta nie licza sie
        }
        return out;
    };
    std::vector<long> x = numbers(a), y = numbers(b);
    size_t n = std::max(x.size(), y.size());
    x.resize(n, 0);
    y.resize(n, 0);
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

std::optional<Release> parse_release(std::string_view json_text, std::string& error) {
    json::Value root;
    try {
        root = json::parse(json_text);
    } catch (const std::exception& exc) {
        error = std::string("zła odpowiedź GitHuba: ") + exc.what();
        return std::nullopt;
    }
    if (!root.is_object()) {
        error = "zła odpowiedź GitHuba";
        return std::nullopt;
    }
    Release r;
    r.tag = str(root.find("tag_name"));
    r.version = (!r.tag.empty() && (r.tag[0] == 'v' || r.tag[0] == 'V')) ? r.tag.substr(1) : r.tag;
    r.title = str(root.find("name"));
    r.notes = str(root.find("body"));
    r.page = str(root.find("html_url"));
    if (r.version.empty() || compare_versions(r.version, "0") == 0) {
        error = "wydanie bez numeru wersji (" + r.tag + ")";
        return std::nullopt;
    }
    if (const json::Value* list = root.find("assets"); list && list->is_array()) {
        for (const json::Value& a : list->items()) {
            Asset asset;
            asset.name = str(a.find("name"));
            asset.url = str(a.find("browser_download_url"));
            if (const json::Value* s = a.find("size")) asset.size = static_cast<uint64_t>(std::max<int64_t>(0, s->to_int(0)));
            if (!asset.name.empty() && !asset.url.empty()) r.assets.push_back(std::move(asset));
        }
    }
    return r;
}

std::map<std::string, std::string> parse_sums(std::string_view text) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        std::string_view line = text.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        pos = end == std::string_view::npos ? text.size() : end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        size_t sp = line.find(' ');
        if (sp != 64) continue;  // SHA-256 = 64 znaki szesnastkowe
        std::string hash = lower(std::string(line.substr(0, 64)));
        if (!std::all_of(hash.begin(), hash.end(), [](char c) { return isxdigit(static_cast<unsigned char>(c)); })) continue;
        std::string_view name = line.substr(sp);
        while (!name.empty() && (name.front() == ' ' || name.front() == '*')) name.remove_prefix(1);
        if (!name.empty()) out[std::string(name)] = hash;
    }
    return out;
}

bool trusted_url(std::string_view url) {
    return url.starts_with(std::string("https://github.com/") + REPO + "/releases/download/");
}

std::string sha256_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    std::string result;
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<char> buf(256 * 1024);
        bool ok = true;
        while (ok && in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize n = in.gcount();
            if (n > 0) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0) >= 0;
        }
        UCHAR digest[32];
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0)
            for (UCHAR b : digest) result += std::format("{:02x}", b);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return result;
}

bool download_file(const std::string& url, const fs::path& path, const std::function<bool(uint64_t, uint64_t)>& progress,
                   std::string& error) {
    std::error_code ec;
    fs::path part = path;
    part += L".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "nie da się zapisać " + narrow(part.wstring());
        return false;
    }
    uint64_t total = 0, done = 0;
    bool ok = http_get(url, [&](const char* d, size_t n) {
        out.write(d, static_cast<std::streamsize>(n));
        done += n;
        return static_cast<bool>(out) && (!progress || progress(done, total));
    }, total, error);
    out.close();
    if (ok && !out) {
        ok = false;
        error = "błąd zapisu na dysk";
    }
    if (ok && total && done != total) {
        ok = false;
        error = "niepełny plik";
    }
    if (ok) {
        fs::rename(part, path, ec);
        if (ec) {
            ok = false;
            error = ec.message();
        }
    }
    if (!ok) fs::remove(part, ec);
    return ok;
}

InstallKind install_kind(const fs::path& exe_dir) {
    std::error_code ec;
    return fs::exists(exe_dir / L"unins000.exe", ec) ? InstallKind::Installer : InstallKind::Portable;
}

fs::path download_dir() {
    wchar_t* base = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) out = fs::path(base) / L"Kierownica" / L"aktualizacje";
    CoTaskMemFree(base);
    if (out.empty()) out = fs::temp_directory_path() / L"Kierownica-aktualizacje";
    return out;
}

// ------------------------------------------------------------------ Updater

Updater::Updater(std::string current_version, InstallKind kind) : current_(std::move(current_version)), kind_(kind) {}

Updater::~Updater() {
    cancel_.store(true);
    if (worker_.joinable()) worker_.join();
}

State Updater::state() const {
    std::lock_guard lock(mutex_);
    return state_;
}

void Updater::set(const State& s) {
    std::lock_guard lock(mutex_);
    uint64_t serial = state_.serial + 1;
    state_ = s;
    state_.serial = serial;
}

bool Updater::start(const State& initial, std::function<void()> job) {
    {
        std::lock_guard lock(mutex_);
        if (busy_) return false;  // poprzednie sprawdzanie / pobieranie jeszcze trwa
        busy_ = true;
        uint64_t serial = state_.serial + 1;
        state_ = initial;
        state_.serial = serial;
    }
    if (worker_.joinable()) worker_.join();  // poprzedni watek juz skonczyl
    worker_ = std::thread([this, job = std::move(job)] {
        job();
        std::lock_guard lock(mutex_);
        busy_ = false;
    });
    return true;
}

void Updater::check(bool manual) {
    {
        std::lock_guard lock(mutex_);
        if (state_.phase == State::Phase::Ready) return;  // gotowe do instalacji - nic nowego nie trzeba
    }
    State s = state();
    s.phase = State::Phase::Checking;
    s.manual = manual;
    s.message = "Sprawdzanie…";
    start(s, [this, manual] { run_check(manual); });
}

void Updater::run_check(bool manual) {
    State s;
    s.manual = manual;
    std::string body, error;
    std::string url = std::string("https://api.github.com/repos/") + REPO + "/releases/latest";
    if (!http_text(url, body, error, true)) {
        s.phase = State::Phase::Failed;
        if (error == "HTTP 404") s.message = "Na GitHubie nie ma jeszcze żadnego wydania";
        else if (error == "HTTP 403" || error == "HTTP 429") s.message = "GitHub chwilowo odrzuca zapytania - spróbuj za godzinę";
        else s.message = "Nie udało się sprawdzić aktualizacji (" + error + ")";
        set(s);
        return;
    }
    auto release = parse_release(body, error);
    if (!release) {
        s.phase = State::Phase::Failed;
        s.message = error;
        set(s);
        return;
    }
    s.release = release;
    if (compare_versions(release->version, current_) <= 0) {
        s.phase = State::Phase::UpToDate;
        s.message = "Masz najnowszą wersję (" + current_ + ")";
        set(s);
        return;
    }
    const Asset* file = kind_ == InstallKind::Installer ? release->installer() : release->asset(EXE_ASSET);
    const Asset* sums = release->asset(SUMS_ASSET);
    s.can_install = file && sums && trusted_url(file->url) && trusted_url(sums->url);
    s.phase = State::Phase::Available;
    s.message = "Dostępna wersja " + release->version;
    set(s);
}

void Updater::download() {
    State current = state();
    if (current.phase != State::Phase::Available || !current.release || !current.can_install) return;
    Release release = *current.release;
    current.phase = State::Phase::Downloading;
    current.progress = 0.0;
    current.message = "Pobieranie…";
    start(current, [this, release] { run_download(release); });
}

void Updater::run_download(Release release) {
    State s;
    s.release = release;
    s.can_install = true;
    s.manual = true;
    auto fail = [&](const std::string& msg) {
        s.phase = State::Phase::Available;  // mozna sprobowac jeszcze raz
        s.message = msg;
        set(s);
    };

    const Asset* file = kind_ == InstallKind::Installer ? release.installer() : release.asset(EXE_ASSET);
    const Asset* sums_asset = release.asset(SUMS_ASSET);
    if (!file || !sums_asset) return fail("Wydanie nie ma pliku dla tej instalacji");

    std::string sums_text, error;
    if (!http_text(sums_asset->url, sums_text, error)) return fail("Nie udało się pobrać sum kontrolnych (" + error + ")");
    auto sums = parse_sums(sums_text);
    auto expected = sums.find(file->name);
    if (expected == sums.end()) return fail("W SHA256SUMS.txt nie ma " + file->name);

    std::error_code ec;
    fs::path dir = download_dir() / widen(release.version);
    fs::create_directories(dir, ec);
    fs::path target = dir / widen(file->name);
    // pobrane wczesniej i poprawne - nie ma po co sciagac drugi raz
    if (!(fs::exists(target, ec) && sha256_file(target) == expected->second)) {
        int last_pct = -1;
        s.phase = State::Phase::Downloading;
        bool ok = download_file(file->url, target, [&](uint64_t done, uint64_t total) {
            if (cancel_.load()) return false;
            uint64_t size = total ? total : file->size;
            int pct = size ? static_cast<int>(std::min<uint64_t>(100, done * 100 / size)) : 0;
            if (pct != last_pct) {
                last_pct = pct;
                s.progress = static_cast<double>(pct) / 100.0;
                s.message = std::format("Pobieranie… {}%", pct);
                set(s);
            }
            return true;
        }, error);
        if (!ok) return fail("Pobieranie nie powiodło się (" + error + ")");
        if (sha256_file(target) != expected->second) {
            fs::remove(target, ec);
            return fail("Pobrany plik ma złą sumę SHA-256 - usunięty. Spróbuj ponownie.");
        }
    }
    s.phase = State::Phase::Ready;
    s.progress = 1.0;
    s.file = target;
    s.message = "Wersja " + release.version + " gotowa do instalacji";
    set(s);
}

} // namespace wb::update
