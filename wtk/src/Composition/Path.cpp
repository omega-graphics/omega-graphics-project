#include "PathImpl.h"
#include "backend/GeometryConvert.h"
#include <cmath>

namespace OmegaWTK::Composition {

namespace {
constexpr float kPi = 3.14159265358979f;
}

    Path::Path(Point2D startPoint,unsigned initialStroke):
    impl_(std::make_unique<Impl>()){
        impl_->currentStroke = initialStroke;
        impl_->arcPrecision = 1.f/100.f;
        impl_->pathBrush = nullptr;
        goTo(startPoint);
    }

    void Path::goTo(Point2D pt) {
        impl_->segments.emplace_back(toGTE(pt));
    }

    void Path::addLine(Point2D dest_pt) {
        auto & current = impl_->segments.back();
        auto start = current.path.lastPt();
        auto gDest = toGTE(dest_pt);
        const float dx = gDest.x - start.x;
        const float dy = gDest.y - start.y;
        const float len = std::sqrt((dx * dx) + (dy * dy));
        if(len <= 0.000001f){
            return;
        }

        const float halfStroke = float(impl_->currentStroke) * 0.5f;
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

        current.path.append(gDest);

        OmegaGTE::GPoint2D leftEnd = gDest;
        leftEnd.x += delta_x;
        leftEnd.y += delta_y;
        current.final_path_a.append(leftEnd);

        OmegaGTE::GPoint2D rightEnd = gDest;
        rightEnd.x -= delta_x;
        rightEnd.y -= delta_y;
        current.final_path_b.append(rightEnd);

    }

    void Path::addArc(Rect bounds,float startAngle,float endAngle) {
        auto & current = impl_->segments.back();

        auto pivot_x = bounds.pos.x + (bounds.w/2.f);
        auto pivot_y = bounds.pos.y + (bounds.h/2.f);

        auto width = float(impl_->currentStroke)/2.f;

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

            angle_it += impl_->arcPrecision;
        }

    }

    Path::Path(OmegaGTE::GVectorPath2D & path,unsigned stroke):
    impl_(std::make_unique<Impl>()){
        impl_->currentStroke = stroke;
        impl_->arcPrecision = 1.f/100.f;
        impl_->pathBrush = nullptr;
        auto path_it = path.begin();
        for(;path_it != path.end();path_it.operator++()){
            auto segment = *path_it;
            if(path_it == path.begin()){
                goTo(fromGTE(*segment.pt_A));
            }
            addLine(fromGTE(*segment.pt_B));
        }
    }

    void Path::setStroke(unsigned int newStroke) {
        assert(newStroke > 0 && "Stroke must be greater than 0");
        impl_->currentStroke = newStroke;
    };

    void Path::setArcPrecision(unsigned int newPrecision) {
        assert(newPrecision > 1 && "Precison must be greater than 1");
        impl_->arcPrecision = 1.f/float(newPrecision);
    }

    void Path::setPathBrush(Core::SharedPtr<Brush> &brush) {
        impl_->pathBrush = brush;
    }

    void Path::close(){
        if(impl_->segments.back().path.size() >= 2){
            impl_->segments.back().closed = true;
        }
    }

    OmegaCommon::Vector<Point2D> Path::getControlPoints() const {
        OmegaCommon::Vector<Point2D> result;
        if(impl_->segments.empty()) return result;
        auto & gpath = impl_->segments[0].path;
        auto copy = gpath;
        auto first = copy.firstPt();
        result.push_back(fromGTE(first));
        for(auto it = copy.begin(); it != copy.end(); ++it){
            auto seg = *it;
            if(seg.pt_B != nullptr){
                result.push_back(fromGTE(*seg.pt_B));
            }
        }
        return result;
    }

    Path Path::fromControlPoints(const OmegaCommon::Vector<Point2D> & points,unsigned stroke){
        if(points.empty()){
            return Path(Point2D{0.f,0.f},stroke);
        }
        Path p(points.front(),stroke);
        for(std::size_t i = 1; i < points.size(); i++){
            p.addLine(points[i]);
        }
        return p;
    }

    Path::Path(Path && other) noexcept = default;
    Path & Path::operator=(Path && other) noexcept = default;

    Path::Path(const Path & other)
        : impl_(other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr) {}

    Path & Path::operator=(const Path & other){
        if(this != &other){
            impl_ = other.impl_ ? std::make_unique<Impl>(*other.impl_) : nullptr;
        }
        return *this;
    }

    Path::~Path() = default;

    Core::SharedPtr<Path> RectFrame(Composition::Rect rect, unsigned width) {
        auto path = std::make_shared<Path>(
            Point2D{rect.pos.x, rect.pos.y},
            width);
        path->addLine(Point2D{rect.pos.x + rect.w, rect.pos.y});
        path->addLine(Point2D{rect.pos.x + rect.w, rect.pos.y + rect.h});
        path->addLine(Point2D{rect.pos.x, rect.pos.y + rect.h});
        path->close();
        return path;
    }

    Core::SharedPtr<Path> RoundedRectFrame(Composition::RoundedRect rect, unsigned width) {
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

        auto path = std::make_shared<Path>(
            Point2D{x + rad_x, y},
            width);

        // Bottom edge
        path->addLine(Point2D{x + w - rad_x, y});

        // Bottom-right arc
        path->addArc(Rect{{x + w - rad_x * 2.f, y}, rad_x * 2.f, rad_y * 2.f},
                     kPi * 1.5f, kPi * 2.f);

        // Right edge
        path->addLine(Point2D{x + w, y + h - rad_y});

        // Top-right arc
        path->addArc(Rect{{x + w - rad_x * 2.f, y + h - rad_y * 2.f}, rad_x * 2.f, rad_y * 2.f},
                     0.f, kPi * 0.5f);

        // Top edge
        path->addLine(Point2D{x + rad_x, y + h});

        // Top-left arc
        path->addArc(Rect{{x, y + h - rad_y * 2.f}, rad_x * 2.f, rad_y * 2.f},
                     kPi * 0.5f, kPi);

        // Left edge
        path->addLine(Point2D{x, y + rad_y});

        // Bottom-left arc
        path->addArc(Rect{{x, y}, rad_x * 2.f, rad_y * 2.f},
                     kPi, kPi * 1.5f);

        path->close();
        return path;
    }

    Core::SharedPtr<Path> EllipseFrame(Composition::Ellipse ellipse, unsigned width) {
        Rect bounds{
            {ellipse.x - ellipse.rad_x, ellipse.y - ellipse.rad_y},
            ellipse.rad_x * 2.f,
            ellipse.rad_y * 2.f
        };
        auto path = std::make_shared<Path>(
            Point2D{ellipse.x + ellipse.rad_x, ellipse.y},
            width);
        path->addArc(bounds, 0.f, kPi * 2.f);
        path->close();
        return path;
    }

};
