#pragma once
#include <iostream>
#ifdef RACKMON_SYSLOG
#include <openbmc/syslog.hpp>
[[maybe_unused]] static thread_local std::ostream& logError =
    openbmc::syslog_error;
[[maybe_unused]] static thread_local std::ostream& logEarn =
    openbmc::syslog_warn;
[[maybe_unused]] static thread_local std::ostream& logInfo =
    openbmc::syslog_notice;
#else
[[maybe_unused]] static thread_local std::ostream& logError = std::cerr;
[[maybe_unused]] static thread_local std::ostream& logInfo = std::cout;
[[maybe_unused]] static thread_local std::ostream& logEarn = std::cout;
#endif

#ifdef PROFILING
#include <openbmc/profile.hpp>
#define RACKMON_PROFILE_SCOPE(name, desc, os) openbmc::Profile name(desc, os)
#else
#define RACKMON_PROFILE_SCOPE(name, desc, os)
#endif
