#include "embedded.h"

#include <windows.h>

#include <shlobj.h>

#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "embedded_ids.h"

namespace fs = std::filesystem;

namespace wb::embedded {

namespace {

std::optional<std::string_view> resource(int id) {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC found = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    if (!found) return std::nullopt;
    HGLOBAL loaded = LoadResource(self, found);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data) return std::nullopt;
    return std::string_view(static_cast<const char*>(data), SizeofResource(self, found));
}

std::optional<fs::path> base_dir() {
    wchar_t* path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) return std::nullopt;
    fs::path out = fs::path(path) / L"Kierownica";
    CoTaskMemFree(path);
    return out;
}

bool write_if_changed(const fs::path& file, std::string_view content) {
    {
        std::ifstream in(file, std::ios::binary);
        if (in && std::string(std::istreambuf_iterator<char>(in), {}) == content) return true;
    }
    // przez plik tymczasowy - przerwany zapis nie zostawi polowy biblioteki
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.write(content.data(), static_cast<std::streamsize>(content.size())) || !out.flush()) return false;
    }
    return MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

struct Item {
    int id;
    const wchar_t* name;
};

bool extract(const fs::path& dir, std::initializer_list<Item> items) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const Item& item : items) {
        auto content = resource(item.id);
        if (!content || !write_if_changed(dir / item.name, *content)) return false;
    }
    return true;
}

} // namespace

std::optional<fs::path> vigem_dir() {
    // raz na uruchomienie: diagnostyka pyta o biblioteke wielokrotnie
    static const std::optional<fs::path> dir = []() -> std::optional<fs::path> {
        auto base = base_dir();
        if (!base || !resource(IDR_VIGEM_DLL)) return std::nullopt;
        fs::path out = *base / L"vigem" / L"x64";
        // licencja MIT wymaga, zeby tekst szedl razem z biblioteka
        if (!extract(out, {{IDR_VIGEM_DLL, L"ViGEmClient.dll"}, {IDR_VIGEM_LICENSE, L"LICENSE-vgamepad.txt"}})) return std::nullopt;
        return out;
    }();
    return dir;
}

bool has_firmware() { return resource(IDR_FW_BAT).has_value(); }

std::optional<fs::path> firmware_script() {
    auto base = base_dir();
    if (!base || !has_firmware()) return std::nullopt;
    // arduino-cli wymaga, zeby katalog nazywal sie tak jak plik .ino
    fs::path dir = *base / L"firmware" / L"kierownica";
    if (!extract(dir, {{IDR_FW_INO, L"kierownica.ino"},
                       {IDR_FW_USB_H, L"g29_usb.h"},
                       {IDR_FW_BAT, L"wgraj_firmware.bat"},
                       {IDR_FW_PS1, L"wgraj_firmware.ps1"}}))
        return std::nullopt;
    return dir / L"wgraj_firmware.bat";
}

} // namespace wb::embedded
