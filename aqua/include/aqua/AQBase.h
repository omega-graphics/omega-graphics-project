#ifndef AQUA_AQBASE_H
#define AQUA_AQBASE_H

#ifdef _WIN32
#ifdef AQUA__BUILD__
#define AQUA_EXPORT __declspec(dllexport)
#else
#define AQUA_EXPORT __declspec(dllimport)
#endif
#else
#define AQUA_EXPORT
#endif

#if __cplusplus >= 2017
#define AQUA_NODISCARD [[nodiscard]]
#else
#define AQUA_NODISCARD
#endif

#endif // AQUA_AQBASE_H
