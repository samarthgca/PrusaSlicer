#include <catch2/catch_test_macros.hpp>
#include <test_utils.hpp>

#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/BoundingBox.hpp>
#include <libslic3r/SLA/SpatIndex.hpp>
#include <libslic3r/ClipperUtils.hpp>

#include <libslic3r/SLA/SupportIslands/SampleConfig.hpp>
#include <libslic3r/SLA/SupportIslands/VoronoiGraphUtils.hpp>
#include <libslic3r/SLA/SupportIslands/SampleIslandUtils.hpp>
#include <libslic3r/SLA/SupportIslands/PolygonUtils.hpp>

#include "sla_test_utils.hpp"

using namespace Slic3r;
using namespace Slic3r::sla;

#define STORE_SAMPLE_INTO_SVG_FILES

TEST_CASE("Overhanging point should be supported", "[SupGen]") {

    // Pyramid with 45 deg slope
    TriangleMesh mesh = make_pyramid(10.f, 10.f);
    mesh.rotate_y(float(PI));
    mesh.WriteOBJFile("Pyramid.obj");

    sla::SupportPoints pts = calc_support_pts(mesh);

    // The overhang, which is the upside-down pyramid's edge
    Vec3f overh{0., 0., -10.};

    REQUIRE(!pts.empty());

    float dist = (overh - pts.front().pos).norm();

    for (const auto &pt : pts)
        dist = std::min(dist, (overh - pt.pos).norm());

    // Should require exactly one support point at the overhang
    REQUIRE(pts.size() > 0);
    REQUIRE(dist < 1.f);
}

double min_point_distance(const sla::SupportPoints &pts)
{
    sla::PointIndex index;

    for (size_t i = 0; i < pts.size(); ++i)
        index.insert(pts[i].pos.cast<double>(), i);

    auto d = std::numeric_limits<double>::max();
    index.foreach([&d, &index](const sla::PointIndexEl &el) {
        auto res = index.nearest(el.first, 2);
        for (const sla::PointIndexEl &r : res)
            if (r.second != el.second)
                d = std::min(d, (el.first - r.first).norm());
    });

    return d;
}

TEST_CASE("Overhanging horizontal surface should be supported", "[SupGen]") {
    double width = 10., depth = 10., height = 1.;

    TriangleMesh mesh = make_cube(width, depth, height);
    mesh.translate(0., 0., 5.); // lift up
    mesh.WriteOBJFile("Cuboid.obj");

    sla::SupportPoints pts = calc_support_pts(mesh);

    double mm2 = width * depth;

    REQUIRE(!pts.empty());
}

template<class M> auto&& center_around_bb(M &&mesh)
{
    auto bb = mesh.bounding_box();
    mesh.translate(-bb.center().template cast<float>());

    return std::forward<M>(mesh);
}

TEST_CASE("Overhanging edge should be supported", "[SupGen]") {
    float width = 10.f, depth = 10.f, height = 5.f;

    TriangleMesh mesh = make_prism(width, depth, height);
    mesh.rotate_y(float(PI)); // rotate on its back
    mesh.translate(0., 0., height);
    mesh.WriteOBJFile("Prism.obj");

    sla::SupportPoints pts = calc_support_pts(mesh);

    Linef3 overh{ {0.f, -depth / 2.f, 0.f}, {0.f, depth / 2.f, 0.f}};

    // Get all the points closer that 1 mm to the overhanging edge:
    sla::SupportPoints overh_pts; overh_pts.reserve(pts.size());

    std::copy_if(pts.begin(), pts.end(), std::back_inserter(overh_pts),
                 [&overh](const sla::SupportPoint &pt){
                     return line_alg::distance_to(overh, Vec3d{pt.pos.cast<double>()}) < 1.;
                 });

    //double ddiff = min_point_distance(pts) - cfg.minimal_distance;
    //REQUIRE(ddiff > - 0.1 * cfg.minimal_distance);
}

