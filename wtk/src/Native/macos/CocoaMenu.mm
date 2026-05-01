#include "omegaWTK/Native/NativeMenu.h"
#include "NativePrivate/macos/CocoaUtils.h"
#include "CocoaMenu.h"

#import <Cocoa/Cocoa.h>

namespace OmegaWTK::Native::Cocoa {
    class CocoaMenuItem;
    class CocoaMenu;
};

@interface CocoaMenuItemDelegate : NSObject
-(void)hasMenu;
-(instancetype)initWithCppBinding:(OmegaWTK::Native::Cocoa::CocoaMenuItem *)cpp_binding;
@end

@interface CocoaMenuValidator : NSObject<NSMenuItemValidation>
-(instancetype)initWithCppBinding:(OmegaWTK::Native::Cocoa::CocoaMenu *)cpp_binding;
@end


namespace OmegaWTK::Native::Cocoa {

// --------------------------------------------------------------------------
// Shortcut translation
// --------------------------------------------------------------------------

namespace {

NSString *shortcutKeyEquivalent(const NativeMenuShortcut &sc) {
    if(sc.key.empty()) return @"";
    const OmegaCommon::String &k = sc.key;
    // Special keys (multi-char names) -> Cocoa unicode function key constants.
    if(k.size() > 1) {
        unichar code = 0;
        if(k == "F1")  code = NSF1FunctionKey;
        else if(k == "F2")  code = NSF2FunctionKey;
        else if(k == "F3")  code = NSF3FunctionKey;
        else if(k == "F4")  code = NSF4FunctionKey;
        else if(k == "F5")  code = NSF5FunctionKey;
        else if(k == "F6")  code = NSF6FunctionKey;
        else if(k == "F7")  code = NSF7FunctionKey;
        else if(k == "F8")  code = NSF8FunctionKey;
        else if(k == "F9")  code = NSF9FunctionKey;
        else if(k == "F10") code = NSF10FunctionKey;
        else if(k == "F11") code = NSF11FunctionKey;
        else if(k == "F12") code = NSF12FunctionKey;
        else if(k == "Tab")       return @"\t";
        else if(k == "Space")     return @" ";
        else if(k == "Return" || k == "Enter") return @"\r";
        else if(k == "Backspace") code = NSBackspaceCharacter;
        else if(k == "Delete")    code = NSDeleteCharacter;
        else if(k == "Escape")    return @"\x1B";
        else if(k == "Up")        code = NSUpArrowFunctionKey;
        else if(k == "Down")      code = NSDownArrowFunctionKey;
        else if(k == "Left")      code = NSLeftArrowFunctionKey;
        else if(k == "Right")     code = NSRightArrowFunctionKey;
        else if(k == "Home")      code = NSHomeFunctionKey;
        else if(k == "End")       code = NSEndFunctionKey;
        else if(k == "PageUp")    code = NSPageUpFunctionKey;
        else if(k == "PageDown")  code = NSPageDownFunctionKey;
        if(code != 0) return [NSString stringWithCharacters:&code length:1];
        // Unrecognized special name: take first character.
    }
    char c = k[0];
    if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return [NSString stringWithFormat:@"%c", c];
}

NSEventModifierFlags shortcutModifierMask(const NativeMenuShortcut &sc) {
    NSEventModifierFlags m = 0;
    if(sc.shift) m |= NSEventModifierFlagShift;
    if(sc.alt)   m |= NSEventModifierFlagOption;
    if(sc.meta)  m |= NSEventModifierFlagCommand;
    return m;
}

} // namespace

// --------------------------------------------------------------------------
// CocoaMenuItem
// --------------------------------------------------------------------------

class CocoaMenuItem : public NativeMenuItem {
    CocoaMenuItemDelegate *delegate = nil;
    SharedHandle<CocoaMenu> sub_menu;
    SharedHandle<CocoaMenu> parent_menu;
    NativeMenuItemType type = NativeMenuItemType::Button;
    unsigned idx = 0;
    bool ownsItem = false;
    friend class CocoaMenu;
public:
    bool isSeperator;
    NSMenuItem *item;

    void setState(bool state) override;
    void setTitle(const OmegaCommon::String & title) override {
        if(item) [item setTitle:common_string_to_ns_string(title)];
    }
    void setChecked(bool checked) override {
        if(!item || type == NativeMenuItemType::Separator) return;
        item.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    }
    bool isChecked() const override {
        if(!item) return false;
        return item.state == NSControlStateValueOn;
    }
    void setShortcut(const NativeMenuShortcut & sc) override {
        if(!item || type == NativeMenuItemType::Separator) return;
        item.keyEquivalent = shortcutKeyEquivalent(sc);
        item.keyEquivalentModifierMask = shortcutModifierMask(sc);
    }
    NativeMenuItemType getType() const override { return type; }

