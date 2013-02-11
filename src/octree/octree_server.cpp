////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2012 Dominic Marcello
//  Copyright (c) 2012 Zach Byerly
//  Copyright (c) 2012 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#include <hpx/config.hpp>
#include <hpx/async.hpp>
#include <hpx/lcos/future.hpp>
#include <hpx/lcos/future_wait.hpp>
#include <hpx/lcos/wait_all.hpp>

#include <octopus/math.hpp>
#include <octopus/iomanip.hpp>
#include <octopus/indexer2d.hpp>
#include <octopus/octree/octree_server.hpp>
#include <octopus/engine/engine_interface.hpp>
#include <octopus/operators/boost_array_arithmetic.hpp>
#include <octopus/operators/std_vector_arithmetic.hpp>

#include <boost/range/adaptor/map.hpp>

// TODO: Verify the size of parent_U and it's elements when initialization is
// complete.

// NOTE (wash): Is it necessary to solve coarser regions of the grid that are
// being solved at a finer level? I know this is necessary for the multigrid
// solver for the Poisson in the original binary code, but do we need it for
// the non-self gravating code?

namespace octopus
{

std::ostream& operator<<(std::ostream& os, oid_type const& id)
{
    os << ( boost::format("{L%u (%i, %i, %i) %016x%016x}")
          % id.level_
          % id.location_[0]
          % id.location_[1]
          % id.location_[2]
          % id.gid_.get_msb()
          % id.gid_.get_lsb()); 
    return os;
}

// IMPLEMENT: Pass only the state that is needed.
void octree_server::parent_to_child_injection(
    vector3d<std::vector<double> > const& parent_U 
    )
{ // {{{
    boost::uint64_t const ss = science().state_size;
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;
    
    indexer2d<2> const indexer(bw, gnx - bw - 1, bw, gnx - bw - 1);

    child_index c = get_child_index();

    std::vector<double> s1(ss), s2(ss), s3(ss);

    for (boost::uint64_t index = 0; index <= indexer.maximum; ++index)
    {
        boost::uint64_t k = indexer.y(index);
        boost::uint64_t j = indexer.x(index);
        boost::uint64_t k0 = (bw + k) / 2 + c.z() * (gnx / 2 - bw);
        boost::uint64_t j0 = (bw + j) / 2 + c.y() * (gnx / 2 - bw);

        for ( boost::uint64_t i = bw, i0 = bw + c.x() * (gnx / 2 - bw)
            ; i < (gnx - bw)
            ; i += 2, ++i0)
        {
            std::vector<double> const& u = parent_U(i0, j0, k0);

            using namespace octopus::operators;

            s1 = minmod(parent_U(i0 + 1, j0, k0) - u
                      , u - parent_U(i0 - 1, j0, k0));

            s2 = minmod(parent_U(i0, j0 + 1, k0) - u
                      , u - parent_U(i0, j0 - 1, k0));

            s3 = minmod(parent_U(i0, j0, k0 + 1) - u
                      , u - parent_U(i0, j0, k0 - 1));

            // FIXME: The little DSEL makes for clean syntax, but I need to
            // check with Joel Falcou/Heller about how copy intensive this is.
            (*U_)(i + 0, j + 0, k + 0) = u - (s1 + s2 + s3) * 0.25;
            (*U_)(i + 1, j + 0, k + 0) = u + (s1 - s2 - s3) * 0.25;
            (*U_)(i + 0, j + 1, k + 0) = u - (s1 - s2 + s3) * 0.25;
            (*U_)(i + 1, j + 1, k + 0) = u + (s1 + s2 - s3) * 0.25;
            (*U_)(i + 0, j + 0, k + 1) = u - (s1 + s2 - s3) * 0.25;
            (*U_)(i + 1, j + 0, k + 1) = u + (s1 - s2 + s3) * 0.25;
            (*U_)(i + 0, j + 1, k + 1) = u - (s1 - s2 - s3) * 0.25;
            (*U_)(i + 1, j + 1, k + 1) = u + (s1 + s2 + s3) * 0.25;
        }
    }
} // }}}

void octree_server::initialize_queues()
{ // {{{
    OCTOPUS_ASSERT(1 <= config().runge_kutta_order);
    OCTOPUS_ASSERT(3 >= config().runge_kutta_order);

    // NOTE: See the math in the header (right before the declaration of
    // ghost_zone_deps_) to see where these numbers come from. 

    for (boost::uint64_t i = 0; i < (config().runge_kutta_order + 1); ++i)
        ghost_zone_deps_.push_back(sibling_state_dependencies());

    if ((level_ + 1) == config().levels_of_refinement)
        return;

    for (boost::uint64_t i = 0; i < (config().runge_kutta_order + 1); ++i)
        children_state_deps_.push_back(children_state_dependencies());

    for (boost::uint64_t i = 0; i < (config().runge_kutta_order); ++i)
        adjust_flux_deps_.push_back(children_state_dependencies());

    // Just hard-code the size of the refinement queue.
    for (boost::uint64_t i = 0; i < 5; ++i)
        refinement_deps_.push_back(sibling_sync_dependencies()); 
} // }}}

/// \brief Construct a root node. 
octree_server::octree_server(
    back_pointer_type back_ptr
  , octree_init_data const& init
    )
// {{{
  : base_type(back_ptr)
  , mtx_()
  , future_self_(hpx::invalid_id)
  , past_self_(hpx::invalid_id)
  , marked_for_refinement_()
  , ghost_zone_deps_()
  , children_state_deps_()
  , adjust_flux_deps_()
  , refinement_deps_()
  , parent_(init.parent)
  , siblings_()
  , nephews_()
  , level_(init.level)
  , location_(init.location)
  , dx_(init.dx)
  , dx0_(science().initial_spacestep())
  , time_(init.time)
  , offset_(init.offset)
  , origin_(init.origin)
  , step_(0)
  , U_(new vector3d<std::vector<double> >(
        config().grid_node_length
      , std::vector<double>(science().state_size)))
  , U0_()
  , FX_(config().grid_node_length, std::vector<double>(science().state_size))
  , FY_(config().grid_node_length, std::vector<double>(science().state_size))
  , FZ_(config().grid_node_length, std::vector<double>(science().state_size))
  , FO_(new std::vector<double>(science().state_size))
  , FO0_()
  , D_(config().grid_node_length, std::vector<double>(science().state_size))
  , DFO_(science().state_size)
{
    OCTOPUS_ASSERT(back_ptr);
    OCTOPUS_ASSERT(parent_ == hpx::invalid_id);

    initialize_queues();

    for (face i = XL; i < invalid_face; i = face(boost::uint8_t(i + 1)))
    {
        siblings_[i] = octree_client(physical_boundary, client_from_this(), i); 
    } 
} // }}}

/// \brief Construct a child node.
octree_server::octree_server(
    back_pointer_type back_ptr
  , octree_init_data const& init
  , boost::shared_ptr<vector3d<std::vector<double> > > const& parent_U
    )
// {{{
  : base_type(back_ptr)
  , mtx_()
  , future_self_(hpx::invalid_id)
  , past_self_(hpx::invalid_id)
  , marked_for_refinement_()
  , ghost_zone_deps_()
  , children_state_deps_()
  , adjust_flux_deps_()
  , refinement_deps_()
  , parent_(init.parent)
  , siblings_()
  , nephews_()
  , level_(init.level)
  , location_(init.location)
  , dx_(init.dx)
  , dx0_(science().initial_spacestep())
  , time_(init.time)
  , offset_(init.offset)
  , origin_(init.origin)
  , step_(init.step)
  , U_(new vector3d<std::vector<double> >(
        config().grid_node_length
      , std::vector<double>(science().state_size)))
  , U0_()
  , FX_(config().grid_node_length, std::vector<double>(science().state_size))
  , FY_(config().grid_node_length, std::vector<double>(science().state_size))
  , FZ_(config().grid_node_length, std::vector<double>(science().state_size))
  , FO_(new std::vector<double>(science().state_size))
  , FO0_()
  , D_(config().grid_node_length, std::vector<double>(science().state_size))
  , DFO_(science().state_size)
{
    OCTOPUS_ASSERT(back_ptr);

    // Make sure our parent reference is not reference counted.
    OCTOPUS_ASSERT_MSG(
        init.parent.get_management_type() == hpx::id_type::unmanaged,
        "reference cycle detected in child");

    initialize_queues();

    parent_to_child_injection(*parent_U);
} // }}}

// NOTE: Should be thread-safe, offset_ and origin_ are only read, and never
// written to.
double octree_server::x_face(boost::uint64_t i) const
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    double const grid_dim = config().spatial_domain;

    using namespace octopus::operators;

    return double(offset_[0] + i) * dx_ - grid_dim - bw * dx0_ - origin_[0];
} // }}}

// NOTE: Should be thread-safe, offset_ and origin_ are only read, and never
// written to.
double octree_server::y_face(boost::uint64_t i) const
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    double const grid_dim = config().spatial_domain;

    using namespace octopus::operators;

    return double(offset_[1] + i) * dx_ - grid_dim - bw * dx0_ - origin_[1];
} // }}}

// NOTE: Should be thread-safe, offset_ and origin_ are only read, and never
// written to.
double octree_server::z_face(boost::uint64_t i) const
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    double const grid_dim = config().spatial_domain;

    using namespace octopus::operators;

    if (config().reflect_on_z)
        return double(offset_[2] + i) * dx_ - bw * dx0_ - origin_[2];
    else
        return double(offset_[2] + i) * dx_ - grid_dim - bw * dx0_;
} // }}}

