///|/ Copyright (c) Prusa Research 2024 Filip Sykala @Jony01
///|/
///|/ PrusaSlicer is released under the terms of the AGPLv3 or higher
///|/

#include "SupportPointGenerator.hpp"

#include "libslic3r/Execution/ExecutionTBB.hpp" // parallel preparation of data for sampling
#include "libslic3r/Execution/Execution.hpp"
#include "libslic3r/KDTreeIndirect.hpp"
#include "libslic3r/ClipperUtils.hpp"

// SupportIslands
#include "libslic3r/SLA/SupportIslands/UniformSupportIsland.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

namespace {
/// <summary>
/// Struct to store support points in KD tree to fast search for nearest ones.
/// </summary>
class NearPoints
{
    /// <summary>
    /// Structure made for KDTree as function to 
    /// acess support point coordinate by index into global support point storage
    /// </summary>
    struct PointAccessor {
        // multiple trees points into same data storage of all support points
        LayerSupportPoints *m_supports_ptr;
        explicit PointAccessor(LayerSupportPoints *supports_ptr) : m_supports_ptr(supports_ptr) {}
        // accessor to coordinate for kd tree
        const coord_t &operator()(size_t idx, size_t dimension) const {
            return m_supports_ptr->at(idx).position_on_layer[dimension];
        }
    };

    PointAccessor m_points;
    using Tree = KDTreeIndirect<2, coord_t, PointAccessor>;
    Tree m_tree;
public:
    /// <summary>
    /// Constructor get pointer on the global storage of support points
    /// </summary>
    /// <param name="supports_ptr">Pointer on Support vector</param>
    explicit NearPoints(LayerSupportPoints* supports_ptr)
        : m_points(supports_ptr), m_tree(m_points) {}

    NearPoints get_copy(){ 
        NearPoints copy(m_points.m_supports_ptr);
        copy.m_tree = m_tree.get_copy(); // copy tree
        return copy;
    }

    /// <summary>
    /// Remove support points from KD-Tree which lay out of expolygons
    /// </summary>
    /// <param name="shapes">Define area where could be support points</param>
    void remove_out_of(const ExPolygons &shapes) {
        std::vector<size_t> indices = get_indices();
        auto it = std::remove_if(indices.begin(), indices.end(), 
            [&pts = *m_points.m_supports_ptr, &shapes](size_t point_index) {
                const Point& p = pts.at(point_index).position_on_layer;
                return !std::any_of(shapes.begin(), shapes.end(), 
                    [&p](const ExPolygon &shape) {
                        return shape.contains(p);
                    });
            });
        if (it == indices.end())
            return; // no point to remove
        indices.erase(it, indices.end());
        m_tree.clear();
        m_tree.build(indices); // consume indices
    }

    /// <summary>
    /// Add point new support point into global storage of support points 
    /// and pointer into tree structure of nearest points
    /// </summary>
    /// <param name="point">New added point</param>
    void add(LayerSupportPoint &&point) {
        // IMPROVE: only add to existing tree, do not reconstruct tree
        std::vector<size_t> indices = get_indices();
        LayerSupportPoints &pts = *m_points.m_supports_ptr;
        size_t index = pts.size();
        pts.emplace_back(std::move(point));
        indices.push_back(index);
        m_tree.clear();
        m_tree.build(indices); // consume indices
    }

    using CheckFnc = std::function<bool(const LayerSupportPoint &, const Point&)>;
    /// <summary>
    /// Iterate over support points in 2d radius and search any of fnc with True.
    /// Made for check wheather surrounding supports support current point p.
    /// </summary>
    /// <param name="pos">Center of search circle</param>
    /// <param name="radius">Search circle radius</param>
    /// <param name="fnc">Function to check supports point</param>
    /// <returns>True wheny any of check function return true, otherwise False</returns>
    bool exist_true_in_radius(const Point &pos, coord_t radius, const CheckFnc &fnc) const {
        std::vector<size_t> point_indices = find_nearby_points(m_tree, pos, radius);
        return std::any_of(point_indices.begin(), point_indices.end(), 
            [&points = *m_points.m_supports_ptr, &pos, &fnc](size_t point_index){
                return fnc(points.at(point_index), pos);
            });
    }

