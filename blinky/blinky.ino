#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <stdlib.h>
#include <time.h>

constexpr char GITHUB_RELEASE_API[] =
    "https://api.github.com/repos/Hallo32/Arduino_App/releases/latest";
constexpr char FIRMWARE_ASSET_NAME[] = "blinky.ino.bin";
constexpr char DEVICE_HOSTNAME_PREFIX[] = "ESP32-C6-";
constexpr char WIFI_NAMESPACE[] = "wifi";
constexpr char OTA_NAMESPACE[] = "ota";
constexpr uint32_t BLINK_INTERVAL_MS = 500;
constexpr uint32_t TIME_SYNC_TIMEOUT_MS = 20000;
constexpr time_t VALID_TIME_THRESHOLD = 1700000000;
constexpr char GERMAN_TIME_ZONE[] =
  "CET-1CEST,M3.5.0/2,M10.5.0/3";

enum class ProvisioningState : uint8_t { Idle, WaitingForSsid, WaitingForPassword };

bool ledState = false;
bool otaCheckComplete = false;
uint32_t lastLedToggle = 0;
String wifiSsid;
String wifiPassword;
String installedReleaseTag;
String deviceHostname;
String pendingSsid;
String serialLine;
ProvisioningState provisioningState = ProvisioningState::Idle;

void checkForFirmwareUpdate();

void loadSettings() {
  Preferences preferences;
  if (preferences.begin(WIFI_NAMESPACE, true)) {
    wifiSsid = preferences.getString("ssid", "");
    wifiPassword = preferences.getString("password", "");
    preferences.end();
  }

  if (preferences.begin(OTA_NAMESPACE, true)) {
    installedReleaseTag = preferences.getString("tag", "");
    preferences.end();
  }
}

void connectToWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(deviceHostname.c_str());
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.println("Connecting to saved Wi-Fi network");
}

void startWifiProvisioning() {
  WiFi.disconnect(false, false);
  pendingSsid = "";
  provisioningState = ProvisioningState::WaitingForSsid;
  Serial.println("Wi-Fi setup: existing credentials will be overwritten");
  Serial.println("Enter SSID in the serial monitor");
}

String createDeviceHostname() {
  const uint64_t mac = ESP.getEfuseMac();
  char hostname[32];
  snprintf(hostname, sizeof(hostname), "%s%06llX", DEVICE_HOSTNAME_PREFIX,
           static_cast<unsigned long long>(mac & 0xFFFFFFULL));
  return String(hostname);
}

void printSerialHelp() {
  Serial.println("Serial commands:");
  Serial.println("  wifi - configure Wi-Fi credentials");
  Serial.println("  Ctrl+C - cancel Wi-Fi configuration");
  Serial.println("  ip - show the current IP address");
  Serial.println("  hostname - show the device hostname");
  Serial.println("  wlan - show Wi-Fi network information");
  Serial.println("  time - show UTC and German local time");
  Serial.println("  version - show the installed firmware version");
  Serial.println("  update - check for a firmware update");
}

void printIpAddress() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("IP address: Wi-Fi is not connected");
  }
}

void printHostname() {
  const char *hostname = WiFi.getHostname();
  Serial.printf("Hostname: %s\n", hostname != nullptr ? hostname : "not set");
}