void octree_server::prepare_compute_queues()
{ // {{{
    for (boost::uint64_t i = 0; i < ghost_zone_deps_.size(); ++i)
        for (boost::uint64_t j = 0; j < 6; ++j)
            ghost_zone_deps_.at(i)(j).reset();

    if ((level_ + 1) == config().levels_of_refinement)
        return;

    for (boost::uint64_t i = 0; i < children_state_deps_.size(); ++i)
        for (boost::uint64_t j = 0; j < 6; ++j)
            children_state_deps_.at(i)(j).reset();

    for (boost::uint64_t i = 0; i < adjust_flux_deps_.size(); ++i)
        for (boost::uint64_t j = 0; j < 6; ++j)
            adjust_flux_deps_.at(i)(j).reset();
} // }}}

void octree_server::clear_refinement_marks()
{ // {{{
    std::vector<hpx::future<void> > recursion_is_parallelism;
    recursion_is_parallelism.reserve(8);

    for (std::size_t i = 0; i < 8; ++i)
        if (  (hpx::invalid_id != children_[i])
           && (level_ + 2) != config().levels_of_refinement)
            recursion_is_parallelism.push_back
                (children_[i].clear_refinement_marks_async());

    marked_for_refinement_.reset();

    hpx::wait(recursion_is_parallelism);
} // }}}

// Internal utility class.
struct relatives
{ // {{{
    // Exterior/interior is relative to the new child.
    face exterior_x_face;
    face interior_x_face;

    face exterior_y_face;
    face interior_y_face;

    face exterior_z_face;
    face interior_z_face;

    child_index x_sib;
    child_index y_sib; 
    child_index z_sib;

    relatives(child_index kid) : x_sib(kid), y_sib(kid), z_sib(kid)
    {
        ///////////////////////////////////////////////////////////////////////
        // X-axis. 
        if (0 == kid.x())
        {
            // The box that is in the (-1, 0, 0) direction (relative to this
            // child) is external, e.g. one of our siblings (or possibly an
            // AMR/physics boundary. The box that is in the (+1, 0, 0)
            // direction is another one of our siblings. 
    
            x_sib.set_x(1);
    
            OCTOPUS_ASSERT(x_sib.x() == 1);
    
            exterior_x_face = XL; // (-1, 0, 0)
            interior_x_face = XU; // (+1, 0, 0)
        }
        else
        {
            // The box that is in the (+1, 0, 0) direction (relative to this
            // child) is external, e.g. one of our siblings (or possibly an
            // AMR/physics boundary. The box that is in the (-1, 0, 0)
            // direction is another one of our siblings. 
    
            x_sib.set_x(0);
    
            OCTOPUS_ASSERT(x_sib.x() == 0);
    
            exterior_x_face = XU; // (+1, 0, 0)
            interior_x_face = XL; // (-1, 0, 0)
        }
    
        ///////////////////////////////////////////////////////////////////////
        // Y-axis. 
        if (0 == kid.y())
        {
            // The box that is in the (0, -1, 0) direction (relative to this
            // child) is external, e.g. one of our siblings (or possibly an
            // AMR/physics boundary. The box that is in the (0, +1, 0)
            // direction is another one of our siblings. 
    
            y_sib.set_y(1);
    
            OCTOPUS_ASSERT(y_sib.y() == 1);
    
            exterior_y_face = YL; // (0, -1, 0)
            interior_y_face = YU; // (0, +1, 0)
        }
        else
        {
            // The box that is in the (0, +1, 0) direction (relative to this
            // child) is external, e.g. one of our siblings (or possibly an
            // AMR/physics boundary. The box that is in the (0, -1, 0)
            // direction is another one of our siblings. 
    
            y_sib.set_y(0);
    
            OCTOPUS_ASSERT(y_sib.y() == 0);
    
            exterior_y_face = YU; // (0, +1, 0)
            interior_y_face = YL; // (0, -1, 0)
        }
    
        ///////////////////////////////////////////////////////////////////////
        // Z-axis. 
        if (0 == kid.z())
        {
            // The box that is in the (0, 0, -1) direction (relative to this
            // child) is external, e.g. one of our siblings (or possibly an
            // AMR/physics boundary. The box that is in the (0, 0, +1)
            // direction is another one of our siblings. 
    
            z_sib.set_z(1);
    
            OCTOPUS_ASSERT(z_sib.z() == 1);
    
            exterior_z_face = ZL; // (0, 0, -1)
            interior_z_face = ZU; // (0, 0, +1)
        }
        else
        {
            // The box that is in the (0, 0, +1) direction (relative to this
            // child) is external, e.g. one of our siblings (or possibly an
            // AMR/physics boundary. The box that is in the (0, 0, -1)
            // direction is another one of our siblings. 
    
            z_sib.set_z(0);
    
            OCTOPUS_ASSERT(z_sib.z() == 0);
    
            exterior_z_face = ZU; // (0, 0, +1)
            interior_z_face = ZL; // (0, 0, -1)
        }
    
        OCTOPUS_ASSERT(exterior_x_face != invalid_face);
        OCTOPUS_ASSERT(interior_x_face != invalid_face);
        OCTOPUS_ASSERT(exterior_y_face != invalid_face);
        OCTOPUS_ASSERT(interior_y_face != invalid_face);
        OCTOPUS_ASSERT(exterior_z_face != invalid_face);
        OCTOPUS_ASSERT(interior_z_face != invalid_face);
    }
}; // }}}

// IMPLEMENT: Pass only the state that is needed.
void octree_server::create_child(
    child_index kid
    )
{ // {{{
    OCTOPUS_ASSERT_FMT_MSG(children_[kid] == hpx::invalid_id,
        "child already exists, child(%1%)", kid);

    relatives r(kid);

    octree_init_data kid_init;

    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    using namespace octopus::operators;

    kid_init.parent   = reference_from_this(); 
    kid_init.level    = level_ + 1; 
    kid_init.location = location_ * 2 + kid.array(); 
    kid_init.dx       = dx_ * 0.5;
    kid_init.time     = time_;
    kid_init.offset   = offset_ * 2 + bw + (kid.array() * (gnx - 2 * bw));
    kid_init.origin   = origin_;
    kid_init.step     = step_;

    // Create the child. 
    octree_client kid_client(create_octree(kid_init, U_));

    OCTOPUS_ASSERT(kid_client != hpx::invalid_id);

    children_[kid] = kid_client;
} // }}}

// REVIEW: Move to header?
void octree_server::set_sibling(
    face f
  , octree_client const& sib
    )
{ // {{{
    mutex_type::scoped_lock l(mtx_);

    if (amr_boundary == siblings_[f].kind() && sib.real())
    {
        octree_client old = siblings_[f];
        siblings_[f] = sib; 

        hpx::util::unlock_the_lock<mutex_type::scoped_lock> ul(l);
        old.remove_nephew(reference_from_this(), invert(f));
    }

    else 
    {
        if (siblings_[f].real() && sib.real())
            OCTOPUS_ASSERT(siblings_[f] == sib); 

        siblings_[f] = sib;  
    }
} // }}}

void octree_server::tie_sibling(
    face target_f
  , octree_client const& target_sib
    )
{ // {{{
    // Locks.
    child_index target_kid = get_child_index();

    face source_f = invert(target_f);

    child_index source_kid = invert(target_f, target_kid);

    // Locks.
    set_sibling(target_f, target_sib);
    
    octree_client source_sib(get_gid());

    // Locks.
    target_sib.set_sibling(source_f, source_sib);  
} // }}}

void octree_server::set_child_sibling(
    child_index kid
  , face f
  , octree_client const& sib
    )
{ // {{{
    octree_client child;

    {
        mutex_type::scoped_lock l(mtx_);
        child = children_[kid];
    }

    if (invalid_boundary != child.kind())
        // Locks.
        child.set_sibling(f, sib);

    else if (!marked_for_refinement_.test(kid))
    {
        // Exterior AMR boundary.
        octree_client bound(amr_boundary
                          , client_from_this()
                          , f 
                          , kid
                          , sib.get_offset() 
                          , get_offset()
                            ); 

        {
            mutex_type::scoped_lock l(mtx_);
            nephews_.insert(interpolation_data(sib, f, bound.offset_));
        }

        // Locks.
        sib.set_sibling(invert(f), bound);
    }
} // }}}

void octree_server::tie_child_sibling(
    child_index target_kid
  , face target_f
  , octree_client const& target_sib
    )
{ // {{{
    octree_client child;

    {
        mutex_type::scoped_lock l(mtx_);
        child = children_[target_kid];
    }

    if (invalid_boundary != child.kind())
        // Locks.
        child.tie_sibling(target_f, target_sib);

    else if (!marked_for_refinement_.test(target_kid))
    {
        // Exterior AMR boundary.
        octree_client bound(amr_boundary
                          , client_from_this()
                          , target_f 
                          , target_kid
                          , target_sib.get_offset() 
                          , get_offset()
                            ); 

        {
            mutex_type::scoped_lock l(mtx_);
            nephews_.insert(
                interpolation_data(target_sib, target_f, bound.offset_));
        }

        // Locks.
        target_sib.set_sibling(invert(target_f), bound);
    }
} // }}} 

///////////////////////////////////////////////////////////////////////////////
// Ghost zone communication

