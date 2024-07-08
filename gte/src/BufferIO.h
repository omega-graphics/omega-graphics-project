#include <omegasl.h>

#ifndef OMEGAGTE_BUFFERIO_H_PRIV
#define OMEGAGTE_BUFFERIO_H_PRIV

namespace OmegaGTE {
    struct DataBlock {
        omegasl_data_type type;
        void *data;
    };
}

#endif