void printWlanInfo() {
  Serial.printf("Wi-Fi status: %s\n", WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("Signal strength: %d dBm\n", WiFi.RSSI());
  Serial.printf("MAC address: %s\n", WiFi.macAddress().c_str());
  Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("Subnet mask: %s\n", WiFi.subnetMask().toString().c_str());
  Serial.printf("DNS server: %s\n", WiFi.dnsIP().toString().c_str());
}

void printCurrentTime() {
  const time_t currentTime = time(nullptr);
  if (currentTime < VALID_TIME_THRESHOLD) {
    Serial.println("Time: not synchronized");
    return;
  }

  struct tm utcTime;
  gmtime_r(&currentTime, &utcTime);

  struct tm localTime;
  localtime_r(&currentTime, &localTime);
  char formattedUtcTime[24];
  char formattedLocalTime[24];
  strftime(formattedUtcTime, sizeof(formattedUtcTime), "%Y-%m-%dT%H:%M:%S",
           &utcTime);
  strftime(formattedLocalTime, sizeof(formattedLocalTime), "%Y-%m-%dT%H:%M:%S",
           &localTime);
  Serial.printf("Time (UTC): %sZ\n", formattedUtcTime);
  Serial.printf("Time (Germany): %s %s\n", formattedLocalTime,
                localTime.tm_isdst > 0 ? "CEST" : "CET");
}

void printFirmwareVersion() {
  if (installedReleaseTag.isEmpty()) {
    Serial.println("Firmware version: unknown (no release tag in NVS)");
  } else {
    Serial.printf("Firmware version: %s\n", installedReleaseTag.c_str());
  }
}

void cancelWifiProvisioning() {
  provisioningState = ProvisioningState::Idle;
  pendingSsid = "";
  serialLine = "";
  Serial.println("Wi-Fi setup cancelled; existing credentials were preserved");

  if (!wifiSsid.isEmpty()) {
    connectToWifi();
  }
}

bool saveWifiCredentials(const String &ssid, const String &password) {
  Preferences preferences;
  if (!preferences.begin(WIFI_NAMESPACE, false)) {
    return false;
  }

  const bool saved = preferences.putString("ssid", ssid) > 0 &&
                     preferences.putString("password", password) > 0;
  preferences.end();
  return saved;
}

void handleSerialLine(const String &line) {
  if (provisioningState == ProvisioningState::Idle) {
    if (line == "wifi") {
      startWifiProvisioning();
    } else if (line == "help" || line.isEmpty()) {
      printSerialHelp();
    } else if (line == "ip") {
      printIpAddress();
    } else if (line == "hostname") {
      printHostname();
    } else if (line == "wlan") {
      printWlanInfo();
    } else if (line == "time") {
      printCurrentTime();
    } else if (line == "version") {
      printFirmwareVersion();
    } else if (line == "update") {
      checkForFirmwareUpdate();
    }
    return;
  }

  if (provisioningState == ProvisioningState::WaitingForSsid) {
    if (line.isEmpty() || line.length() > 32) {
      Serial.println("SSID must contain 1 to 32 characters; enter it again");
      return;
    }

    pendingSsid = line;
    provisioningState = ProvisioningState::WaitingForPassword;
    Serial.println("Enter Wi-Fi password (empty for an open network)");
    return;
  }

  if (line.length() > 63) {
    Serial.println("Password is too long; enter it again");
    return;
  }

  if (!saveWifiCredentials(pendingSsid, line)) {
    Serial.println("Could not save Wi-Fi credentials to NVS; retry with 'wifi'");
    provisioningState = ProvisioningState::Idle;
    return;
  }

  wifiSsid = pendingSsid;
  wifiPassword = line;
  pendingSsid = "";
  provisioningState = ProvisioningState::Idle;
  otaCheckComplete = false;
  connectToWifi();
}

void handleSerialInput() {
  while (Serial.available() > 0) {
    const char character = static_cast<char>(Serial.read());
    if (character == 0x03) {
      cancelWifiProvisioning();
      continue;
    }

    if (character == '\r') {
      continue;
    }

    if (character == '\n') {
      handleSerialLine(serialLine);
      serialLine = "";
    } else if (serialLine.length() < 64) {
      serialLine += character;
    } else {
      serialLine = "";
      Serial.println("Input too long; line discarded");
    }
  }
}

bool synchronizeClock() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  const uint32_t startedAt = millis();

  while (time(nullptr) < VALID_TIME_THRESHOLD &&
         millis() - startedAt < TIME_SYNC_TIMEOUT_MS) {
    delay(250);
  }

  return time(nullptr) >= VALID_TIME_THRESHOLD;
}

bool saveInstalledReleaseTag(const String &tag) {
  Preferences preferences;
  if (!preferences.begin(OTA_NAMESPACE, false)) {
    return false;
  }

  const bool saved = preferences.putString("tag", tag) > 0;
  preferences.end();
  return saved;
}

