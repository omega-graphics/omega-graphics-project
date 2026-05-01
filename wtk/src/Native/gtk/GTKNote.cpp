#include "omegaWTK/Native/NativeNote.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

#if defined(OMEGAWTK_HAS_LIBNOTIFY)
#include <libnotify/notify.h>
#include <glib.h>
#endif

namespace OmegaWTK::Native {

    class GTKNoteCenter : public NativeNoteCenter {
        NativeNoteCenterDelegate *delegate = nullptr;
        std::atomic<NativeNotePermission> cached{NativeNotePermission::Authorized};

        std::mutex catMtx;
        std::unordered_map<OmegaCommon::String, NativeNoteCategory> categories;

#if defined(OMEGAWTK_HAS_LIBNOTIFY)
        bool initialized = false;

        struct ActionCtx {
            GTKNoteCenter *owner;
            OmegaCommon::String noteId;
            OmegaCommon::String actionId;
        };

        std::mutex liveMtx;
        // Keeps NotifyNotification* and the ActionCtx* allocations alive for the
        // lifetime of the notification. Cleared on close/removal.
        std::unordered_map<OmegaCommon::String, NotifyNotification *> live;
        std::unordered_map<OmegaCommon::String, std::vector<ActionCtx *>> liveActions;
        // Pending immediate-fires scheduled via g_timeout_add, indexed by id so
        // we can cancel them via removeNote/removeAllNotes.
        std::unordered_map<OmegaCommon::String, guint> pendingTimers;

        bool ensureInit() {
            if(initialized) return true;
            if(!notify_init("OmegaWTK")) return false;
            initialized = true;
            return true;
        }

        static void onAction(NotifyNotification *, char *, gpointer user_data) {
            auto *ctx = static_cast<ActionCtx *>(user_data);
            if(ctx && ctx->owner && ctx->owner->delegate) {
                ctx->owner->delegate->onNoteAction(ctx->noteId, ctx->actionId);
            }
        }

        static void onClosed(NotifyNotification *n, gpointer user_data) {
            auto *self = static_cast<GTKNoteCenter *>(user_data);
            if(!self) return;

            // Find the id this NotifyNotification belongs to.
            std::lock_guard<std::mutex> lk(self->liveMtx);
            for(auto it = self->live.begin(); it != self->live.end(); ++it) {
                if(it->second == n) {
                    OmegaCommon::String id = it->first;
                    self->live.erase(it);
                    auto ait = self->liveActions.find(id);
                    if(ait != self->liveActions.end()) {
                        for(auto *c : ait->second) delete c;
                        self->liveActions.erase(ait);
                    }
                    g_object_unref(n);
                    break;
                }
            }
        }

        void showNow(const NativeNote & note, const OmegaCommon::String & id) {
            NotifyNotification *n = notify_notification_new(
                note.title.c_str(),
                note.body.c_str(),
                /* icon */ nullptr);
            if(!n) return;

            // Look up category and attach actions.
            NativeNoteCategory cat;
            bool hasCat = false;
            {
                std::lock_guard<std::mutex> lk(catMtx);
                auto it = categories.find(note.categoryIdentifier);
                if(it != categories.end()) { cat = it->second; hasCat = true; }
            }

            std::vector<ActionCtx *> ctxs;
            if(hasCat) {
                for(auto & a : cat.actions) {
                    auto *ctx = new ActionCtx{ this, id, a.identifier };
                    ctxs.push_back(ctx);
                    notify_notification_add_action(
                        n,
                        a.identifier.c_str(),
                        a.title.c_str(),
                        NOTIFY_ACTION_CALLBACK(&GTKNoteCenter::onAction),
                        ctx,
                        /* free_func */ nullptr);
                }
            }

            g_signal_connect(n, "closed", G_CALLBACK(&GTKNoteCenter::onClosed), this);

            {
                std::lock_guard<std::mutex> lk(liveMtx);
                live[id] = n;
                liveActions[id] = std::move(ctxs);
            }

            GError *err = nullptr;
            if(!notify_notification_show(n, &err)) {
                if(err) g_error_free(err);
                // Show failed; tear down.
                std::lock_guard<std::mutex> lk(liveMtx);
                live.erase(id);
                auto ait = liveActions.find(id);
                if(ait != liveActions.end()) {
                    for(auto *c : ait->second) delete c;
                    liveActions.erase(ait);
                }
                g_object_unref(n);
            }
        }