    /// <summary>
    /// Merge another tree structure into current one.
    /// Made for connection of two mesh parts.
    /// </summary>
    /// <param name="near_point">Another near points</param>
    void merge(NearPoints &&near_point) {
        // need to have same global storage of support points
        assert(m_points.m_supports_ptr == near_point.m_points.m_supports_ptr);

        // IMPROVE: merge trees instead of rebuild
        std::vector<size_t> indices = get_indices();
        std::vector<size_t> indices2 = near_point.get_indices();
        // merge
        indices.insert(indices.end(),
            std::move_iterator(indices2.begin()), 
            std::move_iterator(indices2.end()));
        // remove duplicit indices - Diamond case
        std::sort(indices.begin(), indices.end());
        auto it = std::unique(indices.begin(), indices.end());
        indices.erase(it, indices.end());
        // rebuild tree
        m_tree.clear();
        m_tree.build(indices); // consume indices
    }

private:
    std::vector<size_t> get_indices() const {
        std::vector<size_t> indices = m_tree.get_nodes(); // copy
        // nodes in tree contain "max values for size_t" on unused leaf of tree,
        // when count of indices is not exactly power of 2
        auto it = std::remove_if(indices.begin(), indices.end(), 
            [max_index = m_points.m_supports_ptr->size()](size_t i) { return i >= max_index; });
        indices.erase(it, indices.end());
        return indices;
    }
};

/// <summary>
/// Intersection of line segment and circle
/// </summary>
/// <param name="p1">Line segment point A, Point lay inside circle</param>
/// <param name="p2">Line segment point B, Point lay outside or on circle</param>
/// <param name="cnt">Circle center point</param>
/// <param name="r2">squared value of Circle Radius (r2 = r*r)</param>
/// <returns>Intersection point</returns>
Point intersection_line_circle(const Point &p1, const Point &p2, const Point &cnt, double r2) {
    // Vector from p1 to p2
    Vec2d dp_d((p2 - p1).cast<double>());
    // Vector from circle center to p1
    Vec2d f_d((p1 - cnt).cast<double>());

    double a = dp_d.squaredNorm();
    double b = 2 * (f_d.x() * dp_d.x() + f_d.y() * dp_d.y());
    double c = f_d.squaredNorm() - r2;

    // Discriminant of the quadratic equation
    double discriminant = b * b - 4 * a * c;

    // No intersection if discriminant is negative
    assert(discriminant >= 0);
    if (discriminant < 0)
        return {}; // No intersection

    // Calculate the two possible values of t (parametric parameter)
    discriminant = sqrt(discriminant);
    double t1 = (-b - discriminant) / (2 * a);

    // Check for valid intersection points within the line segment
    if (t1 >= 0 && t1 <= 1) {
        return {p1.x() + t1 * dp_d.x(), p1.y() + t1 * dp_d.y()};
    }

    // should not be in use
    double t2 = (-b + discriminant) / (2 * a);
    if (t2 >= 0 && t2 <= 1 && t1 != t2) {
        return {p1.x() + t2 * dp_d.x(), p1.y() + t2 * dp_d.y()};
    }
    return {};
}

/// <summary>
/// Move grid from previous layer to current one
/// for given part
/// </summary>
/// <param name="prev_layer_parts">Grids generated in previous layer</param>
/// <param name="part">Current layer part to process</param>
/// <param name="prev_grids">Grids which will be moved to current grid</param>
/// <returns>Grid for given part</returns>
NearPoints create_near_points(
    const LayerParts &prev_layer_parts,
    const LayerPart &part,
    std::vector<NearPoints> &prev_grids
) {
    const LayerParts::const_iterator &prev_part_it = part.prev_parts.front().part_it;
    size_t index_of_prev_part = prev_part_it - prev_layer_parts.begin();
    NearPoints near_points = (prev_part_it->next_parts.size() == 1)?
        std::move(prev_grids[index_of_prev_part]) :
        // Need a copy there are multiple parts above previus one
        prev_grids[index_of_prev_part].get_copy(); // copy    

    // merge other grid in case of multiple previous parts
    for (size_t i = 1; i < part.prev_parts.size(); ++i) {
        const LayerParts::const_iterator &prev_part_it = part.prev_parts[i].part_it;
        size_t index_of_prev_part = prev_part_it - prev_layer_parts.begin();
        if (prev_part_it->next_parts.size() == 1) {
            near_points.merge(std::move(prev_grids[index_of_prev_part]));
        } else { // Need a copy there are multiple parts above previus one
            NearPoints grid_ = prev_grids[index_of_prev_part].get_copy(); // copy
            near_points.merge(std::move(grid_));
        }
    }
    return near_points;
}

/// <summary>
/// Add support point to near_points when it is neccessary
/// </summary>
/// <param name="part">Current part - keep samples</param>
/// <param name="config">Configuration to sample</param>
/// <param name="near_points">Keep previous sampled suppport points</param>
/// <param name="part_z">current z coordinate of part</param>
/// <param name="maximal_radius">Max distance to seach support for sample</param>
void support_part_overhangs(
    const LayerPart &part,
    const SupportPointGeneratorConfig &config,
    NearPoints &near_points,
    float part_z,
    coord_t maximal_radius
) {
    NearPoints::CheckFnc is_supported = []
    (const LayerSupportPoint &support_point, const Point &p) -> bool {
        // Debug visualization of all sampled outline
        //return false;
        coord_t r = support_point.current_radius;
        Point dp = support_point.position_on_layer - p;
        if (std::abs(dp.x()) > r) return false;
        if (std::abs(dp.y()) > r) return false;
        double r2 = sqr(static_cast<double>(r));
        return dp.cast<double>().squaredNorm() < r2;
    };

    for (const Point &p : part.samples) {
        if (!near_points.exist_true_in_radius(p, maximal_radius, is_supported)) {
            // not supported sample, soo create new support point
            near_points.add(LayerSupportPoint{
                SupportPoint{
                    Vec3f{unscale<float>(p.x()), unscale<float>(p.y()), part_z},
                    /* head_front_radius */ config.head_diameter / 2,
                    SupportPointType::slope
                },
                /* position_on_layer */ p,
                /* direction_to_mass */ Point(1,0), // TODO: change direction
                /* radius_curve_index */ 0,
                /* current_radius */ static_cast<coord_t>(scale_(config.support_curve.front().x()))
                });
        }    
    }
}

/// <summary>
/// Sample part as Island
/// Result store to grid
/// </summary>
/// <param name="part">Island to support</param>
/// <param name="near_points">OUT place to store new supports</param>
/// <param name="part_z">z coordinate of part</param>
/// <param name="cfg"></param>
void support_island(const LayerPart &part, NearPoints& near_points, float part_z,
    const SupportPointGeneratorConfig &cfg) {
    SupportIslandPoints samples = uniform_support_island(*part.shape, cfg.island_configuration);
    for (const SupportIslandPointPtr &sample : samples)
        near_points.add(LayerSupportPoint{
            SupportPoint{
                Vec3f{
                    unscale<float>(sample->point.x()), 
                    unscale<float>(sample->point.y()), 
                    part_z
                },
                /* head_front_radius */ cfg.head_diameter / 2,
                SupportPointType::island
            },
            /* position_on_layer */ sample->point,
            /* direction_to_mass */ Point(0,0), // direction from bottom
            /* radius_curve_index */ 0,
            /* current_radius */ static_cast<coord_t>(scale_(cfg.support_curve.front().x()))
        });
}

void support_peninsulas(const Peninsulas& peninsulas, NearPoints& near_points, float part_z,
    const SupportPointGeneratorConfig &cfg) {
    for (const Peninsula& peninsula: peninsulas) {
        SupportIslandPoints peninsula_supports =
            uniform_support_peninsula(peninsula, cfg.island_configuration);
        for (const SupportIslandPointPtr &support : peninsula_supports)
            near_points.add(LayerSupportPoint{
                SupportPoint{
                    Vec3f{
                        unscale<float>(support->point.x()), 
                        unscale<float>(support->point.y()), 
                        part_z
                    },
                    /* head_front_radius */ cfg.head_diameter / 2, 
                    SupportPointType::island
                },
                /* position_on_layer */ support->point,
                /* direction_to_mass */ Point(0, 0), // direction from bottom
                /* radius_curve_index */ 0,
                /* current_radius */ static_cast<coord_t>(scale_(cfg.support_curve.front().x()))
            });
    }   
}

/// <summary>
/// Copy parts from link to output
/// </summary>
/// <param name="part_links">Links between part of mesh</param>
/// <returns>Collected polygons from links</returns>
Polygons get_polygons(const PartLinks& part_links) {
    size_t cnt = 0;
    for (const PartLink &part_link : part_links)
        cnt += 1 + part_link.part_it->shape->holes.size();

    Polygons out;
    out.reserve(cnt);
    for (const PartLink &part_link : part_links) {
        const ExPolygon &shape = *part_link.part_it->shape;
        out.emplace_back(shape.contour);
        append(out, shape.holes);
    }
    return out;
}

/// <summary>
/// Uniformly sample Polyline,
/// Use first point and each next point is first crosing radius from last added
/// </summary>
/// <param name="b">Begin of polyline points to sample</param>
/// <param name="e">End of polyline points to sample</param>
/// <param name="dist2">Squared distance for sampling</param>
/// <returns>Uniformly distributed points laying on input polygon
/// with exception of first and last point(they are closer than dist2)</returns>
Slic3r::Points sample(Points::const_iterator b, Points::const_iterator e, double dist2) {
    assert(e-b >= 2);
    if (e - b < 2)
        return {}; // at least one line(2 points)

    // IMPROVE1: start of sampling e.g. center of Polyline
    // IMPROVE2: Random offset(To remove align of point between slices)
    // IMPROVE3: Sample small overhangs with memory for last sample(OR in center)
    Slic3r::Points r;
    r.push_back(*b);

    //Points::const_iterator prev_pt = e;
    const Point *prev_pt = nullptr;
    for (Points::const_iterator it = b; it+1 < e; ++it){        
        const Point &pt = *(it+1);
        double p_dist2 = (r.back() - pt).cast<double>().squaredNorm();
        while (p_dist2 > dist2) { // line segment goes out of radius
            if (prev_pt == nullptr)
                prev_pt = &(*it);
            r.push_back(intersection_line_circle(*prev_pt, pt, r.back(), dist2));
            p_dist2 = (r.back() - pt).cast<double>().squaredNorm();
            prev_pt = &r.back();
        }
        prev_pt = nullptr;
    }
    return r;
}

bool contain_point(const Point &p, const Points &sorted_points) {
    auto it = std::lower_bound(sorted_points.begin(), sorted_points.end(), p);
    if (it == sorted_points.end())
        return false;
    ++it; // next point should be same as searched
    if (it == sorted_points.end())
        return false;
    return it->x() == p.x() && it->y() == p.y();
};

#ifndef NDEBUG
bool exist_same_points(const ExPolygon &shape, const Points& prev_points) {
    auto shape_points = to_points(shape);
    return shape_points.end() !=
        std::find_if(shape_points.begin(), shape_points.end(), [&prev_points](const Point &p) {
            return contain_point(p, prev_points);
        });
}
#endif // NDEBUG

Points sample_overhangs(const LayerPart& part, double dist2) {
    const ExPolygon &shape = *part.shape;

    // Collect previous expolygons by links collected in loop before    
    Polygons prev_polygons = get_polygons(part.prev_parts);
    assert(!prev_polygons.empty());
    ExPolygons overhangs = diff_ex(shape, prev_polygons);    
    if (overhangs.empty()) // above part is smaller in whole contour
        return {};
    
    Points prev_points = to_points(prev_polygons);
    std::sort(prev_points.begin(), prev_points.end());

    // TODO: solve case when shape and prev points has same point
    assert(!exist_same_points(shape, prev_points));
        
    auto sample_overhang = [&prev_points, dist2](const Polygon &polygon, Points &samples) {
        const Points &pts = polygon.points;
        // first point which is not part of shape
        Points::const_iterator first_bad = pts.end();
        Points::const_iterator start_it = pts.end();
        for (auto it = pts.begin(); it != pts.end(); ++it) {
            const Point &p = *it;
            if (contain_point(p, prev_points)) {
                if (first_bad == pts.end()) {
                    // remember begining
                    first_bad = it;
                }
                if (start_it != pts.end()) {
                    // finish sampling
                    append(samples, sample(start_it, it, dist2));
                    // prepare for new start
                    start_it = pts.end();
                }
            } else if (start_it == pts.end()) {
                start_it = it;
            }
        }

        // sample last segment
        if (start_it == pts.end()) { // tail is without points
            if (first_bad != pts.begin()) // only begining
                append(samples, sample(pts.begin(), first_bad, dist2));
        } else { // tail contain points
            if (first_bad == pts.begin()) { // only tail
                append(samples, sample(start_it, pts.end(), dist2));
            } else if (start_it == pts.begin()) { // whole polygon is overhang
                assert(first_bad == pts.end());
                Points pts2 = pts; // copy
                pts2.push_back(pts.front());
                append(samples, sample(pts2.begin(), pts2.end(), dist2));
            } else { // need connect begining and tail
                Points pts2;
                pts2.reserve((pts.end() - start_it) + 
                             (first_bad - pts.begin()));
                for (auto it = start_it; it < pts.end(); ++it)
                    pts2.push_back(*it);
                for (auto it = pts.begin(); it < first_bad; ++it)
                    pts2.push_back(*it);
                append(samples, sample(pts2.begin(), pts2.end(), dist2));
            }
        }
    };

    Points samples;
    for (const ExPolygon &overhang : overhangs) {
        sample_overhang(overhang.contour, samples);
        for (const Polygon &hole : overhang.holes) {            
            sample_overhang(hole, samples);
        }
    }
    return samples;
}


void prepare_supports_for_layer(LayerSupportPoints &supports, float layer_z, 
    const SupportPointGeneratorConfig &config) {
    auto set_radius = [&config](LayerSupportPoint &support, float radius) {
        if (!is_approx(config.density_relative, 1.f, 1e-4f)) // exist relative density
            radius /= config.density_relative;
        support.current_radius = static_cast<coord_t>(scale_(radius));
    };

    const std::vector<Vec2f>& curve = config.support_curve;
    // calculate support area for each support point as radius
    // IMPROVE: use some offsets of previous supported island
    for (LayerSupportPoint &support : supports) {
        size_t &index = support.radius_curve_index;
        if (index + 1 >= curve.size())
            continue; // already contain maximal radius

        // find current segment
        float diff_z = layer_z - support.pos.z();
        while ((index + 1) < curve.size() && diff_z > curve[index + 1].y())
            ++index;

        if ((index+1) >= curve.size()) {
            // set maximal radius
            set_radius(support, curve.back().x());
            continue;
        }
        // interpolate radius on input curve
        Vec2f a = curve[index];
        Vec2f b = curve[index+1];
        assert(a.y() <= diff_z && diff_z <= b.y());
        float t = (diff_z - a.y()) / (b.y() - a.y());
        assert(0 <= t && t <= 1);
        set_radius(support, a.x() + t * (b.x() - a.x()));
    }
}

/// <summary>
/// Near points do not have to contain support points out of part.
/// Due to be able support in same area again(overhang above another overhang)
/// Wanted Side effect, it supports thiny part of overhangs
/// </summary>
/// <param name="near_points"></param>
/// <param name="part"></param>
/// <param name="config"></param>
void remove_supports_out_of_part(NearPoints& near_points, const LayerPart &part,
    const SupportPointGeneratorConfig &config) {
    ExPolygons extend_shape = offset_ex(*part.shape, config.removing_delta, ClipperLib::jtSquare);
    near_points.remove_out_of(extend_shape);
}

/// <summary>
/// Detect existence of peninsula on current layer part
/// </summary>
/// <param name="part">IN/OUT island part containing peninsulas</param>
/// <param name="min_peninsula_width">minimal width of overhang to become peninsula</param>
/// <param name="self_supported_width">supported distance from mainland</param>
void create_peninsulas(LayerPart &part, const PrepareSupportConfig &config) {
    assert(config.peninsula_min_width > config.peninsula_self_supported_width);
    const Polygons below_polygons = get_polygons(part.prev_parts);
    const Polygons below_expanded = expand(below_polygons, config.peninsula_min_width, ClipperLib::jtSquare);
    const ExPolygon &part_shape = *part.shape;
    ExPolygons over_peninsula = diff_ex(part_shape, below_expanded);
    if (over_peninsula.empty())
        return; // only tiny overhangs

    Polygons below_self_supported = expand(below_polygons, config.peninsula_self_supported_width, ClipperLib::jtSquare);
    // exist layer part over peninsula limit
    ExPolygons peninsulas_shape = diff_ex(part_shape, below_self_supported);

    // IMPROVE: Anotate source of diff by ClipperLib_Z
    Lines below_lines = to_lines(below_self_supported);
    auto get_angle = [](const Line &l) {
        Point diff = l.b - l.a;
        if (diff.x() < 0) // Only positive direction X
            diff = -diff;
        return atan2(diff.y(), diff.x());
    };
    std::vector<double> belowe_line_angle; // define direction of line with positive X
    belowe_line_angle.reserve(below_lines.size()); 
    for (const Line& l : below_lines)
        belowe_line_angle.push_back(get_angle(l));
    std::vector<size_t> idx(below_lines.size());
    std::iota(idx.begin(), idx.end(), 0);
    auto is_lower = [&belowe_line_angle](size_t i1, size_t i2) {
        return belowe_line_angle[i1] < belowe_line_angle[i2]; };
    std::sort(idx.begin(), idx.end(), is_lower);

    // Check, wheather line exist in set of belowe lines
    // True .. line exist in previous layer (or partialy overlap previous line), connection to land
    // False .. line is made by border of current layer part(peninsula coast)
    auto exist_belowe = [&get_angle, &idx, &is_lower, &below_lines, &belowe_line_angle]
    (const Line &l) {
        // allowed angle epsilon
        const double angle_epsilon = 1e-3; // < 0.06 DEG
        const double paralel_epsilon = scale_(1e-2); // 10 um
        double angle = get_angle(l);
        double low_angle = angle - angle_epsilon;
        bool is_over = false;
        if (low_angle <= -M_PI_2) {
            low_angle += M_PI;
            is_over = true;
        }
        double hi_angle = angle + angle_epsilon;
        if (hi_angle >= M_PI_2) {
            hi_angle -= M_PI;
            is_over = true;
        }
        int mayorit_idx = 0;
        if (Point d = l.a - l.b; 
            abs(d.x()) < abs(d.y()))
            mayorit_idx = 1;

        coord_t low = l.a[mayorit_idx];
        coord_t high = l.b[mayorit_idx];
        if (low > high)
            std::swap(low, high);

        auto is_lower_angle = [&belowe_line_angle](size_t index, double angle) {
            return belowe_line_angle[index] < angle; };
        auto it_idx = std::lower_bound(idx.begin(), idx.end(), low_angle, is_lower_angle);
        if (it_idx == idx.end()) {
            if (is_over) {
                it_idx = idx.begin();
                is_over = false;
            } else {
                return false;
            }
        }
        while (is_over || belowe_line_angle[*it_idx] < hi_angle) {
            const Line &l2 = below_lines[*it_idx];
            coord_t l2_low = l2.a[mayorit_idx];
            coord_t l2_high = l2.b[mayorit_idx];
            if (low > high)
                std::swap(low, high);
            if ((l2_high >= low && l2_low <= high) && (
                ((l2.a == l.a && l2.b == l.b) ||(l2.a == l.b && l2.b == l.a)) || // speed up - same line
                l.perp_distance_to(l2.a) < paralel_epsilon)) // check distance of parallel lines
                return true;
            ++it_idx;
            if (it_idx == idx.end()){
                if (is_over) {
                    it_idx = idx.begin();
                    is_over = false;
                } else {
                    break;            
                }
            }
        }
        return false;
    };

    // anotate source of peninsula: overhang VS previous layer  
    for (const ExPolygon &peninsula : peninsulas_shape) {
        // Check that peninsula is wide enough(min_peninsula_width)
        if (intersection_ex(ExPolygons{peninsula}, over_peninsula).empty())
            continue; 

        // need to know shape and edges of peninsula
        Lines lines = to_lines(peninsula);
        std::vector<bool> is_outline(lines.size());
        // when line overlap with belowe lines it is not outline
        for (size_t i = 0; i < lines.size(); i++) 
            is_outline[i] = !exist_belowe(lines[i]);
        part.peninsulas.push_back(Peninsula{peninsula, is_outline});
    }
}
} // namespace

