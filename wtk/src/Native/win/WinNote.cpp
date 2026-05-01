#include "omegaWTK/Native/NativeNote.h"

#include <Windows.h>
#include <NotificationActivationCallback.h>
#include <windows.ui.notifications.h>
#include <windows.data.xml.dom.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <appmodel.h>

#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_map>

#pragma comment(lib,"runtimeobject.lib")

using namespace ABI;
using namespace ABI::Windows::UI::Notifications;
using namespace ABI::Windows::Data::Xml::Dom;
using namespace ABI::Windows::Foundation;
using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;

namespace OmegaWTK::Native {

    namespace {

        // Cached AUMID for the running process. Set once on first toast send.
        // Required for ToastNotificationManager::CreateToastNotifierWithId on
        // unpackaged Win32 apps.
        OmegaCommon::String g_aumid;

        std::wstring widen(const OmegaCommon::String & s) {
            if(s.empty()) return std::wstring();
            int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            std::wstring w(len, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), len);
            return w;
        }

        OmegaCommon::String narrow(LPCWSTR s) {
            if(!s) return {};
            int len = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
            if(len <= 1) return {};
            OmegaCommon::String out(len - 1, '\0');
            WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), len, nullptr, nullptr);
            return out;
        }

        // Escape XML attribute / element text.
        std::wstring xml_escape(const std::wstring & in) {
            std::wstring out;
            out.reserve(in.size());
            for(wchar_t c : in) {
                switch(c) {
                    case L'&':  out += L"&amp;";  break;
                    case L'<':  out += L"&lt;";   break;
                    case L'>':  out += L"&gt;";   break;
                    case L'"':  out += L"&quot;"; break;
                    case L'\'': out += L"&apos;"; break;
                    default:    out += c;         break;
                }
            }
            return out;
        }

        OmegaCommon::String resolveAumid() {
            if(!g_aumid.empty()) return g_aumid;
            // Prefer an explicitly-set AUMID. Falls back to the packaged identity
            // when the app is running from a packaged context.
            wchar_t *raw = nullptr;
            if(SUCCEEDED(GetCurrentProcessExplicitAppUserModelID(&raw)) && raw) {
                g_aumid = narrow(raw);
                CoTaskMemFree(raw);
                return g_aumid;
            }
            // Last resort: don't fabricate one. CreateToastNotifierWithId will fail
            // and we'll log; the caller is expected to call SetCurrentProcessExplicitAppUserModelID
            // (or be packaged) before sending notifications.
            return {};
        }

        struct CategoryStore {
            std::mutex mtx;
            std::unordered_map<OmegaCommon::String, NativeNoteCategory> map;
        };

    } // anonymous namespace

    class WinNotificationCenter : public NativeNoteCenter {
        ComPtr<IToastNotificationManagerStatics> mgrStatics;
        ComPtr<IToastNotificationManagerStatics2> mgrStatics2;
        ComPtr<IToastNotifier> notifier;
        HString aumidHs;
        OmegaCommon::String aumid;
        bool initialized = false;
        std::atomic<NativeNotePermission> cached{NativeNotePermission::Authorized};
        NativeNoteCenterDelegate *delegate = nullptr;

        CategoryStore categories;

        // Keep ComPtrs to live toasts alive until they're activated/dismissed,
        // otherwise the Activated event handler is released before it fires.
        std::mutex liveMtx;
        std::unordered_map<OmegaCommon::String, ComPtr<IToastNotification>> live;

        bool ensureInitialized() {
            if(initialized) return true;

            aumid = resolveAumid();
            if(aumid.empty()) {
                OutputDebugStringW(L"[OmegaWTK] No AUMID set; toast notifications disabled. "
                                   L"Call SetCurrentProcessExplicitAppUserModelID before NotificationCenter::send.\n");
                return false;
            }

            HRESULT hr = Windows::Foundation::GetActivationFactory(
                HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
                &mgrStatics);
            if(FAILED(hr)) return false;

            mgrStatics.As(&mgrStatics2); // optional

            std::wstring waumid = widen(aumid);
            hr = aumidHs.Set(waumid.c_str());
            if(FAILED(hr)) return false;

            hr = mgrStatics->CreateToastNotifierWithId(aumidHs.Get(), &notifier);
            if(FAILED(hr)) return false;

            initialized = true;
            return true;
        }

        ComPtr<IXmlDocument> buildXml(const NativeNote & note) {
            // Look up the category (if any) so we can emit <action> elements.
            NativeNoteCategory cat;
            if(!note.categoryIdentifier.empty()) {
                std::lock_guard<std::mutex> lk(categories.mtx);
                auto it = categories.map.find(note.categoryIdentifier);
                if(it != categories.map.end()) cat = it->second;
            }

            std::wstringstream ss;
            ss << L"<toast>"
               << L"<visual><binding template=\"ToastGeneric\">"
               << L"<text>" << xml_escape(widen(note.title)) << L"</text>"
               << L"<text>" << xml_escape(widen(note.body))  << L"</text>"
               << L"</binding></visual>";

            if(!cat.actions.empty()) {
                ss << L"<actions>";
                for(auto & a : cat.actions) {
                    ss << L"<action content=\"" << xml_escape(widen(a.title))
                       << L"\" arguments=\""    << xml_escape(widen(a.identifier))
                       << L"\" activationType=\"foreground\"/>";
                }
                ss << L"</actions>";
            }
            ss << L"</toast>";
            std::wstring xml = ss.str();

            ComPtr<IXmlDocument> doc;
            HRESULT hr = Windows::Foundation::ActivateInstance(
                HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(),
                &doc);
            if(FAILED(hr)) return nullptr;

            ComPtr<IXmlDocumentIO> docIo;
            if(FAILED(doc.As(&docIo))) return nullptr;

            HString xmlHs;
            xmlHs.Set(xml.c_str());
            if(FAILED(docIo->LoadXml(xmlHs.Get()))) return nullptr;

            return doc;
        }

        void hookActivation(IToastNotification *toast, const OmegaCommon::String & noteId) {
            // Activation: tap on body or on a button. The 'arguments' attribute on the
            // <toast> element is the activation argument; for buttons it's the action id.
            auto handler = Callback<ITypedEventHandler<ToastNotification *, IInspectable *>>(
                [this, noteId](IToastNotification *sender, IInspectable *args) -> HRESULT {
                    ComPtr<IToastActivatedEventArgs> a;
                    OmegaCommon::String actionArg;
                    if(args && SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&a))) && a) {
                        HString h;
                        if(SUCCEEDED(a->get_Arguments(h.GetAddressOf()))) {
                            UINT32 len = 0;
                            const wchar_t *raw = h.GetRawBuffer(&len);
                            actionArg = narrow(raw);
                        }
                    }
                    // No arguments -> body tap. Otherwise -> action button.
                    if(delegate) {
                        if(actionArg.empty()) delegate->onNoteActivated(noteId);
                        else                  delegate->onNoteAction(noteId, actionArg);
                    }
                    // Drop our retained reference now that the toast is consumed.
                    std::lock_guard<std::mutex> lk(liveMtx);
                    live.erase(noteId);
                    return S_OK;
                });
            EventRegistrationToken tok{};
            toast->add_Activated(handler.Get(), &tok);

            auto dismissHandler = Callback<ITypedEventHandler<ToastNotification *, ToastDismissedEventArgs *>>(
                [this, noteId](IToastNotification *, IToastDismissedEventArgs *) -> HRESULT {
                    std::lock_guard<std::mutex> lk(liveMtx);
                    live.erase(noteId);
                    return S_OK;
                });
            EventRegistrationToken tok2{};
            toast->add_Dismissed(dismissHandler.Get(), &tok2);
        }

    public:
        WinNotificationCenter() = default;

        void requestPermission(std::function<void(NativeNotePermission)> callback) override {
            // Win32 has no explicit user permission model for toast notifications;
            // they're governed by the system "notifications & actions" settings.
            // Treat as Authorized and fire the callback synchronously.
            cached.store(NativeNotePermission::Authorized);
            if(callback) callback(NativeNotePermission::Authorized);
        }

        NativeNotePermission currentPermission() const override {
            return cached.load();
        }

        void sendNativeNote(NativeNote & note) override {
            if(!ensureInitialized()) return;

            ComPtr<IXmlDocument> doc = buildXml(note);
            if(!doc) return;

            ComPtr<IToastNotificationFactory> toastFactory;
            HRESULT hr = Windows::Foundation::GetActivationFactory(
                HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(),
                &toastFactory);
            if(FAILED(hr)) return;

            ComPtr<IToastNotification> toast;
            if(FAILED(toastFactory->CreateToastNotification(doc.Get(), &toast))) return;

            // Stable identifier for removal. ToastNotification.Tag has a 16-char
            // limit on older OS versions; truncate but keep enough to be useful.
            OmegaCommon::String idStr = note.identifier;
            if(idStr.empty()) {
                static std::atomic<unsigned long long> seq{0};
                std::ostringstream s;
                s << "owtk-" << seq.fetch_add(1);
                idStr = s.str();
            }

            ComPtr<IToastNotification2> toast2;
            if(SUCCEEDED(toast.As(&toast2))) {
                std::wstring wid = widen(idStr);
                std::wstring wtag = wid.size() > 16 ? wid.substr(0, 16) : wid;
                HString tagHs; tagHs.Set(wtag.c_str());
                toast2->put_Tag(tagHs.Get());
                HString grpHs; grpHs.Set(L"OmegaWTK");
                toast2->put_Group(grpHs.Get());
            }

            // Keep the toast alive long enough for the Activated/Dismissed handlers.
            {
                std::lock_guard<std::mutex> lk(liveMtx);
                live[idStr] = toast;
            }
            hookActivation(toast.Get(), idStr);

            if(note.delaySeconds > 0.f) {
                // Schedule via ScheduledToastNotification.
                ComPtr<IScheduledToastNotificationFactory> schedFactory;
                hr = Windows::Foundation::GetActivationFactory(
                    HStringReference(RuntimeClass_Windows_UI_Notifications_ScheduledToastNotification).Get(),
                    &schedFactory);
                if(FAILED(hr)) return;

                FILETIME ft;
                GetSystemTimeAsFileTime(&ft);
                ULARGE_INTEGER ui; ui.LowPart = ft.dwLowDateTime; ui.HighPart = ft.dwHighDateTime;
                ui.QuadPart += (ULONGLONG)(note.delaySeconds * 1e7);  // 100ns ticks
                DateTime when{ (INT64)ui.QuadPart };

                ComPtr<IScheduledToastNotification> sched;
                if(FAILED(schedFactory->CreateScheduledToastNotification(doc.Get(), when, &sched))) return;
                notifier->AddToSchedule(sched.Get());
            } else {
                notifier->Show(toast.Get());
            }
        }

        void removeNote(const OmegaCommon::String & identifier) override {
            if(!ensureInitialized() || !mgrStatics2) return;
            ComPtr<IToastNotificationHistory> history;
            if(FAILED(mgrStatics2->get_History(&history))) return;

            std::wstring wid = widen(identifier);
            std::wstring wtag = wid.size() > 16 ? wid.substr(0, 16) : wid;
            HString tagHs; tagHs.Set(wtag.c_str());
            HString grpHs; grpHs.Set(L"OmegaWTK");
            HString aumHs; aumHs.Set(widen(aumid).c_str());
            history->RemoveGroupedTagWithId(tagHs.Get(), grpHs.Get(), aumHs.Get());

            std::lock_guard<std::mutex> lk(liveMtx);
            live.erase(identifier);
        }

        void removeAllNotes() override {
            if(!ensureInitialized() || !mgrStatics2) return;
            ComPtr<IToastNotificationHistory> history;
            if(FAILED(mgrStatics2->get_History(&history))) return;
            HString aumHs; aumHs.Set(widen(aumid).c_str());
            history->ClearWithId(aumHs.Get());

            std::lock_guard<std::mutex> lk(liveMtx);
            live.clear();
        }

        void registerCategories(const OmegaCommon::Vector<NativeNoteCategory> & cats) override {
            std::lock_guard<std::mutex> lk(categories.mtx);
            for(auto & c : cats) categories.map[c.identifier] = c;
        }

        void setDelegate(NativeNoteCenterDelegate * d) override {
            delegate = d;
        }
    };

    NNCP make_native_note_center(){
        return std::make_shared<WinNotificationCenter>();
    }

}
