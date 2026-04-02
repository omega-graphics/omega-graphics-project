#include "omegaWTK/Core/XML.h"

#include <libxml/xmlversion.h>
#include <libxml/parser.h>
#include <libxml/xmlIO.h>

namespace OmegaWTK::Core {

    XMLDocument XMLDocument::parseFromString(OmegaCommon::String str){
        std::istringstream is(str);
        return parseFromStream(is);
    };

    XMLDocument::Tag::Tag(void *data):data(data){

    }

    OmegaCommon::StrRef XMLDocument::Tag::attribute(const OmegaCommon::StrRef & name) {
        auto node = (xmlNodePtr)data;
        return (const char *)xmlGetProp(node,(xmlChar *)name.data());
    }

    OmegaCommon::Vector<XMLDocument::Tag> XMLDocument::Tag::children() {
        auto node = (xmlNodePtr)data;
        OmegaCommon::Vector<XMLDocument::Tag> tags;
        for(auto child_it = node->children; child_it != nullptr; child_it = child_it->next){
            tags.push_back(Tag(child_it));
        }
        return tags;
    }

    bool XMLDocument::Tag::isElement() const {
        auto node = (xmlNodePtr)data;
        return node->type == XML_ELEMENT_NODE;
    }

    bool XMLDocument::Tag::isText() const {
        auto node = (xmlNodePtr)data;
        return node->type == XML_TEXT_NODE;
    }

    OmegaCommon::StrRef XMLDocument::Tag::content() {
        auto node = (xmlNodePtr)data;
        return {(const char *)node->content};
    }

    OmegaCommon::StrRef XMLDocument::Tag::name() {
        auto node = (xmlNodePtr)data;

        return {(const char *)node->name};
    }

    XMLDocument::Tag XMLDocument::root() {
        auto doc = (xmlDocPtr)data;
        auto node_ptr = xmlDocGetRootElement(doc);
        return XMLDocument::Tag{node_ptr};
    }

    XMLDocument XMLDocument::parseFromStream(std::istream &in){
        std::streamsize fileSize;
        in.seekg(0,std::ios::end);
        fileSize = in.tellg();
        in.seekg(0,std::ios::beg);

        auto * buffer = new char[fileSize];

        in.read(buffer,fileSize);

        auto doc =  xmlParseMemory(buffer,(int)fileSize);
        delete[] buffer;
        XMLDocument ret {};
        ret.data = doc;
        return ret;
    };

    OmegaCommon::String XMLDocument::serialize(){
        std::ostringstream out;
        xmlChar *buffer;
        int _size;
        auto doc = (xmlDocPtr)data;
        xmlDocDumpMemory(doc,&buffer,&_size);
        out.write((char *)buffer,_size);
        return out.str();
    };

    XMLDocument & XMLDocument::operator=(XMLDocument && other) noexcept {
        if(this != &other){
            if(data != nullptr){
                xmlFreeDoc((xmlDocPtr)data);
            }
            data = other.data;
            other.data = nullptr;
        }
        return *this;
    }

    XMLDocument::~XMLDocument() {
        if(data != nullptr){
            xmlFreeDoc((xmlDocPtr)data);
        }
    }

}