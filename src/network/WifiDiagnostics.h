#pragma once
#include <string>

#include "WifiFailure.h"
namespace wifi_connection {
void startDiagnostics(const std::string& ssid);
// Exactly one bounded log line per attempt; caller supplies nonsecret outcome.
void finishDiagnostics(const char* outcome);
Failure lastFailure();
}  // namespace wifi_connection
