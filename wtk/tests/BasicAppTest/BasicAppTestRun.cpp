#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
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
    Composition::Rect windowRect{{0, 0}, 600, 500};

    auto window = make<AppWindow>(windowRect, new TestWindowDelegate());
    g_mainWindow = window.get();
    window->setTitle("BasicAppTest");

    // --- Menu bar ---

    static TestMenuDelegate fileDelegate;
    static EditMenuDelegate editDelegate;
    static HelpMenuDelegate helpDelegate;

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
    titleProps.textColor = Composition::Color::create8Bit(Composition::Color::White8);
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
    descProps.textColor = Composition::Color::create8Bit(0xCCCCCC);
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
    buttonRow->addChild(slowHover);

    // Snap (no transition) — instant state change for comparison.
    ButtonProps snapProps;
    snapProps.text = U"Snap";
    snapProps.hoverTransitionDuration = 0.f;
    auto snapBtn = make<Button>(
        Composition::Rect{{0, 0}, 80.f, 32.f}, snapProps);
    buttonRow->addChild(snapBtn);

    // Disabled — dimmed at 0.4 alpha, never enters hover/pressed.
    ButtonProps disabledProps;
    disabledProps.text = U"Disabled";
    disabledProps.enabled = false;
    auto disabledBtn = make<Button>(
        Composition::Rect{{0, 0}, 100.f, 32.f}, disabledProps);
    buttonRow->addChild(disabledBtn);

    StackSlot buttonRowSlot;
    buttonRowSlot.flexGrow = 0.f;
    root->addChild(buttonRow, buttonRowSlot);

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
