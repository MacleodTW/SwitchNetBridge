#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <lwip/ip6_addr.h>

// Network and service objects
Preferences prefs;
WebServer webServer(80);
WiFiUDP dnsServer;
WiFiUDP dnsUpstream;

// Default custom DNS list (supports both IPv4 and IPv6)
// Format:
//   "domain"          -> resolves A to ESP32 AP IP (192.168.4.1)
//   "domain ipv4"     -> resolves A to custom IPv4
//   "domain ipv6"     -> resolves AAAA to custom IPv6
const char* DEFAULT_CUSTOM_DNS = 
R"(# Resolve to ESP32-S3 AP IP (192.168.4.1)
relay.local
esp32.local

# Nintendo connectivity-check hosts (handled by local HTTP server)
conntest.nintendowifi.net
ctest.cdn.nintendo.net
ctest.p01.ctest.srv.nintendo.net
ctest-dl-lp1.cdn.nintendo.net
ctest-ul-lp1.cdn.nintendo.net
api.p01.lp1.ctest.srv.nintendo.net
api-ipv6.p01.lp1.ctest.srv.nintendo.net

# Example: resolve to specific IPv4 and IPv6 addresses
# nas.local 192.168.1.100
# nas.local fd00::100
)";

// Default DNS allowlist (only matching domains are forwarded to upstream DNS)
const char* DEFAULT_ALLOWLIST = 
R"(# Add allowed domains here (one per line, # for comments)

# Microsoft connection test
msftconnecttest.com

# Github
github.com
)";

// Default configuration
String staSsid = "";
String staPass = "";
String apSsid = "SwitchNetBridge";
String apPass = "password";
String upstreamDnsStr = ""; // Blank = auto-obtain from upstream AP DHCP
String customDnsRaw = DEFAULT_CUSTOM_DNS;
String allowlistRaw = DEFAULT_ALLOWLIST;
bool showAllowDns = false;
bool showBlockDns = false;

IPAddress apIP(192, 168, 4, 1);
IPAddress apGateway(192, 168, 4, 1);
IPAddress apSubnet(255, 255, 255, 0);
IPAddress apDhcpStart(192, 168, 4, 2);
IPAddress upstreamDnsIP(0, 0, 0, 0);

bool naptEnabled = false;
uint8_t dnsBuf[512];

// Load settings from NVS
void loadConfig() {
    if (prefs.begin("relay_cfg", true)) {
        staSsid = prefs.getString("sta_ssid", "");
        staPass = prefs.getString("sta_pass", "");
        apSsid = prefs.getString("ap_ssid", "SwitchNetBridge");
        apPass = prefs.getString("ap_pass", "password");
        upstreamDnsStr = prefs.getString("up_dns", "");
        upstreamDnsStr.trim();
        customDnsRaw = prefs.getString("custom_dns", DEFAULT_CUSTOM_DNS);
        allowlistRaw = prefs.getString("allowlist", DEFAULT_ALLOWLIST);
        showAllowDns = prefs.getBool("show_allow_dns", false);
        showBlockDns = prefs.getBool("show_block_dns", false);
        prefs.end();
    } else {
        Serial.println("[CFG] No saved config found, using defaults.");
    }

    if (upstreamDnsStr.length() > 0 && upstreamDnsIP.fromString(upstreamDnsStr)) {
        Serial.printf("[DNS] Manual Upstream DNS: %s\n", upstreamDnsIP.toString().c_str());
    } else {
        upstreamDnsIP = IPAddress(0, 0, 0, 0);
    }
}

// Save settings to NVS
void saveConfig() {
    prefs.begin("relay_cfg", false);
    prefs.putString("sta_ssid", staSsid);
    prefs.putString("sta_pass", staPass);
    prefs.putString("ap_ssid", apSsid);
    prefs.putString("ap_pass", apPass);
    prefs.putString("up_dns", upstreamDnsStr);
    prefs.putString("custom_dns", customDnsRaw);
    prefs.putString("allowlist", allowlistRaw);
    prefs.putBool("show_allow_dns", showAllowDns);
    prefs.putBool("show_block_dns", showBlockDns);
    prefs.end();
}

