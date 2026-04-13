#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/CompositeFrame.h"
#include "omegaWTK/Composition/Canvas.h"
#include "Compositor.h"

#include <atomic>

namespace OmegaWTK::Composition {

    namespace {
        std::atomic<uint64_t> g_syncLaneSeed {1};
        std::atomic<uint64_t> g_syncPacketSeed {1};

        inline uint64_t allocateGlobalPacketId(){
            return g_syncPacketSeed.fetch_add(1,std::memory_order_relaxed);
        }
    }

    CompositorClientProxy::CompositorClientProxy(SharedHandle<CompositionRenderTarget> renderTarget):
    renderTarget(renderTarget),
    syncLaneId(g_syncLaneSeed.fetch_add(1)) {

    }

    CompositorClientProxy::CompositorClientProxy():
    renderTarget(nullptr),
    syncLaneId(g_syncLaneSeed.fetch_add(1)) {

    }

    void CompositorClientProxy::setRenderTarget(SharedHandle<CompositionRenderTarget> newRenderTarget){
        std::lock_guard<std::mutex> lk(commandMutex);
        renderTarget = std::move(newRenderTarget);
    }

    void CompositorClientProxy::setSyncLaneId(uint64_t syncLaneId){
        std::lock_guard<std::mutex> lk(commandMutex);
        this->syncLaneId = syncLaneId;
    }

