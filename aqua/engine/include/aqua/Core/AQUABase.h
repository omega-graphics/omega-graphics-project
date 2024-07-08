#include <vector>
#include <memory>

#ifndef AQUA_CORE_AQUABASE_H
#define AQUA_CORE_AQUABASE_H


#ifdef _WIN32
#ifdef __BUILD__ 
#define AQUA_PUBLIC __declspec(dllexport)
#else 
#define AQUA_PUBLIC __declspec(dllimport)
#endif
#else 
#define AQUA_PUBLIC
#endif

#define AQUA_NAMESPACE_BEGIN namespace AQUA {
#define AQUA_NAMESPACE_END }

#define READONLY_I_PROPERTY const
#define READWRITE_I_PROPERTY 

#define READONLY_C_PROPERTY static const
#define READWRITE_C_PROPERTY static

AQUA_NAMESPACE_BEGIN
template<class T>
using Vector = std::vector<T>;
template<class T>
using SharedHandle = std::shared_ptr<T>;


AQUA_NAMESPACE_END

#endif