TEST_CASE("Hollowed cube should be supported from the inside", "[SupGen][Hollowed]") {
    TriangleMesh mesh = make_cube(20., 20., 20.);

    hollow_mesh(mesh, HollowingConfig{});

    mesh.WriteOBJFile("cube_hollowed.obj");

    auto bb = mesh.bounding_box();
    auto h  = float(bb.max.z() - bb.min.z());
    Vec3f mv = bb.center().cast<float>() - Vec3f{0.f, 0.f, 0.5f * h};
    mesh.translate(-mv);

    sla::SupportPoints pts = calc_support_pts(mesh);
    //sla::remove_bottom_points(pts, mesh.bounding_box().min.z() + EPSILON);

    REQUIRE(!pts.empty());
}

TEST_CASE("Two parallel plates should be supported", "[SupGen][Hollowed]")
{
    double width = 20., depth = 20., height = 1.;

    TriangleMesh mesh = center_around_bb(make_cube(width + 5., depth + 5., height));
    TriangleMesh mesh_high = center_around_bb(make_cube(width, depth, height));
    mesh_high.translate(0., 0., 10.); // lift up
    mesh.merge(mesh_high);

    mesh.WriteOBJFile("parallel_plates.obj");

    sla::SupportPoints pts = calc_support_pts(mesh);
    //sla::remove_bottom_points(pts, mesh.bounding_box().min.z() + EPSILON);

    REQUIRE(!pts.empty());
}


Slic3r::Polygon create_cross_roads(double size, double width)
{
    auto r1 = PolygonUtils::create_rect(5.3 * size, width);
    r1.rotate(3.14/4);
    r1.translate(2 * size, width / 2);
    auto r2 = PolygonUtils::create_rect(6.1 * size, 3 / 4. * width);
    r2.rotate(-3.14 / 5);
    r2.translate(3 * size, width / 2);
    auto r3 = PolygonUtils::create_rect(7.9 * size, 4 / 5. * width);
    r3.translate(2*size, width/2);
    auto r4 = PolygonUtils::create_rect(5 / 6. * width, 5.7 * size);
    r4.translate(-size,3*size);
    Polygons rr = union_(Polygons({r1, r2, r3, r4}));
    return rr.front();
}

ExPolygon create_trinagle_with_hole(double size)
{
    auto hole = PolygonUtils::create_equilateral_triangle(size / 3);
    hole.reverse();
    hole.rotate(3.14 / 4);
    return ExPolygon(PolygonUtils::create_equilateral_triangle(size), hole);
}

ExPolygon create_square_with_hole(double size, double hole_size)
{
    assert(sqrt(hole_size *hole_size / 2) < size);
    auto hole = PolygonUtils::create_square(hole_size);
    hole.rotate(M_PI / 4.); // 45
    hole.reverse();
    return ExPolygon(PolygonUtils::create_square(size), hole);
}

ExPolygon create_square_with_4holes(double size, double hole_size) {
    auto hole = PolygonUtils::create_square(hole_size);
    hole.reverse();
    double size_4 = size / 4;
    auto h1 = hole;
    h1.translate(size_4, size_4);
    auto h2 = hole;
    h2.translate(-size_4, size_4);
    auto h3   = hole;
    h3.translate(size_4, -size_4);
    auto h4   = hole;
    h4.translate(-size_4, -size_4);
    ExPolygon result(PolygonUtils::create_square(size));
    result.holes = Polygons({h1, h2, h3, h4});
    return result;
}

// boudary of circle
ExPolygon create_disc(double radius, double width, size_t count_line_segments)
{
    double width_2 = width / 2;
    auto   hole    = PolygonUtils::create_circle(radius - width_2,
                                            count_line_segments);
    hole.reverse();
    return ExPolygon(PolygonUtils::create_circle(radius + width_2,
                                                 count_line_segments),
                     hole);
}

Slic3r::Polygon create_V_shape(double height, double line_width, double angle = M_PI/4) {
    double angle_2 = angle / 2;
    auto   left_side  = PolygonUtils::create_rect(line_width, height);
    auto   right_side = left_side;
    right_side.rotate(-angle_2);
    double small_move = cos(angle_2) * line_width / 2;
    double side_move  = sin(angle_2) * height / 2 + small_move;
    right_side.translate(side_move,0);
    left_side.rotate(angle_2);
    left_side.translate(-side_move, 0);
    auto bottom = PolygonUtils::create_rect(4 * small_move, line_width);
    bottom.translate(0., -cos(angle_2) * height / 2 + line_width/2);
    Polygons polygons = union_(Polygons({left_side, right_side, bottom}));
    return polygons.front();
}

