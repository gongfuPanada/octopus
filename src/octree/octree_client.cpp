////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2012 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#include <hpx/lcos/future.hpp>
#include <hpx/lcos/future_wait.hpp>
#include <hpx/lcos/local/packaged_continuation.hpp>
#include <hpx/async.hpp>
#include <hpx/apply.hpp>
#include <hpx/runtime/components/runtime_support.hpp>

#include <octopus/octree/octree_server.hpp>
#include <octopus/octree/octree_apply_leaf.hpp>
#include <octopus/engine/engine_interface.hpp>
#include <octopus/operators/std_vector_arithmetic.hpp>
#include <octopus/operators/boost_array_arithmetic.hpp>
#include <octopus/trivial_serialization.hpp>
#include <octopus/math.hpp>

namespace octopus
{

// FIXME: sib_offset is implicit knowledge (from index and source_offset).
octree_client::octree_client(
    boundary_kind kind
  , octree_client const& source 
  , face f ///< Relative to caller.
  , child_index index
  , boost::array<boost::int64_t, 3> sib_offset
  , boost::array<boost::int64_t, 3> source_offset
    )
  : kind_(amr_boundary)
  , gid_(source.gid_)
  , face_(f)
  , index_(index)
  , offset_() 
{ // {{{
    OCTOPUS_ASSERT(amr_boundary == kind);

    boost::array<boost::int64_t, 3> v;
    v[0] = 0;
    v[1] = 0;
    v[2] = 0;

    switch (f)
    {
        case XU:
            v[0] = -1;
            break;
        case XL:
            v[0] = 1;
            break;
        case YU:
            v[1] = -1;
            break;
        case YL:
            v[1] = 1;
            break;
        case ZU:
            v[2] = -1;
            break;
        case ZL:
            v[2] = 1;
            break;
        default:
            OCTOPUS_ASSERT(false);
            break;
    }

    boost::uint64_t const bw = science().ghost_zone_width;
    boost::uint64_t const gnx = config().grid_node_length;

    using namespace octopus::operators;

    v *= (gnx - 2 * bw);

    offset_ = sib_offset;
    offset_ += v;
    offset_ -= source_offset * 2;
} // }}}

///////////////////////////////////////////////////////////////////////////////
void octree_client::create_root(
    hpx::id_type const& locality
  , octree_init_data const& init
    ) 
{
    kind_ = real_boundary;

    OCTOPUS_ASSERT_FMT_MSG(!(locality.get_msb() & 0xFF),
                           "target is not a locality, gid(%1%)",
                           locality);

    hpx::components::runtime_support rts(locality);
    gid_ = rts.create_component_async<octopus::octree_server>(init).get();
}

void octree_client::create_root(
    hpx::id_type const& locality
  , BOOST_RV_REF(octree_init_data) init
    ) 
{
    kind_ = real_boundary;

    OCTOPUS_ASSERT_FMT_MSG(!(locality.get_msb() & 0xFF),
                           "target is not a locality, gid(%1%)",
                           locality);

    hpx::components::runtime_support rts(locality);
    gid_ = rts.create_component_async<octopus::octree_server>(init).get();
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::create_child_async(
    child_index kid
    ) const
{
    ensure_real();
    return hpx::async<octree_server::create_child_action>(gid_, kid);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::require_child_async(
    child_index kid
    ) const
{
    return hpx::async<octree_server::require_child_action>(gid_, kid);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::set_sibling_async(
    face f ///< Relative to us.
  , octree_client const& sib
    ) const
{
    ensure_real();
    OCTOPUS_ASSERT_FMT_MSG(invalid_face > f,
                           "invalid face, face(%1%)",
                           boost::uint16_t(f));
    return hpx::async<octree_server::set_sibling_action>(gid_, f, sib);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::tie_sibling_async(
    face target_f ///< Relative to \a target_sib.
  , octree_client const& target_sib
    ) const
{
    ensure_real();
    OCTOPUS_ASSERT_FMT_MSG(invalid_face > target_f,
                           "invalid face, face(%1%)",
                           boost::uint16_t(target_f));
    return hpx::async<octree_server::tie_sibling_action>
        (gid_, target_f, target_sib);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::set_child_sibling_async(
    child_index kid
  , face f ///< Relative to \a sib
  , octree_client const& sib
    ) const
{
    ensure_real();
    OCTOPUS_ASSERT_FMT_MSG(invalid_face > f,
                           "invalid face, face(%1%)",
                           boost::uint16_t(f));
    return hpx::async<octree_server::set_child_sibling_action>
        (gid_, kid, f, sib);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::tie_child_sibling_async(
    child_index target_kid
  , face target_f ///< Relative to \a target_sib 
  , octree_client const& target_sib
    ) const
{
    ensure_real();
    OCTOPUS_ASSERT_FMT_MSG(invalid_face > target_f,
                           "invalid face, face(%1%)",
                           boost::uint16_t(target_f));
    return hpx::async<octree_server::tie_child_sibling_action>
        (gid_, target_kid, target_f, target_sib);
}
    
///////////////////////////////////////////////////////////////////////////////
hpx::future<boost::array<octree_client, 6> >
octree_client::get_siblings_async() const
{
    ensure_real();
    return hpx::async<octree_server::get_siblings_action>(gid_);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<boost::array<boost::int64_t, 3> >
octree_client::get_offset_async() const
{
    ensure_real();
    return hpx::async<octree_server::get_offset_action>(gid_);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<boost::array<boost::int64_t, 3> >
octree_client::get_location_async() const
{
    ensure_real();
    return hpx::async<octree_server::get_location_action>(gid_);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<vector3d<std::vector<double> > >
octree_client::send_interpolated_ghost_zone_async(
    face f ///< Direction, relative to us 
    ) const
{
/*
    OCTOPUS_ASSERT_FMT_MSG(f == invert(face_) 
                         , "supplied face (%1%) is not inverse of the "
                           "stored face (%2%)"
                         , f % invert(face_)); 
*/
    return hpx::async<octree_server::send_interpolated_ghost_zone_action>
        (gid_, f, offset_);
}

hpx::future<vector3d<std::vector<double> > >
octree_client::send_mapped_ghost_zone_async(
    face f ///< Direction, relative to us 
    ) const
{
/*
    OCTOPUS_ASSERT_FMT_MSG(f == invert(face_) 
                         , "supplied face (%1%) is not inverse of the "
                           "stored face (%2%)"
                         , f % invert(face_)); 
*/
    return hpx::async<octree_server::send_mapped_ghost_zone_action>
        (gid_, face_);
}

vector3d<std::vector<double> > octree_client::send_ghost_zone(
    face f ///< Direction, relative to us 
    ) const
{
    switch (kind_)
    {
        case real_boundary:
            return send_ghost_zone_async(f).get(); 
        case amr_boundary:
            return send_interpolated_ghost_zone(f);
        case physical_boundary:
            return send_mapped_ghost_zone(f); 
        default:
            break;
    }

    OCTOPUS_ASSERT(false);
    return vector3d<std::vector<double> >();
}

hpx::future<vector3d<std::vector<double> > > 
octree_client::send_ghost_zone_async(
    face f ///< Direction, relative to us. 
    ) const
{
    switch (kind_)
    {
        case real_boundary:
            return hpx::async<octree_server::send_ghost_zone_action>(gid_, f);
        case amr_boundary:
            return send_interpolated_ghost_zone_async(f);
        case physical_boundary:
            return send_mapped_ghost_zone_async(f); 
        default:
            break;
    }

    OCTOPUS_ASSERT(false);
    return hpx::future<vector3d<std::vector<double> > >();
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::receive_ghost_zone_async(
    boost::uint64_t step ///< For debugging purposes.
  , boost::uint64_t phase 
  , face f ///< Relative to caller.
  , BOOST_RV_REF(vector3d<std::vector<double> >) zone
    ) const
{
    ensure_real();
    return hpx::async<octree_server::receive_ghost_zone_action>
        (gid_, step, phase, f, boost::move(zone));
}

void octree_client::receive_ghost_zone_push(
    boost::uint64_t step ///< For debugging purposes.
  , boost::uint64_t phase 
  , face f ///< Relative to caller.
  , BOOST_RV_REF(vector3d<std::vector<double> >) zone
    ) const
{
    ensure_real();
    hpx::apply<octree_server::receive_ghost_zone_action>
        (gid_, step, phase, f, boost::move(zone));
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::child_to_parent_injection_async(
    boost::uint64_t phase 
    ) const
{
    ensure_real();
    return hpx::async<octree_server::child_to_parent_injection_action>
        (gid_, phase);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::receive_child_state_async(
    boost::uint64_t step ///< For debugging purposes.
  , boost::uint64_t phase 
  , child_index idx 
  , BOOST_RV_REF(vector3d<std::vector<double> >) zone
    ) const
{
    ensure_real();
    return hpx::async<octree_server::receive_child_state_action>
        (gid_, step, phase, idx, boost::move(zone));
}

void octree_client::receive_child_state_push(
    boost::uint64_t step ///< For debugging purposes.
  , boost::uint64_t phase 
  , child_index idx 
  , BOOST_RV_REF(vector3d<std::vector<double> >) zone
    ) const
{
    ensure_real();
    hpx::apply<octree_server::receive_child_state_action>
        (gid_, step, phase, idx, boost::move(zone));
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::apply_async(
    hpx::util::function<void(octree_server&)> const& f
    ) const
{
    ensure_real();
    return hpx::async<octree_server::apply_action>(gid_, f);
}

///////////////////////////////////////////////////////////////////////////////
hpx::future<void> octree_client::step_async() const
{
    ensure_real();
    return hpx::async<octree_server::step_action>(gid_);
}

///////////////////////////////////////////////////////////////////////////////
struct begin_io_epoch_locally
{
    typedef void result_type;

    std::string file_;

    begin_io_epoch_locally() : file_() {}

    begin_io_epoch_locally(std::string const& file) : file_(file) {}

    result_type operator()(octree_server& root) const
    {
        if (file_.empty())
            science().output.begin_epoch(root);
        else
            science().output.begin_epoch(root, file_);
    }

    template <typename Archive>
    void serialize(Archive& ar, unsigned int)
    {
        ar & file_;
    }
};

struct end_io_epoch_locally : trivial_serialization
{
    typedef void result_type;

    result_type operator()(octree_server& root) const
    {
        science().output.end_epoch(root);
    }
};

struct output_locally : trivial_serialization
{
    typedef void result_type;

    result_type operator()(octree_server& e) const
    {
        science().output(e);
    }
};

struct output_continuation
{
    typedef void result_type;

    octree_client const& self_;

    // FIXME: Workaround for a bug with future lifetimes in HPX.
    hpx::future<void> f_;

    output_continuation(
        octree_client const& self
      , hpx::future<void> const& f
        )
      : self_(self)
      , f_(f)
    {}

    // future continuation overload
    result_type operator()(hpx::future<void> res) const
    {
        self_.apply(output_locally());
    }
};
    
struct end_io_epoch_continuation : trivial_serialization
{
    typedef void result_type;

    octree_client const& self_;

    // FIXME: Workaround for a bug with future lifetimes in HPX.
    hpx::future<void> f_;

    end_io_epoch_continuation(
        octree_client const& self
      , hpx::future<void> const& f
        )
      : self_(self)
      , f_(f)
    {}

    // future continuation overload
    result_type operator()(hpx::future<void> res) const
    {
        // Send ourselves to the target.
        self_.apply_leaf<void>(end_io_epoch_locally());
    }
};

// TODO: Make sure we are only called on the root node.
hpx::future<void> octree_client::output_async() const
{
    ensure_real();

    begin_io_epoch_locally begin_functor;

    hpx::future<void> begin = apply_leaf_async<void>(begin_functor);
    hpx::future<void> out   = begin.then(output_continuation(*this, begin));
    hpx::future<void> end   = out.then(end_io_epoch_continuation(*this, out));
 
    return end;
}

// TODO: Make sure we are only called on the root node.
hpx::future<void> octree_client::output_async(std::string const& file) const
{
    ensure_real();

    begin_io_epoch_locally begin_functor(file);

    hpx::future<void> begin = apply_leaf_async<void>(begin_functor);
    hpx::future<void> out   = begin.then(output_continuation(*this, begin));
    hpx::future<void> end   = out.then(end_io_epoch_continuation(*this, out));
 
    return end;
}


hpx::future<void> octree_client::refine_async() const
{
    ensure_real();
    return hpx::async<octree_server::refine_action>(gid_);
}

hpx::future<void> octree_client::mark_async() const
{
    ensure_real();
    return hpx::async<octree_server::mark_action>(gid_);
}

void octree_client::receive_sibling_refinement_signal_push(face f) const
{
    ensure_real();
    hpx::apply<octree_server::receive_sibling_refinement_signal_action>(gid_, f); 
}

}

