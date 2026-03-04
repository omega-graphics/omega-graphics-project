#import "NativePrivate/macos/CocoaEvent.h"
#import "NativePrivate/macos/CocoaItem.h"
#import "NativePrivate/macos/CocoaUtils.h"
#include "omegaWTK/Native/NativeEvent.h"
#import <Cocoa/Cocoa.h>

namespace OmegaWTK::Native::Cocoa {

static ModifierFlags modifier_flags_from_ns(NSEventModifierFlags flags) {
    ModifierFlags m;
    m.shift = (flags & NSEventModifierFlagShift) != 0;
    m.control = (flags & NSEventModifierFlagControl) != 0;
    m.alt = (flags & NSEventModifierFlagOption) != 0;
    m.meta = (flags & NSEventModifierFlagCommand) != 0;
    m.capsLock = (flags & NSEventModifierFlagCapsLock) != 0;
    return m;
}

static void fill_position_from_event(NSEvent *event, NSView *inView,
                                     Core::Position &position,
                                     Core::Position &screenPosition) {
    NSPoint loc = event.locationInWindow;
    if (inView != nil) {
        NSPoint viewLoc = [inView convertPoint:loc fromView:nil];
        position.x = static_cast<float>(viewLoc.x);
        position.y = static_cast<float>(viewLoc.y);
    } else {
        position.x = static_cast<float>(loc.x);
        position.y = static_cast<float>(loc.y);
    }
    NSWindow *win = event.window;
    if (win != nil) {
        NSPoint screenPt = [win convertPointToScreen:loc];
        NSRect screenRect = { .origin = screenPt, .size = NSMakeSize(0, 0) };
        NSScreen *screen = win.screen ?: [NSScreen mainScreen];
        if (screen != nil) {
            CGFloat screenHeight = screen.frame.size.height;
            screenPosition.x = static_cast<float>(screenPt.x);
            screenPosition.y = static_cast<float>(screenHeight - screenPt.y);
        } else {
            screenPosition.x = static_cast<float>(screenPt.x);
            screenPosition.y = static_cast<float>(screenPt.y);
        }
    } else {
        screenPosition = position;
    }
}

static KeyCode key_code_from_ns(unsigned short keyCode) {
    // macOS virtual key codes (kVK_*)
    switch (keyCode) {
        case 0x00: return KeyCode::A;
        case 0x01: return KeyCode::S;
        case 0x02: return KeyCode::D;
        case 0x03: return KeyCode::F;
        case 0x04: return KeyCode::H;
        case 0x05: return KeyCode::G;
        case 0x06: return KeyCode::Z;
        case 0x07: return KeyCode::X;
        case 0x08: return KeyCode::C;
        case 0x09: return KeyCode::V;
        case 0x0B: return KeyCode::B;
        case 0x0C: return KeyCode::Q;
        case 0x0D: return KeyCode::W;
        case 0x0E: return KeyCode::E;
        case 0x0F: return KeyCode::R;
        case 0x10: return KeyCode::Y;
        case 0x11: return KeyCode::T;
        case 0x12: return KeyCode::Num1;
        case 0x13: return KeyCode::Num2;
        case 0x14: return KeyCode::Num3;
        case 0x15: return KeyCode::Num4;
        case 0x16: return KeyCode::Num5;
        case 0x17: return KeyCode::Num6;
        case 0x18: return KeyCode::Num7;
        case 0x19: return KeyCode::Num8;
        case 0x1A: return KeyCode::Num9;
        case 0x1B: return KeyCode::Num0;
        case 0x1C: return KeyCode::O;
        case 0x1D: return KeyCode::U;
        case 0x1E: return KeyCode::I;
        case 0x1F: return KeyCode::P;
        case 0x20: return KeyCode::L;
        case 0x21: return KeyCode::J;
        case 0x22: return KeyCode::Unknown; // quote
        case 0x23: return KeyCode::K;
        case 0x24: return KeyCode::Enter;
        case 0x25: return KeyCode::N;
        case 0x26: return KeyCode::M;
        case 0x27: return KeyCode::Unknown; // period
        case 0x28: return KeyCode::Tab;
        case 0x29: return KeyCode::Space;
        case 0x2A: return KeyCode::Backspace;
        case 0x2B: return KeyCode::Escape;
        case 0x2C: return KeyCode::RightMeta; // right command
        case 0x2D: return KeyCode::RightShift;
        case 0x2E: return KeyCode::Unknown; // period
        case 0x2F: return KeyCode::Unknown; // slash
        case 0x30: return KeyCode::CapsLock;
        case 0x31: return KeyCode::LeftAlt;
        case 0x32: return KeyCode::LeftShift;
        case 0x33: return KeyCode::LeftControl;
        case 0x34: return KeyCode::LeftMeta;
        case 0x35: return KeyCode::Escape; // keypad escape
        case 0x36: return KeyCode::RightControl;
        case 0x37: return KeyCode::Unknown; // keypad *
        case 0x38: return KeyCode::RightShift;
        case 0x39: return KeyCode::Unknown; // keypad +
        case 0x3A: return KeyCode::RightAlt;
        case 0x3B: return KeyCode::LeftControl;
        case 0x3C: return KeyCode::RightShift;
        case 0x3D: return KeyCode::Unknown; // keypad =
        case 0x3E: return KeyCode::Unknown; // keypad 0
        case 0x3F: return KeyCode::Unknown; // keypad 1
        case 0x40: return KeyCode::Unknown; // keypad 2
        case 0x41: return KeyCode::Unknown; // keypad 3
        case 0x42: return KeyCode::Unknown; // keypad 4
        case 0x43: return KeyCode::Unknown; // keypad 5
        case 0x44: return KeyCode::Unknown; // keypad 6
        case 0x45: return KeyCode::Unknown; // keypad 7
        case 0x46: return KeyCode::Unknown; // keypad 8
        case 0x47: return KeyCode::Unknown; // keypad 9
        case 0x48: return KeyCode::Unknown; // keypad clear
        case 0x49: return KeyCode::Unknown; // keypad enter
        case 0x4A: return KeyCode::Unknown; // keypad -
        case 0x4B: return KeyCode::Unknown; // keypad /
        case 0x4C: return KeyCode::Enter; // keypad enter
        case 0x4E: return KeyCode::Unknown; // keypad +
        case 0x4F: return KeyCode::Unknown; // keypad 1
        case 0x50: return KeyCode::ArrowDown;
        case 0x51: return KeyCode::Unknown; // keypad 2
        case 0x52: return KeyCode::Unknown; // keypad 3
        case 0x53: return KeyCode::Unknown; // keypad 4
        case 0x54: return KeyCode::Unknown; // keypad 5
        case 0x55: return KeyCode::Unknown; // keypad 6
        case 0x56: return KeyCode::Unknown; // keypad 7
        case 0x57: return KeyCode::Unknown; // keypad 8
        case 0x58: return KeyCode::Unknown; // keypad 9
        case 0x59: return KeyCode::Unknown; // keypad 0
        case 0x5A: return KeyCode::Unknown; // keypad .
        case 0x5B: return KeyCode::ArrowLeft;
        case 0x5C: return KeyCode::ArrowRight;
        case 0x5D: return KeyCode::ArrowRight;
        case 0x5E: return KeyCode::ArrowLeft;
        case 0x5F: return KeyCode::Unknown; // keypad /
        case 0x60: return KeyCode::F5;
        case 0x61: return KeyCode::F6;
        case 0x62: return KeyCode::F7;
        case 0x63: return KeyCode::F3;
        case 0x64: return KeyCode::F8;
        case 0x65: return KeyCode::F9;
        case 0x67: return KeyCode::F11;
        case 0x69: return KeyCode::F13;
        case 0x6A: return KeyCode::Unknown; // keypad *
        case 0x6B: return KeyCode::F14;
        case 0x6D: return KeyCode::F10;
        case 0x6F: return KeyCode::F12;
        case 0x71: return KeyCode::F15;
        case 0x72: return KeyCode::Unknown; // help
        case 0x73: return KeyCode::Home;
        case 0x74: return KeyCode::PageUp;
        case 0x75: return KeyCode::Delete;
        case 0x76: return KeyCode::F4;
        case 0x77: return KeyCode::End;
        case 0x78: return KeyCode::F2;
        case 0x79: return KeyCode::PageDown;
        case 0x7A: return KeyCode::F1;
        case 0x7B: return KeyCode::ArrowLeft;
        case 0x7C: return KeyCode::ArrowRight;
        case 0x7D: return KeyCode::ArrowDown;
        case 0x7E: return KeyCode::ArrowUp;
        default: return KeyCode::Unknown;
    }
}

NativeEventPtr ns_event_to_omega_wtk_native_event(NSEvent *event, NSView *inView) {
    NativeEvent::EventType type = NativeEvent::Unknown;
    NativeEventParams params = nullptr;

    switch (event.type) {
        case NSEventTypeMouseEntered: {
            auto *p = new CursorEnterParams();
            Core::Position screenTmp;
            fill_position_from_event(event, inView, p->position, screenTmp);
            params = p;
            type = NativeEvent::CursorEnter;
            break;
        }
        case NSEventTypeMouseExited: {
            auto *p = new CursorExitParams();
            Core::Position screenTmp;
            fill_position_from_event(event, inView, p->position, screenTmp);
            params = p;
            type = NativeEvent::CursorExit;
            break;
        }
        case NSEventTypeLeftMouseDown: {
            auto *p = new LMouseDownParams();
            fill_position_from_event(event, inView, p->position, p->screenPosition);
            p->modifiers = modifier_flags_from_ns(event.modifierFlags);
            p->clickCount = static_cast<unsigned>(event.clickCount);
            params = p;
            type = NativeEvent::LMouseDown;
            break;
        }
        case NSEventTypeLeftMouseUp: {
            auto *p = new LMouseUpParams();
            fill_position_from_event(event, inView, p->position, p->screenPosition);
            p->modifiers = modifier_flags_from_ns(event.modifierFlags);
            p->clickCount = static_cast<unsigned>(event.clickCount);
            params = p;
            type = NativeEvent::LMouseUp;
            break;
        }
        case NSEventTypeRightMouseDown: {
            auto *p = new RMouseDownParams();
            fill_position_from_event(event, inView, p->position, p->screenPosition);
            p->modifiers = modifier_flags_from_ns(event.modifierFlags);
            p->clickCount = static_cast<unsigned>(event.clickCount);
            params = p;
            type = NativeEvent::RMouseDown;
            break;
        }
        case NSEventTypeRightMouseUp: {
            auto *p = new RMouseUpParams();
            fill_position_from_event(event, inView, p->position, p->screenPosition);
            p->modifiers = modifier_flags_from_ns(event.modifierFlags);
            p->clickCount = static_cast<unsigned>(event.clickCount);
            params = p;
            type = NativeEvent::RMouseUp;
            break;
        }
        case NSEventTypeMouseMoved: {
            auto *p = new CursorMoveParams();
            fill_position_from_event(event, inView, p->position, p->screenPosition);
            p->modifiers = modifier_flags_from_ns(event.modifierFlags);
            params = p;
            type = NativeEvent::CursorMove;
            break;
        }
        case NSEventTypeKeyDown: {
            auto *p = new KeyDownParams();
            p->code = key_code_from_ns(event.keyCode);
            NSString *chars = event.characters;
            if (chars != nil && chars.length > 0) {
                unichar u = [chars characterAtIndex:0];
                p->key = static_cast<OmegaWTK::Unicode32Char>(u);
            } else {
                p->key = 0;
            }
            p->modifiers = modifier_flags_from_ns(event.modifierFlags);
            p->isRepeat = event.ARepeat;
            params = p;
            type = NativeEvent::KeyDown;
            break;
        }
        case NSEventTypeKeyUp: {
            auto *p = new KeyUpParams();
            p->code = key_code_from_ns(event.keyCode);
            NSString *chars = event.characters;
            if (chars != nil && chars.length > 0) {
                unichar u = [chars characterAtIndex:0];
                p->key = static_cast<OmegaWTK::Unicode32Char>(u);
            } else {
                p->key = 0;
            }
            p->modifiers = modifier_flags_from_ns(event.modifierFlags);
            params = p;
            type = NativeEvent::KeyUp;
            break;
        }
        default:
            type = NativeEvent::Unknown;
            params = nullptr;
            break;
    }

    return NativeEventPtr(new NativeEvent(type, params));
}

}