// {{{ Description of the algorithm for send_ghost_zone 
/// Pseudo code based on the original code (GS = GNX - 2 BW = dimensions of
/// the grid without ghostzones):
///
/// for i in [0, BW)
///     for j in [BW, GNX - BW)
///         for k in [BW, GNX - BW)
///             U(i, j, k) = sibling[XL].U(GNX - 2 * BW + i, j, k) 
///              
/// for i in [GNX - BW, GNX)
///     for j in [BW, GNX - BW)
///         for k in [BW, GNX - BW)
///             U(i, j, k) = sibling[XU].U(-GNX - 2 * BW + i, j, k) 
///
/// for i in [BW, GNX - BW)
///     for j in [0, BW)
///         for k in [BW, GNX - BW)
///             U(i, j, k) = sibling[YL].U(i, GNX - 2 * BW + j, k) 
///
/// for i in [BW, GNX - BW)
///     for j in [GNX - BW, GNX)
///         for k in [BW, GNX - BW)
///             U(i, j, k) = sibling[YU].U(i, -GNX - 2 * BW + j, k) 
///
/// for i in [BW, GNX - BW)
///     for j in [BW, GNX - BW)
///         for k in [0, BW)
///             U(i, j, k) = sibling[ZL].U(i, j, GNX - 2 * BW + k) 
///
/// for i in [BW, GNX - BW)
///     for j in [BW, GNX - BW)
///         for k in [GNX - BW, GNX)
///             U(i, j, k) = sibling[ZU].U(i, j, -GNX - 2 * BW + k) 
// }}}

// REVIEW: I think step 2.) can come before step 1.).
/// 0.) Push ghost zone data to our siblings and determine which ghost zones we
///     will receive.
/// 1.) Wait for our ghost zones to be delivered by our siblings.
/// 2.) Push ghost zone data to our nephews.
void octree_server::communicate_ghost_zones(
    boost::uint64_t phase
    )
{ // {{{
    OCTOPUS_ASSERT_FMT_MSG(
        phase < ghost_zone_deps_.size(),
        "phase (%1%) is greater than the ghost zone queue length (%2%)",
        phase % ghost_zone_deps_.size());

    std::vector<hpx::future<void> > dependencies;
    dependencies.reserve(6);

    ///////////////////////////////////////////////////////////////////////////
    // Push ghost zone data to our siblings and determine which ghost zones we
    // will receive.
    for (boost::uint64_t i = 0; i < 6; ++i)
    {
        face const fi = face(i);

        OCTOPUS_ASSERT(invalid_boundary != siblings_[i].kind());

        if (siblings_[i].real())
        {
            // Set up a callback which adds the ghost zones to our state
            // when they arrive. 
            dependencies.push_back( 
                ghost_zone_deps_.at(phase)(i).then(
                    boost::bind(&octree_server::add_ghost_zone_callback,
                        this, fi, _1))); 

            // Send out ghost zone data for our neighbors.
            // FIXME: send_ghost_zone is somewhat compute intensive,
            // parallelize?
            siblings_[i].receive_ghost_zone_push(step_, phase, invert(fi),
                send_ghost_zone(invert(fi)));
        }

        else if (amr_boundary == siblings_[i].kind())
        {
            // Set up a callback which adds the ghost zones to our state
            // when they arrive. 
            dependencies.push_back(
                ghost_zone_deps_.at(phase)(i).then(boost::bind
                    (&octree_server::add_ghost_zone_callback, this, fi, _1))); 
        }
    }

    // Handle physical boundaries.
    // FIXME: Optimize.
    for (boost::uint64_t i = 0; i < 6; ++i)
    {
        face const fi = face(i);

        if (physical_boundary == siblings_[i].kind())
            add_ghost_zone(fi, boost::move(send_mapped_ghost_zone(fi)));
    }

    ///////////////////////////////////////////////////////////////////////////
    // Wait for our ghost zones to be delivered by our siblings.
    for (boost::uint64_t i = 0; i < dependencies.size(); ++i)
        dependencies[i].move();

    ///////////////////////////////////////////////////////////////////////////
    // Push ghost zone data to our nephews.
    std::vector<hpx::future<void> > nephews;
    nephews.reserve(nephews_.size());

    BOOST_FOREACH(interpolation_data const& nephew, nephews_) 
    {
        nephews.push_back(nephew.subject.receive_ghost_zone_async
            (step_, phase, invert(nephew.direction),
                send_interpolated_ghost_zone(nephew.direction
                                           , nephew.offset)));
    }

    hpx::wait(nephews);
} // }}}

void octree_server::add_ghost_zone(
    face f ///< Bound parameter.
  , BOOST_RV_REF(vector3d<std::vector<double> >) zone
    )
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    // The index of the futures in the vector is the face.
    switch (f)
    {
        ///////////////////////////////////////////////////////////////////////
        // X-axis.
        /// for i in [0, BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[XL].U(GNX - 2 * BW + i, j, k) 
        case XL:
        {
            OCTOPUS_ASSERT(zone.x_length() == bw);           // [0, BW)
            OCTOPUS_ASSERT(zone.y_length() == gnx - 2 * bw); // [BW, GNX - BW)  
            OCTOPUS_ASSERT(zone.z_length() == gnx - 2 * bw); // [BW, GNX - BW)

            for (boost::uint64_t i = 0; i < bw; ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i;
                        boost::uint64_t const jj = j - bw;
                        boost::uint64_t const kk = k - bw; 

                        (*U_)(i, j, k) = zone(ii, jj, kk);
                    }

            return;
        } 

        /// for i in [GNX - BW, GNX)
        ///     for j in [BW, GNX - BW)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[XU].U(-GNX - 2 * BW + i, j, k) 
        case XU:
        {
            OCTOPUS_ASSERT(zone.x_length() == bw);           // [GNX - BW, GNX)
            OCTOPUS_ASSERT(zone.y_length() == gnx - 2 * bw); // [BW, GNX - BW)  
            OCTOPUS_ASSERT(zone.z_length() == gnx - 2 * bw); // [BW, GNX - BW)

            for (boost::uint64_t i = gnx - bw; i < gnx; ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - (gnx - bw);
                        boost::uint64_t const jj = j - bw;
                        boost::uint64_t const kk = k - bw; 

                        (*U_)(i, j, k) = zone(ii, jj, kk);
                    }

            return;
        }

        ///////////////////////////////////////////////////////////////////////
        // Y-axis.
        /// for i in [BW, GNX - BW)
        ///     for j in [0, BW)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[YL].U(i, GNX - 2 * BW + j, k) 
        case YL:
        {
            OCTOPUS_ASSERT(zone.x_length() == gnx - 2 * bw); // [BW, GNX - BW)  
            OCTOPUS_ASSERT(zone.y_length() == bw);           // [0, BW)
            OCTOPUS_ASSERT(zone.z_length() == gnx - 2 * bw); // [BW, GNX - BW)

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = 0; j < bw; ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j;
                        boost::uint64_t const kk = k - bw; 

                        (*U_)(i, j, k) = zone(ii, jj, kk);
                    }

            return;
        } 

        /// for i in [BW, GNX - BW)
        ///     for j in [GNX - BW, GNX)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[YU].U(i, -GNX - 2 * BW + j, k) 
        case YU:
        {
            OCTOPUS_ASSERT(zone.x_length() == gnx - 2 * bw); // [BW, GNX - BW)  
            OCTOPUS_ASSERT(zone.y_length() == bw);           // [GNX - BW, GNX)
            OCTOPUS_ASSERT(zone.z_length() == gnx - 2 * bw); // [BW, GNX - BW)

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = gnx - bw; j < gnx; ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - (gnx - bw);
                        boost::uint64_t const kk = k - bw; 

                        (*U_)(i, j, k) = zone(ii, jj, kk);
                    }

            return;
        }

        ///////////////////////////////////////////////////////////////////////
        // Z-axis.
        /// for i in [BW, GNX - BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [0, BW)
        ///             U(i, j, k) = sibling[ZL].U(i, j, GNX - 2 * BW + k) 
        case ZL:
        {
            OCTOPUS_ASSERT(zone.x_length() == gnx - 2 * bw); // [BW, GNX - BW)  
            OCTOPUS_ASSERT(zone.y_length() == gnx - 2 * bw); // [BW, GNX - BW)
            OCTOPUS_ASSERT(zone.z_length() == bw);           // [0, BW)

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = 0; k < bw; ++k)
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - bw; 
                        boost::uint64_t const kk = k;

                        (*U_)(i, j, k) = zone(ii, jj, kk);
                    }

            return;
        } 

        /// for i in [BW, GNX - BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [GNX - BW, GNX)
        ///             U(i, j, k) = sibling[ZU].U(i, j, -GNX - 2 * BW + k) 
        case ZU:
        {
            OCTOPUS_ASSERT(zone.x_length() == gnx - 2 * bw); // [BW, GNX - BW)  
            OCTOPUS_ASSERT(zone.y_length() == gnx - 2 * bw); // [BW, GNX - BW)
            OCTOPUS_ASSERT(zone.z_length() == bw);           // [GNX - BW, GNX)

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = gnx - bw; k < gnx; ++k)
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - bw; 
                        boost::uint64_t const kk = k - (gnx - bw);

                        (*U_)(i, j, k) = zone(ii, jj, kk);
                    }

            return;
        }

        default:
        {
            OCTOPUS_ASSERT_MSG(false, "face shouldn't be out-of-bounds");
        }
    }; 
} // }}} 

