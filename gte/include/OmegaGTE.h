#include "omegaGTE/GTEBase.h"

#include "omegaGTE/GE.h"
#include "omegaGTE/GECommandQueue.h"
#include "omegaGTE/GEPipeline.h"
#include "omegaGTE/GERenderTarget.h"
#include "omegaGTE/GETexture.h"

#include "omegaGTE/TE.h"


#ifndef _OMEGAGTE_H
#define _OMEGAGTE_H

_NAMESPACE_BEGIN_

/**
  @brief All the features that are supported on a GTEDevice.
 * */
struct GTEDeviceFeatures {
    bool raytracing;
    bool msaa4x;
    bool msaa8x;
};

/**
  @brief A hardware (or software) device that is capable of performing graphical/matrix calculations.
  @paragraph This class is an easy interface for selecting the most suitable device to create the GTE.
 */
struct GTEDevice {
    /// @brief Defines the type of GPU device.
    typedef enum : int {
        Integrated,
        Discrete
    } Type;
    /// @enum Type
    const Type type;
    /// The Device name.
    const OmegaCommon::String name;
    /// The Device's features.
    const GTEDeviceFeatures features;
protected:
    GTEDevice(Type type,const char *name,GTEDeviceFeatures & features):type(type),name(name),features(features){

    };
public:
     OMEGACOMMON_CLASS("OmegaGTE.GTEDevice")
    virtual const void *native() = 0;
    virtual ~GTEDevice() = default;
};

/** @brief Enumerate all GPU devices on this system.
 * @return A Vector of all the existing GTEDevices on this system.
 * */
OMEGAGTE_EXPORT OmegaCommon::Vector<SharedHandle<GTEDevice>> enumerateDevices();

/// @brief The Graphics and Tessalation Engine!
struct OMEGAGTE_EXPORT GTE {
    SharedHandle<OmegaGraphicsEngine> graphicsEngine;
    SharedHandle<OmegaTessellationEngine> tessalationEngine;
#if RUNTIME_SHADER_COMP_SUPPORT
    SharedHandle<OmegaSLCompiler> omegaSlCompiler;
#endif
};


OMEGAGTE_EXPORT GTE Init(SharedHandle<GTEDevice> & device);

OMEGAGTE_EXPORT GTE InitWithDefaultDevice();

OMEGAGTE_EXPORT void Close(GTE &gte);

_NAMESPACE_END_


#endif