void checkForFirmwareUpdate() {
  if (!synchronizeClock()) {
    Serial.println("OTA skipped: system time could not be synchronized");
    return;
  }

  WiFiClientSecure apiClient;
  apiClient.useBuiltinCACertBundle();
  apiClient.setTimeout(15000);

  HTTPClient apiRequest;
  if (!apiRequest.begin(apiClient, GITHUB_RELEASE_API)) {
    Serial.println("OTA skipped: could not start GitHub API request");
    return;
  }
  apiRequest.setTimeout(15000);

  apiRequest.addHeader("Accept", "application/vnd.github+json");
  apiRequest.addHeader("User-Agent", "ESP32-C6");
  apiRequest.addHeader("X-GitHub-Api-Version", "2022-11-28");
  const int statusCode = apiRequest.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("GitHub API request failed with HTTP %d\n", statusCode);
    apiRequest.end();
    return;
  }

  const String releasePayload = apiRequest.getString();
  apiRequest.end();

  if (releasePayload.isEmpty()) {
    Serial.println("Could not parse GitHub release JSON: empty response");
    return;
  }

  JsonDocument release;
  const DeserializationError jsonError =
      deserializeJson(release, releasePayload);
  if (jsonError) {
    Serial.printf("Could not parse GitHub release JSON: %s\n", jsonError.c_str());
    return;
  }

  const String latestTag = release["tag_name"] | "";
  String firmwareUrl;
  for (JsonObject asset : release["assets"].as<JsonArray>()) {
    if (asset["name"] == FIRMWARE_ASSET_NAME) {
      firmwareUrl = asset["browser_download_url"] | "";
      break;
    }
  }

  if (latestTag.isEmpty() || firmwareUrl.isEmpty()) {
    Serial.println("Latest GitHub release has no tag or firmware asset");
    return;
  }

  if (latestTag == installedReleaseTag) {
    Serial.printf("OTA: release %s is already installed\n", latestTag.c_str());
    return;
  }

  if (!firmwareUrl.startsWith("https://")) {
    Serial.println("OTA skipped: release asset URL is not HTTPS");
    return;
  }

  Serial.printf("Installing GitHub release %s\n", latestTag.c_str());
  WiFiClientSecure firmwareClient;
  firmwareClient.useBuiltinCACertBundle();
  firmwareClient.setTimeout(15000);

  HTTPUpdate updater;
  updater.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  updater.rebootOnUpdate(false);
  int lastReportedPercent = -1;
  updater.onProgress([&lastReportedPercent](int current, int total) {
    if (total <= 0) {
      return;
    }

    const int percent = current * 100 / total;
    if (percent != lastReportedPercent) {
      lastReportedPercent = percent;
      Serial.printf("OTA progress: %d%%\n", percent);
    }
  });

  const t_httpUpdate_return result =
      updater.update(firmwareClient, firmwareUrl, installedReleaseTag);

  if (result == HTTP_UPDATE_NO_UPDATES) {
    Serial.println("OTA server reports no update");
  } else if (result == HTTP_UPDATE_FAILED) {
    Serial.printf("OTA failed (%d): %s\n", updater.getLastError(),
                  updater.getLastErrorString().c_str());
  } else if (result == HTTP_UPDATE_OK) {
    if (!saveInstalledReleaseTag(latestTag)) {
      Serial.println("Could not save installed release tag to NVS");
    }
    Serial.println("OTA complete; restarting");
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  setenv("TZ", GERMAN_TIME_ZONE, 1);
  tzset();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  deviceHostname = createDeviceHostname();
  loadSettings();

  if (wifiSsid.isEmpty()) {
    startWifiProvisioning();
  } else {
    connectToWifi();
  }
  printSerialHelp();
}

void loop() {
  handleSerialInput();

  const uint32_t now = millis();
  if (now - lastLedToggle >= BLINK_INTERVAL_MS) {
    lastLedToggle = now;
    ledState = !ledState;
    digitalWrite(LED_BUILTIN, ledState ? HIGH : LOW);
  }

  if (!otaCheckComplete && WiFi.status() == WL_CONNECTED) {
    otaCheckComplete = true;
    Serial.println("Wi-Fi connected; checking latest GitHub release");
    checkForFirmwareUpdate();
  }
}