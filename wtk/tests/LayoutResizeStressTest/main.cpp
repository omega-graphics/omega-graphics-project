#include "omegaWTK/UI/Layout.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace OmegaWTK;

static constexpr float kEpsilon = 0.001f;

static bool nearEq(float a,float b){
    return std::fabs(a - b) <= kEpsilon;
}

static void testResolveLengthBasic(){
    assert(nearEq(resolveLength(LayoutLength::Dp(50.f),200.f,1.f),50.f));
    assert(nearEq(resolveLength(LayoutLength::Px(100.f),200.f,2.f),50.f));
    assert(nearEq(resolveLength(LayoutLength::Percent(0.5f),200.f,1.f),100.f));
    assert(nearEq(resolveLength(LayoutLength::Auto(),200.f,1.f),200.f));
    assert(nearEq(resolveLength(LayoutLength::Intrinsic(),300.f,1.f),300.f));
    assert(nearEq(resolveLength(LayoutLength::Fr(2.f),100.f,1.f),2.f));
    std::printf("  [PASS] testResolveLengthBasic\n");
}

static void testClampValue(){
    float val = clampValue(50.f,
                           LayoutLength::Dp(10.f),
                           LayoutLength::Dp(80.f),
                           200.f,1.f);
    assert(nearEq(val,50.f));

    val = clampValue(5.f,
                     LayoutLength::Dp(10.f),
                     LayoutLength::Dp(80.f),
                     200.f,1.f);
    assert(nearEq(val,10.f));

    val = clampValue(100.f,
                     LayoutLength::Dp(10.f),
                     LayoutLength::Dp(80.f),
                     200.f,1.f);
    assert(nearEq(val,80.f));

    val = clampValue(150.f,
                     LayoutLength::Auto(),
                     LayoutLength::Auto(),
                     200.f,1.f);
    assert(nearEq(val,150.f));

    std::printf("  [PASS] testClampValue\n");
}

static void testResolveClampedRect(){
    LayoutStyle style {};
    style.width = LayoutLength::Dp(100.f);
    style.height = LayoutLength::Dp(60.f);

    Core::Rect avail {Core::Position{0.f,0.f},400.f,300.f};
    auto rect = resolveClampedRect(style,avail,1.f);
    assert(nearEq(rect.w,100.f));
    assert(nearEq(rect.h,60.f));

    style.width = LayoutLength::Percent(0.5f);
    style.height = LayoutLength::Percent(0.25f);
    rect = resolveClampedRect(style,avail,1.f);
    assert(nearEq(rect.w,200.f));
    assert(nearEq(rect.h,75.f));

    std::printf("  [PASS] testResolveClampedRect\n");
}

static void testDpiConversion(){
    LayoutContext ctx {};
    ctx.availableRectPx = {Core::Position{0.f,0.f},400.f,300.f};
    ctx.dpiScale = 2.f;
    assert(nearEq(ctx.dpToPx(50.f),100.f));

    auto dp = ctx.availableRectDp();
    assert(nearEq(dp.w,200.f));
    assert(nearEq(dp.h,150.f));

    std::printf("  [PASS] testDpiConversion\n");
}

static void testMultiDpiDeterminism(){
    const float dpiScales[] = {1.0f,1.25f,1.5f,2.0f};

    LayoutStyle style {};
    style.width = LayoutLength::Dp(120.f);
    style.height = LayoutLength::Dp(80.f);
    style.clamp.minWidth = LayoutLength::Dp(50.f);
    style.clamp.minHeight = LayoutLength::Dp(30.f);
    style.clamp.maxWidth = LayoutLength::Dp(200.f);
    style.clamp.maxHeight = LayoutLength::Dp(150.f);

    for(float dpi : dpiScales){
        LayoutContext ctx {};
        ctx.availableRectPx = {Core::Position{0.f,0.f},800.f * dpi,600.f * dpi};
        ctx.dpiScale = dpi;
        auto avail = ctx.availableRectDp();

        auto rectDp = resolveClampedRect(style,avail,dpi);
        assert(nearEq(rectDp.w,120.f));
        assert(nearEq(rectDp.h,80.f));

        float rectPxW = rectDp.w * dpi;
        float rectPxH = rectDp.h * dpi;
        assert(nearEq(rectPxW,120.f * dpi));
        assert(nearEq(rectPxH,80.f * dpi));
    }

    std::printf("  [PASS] testMultiDpiDeterminism\n");
}

static void testResizeStress(){
    LayoutStyle style {};
    style.width = LayoutLength::Percent(0.5f);
    style.height = LayoutLength::Percent(0.3f);
    style.clamp.minWidth = LayoutLength::Dp(20.f);
    style.clamp.minHeight = LayoutLength::Dp(15.f);
    style.clamp.maxWidth = LayoutLength::Dp(500.f);
    style.clamp.maxHeight = LayoutLength::Dp(400.f);

    constexpr int kIterations = 1000;
    const float dpiScales[] = {1.0f,1.25f,1.5f,2.0f};

    for(float dpi : dpiScales){
        for(int i = 0; i < kIterations; ++i){
            float parentW = 100.f + static_cast<float>(i) * 1.5f;
            float parentH = 80.f + static_cast<float>(i) * 1.2f;

            LayoutContext ctx {};
            ctx.availableRectPx = {Core::Position{0.f,0.f},parentW * dpi,parentH * dpi};
            ctx.dpiScale = dpi;
            auto avail = ctx.availableRectDp();

            auto rectDp = resolveClampedRect(style,avail,dpi);

            assert(std::isfinite(rectDp.w) && rectDp.w >= 1.f);
            assert(std::isfinite(rectDp.h) && rectDp.h >= 1.f);

            auto rectDp2 = resolveClampedRect(style,avail,dpi);
            assert(nearEq(rectDp.w,rectDp2.w));
            assert(nearEq(rectDp.h,rectDp2.h));
            assert(nearEq(rectDp.pos.x,rectDp2.pos.x));
            assert(nearEq(rectDp.pos.y,rectDp2.pos.y));
        }
    }

    std::printf("  [PASS] testResizeStress (%d iterations x %zu DPI scales)\n",
                kIterations,sizeof(dpiScales)/sizeof(dpiScales[0]));
}

