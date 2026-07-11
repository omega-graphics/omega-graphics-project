#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/Theme.h>
#include <omegaWTK/UI/Menu.h>
#include <omegaWTK/UI/Notification.h>
#include <omegaWTK/Widgets/Primatives.h>
#include <omegaWTK/Widgets/Containers.h>
#include <omegaWTK/Widgets/BasicWidgets.h>
#include <omegaWTK/Widgets/UserInputs.h>

using namespace OmegaWTK;

// ---------------------------------------------------------------------------
// Menu delegates
// ---------------------------------------------------------------------------

static AppWindow *g_mainWindow = nullptr;
static SharedHandle<NotificationCenter> nc;

// Native-Theme-Application-Plan Tier 3 demo: a custom Theme with an
// obviously non-OS surface color per variant so the "Theme" menu below
// makes row 2 (custom surface override) + appearance forcing visible.
static SharedHandle<Theme> g_customTheme;
static bool g_customThemeOn = false;

class TestMenuDelegate final : public MenuDelegate {
public:
    void onSelectItem(unsigned itemIndex) override {
        switch (itemIndex) {
        case 0:
            // "Open" — show a file dialog
            if (g_mainWindow) {
                g_mainWindow->openFSDialog(
                    {Native::NativeFSDialog::Read, "."});
            }
            break;
        case 2:
            // "Quit"
            AppInst::terminate();
            break;
        default:
            break;
        }
    }
};

class EditMenuDelegate final : public MenuDelegate {
public:
    void onSelectItem(unsigned itemIndex) override {
        (void)itemIndex;
    }
};

class HelpMenuDelegate final : public MenuDelegate {
public:
    void onSelectItem(unsigned itemIndex) override {
        if (itemIndex == 0) {
            nc->send({"BasicAppTest", "OmegaWTK Widget & Menu Integration Test"});
        }
    }
};

// Native-Theme-Application-Plan Tier 3 exercise. Install/clear the custom
// theme (row 2 surface override) and pin / release the appearance
// (setForcedAppearance). setTheme/setForcedAppearance already dirty the
// cascade + request a frame, so the window re-clears to the new surface.
class ThemeMenuDelegate final : public MenuDelegate {
public:
    void onSelectItem(unsigned itemIndex) override {
        auto *app = AppInst::inst();
        if (app == nullptr) return;
        switch (itemIndex) {
        case 0: // Toggle Custom Theme
            g_customThemeOn = !g_customThemeOn;
            app->setTheme(g_customThemeOn ? g_customTheme : SharedHandle<Theme>{nullptr});
            if (nc) nc->send({"Theme", g_customThemeOn
                ? "Custom theme ON — lavender (Light) / indigo (Dark) surface"
                : "Custom theme OFF — OS surface"});
            break;
        case 1: // Force Light
            app->setForcedAppearance(Native::ThemeAppearance::Light);
            if (nc) nc->send({"Theme", "Forced Light appearance"});
            break;
        case 2: // Force Dark
            app->setForcedAppearance(Native::ThemeAppearance::Dark);
            if (nc) nc->send({"Theme", "Forced Dark appearance"});
            break;
        case 3: // Follow OS
            app->setForcedAppearance(Core::Optional<Native::ThemeAppearance>{});
            if (nc) nc->send({"Theme", "Following OS appearance"});
            break;
        default:
            break;
        }
    }
};

// ---------------------------------------------------------------------------
// Window delegate
// ---------------------------------------------------------------------------

class TestWindowDelegate final : public AppWindowDelegate {
public:
    void windowWillClose(Native::NativeEventPtr event) override {
        // Window-close is per-window now: disposing of the window
        // does not quit the app. macOS HIG; on Win/GTK the canonical
        // exit path is the File > Quit menu item, which is wired to
        // AppInst::terminate() in TestMenuDelegate::onSelectItem.
        (void)event;
    }
};

// ---------------------------------------------------------------------------
// Entry point — all widget shared_ptrs must stay alive until start() returns
// ---------------------------------------------------------------------------