        struct DeferredCtx {
            GTKNoteCenter *owner;
            NativeNote note;
            OmegaCommon::String id;
        };

        static gboolean deferredFire(gpointer data) {
            auto *d = static_cast<DeferredCtx *>(data);
            if(d && d->owner) {
                d->owner->pendingTimers.erase(d->id);
                d->owner->showNow(d->note, d->id);
            }
            delete d;
            return G_SOURCE_REMOVE;
        }
#endif

        OmegaCommon::String autoId() {
            static std::atomic<unsigned long long> seq{0};
            return OmegaCommon::String("owtk-") + std::to_string(seq.fetch_add(1));
        }

    public:
        GTKNoteCenter() = default;
        ~GTKNoteCenter() override {
#if defined(OMEGAWTK_HAS_LIBNOTIFY)
            removeAllNotes();
            if(initialized) notify_uninit();
#endif
        }

        void requestPermission(std::function<void(NativeNotePermission)> callback) override {
            // No permission model in libnotify / freedesktop notifications.
            cached.store(NativeNotePermission::Authorized);
            if(callback) callback(NativeNotePermission::Authorized);
        }

        NativeNotePermission currentPermission() const override {
            return cached.load();
        }

        void sendNativeNote(NativeNote & note) override {
#if defined(OMEGAWTK_HAS_LIBNOTIFY)
            if(!ensureInit()) return;
            OmegaCommon::String id = note.identifier.empty() ? autoId() : note.identifier;

            if(note.delaySeconds > 0.f) {
                // libnotify has no native scheduling; defer via the GLib main loop.
                auto *d = new DeferredCtx{ this, note, id };
                guint timer = g_timeout_add((guint)(note.delaySeconds * 1000.f),
                                            &GTKNoteCenter::deferredFire,
                                            d);
                pendingTimers[id] = timer;
            } else {
                showNow(note, id);
            }
#else
            (void)note;
#endif
        }

        void removeNote(const OmegaCommon::String & identifier) override {
#if defined(OMEGAWTK_HAS_LIBNOTIFY)
            // Cancel a pending scheduled fire if any.
            {
                auto it = pendingTimers.find(identifier);
                if(it != pendingTimers.end()) {
                    g_source_remove(it->second);
                    pendingTimers.erase(it);
                }
            }
            std::lock_guard<std::mutex> lk(liveMtx);
            auto it = live.find(identifier);
            if(it == live.end()) return;
            NotifyNotification *n = it->second;
            GError *err = nullptr;
            notify_notification_close(n, &err);
            if(err) g_error_free(err);
            // The "closed" signal handler will clean up the maps and unref.
#else
            (void)identifier;
#endif
        }

        void removeAllNotes() override {
#if defined(OMEGAWTK_HAS_LIBNOTIFY)
            for(auto & kv : pendingTimers) g_source_remove(kv.second);
            pendingTimers.clear();

            std::lock_guard<std::mutex> lk(liveMtx);
            for(auto & kv : live) {
                GError *err = nullptr;
                notify_notification_close(kv.second, &err);
                if(err) g_error_free(err);
                // 'closed' callback will unref; but in case it doesn't fire
                // synchronously, we still clear our maps below.
            }
            // Drop our references; if 'closed' hasn't fired yet, the per-note
            // unref there becomes the final unref via reference counting.
            live.clear();
            for(auto & kv : liveActions) {
                for(auto *c : kv.second) delete c;
            }
            liveActions.clear();
#endif
        }

        void registerCategories(const OmegaCommon::Vector<NativeNoteCategory> & cats) override {
            std::lock_guard<std::mutex> lk(catMtx);
            for(auto & c : cats) categories[c.identifier] = c;
        }

        void setDelegate(NativeNoteCenterDelegate * d) override {
            delegate = d;
        }
    };

    NNCP make_native_note_center() {
        return std::make_shared<GTKNoteCenter>();
    }

}
