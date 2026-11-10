// Pliki wbudowane w Kierownica.exe. Biblioteki DLL nie da sie zaladowac z
// pamieci zwyklym LoadLibrary, wiec zasoby trafiaja do %LOCALAPPDATA%\Kierownica.
#pragma once

#include <filesystem>
#include <optional>

namespace wb::embedded {

// Katalog z rozpakowanym ViGEmClient.dll; nic, gdy program nie ma zasobu (testy).
std::optional<std::filesystem::path> vigem_dir();

bool has_firmware();
// Rozpakowuje firmware i zwraca sciezke skryptu wgrywania.
std::optional<std::filesystem::path> firmware_script();

} // namespace wb::embedded