int RunBasicAppTest(AppInst *app) {
    Composition::Rect windowRect{{0, 0}, 600, 640};

    auto window = make<AppWindow>(windowRect, new TestWindowDelegate());
    g_mainWindow = window.get();
    window->setTitle("BasicAppTest");

    // --- Menu bar ---

    static TestMenuDelegate fileDelegate;
    static EditMenuDelegate editDelegate;
    static HelpMenuDelegate helpDelegate;
    static ThemeMenuDelegate themeDelegate;

    // Tier 3 demo theme: surfaces chosen to be unmistakably NOT the OS
    // default (white / dark-gray) so row 2 is obvious when toggled.
    {
        ThemeVariant lightVariant;
        lightVariant.surface = Composition::Color::create8Bit(0xE8, 0xE0, 0xF5, 255); // lavender
        ThemeVariant darkVariant;
        darkVariant.surface = Composition::Color::create8Bit(0x1A, 0x10, 0x30, 255);  // deep indigo
        g_customTheme = Theme::Create("DemoTheme", lightVariant, darkVariant);
    }

    auto menu = make<Menu>("MainMenu", std::initializer_list<SharedHandle<MenuItem>>{
        CategoricalMenu("File", {
            ButtonMenuItem("Open"),
            MenuItemSeperator(),
            ButtonMenuItem("Quit")
        }, &fileDelegate),
        CategoricalMenu("Edit", {
            ButtonMenuItem("Cut"),
            ButtonMenuItem("Copy"),
            ButtonMenuItem("Paste")
        }, &editDelegate),
        CategoricalMenu("Theme", {
            ButtonMenuItem("Toggle Custom Theme"),
            ButtonMenuItem("Force Light"),
            ButtonMenuItem("Force Dark"),
            ButtonMenuItem("Follow OS")
        }, &themeDelegate),
        CategoricalMenu("Help", {
            ButtonMenuItem("About")
        }, &helpDelegate)
    });
    window->setMenu(menu);

    // --- Widget tree ---
    // All shared_ptrs live in this scope until AppInst::start() blocks and returns.

    float contentW = windowRect.w - 32.f;

    StackOptions rootOpts;
    rootOpts.spacing = 8.f;
    rootOpts.padding = StackInsets{16.f, 16.f, 16.f, 16.f};
    rootOpts.mainAlign = StackMainAlign::Start;
    rootOpts.crossAlign = StackCrossAlign::Stretch;
    auto root = make<VStack>(windowRect, rootOpts);

    // Title
    LabelProps titleProps;
    titleProps.text = U"BasicAppTest — Widget Integration";
    // Follow the OS theme foreground so the title stays legible in both
    // Light and Dark and transitions with the appearance flip (was a
    // hardcoded white that vanished on a light background).
    titleProps.followThemeForeground = true;
    titleProps.alignment = Composition::TextLayoutDescriptor::MiddleCenter;
    titleProps.wrapping = Composition::TextLayoutDescriptor::None;
    auto titleLabel = make<Label>(
        Composition::Rect{{0, 0}, contentW, 30.f}, titleProps);
    StackSlot titleSlot;
    titleSlot.flexGrow = 0.f;
    root->addChild(titleLabel, titleSlot);

    // Separator
    SeparatorProps sep1Props;
    sep1Props.orientation = Orientation::Horizontal;
    sep1Props.thickness = 1.f;
    sep1Props.inset = 0.f;
    sep1Props.brush = Composition::ColorBrush(
        Composition::Color::create8Bit(0x555555));
    auto sep1 = make<Separator>(
        Composition::Rect{{0, 0}, contentW, 4.f}, sep1Props);
    StackSlot sep1Slot;
    sep1Slot.flexGrow = 0.f;
    root->addChild(sep1, sep1Slot);

    // Shape row
    StackOptions shapeRowOpts;
    shapeRowOpts.spacing = 12.f;
    shapeRowOpts.mainAlign = StackMainAlign::Center;
    shapeRowOpts.crossAlign = StackCrossAlign::Center;
    auto shapeRow = make<HStack>(
        Composition::Rect{{0, 0}, contentW, 100.f}, shapeRowOpts);

    RectangleProps redProps;
    redProps.fill = Composition::ColorBrush(
        Composition::Color::create8Bit(Composition::Color::Red8));
    auto redRect = make<Rectangle>(
        Composition::Rect{{0, 0}, 80.f, 80.f}, redProps);
    shapeRow->addChild(redRect);

    RoundedRectangleProps blueProps;
    blueProps.fill = Composition::ColorBrush(
        Composition::Color::create8Bit(Composition::Color::Blue8));
    blueProps.topLeft = 12.f;
    blueProps.topRight = 12.f;
    blueProps.bottomLeft = 12.f;
    blueProps.bottomRight = 12.f;
    auto blueRR = make<RoundedRectangle>(
        Composition::Rect{{0, 0}, 80.f, 80.f}, blueProps);
    shapeRow->addChild(blueRR);

    EllipseProps greenProps;
    greenProps.fill = Composition::ColorBrush(
        Composition::Color::create8Bit(Composition::Color::Green8));
    auto greenEllipse = make<Ellipse>(
        Composition::Rect{{0, 0}, 80.f, 80.f}, greenProps);
    shapeRow->addChild(greenEllipse);

    RectangleProps yellowProps;
    yellowProps.fill = Composition::ColorBrush(
        Composition::Color::create8Bit(Composition::Color::Yellow8));
    yellowProps.stroke = Composition::ColorBrush(
        Composition::Color::create8Bit(Composition::Color::Black8));
    yellowProps.strokeWidth = 2.f;
    auto yellowRect = make<Rectangle>(
        Composition::Rect{{0, 0}, 80.f, 80.f}, yellowProps);
    shapeRow->addChild(yellowRect);

    StackSlot shapeRowSlot;
    shapeRowSlot.flexGrow = 0.f;
    root->addChild(shapeRow, shapeRowSlot);

    // Separator
    SeparatorProps sep2Props;
    sep2Props.orientation = Orientation::Horizontal;
    sep2Props.thickness = 1.f;
    sep2Props.brush = Composition::ColorBrush(
        Composition::Color::create8Bit(0x555555));
    auto sep2 = make<Separator>(
        Composition::Rect{{0, 0}, contentW, 4.f}, sep2Props);
    StackSlot sep2Slot;
    sep2Slot.flexGrow = 0.f;
    root->addChild(sep2, sep2Slot);

    // Description
    LabelProps descProps;
    descProps.text = U"This test exercises shape primitives (Rectangle, RoundedRectangle, "
                     U"Ellipse), text (Label), layout (VStack/HStack), the Button widget "
                     U"with hover transitions, and the app menu system "
                     U"(File > Open, Help > About).";
    // Follow the OS theme foreground (was a fixed light gray that only
    // happened to read on both backgrounds).
    descProps.followThemeForeground = true;
    descProps.alignment = Composition::TextLayoutDescriptor::LeftUpper;
    descProps.wrapping = Composition::TextLayoutDescriptor::WrapByWord;
    auto descLabel = make<Label>(
        Composition::Rect{{0, 0}, contentW, 60.f}, descProps);
    StackSlot descSlot;
    descSlot.flexGrow = 1.f;
    root->addChild(descLabel, descSlot);

    // Button row — exercises the Phase 4A Button base implementation with
    // explicit transition specs on hover. The default
    // ButtonProps::hoverTransitionDuration is 150 ms (matches the macOS
    // / Win32 convention); this row demonstrates it with three buttons
    // at different durations, plus a disabled one.
    StackOptions buttonRowOpts;
    buttonRowOpts.spacing = 12.f;
    buttonRowOpts.mainAlign = StackMainAlign::Center;
    buttonRowOpts.crossAlign = StackCrossAlign::Center;
    auto buttonRow = make<HStack>(
        Composition::Rect{{0, 0}, contentW, 40.f}, buttonRowOpts);

    // Default 150 ms hover transition (per ButtonProps default).
    ButtonProps clickMeProps;
    clickMeProps.text = U"Click me";
    auto clickMe = make<Button>(
        Composition::Rect{{0, 0}, 120.f, 32.f}, clickMeProps);
    clickMe->setOnPress([](){
        if(nc){
            nc->send({"BasicAppTest", "Button clicked — default 150ms hover transition."});
        }
    });
    // §2.3a C1 demo: a clickable control shows the pointing-hand cursor on
    // hover. Set on the Button's root view; the hover dispatcher's
    // ancestor walk applies it even when the hit lands on the button's
    // inner label view.
    clickMe->viewRef().setCursorShape(Native::CursorShape::PointingHand);
    // §2.3a T1 demo: hover for ~500ms to show a tooltip near the cursor.
    clickMe->setTooltip("Default 150ms hover transition");
    buttonRow->addChild(clickMe);

    // Slow 400 ms hover transition with a custom accent color.
    ButtonProps slowProps;
    slowProps.text = U"Slow hover";
    slowProps.hoverTransitionDuration = 0.4f;
    slowProps.tintOverride = Composition::Color::create8Bit(0x2A6FCC); // deep blue
    auto slowHover = make<Button>(
        Composition::Rect{{0, 0}, 140.f, 32.f}, slowProps);
    slowHover->setOnPress([](){
        if(nc){
            nc->send({"BasicAppTest", "Slow-hover button clicked — 400ms transition."});
        }
    });
    slowHover->viewRef().setCursorShape(Native::CursorShape::PointingHand);
    slowHover->setTooltip("Slow 400ms accent hover");
    buttonRow->addChild(slowHover);

    // Snap (no transition) — instant state change for comparison.
    ButtonProps snapProps;
    snapProps.text = U"Snap";
    snapProps.hoverTransitionDuration = 0.f;
    auto snapBtn = make<Button>(
        Composition::Rect{{0, 0}, 80.f, 32.f}, snapProps);
    snapBtn->viewRef().setCursorShape(Native::CursorShape::PointingHand);
    buttonRow->addChild(snapBtn);

    // Icon + label — exercises the "icon" element geometry (left, square,
    // vertically centered, to the left of the label). Regression probe for
    // the view-local-coords fix (2026-07-10): before it, the positioned icon
    // square was clamped to the wrong spot by clampRectToParent because its
    // rect was authored in absolute window coords. The glyph should now sit
    // vertically centered at the field's left inset, ahead of "Icon".
    ButtonProps iconProps;
    iconProps.text = U"Icon";
    iconProps.iconToken = "+";
    auto iconBtn = make<Button>(
        Composition::Rect{{0, 0}, 100.f, 32.f}, iconProps);
    iconBtn->viewRef().setCursorShape(Native::CursorShape::PointingHand);
    iconBtn->setTooltip("Button with a leading icon glyph");
    buttonRow->addChild(iconBtn);

    // Disabled — dimmed at 0.4 alpha, never enters hover/pressed.
    ButtonProps disabledProps;
    disabledProps.text = U"Disabled";
    disabledProps.enabled = false;
    auto disabledBtn = make<Button>(
        Composition::Rect{{0, 0}, 100.f, 32.f}, disabledProps);
    // A disabled control shows the not-allowed cursor — a second shape so
    // the demo also exercises moving between distinct cursors.
    disabledBtn->viewRef().setCursorShape(Native::CursorShape::NotAllowed);
    // Tooltip on a disabled control — hit-testing still reaches disabled
    // views (see C1 notes), so the tooltip shows even though the button
    // itself never enters hover/pressed.
    disabledBtn->setTooltip("This button is disabled");
    buttonRow->addChild(disabledBtn);

    StackSlot buttonRowSlot;
    buttonRowSlot.flexGrow = 0.f;
    root->addChild(buttonRow, buttonRowSlot);

    // TextInput row — Phase 4B v0. Click (or Tab) to focus it (accent ring +
    // caret appear via the FocusManager), then type: printable chars insert,
    // Backspace/Delete edit, Left/Right/Home/End move the caret. The mirror
    // Label below echoes the value through the onValueChange callback, so the
    // edit loop is visible without notification spam.
    TextInputProps inputProps;
    inputProps.placeholder = U"Type here…";
    auto textInput = make<TextInput>(
        Composition::Rect{{0, 0}, contentW, 32.f}, inputProps);
    StackSlot inputSlot;
    inputSlot.flexGrow = 0.f;
    root->addChild(textInput, inputSlot);

    LabelProps mirrorProps;
    mirrorProps.text = U"(nothing typed yet)";
    mirrorProps.followThemeForeground = true;
    mirrorProps.alignment = Composition::TextLayoutDescriptor::LeftCenter;
    mirrorProps.wrapping = Composition::TextLayoutDescriptor::None;
    auto mirrorLabel = make<Label>(
        Composition::Rect{{0, 0}, contentW, 22.f}, mirrorProps);
    textInput->setOnValueChange([mirrorLabel](const OmegaCommon::UString & value){
        OmegaCommon::UString shown = U"You typed: ";
        shown += value;
        mirrorLabel->setText(shown);
    });
    StackSlot mirrorSlot;
    mirrorSlot.flexGrow = 0.f;
    root->addChild(mirrorLabel, mirrorSlot);

    // Separator
    SeparatorProps sep3Props;
    sep3Props.orientation = Orientation::Horizontal;
    sep3Props.thickness = 1.f;
    sep3Props.brush = Composition::ColorBrush(
        Composition::Color::create8Bit(0x555555));
    auto sep3 = make<Separator>(
        Composition::Rect{{0, 0}, contentW, 4.f}, sep3Props);
    StackSlot sep3Slot;
    sep3Slot.flexGrow = 0.f;
    root->addChild(sep3, sep3Slot);

    // ScrollableContainer (ScrollableContainer-Implementation-Plan S1)
    // ---------------------------------------------------------------
    // A 200x200 scroll viewport whose content extent is 200x600. Three
    // contiguous 200x200 colored bands are placed in content space at
    // y = 0 / 200 / 400, so only the first band fits the viewport and
    // the lower two are reachable only by scrolling. §6.1's upper-bound
    // clamp keeps the wheel from scrolling past the bottom band
    // (content.h - viewport.h = 400) or above the top (0).
    ScrollableContainerOptions scrollOpts;
    scrollOpts.verticalScroll = true;
    scrollOpts.horizontalScroll = false;
    // Pin the viewport to its own 200px width even though the root VStack
    // uses crossAlign=Stretch — otherwise the parent would widen the
    // viewport to the full window (the nested-scroll-on-a-page case).
    scrollOpts.resizeWithParent = false;
    auto scrollBox = make<ScrollableContainer>(
        Composition::Rect{{0, 0}, 200.f, 200.f}, scrollOpts);
    // Set the extent BEFORE adding children so the inner content host
    // clamps them against 200x600, not the initial viewport-sized rect
    // (plan §5.3).
    scrollBox->setContentSize(200.f, 600.f);

    struct ScrollBand { float y; uint32_t color; };
    const ScrollBand bands[] = {
        {0.f,   Composition::Color::Red8},
        {200.f, Composition::Color::Green8},
        {400.f, Composition::Color::Blue8},
    };
    // Keep the band widgets alive past this loop — addChild stores them
    // in the inner Container, but holding our own handles documents the
    // ownership and matches the rest of this function's scope rules.
    SharedHandle<Widget> scrollBands[3];
    for (int i = 0; i < 3; ++i) {
        RectangleProps bandProps;
        bandProps.fill = Composition::ColorBrush(
            Composition::Color::create8Bit(bands[i].color));
        scrollBands[i] = make<Rectangle>(
            Composition::Rect{{0.f, bands[i].y}, 200.f, 200.f}, bandProps);
        scrollBox->addChild(scrollBands[i]);
    }

    // V2.1 Invariant-A probe: a Button near the top of the scroll content
    // (content y=20, inside the viewport unscrolled). Wheeling *over* this
    // button must still scroll the list (the button ignores ScrollWheel,
    // so it bubbles to the ScrollView); clicking it must fire its onPress.
    ButtonProps scrollBtnProps;
    scrollBtnProps.text = U"In-scroll";
    auto scrollBtn = make<Button>(
        Composition::Rect{{20.f, 20.f}, 120.f, 32.f}, scrollBtnProps);
    scrollBtn->setOnPress([](){
        if(nc){
            nc->send({"BasicAppTest", "In-scroll button clicked."});
        }
    });
    scrollBox->addChild(scrollBtn);

    StackSlot scrollSlot;
    scrollSlot.flexGrow = 0.f;
    root->addChild(scrollBox, scrollSlot);

    window->setRootWidget(root);

    // Notifications — one long-lived center for the whole app (a throwaway
    // stack local would tear down the native center before delivery). The API
    // now gates delivery on authorization, so request permission up front and
    // send the startup note once it's granted.
    nc = make<NotificationCenter>();
    nc->requestPermission([](bool granted){
        if (granted) {
            nc->send({"BasicAppTest", "Window opened with widget tree and menu bar."});
        }
    });

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return AppInst::start();
}