ExPolygon create_tiny_wide_test_1(double wide, double tiny)
{
    double hole_size = wide;
    double width     = 2 * wide + hole_size;
    double height    = wide + hole_size + tiny;
    auto   outline   = PolygonUtils::create_rect(width, height);
    auto   hole      = PolygonUtils::create_rect(hole_size, hole_size);
    hole.reverse();
    int hole_move_y = height/2 - (hole_size/2 + tiny);
    hole.translate(0, hole_move_y);
    
    ExPolygon result(outline);
    result.holes = {hole};
    return result;
}

ExPolygon create_tiny_wide_test_2(double wide, double tiny)
{
    double hole_size = wide;
    double width     = (3 + 1) * wide + 3 * hole_size;
    double height    = 2*wide + 2*tiny + 3*hole_size;
    auto outline = PolygonUtils::create_rect( width, height);
    auto   hole      = PolygonUtils::create_rect(hole_size, hole_size);
    hole.reverse();
    auto  hole2 = hole;// copy
    auto  hole3       = hole; // copy
    auto  hole4       = hole; // copy

    int   hole_move_x = wide + hole_size;
    int   hole_move_y = wide + hole_size;
    hole.translate(hole_move_x, hole_move_y);
    hole2.translate(-hole_move_x, hole_move_y);
    hole3.translate(hole_move_x, -hole_move_y);
    hole4.translate(-hole_move_x, -hole_move_y);

    auto hole5 = PolygonUtils::create_circle(hole_size / 2, 16);
    hole5.reverse();
    auto hole6 = hole5; // copy
    hole5.translate(0, hole_move_y);
    hole6.translate(0, -hole_move_y);

    auto hole7 = PolygonUtils::create_equilateral_triangle(hole_size);
    hole7.reverse();
    auto hole8 = PolygonUtils::create_circle(hole_size/2, 7, Point(hole_move_x,0));
    hole8.reverse();
    auto hole9 = PolygonUtils::create_circle(hole_size/2, 5, Point(-hole_move_x,0));
    hole9.reverse();

    ExPolygon result(outline);
    result.holes = {hole, hole2, hole3, hole4, hole5, hole6, hole7, hole8, hole9};
    return result;
}

ExPolygon create_tiny_between_holes(double wide, double tiny)
{
    double hole_size = wide;
    double width     = 2 * wide + 2*hole_size + tiny;
    double height    = 2 * wide + hole_size;
    auto   outline   = PolygonUtils::create_rect(width, height);
    auto   holeL      = PolygonUtils::create_rect(hole_size, hole_size);
    holeL.reverse();
    auto holeR       = holeL;
    int hole_move_x = (hole_size + tiny)/2;
    holeL.translate(-hole_move_x, 0);
    holeR.translate(hole_move_x, 0);

    ExPolygon result(outline);
    result.holes = {holeL, holeR};
    return result;
}

// stress test for longest path
// needs reshape
ExPolygon create_mountains(double size) {
    return ExPolygon({{0., 0.},
                      {size, 0.},
                      {5 * size / 6, size},
                      {4 * size / 6, size / 6},
                      {3 * size / 7, 2 * size},
                      {2 * size / 7, size / 6},
                      {size / 7, size}});
}