// Who ya gonna call? Ghostbusters!
vector3d<std::vector<double> > octree_server::send_ghost_zone(
    face f ///< Our direction, relative to the caller.
    )
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    switch (f)
    {
        ///////////////////////////////////////////////////////////////////////
        // X-axis.
        /// for i in [0, BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[XL].U(GNX - 2 * BW + i, j, k) 
        ///              
        case XL:
        {
            vector3d<std::vector<double> > zone
                (
                /* [0, BW) */         bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = 0; i < bw; ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i;
                        boost::uint64_t const jj = j - bw;
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(gnx - 2 * bw + i, j, k);
                    }

            return zone;
        } 

        /// for i in [GNX - BW, GNX)
        ///     for j in [BW, GNX - BW)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[XU].U(-GNX - 2 * BW + i, j, k) 
        case XU:
        {
            vector3d<std::vector<double> > zone
                (
                /* [GNX - BW, GNX) */ bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = gnx - bw; i < gnx; ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - (gnx - bw);
                        boost::uint64_t const jj = j - bw;
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(-gnx + 2 * bw + i, j, k);
                    }

            return zone;
        }

        ///////////////////////////////////////////////////////////////////////
        // Y-axis.
        /// for i in [BW, GNX - BW)
        ///     for j in [0, BW)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[YL].U(i, GNX - 2 * BW + j, k) 
        ///
        case YL:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [0, BW) */         bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = 0; j < bw; ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j;
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(i, gnx - 2 * bw + j, k);
                    }

            return zone;
        } 

        /// for i in [BW, GNX - BW)
        ///     for j in [GNX - BW, GNX)
        ///         for k in [BW, GNX - BW)
        ///             U(i, j, k) = sibling[YU].U(i, -GNX - 2 * BW + j, k) 
        case YU:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [GNX - BW, GNX) */ bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = gnx - bw; j < gnx; ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - (gnx - bw);
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(i, -gnx + 2 * bw + j, k);
                    }

            return zone;
        }

        ///////////////////////////////////////////////////////////////////////
        // Z-axis.
        /// for i in [BW, GNX - BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [0, BW)
        ///             U(i, j, k) = sibling[ZL].U(i, j, GNX - 2 * BW + k) 
        case ZL:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [0, BW) */         bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = 0; k < bw; ++k)
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - bw; 
                        boost::uint64_t const kk = k;

                        zone(ii, jj, kk) = (*U_)(i, j, gnx - 2 * bw + k);
                    }

            return zone;
        } 

        /// for i in [BW, GNX - BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [GNX - BW, GNX)
        ///             U(i, j, k) = sibling[ZU].U(i, j, -GNX - 2 * BW + k) 
        case ZU:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [GNX - BW, GNX) */ bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = gnx - bw; k < gnx; ++k)
                    {
                        // Adjusted indices. 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - bw; 
                        boost::uint64_t const kk = k - (gnx - bw);

                        zone(ii, jj, kk) = (*U_)(i, j, -gnx + 2 * bw + k);
                    }

            return zone;
        }

        default:
        {
            OCTOPUS_ASSERT_MSG(false, "face shouldn't be out-of-bounds");
        }
    }; 

    // Unreachable.
    OCTOPUS_ASSERT(false);
    return vector3d<std::vector<double> >(); 
} // }}} 

///////////////////////////////////////////////////////////////////////////////
vector3d<std::vector<double> > octree_server::send_interpolated_ghost_zone(
    face f ///< Our direction, relative to the caller.
  , boost::array<boost::int64_t, 3> amr_offset
    ) 
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    vector3d<std::vector<double> > output; 

    boost::uint64_t count = 0;

    switch (f)
    {
        ///////////////////////////////////////////////////////////////////////
        // X-axis.
        case XL:
        {
            output.resize
                (
                /* [0, BW) */         bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = bw; i < (2 * bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k)
                    {
                        using namespace octopus::operators;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices 
                        boost::uint64_t const i_out = i - bw;
                        boost::uint64_t const j_out = j - bw;
                        boost::uint64_t const k_out = k - bw; 

                        ///////////////////////////////////////////////////////
                        bool const i0 = (amr_offset[0] + i) % 2;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (input). 
                        boost::uint64_t const i_in = (amr_offset[0] + i) / 2;
                        boost::uint64_t const j_in = (amr_offset[1] + j) / 2;
                        boost::uint64_t const k_in = (amr_offset[2] + k) / 2;

                        std::vector<double> u = (*U_)(i_in, j_in, k_in); 

                        u = minmod((*U_)(i_in + 1, j_in, k_in) - u
                                 , u - (*U_)(i_in - 1, j_in, k_in));

                        if (1 == i0)
                            u = -u;

                        output(i_out, j_out, k_out)  = (*U_)(i_in, j_in, k_in);
                        output(i_out, j_out, k_out) -= u * 0.25;

                        // FIXME: This is too specific to Dominic/Zach's
                        // code, move this into the science table.
                        OCTOPUS_ASSERT(output(i_out, j_out, k_out)[0] > 0.0);

                        ++count;
                    }

            break;
        } 

        case XU:
        {
            output.resize
                (
                /* [GNX - BW, GNX) */ bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = gnx - 2 * bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        using namespace octopus::operators;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (output). 
                        boost::uint64_t const i_out = i - (gnx - 2 * bw);
                        boost::uint64_t const j_out = j - bw;
                        boost::uint64_t const k_out = k - bw; 

                        ///////////////////////////////////////////////////////
                        bool const i0 = (amr_offset[0] + i) % 2;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (input). 
                        boost::uint64_t const i_in = (amr_offset[0] + i) / 2;
                        boost::uint64_t const j_in = (amr_offset[1] + j) / 2;
                        boost::uint64_t const k_in = (amr_offset[2] + k) / 2;

                        std::vector<double> u = (*U_)(i_in, j_in, k_in); 

                        u = minmod((*U_)(i_in + 1, j_in, k_in) - u
                                 , u - (*U_)(i_in - 1, j_in, k_in));

                        if (1 == i0)
                            u = -u;

                        output(i_out, j_out, k_out)  = (*U_)(i_in, j_in, k_in);
                        output(i_out, j_out, k_out) -= u * 0.25;

                        // FIXME: This is too specific to Dominic/Zach's
                        // code, move this into the science table.
                        OCTOPUS_ASSERT(output(i_out, j_out, k_out)[0] > 0.0);

                        ++count;
                    }

            break;
        }

        ///////////////////////////////////////////////////////////////////////
        // Y-axis.
        case YL:
        {
            output.resize
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [0, BW) */         bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (2 * bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        using namespace octopus::operators;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (output). 
                        boost::uint64_t const i_out = i - bw;
                        boost::uint64_t const j_out = j - bw;
                        boost::uint64_t const k_out = k - bw; 

                        //////////////////////////////////////////////////////
                        bool const j0 = (amr_offset[1] + j) % 2;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (input). 
                        boost::uint64_t const i_in = (amr_offset[0] + i) / 2;
                        boost::uint64_t const j_in = (amr_offset[1] + j) / 2;
                        boost::uint64_t const k_in = (amr_offset[2] + k) / 2;

                        std::vector<double> u = (*U_)(i_in, j_in, k_in); 

                        u = minmod((*U_)(i_in, j_in + 1, k_in) - u
                                 , u - (*U_)(i_in, j_in - 1, k_in));

                        if (1 == j0)
                            u = -u;

                        output(i_out, j_out, k_out)  = (*U_)(i_in, j_in, k_in);
                        output(i_out, j_out, k_out) -= u * 0.25;

                        // FIXME: This is too specific to Dominic/Zach's
                        // code, move this into the science table.
                        OCTOPUS_ASSERT(output(i_out, j_out, k_out)[0] > 0.0);

                        ++count;
                    }

            break;
        } 

        case YU:
        {
            output.resize
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [GNX - BW, GNX) */ bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = gnx - 2 * bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        using namespace octopus::operators;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (output). 
                        boost::uint64_t const i_out = i - bw;
                        boost::uint64_t const j_out = j - (gnx - 2 * bw);
                        boost::uint64_t const k_out = k - bw; 

                        ///////////////////////////////////////////////////////
                        bool const j0 = (amr_offset[1] + j) % 2;

                        ///////////////////////////////////////////////////////
                        boost::uint64_t const i_in = (amr_offset[0] + i) / 2;
                        boost::uint64_t const j_in = (amr_offset[1] + j) / 2;
                        boost::uint64_t const k_in = (amr_offset[2] + k) / 2;

                        std::vector<double> u = (*U_)(i_in, j_in, k_in); 

                        u = minmod((*U_)(i_in, j_in + 1, k_in) - u
                                 , u - (*U_)(i_in, j_in - 1, k_in));

                        if (1 == j0)
                            u = -u;

                        output(i_out, j_out, k_out)  = (*U_)(i_in, j_in, k_in);
                        output(i_out, j_out, k_out) -= u * 0.25;

                        // FIXME: This is too specific to Dominic/Zach's
                        // code, move this into the science table.
                        OCTOPUS_ASSERT(output(i_out, j_out, k_out)[0] > 0.0);

                        ++count;
                    }

            break;
        }

        ///////////////////////////////////////////////////////////////////////
        // Z-axis.
        case ZL:
        {
            output.resize
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [0, BW) */         bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = bw; k < (2 * bw); ++k)
                    {
                        using namespace octopus::operators;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (output). 
                        boost::uint64_t const i_out = i - bw;
                        boost::uint64_t const j_out = j - bw; 
                        boost::uint64_t const k_out = k - bw;

                        ///////////////////////////////////////////////////////
                        bool const k0 = (amr_offset[2] + k) % 2;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (input). 
                        boost::uint64_t const i_in = (amr_offset[0] + i) / 2;
                        boost::uint64_t const j_in = (amr_offset[1] + j) / 2;
                        boost::uint64_t const k_in = (amr_offset[2] + k) / 2;

                        std::vector<double> u = (*U_)(i_in, j_in, k_in); 

                        u = minmod((*U_)(i_in, j_in, k_in + 1) - u
                                 , u - (*U_)(i_in, j_in, k_in - 1));

                        if (1 == k0)
                            u = -u;

                        output(i_out, j_out, k_out)  = (*U_)(i_in, j_in, k_in);
                        output(i_out, j_out, k_out) -= u * 0.25;

                        // FIXME: This is too specific to Dominic/Zach's
                        // code, move this into the science table.
                        OCTOPUS_ASSERT(output(i_out, j_out, k_out)[0] > 0.0);

                        ++count;
                    }

            break;
        } 

        case ZU:
        {
            output.resize
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [GNX - BW, GNX) */ bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = gnx - 2 * bw; k < (gnx - bw); ++k)
                    {
                        using namespace octopus::operators;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (output). 
                        boost::uint64_t const i_out = i - bw;
                        boost::uint64_t const j_out = j - bw; 
                        boost::uint64_t const k_out = k - (gnx - 2 * bw);

                        ///////////////////////////////////////////////////////
                        bool const k0 = (amr_offset[2] + k) % 2;

                        ///////////////////////////////////////////////////////
                        // Adjusted indices (input). 
                        boost::uint64_t const i_in = (amr_offset[0] + i) / 2;
                        boost::uint64_t const j_in = (amr_offset[1] + j) / 2;
                        boost::uint64_t const k_in = (amr_offset[2] + k) / 2;

                        std::vector<double> u = (*U_)(i_in, j_in, k_in); 

                        u = minmod((*U_)(i_in, j_in, k_in + 1) - u
                                 , u - (*U_)(i_in, j_in, k_in - 1));

                        if (1 == k0)
                            u = -u;

                        output(i_out, j_out, k_out)  = (*U_)(i_in, j_in, k_in);
                        output(i_out, j_out, k_out) -= u * 0.25;

                        // FIXME: This is too specific to Dominic/Zach's
                        // code, move this into the science table.
                        OCTOPUS_ASSERT(output(i_out, j_out, k_out)[0] > 0.0);

                        ++count;
                    }

            break;
        }

        default:
        {
            OCTOPUS_ASSERT_MSG(false, "face shouldn't be out-of-bounds");
        }
    }; 

    OCTOPUS_ASSERT(output.size() == count);

    return output; 
} // }}}

