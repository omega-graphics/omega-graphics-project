#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeEvent.h"
#import "CocoaItem.h"

#import <Cocoa/Cocoa.h>

#ifndef OMEGAWTK_NATIVE_COCOAEVENT_H
#define OMEGAWTK_NATIVE_COCOAEVENT_H


namespace OmegaWTK::Native {
namespace Cocoa {
/// Converts NSEvent to NativeEvent. If inView is non-nil, position is in view coordinates; otherwise local position is zero.
NativeEventPtr ns_event_to_omega_wtk_native_event(NSEvent *event, NSView *inView = nil);
};
}

#endif
