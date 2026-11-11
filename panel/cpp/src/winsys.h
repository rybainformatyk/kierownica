// Drobne sprawy Windowsa dla okna panelu:
//   * jedna kopia panelu naraz (nazwany mutex + przywrocenie dzialajacego okna),
//   * uruchamianie razem z Windowsem (klucz Run w rejestrze uzytkownika),
//   * czy zapamietane polozenie okna lezy na ktoryms monitorze,
//   * czy okno panelu jest zasloniete (np. gra na pelnym ekranie),
//   * mruganie przyciskiem na pasku zadan.
//
// Przy bledzie API kazda funkcja zwraca bezpieczna wartosc domyslna.
#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>

namespace wb::winsys {

// Mutex wspolny z wersja Python - dwa panele walczylyby o port COM i vJoy.
inline constexpr const wchar_t* INSTANCE_MUTEX = L"Local\\KierownicaPanel";
inline constexpr const wchar_t* WINDOW_CLASS = L"KierownicaPanelWindow";
inline constexpr const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* RUN_VALUE = L"KierownicaPanel";

// Komunikat "pokaz sie" od drugiej kopii panelu - okno moze byc schowane
// w zasobniku, wiec samo ShowWindow nie wystarczy (panel musi o tym wiedziec).
UINT show_message();

// Pelna sciezka Kierownica.exe.
std::filesystem::path exe_path();

// True, gdy to jedyna dzialajaca kopia panelu.
bool claim_single_instance(const wchar_t* name = INSTANCE_MUTEX);

// Przywraca i wysuwa na wierzch okno panelu innego procesu (takze schowane
// w zasobniku).
bool bring_to_front(const std::wstring& title_prefix);

// Polecenie dla klucza Run: program + --minimized.
std::wstring autostart_command(const std::filesystem::path& exe);
// Sciezka programu z polecenia autostartu (pierwszy napis w cudzyslowie).
std::optional<std::wstring> autostart_target(const std::wstring& command);
std::optional<std::wstring> autostart_get(const wchar_t* key = RUN_KEY, const wchar_t* value = RUN_VALUE);
// Wpisuje polecenie do klucza Run; nullopt usuwa wpis. False przy bledzie.
bool autostart_set(const std::optional<std::wstring>& command, const wchar_t* key = RUN_KEY,
                   const wchar_t* value = RUN_VALUE);
// Czy z Windowsem startuje wlasnie ten program (a nie inna kopia panelu).
bool autostart_points_here();
// Wlacza (dla tego programu) albo usuwa autostart. False przy bledzie.
bool autostart_enable(bool on);

struct Geometry {
    int w, h, x, y;
};

// "1240x880+40+-10" -> geometria; nullopt dla czegos innego.
std::optional<Geometry> parse_geometry(const std::string& text);

// Obszar roboczy (l, g, p, d) monitora pod paskiem tytulu okna; nullopt, gdy
// pasek tytulu nie lezy porzadnie na zadnym monitorze.
std::optional<std::array<int, 4>> work_area_at(int x, int y, int w);

// Czy prostokat outer (l, g, p, d) zakrywa caly inner.
bool covers(const std::array<int, 4>& outer, const std::array<int, 4>& inner, int slack = 2);

// Czy okna panelu na pewno nie widac (inny pulpit wirtualny albo w calosci
// zasloniete przez okno innego programu na pierwszym planie).
bool window_hidden(HWND hwnd);

bool is_foreground(HWND hwnd);
void flash_taskbar(HWND hwnd);

} // namespace wb::winsys