// FIXME: Range checking.
boost::array<boost::uint64_t, 3> map_location(
    face f ///< Our direction, relative to the caller.
  , boost::uint64_t i
  , boost::uint64_t j
  , boost::uint64_t k
    )
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;
    bool const reflect_on_z = config().reflect_on_z;

    boost::array<boost::uint64_t, 3> v;

    v[0] = i;
    v[1] = j;
    v[2] = k;

    switch (invert(f))
    {
        case XU:
            v[0] = bw;
            break;
        case XL:
            v[0] = gnx - bw - 1;
            break;
    
        case YU:
            v[1] = bw;
            break;
        case YL:
            v[1] = gnx - bw - 1;
            break;
    
        case ZU:
            v[2] = (reflect_on_z ? gnx - k - 1 : bw);
            break;
        case ZL:
            v[2] = (reflect_on_z ? gnx - k - 1 : gnx - bw - 1);
            break;

        default:
            OCTOPUS_ASSERT(false);
            break;
    }

    return v;
} // }}}

vector3d<std::vector<double> > octree_server::send_mapped_ghost_zone(
    face f ///< Our direction, relative to the caller.
    )
{ // {{{ 
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    switch (f)
    {
        ///////////////////////////////////////////////////////////////////////
        // X-axis.
        /// for i in [0, BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [BW, GNX - BW)
        case XL:
        {
            vector3d<std::vector<double> > zone
                (
                /* [0, BW) */         bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = 0; i < bw; ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        boost::array<boost::uint64_t, 3> v =
                            map_location(f, gnx - 2 * bw + i, j, k);

                        // Adjusted indices (for output ghost zone). 
                        boost::uint64_t const ii = i;
                        boost::uint64_t const jj = j - bw;
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(v);

                        science().enforce_outflow
                            (f, x_face_coords(v[0] + 1, v[1], v[2]));
                    }

            return zone;
        } 

        /// for i in [GNX - BW, GNX)
        ///     for j in [BW, GNX - BW)
        ///         for k in [BW, GNX - BW)
        case XU:
        {
            vector3d<std::vector<double> > zone
                (
                /* [GNX - BW, GNX) */ bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = gnx - bw; i < gnx; ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        boost::array<boost::uint64_t, 3> v =
                            map_location(f, -gnx + 2 * bw + i, j, k);

                        // Adjusted indices (for output ghost zone). 
                        boost::uint64_t const ii = i - (gnx - bw);
                        boost::uint64_t const jj = j - bw;
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(v);

                        science().enforce_outflow
                            (f, x_face_coords(v[0], v[1], v[2]));
                    }

            return zone;
        }

        ///////////////////////////////////////////////////////////////////////
        // Y-axis.
        /// for i in [BW, GNX - BW)
        ///     for j in [0, BW)
        ///         for k in [BW, GNX - BW)
        case YL:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [0, BW) */         bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = 0; j < bw; ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        boost::array<boost::uint64_t, 3> v =
                            map_location(f, i, gnx - 2 * bw + j, k);

                        // Adjusted indices (for output ghost zone). 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j;
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(v);

                        science().enforce_outflow
                            (f, y_face_coords(v[0], v[1] + 1, v[2]));
                    }

            return zone;
        } 

        /// for i in [BW, GNX - BW)
        ///     for j in [GNX - BW, GNX)
        ///         for k in [BW, GNX - BW)
        case YU:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [GNX - BW, GNX) */ bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = gnx - bw; j < gnx; ++j)
                    for (boost::uint64_t k = bw; k < (gnx - bw); ++k) 
                    {
                        boost::array<boost::uint64_t, 3> v =
                            map_location(f, i, -gnx + 2 * bw + j, k);

                        // Adjusted indices (for output ghost zone). 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - (gnx - bw);
                        boost::uint64_t const kk = k - bw; 

                        zone(ii, jj, kk) = (*U_)(v);

                        science().enforce_outflow
                            (f, y_face_coords(v[0], v[1], v[2]));
                    }

            return zone;
        }

        ///////////////////////////////////////////////////////////////////////
        // Z-axis.
        /// for i in [BW, GNX - BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [0, BW)
        case ZL:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [0, BW) */         bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = 0; k < bw; ++k)
                    {
                        boost::array<boost::uint64_t, 3> v =
                            map_location(f, i, j, gnx - 2 * bw + k);

                        // Adjusted indices (for output ghost zone). 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - bw; 
                        boost::uint64_t const kk = k;

                        zone(ii, jj, kk) = (*U_)(v);

                        if (config().reflect_on_z)
                            science().reflect_z(zone(ii, jj, kk));
                        else
                            science().enforce_outflow
                                (f, z_face_coords(v[0], v[1], v[2] + 1));

                        OCTOPUS_ASSERT(zone(ii, jj, kk)[0] > 0.0);
                    }

            return zone;
        } 

        /// for i in [BW, GNX - BW)
        ///     for j in [BW, GNX - BW)
        ///         for k in [GNX - BW, GNX)
        case ZU:
        {
            vector3d<std::vector<double> > zone
                (
                /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [BW, GNX - BW) */  gnx - 2 * bw
              , /* [GNX - BW, GNX) */ bw
                );

            for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
                for (boost::uint64_t j = bw; j < (gnx - bw); ++j) 
                    for (boost::uint64_t k = gnx - bw; k < gnx; ++k)
                    {
                        boost::array<boost::uint64_t, 3> v =  
                            map_location(f, i, j, -gnx + 2 * bw + k);

                        // Adjusted indices (for output ghost zone). 
                        boost::uint64_t const ii = i - bw;
                        boost::uint64_t const jj = j - bw; 
                        boost::uint64_t const kk = k - (gnx - bw);

                        zone(ii, jj, kk) = (*U_)(v);

                        if (config().reflect_on_z)
                            science().reflect_z(zone(ii, jj, kk));
                        else
                            science().enforce_outflow
                                (f, z_face_coords(v[0], v[1], v[2]));

                        OCTOPUS_ASSERT(zone(ii, jj, kk)[0] > 0.0);
                    }

            return zone;
        }

        default:
        {
            OCTOPUS_ASSERT_MSG(false, "face shouldn't be out-of-bounds");
        }
    };

    // Unreachable.
    OCTOPUS_ASSERT(false);
    return vector3d<std::vector<double> >(); 
} // }}}

///////////////////////////////////////////////////////////////////////////////
// Child -> parent injection of state.

void octree_server::child_to_parent_injection(
    boost::uint64_t phase
    )
{ // {{{
    std::vector<hpx::future<void> > recursion_is_parallelism;

    recursion_is_parallelism.reserve(8);
    
    for (boost::uint64_t i = 0; i < 8; ++i)
        if (hpx::invalid_id != children_[i])
            recursion_is_parallelism.push_back
                (children_[i].child_to_parent_injection_async(phase));

    // Invoke the kernel on ourselves ...
    child_to_parent_injection_kernel(phase); 

    // ... and block while our children compute.
    hpx::wait(recursion_is_parallelism); 
} // }}}

/// 0.) Wait for all children to signal us.
/// 1.) Signal our parent.
void octree_server::child_to_parent_injection_kernel(
    boost::uint64_t phase
    )
{ // {{{
    std::vector<hpx::future<void> > dependencies;
    
    bool has_children = false;

    if ((level_ + 1) == config().levels_of_refinement)
        has_children = false;

    else
    {
        // children_state_queue is only allocated if the max refinement level
        // isn't the current level.
        OCTOPUS_ASSERT_FMT_MSG(
            phase < children_state_deps_.size(),
            "phase (%1%) is greater than the children state queue length (%2%)",
            phase % children_state_deps_.size());

        dependencies.reserve(8);

        for (boost::uint64_t i = 0; i < 8; ++i)
            if (children_[i] != hpx::invalid_id)
            {
                has_children = true;
                break;
            }
    }

    if (has_children)
    {
        OCTOPUS_ASSERT((level_ + 1) != config().levels_of_refinement);

        for (boost::uint64_t i = 0; i < 8; ++i)
            if (children_[i] != hpx::invalid_id)
                dependencies.push_back(
                    children_state_deps_.at(phase)(i).then(
                        boost::bind(&octree_server::add_child_state,
                            this, child_index(i), _1))); 
    }

    ///////////////////////////////////////////////////////////////////////////
    // Wait for all children to signal us.
    for (boost::uint64_t i = 0; i < dependencies.size(); ++i)
        dependencies[i].move();

    ///////////////////////////////////////////////////////////////////////////
    // Send a signal to our parent (if we have a parent). 
    if (parent_ != hpx::invalid_id)
    {
        OCTOPUS_ASSERT(level_ != 0);

        parent_.receive_child_state_push(step_, phase,
            get_child_index(), send_child_state());
    }
} // }}}