#include "libslic3r/Execution/ExecutionSeq.hpp"
SupportPointGeneratorData Slic3r::sla::prepare_generator_data(
    std::vector<ExPolygons> &&slices,
    const std::vector<float> &heights,
    const PrepareSupportConfig &config,
    ThrowOnCancel throw_on_cancel,
    StatusFunction statusfn
) {
    // check input
    assert(!slices.empty());
    assert(slices.size() == heights.size());
    if (slices.empty() || slices.size() != heights.size())
        return SupportPointGeneratorData{};

    // Move input into result
    SupportPointGeneratorData result;
    result.slices = std::move(slices);

    // Allocate empty layers.
    result.layers = Layers(result.slices.size());

    // Generate Extents and SampleLayers
    execution::for_each(ex_tbb, size_t(0), result.slices.size(),
        [&result, &heights, throw_on_cancel](size_t layer_id) {
        if ((layer_id % 8) == 0)
            // Don't call the following function too often as it flushes
            // CPU write caches due to synchronization primitves.
            throw_on_cancel();

        Layer &layer = result.layers[layer_id];
        layer.print_z = heights[layer_id]; // copy
        const ExPolygons &islands = result.slices[layer_id];
        layer.parts.reserve(islands.size());
        for (const ExPolygon &island : islands) {
            layer.parts.push_back(LayerPart{
                &island, 
                get_extents(island.contour)
                // sample - only hangout part of expolygon could be known after linking
            });
        }        
    }, 32 /*gransize*/);

    double sample_distance_in_um = scale_(config.discretize_overhang_step);
    double sample_distance_in_um2 = sample_distance_in_um * sample_distance_in_um;

    // Link parts by intersections
    execution::for_each(ex_tbb, size_t(1), result.slices.size(),
    [&result, sample_distance_in_um2, throw_on_cancel](size_t layer_id) {
        if ((layer_id % 2) == 0)
            // Don't call the following function too often as it flushes CPU write caches due to synchronization primitves.
            throw_on_cancel();

        LayerParts &parts_above = result.layers[layer_id].parts;
        LayerParts &parts_below = result.layers[layer_id-1].parts;
        for (auto it_above = parts_above.begin(); it_above < parts_above.end(); ++it_above) {
            for (auto it_below = parts_below.begin(); it_below < parts_below.end(); ++it_below) {
                // Improve: do some sort of parts + skip some of them
                if (!it_above->shape_extent.overlap(it_below->shape_extent))
                    continue; // no bounding box overlap

                // Improve: test could be done faster way
                Polygons polys = intersection(*it_above->shape, *it_below->shape);
                if (polys.empty())
                    continue; // no intersection

                // TODO: check minimal intersection!

                it_above->prev_parts.emplace_back(PartLink{it_below});
                it_below->next_parts.emplace_back(PartLink{it_above});
            }

            if (it_above->prev_parts.empty())
                continue;

            // IMPROVE: overhangs could be calculated with Z coordninate
            // soo one will know source shape of point and do not have to search this information
            // Get inspiration at https://github.com/Prusa-Development/PrusaSlicerPrivate/blob/e00c46f070ec3d6fc325640b0dd10511f8acf5f7/src/libslic3r/PerimeterGenerator.cpp#L399
            it_above->samples = sample_overhangs(*it_above, sample_distance_in_um2);
        }
    }, 8 /* gransize */);

    // Detect peninsula
    execution::for_each(ex_tbb, size_t(1), result.slices.size(),
    [&layers = result.layers, &config, throw_on_cancel](size_t layer_id) {
        if ((layer_id % 16) == 0)
            // Don't call the following function too often as it flushes CPU write caches due to synchronization primitves.
            throw_on_cancel();
        LayerParts &parts = layers[layer_id].parts;
        for (auto it_part = parts.begin(); it_part < parts.end(); ++it_part) {
            if (it_part->prev_parts.empty())
                continue; // island
            create_peninsulas(*it_part, config);
        }
    }, 8 /* gransize */);
    return result;
}

