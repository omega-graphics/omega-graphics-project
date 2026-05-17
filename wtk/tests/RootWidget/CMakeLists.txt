OmegaWTKApp(
    NAME
    RootWidgetTest
    BUNDLE_ID
    "org.omegagraphics.RootWidgetTest"
    SOURCES
    Main.cpp)

# Tier 3 Phase 3.2: mirror the OmegaWTK_UI build flag onto the test
# binary so the same -DOMEGAWTK_WINDOW_SCOPED_PAINT=ON cmake option
# selects both (a) the runtime default for AppWindow::windowScopedPaint
# and (b) the Phase32Widget multi-UIView scene in Main.cpp.
if(OMEGAWTK_WINDOW_SCOPED_PAINT)
    target_compile_definitions(RootWidgetTest PRIVATE OMEGAWTK_WINDOW_SCOPED_PAINT=1)
endif()
