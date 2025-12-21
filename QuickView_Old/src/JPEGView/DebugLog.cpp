#include "StdAfx.h"
#include <fstream>

void LogDebug(LPCTSTR message) {
    std::wofstream logFile("debug.log", std::ios_base::app);
    logFile << message << std::endl;
}
