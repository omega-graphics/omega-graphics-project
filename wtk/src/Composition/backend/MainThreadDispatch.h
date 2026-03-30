#ifndef OMEGAWTK_COMPOSITION_BACKEND_MAINTHREADDISPATCH_H
#define OMEGAWTK_COMPOSITION_BACKEND_MAINTHREADDISPATCH_H

#include <type_traits>

#if defined(TARGET_MACOS) || defined(TARGET_IOS)
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

namespace OmegaWTK::Composition {

    /// Runs a callable synchronously on the main thread.
    /// If already on the main thread, executes inline to avoid deadlock.
    /// Uses dispatch_sync_f (C function pointer) to avoid -fblocks requirement.
    template<typename Fn>
    void runOnMainThread(Fn && fn){
#if defined(TARGET_MACOS) || defined(TARGET_IOS)
        if(pthread_main_np() != 0){
            fn();
        }
        else {
            using FnType = std::remove_reference_t<Fn>;
            dispatch_sync_f(dispatch_get_main_queue(),
                            static_cast<void *>(&fn),
                            [](void *ctx){ (*static_cast<FnType *>(ctx))(); });
        }
#elif defined(_WIN32)
        // TODO: Windows main-thread dispatch.
        fn();
#else
        fn();
#endif
    }

}

#endif
