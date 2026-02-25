#include "omegaWTK/Native/NativeItem.h"

#include "Layer.h"

#include <queue>
#include <cstdint>
#include <mutex>

#ifndef OMEGAWTK_COMPOSITION_COMPOSITORCLIENT_H
#define OMEGAWTK_COMPOSITION_COMPOSITORCLIENT_H

namespace OmegaWTK::Composition {
    class ViewRenderTarget;
    class Compositor;

    void InitializeEngine();
    void CleanupEngine();
    
    struct CanvasFrame;


    typedef std::chrono::time_point<std::chrono::high_resolution_clock> Timestamp;

    class OMEGAWTK_EXPORT CompositionRenderTarget {
    public:
        virtual ~CompositionRenderTarget() = default;
    };

    enum class CommandStatus : int {
        Ok,
        Failed,
        Delayed
    };

    class CompositorClient;

    struct CompositorCommand {
        unsigned id;
        CompositorClient & client;
        uint64_t syncLaneId = 0;
        uint64_t syncPacketId = 0;
        uint64_t requiredTreeEpoch = 0;
        typedef enum : int {
            /// A frame draw commmand
            Render,
            /// A view command
            View,
            /// A Layer command
            Layer,
            /// Cancel execution of commands from client.
            Cancel,
            /// Atomic group of commands from one sync lane.
            Packet
        } Type;
        Type type;
        typedef enum {
            Low,
            High
        } Priority;
        Priority priority;
        struct {
            bool hasThreshold = false;
            Timestamp timeStamp;
            Timestamp threshold;
        }thresholdParams;
        OmegaCommon::Promise<CommandStatus> status;
        CompositorCommand(unsigned id,
                        CompositorClient &client,
                        Type type,
                        Priority priority,
                        decltype(thresholdParams) thresholdParams,
                        OmegaCommon::Promise<CommandStatus> status):
        id(id),
        client(client),
        type(type),
        priority(priority),
        thresholdParams(thresholdParams),
        status(std::move(status))
        {

        };
        virtual ~CompositorCommand() = default;
    };

    struct CompositorLayerCommand : public CompositorCommand {
        class Layer *layer;
        SharedHandle<CompositionRenderTarget> parentTarget;
        enum : int {
           Resize,
           Effect
        } subtype;
        int delta_x = 0,delta_y = 0,delta_w = 0,delta_h = 0;
        SharedHandle<LayerEffect> effect;
        explicit CompositorLayerCommand(unsigned id,
                                        CompositorClient &client,
                                        Type type,
                                        Priority priority,
                                        decltype(CompositorCommand::thresholdParams) thresholdParams,
                                        OmegaCommon::Promise<CommandStatus> status,
                                        class Layer *layer,
                                        SharedHandle<CompositionRenderTarget> & parentTarget,
                                        SharedHandle<LayerEffect> & effect):CompositorCommand(id,client,type,priority,thresholdParams,std::move(status)),
                                        subtype(Effect),
                                        layer(layer),
                                        parentTarget(parentTarget),
                                        effect(effect){

        };
        explicit CompositorLayerCommand(unsigned id,
                                    CompositorClient &client,
                                    Type type,
                                    Priority priority,
                                    decltype(CompositorCommand::thresholdParams) thresholdParams,
                                    OmegaCommon::Promise<CommandStatus> status,
                                    class Layer *layer,
                                    SharedHandle<CompositionRenderTarget> parentTarget,
                                    int delta_x,
                                    int delta_y,
                                    int delta_w,
                                    int delta_h): CompositorCommand(id,client,type,priority,thresholdParams,std::move(status)),
                                    subtype(Resize),
                                    layer(layer),
                                    parentTarget(parentTarget),
                                    delta_x(delta_x),
                                    delta_y(delta_y),
                                    delta_w(delta_w),
                                    delta_h(delta_h){

                                    };
        ~CompositorLayerCommand() override = default;
    };

   struct CompositionRenderCommand : public CompositorCommand {
       SharedHandle<CompositionRenderTarget> renderTarget;
       SharedHandle<CanvasFrame> frame;
       explicit CompositionRenderCommand(unsigned id,
                        CompositorClient &client,
                        Type type,
                        Priority priority,
                        decltype(thresholdParams) thresholdParams,
                        OmegaCommon::Promise<CommandStatus> status,
                        SharedHandle<CompositionRenderTarget> renderTarget,
                        SharedHandle<CanvasFrame> frame):CompositorCommand(id,client,type,priority,thresholdParams,std::move(status)),
                        renderTarget(renderTarget),
                        frame(frame){

                        }
       ~CompositionRenderCommand() override = default;
   };

   struct CompositorViewCommand : public CompositorCommand {
       typedef enum : int {
           Resize,
       } CommandType;
       CommandType subType;
       Native::NativeItemPtr viewPtr;
       int delta_x = 0,delta_y = 0,delta_w = 0,delta_h = 0;
       CompositorViewCommand(unsigned id,
                        CompositorClient &client,
                        Type type,
                        Priority priority,
                        decltype(thresholdParams) thresholdParams,
                        OmegaCommon::Promise<CommandStatus> status,
                        CommandType subType,
                        Native::NativeItemPtr viewPtr,
                        int delta_x,
                        int delta_y,
                        int delta_w,
                        int delta_h):CompositorCommand(id,client,type,priority,thresholdParams,std::move(status)),
                        subType(subType),
                        viewPtr(viewPtr),
                        delta_x(delta_x),
                        delta_y(delta_y),
                        delta_w(delta_w),
                        delta_h(delta_h)
                        {

                        };
       ~CompositorViewCommand() override = default;
   };

