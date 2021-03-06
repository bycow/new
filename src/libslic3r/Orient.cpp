#include "Orient.hpp"
#include "Geometry.hpp"
#include <numeric>
#include <ClipperUtils.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <tbb/parallel_for.h>
#include <tbb/atomic.h>

#if defined(_MSC_VER) && defined(__clang__)
#define BOOST_NO_CXX17_HDR_STRING_VIEW
#endif

#include <boost/multiprecision/integer.hpp>
#include <boost/rational.hpp>

#undef MAX3
#define MAX3(a,b,c) std::max(std::max(a,b),c)

#undef MEDIAN
#define MEDIAN3(a,b,c) std::max(std::min(a,b), std::min(std::max(a,b),c))
#ifndef SQ
#define SQ(x) ((x)*(x))
#endif

namespace Slic3r {

namespace orientation {

    struct CostItems {
        float overhang;
        float bottom;
        float bottom_hull;
        float contour;
        float area_laf;  // area_of_low_angle_faces
        float area_projected; // area of projected 2D profile
        float volume;
        float area_total;  // total area of all faces
        float radius;    // radius of bounding box
        float height_to_bottom_hull_ratio;  // affects stability, the lower the better
        float unprintability;
        CostItems(CostItems const & other) = default;
        CostItems() { memset(this, 0, sizeof(*this)); }
        static std::string field_names() {
            return "                                      overhang, bottom, bothull, contour, A_laf, A_prj, unprintability";
        }
        std::string field_values() {
            std::stringstream ss;
            ss << std::fixed << std::setprecision(1);
            ss << overhang << ",\t" << bottom << ",\t" << bottom_hull << ",\t" << contour << ",\t" << area_laf << ",\t" << area_projected << ",\t" << unprintability;
            return ss.str();
        }
    };



// A class encapsulating the libnest2d Nester class and extending it with other
// management and spatial index structures for acceleration.
class AutoOrienter {
public:
    OrientMesh *orient_mesh = NULL;
    TriangleMesh* mesh;
    TriangleMesh mesh_convex_hull;
    Eigen::MatrixXf normals, normals_hull;
    Eigen::VectorXf areas, areas_hull;
    Eigen::VectorXf is_apperance; // whether a facet is outer apperance
    Eigen::MatrixXf z_projected;
    Eigen::VectorXf z_max, z_max_hull;  // max of projected z
    Eigen::VectorXf z_median;  // median of projected z
    Eigen::VectorXf z_mean;  // mean of projected z
    OrientParams params;


    std::vector< Vec3f> orientations;  // Vec3f == stl_normal
    std::function<void(unsigned)> progressind = { };  // default empty indicator function

public:
    AutoOrienter(OrientMesh* orient_mesh_,
                 const OrientParams           &params_,
                 std::function<void(unsigned)> progressind_,
                 std::function<bool(void)>     stopcond_)
    {
        orient_mesh = orient_mesh_;
        mesh = &orient_mesh->mesh;
        params = params_;
        progressind = progressind_;
        params.ASCENT = cos(PI - orient_mesh->overhang_angle * PI / 180); // use per-object overhang angle
        
        // BOOST_LOG_TRIVIAL(info) << orient_mesh->name << ", angle=" << orient_mesh->overhang_angle << ", params.ASCENT=" << params.ASCENT;
        // std::cout << orient_mesh->name << ", angle=" << orient_mesh->overhang_angle << ", params.ASCENT=" << params.ASCENT;

        preprocess();
    }

    AutoOrienter(TriangleMesh* mesh_)
    {
        mesh = mesh_;
        preprocess();
    }

    struct VecHash {
        size_t operator()(const Vec3f& n1) const {
            return std::hash<coord_t>()(int(n1(0)*100+100)) + std::hash<coord_t>()(int(n1(1)*100+100)) * 101 + std::hash<coord_t>()(int(n1(2)*100+100)) * 10221;
        }
    };

    Vec3f quantize_vec3f(const Vec3f n1) {
        return Vec3f(floor(n1(0) * 1000) / 1000, floor(n1(1) * 1000) / 1000, floor(n1(2) * 1000) / 1000);
    }