// Extract queried domain name, QTYPE, and end offset of the Question section
String parseDnsQuery(const uint8_t* buf, int len, uint16_t& qType, int& qEndOffset) {
    qType = 0;
    qEndOffset = 0;
    if (len < 17) return "";

    String domain = "";
    int pos = 12; // DNS header is 12 bytes
    while (pos < len && buf[pos] != 0) {
        uint8_t labelLen = buf[pos];
        if (labelLen > 63 || pos + labelLen >= len) return "";
        if (domain.length() > 0) domain += ".";
        for (int i = 1; i <= labelLen; i++) {
            domain += (char)tolower(buf[pos + i]);
        }
        pos += labelLen + 1;
    }

    if (pos + 4 >= len) return "";
    pos++; // Skip null terminator of QNAME
    qType = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
    qEndOffset = pos + 4; // Skip QTYPE (2 bytes) and QCLASS (2 bytes)
    return domain;
}

// Search custom DNS list for IPv4 (qType=1) or IPv6 (qType=28) match
bool findCustomDnsRecord(const String& domain, uint16_t qType, uint8_t* outAddr, int& outAddrLen, bool& domainMatched) {
    outAddrLen = 0;
    domainMatched = false;
    if (domain.length() == 0 || customDnsRaw.length() == 0) return false;

    int start = 0;
    while (start < (int)customDnsRaw.length()) {
        int end = customDnsRaw.indexOf('\n', start);
        if (end == -1) end = customDnsRaw.length();

        String line = customDnsRaw.substring(start, end);
        line.trim();
        start = end + 1;

        if (line.length() == 0 || line.startsWith("#")) continue;

        int sep = line.indexOf(' ');
        if (sep == -1) sep = line.indexOf('\t');

        String ruleDomain;
        String ipStr;
        if (sep == -1) {
            ruleDomain = line;
            ipStr = "";
        } else {
            ruleDomain = line.substring(0, sep);
            ipStr = line.substring(sep + 1);
            ipStr.trim();
        }
        ruleDomain.toLowerCase();

        if (ruleDomain.length() > 0 && (domain == ruleDomain || domain.endsWith("." + ruleDomain))) {
            domainMatched = true;

            // No IP specified -> default to ESP32 AP IPv4
            if (ipStr.length() == 0) {
                if (qType == 1) {
                    for (int i = 0; i < 4; i++) outAddr[i] = apIP[i];
                    outAddrLen = 4;
                    return true;
                }
                continue;
            }

            // IPv6 address (contains ':')
            if (ipStr.indexOf(':') != -1) {
                if (qType == 28) {
                    ip6_addr_t ip6;
                    if (ip6addr_aton(ipStr.c_str(), &ip6)) {
                        memcpy(outAddr, ip6.addr, 16);
                        outAddrLen = 16;
                        return true;
                    }
                }
                continue;
            }

            // IPv4 address
            if (qType == 1) {
                IPAddress tmpIP;
                if (tmpIP.fromString(ipStr)) {
                    for (int i = 0; i < 4; i++) outAddr[i] = tmpIP[i];
                    outAddrLen = 4;
                    return true;
                }
            }
        }
    }
    return false;
}

// Check if domain matches any rule in the allowlist
bool isDomainAllowed(const String& domain) {
    if (domain.length() == 0 || allowlistRaw.length() == 0) return false;

    int start = 0;
    while (start < (int)allowlistRaw.length()) {
        int end = allowlistRaw.indexOf('\n', start);
        if (end == -1) end = allowlistRaw.length();

        String rule = allowlistRaw.substring(start, end);
        rule.trim();
        rule.toLowerCase();
        start = end + 1;

        if (rule.length() > 0 && !rule.startsWith("#")) {
            if (domain == rule || domain.indexOf(rule) >= 0) {
                return true;
            }
        }
    }

    return false;
}

