/**
 @file View.h
 
 */

#include "omegaWTK/Composition/CompositorClient.h"
#include "omegaWTK/Composition/Canvas.h"
#include "omegaWTK/Native/NativeEvent.h"
#include "omegaWTK/Native/NativeItem.h"
#include "omegaWTK/Native/NativeApp.h"

#include "omegaWTK/Core/XML.h"
#include "omegaWTK/Media/Video.h"
#include "omegaWTK/Media/MediaPlaybackSession.h"
#include <cstdint>
#include <limits>

#ifndef OMEGAWTK_UI_VIEW_H
#define OMEGAWTK_UI_VIEW_H

namespace OmegaWTK {
    namespace Composition {
        class ViewAnimator;
        class Font;
        class TextRect;
        struct PreCreatedVisualTreeData;
    }

    namespace Native {
        class NativeEvent;
        typedef SharedHandle<NativeEvent> NativeEventPtr;
    }

    
    class Widget;
    class AppInst;
    class ViewDelegate;
    class ScrollView;
    class View;
    OMEGACOMMON_SHARED_CLASS(View);

    struct OMEGAWTK_EXPORT ResizeClamp {
        float minWidth = 1.f;
        float minHeight = 1.f;
        float maxWidth = std::numeric_limits<float>::infinity();
        float maxHeight = std::numeric_limits<float>::infinity();
    };

    enum class OMEGAWTK_EXPORT ChildResizePolicy : std::uint8_t {
        Fixed,
        Fill,
        FitContent,
        Proportional
    };

    struct OMEGAWTK_EXPORT ChildResizeSpec {
        bool resizable = true;
        ChildResizePolicy policy = ChildResizePolicy::FitContent;
        ResizeClamp clamp {};
        float growWeightX = 1.f;
        float growWeightY = 1.f;
    };

    class OMEGAWTK_EXPORT ViewResizeCoordinator {
        struct ChildState {
            ChildResizeSpec spec {};
            Core::Rect baselineParentRect {Core::Position{0.f,0.f},1.f,1.f};
            Core::Rect baselineChildRect {Core::Position{0.f,0.f},1.f,1.f};
            bool hasBaseline = false;
        };
        View * parentView = nullptr;
        std::uint64_t activeSessionId = 0;
        OmegaCommon::Map<View *,ChildState> childState;
    public:
        void attachView(View * parent);
        void registerChild(View * child,const ChildResizeSpec & spec);
        void updateChildSpec(View * child,const ChildResizeSpec & spec);
        void unregisterChild(View * child);
        void beginResizeSession(std::uint64_t sessionId);
        Core::Rect resolveChildRect(View * child,
                                    const Core::Rect & requested,
                                    const Core::Rect & parentContentRect);
        void resolve(const Core::Rect & parentContentRect);
        static Core::Rect clampRectToParent(const Core::Rect & requested,
                                            const Core::Rect & parentContentRect,
                                            const ChildResizeSpec & spec);
    };
    

    /**
        @brief Controls all the basic functionality of a Widget!
        Sometimes referred to as the CanvasView.
        @relates Widget
     */ 
    class OMEGAWTK_EXPORT View : public Native::NativeEventEmitter {
        OmegaCommon::Vector<View *> subviews;
    protected:
        SharedHandle<Composition::ViewRenderTarget> renderTarget;
        Composition::CompositorClientProxy & compositorProxy(){ return proxy; }
        const Composition::CompositorClientProxy & compositorProxy() const { return proxy; }
        friend class Widget;
    private:
        Composition::CompositorClientProxy proxy;
        void setFrontendRecurse(Composition::Compositor *frontend);
        void setSyncLaneRecurse(uint64_t syncLaneId);
        ViewResizeCoordinator resizeCoordinator;
        SharedHandle<Composition::LayerTree> ownLayerTree;
        View *parent_ptr = nullptr;
        Core::Rect rect {Core::Position{0.f,0.f},1.f,1.f};
        ViewDelegate *delegate = nullptr;
        virtual bool hasDelegate();
        void addSubView(View *view);
        void removeSubView(View * view);
        Core::UniquePtr<Composition::PreCreatedVisualTreeData> preCreatedVisualTree_;
        void preCreateVisualResources();
        friend class AppWindow;
        friend class Composition::ViewAnimator;
        friend class ScrollView;
        friend class Widget;
    protected:
//        /**
//            Constructs a View using a Rect param; (With NO Layers!!)
//            NOTE:
//            This Constructed is only called when making a VideoView
//            In other words, the View that is returned has NO layers will be completlty blank.
//            @param rect[in] The Rect to use
//            @param parent[in] The Parent View
//            @returns A View!
//         */
//        View(const Core::Rect & rect,View *parent);
        /**
            Constructs a View using a Rect param and a NativeItem; (With NO Layers!!)
            NOTE:
            This Constructed is only called when making a ScrollView.
            @param rect[in] The Rect to use
            @param nativeItem[in] The Native View to bind to
            @param parent[in] The Parent View
            @returns A View!
         */
        View(const Core::Rect & rect,Native::NativeItemPtr nativeItem,ViewPtr parent);
        /**
            Constructs a View. Creates its own LayerTree with a root Layer.
            @param rect The Rect to use
            @param parent The parent View (nullptr for root views)
            @returns A View!
         */
        View(const Core::Rect & rect,ViewPtr parent = nullptr);
    public:
        OMEGACOMMON_CLASS("OmegaWTK.View")
        /**
         * @brief Create A Layer
         * @param rect The Rectangle defining the bounds of the layer.
         * @returns Layer*/
        SharedHandle<Composition::Layer> makeLayer(Core::Rect rect);

