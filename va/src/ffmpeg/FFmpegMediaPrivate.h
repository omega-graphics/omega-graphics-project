#ifndef OMEGAVA_FFMPEG_FFMPEGMEDIAPRIVATE_H
#define OMEGAVA_FFMPEG_FFMPEGMEDIAPRIVATE_H

#include "omegaVA/Core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace OmegaVA {

    // The public header forward-declares `class AudioPlaybackDevice`
    // but doesn't define it; the concrete shape is per-backend. On
    // Linux it carries the ALSA PCM hint string that the playback
    // session passes to snd_pcm_open. Both the capture session (audio
    // preview routes through a playback device) and the playback
    // sessions need this — owning it in a small shared header keeps
    // the two TUs from accidentally ODR-conflicting.
    class AudioPlaybackDevice {
    public:
        OmegaCommon::String name;       ///< Human-readable
        OmegaCommon::String alsaName;   ///< Passed to snd_pcm_open
        AudioPlaybackDevice() = default;
        AudioPlaybackDevice(OmegaCommon::String n, OmegaCommon::String alsa)
            : name(std::move(n)), alsaName(std::move(alsa)) {}
        virtual ~AudioPlaybackDevice() = default;
    };

    // Generic ticking-clients dispatch queue. Sessions register a
    // callback that the worker thread calls at ~5 ms intervals while
    // the client is active. Defined here (not in the playback TU) so
    // the capture TU's preview path could later piggy-back without
    // duplicating the class.
    class PlaybackDispatchQueue {
    public:
        struct Client {
            std::function<void()> tick;
            std::atomic<bool> active{false};
            std::atomic<bool> remove{false};
        };

        PlaybackDispatchQueue() {
            worker = std::thread([this]{ runLoop(); });
        }
        ~PlaybackDispatchQueue() {
            {
                std::lock_guard<std::mutex> g(mtx);
                stopping = true;
            }
            cv.notify_all();
            if(worker.joinable()) worker.join();
        }

        std::shared_ptr<Client> registerClient(std::function<void()> tick) {
            auto c = std::make_shared<Client>();
            c->tick = std::move(tick);
            {
                std::lock_guard<std::mutex> g(mtx);
                clients.push_back(c);
            }
            cv.notify_all();
            return c;
        }
        void unregisterClient(const std::shared_ptr<Client> & c) {
            if(!c) return;
            c->remove.store(true);
            cv.notify_all();
        }

    private:
        std::thread worker;
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<std::shared_ptr<Client>> clients;
        bool stopping = false;

        void runLoop() {
            while(true) {
                std::vector<std::shared_ptr<Client>> snapshot;
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    cv.wait_for(lk, std::chrono::milliseconds(5), [&]{
                        if(stopping) return true;
                        for(auto & c : clients) if(c->active.load() || c->remove.load()) return true;
                        return false;
                    });
                    if(stopping) return;
                    clients.erase(std::remove_if(clients.begin(), clients.end(),
                        [](const std::shared_ptr<Client> & c){ return c->remove.load(); }),
                        clients.end());
                    snapshot = clients;
                }
                for(auto & c : snapshot) {
                    if(c->active.load()) c->tick();
                }
            }
        }
    };

} // namespace OmegaVA

#endif