// Send a synthetic DNS response for A (4 bytes), AAAA (16 bytes), or NODATA (0 bytes)
void sendCustomDnsResponse(IPAddress clientIP, uint16_t clientPort, uint16_t qType, int qEndOffset, const uint8_t* addr, int addrLen) {
    if (qEndOffset + 12 + addrLen > (int)sizeof(dnsBuf)) return;

    dnsBuf[2] = 0x81;  // QR=1, RD=1
    dnsBuf[3] = 0x80;  // RA=1, RCODE=0 (NOERROR)
    dnsBuf[8] = 0x00;  // NSCOUNT = 0
    dnsBuf[9] = 0x00;
    dnsBuf[10] = 0x00; // ARCOUNT = 0
    dnsBuf[11] = 0x00;

    int respLen = qEndOffset;

    if ((qType == 1 && addrLen == 4) || (qType == 28 && addrLen == 16)) {
        dnsBuf[6] = 0x00;
        dnsBuf[7] = 0x01; // ANCOUNT = 1

        // Answer section
        dnsBuf[respLen++] = 0xC0; // Name pointer to offset 12
        dnsBuf[respLen++] = 0x0C;
        dnsBuf[respLen++] = (qType >> 8) & 0xFF; // Type A (1) or AAAA (28)
        dnsBuf[respLen++] = qType & 0xFF;
        dnsBuf[respLen++] = 0x00; // Class IN
        dnsBuf[respLen++] = 0x01;
        dnsBuf[respLen++] = 0x00; // TTL: 60 seconds
        dnsBuf[respLen++] = 0x00;
        dnsBuf[respLen++] = 0x00;
        dnsBuf[respLen++] = 0x3C;
        dnsBuf[respLen++] = 0x00; // RDLENGTH: 4 or 16 bytes
        dnsBuf[respLen++] = (uint8_t)addrLen;
        memcpy(&dnsBuf[respLen], addr, addrLen);
        respLen += addrLen;
    } else {
        // Return NOERROR with 0 answers (NODATA)
        dnsBuf[6] = 0x00;
        dnsBuf[7] = 0x00; // ANCOUNT = 0
    }

    dnsServer.beginPacket(clientIP, clientPort);
    dnsServer.write(dnsBuf, respLen);
    dnsServer.endPacket();
}

// Send a clean NXDOMAIN response (strips EDNS0/additional sections)
void sendNxDomainResponse(IPAddress clientIP, uint16_t clientPort, int qEndOffset) {
    if (qEndOffset <= 0 || qEndOffset > (int)sizeof(dnsBuf)) return;

    dnsBuf[2] = 0x81;  // QR=1, RD=1
    dnsBuf[3] = 0x83;  // RA=1, RCODE=3 (NXDOMAIN)
    dnsBuf[6] = 0x00;  // ANCOUNT = 0
    dnsBuf[7] = 0x00;
    dnsBuf[8] = 0x00;  // NSCOUNT = 0
    dnsBuf[9] = 0x00;
    dnsBuf[10] = 0x00; // ARCOUNT = 0
    dnsBuf[11] = 0x00;

    dnsServer.beginPacket(clientIP, clientPort);
    dnsServer.write(dnsBuf, qEndOffset);
    dnsServer.endPacket();
}

// Skip a DNS name field (handles both standard labels and 0xC0 compression pointers)
int skipDnsName(const uint8_t* buf, int len, int pos) {
    while (pos < len) {
        uint8_t labelLen = buf[pos];
        if (labelLen == 0) {
            return pos + 1;
        }
        if ((labelLen & 0xC0) == 0xC0) {
            return (pos + 2 <= len) ? (pos + 2) : -1;
        }
        pos += labelLen + 1;
    }
    return -1;
}

// Parse upstream DNS response and print resolved IPv4/IPv6 addresses
void logUpstreamDnsResponse(const String& domain, uint16_t qType, const uint8_t* buf, int len) {
    if (len < 12) return;

    uint16_t qdCount = ((uint16_t)buf[4] << 8) | buf[5];
    uint16_t anCount = ((uint16_t)buf[6] << 8) | buf[7];
    int pos = 12;

    // Skip Question section
    for (uint16_t i = 0; i < qdCount; i++) {
        pos = skipDnsName(buf, len, pos);
        if (pos < 0 || pos + 4 > len) return;
        pos += 4; // QTYPE (2) + QCLASS (2)
    }

    // Parse Answer section
    String ipList = "";
    for (uint16_t i = 0; i < anCount; i++) {
        pos = skipDnsName(buf, len, pos);
        if (pos < 0 || pos + 10 > len) break;

        uint16_t rType = ((uint16_t)buf[pos] << 8) | buf[pos + 1];
        uint16_t rdLen = ((uint16_t)buf[pos + 8] << 8) | buf[pos + 9];
        pos += 10; // TYPE(2) + CLASS(2) + TTL(4) + RDLENGTH(2)

        if (pos + rdLen > len) break;

        if (rType == 1 && rdLen == 4) { // A (IPv4)
            IPAddress ip(buf[pos], buf[pos + 1], buf[pos + 2], buf[pos + 3]);
            if (ipList.length() > 0) ipList += ", ";
            ipList += ip.toString();
        } else if (rType == 28 && rdLen == 16) { // AAAA (IPv6)
            ip6_addr_t ip6;
            memcpy(ip6.addr, &buf[pos], 16);
            if (ipList.length() > 0) ipList += ", ";
            ipList += ip6addr_ntoa(&ip6);
        }
        pos += rdLen;
    }

    if (ipList.length() > 0) {
        Serial.printf("[DNS FWD] %s (Type %u) -> %s\n", domain.c_str(), qType, ipList.c_str());
    } else {
        Serial.printf("[DNS FWD] %s (Type %u) -> (no IP, ANCOUNT=%u)\n", domain.c_str(), qType, anCount);
    }
}

