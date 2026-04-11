#ifndef AQUA_BASE_H
#define AQUA_BASE_H

#ifdef _WIN32
#ifdef AQUA__BUILD__
#define AQUA_EXPORT __declspec(dllexport)
#else
#define AQUA_EXPORT __declspec(dllimport)
#endif
#else
#define AQUA_EXPORT
#endif

#endif // AQUA_BASE_H