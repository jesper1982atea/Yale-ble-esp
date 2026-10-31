#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <Update.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <mbedtls/sha256.h>
#include <time.h>

// GitHub Releases format: manifest.txt contains version, size and sha256.
// The firmware URL is derived from the configured repository and tag.
// Both downloads use TLS with ESP-IDF's CA bundle; the binary is checked
// against the manifest's SHA-256 before it is activated.
class GitHubOta {
 public:
  enum Action : uint8_t { NONE, CHECK, INSTALL };
  String repository;
  String status = "Ingen uppdatering kontrollerad";
  String availableVersion;
  Action pending = NONE;

  static bool validRepository(const String &value) {
    int slash = value.indexOf('/');
    if (slash < 1 || slash == static_cast<int>(value.length()) - 1 ||
        value.indexOf('/', slash + 1) >= 0 || value.length() > 100) return false;
    for (size_t i = 0; i < value.length(); ++i) {
      char c = value[i];
      if (c == '/') continue;
      if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') return false;
    }
    return true;
  }

  void run(const char *currentVersion) {
    Action action = pending;
    pending = NONE;
    if (action == NONE) return;
    if (!validRepository(repository)) { status = "Ange ett offentligt GitHub-repo som ägare/namn"; return; }
    if (WiFi.status() != WL_CONNECTED) { status = "Wi-Fi är inte anslutet"; return; }
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    uint32_t started = millis();
    while (time(nullptr) < 1700000000 && millis() - started < 15000) delay(100);
    if (time(nullptr) < 1700000000) { status = "Kunde inte synkronisera klockan för HTTPS"; return; }
    if (!check(currentVersion)) return;
    if (action == INSTALL && availableVersion != currentVersion) install();
  }

 private:
  struct Transfer {
    String text;
    bool firmware = false;
    bool failed = false;
    size_t received = 0;
    mbedtls_sha256_context sha;
  };
  size_t availableSize = 0;
  String availableHash;

  static esp_err_t onHttpEvent(esp_http_client_event_t *event) {
    auto *transfer = static_cast<Transfer *>(event->user_data);
    if (event->event_id != HTTP_EVENT_ON_DATA || !transfer || event->data_len <= 0) return ESP_OK;
    if (transfer->firmware) {
      size_t count = static_cast<size_t>(event->data_len);
      if (Update.write(reinterpret_cast<uint8_t *>(event->data), count) != count) {
        transfer->failed = true;
        return ESP_FAIL;
      }
      mbedtls_sha256_update(&transfer->sha,
                            reinterpret_cast<const unsigned char *>(event->data), count);
      transfer->received += count;
    } else {
      if (transfer->text.length() + event->data_len > 512) {
        transfer->failed = true;
        return ESP_FAIL;
      }
      transfer->text.concat(reinterpret_cast<const char *>(event->data), event->data_len);
    }
    return ESP_OK;
  }

  bool download(const String &url, Transfer &transfer) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = onHttpEvent;
    config.user_data = &transfer;
    config.timeout_ms = 20000;
    config.max_redirection_count = 5;
    config.buffer_size = 1024;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { status = "Kunde inte starta HTTPS-klienten"; return false; }
    esp_http_client_set_header(client, "User-Agent", "YaleBLEHomeKit");
    esp_err_t result = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (result != ESP_OK || transfer.failed || code != 200) {
      status = "GitHub HTTPS-fel (HTTP " + String(code) + ", " + esp_err_to_name(result) + ")";
      return false;
    }
    return true;
  }

  static String field(const String &manifest, const char *key) {
    String prefix = String(key) + "=";
    int start = manifest.indexOf(prefix);
    if (start < 0 || (start > 0 && manifest[start - 1] != '\n')) return "";
    start += prefix.length();
    int end = manifest.indexOf('\n', start);
    String value = manifest.substring(start, end < 0 ? manifest.length() : end);
    value.trim();
    return value;
  }

  bool check(const char *currentVersion) {
    status = "Hämtar versionsinformation från GitHub…";
    availableVersion = "";
    Transfer transfer;
    String url = "https://github.com/" + repository + "/releases/latest/download/manifest.txt";
    if (!download(url, transfer)) return false;
    String version = field(transfer.text, "version");
    String sizeText = field(transfer.text, "size");
    String digest = field(transfer.text, "sha256");
    if (version.isEmpty() || version.length() > 32 || sizeText.isEmpty() || digest.length() != 64) {
      status = "Ogiltigt manifest från GitHub Release"; return false;
    }
    for (size_t i = 0; i < version.length(); ++i) {
      char c = version[i];
      if (!isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') {
        status = "Ogiltigt versionsnummer"; return false;
      }
    }
    for (size_t i = 0; i < sizeText.length(); ++i) if (!isdigit(static_cast<unsigned char>(sizeText[i]))) {
      status = "Ogiltig firmwarestorlek"; return false;
    }
    for (size_t i = 0; i < digest.length(); ++i) if (!isxdigit(static_cast<unsigned char>(digest[i]))) {
      status = "Ogiltig SHA-256"; return false;
    }
    unsigned long size = strtoul(sizeText.c_str(), nullptr, 10);
    if (size < 100000 || size > 0x1F0000) { status = "Firmwarestorleken passar inte OTA-partitionen"; return false; }
    availableVersion = version;
    availableSize = size;
    availableHash = digest;
    availableHash.toLowerCase();
    status = version == currentVersion ? "Senaste versionen är redan installerad" : "Ny version tillgänglig: " + version;
    return true;
  }

  void install() {
    status = "Laddar ner och verifierar version " + availableVersion + "…";
    if (!Update.begin(availableSize, U_FLASH)) {
      status = "Ingen OTA-partition eller för lite plats; USB-flashning behövs";
      return;
    }
    Transfer transfer;
    transfer.firmware = true;
    mbedtls_sha256_init(&transfer.sha);
    mbedtls_sha256_starts(&transfer.sha, 0);
    String url = "https://github.com/" + repository + "/releases/download/v" +
                 availableVersion + "/YaleBleHomeKit.bin";
    bool ok = download(url, transfer);
    uint8_t digest[32];
    mbedtls_sha256_finish(&transfer.sha, digest);
    mbedtls_sha256_free(&transfer.sha);
    if (!ok || transfer.received != availableSize) {
      Update.abort();
      if (ok) status = "Nedladdad storlek stämmer inte med manifestet";
      return;
    }
    const char *digits = "0123456789abcdef";
    String actual;
    actual.reserve(64);
    for (uint8_t value : digest) { actual += digits[value >> 4]; actual += digits[value & 15]; }
    if (actual != availableHash) {
      Update.abort();
      status = "SHA-256 stämmer inte; uppdateringen avbröts";
      return;
    }
    if (!Update.end(false)) {
      status = String("Kunde inte aktivera firmware: ") + Update.errorString();
      Update.abort();
      return;
    }
    status = "Installerad. Startar om…";
    delay(500);
    ESP.restart();
  }
};
