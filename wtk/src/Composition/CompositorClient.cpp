#include "omegaWTK/Composition/CompositorClient.h"
#include "Compositor.h"

#include <atomic>

namespace OmegaWTK::Composition {
    
    // CompositorClient::CompositorClient(){

    // };

    namespace {
        std::atomic<uint64_t> g_syncLaneSeed {1};
    }

    CompositorClientProxy::CompositorClientProxy(SharedHandle<CompositionRenderTarget> renderTarget):
    renderTarget(renderTarget),
    syncLaneId(g_syncLaneSeed.fetch_add(1)) {

    }

    void CompositorClientProxy::setSyncLaneId(uint64_t syncLaneId){
        std::lock_guard<std::mutex> lk(commandMutex);
        this->syncLaneId = syncLaneId;
    }

    uint64_t CompositorClientProxy::getSyncLaneId() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return syncLaneId;
    }

    uint64_t CompositorClientProxy::peekNextPacketId() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return nextPacketId;
    }

    Compositor *CompositorClientProxy::getFrontendPtr() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return frontend;
    }

    bool CompositorClientProxy::isRecording() const {
        std::lock_guard<std::mutex> lk(commandMutex);
        return recording;
    }

    void CompositorClientProxy::beginRecord() {
        std::lock_guard<std::mutex> lk(commandMutex);
        if(recording){
            return;
        }
        recording = true;
    }

    void CompositorClientProxy::endRecord() {
        {
            std::lock_guard<std::mutex> lk(commandMutex);
            if(!recording){
                return;
            }
            recording = false;
        }
        submit();
    }

   OmegaCommon::Async<CommandStatus> CompositorClientProxy::queueTimedFrame(unsigned & id,
                                                                            CompositorClient & client,
                                                                            SharedHandle<CanvasFrame> &frame,
                                                                            Timestamp &start,
                                                                            Timestamp &deadline){
       OmegaCommon::Promise<CommandStatus> status;
       auto async = status.async();
       std::lock_guard<std::mutex> lk(commandMutex);
        commandQueue.emplace(new
                        CompositionRenderCommand(id,client,CompositorCommand::Type::Render,
                                 CompositorCommand::Priority::High,
                                 {true,start,deadline},std::move(status),renderTarget,frame));
        return async;
    };

    OmegaCommon::Async<CommandStatus> CompositorClientProxy::queueFrame(unsigned & id,
                                           CompositorClient & client,
                                           SharedHandle<CanvasFrame> & frame,
                                           Timestamp &start){
        OmegaCommon::Promise<CommandStatus> status;
        auto async = status.async();
        std::lock_guard<std::mutex> lk(commandMutex);
        commandQueue.emplace(
            new
            CompositionRenderCommand{
                                id,client,CompositorCommand::Type::Render,
                                 CompositorCommand::Priority::Low,
                                 {false,start,start},std::move(status),std::static_pointer_cast<CompositionRenderTarget>(renderTarget),frame});
        return async;
    };

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

    OmegaCommon::Async<CommandStatus> CompositorClientProxy::queueCancelCommand(unsigned & id,
                                                                                CompositorClient & client,
                                                                                unsigned startID,
                                                                                unsigned endID) {
        Timestamp ts = std::chrono::high_resolution_clock::now();
        OmegaCommon::Promise<CommandStatus> status;
        auto async = status.async();
        std::lock_guard<std::mutex> lk(commandMutex);
        commandQueue.emplace(new CompositorCancelCommand {
            id,
            client,
            CompositorCommand::Type::Cancel,
            CompositorCommand::Priority::High,
            {false,ts,ts},std::move(status),startID,endID});
        return async;
    }

    void CompositorClientProxy::setFrontendPtr(Compositor *frontend){
        std::lock_guard<std::mutex> lk(commandMutex);
        this->frontend = frontend;
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
               while(!commandQueue.empty()){
                   auto comm = commandQueue.front();
                   commandQueue.pop();
                   comm->status.set(CommandStatus::Failed);
               }
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
           packetId = nextPacketId++;
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
    }

    void CompositorClient::pushTimedFrame(SharedHandle<CanvasFrame> &frame, Timestamp &start, Timestamp &deadline) {
        busy();
        currentJobStatuses.push_back(
            {currentCommandID,
            parentProxy.queueTimedFrame(currentCommandID,*this,frame,start,deadline)
            });
        ++currentCommandID;
    }

    void CompositorClient::pushViewResizeCommand(Native::NativeItemPtr nativeView,int delta_x,int delta_y,int delta_w,int delta_h,Timestamp &start,Timestamp & deadline){
        busy();
        currentJobStatuses.push_back(
            {currentCommandID,
            parentProxy.queueViewResizeCommand(currentCommandID,*this,nativeView,delta_x,delta_y,delta_w,delta_h,start,deadline)
            });
        ++currentCommandID;
    }

    void CompositorClient::pushFrame(SharedHandle<CanvasFrame> &frame, Timestamp &start) {
        busy();
        currentJobStatuses.push_back({currentCommandID,
                                    parentProxy.queueFrame(currentCommandID,*this,frame,start)
                                                    });
        ++currentCommandID;
    }

    void CompositorClient::cancelCurrentJobs() {
        if(busy()) {
            auto idStart = currentJobStatuses.front().id;
            auto idEnd = currentJobStatuses.back().id;
            currentJobStatuses.push_back({currentCommandID,
                                            parentProxy.queueCancelCommand(currentCommandID, *this, idStart,idEnd)
                                            });
            ++currentCommandID;
        }
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

////ViewRenderTargetFrameScheduler::ViewRenderTargetFrameScheduler(Composition::Compositor * comp):
////compositor(comp){
////    
////};
//
//Core::UniquePtr<ViewRenderTargetFrameScheduler> ViewRenderTargetFrameScheduler::Create(Core::UniquePtr<ViewRenderTarget> & ptr,Compositor * comp){
//    
//};
}
