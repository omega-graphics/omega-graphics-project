#include "omegaWTK/Native/NativeDialog.h"


namespace OmegaWTK::Native {

    NativeDialog::NativeDialog(NWH nativeWindow):parentWindow(nativeWindow){

    };

    NativeFSDialog::NativeFSDialog(NWH nativeWindow):NativeDialog(nativeWindow){};

    NativeNoteDialog::NativeNoteDialog(NWH nativeWindow):NativeDialog(nativeWindow){};

#ifdef TARGET_GTK
    SharedHandle<NativeFSDialog> NativeFSDialog::Create(const Descriptor &desc, NWH nativeWindow){
        (void)desc;
        (void)nativeWindow;
        return nullptr;
    }

    SharedHandle<NativeNoteDialog> NativeNoteDialog::Create(const Descriptor &desc, NWH nativeWindow){
        (void)desc;
        (void)nativeWindow;
        return nullptr;
    }
#endif

};