// Handle incoming DNS queries on UDP port 53
void handleDnsFilter() {
    int packetSize = dnsServer.parsePacket();
    if (packetSize <= 0 || packetSize > (int)sizeof(dnsBuf)) return;

    int len = dnsServer.read(dnsBuf, sizeof(dnsBuf));
    IPAddress clientIP = dnsServer.remoteIP();
    uint16_t clientPort = dnsServer.remotePort();

    uint16_t qType = 0;
    int qEndOffset = 0;
    String domain = parseDnsQuery(dnsBuf, len, qType, qEndOffset);
    if (domain.length() == 0) return;

    // 1. Check Custom DNS IP List (IPv4 & IPv6)
    uint8_t addrBuf[16];
    int addrLen = 0;
    bool domainMatched = false;
    findCustomDnsRecord(domain, qType, addrBuf, addrLen, domainMatched);
    if (domainMatched) {
        sendCustomDnsResponse(clientIP, clientPort, qType, qEndOffset, addrBuf, addrLen);
        Serial.printf("[DNS CUSTOM] %s (Type %u) -> %s\n",
                      domain.c_str(), qType, addrLen == 4 ? "IPv4" : (addrLen == 16 ? "IPv6" : "NODATA"));
        return;
    }

    // 2. Check Allowlist (block everything NOT in the allowlist)
    if (!isDomainAllowed(domain)) {

        if (qType == 1) {
            //const uint8_t loopbackV4[4] = {127, 0, 0, 1};
            //sendCustomDnsResponse(clientIP, clientPort, qType, qEndOffset, loopbackV4, 4);
            if (showBlockDns) {
                //Serial.printf("[DNS BLOCK] %s (Type 1) -> 127.0.0.1\n", domain.c_str());
                Serial.printf("[DNS BLOCK] %s (Type 1)\n", domain.c_str());
            }
        } else if (qType == 28) {
            //const uint8_t loopbackV6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            //sendCustomDnsResponse(clientIP, clientPort, qType, qEndOffset, loopbackV6, 16);
            if (showBlockDns) {
                Serial.printf("[DNS BLOCK] %s (Type 28) -> ::1\n", domain.c_str());
                Serial.printf("[DNS BLOCK] %s (Type 28)\n", domain.c_str());
            }
        } else {
            //sendCustomDnsResponse(clientIP, clientPort, qType, qEndOffset, nullptr, 0);
            if (showBlockDns) {
                Serial.printf("[DNS BLOCK] %s (Type %u) -> NODATA\n", domain.c_str(), qType);
            }
        }
        
        return;
    }

    // 3. Forward allowed query to upstream DNS if STA is connected
    if (WiFi.status() == WL_CONNECTED) {
        dnsUpstream.beginPacket(upstreamDnsIP, 53);
        dnsUpstream.write(dnsBuf, len);
        dnsUpstream.endPacket();

        unsigned long startWait = millis();
        while (millis() - startWait < 300) {
            int respSize = dnsUpstream.parsePacket();
            if (respSize > 0 && respSize <= (int)sizeof(dnsBuf)) {
                int respLen = dnsUpstream.read(dnsBuf, sizeof(dnsBuf));
                dnsServer.beginPacket(clientIP, clientPort);
                dnsServer.write(dnsBuf, respLen);
                dnsServer.endPacket();

                if (showAllowDns) {
                    logUpstreamDnsResponse(domain, qType, dnsBuf, respLen);
                }

                return;
            }
            delay(1);
        }
    }
}

