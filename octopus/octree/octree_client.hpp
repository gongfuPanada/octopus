////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2012 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#if !defined(OCTOPUS_17096997_37B3_4F9E_80F3_4C964006BFAA)
#define OCTOPUS_17096997_37B3_4F9E_80F3_4C964006BFAA

#include <hpx/runtime/naming/name.hpp>
#include <hpx/lcos/future.hpp>

#include <octopus/octree/octree_init_data.hpp>
#include <octopus/child_index.hpp>
#include <octopus/face.hpp>
#include <octopus/axis.hpp>

#include <boost/serialization/access.hpp>
#include <boost/serialization/array.hpp>

namespace octopus
{

struct OCTOPUS_EXPORT octree_server;

/// The set of types in our type-punning system. We call these types "kinds",
/// to distinguish them from C++ types.
// REVIEW: Should this have a serialization version?
enum boundary_kind
{
    invalid_boundary
  , real_boundary
  , physical_boundary
  , amr_boundary 
};

inline std::ostream& operator<<(std::ostream& os, boundary_kind k)
{
    switch (k)
    {
        case real_boundary:     os << "real_boundary"; break; 
        case physical_boundary: os << "physical_boundary"; break; 
        case amr_boundary:      os << "amr_boundary"; break; 
        default: os << "invalid_boundary"; break; 
    }
    return os;
}

}

///////////////////////////////////////////////////////////////////////////////
namespace boost { namespace serialization
{
    template <typename Archive>
    void save(Archive& ar, octopus::boundary_kind const& k, const unsigned int)
    {
        boost::uint8_t tmp(k);
        ar & tmp; 
    }

    template <typename Archive>
    void load(Archive& ar, octopus::boundary_kind& k, const unsigned int)
    {
        boost::uint8_t tmp;
        ar & tmp; 
        OCTOPUS_ASSERT_FMT_MSG(octopus::invalid_boundary > tmp,
                               "invalid face deserialized, face(%1%)",
                               boost::uint16_t(tmp));
        k = tmp; 
    }
}}

BOOST_SERIALIZATION_SPLIT_FREE(octopus::boundary_kind);

namespace octopus
{

// FIXME: Avoid using 'mutable'. 
// NOTE: This class is NOT thread safe when it is a physical or AMR boundary.
struct OCTOPUS_EXPORT octree_client
{
  private:
    boundary_kind kind_;

    mutable hpx::id_type gid_;

    // FIXME: This is only used by non-real boundaries, optimize.
    mutable face face_;

    // FIXME: This is only used for AMR boundaries, optimize.
    mutable boost::array<boost::int64_t, 3> offset_; ///< Relative offset.
    
    BOOST_COPYABLE_AND_MOVABLE(octree_client);

    friend struct octree_server;

    friend class boost::serialization::access;

    template <typename Archive>
    void serialize(Archive& ar, const unsigned int version)
    {
        ar & gid_;
        ar & kind_;
        if (!real())
        {
            ar & face_;
            ar & offset_;
        }
    }

    /// \brief Create a client for a real grid node.
    ///
    /// \note Internal use only; GIDs are not exposed to the user.
    octree_client(hpx::id_type const& gid)
      : kind_(real_boundary)
      , gid_(gid)
      , face_()
      , offset_()
    {}

    /// \brief Create a client for a real grid node.
    ///
    /// \note Internal use only; GIDs are not exposed to the user.
    octree_client(BOOST_RV_REF(hpx::id_type) gid)
      : kind_(real_boundary)
      , gid_(gid)
      , face_()
      , offset_()
    {}

    /// \brief Assign the GID of a real grid node to this client. 
    ///
    /// \note Internal use only; GIDs are not exposed to the user.
    octree_client& operator=(hpx::id_type const& gid)
    {
        gid_ = gid;
        kind_ = real_boundary;
        face_ = face();
        offset_[0] = 0;
        offset_[1] = 0;
        offset_[2] = 0;
        return *this;
    }

    /// \brief Assign the GID of a real grid node to this client. 
    ///
    /// \note Internal use only; GIDs are not exposed to the user.
    octree_client& operator=(BOOST_RV_REF(hpx::id_type) gid)
    {
        kind_ = real_boundary;
        gid_ = gid;
        face_ = face();
        offset_[0] = 0;
        offset_[1] = 0;
        offset_[2] = 0;
        return *this;
    }

    // AMR boundary.
    octree_client(
        boundary_kind kind
      , octree_client const& parent 
      , face f
      , boost::array<boost::int64_t, 3> const& sib_offset
      , boost::array<boost::int64_t, 3> const& parent_offset
        );

    // Physical boundary.
    octree_client( 
        boundary_kind kind
      , octree_client const& sib 
      , face f
        )
      : kind_(physical_boundary)
      , gid_(sib.gid_)
      , face_(f)
      , offset_()
    {
        OCTOPUS_ASSERT(physical_boundary == kind);
    }

