// Wersja panelu - jedno miejsce dla programu, zasobu VERSIONINFO i instalatora
// (instalator czyta ja z Kierownica.exe).
#pragma once

#define APP_VERSION_MAJOR 3
#define APP_VERSION_MINOR 2
#define APP_VERSION_PATCH 0

#define APP_STR2(x) #x
#define APP_STR(x) APP_STR2(x)
#define APP_VERSION_TEXT APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR)
#define APP_VERSION_FULL APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR) "." APP_STR(APP_VERSION_PATCH) ".0"
