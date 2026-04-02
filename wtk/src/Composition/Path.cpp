#include "omegaWTK/Composition/Path.h"
#include <cmath>

namespace OmegaWTK::Composition {

namespace {
constexpr float kPi = 3.14159265358979f;
}

    Path::Segment::Segment(OmegaGTE::GPoint2D startPoint):path(startPoint), final_path_a(startPoint), final_path_b(startPoint){

    };

    Path::Path(OmegaGTE::GPoint2D startPoint,unsigned initialStroke):
    segments(),
    currentStroke(initialStroke),
    arcPrecision(1.f/100.f),
    pathBrush(nullptr){
        goTo(startPoint);
    }

    void Path::goTo(OmegaGTE::GPoint2D pt) {
        segments.emplace_back(pt);
    }

    void Path::addLine(OmegaGTE::GPoint2D dest_pt) {
        auto & current = segments.back();
        auto start = current.path.lastPt();
        const float dx = dest_pt.x - start.x;
        const float dy = dest_pt.y - start.y;
        const float len = std::sqrt((dx * dx) + (dy * dy));
        if(len <= 0.000001f){
            return;
        }

        const float halfStroke = float(currentStroke) * 0.5f;
        const float nx = -dy / len;
        const float ny = dx / len;
        const float delta_x = nx * halfStroke;
        const float delta_y = ny * halfStroke;

        auto & leftStart = current.final_path_a.lastPt();
        leftStart.x += delta_x;
        leftStart.y += delta_y;

        auto & rightStart = current.final_path_b.lastPt();
        rightStart.x -= delta_x;
        rightStart.y -= delta_y;

        current.path.append(dest_pt);

        OmegaGTE::GPoint2D leftEnd = dest_pt;
        leftEnd.x += delta_x;
        leftEnd.y += delta_y;
        current.final_path_a.append(leftEnd);

        OmegaGTE::GPoint2D rightEnd = dest_pt;
        rightEnd.x -= delta_x;
        rightEnd.y -= delta_y;
        current.final_path_b.append(rightEnd);

    }

    void Path::addArc(OmegaGTE::GRect bounds,float startAngle,float endAngle) {
        auto & current = segments.back();

        auto pivot_x = bounds.pos.x + (bounds.w/2.f);
        auto pivot_y = bounds.pos.y + (bounds.h/2.f);

        auto width = float(currentStroke)/2.f;

        auto rad_x = bounds.w/2.f;
        auto rad_y = bounds.h/2.f;

        auto angle_it = startAngle;

        while(angle_it <= endAngle){
            auto _cos = std::cos(angle_it);
            auto _sin = std::sin(angle_it);

            current.path.append(OmegaGTE::GPoint2D {
                pivot_x + rad_x * _cos,
                pivot_y + rad_y * _sin});

            current.final_path_a.append(OmegaGTE::GPoint2D {
                pivot_x + (rad_x - width) * _cos,
                pivot_y + (rad_y - width) * _sin});

            current.final_path_b.append(OmegaGTE::GPoint2D {
                pivot_x + (rad_x + width) * _cos,
                pivot_y + (rad_y + width) * _sin});

            angle_it += arcPrecision;
        }

    }

    Path::Path(OmegaGTE::GVectorPath2D & path,unsigned stroke):segments(),
                                                               currentStroke(stroke),
                                                               arcPrecision(1.f/100.f),
                                                               pathBrush(nullptr){
        auto path_it = path.begin();
        for(;path_it != path.end();path_it.operator++()){
            auto segment = *path_it;
            if(path_it == path.begin()){
                goTo(*segment.pt_A);
            }
            addLine(*segment.pt_B);
        }
    }

    void Path::setStroke(unsigned int newStroke) {
        assert(newStroke > 0 && "Stroke must be greater than 0");
        currentStroke = newStroke;
    };

