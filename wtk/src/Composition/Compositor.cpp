#include "Compositor.h"
#include "omegaWTK/Composition/Layer.h"
#include <chrono>
#include <mutex>

#if defined(TARGET_MACOS)
#include <dispatch/dispatch.h>
#include <pthread.h>
#endif

namespace OmegaWTK::Composition {

//void Compositor::hasDetached(LayerTree *tree){
//    for(auto it = targetLayerTrees.begin();it != targetLayerTrees.end();it++){
//        if(tree == *it){
//            targetLayerTrees.erase(it);
//            renderTargetStore.cleanTreeTargets(tree);
//            tree->removeObserver(this);
//            break;
//        }
//    }
//};

void CompositorScheduler::processCommand(SharedHandle<CompositorCommand> & command ){
    auto _now = std::chrono::high_resolution_clock::now();
    std::cout << "Processing Command:" << command->id << std::endl;
    auto executeCurrentCommand = [&](){
#if defined(TARGET_MACOS)
        if(pthread_main_np() != 0){
            compositor->executeCurrentCommand();
        }
        else {
            dispatch_sync_f(dispatch_get_main_queue(),compositor,[](void *ctx){
                ((Compositor *)ctx)->executeCurrentCommand();
            });
        }
#else
        compositor->executeCurrentCommand();
#endif
    };

    if(command->thresholdParams.hasThreshold) {
        if(command->thresholdParams.threshold >= _now){
            /// Command will execute on time.
            std::unique_lock<std::mutex> lk(compositor->mutex);
            compositor->queueCondition.wait_until(lk,command->thresholdParams.threshold,[&]{
                return shutdown;
            });
            if(shutdown){
                command->status.set(CommandStatus::Failed);
                return;
            }
            lk.unlock();
            
            executeCurrentCommand();
            
        }
        else {
            // Command is late!!
            executeCurrentCommand();
        };
    }
    else {
        /// Command will be executed right away.
        executeCurrentCommand();
    }
};




CompositorScheduler::CompositorScheduler(Compositor * compositor):compositor(compositor),shutdown(false),t([this](Compositor *compositor){
//        std::cout << "--> Starting Up" << std::endl;
        while(true){
            SharedHandle<CompositorCommand> command;
            {
                std::unique_lock<std::mutex> lk(compositor->mutex);
                compositor->queueCondition.wait(lk,[&]{
                    return shutdown || !compositor->commandQueue.empty();
                });
                if(shutdown){
                    while(!compositor->commandQueue.empty()){
                        auto pending = compositor->commandQueue.first();
                        compositor->commandQueue.pop();
                        pending->status.set(CommandStatus::Failed);
                    }
                    break;
                }
                command = compositor->commandQueue.first();
                compositor->commandQueue.pop();
                compositor->currentCommand = command;
            }
            processCommand(command);
        };

        {
            std::lock_guard<std::mutex> lk(compositor->mutex);
            if(!compositor->commandQueue.empty()){
                std::cout << "--> Unfinished Jobs:" << compositor->commandQueue.length() << std::endl;
            };
        }
        
        std::cout << "--> Shutting Down" << std::endl;
    },compositor){

};

void CompositorScheduler::shutdownAndJoin(){
    {
        std::lock_guard<std::mutex> lk(compositor->mutex); 
        shutdown = true;
    }
    compositor->queueCondition.notify_all();
    if(t.joinable() && t.get_id() != std::this_thread::get_id()){
        t.join();
    }
}

CompositorScheduler::~CompositorScheduler(){
    std::cout << "close" << std::endl;
    shutdownAndJoin();
};


Compositor::Compositor():queueIsReady(false),queueCondition(),commandQueue(200),scheduler(this){
    
};

Compositor::~Compositor(){
     scheduler.shutdownAndJoin();
     std::cout << "~Compositor()" << std::endl;
};

void Compositor::scheduleCommand(SharedHandle<CompositorCommand> & command){
    {
        std::lock_guard<std::mutex> lk(mutex);
        commandQueue.push(std::move(command));
    }
    queueCondition.notify_one();
};


};