ExPolygons createTestIslands(double size)
{
    bool      useFrogLeg = false;    
    // need post reorganization of longest path
    ExPolygons result = {
        // one support point
        ExPolygon(PolygonUtils::create_equilateral_triangle(size)), 
        ExPolygon(PolygonUtils::create_square(size)),
        ExPolygon(PolygonUtils::create_rect(size / 2, size)),
        ExPolygon(PolygonUtils::create_isosceles_triangle(size / 2, 3 * size / 2)), // small sharp triangle
        ExPolygon(PolygonUtils::create_circle(size / 2, 10)),
        create_square_with_4holes(size, size / 4),
        create_disc(size/4, size / 4, 10),
        ExPolygon(create_V_shape(2*size/3, size / 4)),

        // two support points
        ExPolygon(PolygonUtils::create_isosceles_triangle(size / 2, 3 * size)), // small sharp triangle
        ExPolygon(PolygonUtils::create_rect(size / 2, 3 * size)),
        ExPolygon(create_V_shape(1.5*size, size/3)),

        // tiny line support points
        ExPolygon(PolygonUtils::create_rect(size / 2, 10 * size)), // long line
        ExPolygon(create_V_shape(size*4, size / 3)),
        ExPolygon(create_cross_roads(size, size / 3)),
        create_disc(3*size, size / 4, 30),
        create_disc(2*size, size, 12), // 3 points
        create_square_with_4holes(5 * size, 5 * size / 2 - size / 3),

        // Tiny and wide part together with holes
        ExPolygon(PolygonUtils::create_isosceles_triangle(5. * size, 40. * size)),
        create_tiny_wide_test_1(3 * size, 2 / 3. * size),
        create_tiny_wide_test_2(3 * size, 2 / 3. * size),
        create_tiny_between_holes(3 * size, 2 / 3. * size),

        // still problem
        // three support points
        ExPolygon(PolygonUtils::create_equilateral_triangle(3 * size)), 
        ExPolygon(PolygonUtils::create_circle(size, 20)),

        create_mountains(size),
        create_trinagle_with_hole(size),
        create_square_with_hole(size, size / 2),
        create_square_with_hole(size, size / 3)
    };
    
    if (useFrogLeg) {
        TriangleMesh            mesh = load_model("frog_legs.obj");
        TriangleMeshSlicer      slicer{&mesh};
        std::vector<float>      grid({0.1f});
        std::vector<ExPolygons> slices;
        slicer.slice(grid, SlicingMode::Regular, 0.05f, &slices, [] {});
        ExPolygon frog_leg = slices.front()[1];
        result.push_back(frog_leg);
    }
    return result;
}

Points createNet(const BoundingBox& bounding_box, double distance)
{ 
    Point  size       = bounding_box.size();
    double distance_2 = distance / 2;
    int    cols1 = static_cast<int>(floor(size.x() / distance))+1;
    int    cols2 = static_cast<int>(floor((size.x() - distance_2) / distance))+1;
    // equilateral triangle height with side distance
    double h      = sqrt(distance * distance - distance_2 * distance_2);
    int    rows   = static_cast<int>(floor(size.y() / h)) +1;
    int    rows_2 = rows / 2;
    size_t count_points = rows_2 * (cols1 + static_cast<size_t>(cols2));
    if (rows % 2 == 1) count_points += cols2;
    Points result;
    result.reserve(count_points);
    bool   isOdd = true;
    Point offset = bounding_box.min;
    double x_max = offset.x() + static_cast<double>(size.x());
    double y_max  = offset.y() + static_cast<double>(size.y());
    for (double y = offset.y(); y <= y_max; y += h) {
        double x_offset = offset.x();
        if (isOdd) x_offset += distance_2;
        isOdd = !isOdd;
        for (double x = x_offset; x <= x_max; x += distance) {
            result.emplace_back(x, y);
        }
    }
    assert(result.size() == count_points);
    return result; 
}

// create uniform triangle net and return points laying inside island
Points rasterize(const ExPolygon &island, double distance) {
    BoundingBox bb;
    for (const Point &pt : island.contour.points) bb.merge(pt);
    Points      fullNet = createNet(bb, distance);
    Points result;
    result.reserve(fullNet.size());
    std::copy_if(fullNet.begin(), fullNet.end(), std::back_inserter(result),
                 [&island](const Point &p) { return island.contains(p); });
    return result;
}

