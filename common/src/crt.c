#include "omega-common/crt.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>



struct __OmegaObject {
    OmegaObjectType  type;
    void *data;
    size_t size;
};

OmegaRTObject * omega_common_alloc(void *data,size_t size,OmegaObjectType t){
    OmegaRTObject *rc = malloc(sizeof(OmegaRTObject));
    OmegaRTObject obj;
    obj.type = t;
    obj.data = malloc(size);
    obj.size = size;

    memcpy(obj.data,data,size);
    memcpy(rc,&obj,sizeof(OmegaRTObject));
    return rc;
};

void omega_common_object_get_data(OmegaRTObject * obj,void **data){
    if(data != NULL){
        *data = obj->data;
    }
};

OmegaCommonBool omega_common_exists(OmegaRTObject *obj){
    if(obj == NULL){
        return FALSE;
    }
    else {
        return TRUE;
    };
};

OmegaCommonBool omega_common_typecheck(OmegaRTObject *obj, OmegaObjectType type){
    if(obj->type == type){
        return TRUE;
    }
    else {
        return FALSE;
    };
};

void omega_common_free(OmegaRTObject *obj){
    free(obj->data);
    free(obj);
};