        /**
         * @brief Create a Canvas that renders to CanvasFrames compatible with a Layer.
         * @param targetLayer The Layer to target.
         * @returns Canvas*/
        SharedHandle<Composition::Canvas> makeCanvas(SharedHandle<Composition::Layer> & targetLayer);

        /// @brief Retrieves the Rect that defines the position and bounds of the View.
        Core::Rect & getRect(){ return rect;};
        /// @brief Retrieves the View's own LayerTree.
        Composition::LayerTree * getLayerTree(){ return ownLayerTree.get(); };
        /// @brief Checks to see if this View is the root View of a Widget.
        bool isRootView(){return parent_ptr == nullptr;};
        /// @brief Returns the resize coordinator associated with this view.
        ViewResizeCoordinator & getResizeCoordinator(){ return resizeCoordinator; }
        const ViewResizeCoordinator & getResizeCoordinator() const { return resizeCoordinator; }
        /// @brief Propagates resize governor metadata through this view subtree.
        void setResizeGovernorMetadataRecurse(const Composition::ResizeGovernorMetadata & metadata,
                                              std::uint64_t coordinatorGeneration);

        /// @brief Sets the object to recieve View related events.
        virtual void setDelegate(ViewDelegate *_delegate);

        /// @brief Resize the view synchronously.
        /// @note If you wish to animate the View resize, please use the ViewAnimator to perform that action.
        virtual void resize(Core::Rect newRect);

        /// @brief Starts a Composition Session for this View.
        /// @paragraph Upon invocation, this will allow Canvases to render to child Layers in the View's LayerTree
        /// and it will allow submission of render and animation commands from the child LayerAnimators and ViewAnimator.
        /// If one attempts to try animate or render to the View or any child Layers without calling this method FIRST, will recieve an access error.
        void startCompositionSession();

        /// @brief Ends a Composition Session for this View.
        /// @paragraph This method closes the submission queue of all render commands and submits them to the Compositor.
        /// Any commands posted to the CompositorClientProxy after invocation of this method will be ignored and an access error will be thrown.
        void endCompositionSession();

        /// @brief Make the View visible.
        void enable();

        /// @brief Make the View invisible.
        void disable();

        void applyLayoutDelta(const struct LayoutDelta & delta,
                              const struct LayoutTransitionSpec & spec);

        ~View();
    };

    /// CanvasView Def.
    typedef View CanvasView;