// Wi-Fi event handler to obtain upstream DNS and enable NAPT once STA gets an IP
void onWiFiEvent(WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
        Serial.print("[STA] Connected! IP: ");
        Serial.println(WiFi.localIP());

        // Auto-obtain upstream DNS from AP DHCP if not manually overridden
        if (upstreamDnsStr.length() == 0) {
            IPAddress dhcpDns = WiFi.dnsIP(0);
            if (dhcpDns != IPAddress(0, 0, 0, 0)) {
                upstreamDnsIP = dhcpDns;
                Serial.printf("[DNS] Upstream DNS from AP DHCP: %s\n", upstreamDnsIP.toString().c_str());
            } else {
                upstreamDnsIP = IPAddress(0, 0, 0, 0);
                Serial.println("[DNS] No DHCP DNS from AP, fallback to 0.0.0.0");
            }
        }

        if (!naptEnabled) {
            if (WiFi.AP.enableNAPT(true)) {
                naptEnabled = true;
                Serial.println("[NAPT] Enabled successfully.");
            } else {
                Serial.println("[NAPT] Failed to enable.");
            }
        }
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        Serial.println("[STA] Disconnected. Reconnecting...");
        naptEnabled = false;
    }
}

// Nintendo connectivity-check handler for unmatched HTTP routes
void handleHttpNotFound() {
    String host = webServer.hostHeader();
    int colonIdx = host.indexOf(':');
    if (colonIdx != -1) {
        host = host.substring(0, colonIdx);
    }
    host.toLowerCase();

    // Legacy connectivity test (conntest.nintendowifi.net)
    if (host == "conntest.nintendowifi.net") {
        webServer.sendHeader("Cache-Control", "no-store");
        webServer.sendHeader("X-Organization", "Nintendo");
        webServer.send(200, "text/html",
            "<html><head><title>HTML Page</title></head>"
            "<body>This is test.html page</body></html>");
        return;
    }

    // Modern connectivity tests (ctest / api endpoints)
    if (host == "ctest.cdn.nintendo.net" ||
        host == "ctest.p01.ctest.srv.nintendo.net" ||
        host == "ctest-dl-lp1.cdn.nintendo.net" ||
        host == "ctest-ul-lp1.cdn.nintendo.net" ||
        host == "api.p01.lp1.ctest.srv.nintendo.net" ||
        host == "api-ipv6.p01.lp1.ctest.srv.nintendo.net") {
        webServer.sendHeader("Cache-Control", "no-store");
        webServer.sendHeader("X-Organization", "Nintendo");
        webServer.send(200, "text/plain", "ok");
        return;
    }

    // Everything else
    webServer.send(404, "text/plain", "");
}

// Web UI: Settings page at /dns
void handleDnsPage() {
    String staStatus = (WiFi.status() == WL_CONNECTED)
        ? ("Connected (" + WiFi.localIP().toString() + ")")
        : "Disconnected";

    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
    html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>ESP32-S3 SwitchBridge</title>";
    html += "<style>body{font-family:sans-serif;max-width:560px;margin:20px auto;padding:0 15px;}";
    html += "input,textarea{width:100%;box-sizing:border-box;margin:6px 0 14px;padding:8px;font-family:monospace;}";
    html += "input[type='checkbox']{width:auto;margin:0 8px 0 0;vertical-align:middle;}";
    html += "button{padding:10px 16px;background:#0066cc;color:#fff;border:none;cursor:pointer;margin-right:8px;}";
    html += "button.sec{background:#666;}</style></head><body>";
    html += "<h2>ESP32-S3 SwitchBridge</h2>";
    html += "<p><b>STA Status:</b> " + staStatus + "<br>";
    html += "<b>NAPT:</b> " + String(naptEnabled ? "Active" : "Inactive") + "<br>";
    html += "<b>Active Upstream DNS:</b> " + upstreamDnsIP.toString() +
            (upstreamDnsStr.length() == 0 ? " (Auto from AP)" : " (Manual)") + "</p>";
    html += "<form method='POST' action='/dns/save'>";
    html += "<h3>Upstream Wi-Fi (Station)</h3>";
    html += "SSID:<input name='sta_ssid' value='" + staSsid + "'>";
    html += "Password:<input name='sta_pass' type='password' value='" + staPass + "'>";
    html += "<h3>Relay AP</h3>";
    html += "AP SSID:<input name='ap_ssid' value='" + apSsid + "'>";
    html += "AP Password (min 8 chars):<input name='ap_pass' type='password' value='" + apPass + "'>";
    html += "<h3>DNS Settings (IPv4 & IPv6)</h3>";
    html += "<label style='display:block;margin:6px 0 8px;cursor:pointer;'>";
    html += "<input type='checkbox' name='show_allow_dns' value='1'" + String(showAllowDns ? " checked" : "") + ">show allow dns</label>";
    html += "<label style='display:block;margin:6px 0 14px;cursor:pointer;'>";
    html += "<input type='checkbox' name='show_block_dns' value='1'" + String(showBlockDns ? " checked" : "") + ">show block dns</label>";
    html += "Upstream DNS (leave blank for Auto from AP):<input name='up_dns' placeholder='Auto (from upstream AP)' value='" + upstreamDnsStr + "'>";
    html += "Custom DNS IP List (<code>domain</code>, <code>domain ipv4</code>, or <code>domain ipv6</code>):<textarea name='custom_dns' rows='10'>" + customDnsRaw + "</textarea>";
    html += "Allowed Domains (whitelist, blocks all others, # for comments):<textarea name='allowlist' rows='10'>" + allowlistRaw + "</textarea>";
    html += "<button type='submit'>Save & Reboot</button>";
    html += "</form><br>";
    html += "<form method='POST' action='/dns/reset' onsubmit=\"return confirm('Reset Custom DNS & Allowlist to default?');\">";
    html += "<button type='submit' class='sec'>Reset DNS Lists to Default</button></form>";
    html += "</body></html>";

    webServer.send(200, "text/html", html);
}