#include "libslic3r/NSVGUtils.hpp"
#include "libslic3r/Utils.hpp"
std::vector<Vec2f> load_curve_from_file() {
    std::string filePath = Slic3r::resources_dir() + "/data/sla_support.svg";
    EmbossShape::SvgFile svg_file{filePath};
    NSVGimage *image = init_image(svg_file);
    if (image == nullptr) {
        // In test is not known resource_dir!
        // File is not located soo return DEFAULT permanent radius 5mm is returned
        return {Vec2f{5.f,.0f}, Vec2f{5.f, 1.f}};
    }
    for (NSVGshape *shape_ptr = image->shapes; shape_ptr != NULL; shape_ptr = shape_ptr->next) {
        const NSVGshape &shape = *shape_ptr;
        if (!(shape.flags & NSVG_FLAGS_VISIBLE)) continue; // is visible
        if (shape.fill.type != NSVG_PAINT_NONE) continue; // is not used fill
        if (shape.stroke.type == NSVG_PAINT_NONE) continue; // exist stroke
        if (shape.strokeWidth < 1e-5f) continue; // is visible stroke width
        if (shape.stroke.color != 4278190261) continue; // is red

        // use only first path
        const NSVGpath *path = shape.paths;
        size_t count_points = path->npts;
        assert(count_points > 1);
        --count_points;
        std::vector<Vec2f> points;
        points.reserve(count_points/3+1);
        points.push_back({path->pts[0], path->pts[1]});
        for (size_t i = 0; i < count_points; i += 3) {
            const float *p = &path->pts[i * 2];
            points.push_back({p[6], p[7]});
        }
        assert(points.size() >= 2);
        return points;
    }

    // red curve line is not found
    assert(false);
    return {};
}

