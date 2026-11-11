// Konfiguracja aplikacji - struktury + zapis/odczyt JSON.
//
// Kalibracja i mapowanie zyja TUTAJ, nie w firmware. Dzieki temu zmiana
// zakresu obrotu, martwej strefy czy mapowania przyciskow nie wymaga
// ponownego programowania plytki.
//
// Format pliku jest ten sam co w wersji Python.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "protocol.h"

namespace wb {

inline constexpr const char* APP_NAME = "KierownicaPanel";
// Plik obok Kierownica.exe: ustawienia zostaja w katalogu programu (wersja przenosna).
inline constexpr const char* PORTABLE_MARKER = "portable.txt";

using Names = std::vector<std::string>;
using LabelMap = std::map<std::string, std::string>;

// Nazwy osi wyjsciowych vJoy w kolejnosci uzywanej przez aplikacje.
inline constexpr int NUM_PEDALS = 6;
extern const std::array<std::string, NUM_PEDALS> PEDAL_KEYS;
extern const LabelMap PEDAL_LABELS;
int pedal_index(std::string_view key);  // -1 gdy nie ma

extern const LabelMap DEFAULT_VJOY_AXIS;
extern const Names VJOY_AXIS_CHOICES;

// Kontrolki wirtualnego pada, ktorym mozna przypisac przyciski plytki.
extern const Names PAD_BUTTON_NAMES;
extern const std::map<std::string, int> DEFAULT_PAD_BUTTONS;

// Kierunki wirtualnego pada. Kazda polowka osi jest osobnym gniazdem -
// dokladnie jak w x360ce. Dla osi Y "+" to gora (tak liczy XInput).
extern const Names PAD_DIRECTIONS;
extern const LabelMap PAD_DIRECTION_LABELS;
extern const LabelMap PAD_DIRECTION_SHORT;

// Zrodla analogowe, ktore mozna wpiac w kierunek pada. Kazde daje 0..1.
extern const Names PAD_SOURCE_KEYS;
extern const LabelMap PAD_SOURCE_LABELS;
extern const LabelMap DEFAULT_PAD_AXES;

extern const Names PAD_PROFILE_ORDER;
extern const LabelMap PAD_PROFILE_LABELS;

extern const Names OUTPUT_MODES;
extern const LabelMap OUTPUT_MODE_LABELS;

// Przyciski G29 w kolejnosci z raportu: pozycja + 1 = numer przycisku w grze.
extern const Names G29_BUTTON_NAMES;
extern const Names G29_DPAD;
extern const Names G29_CONTROLS;
extern const LabelMap G29_CONTROL_LABELS;
extern const std::vector<std::pair<std::string, std::string>> PAD_TO_G29;

extern const Names DIRECTION_MODES;

using ButtonMap = std::map<std::string, int>;
ButtonMap g29_buttons_from_pad(const ButtonMap& pad_buttons);
bool contains(const Names& list, std::string_view value);

// Jeden kanal analogowy: surowy ADC 0..4095 -> znormalizowane 0..1.
struct PedalConfig {
    int source = 0;               // indeks kanalu ADC (0 = A0 ... 5 = A5), -1 = wylaczony
    int raw_min = 0;
    int raw_max = 4095;
    bool invert = false;
    double deadzone_low = 0.02;   // 0..0.45 - ucina szum na poczatku skoku
    double deadzone_high = 0.02;  // 0..0.45 - gwarantuje osiagniecie 100 %
    double gamma = 1.0;           // < 1 agresywniej na poczatku, > 1 lagodniej
    double smoothing = 0.0;       // 0..0.95 filtr EMA
    std::string vjoy_axis = "Y";
    bool operator==(const PedalConfig&) const = default;
};

struct SteerConfig {
    int counts_per_rev = 2400;    // impulsy enkodera x4 na obrot kierownicy
    int range_deg = 900;          // pelny zakres obrotu (lock-to-lock)
    bool invert = false;
    double smoothing = 0.0;
    double linearity = 1.0;       // 1.0 = liniowo
    std::string vjoy_axis = "X";
    int64_t center_counts = 0;    // offset srodka ustawiany w aplikacji
    bool soft_endstop = true;     // miekki ogranicznik w firmware (wymaga FFB)
    int endstop_gain = 8;
    bool operator==(const SteerConfig&) const = default;
};

struct FfbConfig {
    bool enabled = false;
    double overall_gain = 0.70;   // 0..1 - globalne wzmocnienie
    double min_force = 0.00;      // 0..0.5 - pokonanie tarcia mostka/silnika
    double max_force = 1.00;      // 0..1 - ograniczenie pradu
    bool invert = false;
    double deadband = 0.01;       // 0..0.2 - ponizej tego nie ruszamy silnika
    int slew_per_ms = 4000;       // max zmiana momentu na ms (ochrona mechaniki)
    int timeout_ms = 200;         // watchdog - brak komendy = zwolnienie silnika
    int min_duty = 0;             // 0..255 PWM
    int max_duty = 255;           // 0..255 PWM
    int update_hz = 400;
    // Efekty generowane lokalnie (dzialaja takze w grach bez FFB)
    double spring_gain = 0.0;     // samocentrowanie
    double damper_gain = 0.0;     // tlumienie predkosci obrotu
    double friction_gain = 0.0;
    // Skalowanie efektow przychodzacych z gry
    double game_constant_gain = 1.0;
    double game_spring_gain = 1.0;
    double game_damper_gain = 1.0;
    double game_periodic_gain = 1.0;
    // Jak czytac kierunek efektow z gry - patrz DIRECTION_MODES.
    std::string direction_mode = "auto";
    bool operator==(const FfbConfig&) const = default;
};

struct ButtonsConfig {
    ButtonsConfig();
    // 32 pozycje: indeks = przycisk plytki, wartosc = numer przycisku vJoy
    // (1-based) lub 0 = nieuzywany.
    std::vector<int> mapping;
    // Przyciski plytki (0-based) uzyte jako krzyzak: gora, prawo, dol, lewo.
    // -1 = nieuzywany. Przypisany przycisk NIE jest juz wysylany jako przycisk.
    std::vector<int> hat;
    // Odwrocenie logiki przycisku (styk normalnie zwarty / NC).
    std::vector<bool> invert;
    bool operator==(const ButtonsConfig&) const = default;
};

struct PadConfig {
    PadConfig();
    // Ile stopni obrotu kierownicy odpowiada pelnemu wychyleniu drazka.
    int steer_range_deg = 360;
    double steer_deadzone = 0.0;
    // Kompensacja martwej strefy GRY ("anty-martwa strefa").
    double steer_antideadzone = 0.0;
    double trigger_antideadzone = 0.0;
    double steer_linearity = 1.0;  // < 1 czulszy srodek, > 1 lagodniejszy srodek
    std::string profile;           // profil gry, z ktorego pochodza wartosci ("" = wlasne)
    bool steer_invert = false;
    LabelMap axes;                 // kierunek pada -> klucz zrodla, "" = nieuzywany
    ButtonMap buttons;             // nazwa kontrolki pada -> przycisk plytki, -1 = brak