   struct CompositorCancelCommand : public CompositorCommand {
       unsigned startID = 0,endID = 0;
       CompositorCancelCommand(unsigned id,
                        CompositorClient &client,
                        Type type,
                        Priority priority,
                        decltype(thresholdParams) thresholdParams,
                        OmegaCommon::Promise<CommandStatus> status,unsigned startID,unsigned endID):CompositorCommand(id,client,type,priority,thresholdParams,std::move(status)),
                        startID(startID),
                        endID(endID){

                        };
       ~CompositorCancelCommand() override = default;
   };

    struct CompositorPacketCommand : public CompositorCommand {
        OmegaCommon::Vector<SharedHandle<CompositorCommand>> commands;
        explicit CompositorPacketCommand(unsigned id,
                                         CompositorClient &client,
                                         Priority priority,
                                         decltype(CompositorCommand::thresholdParams) thresholdParams,
                                         OmegaCommon::Promise<CommandStatus> status,
                                         OmegaCommon::Vector<SharedHandle<CompositorCommand>> && commands):
                                         CompositorCommand(id,client,CompositorCommand::Type::Packet,priority,thresholdParams,std::move(status)),
                                         commands(std::move(commands)){

        };
        ~CompositorPacketCommand() override = default;
    };



    /** @brief Compositor Client Proxy class for interaction with a Compositor
        @paragraph Interaction includes submitting render commands to a Compositor,
        and verifying successful frame completion. 
    */
    class OMEGAWTK_EXPORT CompositorClientProxy {
        friend class CompositorClient;

        Compositor *frontend = nullptr;

        unsigned recordDepth = 0;

        SharedHandle<CompositionRenderTarget> renderTarget;

        std::queue<SharedHandle<CompositorCommand>> commandQueue;
        uint64_t syncLaneId = 0;
        // Reserved packet id used by preview paths (animations) so
        // the id returned by peekNextPacketId() matches the next submit().
        mutable uint64_t reservedPacketId = 0;
        mutable std::mutex commandMutex;

        OmegaCommon::Async<CommandStatus> queueTimedFrame(unsigned & id,CompositorClient &client,
                                                          SharedHandle<CanvasFrame> & frame,
                                                          Timestamp & start,
                                                          Timestamp & deadline);

        OmegaCommon::Async<CommandStatus> queueFrame(unsigned & id,CompositorClient &client,
                                                     SharedHandle<CanvasFrame> & frame,
                                                     Timestamp & start);

        OmegaCommon::Async<CommandStatus> queueLayerResizeCommand(unsigned & id,CompositorClient &client,
                                                                  Layer *target,
                                                                  int delta_x,
                                                                  int delta_y,
                                                                  int delta_w,
                                                                  int delta_h,
                                                                  Timestamp &start,
                                                                  Timestamp & deadline);

        OmegaCommon::Async<CommandStatus> queueLayerEffectCommand(unsigned & id,CompositorClient &client,
                                                                  Layer *target,
                                                                  SharedHandle<LayerEffect> & effect,
                                                                  Timestamp &start,
                                                                  Timestamp & deadline);

        OmegaCommon::Async<CommandStatus> queueViewResizeCommand(unsigned & id,CompositorClient &client,
                                                                 Native::NativeItemPtr nativeView,
                                                                 int delta_x,
                                                                 int delta_y,
                                                                 int delta_w,
                                                                 int delta_h,
                                                                 Timestamp &start,
                                                                 Timestamp & deadline);

        OmegaCommon::Async<CommandStatus> queueCancelCommand(unsigned & id,CompositorClient &client,unsigned startID,unsigned endID);

    protected:
        void submit();
    public:
        explicit CompositorClientProxy(SharedHandle<CompositionRenderTarget> renderTarget);
        void setSyncLaneId(uint64_t syncLaneId);
        uint64_t getSyncLaneId() const;
        uint64_t peekNextPacketId() const;
        Compositor *getFrontendPtr() const;
        bool isRecording() const;
        void setFrontendPtr(Compositor *frontend);
        void beginRecord();
        void endRecord();
        virtual ~CompositorClientProxy() = default;
    };

    struct ActiveCommandEntry {
        unsigned id;
        OmegaCommon::Async<CommandStatus> status;
    };


    class OMEGAWTK_EXPORT CompositorClient {
        CompositorClientProxy & parentProxy;

        OmegaCommon::Vector<ActiveCommandEntry> currentJobStatuses;

        unsigned currentCommandID = 0;

    protected:
        void pushTimedFrame(SharedHandle<CanvasFrame> & frame,Timestamp & start,Timestamp & deadline);
        void pushFrame(SharedHandle<CanvasFrame> & frame,Timestamp & start);
        void pushLayerResizeCommand(Layer *target,int delta_x,int delta_y,int delta_w,int delta_h,Timestamp &start,Timestamp & deadline);
        void pushLayerEffectCommand(Layer *target,SharedHandle<LayerEffect> & effect,Timestamp &start,Timestamp & deadline);
        void pushViewResizeCommand(Native::NativeItemPtr nativeView,int delta_x,int delta_y,int delta_w,int delta_h,Timestamp &start,Timestamp & deadline);
        void cancelCurrentJobs();
    public:
        bool busy();

        explicit CompositorClient(CompositorClientProxy & proxy);
        virtual ~CompositorClient() = default;
    };

    

    /**
     The Compositor's interface for composing to a widget's view.
     */
    class OMEGAWTK_EXPORT ViewRenderTarget : public CompositionRenderTarget {
        Native::NativeItemPtr native;
    public:
        Native::NativeItemPtr getNativePtr();
        explicit ViewRenderTarget(Native::NativeItemPtr _native);
        ~ViewRenderTarget() override;
    };

    
};


#endif
