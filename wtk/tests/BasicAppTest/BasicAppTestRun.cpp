#include <omegaWTK/UI/Widget.h>
#include <omegaWTK/UI/CanvasView.h>
#include <omegaWTK/UI/AppWindow.h>
#include <omegaWTK/UI/App.h>
#include <omegaWTK/UI/Menu.h>
#include <omegaWTK/UI/Notification.h>
#include <omegaWTK/Widgets/Primatives.h>
#include <omegaWTK/Widgets/Containers.h>
#include <omegaWTK/Widgets/BasicWidgets.h>

using namespace OmegaWTK;

// ---------------------------------------------------------------------------
// Menu delegate — responds to menu item selections
// ---------------------------------------------------------------------------

static AppWindow *g_mainWindow = nullptr;

class TestMenuDelegate final : public MenuDelegate {
public:
    void onSelectItem(unsigned itemIndex) override {
        switch (itemIndex) {
        case 0: {
            // "Open" — show a file dialog
            if (g_mainWindow) {
                g_mainWindow->openFSDialog(
                    {Native::NativeFSDialog::Read, "."});
            }
            break;
        }
        case 1:
            // separator — unreachable
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
        // Edit menu items — placeholder for future clipboard actions
        (void)itemIndex;
    }
};

class HelpMenuDelegate final : public MenuDelegate {
public:
    void onSelectItem(unsigned itemIndex) override {
        if (itemIndex == 0) {
            // "About" — send a system notification
            NotificationCenter nc;
            nc.send({"BasicAppTest", "OmegaWTK Widget & Menu Integration Test"});
        }
    }
};

// ---------------------------------------------------------------------------
// Window delegate
// ---------------------------------------------------------------------------

class TestWindowDelegate final : public AppWindowDelegate {
public:
    void windowWillClose(Native::NativeEventPtr event) override {
        AppInst::terminate();
    }
};

// ---------------------------------------------------------------------------
// Build the widget tree
// ---------------------------------------------------------------------------

static WidgetPtr buildWidgetTree(const Core::Rect & bounds) {
    // Root: VStack filling the window
    auto root = make<VStack>(bounds, StackOptions{
        .spacing = 8.f,
        .padding = {16.f, 16.f, 16.f, 16.f},
        .mainAlign = StackMainAlign::Start,
        .crossAlign = StackCrossAlign::Stretch
    });

    // 1. Title label
    LabelProps titleProps;
    titleProps.text = U"BasicAppTest — Widget Integration";
    titleProps.textColor = Composition::Color::create8Bit(Composition::Color::White8);
    titleProps.alignment = Composition::TextLayoutDescriptor::MiddleCenter;
    titleProps.wrapping = Composition::TextLayoutDescriptor::None;
    auto titleLabel = make<Label>(
        Core::Rect{{0, 0}, bounds.w - 32.f, 30.f}, titleProps);
    root->addChild(titleLabel, StackSlot{.flexGrow = 0.f});

    // 2. Separator
    auto sep1 = make<Separator>(
        Core::Rect{{0, 0}, bounds.w - 32.f, 4.f},
        SeparatorProps{
            .orientation = Orientation::Horizontal,
            .thickness = 1.f,
            .inset = 0.f,
            .brush = Composition::ColorBrush(
                Composition::Color::create8Bit(0x555555))
        });
    root->addChild(sep1, StackSlot{.flexGrow = 0.f});

    // 3. Row of shape primitives in an HStack
    auto shapeRow = make<HStack>(
        Core::Rect{{0, 0}, bounds.w - 32.f, 100.f},
        StackOptions{
            .spacing = 12.f,
            .mainAlign = StackMainAlign::Center,
            .crossAlign = StackCrossAlign::Center
        });

    // Red rectangle
    auto redRect = make<Rectangle>(
        Core::Rect{{0, 0}, 80.f, 80.f},
        RectangleProps{
            .fill = Composition::ColorBrush(
                Composition::Color::create8Bit(Composition::Color::Red8))
        });
    shapeRow->addChild(redRect);

    // Blue rounded rectangle
    auto blueRR = make<RoundedRectangle>(
        Core::Rect{{0, 0}, 80.f, 80.f},
        RoundedRectangleProps{
            .fill = Composition::ColorBrush(
                Composition::Color::create8Bit(Composition::Color::Blue8)),
            .topLeft = 12.f, .topRight = 12.f,
            .bottomLeft = 12.f, .bottomRight = 12.f
        });
    shapeRow->addChild(blueRR);

    // Green ellipse
    auto greenEllipse = make<Ellipse>(
        Core::Rect{{0, 0}, 80.f, 80.f},
        EllipseProps{
            .fill = Composition::ColorBrush(
                Composition::Color::create8Bit(Composition::Color::Green8))
        });
    shapeRow->addChild(greenEllipse);

    // Yellow rectangle with border
    auto yellowRect = make<Rectangle>(
        Core::Rect{{0, 0}, 80.f, 80.f},
        RectangleProps{
            .fill = Composition::ColorBrush(
                Composition::Color::create8Bit(Composition::Color::Yellow8)),
            .stroke = Composition::ColorBrush(
                Composition::Color::create8Bit(Composition::Color::Black8)),
            .strokeWidth = 2.f
        });
    shapeRow->addChild(yellowRect);

    root->addChild(shapeRow, StackSlot{.flexGrow = 0.f});

    // 4. Another separator
    auto sep2 = make<Separator>(
        Core::Rect{{0, 0}, bounds.w - 32.f, 4.f},
        SeparatorProps{
            .orientation = Orientation::Horizontal,
            .thickness = 1.f,
            .brush = Composition::ColorBrush(
                Composition::Color::create8Bit(0x555555))
        });
    root->addChild(sep2, StackSlot{.flexGrow = 0.f});

    // 5. Description label
    LabelProps descProps;
    descProps.text = U"This test exercises shape primitives (Rectangle, RoundedRectangle, "
                     U"Ellipse), text (Label), layout (VStack/HStack), and the app menu "
                     U"system (File > Open, Help > About).";
    descProps.textColor = Composition::Color::create8Bit(0xCCCCCC);
    descProps.alignment = Composition::TextLayoutDescriptor::LeftUpper;
    descProps.wrapping = Composition::TextLayoutDescriptor::WrapByWord;
    auto descLabel = make<Label>(
        Core::Rect{{0, 0}, bounds.w - 32.f, 60.f}, descProps);
    root->addChild(descLabel, StackSlot{.flexGrow = 1.f});

    return root;
}

// ---------------------------------------------------------------------------
// Build the menu bar
// ---------------------------------------------------------------------------

static SharedHandle<Menu> buildMenuBar() {
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

    return menu;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int RunBasicAppTest(AppInst *app) {
    Core::Rect windowRect{{0, 0}, 600, 500};

    auto window = make<AppWindow>(windowRect, new TestWindowDelegate());
    g_mainWindow = window.get();

    window->setTitle("BasicAppTest");

    // Menu bar
    auto menu = buildMenuBar();
    window->setMenu(menu);

    // Widget tree
    auto root = buildWidgetTree(windowRect);
    window->setRootWidget(root);

    // Show a startup notification
    NotificationCenter nc;
    nc.send({"BasicAppTest", "Window opened with widget tree and menu bar."});

    auto & windowManager = app->windowManager;
    windowManager->setRootWindow(window);
    windowManager->displayRootWindow();

    return AppInst::start();
}