    Vec3d process()
    {
        orientations = { { 0,0,-1 } }; // original orientation

        area_cumulation(normals, areas);

        area_cumulation(normals_hull, areas_hull, 10);

        add_supplements();

        if(progressind)
            progressind(20);

        remove_duplicates();

        if (progressind)
            progressind(30);

        std::unordered_map<Vec3f, CostItems, VecHash> results;
        BOOST_LOG_TRIVIAL(info) << CostItems::field_names();
        std::cout << CostItems::field_names() << std::endl;
        for (int i = 0; i < orientations.size();i++) {
            auto orientation = -orientations[i];

            project_vertices(orientation);

            auto cost_items = get_features(orientation, params.min_volume);

            float unprintability = target_function(cost_items, params.min_volume);

            results[orientation] = cost_items;

            BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(4) << "orientation:" << orientation.transpose() << ", cost:" << std::fixed << std::setprecision(4) << cost_items.field_values();
            std::cout << std::fixed << std::setprecision(4) << "orientation:" << orientation.transpose() << ", cost:" << std::fixed << std::setprecision(4) << cost_items.field_values() << std::endl;
        }
        if (progressind)
            progressind(60);

        typedef std::pair<Vec3f, CostItems> PAIR;
        std::vector<PAIR> results_vector(results.begin(), results.end());
        sort(results_vector.begin(), results_vector.end(), [](const PAIR& p1, const PAIR& p2) {return p1.second.unprintability < p2.second.unprintability; });

        if (progressind)
            progressind(80);

        auto best_orientation = results_vector[0].first;

        BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(6) << "best:" << best_orientation.transpose() << ", costs:" << results_vector[0].second.field_values();
        std::cout << std::fixed << std::setprecision(6) << "best:" << best_orientation.transpose() << ", costs:" << results_vector[0].second.field_values() << std::endl;

        return best_orientation.cast<double>();
    }

    void preprocess()
    {
        int count_apperance = 0;
        {
            int face_count = mesh->facets_count();
            auto its = mesh->its;
            auto face_normals = its_face_normals(its);
            areas = Eigen::VectorXf::Zero(face_count);
            is_apperance = Eigen::VectorXf::Zero(face_count);
            normals = Eigen::MatrixXf::Zero(face_count, 3);
            for (size_t i = 0; i < face_count; i++)
            {
                float area = its.facet_area(i);
                if (params.NEGL_FACE_SIZE > 0 && area < params.NEGL_FACE_SIZE)
                    continue;
                
                normals.row(i) = quantize_vec3f(face_normals[i]);
                areas(i) = area;
                is_apperance(i) = (its.get_property(i).type == EnumFaceTypes::eExteriorAppearance);
                count_apperance += (is_apperance(i)==1);
            }
        }

        if (orient_mesh)
            BOOST_LOG_TRIVIAL(debug) <<orient_mesh->name<< ", count_apperance=" << count_apperance;

        // get convex hull statistics
        {
            mesh_convex_hull = mesh->convex_hull_3d();
            //mesh_convex_hull.write_binary("convex_hull_debug.stl");

            int face_count = mesh_convex_hull.facets_count();
            auto its = mesh_convex_hull.its;
            auto face_normals = its_face_normals(its);
            areas_hull = Eigen::VectorXf::Zero(face_count);
            normals_hull = Eigen::MatrixXf::Zero(face_count, 3);
            for (size_t i = 0; i < face_count; i++)
            {
                float area = its.facet_area(i);
                if (params.NEGL_FACE_SIZE > 0 && area < params.NEGL_FACE_SIZE)
                    continue;
                normals_hull.row(i) = quantize_vec3f(face_normals[i]);
                areas_hull(i) = area;
            }
        }
    }