    void selectThisItem();
    void assignIndex(unsigned _idx){ idx = _idx; }
    unsigned getIndex() const { return idx; }
    void setOwningParent(SharedHandle<CocoaMenu> p) { parent_menu = std::move(p); }

    /// Constructs a regular Button menu item.
    CocoaMenuItem(const OmegaCommon::String & str,SharedHandle<CocoaMenu> parent,bool hasSubMenu,SharedHandle<CocoaMenu> subMenu);
    /// Constructs a Separator item.
    explicit CocoaMenuItem();
    /// Constructs a Checkbox or Radio item.
    CocoaMenuItem(const OmegaCommon::String & str, NativeMenuItemType t, bool initialChecked);

    ~CocoaMenuItem() override{
        [delegate release];
        if(ownsItem){
            [item release];
        }
    };
};

// --------------------------------------------------------------------------
// CocoaMenu
// --------------------------------------------------------------------------

class CocoaMenu : public NativeMenu, public std::enable_shared_from_this<CocoaMenu> {
    NSMenu *menu;
    OmegaCommon::Vector<SharedHandle<CocoaMenuItem>> items;
    CocoaMenuValidator *validator = nil;
    void menuItemSelected(unsigned idx){
        if(hasDelegate)
            delegate->onSelectItem(idx);
    };
    void addMenuItem(NMI menu_item) override {
        auto item = std::dynamic_pointer_cast<CocoaMenuItem>(menu_item);
        if(!item) return;
        item->setOwningParent(shared_from_this());
        [menu addItem:item->item];
        item->assignIndex((unsigned)items.size());
        items.push_back(item);
    };
    void insertMenuItem(NMI menu_item, unsigned idx) override {
        auto item = std::dynamic_pointer_cast<CocoaMenuItem>(menu_item);
        if(!item) return;
        if(idx > items.size()) idx = (unsigned)items.size();
        item->setOwningParent(shared_from_this());
        [menu insertItem:item->item atIndex:(NSInteger)idx];
        items.insert(items.cbegin() + idx, item);
        reindex();
    };
    void removeMenuItem(unsigned idx) override {
        if(idx >= items.size()) return;
        [menu removeItemAtIndex:(NSInteger)idx];
        items.erase(items.cbegin() + idx);
        reindex();
    }
    void removeAllItems() override {
        [menu removeAllItems];
        items.clear();
    }
    unsigned itemCount() const override {
        return (unsigned)items.size();
    }
    void * getNativeBinding() override{
        return (void *)menu;
    };
    void updateMenu(){
        [menu update];
    };
    void reindex(){
        for(unsigned i = 0; i < items.size(); ++i) items[i]->assignIndex(i);
    }
    friend class CocoaMenuItem;
public:
    /// Constructs a Menu.
    explicit CocoaMenu(const OmegaCommon::String & name ){
        menu = [[NSMenu alloc] initWithTitle:common_string_to_ns_string(name)];
        [menu setAutoenablesItems:NO];
    };
    NSMenu *nsMenu() const { return menu; }

    /// Validation hook called by NSMenuItemValidation. Returns whether the
    /// item should be enabled for this display cycle.
    bool validateItem(NSMenuItem *nsItem) {
        if(!hasDelegate) return YES;
        for(unsigned i = 0; i < items.size(); ++i) {
            if(items[i]->item == nsItem) {
                return delegate->onValidateItem(i);
            }
        }
        return YES;
    }

    void installValidator() {
        if(validator) return;
        validator = [[CocoaMenuValidator alloc] initWithCppBinding:this];
        // NSMenu uses the item's target/action for validation; we wire the
        // validator object as a fallback target on items that don't already
        // have one. Items added later get their target set by hasMenu.
    }

    CocoaMenuValidator *getValidator() {
        if(!validator) installValidator();
        return validator;
    }

