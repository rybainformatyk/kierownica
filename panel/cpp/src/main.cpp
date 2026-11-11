// Panel kierownicy - punkt wejscia.
//
//   Kierownica.exe                       zwykle uruchomienie
//   Kierownica.exe --minimized           start zminimalizowany (autostart)
//   Kierownica.exe --config plik.json    inny plik ustawien
//   Kierownica.exe --selftest katalog    test bez okna i bez sprzetu
//   Kierownica.exe --po-aktualizacji     start po aktualizacji z panelu (czeka,
//                                        az stara wersja sie zamknie)
#include <windows.h>

#include <shellapi.h>

#include <exception>
#include <filesystem>
#include <string>

#include "config.h"
#include "ui/app.h"
#include "util.h"
#include "winsys.h"

using namespace wb;

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    ui::AppOptions options;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--minimized") options.minimized = true;
        else if (arg == L"--po-aktualizacji") options.after_update = true;
        else if (arg == L"--config" && i + 1 < argc) set_config_path_override(argv[++i]);
        else if (arg == L"--selftest" && i + 1 < argc) options.selftest_dir = argv[++i];
    }
    if (argv) LocalFree(argv);

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    std::wstring title = widen(ui::APP_TITLE);
    try {
        if (!options.selftest_dir.empty()) {
            if (options.selftest_dir.is_relative()) options.selftest_dir = std::filesystem::absolute(options.selftest_dir);
            set_config_path_override(options.selftest_dir / L"config.json");
            std::error_code ec;
            std::filesystem::create_directories(options.selftest_dir, ec);
            std::filesystem::remove(options.selftest_dir / L"config.json", ec);
            // Silnik i okno zyja do konca procesu - callback vJoy zostaje
            // zarejestrowany w sterowniku i nie ma jak go wyrejestrowac.
            auto* app = new ui::PanelApp(options);
            ExitProcess(static_cast<UINT>(app->selftest()));
        }

        // Druga kopia walczylaby z pierwsza o port COM i o urzadzenie vJoy -
        // zamiast niej na wierzch idzie okno, ktore juz dziala.
        // Po aktualizacji stara wersja moze sie jeszcze zamykac (zatrzymuje
        // silnik, zapisuje ustawienia) - czekamy na nia do 15 s.
        bool claimed = winsys::claim_single_instance();
        for (int i = 0; !claimed && options.after_update && i < 60; ++i) {
            Sleep(250);
            claimed = winsys::claim_single_instance();
        }
        if (!claimed) {
            if (!winsys::bring_to_front(title)) MessageBoxW(nullptr, L"Panel kierownicy już działa.", title.c_str(), MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        auto* app = new ui::PanelApp(options);
        int code = app->run();
        ExitProcess(static_cast<UINT>(code));
    } catch (const std::exception& exc) {
        MessageBoxW(nullptr, (L"Aplikacja zakończyła się błędem:\n\n" + widen(exc.what())).c_str(), L"Błąd aplikacji", MB_OK | MB_ICONERROR);
        return 1;
    }
}