LayerSupportPoints Slic3r::sla::generate_support_points(
    const SupportPointGeneratorData &data,
    const SupportPointGeneratorConfig &config,
    ThrowOnCancel throw_on_cancel,
    StatusFunction statusfn
){
    const Layers &layers = data.layers;
    double increment = 100.0 / static_cast<double>(layers.size());
    double status = 0; // current progress
    int status_int = 0; 

    // Hack to set curve for testing
    if (config.support_curve.empty())
        const_cast<SupportPointGeneratorConfig &>(config).support_curve = load_curve_from_file();
    
    // Maximal radius of supported area of one support point
    double max_support_radius = config.support_curve.back().x();
    // check distance to nearest support points from grid
    coord_t maximal_radius = static_cast<coord_t>(scale_(max_support_radius));

    // Storage for support points used by grid
    LayerSupportPoints result;

    // grid index == part in layer index
    std::vector<NearPoints> prev_grids; // same count as previous layer item size
    for (size_t layer_id = 0; layer_id < layers.size(); ++layer_id) {
        const Layer &layer = layers[layer_id];

        prepare_supports_for_layer(result, layer.print_z, config);

        // grid index == part in layer index
        std::vector<NearPoints> grids;
        grids.reserve(layer.parts.size());
        
        for (const LayerPart &part : layer.parts) {
            if (part.prev_parts.empty()) { // Island ?
                // only island add new grid
                grids.emplace_back(&result);
                // new island - needs support no doubt
                support_island(part, grids.back(), layer.print_z, config);
                continue;
            }

            // first layer should have empty prev_part
            assert(layer_id != 0);
            const LayerParts &prev_layer_parts = layers[layer_id - 1].parts;
            NearPoints near_points = create_near_points(prev_layer_parts, part, prev_grids);
            remove_supports_out_of_part(near_points, part, config);
            if (!part.peninsulas.empty())
                support_peninsulas(part.peninsulas, near_points, layer.print_z, config);
            support_part_overhangs(part, config, near_points, layer.print_z, maximal_radius);
            grids.push_back(std::move(near_points));            
        }
        prev_grids = std::move(grids);

        throw_on_cancel();

        int old_status_int = status_int;
        status += increment;
        status_int = static_cast<int>(std::round(status));
        if (old_status_int < status_int)
            statusfn(status_int);
    }
    return result;
}

