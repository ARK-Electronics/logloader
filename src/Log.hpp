#pragma once

#include <iostream>
#include <sstream>

#define LOG(x) std::cout << x << std::endl

#ifdef DEBUG_BUILD
#define LOG_DEBUG(x) std::cout << "[DEBUG] " << x << std::endl
#else
#define LOG_DEBUG(x) do {} while(0)
#endif