    /// \brief Get the GID this client is holding. 
    ///
    /// \note Internal use only; GIDs are not exposed to the user.
    hpx::id_type const& get_gid() const
    {
        return gid_;
    }

    void ensure_real() const
    {
        OCTOPUS_ASSERT_FMT_MSG(kind_ == real_boundary,
            "illegal operation for %1% client, expected real boundary",
            kind_);
    }

  public:
    octree_client()
      : kind_(invalid_boundary)
      , gid_(hpx::naming::invalid_id)
      , face_()
      , offset_()
    {}

    octree_client(octree_client const& other)
      : kind_(other.kind_)
      , gid_(other.gid_)
      , face_(other.face_)
      , offset_(other.offset_)
    {}

    octree_client(BOOST_RV_REF(octree_client) other)
      : kind_(other.kind_)
      , gid_(other.gid_)
      , face_(other.face_)
      , offset_(other.offset_)
    {}

/* Not sure I need these.
    octree_client(octree_client const& parent, boundary_kind kind)
      : gid_(parent.gid_)
      , kind_(kind)
      , face_()
      , offset_()
    {
        OCTOPUS_ASSERT(kind != real_boundary);
    }

    octree_client(BOOST_RV_REF(octree_client) parent, boundary_kind kind)
      : gid_(parent.gid_)
      , kind_(kind)
      , face_()
      , offset_()
    {
        OCTOPUS_ASSERT(kind != real_boundary);
    }
*/

    octree_client& operator=(BOOST_COPY_ASSIGN_REF(octree_client) other)
    {
        kind_ = other.kind_;        
        gid_ = other.gid_;
        face_ = other.face_;
        offset_ = other.offset_;
        return *this;
    }

    octree_client& operator=(BOOST_RV_REF(octree_client) other)
    {
        kind_ = other.kind_;        
        gid_ = other.gid_;
        face_ = other.face_;
        offset_ = other.offset_;
        return *this;
    }

    operator hpx::util::safe_bool<octree_client>::result_type() const
    {
        return hpx::util::safe_bool<octree_client>()(gid_);
    }

    friend bool operator==(octree_client const& lhs, octree_client const& rhs) 
    {
        return lhs.gid_ == rhs.gid_
            && lhs.kind_ == rhs.kind_; 
    }

    friend bool operator==(octree_client const& lhs, hpx::id_type const& rhs) 
    {
        return lhs.gid_ == rhs;
    }

    friend bool operator==(hpx::id_type const& lhs, octree_client const& rhs) 
    {
        return lhs == rhs.gid_;
    }

    friend bool operator!=(octree_client const& lhs, octree_client const& rhs) 
    {
        return !(lhs == rhs);
    }

    friend bool operator!=(octree_client const& lhs, hpx::id_type const& rhs) 
    {
        return !(lhs == rhs);
    }

    friend bool operator!=(hpx::id_type const& lhs, octree_client const& rhs) 
    {
        return !(lhs == rhs);
    }
    
    ///////////////////////////////////////////////////////////////////////////
    /// \brief Returns true if this client represents a real octree (as opposed
    ///        an AMR or physical boundary).
    bool real() const 
    {
        switch (kind_)
        {
            case real_boundary:
                return true;
            case physical_boundary:
            case amr_boundary:
                return false; 
            default:
                break;
        };

        OCTOPUS_ASSERT_MSG(false, "invalid boundary kind");
        return false;
    }

    /// \brief Return the boundary_kind of this client.
    boundary_kind kind() const
    {
        return kind_;
    }

    ///////////////////////////////////////////////////////////////////////////
    // {{{ create_root FIXME: semantics/syntax are confusing wrt create_child
    void create_root(
        hpx::id_type const& locality
      , octree_init_data const& init   
        );

    void create_root(
        hpx::id_type const& locality
      , BOOST_RV_REF(octree_init_data) init   
        );
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ create_child
    void create_child(
        child_index kid
        ) const
    {
        create_child_async(kid).get(); 
    }