    /**
        @brief The Root View delegate class!
     */
    INTERFACE OMEGAWTK_EXPORT ViewDelegate : public Native::NativeEventProcessor {
        void onRecieveEvent(Native::NativeEventPtr event);
        ViewDelegate *forwardDelegate = nullptr;
        friend class View;
        protected:
        View * view;

        void setForwardDelegate(ViewDelegate *delegate);
        /**
            @name Interface Methods
         
         */
        /// @{
//        /**
//            Called when the view has loaded. NOTE: All View Delegates must implement this method!
//        */
//        virtual void viewHasLoaded(Native::NativeEventPtr event) = 0;
        /**
            Called when the Mouse Enters the View
         */
        virtual void onMouseEnter(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Mouse Exits the View
         */
        virtual void onMouseExit(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Left Mouse Button is pressed
         */
        virtual void onLeftMouseDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Left Mouse Button is raised after being pressed
         */
        virtual void onLeftMouseUp(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Right Mouse Button is pressed
         */
        virtual void onRightMouseDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when the Right Mouse Button is raised after being pressed
         */
        virtual void onRightMouseUp(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when a key on a keyboard is pressed
         */
        virtual void onKeyDown(Native::NativeEventPtr event) DEFAULT;
        /**
            Called when a key on a keyboard is raised after being pressed
         */
        virtual void onKeyUp(Native::NativeEventPtr event) DEFAULT;
        /// @}
        public:
        ViewDelegate();
        ~ViewDelegate();
    };

    // class ViewTreeChildObserver {
    //     SharedHandle<View> view;
    // public:
        
    // };

    
    class ScrollViewDelegate;

    /// @brief ScrollView
    class OMEGAWTK_EXPORT ScrollView : public View {
        SharedHandle<View> child;
        Core::Rect * childViewRect;
        ScrollViewDelegate *delegate = nullptr;
        bool hasDelegate();
        bool hasVerticalScrollBar,hasHorizontalScrollBar;
        friend class Widget;
        explicit ScrollView(const Core::Rect & rect, SharedHandle<View> child, bool hasVerticalScrollBar, bool hasHorizontalScrollBar, ViewPtr parent = nullptr);
    public:
        OMEGACOMMON_CLASS("OmegaWTK.ScrollView")
        void toggleVerticalScrollBar();
        void toggleHorizontalScrollBar();
        void setDelegate(ScrollViewDelegate *_delegate);
        /**
            @param rect The Rect to use
            @returns A ScrollView!
         */
       
    };

    class OMEGAWTK_EXPORT ScrollViewDelegate : public Native::NativeEventProcessor {
        void onRecieveEvent(Native::NativeEventPtr event);
        friend class ScrollView;
    protected:
        ScrollView *scrollView;

        INTERFACE_METHOD void onScrollLeft() DEFAULT;
        INTERFACE_METHOD void onScrollRight() DEFAULT;
        INTERFACE_METHOD void onScrollDown() DEFAULT;
        INTERFACE_METHOD void onScrollUp() DEFAULT;
    };

    // class OMEGAWTK_EXPORT ClickableViewHandler : public ViewDelegate {
    //     std::function<void()> hover_begin_handler,hover_end_handler,click_handler,press_handler,release_handler;
    //     void onMouseEnter(Native::NativeEventPtr event) override;
    //     void onMouseExit(Native::NativeEventPtr event) override;
    //     void onLeftMouseDown(Native::NativeEventPtr event) override;
    //     void onLeftMouseUp(Native::NativeEventPtr event) override;
    // public:
    //     static SharedHandle<ClickableViewHandler> Create();
    //     void onHoverBegin(std::function<void()> handler);
    //     void onHoverEnd(std::function<void()> handler);
    //     void onPress(std::function<void()> handler);
    //     void onRelease(std::function<void()> handler);
    //     void onClick(std::function<void()> handler);
    // };


    // /// @brief TextView
    // class OMEGAWTK_EXPORT TextView : public View {

    //     SharedHandle<Composition::TextRect> textRect;

    //     SharedHandle<Composition::Font> font;

    //     SharedHandle<Composition::Canvas> rootLayerCanvas;

    //     UniqueHandle<ClickableViewHandler> clickableHandler;

    //      SharedHandle<Composition::Layer> cursorLayer;

    //     SharedHandle<Composition::Canvas> cursorCanvas;

    //     OmegaWTK::UniString str;
    //     bool editMode = false;
    //     void moveTextCursorToMousePoint(Core::Position & pos);
    //     void enableCursor();
    //     void disableCursor();
    //     void pushChar(Unicode32Char & ch);
    //     void popChar();
    //     void commitChanges();
    //     friend class TextViewDelegate;
    //     friend class Widget;
    //     explicit TextView(const Core::Rect & rect,Composition::LayerTree * layerTree,View * parent,bool io);
    // public:
    //     void updateFont(SharedHandle<Composition::Font> & font);
    //     void setContent(const UChar * str);
    // };

    // class OMEGAWTK_EXPORT TextViewDelegate : public ViewDelegate { 

    //     UniqueHandle<ClickableViewHandler> clickHandler;

    //     void onKeyDown(Native::NativeEventPtr event) override;
    //     void onKeyUp(Native::NativeEventPtr event) override;
    // public:
    //     TextViewDelegate(TextView *view);
    //     void toggleEdit();
    //     bool editMode();
    //     OmegaWTK::UniString & getString();
    // };

    enum class VideoScaleMode : int { AspectFit, AspectFill, Stretch };
    enum class VideoSourceMode : int { None, Playback, CapturePreview, CaptureRecord };

    struct VideoViewPlaybackOptions {
        bool useHardwareAccel = true;
        bool autoplay = false;
        bool loop = false;
    };

    struct VideoViewCaptureOptions {
        bool previewAudio = false;
        bool recordAudio = true;
    };

    class OMEGAWTK_EXPORT VideoViewDelegate {
    public:
        virtual void onVideoReady() {}
        virtual void onVideoEndOfStream() {}
        virtual void onVideoError(const OmegaCommon::String & message) {}
        virtual ~VideoViewDelegate() = default;
    };

    /**
     @brief The visual display output of a VideoPlaybackSession or a capture preview output of a VideoCaptureSession.
    */
    class OMEGAWTK_EXPORT VideoView : public View,
                                      public Media::VideoFrameSink {

        OmegaCommon::QueueHeap<SharedHandle<Media::VideoFrame>> framebuffer;

        SharedHandle<Composition::Canvas> videoCanvas;

        VideoViewDelegate *delegate_ = nullptr;
        VideoScaleMode scaleMode_ = VideoScaleMode::AspectFit;
        VideoSourceMode sourceMode_ = VideoSourceMode::None;
        SharedHandle<Media::VideoPlaybackSession> playbackSession_;
        UniqueHandle<Media::VideoCaptureSession> captureSession_;
        SharedHandle<Media::PlaybackDispatchQueue> dispatchQueue_;
        bool loop_ = false;

        void queueFrame(SharedHandle<Media::VideoFrame> &frame);

        bool framebuffered() const override {
            return true;
        };
        void flush() override;
        void pushFrame(SharedHandle<Media::VideoFrame> frame) override;
        void presentCurrentFrame() override;
    public:
        OMEGACOMMON_CLASS("OmegaWTK.VideoView")
        friend class Widget;

        VideoView(const Core::Rect & rect, ViewPtr parent = nullptr);

        void setDelegate(VideoViewDelegate *delegate);
        void setScaleMode(VideoScaleMode mode);
        VideoScaleMode scaleMode() const;
        VideoSourceMode sourceMode() const;

        bool bindPlaybackSource(Media::MediaInputStream & input,
                                const VideoViewPlaybackOptions & opts = {});
        bool bindCapturePreview(SharedHandle<Media::VideoDevice> & videoDevice,
                                SharedHandle<Media::AudioCaptureDevice> audioDevice = nullptr,
                                const VideoViewCaptureOptions & opts = {});
        bool bindCaptureRecord(SharedHandle<Media::VideoDevice> & videoDevice,
                               Media::MediaOutputStream & output,
                               SharedHandle<Media::AudioCaptureDevice> audioDevice = nullptr,
                               const VideoViewCaptureOptions & opts = {});

        void play();
        void pause();
        void stop();

        void startPreview();
        void stopPreview();

        void startRecording();
        void stopRecording();

        void clear();
    };
typedef Core::XMLDocument SVGDocument;

enum class SVGScaleMode : int { None, Meet, Slice };

struct SVGViewRenderOptions {
    SVGScaleMode scaleMode = SVGScaleMode::Meet;
    bool antialias = true;
    bool enableAnimation = true;
};

class OMEGAWTK_EXPORT SVGViewDelegate {
public:
    virtual void onSVGLoaded() {}
    virtual void onSVGParseError(const OmegaCommon::String & message) {}
    virtual ~SVGViewDelegate() = default;
};

struct SVGDrawOpList;

/**
 @brief Parses and renders SVG documents to a Canvas.
*/
class OMEGAWTK_EXPORT SVGView : public View {
    SharedHandle<Composition::Canvas> svgCanvas;
    SVGViewDelegate *delegate_ = nullptr;
    SVGViewRenderOptions options_ {};
    Core::Optional<Core::XMLDocument> sourceDoc_;
    Core::UniquePtr<SVGDrawOpList> drawOps_;
    bool needsRebuild_ = true;

    void rebuildDisplayList();
    friend class Widget;

    explicit SVGView(const Core::Rect & rect,ViewPtr parent);
public:
    OMEGACOMMON_CLASS("OmegaWTK.UI.SVGView")

    ~SVGView();
    void setDelegate(SVGViewDelegate *delegate);
    void setRenderOptions(const SVGViewRenderOptions & options);
    const SVGViewRenderOptions & renderOptions() const;

    bool setSourceDocument(Core::XMLDocument doc);
    bool setSourceString(const OmegaCommon::String & svgString);
    bool setSourceStream(std::istream & stream);

    void renderNow();

    void resize(Core::Rect newRect) override;
};

};

#endif
