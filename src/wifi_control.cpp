#include "wifi_control.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "config.h"
#include "lamp.h"
#include "web_pages.h"  // generated from web/*.html by scripts/embed_web.py

namespace {

constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;   // one connection attempt
constexpr uint32_t OFFLINE_SETUP_MS   = 60000;   // network gone this long -> open setup AP
constexpr uint32_t SETUP_RETRY_MS     = 120000;  // in setup mode, retry the saved network this often
constexpr uint32_t AP_LINGER_MS       = 180000;  // after success keep the AP so the phone sees the address

enum class Mode { Connecting, Online, Setup };

// Exposes whether the listening socket really exists: begin() fails silently.
class LampServer : public WebServer {
 public:
  LampServer() : WebServer(80) {}
  bool listening() { return static_cast<bool>(_server); }
};

Lamp* lamp = nullptr;
LampServer server;
DNSServer dns;
uint32_t lastBeginTry = 0;
Preferences prefs;

Mode mode = Mode::Setup;
uint32_t modeSince = 0;

String ssid, pass;        // saved home network
String trySsid, tryPass;  // network entered on the setup page, saved only once it works
bool trying = false;
bool connectQueued = false;
String lastError;

// Optional fixed address on the home network. Gateway, mask and DNS are copied from the
// router's lease when the address is fixed, so the user only picks the IP.
struct FixedIp {
  bool on = false;
  IPAddress ip, gateway, mask, dns;
};
FixedIp fixed;
bool reconnectQueued = false;

bool apOn = false;
bool apClosing = false;
uint32_t apCloseAt = 0;

const char* modeName() {
  switch (mode) {
    case Mode::Connecting: return "connecting";
    case Mode::Online: return "online";
    default: return "setup";
  }
}

void setMode(Mode m) {
  mode = m;
  modeSince = millis();
}

// ---------- radio ----------

void startAp() {
  if (apOn) return;
  WiFi.mode(WIFI_AP_STA);
  const IPAddress ip(4, 3, 2, 1);  // same setup address WLED uses
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  WiFi.softAP(WIFI_SETUP_NAME, WIFI_SETUP_PASSWORD[0] ? WIFI_SETUP_PASSWORD : nullptr);
  dns.start(53, "*", WiFi.softAPIP());  // every name -> us, so phones show the setup page
  apOn = true;
  apClosing = false;
  Serial.printf("WiFi: setup network \"%s\", open http://%s\n", WIFI_SETUP_NAME,
                WiFi.softAPIP().toString().c_str());
}

void stopAp() {
  if (!apOn) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  apClosing = false;
  Serial.println("WiFi: setup network closed");
}

// A network typed on the setup page is always tried with DHCP: a fixed address from
// another network would not fit it.
void connect(const String& s, const String& p, bool useFixed) {
  WiFi.setAutoReconnect(true);
  WiFi.disconnect();
  if (useFixed && fixed.on) WiFi.config(fixed.ip, fixed.gateway, fixed.mask, fixed.dns);
  else WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);  // DHCP
  WiFi.begin(s.c_str(), p.c_str());
  setMode(Mode::Connecting);
  Serial.printf("WiFi: connecting to \"%s\"\n", s.c_str());
}

void enterSetup() {
  startAp();
  // Background reconnects hop channels and keep dropping phones from the setup AP.
  WiFi.setAutoReconnect(false);
  WiFi.disconnect();
  setMode(Mode::Setup);
}

void loadFixed() {
  fixed.on = prefs.getBool("fixed", false);
  fixed.ip = IPAddress(prefs.getUInt("ip", 0));
  fixed.gateway = IPAddress(prefs.getUInt("gw", 0));
  fixed.mask = IPAddress(prefs.getUInt("mask", 0));
  fixed.dns = IPAddress(prefs.getUInt("dns", 0));
}

void saveFixed() {
  prefs.putBool("fixed", fixed.on);
  prefs.putUInt("ip", (uint32_t)fixed.ip);
  prefs.putUInt("gw", (uint32_t)fixed.gateway);
  prefs.putUInt("mask", (uint32_t)fixed.mask);
  prefs.putUInt("dns", (uint32_t)fixed.dns);
}

// Switches between a fixed address and DHCP. Returns an error message, empty on success.
String setAddress(bool on, const String& ipText) {
  if (!on) {
    fixed.on = false;
  } else {
    if (mode != Mode::Online) return "Спершу лампа має бути підключена до домашньої мережі.";
    IPAddress ip;
    if (!ip.fromString(ipText)) return "Невірна адреса.";
    IPAddress gw = WiFi.gatewayIP(), mask = WiFi.subnetMask(), dns = WiFi.dnsIP();
    uint32_t a = ip, g = gw, m = mask;
    if ((a & m) != (g & m)) return "Адреса має бути з тієї ж мережі, що й роутер (" + gw.toString() + ").";
    if (a == g || (a & ~m) == 0 || (a | m) == 0xFFFFFFFF) return "Цю адресу використати не можна.";
    fixed = {true, ip, gw, mask, (uint32_t)dns ? dns : gw};
  }
  saveFixed();
  reconnectQueued = true;  // re-join with the new addressing from update()
  Serial.printf("WiFi: address %s\n", fixed.on ? fixed.ip.toString().c_str() : "from router (DHCP)");
  return "";
}