    hpx::future<void> create_child_async(
        child_index kid
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ set_sibling
    void set_sibling(
        face f
      , octree_client const& sib 
        ) const;

    void set_sibling_push(
        face f
      , octree_client const& sib 
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ tie_sibling
    void tie_sibling(
        face f
      , octree_client const& sib 
        ) const;

    void tie_sibling_push(
        face f
      , octree_client const& sib 
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ set_child_sibling
    void set_child_sibling(
        child_index kid
      , face f
      , octree_client const& sib 
        ) const;

    void set_child_sibling_push(
        child_index kid
      , face f
      , octree_client const& sib 
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ tie_child_sibling
    void tie_child_sibling(
        child_index kid
      , face f
      , octree_client const& sib 
        ) const;

    void tie_child_sibling_push(
        child_index kid
      , face f
      , octree_client const& sib 
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ get_siblings
    boost::array<octree_client, 6> get_siblings() const
    {
        return get_siblings_async().get();
    }

    hpx::future<boost::array<octree_client, 6> > get_siblings_async() const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ get_offset
    boost::array<boost::int64_t, 3> get_offset() const
    {
        return get_offset_async().get();
    }

    hpx::future<boost::array<boost::int64_t, 3> > get_offset_async() const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ Boundary code. 
  private:
    // AMR boundary.
    vector3d<std::vector<double> > interpolate(
        face f
        ) const;

    // AMR boundary.
    hpx::future<vector3d<std::vector<double> > > interpolate_async(
        face f
        ) const;

    // Physical boundary. 
    vector3d<std::vector<double> > map(
        face f
        ) const;

    // Physical boundary. 
    hpx::future<vector3d<std::vector<double> > > map_async(
        face f
        ) const;
    // }}}

  public:
    ///////////////////////////////////////////////////////////////////////////
    // {{{ send_ghost_zone
    vector3d<std::vector<double> > send_ghost_zone(
        face f
        ) const;

    hpx::future<vector3d<std::vector<double> > > send_ghost_zone_async(
        face f
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ receive_ghost_zone
    void receive_ghost_zone(
        boost::uint64_t step ///< For debugging purposes.
      , boost::uint64_t phase 
      , face f ///< Relative to caller.
      , BOOST_RV_REF(vector3d<std::vector<double> >) zone
        ) const
    {
        receive_ghost_zone_async(step, phase, f, boost::move(zone)).get();
    }

    hpx::future<void> receive_ghost_zone_async(
        boost::uint64_t step ///< For debugging purposes.
      , boost::uint64_t phase 
      , face f ///< Relative to caller.
      , BOOST_RV_REF(vector3d<std::vector<double> >) zone
        ) const;

    void receive_ghost_zone_push(
        boost::uint64_t step ///< For debugging purposes.
      , boost::uint64_t phase 
      , face f ///< Relative to caller.
      , BOOST_RV_REF(vector3d<std::vector<double> >) zone
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ receive_child_state
    void receive_child_state(
        boost::uint64_t step ///< For debugging purposes.
      , boost::uint64_t phase 
      , child_index idx 
      , BOOST_RV_REF(vector3d<std::vector<double> >) zone
        ) const
    {
        receive_child_state_async(step, phase, idx, boost::move(zone)).get();
    }

    hpx::future<void> receive_child_state_async(
        boost::uint64_t step ///< For debugging purposes.
      , boost::uint64_t phase 
      , child_index idx 
      , BOOST_RV_REF(vector3d<std::vector<double> >) zone
        ) const;

    void receive_child_state_push(
        boost::uint64_t step ///< For debugging purposes.
      , boost::uint64_t phase 
      , child_index idx 
      , BOOST_RV_REF(vector3d<std::vector<double> >) zone
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ step 
    void step() const
    {
        return step_async().get();
    }

    hpx::future<void> step_async() const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ copy_and_regrid (IMPLEMENT) and refine
    void refine() const
    {
        return refine_async().get();
    }

    hpx::future<void> refine_async() const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ output 
    void output() const
    {
        return output_async().get();
    }

    hpx::future<void> output_async() const;

    void output_initial() const
    {
        return output_initial_async().get();
    }

    hpx::future<void> output_initial_async() const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ apply 
    void apply(
        hpx::util::function<void(octree_server&)> const& f
        ) const
    {
        return apply_async(f).get();
    }

    hpx::future<void> apply_async(
        hpx::util::function<void(octree_server&)> const& f
        ) const;
    // }}}

    ///////////////////////////////////////////////////////////////////////////
    // {{{ apply_leaf - definitions are out-of-line in octree_apply_leaf.hpp
    template <typename T>
    T apply_leaf(
        hpx::util::function<T(octree_server&)> const& f
        ) const;

    template <typename T>
    hpx::future<T> apply_leaf_async(
        hpx::util::function<T(octree_server&)> const& f
        ) const;
    // }}} 

    ///////////////////////////////////////////////////////////////////////////
    // {{{ reduce - definitions are out-of-line in octree_reduce.hpp.
    template <typename T>
    T reduce(
        hpx::util::function<T(octree_server&)> const& f
      , hpx::util::function<T(T const&, T const&)> const& reducer
        ) const;

    template <typename T>
    hpx::future<T> reduce_async(
        hpx::util::function<T(octree_server&)> const& f
      , hpx::util::function<T(T const&, T const&)> const& reducer
        ) const;

    template <typename T>
    T reduce_zonal(
        hpx::util::function<T(std::vector<double>&)> const& f
      , hpx::util::function<T(T const&, T const&)> const& reducer
      , T const& initial = T()
        ) const;

    template <typename T>
    hpx::future<T> reduce_zonal_async(
        hpx::util::function<T(std::vector<double>&)> const& f
      , hpx::util::function<T(T const&, T const&)> const& reducer
      , T const& initial = T()
        ) const;
    // }}}

    // NOTE: (to self) Keep the order the same as octree_server please.
    // (update to self) Bad self.
};

}

#endif // OCTOPUS_17096997_37B3_4F9E_80F3_4C964006BFAA

