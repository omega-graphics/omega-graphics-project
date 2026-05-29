#include "omegaWTK/Native/NativeDialog.h"
#import "NativePrivate/macos/CocoaUtils.h"
#import "CocoaAppWindow.h"

#import <Cocoa/Cocoa.h>

namespace OmegaWTK::Native::Cocoa {

    /// Flatten every filter's extensions into the single flat list NSOpenPanel
    /// / NSSavePanel accept (they have no per-group concept). Returns nil when
    /// there is no restriction so the panel shows all files.
    static NSArray<NSString *> * allowedFileTypes(const NativeFSDialog::Descriptor & desc){
        if(desc.filters.empty())
            return nil;
        NSMutableArray<NSString *> *types = [NSMutableArray array];
        for(auto & filter : desc.filters){
            for(auto & ext : filter.extensions){
                [types addObject:Cocoa::common_string_to_ns_string(ext)];
            }
        }
        return types.count > 0 ? types : nil;
    }

    /// openLocation is a const Path; Path::str() is non-const, so copy first.
    static NSURL * directoryURL(const NativeFSDialog::Descriptor & desc){
        OmegaCommon::FS::Path loc = desc.openLocation;
        if(loc.str().empty())
            return nil;
        return [NSURL fileURLWithPath:Cocoa::common_string_to_ns_string(loc.str())
                          isDirectory:YES];
    }

    class CocoaFSDialog : public NativeFSDialog {
        NSOpenPanel *openPanel;
        NSSavePanel *savePanel;
        bool allowMultiple;
        // shared_ptr (not a plain member) so the completion block can own the
        // promise state without keeping the whole dialog alive — the panel is
        // fire-and-forget safe: the caller may drop its SharedHandle as soon
        // as getResult() returns.
        std::shared_ptr<OmegaCommon::Promise<OmegaCommon::Vector<OmegaCommon::FS::Path>>> promise;
    public:
        OmegaCommon::Async<OmegaCommon::Vector<OmegaCommon::FS::Path>> getResult() override {
            NSWindow *parent = std::dynamic_pointer_cast<CocoaAppWindow>(this->parentWindow)->getWindow();
            // Capture the panel + promise state by value rather than `this`:
            // the block retains the panel and holds a strong ref to the
            // promise state, so neither depends on the CocoaFSDialog outliving
            // the sheet.
            auto prom = promise;
            if(openPanel != nil){
                NSOpenPanel *panel = openPanel;
                [panel beginSheetModalForWindow:parent completionHandler:^(NSModalResponse response){
                    OmegaCommon::Vector<OmegaCommon::FS::Path> result;
                    if(response == NSModalResponseOK){
                        for(NSURL *url in [panel URLs]){
                            result.push_back(OmegaCommon::FS::Path([url fileSystemRepresentation]));
                        }
                    }
                    prom->set(result);
                }];
            }
            else if(savePanel != nil){
                NSSavePanel *panel = savePanel;
                [panel beginSheetModalForWindow:parent completionHandler:^(NSModalResponse response){
                    OmegaCommon::Vector<OmegaCommon::FS::Path> result;
                    if(response == NSModalResponseOK){
                        NSURL *url = [panel URL];
                        if(url != nil){
                            result.push_back(OmegaCommon::FS::Path([url fileSystemRepresentation]));
                        }
                    }
                    prom->set(result);
                }];
            }
            return prom->async();
        };
        explicit CocoaFSDialog(const Descriptor &desc,NWH & nativeWindow):
            NativeFSDialog(nativeWindow),
            allowMultiple(desc.allowMultiple),
            promise(std::make_shared<OmegaCommon::Promise<OmegaCommon::Vector<OmegaCommon::FS::Path>>>()){
            NSArray<NSString *> *types = allowedFileTypes(desc);
            NSURL *dir = directoryURL(desc);
            if(desc.type == Read){
                openPanel = [NSOpenPanel openPanel];
                savePanel = nil;
                [openPanel setAllowsMultipleSelection:(allowMultiple ? YES : NO)];
                [openPanel setCanChooseFiles:YES];
                [openPanel setCanChooseDirectories:NO];
                if(types != nil)
                    [openPanel setAllowedFileTypes:types];
                if(dir != nil)
                    [openPanel setDirectoryURL:dir];
            }
            else if(desc.type == Write) {
                savePanel = [NSSavePanel savePanel];
                openPanel = nil;
                if(types != nil)
                    [savePanel setAllowedFileTypes:types];
                if(dir != nil)
                    [savePanel setDirectoryURL:dir];
            };
        };
    };

