#include "omegaWTK/UI/Layout.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace OmegaWTK;

static bool approx(float a, float b, float eps = 0.001f){
    return std::fabs(a - b) <= eps;
}

static void testLayoutLengthFactories(){
    auto a = LayoutLength::Auto();
    assert(a.isAuto());
    assert(!a.isFixed());
    assert(!a.isIntrinsic());

    auto px = LayoutLength::Px(100.f);
    assert(px.unit == LayoutUnit::Px);
    assert(px.isFixed());
    assert(approx(px.value, 100.f));

    auto dp = LayoutLength::Dp(50.f);
    assert(dp.unit == LayoutUnit::Dp);
    assert(dp.isFixed());

    auto pct = LayoutLength::Percent(0.5f);
    assert(pct.unit == LayoutUnit::Percent);
    assert(!pct.isFixed());

    auto fr = LayoutLength::Fr(2.f);
    assert(fr.unit == LayoutUnit::Fr);
    assert(!fr.isFixed());

    auto intr = LayoutLength::Intrinsic();
    assert(intr.isIntrinsic());
    assert(!intr.isAuto());

    std::printf("  [PASS] LayoutLength factories\n");
}

static void testLayoutEdges(){
    auto z = LayoutEdges::Zero();
    assert(approx(z.left.value, 0.f));
    assert(approx(z.top.value, 0.f));
    assert(approx(z.right.value, 0.f));
    assert(approx(z.bottom.value, 0.f));

    auto all = LayoutEdges::All(LayoutLength::Dp(10.f));
    assert(approx(all.left.value, 10.f));
    assert(approx(all.bottom.value, 10.f));

    auto sym = LayoutEdges::Symmetric(LayoutLength::Dp(5.f), LayoutLength::Dp(20.f));
    assert(approx(sym.left.value, 5.f));
    assert(approx(sym.right.value, 5.f));
    assert(approx(sym.top.value, 20.f));
    assert(approx(sym.bottom.value, 20.f));

    std::printf("  [PASS] LayoutEdges factories\n");
}

static void testResolveLength(){
    const float avail = 200.f;
    const float dpi = 2.f;

    assert(approx(resolveLength(LayoutLength::Auto(), avail, dpi), 200.f));
    assert(approx(resolveLength(LayoutLength::Px(100.f), avail, dpi), 50.f));
    assert(approx(resolveLength(LayoutLength::Dp(80.f), avail, dpi), 80.f));
    assert(approx(resolveLength(LayoutLength::Percent(0.5f), avail, dpi), 100.f));
    assert(approx(resolveLength(LayoutLength::Fr(3.f), avail, dpi), 3.f));
    assert(approx(resolveLength(LayoutLength::Intrinsic(), avail, dpi), 200.f));

    assert(approx(resolveLength(LayoutLength::Px(150.f), avail, 1.f), 150.f));
    assert(approx(resolveLength(LayoutLength::Px(150.f), avail, 1.5f), 100.f));

    std::printf("  [PASS] resolveLength\n");
}

static void testClampValue(){
    const float avail = 400.f;
    const float dpi = 1.f;

    float v1 = clampValue(50.f, LayoutLength::Dp(10.f), LayoutLength::Dp(100.f), avail, dpi);
    assert(approx(v1, 50.f));

    float v2 = clampValue(5.f, LayoutLength::Dp(10.f), LayoutLength::Dp(100.f), avail, dpi);
    assert(approx(v2, 10.f));

    float v3 = clampValue(150.f, LayoutLength::Dp(10.f), LayoutLength::Dp(100.f), avail, dpi);
    assert(approx(v3, 100.f));

    float v4 = clampValue(50.f, LayoutLength::Auto(), LayoutLength::Auto(), avail, dpi);
    assert(approx(v4, 50.f));

    float v5 = clampValue(50.f, LayoutLength::Percent(0.25f), LayoutLength::Auto(), avail, dpi);
    assert(approx(v5, 100.f));

    std::printf("  [PASS] clampValue\n");
}

static void testResolveClampedRect(){
    LayoutStyle style {};
    style.width  = LayoutLength::Dp(120.f);
    style.height = LayoutLength::Dp(60.f);

    Composition::Rect avail {Composition::Point2D{0.f, 0.f}, 400.f, 300.f};
    auto result = resolveClampedRect(style, avail, 1.f);
    assert(approx(result.w, 120.f));
    assert(approx(result.h, 60.f));

    style.clamp.maxWidth = LayoutLength::Dp(80.f);
    result = resolveClampedRect(style, avail, 1.f);
    assert(approx(result.w, 80.f));

    style.width  = LayoutLength::Percent(0.5f);
    style.height = LayoutLength::Percent(0.25f);
    style.clamp.maxWidth = LayoutLength::Auto();
    result = resolveClampedRect(style, avail, 1.f);
    assert(approx(result.w, 200.f));
    assert(approx(result.h, 75.f));

    std::printf("  [PASS] resolveClampedRect\n");
}

static void testLayoutContextDpi(){
    LayoutContext ctx {};
    ctx.availableRectPx = {Composition::Point2D{0.f, 0.f}, 800.f, 600.f};

    ctx.dpiScale = 1.f;
    assert(approx(ctx.dpToPx(100.f), 100.f));
    auto dpRect = ctx.availableRectDp();
    assert(approx(dpRect.w, 800.f));
    assert(approx(dpRect.h, 600.f));

    ctx.dpiScale = 2.f;
    assert(approx(ctx.dpToPx(100.f), 200.f));
    dpRect = ctx.availableRectDp();
    assert(approx(dpRect.w, 400.f));
    assert(approx(dpRect.h, 300.f));

    ctx.dpiScale = 1.5f;
    dpRect = ctx.availableRectDp();
    assert(approx(dpRect.w, 800.f / 1.5f));
    assert(approx(dpRect.h, 600.f / 1.5f));

    ctx.dpiScale = 1.25f;
    assert(approx(ctx.dpToPx(80.f), 100.f));

    std::printf("  [PASS] LayoutContext DPI conversions\n");
}

static void testMultiDpiResolveLength(){
    float dpis[] = {1.f, 1.25f, 1.5f, 2.f};
    for(float dpi : dpis){
        float avail = 500.f / dpi;
        float resolvedPx = resolveLength(LayoutLength::Px(200.f), avail, dpi);
        assert(approx(resolvedPx, 200.f / dpi));

        float resolvedDp = resolveLength(LayoutLength::Dp(100.f), avail, dpi);
        assert(approx(resolvedDp, 100.f));

        float resolvedPct = resolveLength(LayoutLength::Percent(0.5f), avail, dpi);
        assert(approx(resolvedPct, avail * 0.5f));
    }
    std::printf("  [PASS] Multi-DPI resolveLength\n");
}

int main(){
    std::printf("LayoutUnitTest\n");

    testLayoutLengthFactories();
    testLayoutEdges();
    testResolveLength();
    testClampValue();
    testResolveClampedRect();
    testLayoutContextDpi();
    testMultiDpiResolveLength();

    std::printf("\nAll layout unit tests passed.\n");
    return 0;
}