static void testLayoutDeltaComputation(){
    Core::Rect from {Core::Position{10.f,20.f},100.f,80.f};
    Core::Rect to {Core::Position{15.f,20.f},100.f,90.f};
    auto delta = computeLayoutDelta(from,to);
    assert(delta.changedProperties.size() == 2);

    bool hasX = false;
    bool hasH = false;
    for(auto p : delta.changedProperties){
        if(p == LayoutTransitionProperty::X) hasX = true;
        if(p == LayoutTransitionProperty::Height) hasH = true;
    }
    assert(hasX);
    assert(hasH);

    Core::Rect same {Core::Position{0.f,0.f},100.f,100.f};
    auto noDelta = computeLayoutDelta(same,same);
    assert(noDelta.changedProperties.empty());

    std::printf("  [PASS] testLayoutDeltaComputation\n");
}

static void testDiagnosticSink(){
    VectorDiagnosticSink sink;
    LayoutDiagnosticEntry entry {};
    entry.nodeId = "testNode";
    entry.rectDp = {Core::Position{0.f,0.f},100.f,50.f};
    entry.rectPx = {Core::Position{0.f,0.f},200.f,100.f};
    entry.pass = LayoutDiagnosticEntry::Pass::Arrange;
    sink.record(entry);
    assert(sink.entries().size() == 1);
    assert(sink.entries()[0].nodeId == "testNode");
    sink.clear();
    assert(sink.entries().empty());
    std::printf("  [PASS] testDiagnosticSink\n");
}

static void testStyleRuleConversion(){
    StyleRule a {};
    a.specificity = 2;
    a.sourceOrder = 0;

    StyleRule b {};
    b.specificity = 1;
    b.sourceOrder = 5;

    assert(a.beats(b));
    assert(!b.beats(a));

    StyleRule c {};
    c.specificity = 2;
    c.sourceOrder = 3;
    assert(c.beats(a));

    std::printf("  [PASS] testStyleRuleConversion\n");
}

static void testMergeLayoutRulesIntoStyle(){
    OmegaCommon::Vector<StyleRule> rules {};

    StyleRule widthRule {};
    widthRule.selectorTag = "box";
    widthRule.specificity = 2;
    widthRule.sourceOrder = 0;
    widthRule.property = StyleRule::Property::LayoutWidth;
    widthRule.lengthValue = LayoutLength::Dp(150.f);
    rules.push_back(widthRule);

    StyleRule heightRule {};
    heightRule.selectorTag = "box";
    heightRule.specificity = 2;
    heightRule.sourceOrder = 1;
    heightRule.property = StyleRule::Property::LayoutHeight;
    heightRule.lengthValue = LayoutLength::Percent(0.5f);
    rules.push_back(heightRule);

    LayoutStyle style {};
    mergeLayoutRulesIntoStyle(style,rules,"box");
    assert(style.width.unit == LayoutUnit::Dp && nearEq(style.width.value,150.f));
    assert(style.height.unit == LayoutUnit::Percent && nearEq(style.height.value,0.5f));

    LayoutStyle otherStyle {};
    mergeLayoutRulesIntoStyle(otherStyle,rules,"label");
    assert(otherStyle.width.isAuto());
    assert(otherStyle.height.isAuto());

    std::printf("  [PASS] testMergeLayoutRulesIntoStyle\n");
}

static void testAspectRatioResolve(){
    LayoutStyle style {};
    style.width = LayoutLength::Dp(200.f);
    style.height = LayoutLength::Auto();
    style.aspectRatio = 2.f;

    Core::Rect avail {Core::Position{0.f,0.f},400.f,300.f};
    auto rect = resolveClampedRect(style,avail,1.f);
    assert(nearEq(rect.w,200.f));
    assert(nearEq(rect.h,100.f));

    std::printf("  [PASS] testAspectRatioResolve\n");
}

static void testAbsolutePositioning(){
    LayoutStyle style {};
    style.position = LayoutPositionMode::Absolute;
    style.width = LayoutLength::Dp(80.f);
    style.height = LayoutLength::Dp(40.f);
    style.insetLeft = LayoutLength::Dp(10.f);
    style.insetTop = LayoutLength::Dp(20.f);

    Core::Rect avail {Core::Position{0.f,0.f},400.f,300.f};
    auto rect = resolveClampedRect(style,avail,1.f);
    assert(nearEq(rect.pos.x,10.f));
    assert(nearEq(rect.pos.y,20.f));
    assert(nearEq(rect.w,80.f));
    assert(nearEq(rect.h,40.f));

    std::printf("  [PASS] testAbsolutePositioning\n");
}

int main(int argc,char **argv){
    (void)argc;
    (void)argv;
    std::printf("LayoutResizeStressTest\n");
    std::printf("======================\n");

    testResolveLengthBasic();
    testClampValue();
    testResolveClampedRect();
    testDpiConversion();
    testMultiDpiDeterminism();
    testResizeStress();
    testLayoutDeltaComputation();
    testDiagnosticSink();
    testStyleRuleConversion();
    testMergeLayoutRulesIntoStyle();
    testAspectRatioResolve();
    testAbsolutePositioning();

    std::printf("\nAll tests passed.\n");
    return 0;
}