    void area_cumulation(const Eigen::MatrixXf& normals_, const Eigen::VectorXf& areas_, int num_directions = 10)
    {
        std::unordered_map<stl_normal, float, VecHash> alignments;
        // init to 0
        for (size_t i = 0; i < areas_.size(); i++)
            alignments.insert(std::pair(normals_.row(i), 0));
        // cumulate areas
        for (size_t i = 0; i < areas_.size(); i++)
        {
            alignments[normals_.row(i)] += areas_(i);
        }

        typedef std::pair<stl_normal, float> PAIR;
        std::vector<PAIR> align_counts(alignments.begin(), alignments.end());
        sort(align_counts.begin(), align_counts.end(), [](const PAIR& p1, const PAIR& p2) {return p1.second > p2.second; });

        num_directions = std::min((size_t)num_directions, align_counts.size());
        for (size_t i = 0; i < num_directions; i++)
        {
            orientations.push_back(align_counts[i].first);
            BOOST_LOG_TRIVIAL(debug) << align_counts[i].first.transpose() << ", area: " << align_counts[i].second;
        }
    }

    void add_supplements()
    {
        std::vector<Vec3f> vecs = { {0, 0, -1} ,{0.70710678, 0, -0.70710678},{0, 0.70710678, -0.70710678},
            {-0.70710678, 0, -0.70710678},{0, -0.70710678, -0.70710678},
            {1, 0, 0},{0.70710678, 0.70710678, 0},{0, 1, 0},{-0.70710678, 0.70710678, 0},
            {-1, 0, 0},{-0.70710678, -0.70710678, 0},{0, -1, 0},{0.70710678, -0.70710678, 0},
            {0.70710678, 0, 0.70710678},{0, 0.70710678, 0.70710678},
            {-0.70710678, 0, 0.70710678},{0, -0.70710678, 0.70710678},{0, 0, 1} };
        orientations.insert(orientations.end(), vecs.begin(), vecs.end());
    }

    /// <summary>
    /// remove duplicate orientations
    /// </summary>
    /// <param name="tol">tolerance. default 0.01 =sin(0.57\degree)</param>
    void remove_duplicates(float tol=0.01)
    {
        for (auto it = orientations.begin()+1; it < orientations.end(); )
        {
            bool duplicate = false;
            for (auto it_ok = orientations.begin(); it_ok < it; it_ok++)
            {
                if (it_ok->isApprox(*it, tol)) {
                    duplicate = true;
                    break;
                }
            }
            const Vec3f all_zero = { 0,0,0 };
            if (duplicate || it->isApprox(all_zero,tol))
                it = orientations.erase(it);
            else
                it++;
        }
    }

    void project_vertices(Vec3f orientation)
    {
        int face_count = mesh->facets_count();
        auto its = mesh->its;
        z_projected.resize(face_count, 3);
        z_max.resize(face_count, 1);
        z_median.resize(face_count, 1);
        z_mean.resize(face_count, 1);
        for (size_t i = 0; i < face_count; i++)
        {
            float z0 = its.get_vertex(i,0).dot(orientation);
            float z1 = its.get_vertex(i,1).dot(orientation);
            float z2 = its.get_vertex(i,2).dot(orientation);
            z_projected(i, 0) = z0;
            z_projected(i, 1) = z1;
            z_projected(i, 2) = z2;
            z_max(i) = MAX3(z0,z1,z2);
            z_median(i) = MEDIAN3(z0,z1,z2);
            z_mean(i) = (z0 + z1 + z2) / 3;
        }

        z_max_hull.resize(mesh_convex_hull.facets_count(), 1);
        its = mesh_convex_hull.its;
        for (size_t i = 0; i < z_max_hull.rows(); i++)
        {
            float z0 = its.get_vertex(i,0).dot(orientation);
            float z1 = its.get_vertex(i,1).dot(orientation);
            float z2 = its.get_vertex(i,2).dot(orientation);
            z_max_hull(i) = MAX3(z0, z1, z2);
        }
    }

    static Eigen::VectorXi argsort(const Eigen::VectorXf& vec, std::string order="ascend")
    {
        Eigen::VectorXi ind = Eigen::VectorXi::LinSpaced(vec.size(), 0, vec.size() - 1);//[0 1 2 3 ... N-1]
        std::function<bool(int, int)> rule;
        if (order == "ascend") {
            rule = [vec](int i, int j)->bool {
                return vec(i) < vec(j);
                };
            }
        else {
            rule = [vec](int i, int j)->bool {
                return vec(i) > vec(j);
                };
            }
        std::sort(ind.data(), ind.data() + ind.size(), rule);
        return ind;

        //sorted_vec.resize(vec.size());
        //for (int i = 0; i < vec.size(); i++) {
        //    sorted_vec(i) = vec(ind(i));
        //}
    }

