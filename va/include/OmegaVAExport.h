#ifndef OMEGAVAEXPORT_H
#define OMEGAVAEXPORT_H

// TODO: lift OMEGAVA_EXPORT + OMEGAVA_DEPRECATED into omega-common so
// OmegaWTK and OmegaVA share the same export-attribute machinery (mirrors
// the duplication this file currently has with wtk/include/OmegaWTKExport.h).

#if defined(OMEGAVA_BUILD_MODULE) && __cplusplus >= 202002L
#define OMEGAVA_EXPORT export
#elif defined(_WIN32)
#if defined(OMEGAVA_APP)
#define OMEGAVA_EXPORT __declspec(dllimport)
#else
#define OMEGAVA_EXPORT __declspec(dllexport)
#endif
#define _CRT_SECURE_NO_WARNINGS 1
#else
#define OMEGAVA_EXPORT
#endif

#if __cplusplus >= 201402L
#define OMEGAVA_DEPRECATED [[deprecated]]
#elif defined(__GNUC__) || defined(__clang__)
#define OMEGAVA_DEPRECATED __attribute__((deprecated))
#elif defined(_MSC_VER)
#define OMEGAVA_DEPRECATED __declspec(deprecated)
#endif

#endif
