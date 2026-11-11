// Aktualizacje panelu z wydan na GitHubie (rybainformatyk/kierownica).
//
// Przeplyw:
//   1. check()    - pyta API GitHuba o najnowsze wydanie, porownuje wersje,
//   2. download() - pobiera instalator (wersja zainstalowana) albo sam
//                   Kierownica.exe (wersja przenosna) i sprawdza SHA-256 z pliku
//                   SHA256SUMS.txt tego samego wydania,
//   3. zainstalowanie robi PanelApp: uruchamia instalator w trybie cichym albo
//      podmienia exe obok siebie, i zamyka sie.
//
// Bez SHA256SUMS.txt w wydaniu panel nie instaluje niczego sam - pokazuje
// tylko strone wydania. Pobieranie wylacznie po HTTPS z github.com.
//
// Wszystko w tle, w osobnym watku; interfejs czyta stan przez state().
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace wb::update {

inline constexpr const char* REPO = "rybainformatyk/kierownica";
inline constexpr const char* RELEASES_PAGE = "https://github.com/rybainformatyk/kierownica/releases";
inline constexpr const char* SUMS_ASSET = "SHA256SUMS.txt";
inline constexpr const char* EXE_ASSET = "Kierownica.exe";
inline constexpr const char* OLD_EXE_SUFFIX = ".old";  // stary exe po podmianie w miejscu

struct Asset {
    std::string name;
    std::string url;
    uint64_t size = 0;
};

struct Release {
    std::string version;  // "3.3" (tag bez "v")
    std::string tag;      // "v3.3"
    std::string title;
    std::string notes;
    std::string page;
    std::vector<Asset> assets;

    const Asset* asset(std::string_view name) const;
    // Instalator: "Kierownica-X.Y-instalator.exe"
    const Asset* installer() const;
};

// Porownanie numerow wersji "3.2", "v3.10.1" - liczbowo, kolejne pola.
// <0: a starsza, 0: rowne, >0: a nowsza.
int compare_versions(std::string_view a, std::string_view b);

// Odpowiedz API GitHuba /releases/latest. nullopt + error, gdy nie pasuje.
std::optional<Release> parse_release(std::string_view json_text, std::string& error);

// "suma  nazwa" na linie (jak sha256sum). Klucz: nazwa, wartosc: suma malymi literami.
std::map<std::string, std::string> parse_sums(std::string_view text);

// Wolno pobierac tylko pliki z wydan naszego repozytorium.
bool trusted_url(std::string_view url);

std::string sha256_file(const std::filesystem::path& path);

// Pobiera plik (HTTPS, z przekierowaniami) do `path` przez plik .part.
// progress(pobrane, calosc) - false przerywa. calosc = 0, gdy serwer jej nie podal.
bool download_file(const std::string& url, const std::filesystem::path& path,
                   const std::function<bool(uint64_t, uint64_t)>& progress, std::string& error);

// Instalacja z instalatora (obok exe lezy unins000.exe) czy wersja przenosna.
enum class InstallKind { Installer, Portable };
InstallKind install_kind(const std::filesystem::path& exe_dir);

// Katalog na pobrane aktualizacje: %LOCALAPPDATA%\Kierownica\aktualizacje.
std::filesystem::path download_dir();

struct State {
    enum class Phase { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };
    Phase phase = Phase::Idle;
    bool manual = false;        // sprawdzanie na zadanie uzytkownika (pokaz wynik)
    std::string message;        // opis bledu albo stanu
    std::optional<Release> release;
    bool can_install = false;   // wydanie ma plik dla tej instalacji i SHA256SUMS.txt
    double progress = 0.0;      // 0..1 przy pobieraniu
    std::filesystem::path file; // gotowy, sprawdzony plik
    uint64_t serial = 0;        // rosnie przy kazdej zmianie - latwo wykryc nowy stan
};

class Updater {
public:
    Updater(std::string current_version, InstallKind kind);
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    void check(bool manual);
    void download();
    State state() const;
    InstallKind kind() const { return kind_; }

private:
    void run_check(bool manual);
    void run_download(Release release);
    void set(const State& s);
    // Ustawia stan poczatkowy i uruchamia zadanie; false, gdy cos juz trwa.
    bool start(const State& initial, std::function<void()> job);

    std::string current_;
    InstallKind kind_;
    mutable std::mutex mutex_;
    State state_;
    bool busy_ = false;  // pod mutex_
    std::thread worker_;
    std::atomic<bool> cancel_{false};
};

} // namespace wb::update
