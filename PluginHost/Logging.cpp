#include "Logging.h"

namespace lldb::imgui {

std::mutex g_logMutex;
std::deque<std::string> g_log;

}