void octree_server::add_child_state(
    child_index idx ///< Bound parameter.
  , hpx::future<vector3d<std::vector<double> > > state_f
    )
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    vector3d<std::vector<double> > state(state_f.move());

    OCTOPUS_ASSERT(state.x_length() == ((gnx - 2 * bw) / 2));
    OCTOPUS_ASSERT(state.y_length() == ((gnx - 2 * bw) / 2));
    OCTOPUS_ASSERT(state.z_length() == ((gnx - 2 * bw) / 2));

    for (boost::uint64_t i = 0; i < ((gnx - 2 * bw) / 2); ++i)
        for (boost::uint64_t j = 0; j < ((gnx - 2 * bw) / 2); ++j)
            for (boost::uint64_t k = 0; k < ((gnx - 2 * bw) / 2); ++k)
            {
                // Adjusted indices (for destination).
                boost::uint64_t const id
                    = i + bw + idx.x() * ((gnx / 2) - bw);
                boost::uint64_t const jd
                    = j + bw + idx.y() * ((gnx / 2) - bw);
                boost::uint64_t const kd
                    = k + bw + idx.z() * ((gnx / 2) - bw);

                (*U_)(id, jd, kd) = state(i, j, k); 
            }
} // }}}

vector3d<std::vector<double> > octree_server::send_child_state()
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    vector3d<std::vector<double> > state
        (
        (gnx - 2 * bw) / 2
      , (gnx - 2 * bw) / 2
      , (gnx - 2 * bw) / 2
        ); 

    child_index c = get_child_index();

    for (boost::uint64_t i = bw; i < (gnx - bw); i += 2)
        for (boost::uint64_t j = bw; j < (gnx - bw); j += 2)
            for (boost::uint64_t k = bw; k < (gnx - bw); k += 2)
            {
                // Adjusted indices (for output state).
                boost::uint64_t const ii = ((i + bw) / 2) - bw;
                boost::uint64_t const jj = ((j + bw) / 2) - bw; 
                boost::uint64_t const kk = ((k + bw) / 2) - bw; 

                using namespace octopus::operators;

                state(ii, jj, kk)  = (*U_)(i + 0, j + 0, k + 0);
                state(ii, jj, kk) += (*U_)(i + 1, j + 0, k + 0);
                state(ii, jj, kk) += (*U_)(i + 0, j + 1, k + 0);
                state(ii, jj, kk) += (*U_)(i + 1, j + 1, k + 0);
                state(ii, jj, kk) += (*U_)(i + 0, j + 0, k + 1);
                state(ii, jj, kk) += (*U_)(i + 1, j + 0, k + 1);
                state(ii, jj, kk) += (*U_)(i + 0, j + 1, k + 1);
                state(ii, jj, kk) += (*U_)(i + 1, j + 1, k + 1);
                state(ii, jj, kk) *= 0.125;
            }

    return state; 
} // }}}

///////////////////////////////////////////////////////////////////////////////
// Tree traversal.
void octree_server::apply(
    hpx::util::function<void(octree_server&)> const& f
    )
{ // {{{
    std::vector<hpx::future<void> > recursion_is_parallelism;
    
    recursion_is_parallelism.reserve(8);
    
    for (boost::uint64_t i = 0; i < 8; ++i)
        if (hpx::invalid_id != children_[i])
            recursion_is_parallelism.push_back(children_[i].apply_async(f)); 
    
    // Invoke the kernel on ourselves ...
    f(*this);

    // ... and block while our children compute.
    hpx::wait(recursion_is_parallelism); 
} // }}}

///////////////////////////////////////////////////////////////////////////////
void octree_server::step_recurse(double dt)
{ // {{{
    std::vector<hpx::future<void> > recursion_is_parallelism;
    recursion_is_parallelism.reserve(8);

    {
        OCTOPUS_ASSERT_MSG(0 < dt, "invalid timestep size");
    
        // Start recursively executing the kernel function on our children.
        for (boost::uint64_t i = 0; i < 8; ++i)
            if (hpx::invalid_id != children_[i])
                recursion_is_parallelism.push_back
                    (hpx::async<step_recurse_action>
                        (children_[i].get_gid(), dt)); 

        // Kernel.
        step_kernel(dt);
    }

    // Block while our children compute.
    hpx::wait(recursion_is_parallelism); 
} // }}}

void octree_server::step_kernel(double dt)
{ // {{{
    U0_.reset(new vector3d<std::vector<double> >(*U_));
    FO0_.reset(new std::vector<double>(*FO_));

    // We do TVD RK3.
    switch (config().runge_kutta_order)
    {
        case 1:
        {
            sub_step_kernel(0, dt, 1.0);
            break;
        }

        case 2:
        {
            sub_step_kernel(0, dt, 1.0);
            sub_step_kernel(1, dt, 0.5);
            break; 
        }

        case 3:
        {
            sub_step_kernel(0, dt, 1.0);
            sub_step_kernel(1, dt, 0.25);
            sub_step_kernel(2, dt, 2.0 / 3.0);
            break; 
        }

        default:
        {
            OCTOPUS_ASSERT_FMT_MSG(false,
                "runge-kutta order (%1%) is unsupported or invalid",
                config().runge_kutta_order);
        }
    };

    communicate_ghost_zones(config().runge_kutta_order/*, l*/);

    ++step_;
    time_ += dt;
} // }}}

// Two communication phases.
void octree_server::sub_step_kernel(
    boost::uint64_t phase
  , double dt
  , double beta
    )
{ // {{{
    communicate_ghost_zones(phase);

    prepare_differentials_kernel();

    // Operations parallelizes by axis.
    compute_flux_kernel();
    adjust_flux_kernel();

    sum_differentials_kernel();
    add_differentials_kernel(dt, beta);

    child_to_parent_injection_kernel(phase + 1);
} // }}}

void octree_server::add_differentials_kernel(double dt, double beta)
{ // {{{
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    using namespace octopus::operators;

    for (boost::uint64_t i = bw; i < gnx - bw; ++i)
        for (boost::uint64_t j = bw; j < gnx - bw; ++j)
            for (boost::uint64_t k = bw; k < gnx - bw; ++k)
            {
                boost::array<double, 3> c = center_coords(i, j, k);

                D_(i, j, k) += science().source(*this, (*U_)(i, j, k), c);

                // Here, you can see the temporal dependency.
                (*U_)(i, j, k) = ((*U_)(i, j, k) + D_(i, j, k) * dt) * beta
                               + (*U0_)(i, j, k) * (1.0 - beta); 

                science().enforce_limits((*U_)(i, j, k), c);
            }

    (*FO_) = ((*FO_) + DFO_ * dt) * beta + (*FO0_) * (1.0 - beta);
} // }}}

// REVIEW: Make this run only when debugging is enabled.
void octree_server::prepare_differentials_kernel() 
{ // {{{
    boost::uint64_t const ss = science().state_size;
    boost::uint64_t const gnx = config().grid_node_length;

    OCTOPUS_ASSERT(DFO_.size() == ss);

    for (boost::uint64_t i = 0; i < ss; ++i)
        DFO_[i] = 0.0;

    OCTOPUS_ASSERT(D_.size() == (gnx * gnx * gnx));

    for (boost::uint64_t i = 0; i < gnx; ++i)
        for (boost::uint64_t j = 0; j < gnx; ++j)
            for (boost::uint64_t k = 0; k < gnx; ++k)
                for (boost::uint64_t l = 0; l < ss; ++l)
                {
                    OCTOPUS_ASSERT(D_(i, j, k).size() == ss);
                    D_(i, j, k)[l] = 0.0;
                }
} // }}}

void octree_server::compute_flux_kernel()
{ // {{{ 
    ////////////////////////////////////////////////////////////////////////////    
    // Compute our own local fluxes locally in parallel. 

    // Do two in other threads.
    boost::array<hpx::future<void>, 2> xyz =
    { {
        hpx::async(boost::bind
            (&octree_server::compute_x_flux_kernel, this/*, boost::ref(l)*/))
      , hpx::async(boost::bind
            (&octree_server::compute_y_flux_kernel, this/*, boost::ref(l)*/))
    } };

    // And do one here.
    compute_z_flux_kernel(/*l*/);

    // Wait for the local x and y fluxes to be computed.
    xyz[0].move();
    xyz[1].move();
} // }}}

void octree_server::compute_x_flux_kernel()
{ // {{{ 
    boost::uint64_t const ss = science().state_size;
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    std::vector<std::vector<double> > q0(gnx, std::vector<double>(ss));
    std::vector<std::vector<double> > ql(gnx, std::vector<double>(ss));
    std::vector<std::vector<double> > qr(gnx, std::vector<double>(ss));

    for (boost::uint64_t k = bw; k < (gnx - bw); ++k)
        for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
        {
            for (boost::uint64_t i = 0; i < gnx; ++i)
            {
                q0[i] = (*U_)(i, j, k);
    
                boost::array<double, 3> coords = center_coords(i, j, k);
    
                science().conserved_to_primitive(q0[i], coords);
            }
    
            science().reconstruct(q0, ql, qr);
    
            for (boost::uint64_t i = bw; i < gnx - bw + 1; ++i)
            {
                boost::array<double, 3> coords = x_face_coords(i, j, k);
    
                science().primitive_to_conserved(ql[i], coords);
                science().primitive_to_conserved(qr[i], coords);
    
                double const a = (std::max)
                    (science().max_eigenvalue(*this, x_axis, ql[i], coords)
                   , science().max_eigenvalue(*this, x_axis, qr[i], coords));
    
                std::vector<double>
                    ql_flux = science().flux(*this, x_axis, ql[i], coords),
                    qr_flux = science().flux(*this, x_axis, qr[i], coords);
    
                using namespace octopus::operators;
     
                FX_(i, j, k) = ((ql_flux + qr_flux)
                             - (qr[i] - ql[i]) * a) * 0.5;
            }
        }
} // }}}