void onOnline() {
  if (trying) {
    ssid = trySsid;
    pass = tryPass;
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    trying = false;
    fixed.on = false;  // the old fixed address belonged to the previous network
    saveFixed();
  }
  lastError = "";
  setMode(Mode::Online);

  MDNS.end();
  if (MDNS.begin(WIFI_HOSTNAME)) MDNS.addService("http", "tcp", 80);
  if (apOn) {
    apClosing = true;
    apCloseAt = millis() + AP_LINGER_MS;
  }
  Serial.printf("WiFi: online, http://%s or http://%s.local\n", WiFi.localIP().toString().c_str(),
                WIFI_HOSTNAME);
}

// ---------- http ----------

String jsonStr(const String& s) {
  String out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if ((uint8_t)c < 0x20) continue;
    out += c;
  }
  return out + "\"";
}

void logRequest(int code) {
  if (!Serial) return;
  Serial.printf("HTTP %s %s -> %d (client %s)\n", server.method() == HTTP_GET ? "GET" : "POST",
                server.uri().c_str(), code, server.client().remoteIP().toString().c_str());
}

void sendJson(int code, const String& body) {
  logRequest(code);
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

void sendPage(const uint8_t* data, size_t len) {
  logRequest(200);
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", reinterpret_cast<const char*>(data), len);
}

bool viaSetupAp() {
  return apOn && server.client().localIP() == WiFi.softAPIP();
}

// Our pages add this header; a random website in the same browser can't send it
// without a CORS preflight we never answer.
bool fromOurPage() {
  if (server.header("X-Lamp-Request") == "1") return true;
  sendJson(403, "{\"error\":\"forbidden\"}");
  return false;
}

void handleWifiStatus() {
  String out = "{\"mode\":\"";
  out += modeName();
  out += "\",\"ssid\":" + jsonStr(trying ? trySsid : ssid);
  out += ",\"ip\":" + jsonStr(mode == Mode::Online ? WiFi.localIP().toString() : String());
  out += ",\"host\":" + jsonStr(String(WIFI_HOSTNAME) + ".local");
  out += ",\"fixed\":" + String(fixed.on ? "true" : "false");
  out += ",\"gateway\":" + jsonStr(mode == Mode::Online ? WiFi.gatewayIP().toString() : String());
  out += ",\"error\":" + jsonStr(lastError) + "}";
  sendJson(200, out);
}

void handleAddress() {
  if (!fromOurPage()) return;
  bool on = server.arg("mode") == "fixed";
  String err = setAddress(on, server.arg("ip"));
  if (!err.isEmpty()) {
    sendJson(400, "{\"error\":" + jsonStr(err) + "}");
    return;
  }
  // The page follows the lamp to its new address; with DHCP the router decides.
  sendJson(200, "{\"ok\":true,\"url\":" + jsonStr(on ? "http://" + fixed.ip.toString() + "/setup" : String()) + "}");
}

void handleScan() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {  // no scan yet: start one
    if (mode == Mode::Connecting || WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) {
      sendJson(503, "{\"error\":\"Лампа зараз зайнята, спробуй за кілька секунд.\"}");
      return;
    }
    n = WIFI_SCAN_RUNNING;
  }
  if (n == WIFI_SCAN_RUNNING) {
    sendJson(202, "{\"scanning\":true}");
    return;
  }
  String out = "{\"networks\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (name.isEmpty()) continue;  // hidden network
    if (!first) out += ',';
    first = false;
    out += "{\"ssid\":" + jsonStr(name) + ",\"rssi\":" + String(WiFi.RSSI(i)) +
           ",\"lock\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  out += "]}";
  WiFi.scanDelete();
  sendJson(200, out);
}

void handleWifiConnect() {
  if (!fromOurPage()) return;
  String s = server.arg("ssid"), p = server.arg("password");
  if (s.isEmpty() || s.length() > 32 || p.length() > 64 || (p.length() && p.length() < 8)) {
    sendJson(400, "{\"error\":\"Назва мережі — до 32 символів, пароль — від 8 до 64 (або порожній для відкритої мережі).\"}");
    return;
  }
  trySsid = s;
  tryPass = p;
  trying = true;
  lastError = "";
  connectQueued = true;  // switch networks from update(), after this reply has gone out
  sendJson(200, "{\"ok\":true}");
}

void handleCommand() {
  if (!fromOurPage()) return;
  String body = server.arg("plain");
  if (body.isEmpty() || body.length() > 150) {
    sendJson(400, "{\"error\":\"bad command\"}");
    return;
  }
  lamp->handleCommands(body.c_str());
  sendJson(200, lamp->stateJson());
}

