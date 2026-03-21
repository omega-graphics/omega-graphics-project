#ifndef BASICAPPTEST_RUN_H
#define BASICAPPTEST_RUN_H

namespace OmegaWTK {
class AppInst;
}

/// Application logic: create window, widget, run loop.
/// Implemented in BasicAppTestRun.cpp to keep that TU's includes out of main.cpp.
int RunBasicAppTest(OmegaWTK::AppInst* app);

#endif