    bool apply_profile(std::string_view key);
    bool matches_profile(std::string_view key) const;
    std::string detect_profile() const;
    std::string source_of(const std::string& direction) const;
    Names directions_of(const std::string& source) const;
    bool operator==(const PadConfig&) const = default;
};

struct LinkConfig {
    std::string port;             // "" = autodetekcja
    std::string board;            // plytka dla "Wgraj firmware", "" = wedlug HELLO
    int baudrate = protocol::DEFAULT_BAUD;
    bool auto_connect = true;
    int report_hz = 333;
    bool operator==(const LinkConfig&) const = default;
};

struct G29Config {
    G29Config();
    // Prawdziwe G29 raportuje pedaly odwrotnie: 255 = puszczony, 0 = wcisniety.
    bool pedals_inverted = true;
    ButtonMap buttons;            // kontrolka G29 -> przycisk plytki 0..31, -1 = brak
    bool operator==(const G29Config&) const = default;
};

struct OutputConfig {
    std::string mode = "g29";     // patrz OUTPUT_MODES
    int vjoy_device = 1;          // 1..16
    bool auto_acquire = true;
    // Gry widza tylko urzadzenie z wybranego trybu.
    bool hide_unused = true;

    bool wheel_on() const { return mode == "wheel" || mode == "both" || mode == "g29_vjoy"; }
    bool vjoy_buttons_only() const { return mode == "g29_vjoy"; }
    bool pad_on() const { return mode == "pad" || mode == "both"; }
    bool g29_on() const { return mode == "g29" || mode == "g29_vjoy"; }
    // Czy vJoy ma byc widoczne dla gier. nullopt = nie ruszamy (w podgladzie
    // przelaczenie na kalibracje nie powinno pytac o administratora).
    std::optional<bool> vjoy_wanted() const;
    bool g29_wanted() const { return g29_on() || !hide_unused; }
    bool operator==(const OutputConfig&) const = default;
};

// Okno panelu - zapamietywane przy zamknieciu, to nie sa ustawienia kierownicy.
struct UiConfig {
    std::string geometry;         // "SZERxWYS+X+Y" zwyklego okna, "" = na srodku
    bool zoomed = false;
    int tab = 0;
    bool tray = true;             // zamkniecie okna chowa panel do zasobnika
    bool updates = true;          // sprawdzanie aktualizacji przy starcie
    bool operator==(const UiConfig&) const = default;
};

std::filesystem::path config_path();
// Inny plik konfiguracji (opcja --config); pusty = domyslny (patrz config_path).
void set_config_path_override(const std::filesystem::path& path);

struct AppConfig {
    AppConfig();

    LinkConfig link;
    OutputConfig output;
    SteerConfig steer;
    std::array<PedalConfig, NUM_PEDALS> pedals;
    FfbConfig ffb;
    ButtonsConfig buttons;
    PadConfig pad;
    G29Config g29;
    UiConfig ui;

    PedalConfig& pedal(std::string_view key) { return pedals[static_cast<size_t>(pedal_index(key))]; }
    const PedalConfig& pedal(std::string_view key) const {
        return pedals[static_cast<size_t>(pedal_index(key))];
    }

    // Doprowadza wszystkie wartosci do dozwolonych zakresow - uszkodzony lub
    // recznie edytowany config nie moze wywrocic aplikacji.
    void normalize();

    std::string to_json() const;
    // Atomowy zapis. Blad: std::runtime_error.
    std::filesystem::path save(const std::optional<std::filesystem::path>& path = std::nullopt) const;

    // Brak pliku = domyslne. Pliku, ktorego nie da sie odczytac, nie wolno
    // po cichu zastapic domyslnymi - trafia do config.json.bad, a opis
    // problemu do last_load_problem.
    static AppConfig load(const std::optional<std::filesystem::path>& path = std::nullopt);

    bool operator==(const AppConfig&) const = default;
};

// Co poszlo nie tak przy ostatnim AppConfig::load() ("" = wszystko w porzadku).
extern std::string last_load_problem;

} // namespace wb
