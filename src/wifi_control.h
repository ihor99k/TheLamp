#pragma once

class Lamp;

// Wi-Fi: joins the saved home network; without one (or when it's unreachable) opens the
// "TheLamp-Setup" access point with a setup page. Serves the control page either way.
namespace wifi {

void begin(Lamp& lamp);
void update();  // call from loop()

// Serial commands: "wifi" prints status, "wifi forget" drops the saved network.
void handleCommand(const char* line);

}  // namespace wifi