    void Path::setArcPrecision(unsigned int newPrecision) {
        assert(newPrecision > 1 && "Precison must be greater than 1");
        arcPrecision = 1.f/float(newPrecision);
    }

    void Path::setPathBrush(Core::SharedPtr<Brush> &brush) {
        pathBrush = brush;
    }

    void Path::close(){
        if(segments.back().path.size() >= 2){
            segments.back().closed = true;
        }
    }

    Core::SharedPtr<Path> RectFrame(Core::Rect rect, unsigned width) {
        auto path = std::make_shared<Path>(
            OmegaGTE::GPoint2D{rect.pos.x, rect.pos.y},
            width);
        path->addLine(OmegaGTE::GPoint2D{rect.pos.x + rect.w, rect.pos.y});
        path->addLine(OmegaGTE::GPoint2D{rect.pos.x + rect.w, rect.pos.y + rect.h});
        path->addLine(OmegaGTE::GPoint2D{rect.pos.x, rect.pos.y + rect.h});
        path->close();
        return path;
    }

    Core::SharedPtr<Path> RoundedRectFrame(Core::RoundedRect rect, unsigned width) {
        float rad_x = rect.rad_x;
        float rad_y = rect.rad_y;
        if (rad_x > rect.w * 0.5f) rad_x = rect.w * 0.5f;
        if (rad_y > rect.h * 0.5f) rad_y = rect.h * 0.5f;
        if (rad_x < 0.f) rad_x = 0.f;
        if (rad_y < 0.f) rad_y = 0.f;

        const float x = rect.pos.x;
        const float y = rect.pos.y;
        const float w = rect.w;
        const float h = rect.h;

        // Counterclockwise winding for Y-up: bottom → right → top → left.
        auto path = std::make_shared<Path>(
            OmegaGTE::GPoint2D{x + rad_x, y},
            width);

        // Bottom edge →
        path->addLine(OmegaGTE::GPoint2D{x + w - rad_x, y});

        // Bottom-right arc (3π/2 → 2π)
        OmegaGTE::GRect brArc{{x + w - rad_x * 2.f, y}, rad_x * 2.f, rad_y * 2.f};
        path->addArc(brArc, kPi * 1.5f, kPi * 2.f);

        // Right edge ↑
        path->addLine(OmegaGTE::GPoint2D{x + w, y + h - rad_y});

        // Top-right arc (0 → π/2)
        OmegaGTE::GRect trArc{{x + w - rad_x * 2.f, y + h - rad_y * 2.f}, rad_x * 2.f, rad_y * 2.f};
        path->addArc(trArc, 0.f, kPi * 0.5f);

        // Top edge ←
        path->addLine(OmegaGTE::GPoint2D{x + rad_x, y + h});

        // Top-left arc (π/2 → π)
        OmegaGTE::GRect tlArc{{x, y + h - rad_y * 2.f}, rad_x * 2.f, rad_y * 2.f};
        path->addArc(tlArc, kPi * 0.5f, kPi);

        // Left edge ↓
        path->addLine(OmegaGTE::GPoint2D{x, y + rad_y});

        // Bottom-left arc (π → 3π/2)
        OmegaGTE::GRect blArc{{x, y}, rad_x * 2.f, rad_y * 2.f};
        path->addArc(blArc, kPi, kPi * 1.5f);

        path->close();
        return path;
    }

    Core::SharedPtr<Path> EllipseFrame(Core::Ellipse ellipse, unsigned width) {
        OmegaGTE::GRect bounds{
            {ellipse.x - ellipse.rad_x, ellipse.y - ellipse.rad_y},
            ellipse.rad_x * 2.f,
            ellipse.rad_y * 2.f
        };
        auto path = std::make_shared<Path>(
            OmegaGTE::GPoint2D{ellipse.x + ellipse.rad_x, ellipse.y},
            width);
        path->addArc(bounds, 0.f, kPi * 2.f);
        path->close();
        return path;
    }

};
