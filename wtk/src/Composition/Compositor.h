#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Composition/Layer.h"
#include "omegaWTK/Composition/CompositorClient.h"

#include <chrono>
#include <condition_variable>
#include <thread>

#include "backend/RenderTarget.h"

#ifndef OMEGAWTK_COMPOSTION_COMPOSITOR_H
#define OMEGAWTK_COMPOSTION_COMPOSITOR_H

namespace OmegaWTK::Composition {
    // template<class FnTy>
    // class WorkerFarm {    
    //     struct ThreadStatus {
    //         bool busy;
    //     };
    //     std::map<std::thread,ThreadStatus> threadsInFlight;
    //     void _makeThread(FnTy fn){
    //         threadsInFlight.insert(std::make_pair([&]{
    //             fn();
    //         },{false}));
    //     }
    // public:
    //     WorkerFarm<FnTy>(int startThreads){

    //     };
    //     void assign(FnTy fn){

    //     };
    // };
   
   class CompositorScheduler {
       Compositor *compositor;
   public:
        bool shutdown;

        void shutdownAndJoin();
        void processCommand(SharedHandle<CompositorCommand> & command);
        std::thread t;

       explicit CompositorScheduler(Compositor *compositor);
       ~CompositorScheduler();
   };


   struct CompareCommands {
        auto operator()(SharedHandle<Composition::CompositorCommand> & lhs,
                        SharedHandle<Composition::CompositorCommand> & rhs){
        if(lhs->type == CompositorCommand::View && rhs->type != CompositorCommand::View) return true;
           if(rhs->type == CompositorCommand::View && lhs->type != CompositorCommand::View) return false;

           // Cancel next
           if(lhs->type == CompositorCommand::Cancel && rhs->type != CompositorCommand::Cancel) return true;
           if(rhs->type == CompositorCommand::Cancel && lhs->type != CompositorCommand::Cancel) return false;


            else if(lhs->type == CompositorCommand::Render && rhs->type == CompositorCommand::Render) {
                auto _lhs = std::dynamic_pointer_cast<CompositionRenderCommand>(lhs);
                auto _rhs = std::dynamic_pointer_cast<CompositionRenderCommand>(rhs);
               if(_lhs->thresholdParams.timeStamp != _rhs->thresholdParams.timeStamp)
                   return _lhs->thresholdParams.timeStamp < _rhs->thresholdParams.timeStamp;
               // tie-breaker: presence of explicit threshold
               if(_lhs->thresholdParams.hasThreshold != _rhs->thresholdParams.hasThreshold)
                   return _lhs->thresholdParams.hasThreshold; // true first
               // final tie-breaker: id
               return lhs->id < rhs->id;
            }
            // Fallback deterministic ordering: compare types
           return static_cast<int>(lhs->type) < static_cast<int>(rhs->type);
        };
    };

   
    /**
     OmegaWTK's Composition Engine Frontend Interface
     */
    class Compositor {

        OmegaCommon::Vector<LayerTree *> targetLayerTrees;

        RenderTargetStore renderTargetStore;

        std::mutex mutex;

        bool queueIsReady;

        std::condition_variable queueCondition;
        OmegaCommon::PriorityQueueHeap<SharedHandle<CompositorCommand>,CompareCommands> commandQueue;

        friend class CompositorClientProxy;
        friend class CompositorScheduler;

        SharedHandle<CompositorCommand> currentCommand;

        bool allowUpdates = false;

        CompositorScheduler scheduler;

        friend class Layer;
        friend class LayerTree;
        friend class WindowLayer;

        void executeCurrentCommand();

    public:
        void scheduleCommand(SharedHandle<CompositorCommand> & command);
        
        
        Compositor();
        ~Compositor();
    };

    struct BackendExecutionContext {

    };
}

#endif