    uint64_t CompositorClientProxy::getSyncLaneId() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return syncLaneId;
    }

    SyncLaneDiagnostics CompositorClientProxy::getSyncLaneDiagnostics() const {
        SyncLaneDiagnostics diagnostics {};
        std::lock_guard<std::mutex> lk(commandMutex);
        diagnostics.syncLaneId = syncLaneId;
        return diagnostics;
    }

    Compositor *CompositorClientProxy::getFrontendPtr() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return frontend;
    }

    void CompositorClientProxy::setActiveCompositeFrame(CompositeFrame *frame){
        activeCompositeFrame_ = frame;
    }

    OmegaCommon::Async<CommandStatus> CompositorClientProxy::queueViewResizeCommand(unsigned & id,
                                                       CompositorClient & client,
                                                       Native::NativeItemPtr nativeView,
                                                       int delta_x,
                                                       int delta_y,
                                                       int delta_w,
                                                       int delta_h,
                                                       Timestamp &start,
                                                       Timestamp &deadline) {
        OmegaCommon::Promise<CommandStatus> status;
        auto async = status.async();
        std::lock_guard<std::mutex> lk(commandMutex);
        commandQueue.emplace(new CompositorViewCommand{
            id,
            client,
            CompositorCommand::Type::View,
            CompositorCommand::Priority::High,
            {true,start,deadline},
            std::move(status),
            CompositorViewCommand::Resize,nativeView,delta_x,delta_y,delta_w,delta_h});
        return async;
    }

    OmegaCommon::Async<CommandStatus> CompositorClientProxy::queueLayerResizeCommand(unsigned & id,
                                                                                     CompositorClient & client,
                                                                                     Layer *target,
                                                                                     int delta_x,
                                                                                     int delta_y,
                                                                                     int delta_w,
                                                                                     int delta_h,
                                                                                     Timestamp &start,
                                                                                     Timestamp &deadline) {
        OmegaCommon::Promise<CommandStatus> status;
        auto async = status.async();
        std::lock_guard<std::mutex> lk(commandMutex);

        commandQueue.emplace(new CompositorLayerCommand {
            id,
            client,
            CompositorCommand::Type::Layer,
            CompositorCommand::Priority::High,
            {true,start,deadline},std::move(status),target,renderTarget,delta_x,delta_y,delta_w,delta_h});
        return async;
    }

    OmegaCommon::Async<CommandStatus>
    CompositorClientProxy::queueLayerEffectCommand(unsigned int &id, CompositorClient &client, Layer *target,
                                                   SharedHandle<LayerEffect> &effect, Timestamp &start,
                                                   Timestamp &deadline) {
        OmegaCommon::Promise<CommandStatus> status;
        auto async = status.async();
        std::lock_guard<std::mutex> lk(commandMutex);

        commandQueue.emplace(new CompositorLayerCommand {
                id,
                client,
                CompositorCommand::Type::Layer,
                CompositorCommand::Priority::High,
                {true,start,deadline},std::move(status),target,renderTarget,effect});
        return async;
    }

    void CompositorClientProxy::setFrontendPtr(Compositor *frontend){
        bool shouldFlush = false;
        {
            std::lock_guard<std::mutex> lk(commandMutex);
            this->frontend = frontend;
            shouldFlush = (this->frontend != nullptr && !commandQueue.empty());
        }
        if(shouldFlush){
            submit();
        }
    };

    void CompositorClientProxy::submit(){
       Compositor *targetFrontend = nullptr;
       uint64_t laneId = 0;
       uint64_t packetId = 0;
       OmegaCommon::Vector<SharedHandle<CompositorCommand>> packetCommands {};
       {
           std::lock_guard<std::mutex> lk(commandMutex);
           targetFrontend = frontend;
           if(targetFrontend == nullptr){
               return;
           }
           laneId = syncLaneId;
           while(!commandQueue.empty()){
               auto comm = commandQueue.front();
               commandQueue.pop();
               if(comm != nullptr){
                   packetCommands.push_back(comm);
               }
           }
           if(packetCommands.empty()){
               return;
           }
           packetId = allocateGlobalPacketId();
           for(auto & packetCommand : packetCommands){
               if(packetCommand != nullptr){
                   packetCommand->syncLaneId = laneId;
                   packetCommand->syncPacketId = packetId;
               }
           }
       }

       if(packetCommands.size() == 1){
           auto command = packetCommands.front();
           targetFrontend->scheduleCommand(command);
           return;
       }

       auto firstCommand = packetCommands.front();
       if(firstCommand == nullptr){
           return;
       }
       OmegaCommon::Promise<CommandStatus> packetStatus;
       auto packet = SharedHandle<CompositorCommand>(new CompositorPacketCommand{
               firstCommand->id,
               firstCommand->client,
               firstCommand->priority,
               firstCommand->thresholdParams,
               std::move(packetStatus),
               std::move(packetCommands)
       });
       packet->syncLaneId = laneId;
       packet->syncPacketId = packetId;
       targetFrontend->scheduleCommand(packet);
    };

    CompositorClient::CompositorClient(CompositorClientProxy &proxy):
    parentProxy(proxy),
    currentCommandID(0){

    }

    void CompositorClient::pushLayerResizeCommand(Layer *target, int delta_x, int delta_y,
                                                  int delta_w, int delta_h, Timestamp &start,
                                                  Timestamp &deadline) {
        busy();
        currentJobStatuses.push_back({
            currentCommandID,
            parentProxy.queueLayerResizeCommand(currentCommandID,*this,target,delta_x,delta_y,delta_w,delta_h,start,deadline)
            });
        ++currentCommandID;
        parentProxy.submit();
    }

    void CompositorClient::pushLayerEffectCommand(Layer *target,
                                                  SharedHandle<LayerEffect> &effect,
                                                  Timestamp &start,
                                                  Timestamp &deadline) {
        busy();
        currentJobStatuses.push_back({
            currentCommandID,
            parentProxy.queueLayerEffectCommand(currentCommandID,*this,target,effect,start,deadline)
        });
        ++currentCommandID;
        parentProxy.submit();
    }

    void CompositorClient::pushViewResizeCommand(Native::NativeItemPtr nativeView,int delta_x,int delta_y,int delta_w,int delta_h,Timestamp &start,Timestamp & deadline){
        busy();
        currentJobStatuses.push_back(
            {currentCommandID,
            parentProxy.queueViewResizeCommand(currentCommandID,*this,nativeView,delta_x,delta_y,delta_w,delta_h,start,deadline)
            });
        ++currentCommandID;
        parentProxy.submit();
    }

    void CompositorClient::pushFrame(SharedHandle<CanvasFrame> &frame, Timestamp &start) {
        if(parentProxy.activeCompositeFrame_ != nullptr && frame != nullptr){
            CompositeFrame::WidgetSlice slice;
            slice.bounds = frame->rect;
            slice.windowOffset = frame->windowOffset;
            slice.commands = frame->currentVisuals;
            slice.effects = frame->currentEffects;
            slice.background = {frame->background.r,
                                frame->background.g,
                                frame->background.b,
                                frame->background.a};
            slice.targetLayer = frame->targetLayer;
            parentProxy.activeCompositeFrame_->slices.push_back(std::move(slice));
            return;
        }
        // Without an active CompositeFrame the frame has no destination.
        // The legacy queue-based render path was removed in Tier 1 Phase B.
    }

    bool CompositorClient::busy() {
        OmegaCommon::Vector<unsigned> jobsToDelete;
        for(unsigned i = 0;i < currentJobStatuses.size();i++){
            auto & job = currentJobStatuses[i];
            if(job.status.ready()){
                jobsToDelete.push_back(i);
            }
        }

        for(auto it = jobsToDelete.rbegin(); it != jobsToDelete.rend(); ++it){
            currentJobStatuses.erase(currentJobStatuses.begin() + *it);
        }

        return !currentJobStatuses.empty();
    }

    ViewRenderTarget::ViewRenderTarget(Native::NativeItemPtr _native) : native(_native){};
    Native::NativeItemPtr ViewRenderTarget::getNativePtr(){ return native;};
    ViewRenderTarget::~ViewRenderTarget(){};

}
