#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "omegaWTK/Native/NativeClipboard.h"

#include <memory>

namespace OmegaWTK::Native::Cocoa {

/// NSPasteboard-backed system clipboard. Wraps the general pasteboard
/// (the Cmd+C / Cmd+V buffer). The wrapper is stateless and resolves the
/// pasteboard lazily on each call, matching the GTK backend's contract.
///
/// Compile status: source-complete, compile-unverified off-platform
/// (this Linux host cannot build the Cocoa backend). See the §2.6 note
/// in Native-API-Completion-Proposal.md.
class CocoaClipboard : public NativeClipboard {
public:
    CocoaClipboard() = default;
    ~CocoaClipboard() override = default;

    bool hasType(ClipboardDataType type) const override {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSArray<NSPasteboardType> *probe = nil;
        switch(type){
            case ClipboardDataType::PlainText:
                probe = @[ NSPasteboardTypeString ];
                break;
            case ClipboardDataType::HTML:
                probe = @[ NSPasteboardTypeHTML ];
                break;
            case ClipboardDataType::Image:
                probe = @[ NSPasteboardTypeTIFF, NSPasteboardTypePNG ];
                break;
            case ClipboardDataType::FilePaths:
                probe = @[ NSPasteboardTypeFileURL ];
                break;
        }
        if(probe == nil){
            return false;
        }
        return [pb availableTypeFromArray:probe] != nil;
    }

    OmegaCommon::String getText() const override {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSString *str = [pb stringForType:NSPasteboardTypeString];
        if(str == nil){
            return {};
        }
        const char *utf8 = [str UTF8String];
        return utf8 != nullptr ? OmegaCommon::String{utf8} : OmegaCommon::String{};
    }

    void setText(const OmegaCommon::String & text) override {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSString *str = [NSString stringWithUTF8String:text.c_str()];
        if(str != nil){
            [pb setString:str forType:NSPasteboardTypeString];
        }
    }

    OmegaCommon::Vector<OmegaCommon::FS::Path> getFilePaths() const override {
        OmegaCommon::Vector<OmegaCommon::FS::Path> out;
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        NSDictionary *options = @{ NSPasteboardURLReadingFileURLsOnlyKey : @YES };
        NSArray<NSURL *> *urls = [pb readObjectsForClasses:@[ [NSURL class] ]
                                                   options:options];
        for(NSURL *url in urls){
            if(![url isFileURL]){
                continue;
            }
            const char *path = [[url path] UTF8String];
            if(path != nullptr){
                out.push_back(OmegaCommon::FS::Path(path));
            }
        }
        return out;
    }

    void setFilePaths(const OmegaCommon::Vector<OmegaCommon::FS::Path> & paths) override {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
        NSMutableArray<NSURL *> *urls = [NSMutableArray arrayWithCapacity:paths.size()];
        for(const auto & path : paths){
            // str() is non-const; read it off a local copy.
            OmegaCommon::FS::Path p = path;
            OmegaCommon::String s = p.str();
            NSString *nsPath = [NSString stringWithUTF8String:s.c_str()];
            if(nsPath != nil){
                [urls addObject:[NSURL fileURLWithPath:nsPath]];
            }
        }
        if([urls count] > 0){
            [pb writeObjects:urls];
        }
    }

    void clear() override {
        NSPasteboard *pb = [NSPasteboard generalPasteboard];
        [pb clearContents];
    }
};

}

namespace OmegaWTK::Native {
    NativeClipboardPtr get_native_clipboard(){
        static NativeClipboardPtr instance = std::make_shared<Cocoa::CocoaClipboard>();
        return instance;
    }
}
