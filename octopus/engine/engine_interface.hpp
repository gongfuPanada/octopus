////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2012 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#if !defined(OCTOPUS_E40BC60F_0909_4486_8387_6435DB403689)
#define OCTOPUS_E40BC60F_0909_4486_8387_6435DB403689

#include <octopus/assert.hpp>
#include <octopus/engine/engine_server.hpp>

namespace octopus
{

/// \brief Retrieve the runtime configuration data. 
///
/// Remote Operations:   No.
/// Concurrency Control: None (read by multiple threads; data is read-only). 
/// Synchrony Gurantee:  Synchronous.
inline config_data const& config()
{
    OCTOPUS_ASSERT_MSG(engine_ptr == 0, "engine_ptr is NULL");
    return engine_ptr->config();    
}

/// \brief Asynchronously create a new octree node using the distributed
///        load-balancer.
///
/// Remote Operations:   Possibly.
/// Concurrency Control: 1 atomic read and 1 atomic write to \a engine_server's
///                      round_robin_.
/// Synchrony Gurantee:  Asynchronous.
inline hpx::future<hpx::id_type, hpx::naming::gid_type> create_octree_async(
    boost::uint64_t level
  , array1d<boost::uint64_t, 3> const& location
    )
{
    OCTOPUS_ASSERT_MSG(engine_ptr == 0, "engine_ptr is NULL");
    return engine_ptr->create_octree_async(level, location);
}

/// \brief Asynchronously create a new octree node using the distributed
///        load-balancer.
///
/// Remote Operations:   Possibly.
/// Concurrency Control: 1 atomic read and 1 atomic write to \a engine_server's
///                      round_robin_.
/// Synchrony Gurantee:  Synchronous.
inline hpx::id_type create_octree(
    boost::uint64_t level
  , array1d<boost::uint64_t, 3> const& location
    )
{
    return create_octree_async(level, location).get();
} 

}

#endif // OCTOPUS_E40BC60F_0909_4486_8387_6435DB403689