    // previously calc_overhang
    CostItems get_features(Vec3f orientation, bool min_volume = true)
    {
        CostItems costs;
        costs.area_total = mesh->bounding_box().area();
        costs.radius = mesh->bounding_box().radius();
        // volume
        costs.volume = mesh->stats().volume > 0 ? mesh->stats().volume : its_volume(mesh->its);

        float total_min_z = z_projected.minCoeff();
        // filter bottom area
        auto bottom_condition = z_max.array() < total_min_z + this->params.FIRST_LAY_H;
        costs.bottom = bottom_condition.select(areas, 0).sum();

        // filter overhang
        Eigen::VectorXf normal_projection(normals.rows(), 1);// = this->normals.dot(orientation);
        for (size_t i = 0; i < normals.rows(); i++)
        {
            normal_projection(i) = normals.row(i).dot(orientation);
        }
        auto areas_appearance = areas.cwiseProduct((is_apperance * params.APPERANCE_FACE_SUPP + Eigen::VectorXf::Ones(is_apperance.rows(), is_apperance.cols())));
        auto overhang_areas = ((normal_projection.array() < params.ASCENT) * (!bottom_condition)).select(areas_appearance, 0);
        Eigen::MatrixXf inner = normal_projection.array() - params.ASCENT;
        inner = inner.cwiseMin(0).cwiseAbs();
        if (min_volume)
        {
            Eigen::MatrixXf heights = z_mean.array() - total_min_z;
            costs.overhang = (heights.array()* overhang_areas.array()*inner.array()).sum();
        }
        else {
            costs.overhang = overhang_areas.array().cwiseAbs().sum();
        }

        {
            // contour perimeter
#if 1
            // the simple way for contour is even better for faces of small bridges
            costs.contour = 4 * sqrt(costs.bottom);
#else
            float contour = 0;
            int face_count = mesh->facets_count();
            auto its = mesh->its;
            int contour_amout = 0;
            for (size_t i = 0; i < face_count; i++)
            {
                if (bottom_condition(i)) {
                    Eigen::VectorXi index = argsort(z_projected.row(i));
                    stl_vertex line = its.get_vertex(i, index(0)) - its.get_vertex(i, index(1));
                    contour += line.norm();
                    contour_amout++;
                }
            }
            costs.contour += contour + params.CONTOUR_AMOUNT * contour_amout;
#endif
        }

        // bottom of convex hull
        costs.bottom_hull = (z_max_hull.array()< total_min_z + this->params.FIRST_LAY_H).select(areas_hull, 0).sum();

        // low angle faces
        auto normal_projection_abs = normal_projection.cwiseAbs();
        Eigen::MatrixXf laf_areas = ((normal_projection_abs.array() < params.LAF_MAX) * (normal_projection_abs.array() > params.LAF_MIN) * (z_max.array() > total_min_z + params.FIRST_LAY_H)).select(areas, 0);
        costs.area_laf = laf_areas.sum();

        // height to bottom_hull_area ratio
        //float total_max_z = z_projected.maxCoeff();
        //costs.height_to_bottom_hull_ratio = SQ(total_max_z) / (costs.bottom_hull + 1e-7);

        return costs;
    }