SupportIslandPoints test_island_sampling(const ExPolygon &   island,
                                        const SampleConfig &config)
{
    auto points = SupportPointGenerator::uniform_cover_island(island, config);

    Points chck_points = rasterize(island, config.head_radius); // TODO: Use resolution of printer
    bool is_ok = true;
    double              max_distance = config.max_distance;
    std::vector<double> point_distances(chck_points.size(),
                                        {max_distance + 1});
    for (size_t index = 0; index < chck_points.size(); ++index) { 
        const Point &chck_point  = chck_points[index];
        double &min_distance = point_distances[index];
        bool         exist_close_support_point = false;
        for (const auto &island_point : points) {
            const Point& p = island_point->point;
            Point abs_diff(fabs(p.x() - chck_point.x()),
                           fabs(p.y() - chck_point.y()));
            if (abs_diff.x() < min_distance && abs_diff.y() < min_distance) {
                double distance = sqrt((double) abs_diff.x() * abs_diff.x() +
                                       (double) abs_diff.y() * abs_diff.y());
                if (min_distance > distance) {
                    min_distance = distance;
                    exist_close_support_point = true;
                };
            }
        }
        if (!exist_close_support_point) is_ok = false;
    }

    if (!is_ok) { // visualize
        static int  counter              = 0;
        BoundingBox bb;
        for (const Point &pt : island.contour.points) bb.merge(pt);
        SVG svg("Error" + std::to_string(++counter) + ".svg", bb);
        svg.draw(island, "blue", 0.5f);
        for (auto& p : points)
            svg.draw(p->point, "lightgreen", config.head_radius);
        for (size_t index = 0; index < chck_points.size(); ++index) {
            const Point &chck_point = chck_points[index];
            double       distance   = point_distances[index];
            bool         isOk       = distance < max_distance;
            std::string  color      = (isOk) ? "gray" : "red";
            svg.draw(chck_point, color, config.head_radius / 4);
        }
    }
    CHECK(!points.empty());
    //CHECK(is_ok);

    // all points must be inside of island
    for (const auto &point : points) { CHECK(island.contains(point->point)); }
    return points;
}

SampleConfig create_sample_config(double size) {
    //SupportPointGenerator::Config spg_config;
    //return SampleConfigFactory::create(spg_config);

    SampleConfig cfg;
    cfg.max_distance = 3 * size + 0.1;
    cfg.half_distance = cfg.max_distance/2;
    cfg.head_radius = size / 4;
    cfg.minimal_distance_from_outline = cfg.head_radius;
    cfg.maximal_distance_from_outline = cfg.max_distance/4;
    cfg.min_side_branch_length = 2 * cfg.minimal_distance_from_outline;
    cfg.minimal_support_distance = cfg.minimal_distance_from_outline + cfg.half_distance;
    cfg.max_length_for_one_support_point = 2*size;
    cfg.max_length_for_two_support_points = 4*size;
    cfg.max_width_for_center_support_line = size;
    cfg.min_width_for_outline_support = cfg.max_width_for_center_support_line;
    cfg.outline_sample_distance       = cfg.max_distance;

    cfg.minimal_move       = static_cast<coord_t>(size/30);
    cfg.count_iteration = 100; 
    cfg.max_align_distance = 0;
    return cfg;
}

#include <libslic3r/Geometry.hpp>
#include <libslic3r/VoronoiOffset.hpp>
TEST_CASE("Sampling speed test on FrogLegs", "[hide], [VoronoiSkeleton]")
{
    TriangleMesh            mesh = load_model("frog_legs.obj");
    TriangleMeshSlicer      slicer{&mesh};
    std::vector<float>      grid({0.1f});
    std::vector<ExPolygons> slices;
    slicer.slice(grid, SlicingMode::Regular, 0.05f, &slices, [] {});
    ExPolygon frog_leg = slices.front()[1];
    SampleConfig cfg = create_sample_config(3e7);

    using VD = Slic3r::Geometry::VoronoiDiagram;
    VD    vd;
    Lines lines = to_lines(frog_leg);
    construct_voronoi(lines.begin(), lines.end(), &vd);
    Slic3r::Voronoi::annotate_inside_outside(vd, lines);
    
    for (int i = 0; i < 100; ++i) {
        VoronoiGraph::ExPath longest_path;
        VoronoiGraph skeleton = VoronoiGraphUtils::create_skeleton(vd, lines);
        auto samples = SampleIslandUtils::sample_voronoi_graph(skeleton, lines, cfg, longest_path);
    }
}

TEST_CASE("Speed align", "[hide], [VoronoiSkeleton]")
{
    SampleConfig cfg      = create_sample_config(3e7);
    cfg.max_align_distance = 1000;
    cfg.count_iteration    = 1000;
    cfg.max_align_distance = 3e7;

    double       size = 3e7;
    auto island = create_square_with_4holes(5 * size, 5 * size / 2 - size / 3);
    using VD = Slic3r::Geometry::VoronoiDiagram;
    VD    vd;
    Lines lines = to_lines(island);
    construct_voronoi(lines.begin(), lines.end(), &vd);
    Slic3r::Voronoi::annotate_inside_outside(vd, lines);
    VoronoiGraph::ExPath longest_path;
    VoronoiGraph skeleton = VoronoiGraphUtils::create_skeleton(vd, lines);

    for (int i = 0; i < 100; ++i) {
        auto samples = SampleIslandUtils::sample_voronoi_graph(skeleton, lines, cfg, longest_path);
        SampleIslandUtils::align_samples(samples, island, cfg);
    }
}

