#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

constexpr char GITHUB_RELEASE_API[] =
    "https://api.github.com/repos/Hallo32/Arduino_App/releases/latest";
constexpr char FIRMWARE_ASSET_NAME[] = "blinky.ino.bin";
constexpr char WIFI_NAMESPACE[] = "wifi";
constexpr char OTA_NAMESPACE[] = "ota";
constexpr uint32_t BLINK_INTERVAL_MS = 500;
constexpr uint32_t TIME_SYNC_TIMEOUT_MS = 20000;
constexpr time_t VALID_TIME_THRESHOLD = 1700000000;

enum class ProvisioningState : uint8_t { Idle, WaitingForSsid, WaitingForPassword };

bool ledState = false;
bool otaCheckComplete = false;
uint32_t lastLedToggle = 0;
String wifiSsid;
String wifiPassword;
String installedReleaseTag;
String pendingSsid;
String serialLine;
ProvisioningState provisioningState = ProvisioningState::Idle;

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
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.println("Connecting to saved Wi-Fi network");
}

void startWifiProvisioning() {
  provisioningState = ProvisioningState::WaitingForSsid;
  Serial.println("Wi-Fi setup: enter SSID in the serial monitor (115200 baud)");
}

void printSerialHelp() {
  Serial.println("Serial commands:");
  Serial.println("  wifi - configure Wi-Fi credentials");
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

  apiRequest.addHeader("Accept", "application/vnd.github+json");
  apiRequest.addHeader("User-Agent", "ESP32-C6-Blinky");
  apiRequest.addHeader("X-GitHub-Api-Version", "2022-11-28");
  const int statusCode = apiRequest.GET();
  if (statusCode != HTTP_CODE_OK) {
    Serial.printf("GitHub API request failed with HTTP %d\n", statusCode);
    apiRequest.end();
    return;
  }

  JsonDocument release;
  const DeserializationError jsonError =
      deserializeJson(release, apiRequest.getStream());
  apiRequest.end();
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
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
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