    float target_function(CostItems& costs, bool min_volume)
    {
        float cost=0;
        float bottom = costs.bottom;//std::min(costs.bottom, params.BOTTOM_MAX);
        float bottom_hull = costs.bottom_hull;// std::min(costs.bottom_hull, params.BOTTOM_HULL_MAX);
        if (min_volume)
        {
            float overhang = costs.overhang / 25;
            cost = params.TAR_A * (overhang + params.TAR_B) + params.RELATIVE_F * (/*costs.volume/100*/overhang*params.TAR_C + params.TAR_D + params.TAR_LAF * costs.area_laf * params.use_low_angle_face) / (params.TAR_D + params.CONTOUR_F * costs.contour + params.BOTTOM_F * bottom + params.BOTTOM_HULL_F * bottom_hull + params.TAR_E * overhang + params.TAR_PROJ_AREA * costs.area_projected);
        }
        else {
            float overhang = costs.overhang;
            cost = params.RELATIVE_F * (costs.overhang * params.TAR_C + params.TAR_D + params.TAR_LAF * costs.area_laf * params.use_low_angle_face) / (params.TAR_D + params.CONTOUR_F * costs.contour + params.BOTTOM_F * bottom + params.BOTTOM_HULL_F * bottom_hull + params.TAR_PROJ_AREA * costs.area_projected);
        }
        cost += (costs.bottom < params.BOTTOM_MIN) * 100;// +(costs.height_to_bottom_hull_ratio > params.height_to_bottom_hull_ratio_MIN) * 110;

        costs.unprintability = costs.unprintability = cost;

        return cost;
    }
};

void _orient(OrientMeshs& meshs_,
        const OrientParams           &params,
        std::function<void(unsigned, std::string)> progressfn,
        std::function<bool()>         stopfn)
{
    if (!params.parallel)
    {
        for (size_t i = 0; i != meshs_.size(); ++i) {
            auto& mesh_ = meshs_[i];
            progressfn(i, mesh_.name);
            //auto progressfn_i = [&](unsigned cnt) {progressfn(cnt, "Orienting " + mesh_.name); };
            AutoOrienter orienter(&mesh_, params, /*progressfn_i*/{}, stopfn);
            mesh_.orientation = orienter.process();
            Geometry::rotation_from_two_vectors(mesh_.orientation, { 0,0,1 }, mesh_.axis, mesh_.angle, &mesh_.rotation_matrix);
            BOOST_LOG_TRIVIAL(info) << std::fixed << std::setprecision(3) << "v,phi: " << mesh_.axis.transpose() << ", " << mesh_.angle;
            //flush_logs();
        }
    }
    else {
        tbb::parallel_for(tbb::blocked_range<size_t>(0, meshs_.size()), [&meshs_, &params, progressfn, stopfn](const tbb::blocked_range<size_t>& range) {
            for (size_t i = range.begin(); i != range.end(); ++i) {
                auto& mesh_ = meshs_[i];
                progressfn(i, mesh_.name);
                AutoOrienter orienter(&mesh_, params, {}, stopfn);
                mesh_.orientation = orienter.process();
                Geometry::rotation_from_two_vectors(mesh_.orientation, { 0,0,1 }, mesh_.axis, mesh_.angle, &mesh_.rotation_matrix);
                mesh_.euler_angles = Geometry::extract_euler_angles(mesh_.rotation_matrix);
                BOOST_LOG_TRIVIAL(debug) << "rotation_from_two_vectors: " << mesh_.orientation << "; " << mesh_.axis << "; " << mesh_.angle << "; euler: " << mesh_.euler_angles.transpose();
            }});
    }
}

void orient(OrientMeshs &      arrangables,
             const OrientMeshs &excludes,
             const OrientParams &  params)
{

    auto &cfn = params.stopcondition;
    auto &pri = params.progressind;

    _orient(arrangables, params, pri, cfn);

}

void orient(ModelObject* obj)
{
    auto m = obj->mesh();
    AutoOrienter orienter(&m);
    Vec3d orientation = orienter.process();
    Vec3d axis;
    double angle;
    Geometry::rotation_from_two_vectors(orientation, { 0,0,1 }, axis, angle);

    obj->rotate(angle, axis);
    obj->ensure_on_bed();
}

void orient(ModelInstance* instance)
{
    auto m = instance->get_object()->mesh();
    AutoOrienter orienter(&m);
    Vec3d orientation = orienter.process();
    Vec3d axis;
    double angle;
    Matrix3d rotation_matrix;
    Geometry::rotation_from_two_vectors(orientation, { 0,0,1 }, axis, angle, &rotation_matrix);
    instance->rotate(rotation_matrix);
}


} // namespace arr
} // namespace Slic3r