void octree_server::compute_y_flux_kernel()
{ // {{{ 
    boost::uint64_t const ss = science().state_size;
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    std::vector<std::vector<double> > q0(gnx, std::vector<double>(ss));
    std::vector<std::vector<double> > ql(gnx, std::vector<double>(ss));
    std::vector<std::vector<double> > qr(gnx, std::vector<double>(ss));

    for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
        for (boost::uint64_t k = bw; k < (gnx - bw); ++k)
        {
            for (boost::uint64_t j = 0; j < gnx; ++j)
            {
                q0[j] = (*U_)(i, j, k);
    
                boost::array<double, 3> coords = center_coords(i, j, k);
    
                science().conserved_to_primitive(q0[j], coords);
            }
    
            science().reconstruct(q0, ql, qr);
    
            for (boost::uint64_t j = bw; j < gnx - bw + 1; ++j)
            {
                boost::array<double, 3> coords = y_face_coords(i, j, k);
    
                science().primitive_to_conserved(ql[j], coords);
                science().primitive_to_conserved(qr[j], coords);
    
                double const a = (std::max)
                    (science().max_eigenvalue(*this, y_axis, ql[j], coords)
                   , science().max_eigenvalue(*this, y_axis, qr[j], coords));
    
                std::vector<double>
                    ql_flux = science().flux(*this, y_axis, ql[j], coords)
                  , qr_flux = science().flux(*this, y_axis, qr[j], coords);
     
                using namespace octopus::operators;
    
                FY_(i, j, k) = ((ql_flux + qr_flux)
                             - (qr[j] - ql[j]) * a) * 0.5;
            }
        }
} // }}}

void octree_server::compute_z_flux_kernel()
{ // {{{ 
    boost::uint64_t const ss = science().state_size;
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    std::vector<std::vector<double> > q0(gnx, std::vector<double>(ss));
    std::vector<std::vector<double> > ql(gnx, std::vector<double>(ss));
    std::vector<std::vector<double> > qr(gnx, std::vector<double>(ss));

    for (boost::uint64_t i = bw; i < (gnx - bw); ++i)
        for (boost::uint64_t j = bw; j < (gnx - bw); ++j)
        {
            for (boost::uint64_t k = 0; k < gnx; ++k)
            {
                q0[k] = (*U_)(i, j, k);
    
                boost::array<double, 3> coords = center_coords(i, j, k);

                science().conserved_to_primitive(q0[k], coords);
            }
    
            science().reconstruct(q0, ql, qr);
    
            for (boost::uint64_t k = bw; k < gnx - bw + 1; ++k)
            {
                boost::array<double, 3> coords = z_face_coords(i, j, k);
    
                science().primitive_to_conserved(ql[k], coords);
                science().primitive_to_conserved(qr[k], coords);
    
                double const a = (std::max)
                    (science().max_eigenvalue(*this, z_axis, ql[k], coords)
                   , science().max_eigenvalue(*this, z_axis, qr[k], coords));
    
                std::vector<double>
                    ql_flux = science().flux(*this, z_axis, ql[k], coords)
                  , qr_flux = science().flux(*this, z_axis, qr[k], coords)
                    ;
     
                using namespace octopus::operators;
    
                FZ_(i, j, k) = ((ql_flux + qr_flux)
                             - (qr[k] - ql[k]) * a) * 0.5;
            }
        }
} // }}}

void octree_server::adjust_flux_kernel()
{ // {{{ IMPLEMENT

} // }}}

void octree_server::sum_differentials_kernel()
{ // {{{ 
    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    double const dxinv = 1.0 / dx_;

    ///////////////////////////////////////////////////////////////////////////
    // Kernel.
    using namespace octopus::operators;

    // NOTE: This is probably too tight a loop to parallelize with HPX, but
    // could be vectorized. 
    for (boost::uint64_t i = bw; i < gnx - bw; ++i)
        for (boost::uint64_t j = bw; j < gnx - bw; ++j)
        {
            for (boost::uint64_t k = bw; k < gnx - bw; ++k)
            {
                D_(i, j, k) -= (FX_(i + 1, j, k) - FX_(i, j, k)) * dxinv;
                D_(i, j, k) -= (FY_(i, j + 1, k) - FY_(i, j, k)) * dxinv;
                D_(i, j, k) -= (FZ_(i, j, k + 1) - FZ_(i, j, k)) * dxinv;
            }

            // i = y-axis, j = z-axis 
            DFO_ += (FX_(gnx - bw, i, j) - FX_(bw, i, j)) * dx_ * dx_;

            // i = x-axis, j = z-axis 
            DFO_ += (FY_(i, gnx - bw, j) - FY_(i, bw, j)) * dx_ * dx_;
        
            // i = x-axis, j = y-axis 
            if (config().reflect_on_z)
                DFO_ += (FZ_(i, j, gnx - bw)) * dx_ * dx_;
            else
                DFO_ += (FZ_(i, j, gnx - bw) - FZ_(i, j, bw)) * dx_ * dx_;
        }
} // }}}

void octree_server::copy_and_regrid()
{ // {{{ IMPLEMENT
    return;
} // }}}

void octree_server::mark()
{ // {{{
    if ((level_ + 1) == config().levels_of_refinement)
        return;

    std::vector<hpx::future<void> > recursion_is_parallelism;
    recursion_is_parallelism.reserve(8); 

    mark_kernel();

    sibling_refinement_signal(0);

    for (std::size_t i = 0; i < 8; ++i)
        if (  (hpx::invalid_id != children_[i])
           && (level_ + 2) != config().levels_of_refinement)
            recursion_is_parallelism.push_back(children_[i].mark_async());

    hpx::wait(recursion_is_parallelism);

    sibling_refinement_signal(1);
} // }}}

void octree_server::mark_kernel()
{ // {{{ 
    OCTOPUS_ASSERT((level_ + 1) != config().levels_of_refinement);

    std::vector<hpx::future<void> > markings;
    markings.reserve(8*3);

    mutex_type::scoped_lock l(mtx_);

    for (boost::uint64_t i = 0; i < 8; ++i)
    {
        child_index kid(i);

        OCTOPUS_ASSERT(children_[i].real()
                    || (  children_[i].kind() == invalid_boundary
                       && children_[i] == hpx::invalid_id));

        if (  !children_[i].real()
           && science().refine_policy.refine(*this, kid))
        {
            using namespace octopus::operators;

            boost::array<boost::uint64_t, 3> kid_location;
            kid_location = location_ * 2 + kid.array();

            OCTOPUS_ASSERT(children_[i] == hpx::invalid_id);

            marked_for_refinement_.set(kid, true);

            relatives r(kid);

            OCTOPUS_ASSERT
                (invalid_boundary != siblings_[r.exterior_x_face].kind());
            OCTOPUS_ASSERT
                (invalid_boundary != siblings_[r.exterior_y_face].kind());
            OCTOPUS_ASSERT
                (invalid_boundary != siblings_[r.exterior_z_face].kind());

            if (amr_boundary == siblings_[r.exterior_x_face].kind())
            {
                boost::array<boost::uint64_t, 3> dep_location;
                dep_location = siblings_[r.exterior_x_face].get_location() * 2
                    + invert(r.exterior_x_face, get_child_index()).array();
                markings.push_back
                    (siblings_[r.exterior_x_face].require_child_async
                        (invert(r.exterior_x_face, get_child_index())));
            }

            if (amr_boundary == siblings_[r.exterior_y_face].kind())
            {
                boost::array<boost::uint64_t, 3> dep_location;
                dep_location = siblings_[r.exterior_y_face].get_location() * 2
                    + invert(r.exterior_y_face, get_child_index()).array();
                markings.push_back
                    (siblings_[r.exterior_y_face].require_child_async
                        (invert(r.exterior_y_face, get_child_index())));
            }

            if (amr_boundary == siblings_[r.exterior_z_face].kind())
            {
                boost::array<boost::uint64_t, 3> dep_location;
                dep_location = siblings_[r.exterior_z_face].get_location() * 2
                    + invert(r.exterior_z_face, get_child_index()).array();
                markings.push_back
                    (siblings_[r.exterior_z_face].require_child_async
                        (invert(r.exterior_z_face, get_child_index())));
            }
        }
    }

    {
        hpx::util::unlock_the_lock<mutex_type::scoped_lock> ul(l);
        hpx::wait(markings); 
    }
} // }}}

