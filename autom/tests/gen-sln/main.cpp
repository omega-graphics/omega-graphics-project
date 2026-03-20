#include "engine.h"
#include "renderer.h"

int main() {
    engine_init();
    render_frame();
    return engine_run();
}