#include <libslic3r/SLA/SupportIslands/LineUtils.hpp>
/// <summary>
/// Check speed of sampling,
/// for be sure that code is not optimized out store result to svg or print count.
/// </summary>
TEST_CASE("speed sampling", "[hide], [SupGen]") { 
    double       size    = 3e7;
    float      samples_per_mm2 = 0.01f;
    ExPolygons   islands = createTestIslands(size);
    std::random_device rd;
    std::mt19937 m_rng;
    m_rng.seed(rd());

    size_t count = 1;
    
    std::vector<std::vector<Vec2f>> result1;
    result1.reserve(islands.size()*count);
    for (size_t i = 0; i<count; ++i)
        for (const auto& island: islands)
            result1.emplace_back(sample_expolygon(island, samples_per_mm2, m_rng));
    
    std::vector<std::vector<Vec2f>> result2;
    result2.reserve(islands.size()*count);
    for (size_t i = 0; i < count; ++i)
        for (const auto &island : islands)
            result2.emplace_back(SampleIslandUtils::sample_expolygon(island, samples_per_mm2)); //*/

    /*size_t all = 0;
    for (auto& result : result2) { 
        //std::cout << "case " << &result - &result1[0] << " points " << result.size() << std::endl;
        all += result.size();
    }
    std::cout << "All points " << all << std::endl;*/
    
    
#ifdef STORE_SAMPLE_INTO_SVG_FILES
    for (size_t i = 0; i < result1.size(); ++i) {
        size_t     island_index = i % islands.size();
        ExPolygon &island       = islands[island_index];

        Lines       lines = to_lines(island.contour);
        std::string name  = "sample_" + std::to_string(i) + ".svg";
        SVG         svg(name, LineUtils::create_bounding_box(lines));
        svg.draw(island, "lightgray");
        svg.draw_text({0, 0}, ("random samples " + std::to_string(result1[i].size())).c_str(), "blue");
        for (Vec2f &p : result1[i])
            svg.draw((p * 1e6).cast<coord_t>(), "blue", 1e6);
        svg.draw_text({0., 5e6}, ("uniform samples " + std::to_string(result2[i].size())).c_str(), "green");
        for (Vec2f &p : result2[i]) 
            svg.draw((p * 1e6).cast<coord_t>(), "green", 1e6);
    }
#endif // STORE_SAMPLE_INTO_SVG_FILES
}

/// <summary>
/// Check for correct sampling of island
/// 
/// </summary>
TEST_CASE("Small islands should be supported in center", "[SupGen], [VoronoiSkeleton]")
{
    double       size    = 3e7;
    SampleConfig cfg  = create_sample_config(size);
    ExPolygons islands = createTestIslands(size);
    for (ExPolygon &island : islands) {
        // information for debug which island cause problem
        [[maybe_unused]] size_t debug_index = &island - &islands.front(); 
        auto   points = test_island_sampling(island, cfg);
        double angle  = 3.14 / 3; // cca 60 degree

        island.rotate(angle);
        auto pointsR = test_island_sampling(island, cfg);

        // points count should be the same
        //CHECK(points.size() == pointsR.size())
    }
}

