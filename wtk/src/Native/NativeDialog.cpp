#include "omegaWTK/Native/NativeDialog.h"


namespace OmegaWTK::Native {

    NativeDialog::NativeDialog(NWH nativeWindow):parentWindow(nativeWindow){

    };

    NativeFSDialog::NativeFSDialog(NWH nativeWindow):NativeDialog(nativeWindow){};

    NativeAlertDialog::NativeAlertDialog(NWH nativeWindow):NativeDialog(nativeWindow){};

    NativeNoteDialog::NativeNoteDialog(NWH nativeWindow):NativeDialog(nativeWindow){};

};
