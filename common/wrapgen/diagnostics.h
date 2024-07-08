#include "omega-common/common.h"
#include <deque>

#ifndef OMEGA_WRAPGEN_DIAGNOSTICS_H
#define OMEGA_WRAPGEN_DIAGNOSTICS_H

namespace OmegaWrapGen {

struct Diagnostic {
    virtual void format(std::ostream & out) = 0;
    virtual bool isError() = 0;
};

class DiagnosticBuffer {
    std::deque<Diagnostic *> buffer;
public:
    void push(Diagnostic *d);
    bool hasErrored();
    void logAll();
};

};

#endif