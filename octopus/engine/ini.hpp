////////////////////////////////////////////////////////////////////////////////
//  Copyright (c) 2012 Bryce Adelstein-Lelbach
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
////////////////////////////////////////////////////////////////////////////////

#if !defined(OCTOPUS_4B4DC4CC_782C_4A95_BF22_C7D1251BFC64)
#define OCTOPUS_4B4DC4CC_782C_4A95_BF22_C7D1251BFC64

#include <hpx/hpx_fwd.hpp>
#include <hpx/runtime.hpp>
#include <hpx/exception.hpp>

#include <boost/lexical_cast.hpp>

namespace octopus
{

inline bool has_config_entry(std::string const& key)
{                                                                            
    if (NULL == hpx::get_runtime_ptr())                                           
        return false;                                                           
    return hpx::get_runtime().get_config().has_entry(key);                  
}  

struct config_reader
{
    typedef config_reader const& result_type;

    template <typename A, typename B>
    result_type operator()(std::string const& param, A& data, B dflt) const
    {
        std::string key("octopus.");
        key += param;

        try
        {
            if (has_config_entry(key))
            {
                data = boost::lexical_cast<A>(hpx::get_config_entry(key, ""));
            }
            else
            {
                data = dflt;
            }
    
        }
    
        catch (boost::bad_lexical_cast&)
        {
            // REVIEW: This is literally the only place where we throw directly
            // instead of asserting.
            std::string msg = boost::str(boost::format(
                "bad INI parameter, '%1%' is not a valid value for %2%")
                % hpx::get_config_entry(key, "") % key);
            HPX_THROW_EXCEPTION(hpx::bad_parameter,
                "octopus::config_from_ini", msg);
        }

        return *this;
    }

    template <typename A>
    result_type operator()(std::string const& param, A& data) const
    {
        std::string key("octopus.");
        key += param;

        try
        {
            if (has_config_entry(key))
            {
                data = boost::lexical_cast<A>(hpx::get_config_entry(key, ""));
            }
    
        }
    
        catch (boost::bad_lexical_cast&)
        {
            // REVIEW: This is literally the only place where we throw directly
            // instead of asserting.
            std::string msg = boost::str(boost::format(
                "bad INI parameter, '%1%' is not a valid value for %2%")
                % hpx::get_config_entry(key, "") % key);
            HPX_THROW_EXCEPTION(hpx::bad_parameter,
                "octopus::config_from_ini", msg);
        }

        return *this;
    }
};

}

#endif // OCTOPUS_4B4DC4CC_782C_4A95_BF22_C7D1251BFC64