// Web UI: Save configuration at /dns/save
void handleSave() {
    if (webServer.hasArg("sta_ssid")) staSsid = webServer.arg("sta_ssid");
    if (webServer.hasArg("sta_pass")) staPass = webServer.arg("sta_pass");
    if (webServer.hasArg("ap_ssid") && webServer.arg("ap_ssid").length() > 0) apSsid = webServer.arg("ap_ssid");
    if (webServer.hasArg("ap_pass") && webServer.arg("ap_pass").length() >= 8) apPass = webServer.arg("ap_pass");
    if (webServer.hasArg("up_dns")) {
        upstreamDnsStr = webServer.arg("up_dns");
        upstreamDnsStr.trim();
    }
    if (webServer.hasArg("custom_dns")) customDnsRaw = webServer.arg("custom_dns");
    if (webServer.hasArg("allowlist")) allowlistRaw = webServer.arg("allowlist");
    showAllowDns = webServer.hasArg("show_allow_dns");
    showBlockDns = webServer.hasArg("show_block_dns");

    saveConfig();
    webServer.send(200, "text/html", "<h3>Saved! Rebooting ESP32-S3...</h3>");
    delay(1000);
    ESP.restart();
}

// Web UI: Reset DNS lists to default at /dns/reset
void handleResetDns() {
    customDnsRaw = DEFAULT_CUSTOM_DNS;
    allowlistRaw = DEFAULT_ALLOWLIST;
    prefs.begin("relay_cfg", false);
    prefs.putString("custom_dns", customDnsRaw);
    prefs.putString("allowlist", allowlistRaw);
    prefs.end();

    webServer.sendHeader("Location", "/dns");
    webServer.send(303);
}

void setup() {
    Serial.begin(115200);
    delay(500);

    loadConfig();

    WiFi.onEvent(onWiFiEvent);
    WiFi.mode(WIFI_AP_STA);

    // Configure SoftAP with itself (192.168.4.1) as Gateway and DNS Server
    WiFi.softAPConfig(apIP, apGateway, apSubnet, apDhcpStart, apIP);
    WiFi.softAP(apSsid.c_str(), apPass.c_str());
    esp_wifi_set_ps(WIFI_PS_NONE);
    Serial.printf("[AP] Started: %s (%s)\n", apSsid.c_str(), apIP.toString().c_str());

    // Connect to upstream AP if configured
    if (staSsid.length() > 0) {
        Serial.printf("[STA] Connecting to %s...\n", staSsid.c_str());
        WiFi.begin(staSsid.c_str(), staPass.c_str());
    }

    // Start DNS filter and upstream forwarder
    dnsServer.begin(53);
    dnsUpstream.begin(5353);

    // Start Web Server (Settings mounted at /dns, Nintendo connectivity check onNotFound)
    webServer.on("/dns", HTTP_GET, handleDnsPage);
    webServer.on("/dns/save", HTTP_POST, handleSave);
    webServer.on("/dns/reset", HTTP_POST, handleResetDns);
    webServer.onNotFound(handleHttpNotFound);
    webServer.begin();
    Serial.println("[WEB] Settings page started on http://192.168.4.1/dns");
}

void loop() {
    webServer.handleClient();
    handleDnsFilter();
}