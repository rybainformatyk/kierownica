// Wersja panelu - jedno miejsce dla programu, zasobu VERSIONINFO i instalatora
// (instalator czyta ja z Kierownica.exe).
#pragma once

#define APP_VERSION_MAJOR 3
#define APP_VERSION_MINOR 2
#define APP_VERSION_PATCH 1

#define APP_STR2(x) #x
#define APP_STR(x) APP_STR2(x)
// "3.2" albo "3.2.1" - tak samo jak tag wydania. Aktualizator porownuje te
// wersje z GitHubem: bez numeru poprawki 3.2.1 widzialaby sama siebie jako
// nowsza i aktualizowala sie w kolko.
#if APP_VERSION_PATCH
#define APP_VERSION_TEXT APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR) "." APP_STR(APP_VERSION_PATCH)
#else
#define APP_VERSION_TEXT APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR)
#endif
#define APP_VERSION_FULL APP_STR(APP_VERSION_MAJOR) "." APP_STR(APP_VERSION_MINOR) "." APP_STR(APP_VERSION_PATCH) ".0"
