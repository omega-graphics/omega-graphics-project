#include "omegaWTK/Native/NativeNote.h"

namespace OmegaWTK::Native {

    class GTKNoteCenter : public NativeNoteCenter {
    public:
        void sendNativeNote(NativeNote &note) override {}
        GTKNoteCenter() = default;
        ~GTKNoteCenter() = default;
    };

    NNCP make_native_note_center() {
        return (NNCP) new GTKNoteCenter();
    }

}
