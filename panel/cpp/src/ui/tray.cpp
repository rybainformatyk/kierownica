#include "tray.h"

#include <cwchar>

namespace wb::ui {

namespace {

constexpr UINT ICON_ID = 1;

template <size_t N>
void copy_text(wchar_t (&dst)[N], const std::wstring& src) {
    wcsncpy_s(dst, src.c_str(), _TRUNCATE);  // dluzszy tekst zostaje uciety
}

} // namespace

UINT TrayIcon::taskbar_created() {
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}

NOTIFYICONDATAW TrayIcon::base() const {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = ICON_ID;
    return nid;
}

bool TrayIcon::add(HWND hwnd, HICON icon, const std::wstring& tip) {
    hwnd_ = hwnd;
    icon_ = icon;
    tip_ = tip;
    NOTIFYICONDATAW nid = base();
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = CALLBACK_MSG;
    nid.hIcon = icon;
    copy_text(nid.szTip, tip);
    added_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    if (added_) {
        // wersja 4: klawiatura, NIN_SELECT i WM_CONTEXTMENU ze wspolrzednymi
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    return added_;
}

void TrayIcon::update(HICON icon, const std::wstring& tip) {
    if (!added_ || (icon == icon_ && tip == tip_)) return;
    icon_ = icon;
    tip_ = tip;
    NOTIFYICONDATAW nid = base();
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.hIcon = icon;
    copy_text(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::balloon(const std::wstring& title, const std::wstring& text, DWORD info_flags) {
    if (!added_) return;
    NOTIFYICONDATAW nid = base();
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = info_flags | NIIF_RESPECT_QUIET_TIME;  // nie w trakcie gry pelnoekranowej
    copy_text(nid.szInfoTitle, title);
    copy_text(nid.szInfo, text);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::remove() {
    if (!added_) return;
    NOTIFYICONDATAW nid = base();
    Shell_NotifyIconW(NIM_DELETE, &nid);
    added_ = false;
}

bool TrayIcon::readd() {
    if (!hwnd_) return false;
    added_ = false;
    return add(hwnd_, icon_, tip_);
}

} // namespace wb::ui