void octree_server::propagate_locked(
    child_index kid
  , mutex_type::scoped_lock& l
    )
{ // {{{ 
    OCTOPUS_ASSERT((level_ + 1) != config().levels_of_refinement);
    OCTOPUS_ASSERT(marked_for_refinement_.test(kid));
    OCTOPUS_ASSERT(children_[kid] == hpx::invalid_id);

    std::vector<hpx::future<void> > markings;
    markings.reserve(3);

    using namespace octopus::operators;

    boost::array<boost::uint64_t, 3> kid_location;
    kid_location = location_ * 2 + kid.array();

    relatives r(kid);

    if (amr_boundary == siblings_[r.exterior_x_face].kind())
    {
        boost::array<boost::uint64_t, 3> dep_location;
        dep_location = siblings_[r.exterior_x_face].get_location() * 2
            + invert(r.exterior_x_face, get_child_index()).array();
        markings.push_back
            (siblings_[r.exterior_x_face].require_child_async
                (invert(r.exterior_x_face, get_child_index())));
    }

    if (amr_boundary == siblings_[r.exterior_y_face].kind())
    {
        boost::array<boost::uint64_t, 3> dep_location;
        dep_location = siblings_[r.exterior_y_face].get_location() * 2
            + invert(r.exterior_y_face, get_child_index()).array();
        markings.push_back
            (siblings_[r.exterior_y_face].require_child_async
                (invert(r.exterior_y_face, get_child_index())));
    }

    if (amr_boundary == siblings_[r.exterior_z_face].kind())
    {
        boost::array<boost::uint64_t, 3> dep_location;
        dep_location = siblings_[r.exterior_z_face].get_location() * 2
            + invert(r.exterior_z_face, get_child_index()).array();
        markings.push_back
            (siblings_[r.exterior_z_face].require_child_async
                (invert(r.exterior_z_face, get_child_index())));
    }

    {
        hpx::util::unlock_the_lock<mutex_type::scoped_lock> ul(l);
        hpx::wait(markings); 
    }
} // }}}

void octree_server::populate()
{ // {{{
    if ((level_ + 1) == config().levels_of_refinement)
        return;

    std::vector<hpx::future<void> > recursion_is_parallelism;
    recursion_is_parallelism.reserve(8); 

    populate_kernel();

    sibling_refinement_signal(2);

    for (std::size_t i = 0; i < 8; ++i)
        if (  (hpx::invalid_id != children_[i])
           && (level_ + 2) != config().levels_of_refinement)
            recursion_is_parallelism.push_back
                (children_[i].populate_async());

    hpx::wait(recursion_is_parallelism);

    sibling_refinement_signal(3);
} // }}}

void octree_server::populate_kernel()
{ // {{{ 
    OCTOPUS_ASSERT((level_ + 1) != config().levels_of_refinement);

    std::vector<hpx::future<void> > new_children;
    new_children.reserve(8);

    for (boost::uint64_t i = 0; i < 8; ++i)
    {
        child_index const kid(i);

        if (marked_for_refinement_.test(kid))
        {
            OCTOPUS_ASSERT(hpx::invalid_id == children_[i]);
            // REVIEW: This is a weird way to do this.
            new_children.push_back(hpx::async(boost::bind
                (&octree_server::create_child, this, kid)));
        }
    }

    hpx::wait(new_children); 
} // }}}

void octree_server::link()
{ // {{{
    if ((level_ + 1) == config().levels_of_refinement)
        return;

    std::vector<hpx::future<void> > recursion_is_parallelism;
    recursion_is_parallelism.reserve(8); 

    link_kernel();

    for (std::size_t i = 0; i < 8; ++i)
        if (  (hpx::invalid_id != children_[i])
           && (level_ + 2) != config().levels_of_refinement)
            recursion_is_parallelism.push_back
                (children_[i].link_async());

    hpx::wait(recursion_is_parallelism);

    sibling_refinement_signal(4);
} // }}}

void octree_server::link_kernel()
{ // {{{ 
    OCTOPUS_ASSERT((level_ + 1) != config().levels_of_refinement);

    std::vector<hpx::future<void> > links;
    links.reserve(8*6);

    {
        mutex_type::scoped_lock l(mtx_);

        for (boost::uint64_t i = 0; i < 8; ++i)
        {
            child_index const kid(i);

            if (hpx::invalid_id != children_[i])
                link_child(links, kid);
        }
    }

    hpx::wait(links); 
} // }}}

void octree_server::link_child(
    std::vector<hpx::future<void> >& links
  , child_index kid
    )
{ // {{{
    relatives r(kid);

    octree_init_data kid_init;

    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    using namespace octopus::operators;

    kid_init.parent   = reference_from_this(); 
    kid_init.level    = level_ + 1; 
    kid_init.location = location_ * 2 + kid.array(); 
    kid_init.dx       = dx_ * 0.5;
    kid_init.time     = time_;
    kid_init.offset   = offset_ * 2 + bw + (kid.array() * (gnx - 2 * bw));
    kid_init.origin   = origin_;
    kid_init.step     = step_;

    OCTOPUS_ASSERT(children_[kid] != hpx::invalid_id);

    octree_client& kid_client = children_[kid];

    ///////////////////////////////////////////////////////////////////////////
    // Create the interior "family" links.

    // Check if the interior X sibling of the new child exists.
    if (children_[r.x_sib] != hpx::invalid_id)
        links.push_back(children_[r.x_sib].tie_sibling_async
            (r.exterior_x_face, kid_client));

    else if (!marked_for_refinement_.test(r.x_sib))
    {
        OCTOPUS_ASSERT(children_[r.x_sib].kind() != physical_boundary);
        octree_client bound(amr_boundary
                          , client_from_this()
                          , r.exterior_x_face
                          , kid
                          , kid_init.offset
                          , offset_
                            ); 

        nephews_.insert(
            interpolation_data(kid_client, r.exterior_x_face, bound.offset_));

        links.push_back
            (kid_client.set_sibling_async(r.interior_x_face, bound));
    }

    // Check if the interior Y sibling of the new child exists.
    if (children_[r.y_sib] != hpx::invalid_id)
        links.push_back(children_[r.y_sib].tie_sibling_async
            (r.exterior_y_face, kid_client));

    else if (!marked_for_refinement_.test(r.y_sib))
    {
        OCTOPUS_ASSERT(children_[r.y_sib].kind() != physical_boundary);
        octree_client bound(amr_boundary
                          , client_from_this()
                          , r.exterior_y_face
                          , kid
                          , kid_init.offset
                          , offset_
                            ); 

        nephews_.insert(
            interpolation_data(kid_client, r.exterior_y_face, bound.offset_));

        links.push_back
            (kid_client.set_sibling_async(r.interior_y_face, bound));
    }

    // Check if the interior Z sibling of the new child exists.
    if (children_[r.z_sib] != hpx::invalid_id)
        links.push_back(children_[r.z_sib].tie_sibling_async
            (r.exterior_z_face, kid_client));

    else if (!marked_for_refinement_.test(r.z_sib))
    {
        OCTOPUS_ASSERT(children_[r.z_sib].kind() != physical_boundary);
        octree_client bound(amr_boundary
                          , client_from_this()
                          , r.exterior_z_face
                          , kid
                          , kid_init.offset
                          , offset_
                            ); 

        nephews_.insert(
            interpolation_data(kid_client, r.exterior_z_face, bound.offset_));

        links.push_back
            (kid_client.set_sibling_async(r.interior_z_face, bound));
    }

    ///////////////////////////////////////////////////////////////////////////
    // Create the exterior "family" links.

    // These links must exist. They may be non-real (e.g. boundaries), but they
    // must exist.

    // Check if the exterior X uncle (get it? :D) of the new child is real.
    if (real_boundary == siblings_[r.exterior_x_face].kind())
        links.push_back
            (siblings_[r.exterior_x_face].tie_child_sibling_async
                (r.x_sib, r.interior_x_face, kid_client));

    else if (physical_boundary == siblings_[r.exterior_x_face].kind())
    {
        octree_client bound(physical_boundary
                          , kid_client
                          , r.exterior_x_face
                            );
        links.push_back
            (kid_client.set_sibling_async(r.exterior_x_face, bound));
    }

    // Check if the exterior Y uncle (get it? :D) of the new child is real.
    if (real_boundary == siblings_[r.exterior_y_face].kind())
        links.push_back
            (siblings_[r.exterior_y_face].tie_child_sibling_async
                (r.y_sib, r.interior_y_face, kid_client));

    else if (physical_boundary == siblings_[r.exterior_y_face].kind())
    {
        octree_client bound(physical_boundary
                          , kid_client
                          , r.exterior_y_face
                            );
        links.push_back
            (kid_client.set_sibling_async(r.exterior_y_face, bound));
    }

    // Check if the exterior Z uncle (get it? :D) of the new child is real.
    if (real_boundary == siblings_[r.exterior_z_face].kind())
        links.push_back
            (siblings_[r.exterior_z_face].tie_child_sibling_async
                (r.z_sib, r.interior_z_face, kid_client));

    else if (physical_boundary == siblings_[r.exterior_z_face].kind())
    {
        octree_client bound(physical_boundary
                          , kid_client
                          , r.exterior_z_face
                            );
        links.push_back
            (kid_client.set_sibling_async(r.exterior_z_face, bound));
    }
} // }}}

void octree_server::refine()
{ // {{{
    OCTOPUS_ASSERT(0 == level_);

    clear_refinement_marks();

    mark();

    populate();

    link();
} // }}}

void octree_server::sibling_refinement_signal(
    boost::uint64_t phase
    )
{ // {{{
    std::vector<hpx::future<void> > keep_alive;
    keep_alive.reserve(6);

    std::vector<hpx::future<void> > dependencies;
    dependencies.reserve(6);

    {
        mutex_type::scoped_lock l(mtx_);

        for (boost::uint64_t i = 0; i < 6; ++i)
        {
            if (siblings_[i].real())
            {
                keep_alive.push_back(
                    siblings_[i].receive_sibling_refinement_signal_async
                        (phase, invert(face(i))));

                dependencies.push_back
                    (refinement_deps_.at(phase)(i).get_future());
            }
        }
    }

    for (boost::uint64_t i = 0; i < dependencies.size(); ++i)
        dependencies[i].move();

    hpx::wait(keep_alive);
} // }}}

}

