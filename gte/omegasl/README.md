# OmegaSL

A cross platform shading language that transpiles into HLSL, MSL (Metal Shading Language), and GLSL (with Vulkan Semantics)

Move target-specific rendering/rules to a Target specfic subclass. (Target superclass in `Target.h`, subclasses in `HLSL|MSL|GLSLTarget.cpp`) and Make CodeGen non-virtual and carry the shared-rendering rules.

and write test for preprocessor in omegasl. (also includes `#include`) 