void routes() {
  const char* headers[] = {"X-Lamp-Request"};
  server.collectHeaders(headers, 1);

  server.on("/", HTTP_GET, [] {
    if (viaSetupAp()) sendPage(SETUP_PAGE, sizeof(SETUP_PAGE));
    else sendPage(CONTROL_PAGE, sizeof(CONTROL_PAGE));
  });
  server.on("/setup", HTTP_GET, [] { sendPage(SETUP_PAGE, sizeof(SETUP_PAGE)); });
  server.on("/control", HTTP_GET, [] { sendPage(CONTROL_PAGE, sizeof(CONTROL_PAGE)); });

  server.on("/api/state", HTTP_GET, [] { sendJson(200, lamp->stateJson()); });
  server.on("/api/command", HTTP_POST, handleCommand);
  server.on("/api/wifi", HTTP_GET, handleWifiStatus);
  server.on("/api/wifi", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/address", HTTP_POST, handleAddress);
  server.on("/api/scan", HTTP_GET, handleScan);

  // Phones probe random URLs to detect a captive portal: send them to the setup page.
  server.onNotFound([] {
    if (viaSetupAp()) {
      logRequest(302);
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup");
      server.send(302, "text/plain", "");
    } else {
      logRequest(404);
      server.send(404, "text/plain", "Not found");
    }
  });
}

void ensureListening(uint32_t now) {
  if (server.listening() || (lastBeginTry && now - lastBeginTry < 2000)) return;
  lastBeginTry = now | 1;
  server.begin();
  Serial.printf("HTTP: port 80 %s\n", server.listening() ? "listening" : "FAILED, retrying");
}

void logEvents() {
  WiFi.onEvent([](arduino_event_id_t event, arduino_event_info_t info) {
    if (!Serial) return;
    switch (event) {
      case ARDUINO_EVENT_WIFI_AP_STACONNECTED: Serial.println("WiFi: a phone joined the setup network"); break;
      case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED: Serial.println("WiFi: a phone left the setup network"); break;
      case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
        Serial.printf("WiFi: gave the phone address %s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        Serial.printf("WiFi: home network disconnected, reason %u\n", info.wifi_sta_disconnected.reason);
        break;
      default: break;
    }
  });
}

void printStatus() {
  Serial.printf("WiFi: mode=%s saved=\"%s\" ip=%s (%s) rssi=%d setupAP=%s clients=%d http=%s\n",
                modeName(), ssid.c_str(), WiFi.localIP().toString().c_str(),
                fixed.on ? "fixed" : "dhcp", WiFi.RSSI(),
                apOn ? WiFi.softAPIP().toString().c_str() : "off", WiFi.softAPgetStationNum(),
                server.listening() ? "listening" : "NOT listening");
}

}  // namespace

namespace wifi {

void begin(Lamp& l) {
  lamp = &l;
  prefs.begin("wifi", false);
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  loadFixed();

  WiFi.persistent(false);  // credentials live in our own NVS keys, not the driver's
  WiFi.setHostname(WIFI_HOSTNAME);
  logEvents();
  routes();

  if (ssid.isEmpty()) {
    enterSetup();
  } else {
    WiFi.mode(WIFI_STA);
    connect(ssid, pass, true);
  }
}

void update() {
  uint32_t now = millis();
  if (apOn) dns.processNextRequest();
  ensureListening(now);
  server.handleClient();

  if (connectQueued) {
    connectQueued = false;
    WiFi.scanDelete();
    connect(trySsid, tryPass, false);
    return;
  }
  if (reconnectQueued) {
    reconnectQueued = false;
    connect(ssid, pass, true);
    return;
  }

  bool up = WiFi.status() == WL_CONNECTED;
  switch (mode) {
    case Mode::Connecting:
      if (up) {
        onOnline();
      } else if (now - modeSince >= CONNECT_TIMEOUT_MS) {
        Serial.println("WiFi: connection failed");
        if (trying) {
          lastError = "Не вдалося підключитися до «" + trySsid +
                      "». Перевір пароль і що це мережа 2,4 ГГц.";
          trying = false;
        }
        enterSetup();
      }
      break;

    case Mode::Online:
      if (up) {
        modeSince = now;  // counts time since the link was last seen
        if (apClosing && (int32_t)(now - apCloseAt) >= 0) stopAp();
      } else if (now - modeSince >= OFFLINE_SETUP_MS) {
        Serial.println("WiFi: network lost");
        enterSetup();
      }
      break;

    case Mode::Setup:
      if (!ssid.isEmpty() && WiFi.softAPgetStationNum() == 0 && now - modeSince >= SETUP_RETRY_MS) {
        connect(ssid, pass, true);
      }
      break;
  }
}

void handleCommand(const char* line) {
  if (!strcmp(line, "wifi forget")) {
    prefs.remove("ssid");
    prefs.remove("pass");
    ssid = pass = "";
    fixed.on = false;
    saveFixed();
    Serial.println("WiFi: saved network forgotten");
    enterSetup();
  } else if (!strncmp(line, "wifi ip ", 8)) {
    const char* arg = line + 8;
    String err = setAddress(strcmp(arg, "dhcp") != 0, arg);
    if (!err.isEmpty()) Serial.println("WiFi: " + err);
  }
  printStatus();
}

}  // namespace wifi
