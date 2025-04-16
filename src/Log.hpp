#pragma once

#include <iostream>
#include <sstream>

// Regular logs - always shown
#define LOG(x) std::cout << x << std::endl

// Debug logs - only shown in debug builds
#ifdef DEBUG_BUILD
#define LOG_DEBUG(x) std::cout << "[DEBUG] " << x << std::endl
#else
#define LOG_DEBUG(x) do {} while(0)
#endif
