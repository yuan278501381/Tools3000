#pragma once

#ifndef TOOLS3000_CORE_UTILS_EXPORT_H
#define TOOLS3000_CORE_UTILS_EXPORT_H

#if defined(_WIN32)
    #if defined(TOOLS3000CORE_EXPORTS)
        #define TOOLS3000CORE_API __declspec(dllexport)
    #else
        #define TOOLS3000CORE_API __declspec(dllimport)
    #endif

    #if defined(PLUGIN_EXPORTS)
        #define PLUGIN_API extern "C" __declspec(dllexport)
    #else
        #define PLUGIN_API extern "C" __declspec(dllimport)
    #endif
#else
    #define TOOLS3000CORE_API
    #define PLUGIN_API
#endif

#endif // TOOLS3000_CORE_UTILS_EXPORT_H
