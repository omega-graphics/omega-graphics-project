#ifndef KREATE_BASE_H
#define KREATE_BASE_H

#ifdef _WIN32
#ifdef KREATE__BUILD__
#define KREATE_EXPORT __declspec(dllexport)
#else
#define KREATE_EXPORT __declspec(dllimport)
#endif
#else
#define KREATE_EXPORT
#endif

#endif // KREATE_BASE_H