std::vector<Vec2f> sample_old(const ExPolygon &island)
{
    // Create the support point generator
    static TriangleMesh                       mesh;
    static sla::IndexedMesh                   emesh{mesh};
    static sla::SupportPointGenerator::Config autogencfg;
    //autogencfg.minimal_distance = 8.f;
    static sla::SupportPointGenerator generator{emesh, autogencfg, [] {}, [](int) {}};

    // tear preasure
    float tp = autogencfg.tear_pressure();
    size_t layer_id = 13;
    coordf_t print_z = 11.f;
    SupportPointGenerator::MyLayer layer(layer_id, print_z);
    ExPolygon                      poly = island;
    BoundingBox                    bbox(island);
    Vec2f                          centroid;
    float                          area = island.area();
    float                                 h    = 17.f;
    sla::SupportPointGenerator::Structure s(layer, poly, bbox, centroid,area,h);
    auto flag = sla::SupportPointGenerator::IslandCoverageFlags(
        sla::SupportPointGenerator::icfIsNew | sla::SupportPointGenerator::icfWithBoundary);
    SupportPointGenerator::PointGrid3D grid3d;
    generator.uniformly_cover({island}, s, s.area * tp, grid3d, flag);

    std::vector<Vec2f> result;
    result.reserve(grid3d.grid.size());
    for (auto g : grid3d.grid) { 
        const Vec3f &p = g.second.position;
        Vec2f        p2f(p.x(), p.y());
        result.emplace_back(scale_(p2f));
    }
    return result;
}

#include <libslic3r/SLA/SupportIslands/SampleConfigFactory.hpp>
std::vector<Vec2f> sample_filip(const ExPolygon &island)
{
    static SampleConfig cfg = create_sample_config(1e6);
    SupportIslandPoints points = SupportPointGenerator::uniform_cover_island(island, cfg);

    std::vector<Vec2f> result;
    result.reserve(points.size());
    for (auto &p : points) { 
        result.push_back(p->point.cast<float>());
    }
    return result;
}

void store_sample(const std::vector<Vec2f> &samples, const ExPolygon& island)
{ 
    static int counter = 0;
    BoundingBox bb(island);
    SVG svg(("sample_"+std::to_string(counter++)+".svg").c_str(), bb); 

    double mm = scale_(1);
    svg.draw(island, "lightgray");
    for (const auto &s : samples) { 
        svg.draw(s.cast<coord_t>(), "blue", 0.2*mm);
    }

    // draw resolution
    Point p(bb.min.x() + 1e6, bb.max.y() - 2e6);
    svg.draw_text(p, (std::to_string(samples.size()) + " samples").c_str(), "black");
    svg.draw_text(p - Point(0., 1.8e6), "Scale 1 cm ", "black");
    Point  start = p - Point(0., 2.3e6);
    svg.draw(Line(start + Point(0., 5e5), start + Point(10*mm, 5e5)), "black", 2e5);
    svg.draw(Line(start + Point(0., -5e5), start + Point(10*mm, -5e5)), "black", 2e5);
    svg.draw(Line(start + Point(10*mm, 5e5), start + Point(10*mm, -5e5)), "black", 2e5);
    for (int i=0; i<10;i+=2)
        svg.draw(Line(start + Point(i*mm, 0.), start + Point((i+1)*mm, 0.)), "black", 1e6);
}

TEST_CASE("Compare sampling test", "[hide]")
{
    enum class Sampling {
        old,
        filip 
    } sample_type = Sampling::old;
    
    std::function<std::vector<Vec2f>(const ExPolygon &)> sample =
        (sample_type == Sampling::old)   ? sample_old :
        (sample_type == Sampling::filip) ? sample_filip :
                                           nullptr;
    ExPolygons   islands  = createTestIslands(1e6);
    ExPolygons   islands_big = createTestIslands(3e6);
    islands.insert(islands.end(), islands_big.begin(), islands_big.end());

    for (ExPolygon &island : islands) {
        // information for debug which island cause problem
        [[maybe_unused]] size_t debug_index = &island - &islands.front();
        auto samples = sample(island);
#ifdef STORE_SAMPLE_INTO_SVG_FILES
        store_sample(samples, island);
#endif // STORE_SAMPLE_INTO_SVG_FILES
        
        double angle = 3.14 / 3; // cca 60 degree
        island.rotate(angle);
        samples = sample(island);
#ifdef STORE_SAMPLE_INTO_SVG_FILES
        store_sample(samples, island);
#endif // STORE_SAMPLE_INTO_SVG_FILES
    }
}

TEST_CASE("Disable visualization", "[hide]") 
{
    CHECK(true);
#ifdef STORE_SAMPLE_INTO_SVG_FILES
    CHECK(false);
#endif // STORE_SAMPLE_INTO_SVG_FILES
    CHECK(SampleIslandUtils::is_visualization_disabled());
}

