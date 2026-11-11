// Ikona panelu w zasobniku systemowym (obok zegara).
//
// Tylko obsluga Shell_NotifyIcon: dodanie, zmiana ikony i podpowiedzi, dymek
// z komunikatem i ponowne dodanie po restarcie Eksploratora. Menu i to, co
// robia klikniecia, sa w PanelApp.
#pragma once

#include <windows.h>

#include <shellapi.h>

#include <string>

namespace wb::ui {

class TrayIcon {
public:
    // Komunikat z ikony do okna: LOWORD(lParam) = zdarzenie (NIN_SELECT,
    // WM_CONTEXTMENU...), wParam = punkt na ekranie (GET_X/Y_LPARAM).
    static constexpr UINT CALLBACK_MSG = WM_APP + 20;

    ~TrayIcon() { remove(); }

    bool add(HWND hwnd, HICON icon, const std::wstring& tip);
    // Zmienia tylko to, co sie zmienilo - mozna wolac co klatke.
    void update(HICON icon, const std::wstring& tip);
    void balloon(const std::wstring& title, const std::wstring& text, DWORD info_flags = NIIF_INFO);
    void remove();
    // Po restarcie Eksploratora (komunikat taskbar_created) ikony trzeba dodac od nowa.
    bool readd();
    bool added() const { return added_; }

    // "TaskbarCreated" - Eksplorator wystartowal od nowa.
    static UINT taskbar_created();

private:
    NOTIFYICONDATAW base() const;

    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    std::wstring tip_;
    bool added_ = false;
};

} // namespace wb::ui