// TODO: Should be in another file
#include "libslic3r/AABBMesh.hpp"
SupportPoints Slic3r::sla::move_on_mesh_surface(
    const LayerSupportPoints &points,
    const AABBMesh &mesh,
    double allowed_move,
    ThrowOnCancel throw_on_cancel
) {
    SupportPoints pts;
    pts.reserve(points.size());
    for (const LayerSupportPoint &p : points)
        pts.push_back(static_cast<SupportPoint>(p));

    // The function  makes sure that all the points are really exactly placed on the mesh.
    execution::for_each(ex_tbb, size_t(0), pts.size(), [&pts, &mesh, &throw_on_cancel, allowed_move](size_t idx)
    {
        if ((idx % 16) == 0)
            // Don't call the following function too often as it flushes CPU write caches due to synchronization primitves.
            throw_on_cancel();

        Vec3f& p = pts[idx].pos;
        Vec3d p_double = p.cast<double>();
        const Vec3d up_vec(0., 0., 1.);
        const Vec3d down_vec(0., 0., -1.);
        // Project the point upward and downward and choose the closer intersection with the mesh.
        AABBMesh::hit_result hit_up   = mesh.query_ray_hit(p_double, up_vec);
        AABBMesh::hit_result hit_down = mesh.query_ray_hit(p_double, down_vec);

        bool up   = hit_up.is_hit();
        bool down = hit_down.is_hit();
        // no hit means support points lay exactly on triangle surface
        if (!up && !down) return;
        
        AABBMesh::hit_result &hit = (!down || hit_up.distance() < hit_down.distance()) ? hit_up : hit_down;
        if (hit.distance() <= allowed_move) {
            p[2] += static_cast<float>(hit.distance() *
                                        hit.direction()[2]);
            return;
        }
        
        // big distance means that ray fly over triangle side (space between triangles)
        int    triangle_index;
        Vec3d  closest_point;
        double distance = mesh.squared_distance(p_double, triangle_index, closest_point);
        if (distance <= std::numeric_limits<float>::epsilon()) return; // correct coordinate
        p = closest_point.cast<float>();
    }, 64 /* gransize */);
    return pts;
}
