////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2012 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#include <octopus/science/science_table.hpp>
#include <octopus/science/physical_boundaries.hpp>
#include <octopus/science/reconstruction.hpp>
#include <octopus/science/state.hpp>

namespace octopus
{

science_table default_science_table()
{
    science_table sci;

    sci.physical_boundaries = physical_boundaries_at_zero();

    sci.reconstruction = minmod_reconstruction(); 
    sci.ghost_zone_width = minmod_reconstruction::ghost_zone_width;

    return sci;
}

}

