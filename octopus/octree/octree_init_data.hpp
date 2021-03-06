////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2012 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#if !defined(OCTOPUS_E73B96D9_CF42_4331_A4D5_2BE254FD289C)
#define OCTOPUS_E73B96D9_CF42_4331_A4D5_2BE254FD289C

#include <octopus/vector4d.hpp>
#include <octopus/array.hpp>

#include <boost/cstdint.hpp>
#include <boost/serialization/array.hpp>

namespace octopus
{

// FIXME: Tactic needed for constructing the init data for root.
// (update): Tactic - split this into two classes, child_init_data and
// root_init_data.
struct octree_init_data
{
    octree_init_data()
      : parent()
      , level(0)
      , dx(0)
      , time(0)
      , step(0)
    {
        for (boost::uint64_t i = 0; i < 3; ++i)
        {
            location[i] = 0;
            offset[i] = 0;
            origin[i] = 0.0;
        }
    }

    octree_init_data(octree_init_data const& other)
      : parent(other.parent)
      , level(other.level)
      , location(other.location)
      , dx(other.dx)
      , time(other.time)
      , offset(other.offset)
      , origin(other.origin)
      , step(other.step)
    {}

    hpx::id_type              parent;
    boost::uint64_t           level;
    array<boost::uint64_t, 3> location;
    double                    dx;
    double                    time; 
    array<boost::int64_t, 3>  offset; 
    array<double, 3>          origin;
    boost::uint64_t           step;

    template <typename Archive>
    void serialize(Archive& ar, const unsigned int version)
    {
        ar & parent;
        ar & level;
        ar & location; 
        ar & dx;
        ar & time;
        ar & offset; 
        ar & origin; 
        ar & step;
    }
};

}

#endif // OCTOPUS_E73B96D9_CF42_4331_A4D5_2BE254FD289C

