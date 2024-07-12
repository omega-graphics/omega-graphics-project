#include "omegaWTK/UI/UIView.h"
// #include "omegaWTK/Composition/Canvas.h"

namespace OmegaWTK {

    void UIViewLayout::text(UIElementTag tag, OmegaCommon::UString content){
        auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](Element & ele){
            return ele.tag == tag;
        });
        if(taggedElementIt != _content.end()){
            if(taggedElementIt->type == 1){
                std::cout << "CANNOT CHANGE TYPE OF ELEMENT." << taggedElementIt->tag;
            }
            taggedElementIt->str = content;
        }
        else {
            _content.push_back({0,tag,content,{}});
        }
        
    }

    void UIViewLayout::shape(UIElementTag tag,Shape & shape){
        auto taggedElementIt = std::find_if(_content.begin(),_content.end(),[&](Element & ele){
            return ele.tag == tag;
        });
        if(taggedElementIt != _content.end()){
            if(taggedElementIt->type == 0){
                std::cout << "CANNOT CHANGE TYPE OF ELEMENT." << taggedElementIt->tag;
            }
            taggedElementIt->shape = shape;
        }
        else {
            _content.push_back(UIViewLayout::Element{1,tag,{},shape});
        }
    }

    StyleSheetPtr StyleSheet::Create(){
        return make<StyleSheet>();
    }

    StyleSheet::StyleSheet(){
        
    }

    // StyleSheetPtr StyleSheet::border(UIViewTag tag, bool use){

    // }

    UIRenderer::UIRenderer(UIView *view):view(view){
        
    }

    /**
     * @note The UI Renderer builds it UI layering as a stack. 
     (So the first element is the bottom layer, and the second element is the next layer up and so on.)
     * 
     * 
     */

    SharedHandle<Composition::Canvas> UIRenderer::buildLayerRenderTarget(UIElementTag tag){
        auto entry = renderTargetStore.find(tag);
        if(entry != renderTargetStore.end()){
            
        }
        else {
            // Find the element count so the relative layer in the layertree can be created.
            auto relativeLayerIdx = renderTargetStore.size();
            if(relativeLayerIdx > 0){

            }
            else {
                renderTargetStore[tag] = view->makeCanvas(view->getLayerTreeLimb()->getRootLayer());
            }
        }


    }

    void UIRenderer::handleElement(UIElementTag tag){
        
        auto t = buildLayerRenderTarget(tag);

        t->sendFrame();
       
    }

    UIView::UIView(const Core::Rect & rect,Composition::LayerTree *layerTree,ViewPtr parent,UIViewTag tag):CanvasView(rect,layerTree,parent),tag(tag),UIRenderer(this){

    }



    void UIView::update(){
        
    }
};