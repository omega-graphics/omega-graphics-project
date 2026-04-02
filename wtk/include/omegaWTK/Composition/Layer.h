/**
 @file Layer.h
 
 Defines the ViewRenderTarget, Layer, and LayerTree.
 */

#include "omegaWTK/Core/Core.h"
#include "omegaWTK/Native/NativeItem.h"
#include "omegaWTK/Native/NativeWindow.h"

#include "Brush.h"

#ifndef OMEGAWTK_COMPOSITION_LAYER_H
#define OMEGAWTK_COMPOSITION_LAYER_H

namespace OmegaWTK {

class AppWindow;
class AppWindowManager;
class Container;
class View;
class Widget;

namespace Composition {
    
    class ViewRenderTarget;
	class Canvas;
        /**
            A mutlifeatured surface for composing visuals on.
         */
    class Layer;

    typedef Layer CanvasLayer;
    
    INTERFACE LayerTreeObserver;
//    typedef std::function<bool(Layer *)> LayerTreeTraversalCallback;
    /**
     An entire widget's layer construct
     */
    /**
     @brief A per-View layer tree. Owns a root Layer and its sublayers.

     After the Per-View LayerTree isolation refactor, each View creates and
     owns its own LayerTree. The former Limb subclass has been consolidated
     into LayerTree directly — the 1:1 View-to-tree mapping makes the
     intermediate grouping node unnecessary.
     */
    class OMEGAWTK_EXPORT LayerTree : public Native::NativeLayerTreeLimb {
        friend class ::OmegaWTK::View;
        friend class ::OmegaWTK::Widget;
        friend class ::OmegaWTK::Container;
    protected:
        OmegaCommon::Vector<LayerTreeObserver *> observers;

        friend class Layer;

        void notifyObserversOfResize(Layer *layer);
        void notifyObserversOfDisable(Layer *layer);
        void notifyObserversOfEnable(Layer *layer);

        /**
         @note Only called by ::OmegaWTK::Widget
        */
        void notifyObserversOfWidgetDetach();

    private:
        SharedHandle<Layer> rootLayer;
        bool enabled;

    public:
        using iterator = OmegaCommon::Vector<SharedHandle<Layer>>::iterator;

        /// @brief Retrieve the root Layer of this tree.
        SharedHandle<Layer> & getRootLayer();
        /// @brief Add a child Layer (becomes a sublayer of the root Layer).
        void addLayer(SharedHandle<Layer> layer);

        iterator begin();
        iterator end();

        void enable();
        void disable();

        void addObserver(LayerTreeObserver * observer);
        void removeObserver(LayerTreeObserver * observer);

        /// @brief Collect all layers in the tree (root + children) into a flat vector.
        void collectAllLayers(OmegaCommon::Vector<Layer *> & out);

        /**
         Construct a LayerTree with a root Layer sized to the given rect.
         */
        explicit LayerTree(const Core::Rect & rect);
        /// @brief Default-construct an empty tree (no root layer). Legacy compat only.
        LayerTree();
        ~LayerTree();
    };

    struct LayerEffect {
        enum : OPT_PARAM {
            DropShadow,
            Transformation,
        } type;
         typedef struct {
            float x_offset, y_offset;
            float radius;
            float blurAmount;
            float opacity;
            Color color;
        } DropShadowParams;

        typedef struct {
            struct {
                float x;
                float y;
                float z;
            } translate;
            struct {
                float pitch;
                float yaw;
                float roll;
            } rotate;
            struct {
                float x;
                float y;
                float z;
            } scale;
        } TransformationParams;
        union {
           
            DropShadowParams dropShadow;

            TransformationParams transform;
        };
        ~LayerEffect();
    };

    /**
     @brief A resizable rectangular surface for displaying vector graphics.
     */
    class OMEGAWTK_EXPORT Layer {
        unsigned id_gen = 0;
        LayerTree *parentTree;
        OmegaCommon::Vector<SharedHandle<Layer>> children;

        Layer * parent_ptr;
        Core::Rect surface_rect;
        bool enabled;
        bool needsNativeResize;

        /// Tracks the single Canvas bound to this Layer (non-owning).
        /// Enforces the one-Canvas-per-Layer invariant structurally.
        Canvas * boundCanvas_ = nullptr;

        friend class LayerTree;
        friend class Canvas;
        friend class ::OmegaWTK::View;
        void addSubLayer(SharedHandle<Layer> & layer);
        void removeSubLayer(SharedHandle<Layer> & layer);
    public:
        /// Returns true if a Canvas is currently bound to this Layer.
        bool hasCanvas() const { return boundCanvas_ != nullptr; }
        OMEGACOMMON_CLASS("OmegaWTK.Composition.Layer")

        LayerTree *getParentTree();

        /**
            @brief Resize the Layer with the new rect
            @param newRect
         */
        void resize(Core::Rect & newRect);

        /**
           @brief Retrieves the rect that defines the bounds.
           @returns A reference to the Core::Rect.
           */
        Core::Rect & getLayerRect(){return surface_rect;};

        /**
         @brief Enable the layer
         @param state The boolean to use.
         */
        void setEnabled(bool state){enabled = state;};

        /**
          @brief Checks if this layer is a child of another layer.
          @returns bool
          */
        bool isChildLayer(){return parent_ptr != nullptr;}

        /** @brief Construct a Layer.
           @param rect*/
        explicit Layer(const Core::Rect & rect);
        ~Layer();
    };

    /// @interface LayerTreeObserver
    class LayerTreeObserver {
    public:

        /**
         * @fn virtual void LayerTreeObserver::hasDetached(LayerTree *tree) = 0
         @brief A method called when the Widget/LayerTree has detached from a WidgetTree.
        */
        INTERFACE_METHOD void hasDetached(LayerTree *tree) ABSTRACT;

        /**
          @fn virtual void LayerTreeObserver::layerHasResized(Layer *layer) = 0
         @brief A method called when the target layer has resized within this LayerTree
         @param layer
        */
        INTERFACE_METHOD void layerHasResized(Layer *layer) ABSTRACT;

        /**
         @fn virtual void LayerTreeObserver::layerHasDisabled(Layer *layer) = 0
         @brief A method called when the target layer has been disabled within this LayerTree
         @param layer
        */
        INTERFACE_METHOD void layerHasDisabled(Layer *layer) ABSTRACT;

        /**
         @fn virtual void LayerTreeObserver::layerHasEnabled(Layer *layer) = 0
         @brief A method called when the target layer has been enabled within this LayerTree
         @param layer
        */
        INTERFACE_METHOD void layerHasEnabled(Layer *layer) ABSTRACT;

        /// @fn virtual LayerTreeObserver::~LayerTreeObserver() = default
        virtual ~LayerTreeObserver() = default;
    };

    /**
     A singular surface
     */
    class OMEGAWTK_EXPORT  WindowLayer {
        Native::NWH native_window_ptr;
        Core::Rect & rect;
        SharedHandle<Canvas> windowSurface;
        // SharedHandle<MenuStyle> menuStyle;
        friend class OmegaWTK::AppWindow;
        friend class OmegaWTK::AppWindowManager;
        friend class Compositor;
        void redraw();
        // void setWindowStyle(SharedHandle<WindowStyle> & style);
        // void setMenuStyle(SharedHandle<MenuStyle> & style);
    public:
        WindowLayer(Core::Rect & rect,Native::NWH native_window_ptr);
    };
    
};


};

#endif