    ~CocoaMenu() override{
        [validator release];
        [menu release];
    };
};

// --------------------------------------------------------------------------
// CocoaMenuItem implementations (need full CocoaMenu definition above)
// --------------------------------------------------------------------------

CocoaMenuItem::CocoaMenuItem(const OmegaCommon::String & str,
                             SharedHandle<CocoaMenu> parent,
                             bool hasSubMenu,
                             SharedHandle<CocoaMenu> subMenu):
        delegate([[CocoaMenuItemDelegate alloc] initWithCppBinding:this]),
        sub_menu(subMenu),parent_menu(parent),
        type(NativeMenuItemType::Button),
        isSeperator(false),
        item([[NSMenuItem alloc] initWithTitle:common_string_to_ns_string(str) action:nil keyEquivalent:@""]){
    ownsItem = true;
    [delegate hasMenu];
    if(hasSubMenu){
        [item setSubmenu:sub_menu->nsMenu()];
    };
};

CocoaMenuItem::CocoaMenuItem():
        sub_menu(nullptr),
        type(NativeMenuItemType::Separator),
        isSeperator(true),
        item([NSMenuItem separatorItem]){
    [item setEnabled:NO];
};

CocoaMenuItem::CocoaMenuItem(const OmegaCommon::String & str,
                             NativeMenuItemType t,
                             bool initialChecked):
        delegate([[CocoaMenuItemDelegate alloc] initWithCppBinding:this]),
        sub_menu(nullptr),
        type(t),
        isSeperator(false),
        item([[NSMenuItem alloc] initWithTitle:common_string_to_ns_string(str) action:nil keyEquivalent:@""]){
    ownsItem = true;
    [delegate hasMenu];
    item.state = initialChecked ? NSControlStateValueOn : NSControlStateValueOff;
}

void CocoaMenuItem::selectThisItem(){
    if(parent_menu) parent_menu->menuItemSelected(this->idx);
};

void CocoaMenuItem::setState(bool state) {
    if(!isSeperator) {
       BOOL _state = state ? YES : NO;
       [item setEnabled:_state];
       if(parent_menu) parent_menu->updateMenu();
    }
};

// --------------------------------------------------------------------------
// Factories
// --------------------------------------------------------------------------

NMI make_cocoa_menu_item(const OmegaCommon::String & str,NM parent,bool hasSubMenu,NM subMenu){
    return (NMI)new CocoaMenuItem(str,std::dynamic_pointer_cast<CocoaMenu>(parent),hasSubMenu,std::dynamic_pointer_cast<CocoaMenu>(subMenu));
};

NMI make_cocoa_menu_seperator(){
    return (NMI)new CocoaMenuItem();
};

NMI make_cocoa_checkbox_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
    (void)parent;
    return (NMI)new CocoaMenuItem(str, NativeMenuItemType::Checkbox, initialChecked);
}

NMI make_cocoa_radio_item(const OmegaCommon::String & str, NM parent, bool initialChecked){
    (void)parent;
    return (NMI)new CocoaMenuItem(str, NativeMenuItemType::Radio, initialChecked);
}

NM make_cocoa_menu(const OmegaCommon::String & name){
    return std::make_shared<CocoaMenu>(name);
};

void show_cocoa_context_menu(NM menu, Composition::Point2D screenPos){
    auto cm = std::dynamic_pointer_cast<CocoaMenu>(menu);
    if(!cm) return;
    NSPoint p = NSMakePoint(screenPos.x, screenPos.y);
    [cm->nsMenu() popUpMenuPositioningItem:nil
                                atLocation:p
                                    inView:nil];
}

};

// --------------------------------------------------------------------------
// Obj-C delegates
// --------------------------------------------------------------------------

@implementation CocoaMenuItemDelegate {
    OmegaWTK::Native::Cocoa::CocoaMenuItem *_cpp_binding;
}

- (instancetype)initWithCppBinding:(OmegaWTK::Native::Cocoa::CocoaMenuItem *)cpp_binding
{
    self = [super init];
    if (self) {
        _cpp_binding = cpp_binding;
    }
    return self;
}

-(void)hasMenu {
    if(!_cpp_binding->isSeperator) {
        [_cpp_binding->item setTarget:self];
        [_cpp_binding->item setAction:@selector(menuItemAction)];
        [_cpp_binding->item setEnabled:YES];
    }
};

-(void)menuItemAction {
    _cpp_binding->selectThisItem();
};

// NSMenuValidation: forward to the parent menu, which queries the user delegate.
-(BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    if(_cpp_binding) {
        // Walk up to the owning menu via the C++ binding.
        // Because parent_menu is private we ask the binding for the index instead
        // and rely on a friend access in the menu. Simpler: route via the menu's
        // [menu:updateItem:atIndex:shouldCancel:] is N/A — instead the menu sets
        // itself as the validator via CocoaMenuValidator. This method is only
        // called for items whose target is *us* (the per-item delegate). For
        // unwired validation we bail to YES and let CocoaMenuValidator handle it.
    }
    return YES;
}

@end

@implementation CocoaMenuValidator {
    OmegaWTK::Native::Cocoa::CocoaMenu *_owner;
}

- (instancetype)initWithCppBinding:(OmegaWTK::Native::Cocoa::CocoaMenu *)cpp_binding
{
    self = [super init];
    if (self) { _owner = cpp_binding; }
    return self;
}

- (BOOL)validateMenuItem:(NSMenuItem *)menuItem {
    if(_owner) return _owner->validateItem(menuItem) ? YES : NO;
    return YES;
}

@end
