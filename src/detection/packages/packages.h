#pragma once

#ifndef FF_INCLUDED_detection_packages_packages
#define FF_INCLUDED_detection_packages_packages

#include "fastfetch.h"

typedef struct FFPackagesResult
{
    uint32_t all;

    uint32_t apk;
    uint32_t brew;
    uint32_t dpkg;
    uint32_t emerge;
    uint32_t flatpak;
    uint32_t nixDefault;
    uint32_t nixSystem;
    uint32_t nixUser;
    uint32_t pacman;
    uint32_t pkg;
    uint32_t port;
    uint32_t rpm;
    uint32_t scoop;
    uint32_t snap;
    uint32_t xbps;

    FFstrbuf pacmanBranch;
} FFPackagesResult;

const FFPackagesResult* ffDetectPackages(const FFinstance* instance);

#endif