    /// Map a clicked button (by label and whether it was the first/default
    /// button) to the cross-platform Result. Mirrors the contract documented
    /// on NativeAlertDialog::Descriptor.
    static NativeAlertDialog::Result resultForLabel(NSString *label, bool isFirst){
        NSString *lower = [label lowercaseString];
        if([lower isEqualToString:@"ok"])     return NativeAlertDialog::Result::OK;
        if([lower isEqualToString:@"cancel"]) return NativeAlertDialog::Result::Cancel;
        if([lower isEqualToString:@"yes"])    return NativeAlertDialog::Result::Yes;
        if([lower isEqualToString:@"no"])      return NativeAlertDialog::Result::No;
        return isFirst ? NativeAlertDialog::Result::OK : NativeAlertDialog::Result::Cancel;
    }

    class CocoaAlertDialog : public NativeAlertDialog {
        NSAlert *alert;
        NSMutableArray<NSString *> *labels;
        // shared_ptr (not a plain member) so the completion block can own the
        // promise state without keeping the whole dialog alive — the alert
        // sheet is fire-and-forget safe: the caller may drop its SharedHandle
        // as soon as getResult() returns.
        std::shared_ptr<OmegaCommon::Promise<Result>> promise;
    public:
        OmegaCommon::Async<Result> getResult() override {
            NSWindow *parent = std::dynamic_pointer_cast<CocoaAppWindow>(this->parentWindow)->getWindow();
            // Capture the state the block needs by value rather than `this`:
            // the block retains `buttons` (NSArray) and holds a strong ref to
            // the promise state, and the sheet machinery retains `alert`. So
            // none of them depend on the CocoaAlertDialog outliving the sheet.
            NSArray<NSString *> *buttons = labels;
            auto prom = promise;
            [alert beginSheetModalForWindow:parent completionHandler:^(NSModalResponse response){
                // NSAlert returns NSAlertFirstButtonReturn (1000), Second
                // (1001), ... in the order buttons were added.
                NSInteger index = response - NSAlertFirstButtonReturn;
                if(index < 0 || index >= (NSInteger)buttons.count){
                    prom->set(Result::Cancel);
                    return;
                }
                prom->set(resultForLabel(buttons[(NSUInteger)index], index == 0));
            }];
            return prom->async();
        };
        explicit CocoaAlertDialog(const Descriptor &desc,NWH & nativeWindow):
            NativeAlertDialog(nativeWindow),
            promise(std::make_shared<OmegaCommon::Promise<Result>>()){
            alert = [[NSAlert alloc] init];
            alert.showsHelp = NO;
            alert.messageText = Cocoa::common_string_to_ns_string(desc.title);
            alert.informativeText = Cocoa::common_string_to_ns_string(desc.message);
            switch(desc.style){
                case Style::Info:    alert.alertStyle = NSAlertStyleInformational; break;
                case Style::Warning: alert.alertStyle = NSAlertStyleWarning; break;
                case Style::Error:   alert.alertStyle = NSAlertStyleCritical; break;
            }
            labels = [NSMutableArray array];
            if(desc.buttonLabels.empty()){
                [labels addObject:@"OK"];
            } else {
                for(auto & l : desc.buttonLabels)
                    [labels addObject:Cocoa::common_string_to_ns_string(l)];
            }
            for(NSString *l in labels)
                [alert addButtonWithTitle:l];
        };
    };

    class CocoaNoteDialog : public NativeNoteDialog {
        NSAlert *dialog;
    public:
        void show(){
            NSWindow *parentWindow = std::dynamic_pointer_cast<CocoaAppWindow>(this->parentWindow)->getWindow();
            [dialog beginSheetModalForWindow:parentWindow completionHandler:^(NSModalResponse response){

            }];
        };
        explicit CocoaNoteDialog(const Descriptor &desc,NWH & nativeWindow):NativeNoteDialog(nativeWindow){
            dialog = [[NSAlert alloc] init];
            dialog.showsHelp = NO;
            dialog.alertStyle = NSAlertStyleInformational;
            dialog.messageText = Cocoa::common_string_to_ns_string(desc.title);
            dialog.informativeText = Cocoa::common_string_to_ns_string(desc.str);
            show();
        };
    };

};

namespace OmegaWTK::Native {
    SharedHandle<NativeFSDialog> NativeFSDialog::Create(const NativeFSDialog::Descriptor &desc,NWH nativeWindow){
        return SharedHandle<NativeFSDialog>(new Cocoa::CocoaFSDialog(desc,nativeWindow));
    };

    SharedHandle<NativeAlertDialog> NativeAlertDialog::Create(const NativeAlertDialog::Descriptor &desc,NWH nativeWindow){
        return SharedHandle<NativeAlertDialog>(new Cocoa::CocoaAlertDialog(desc,nativeWindow));
    };

    SharedHandle<NativeNoteDialog> NativeNoteDialog::Create(const NativeNoteDialog::Descriptor &desc,NWH nativeWindow){
        return SharedHandle<NativeNoteDialog>(new Cocoa::CocoaNoteDialog(desc,nativeWindow));
    };
}
