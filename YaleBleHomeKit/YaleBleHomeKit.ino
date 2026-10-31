#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <HomeSpan.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <esp_system.h>
#include "GitHubOta.h"

constexpr char YALE_FW_VERSION[] = "0.2.0";

// Minimal ESP32 port of the lock/status/authentication path of yalexs-ble.
// One BLE connection and one HomeKit LockMechanism are supported.
namespace Yale {
constexpr char SERVICE[] = "0000fe24-0000-1000-8000-00805f9b34fb";
constexpr char WRITE[] = "bd4ac611-0b45-11e3-8ffd-0800200c9a66";
constexpr char READ[] = "bd4ac612-0b45-11e3-8ffd-0800200c9a66";
constexpr char SECURE_WRITE[] = "bd4ac613-0b45-11e3-8ffd-0800200c9a66";
constexpr char SECURE_READ[] = "bd4ac614-0b45-11e3-8ffd-0800200c9a66";
constexpr uint8_t FRAME = 18;
}

Preferences settings;
GitHubOta ota;
WebServer adminServer(80);
DNSServer setupDns;
String lockAddress, lockName, offlineKey, adminSalt, adminHash, pairingCode;
uint8_t addressType = 0, keyIndex = 0;
String sessionToken;
bool webStarted = false;
bool wifiRestartRequested = false;
uint32_t wifiRestartAt = 0;

String hexEncode(const uint8_t *data, size_t length) {
  const char *digits = "0123456789abcdef";
  String result;
  result.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    result += digits[data[i] >> 4];
    result += digits[data[i] & 15];
  }
  return result;
}

bool hexDecode(const String &input, uint8_t *output, size_t length) {
  if (input.length() != length * 2) return false;
  for (size_t i = 0; i < length; ++i) {
    char hi = input[i * 2], lo = input[i * 2 + 1];
    if (!isxdigit((unsigned char)hi) || !isxdigit((unsigned char)lo)) return false;
    auto digit = [](char c) -> uint8_t { return c <= '9' ? c - '0' : (tolower(c) - 'a' + 10); };
    output[i] = (digit(hi) << 4) | digit(lo);
  }
  return true;
}

String randomHex(size_t byteCount) {
  uint8_t bytes[32];
  if (byteCount > sizeof(bytes)) byteCount = sizeof(bytes);
  esp_fill_random(bytes, byteCount);
  return hexEncode(bytes, byteCount);
}

String passwordHash(const String &salt, const String &password) {
  String input = salt + ":" + password;
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char *>(input.c_str()), input.length(), digest, 0);
  return hexEncode(digest, sizeof(digest));
}

String escapeHtml(const String &value) {
  String out;
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c;
    }
  }
  return out;
}

String page(const String &title, const String &body) {
  return String(F("<!doctype html><html lang='sv'><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><title>")) +
    escapeHtml(title) + F("</title><style>body{font:16px system-ui;background:#f3f5f8;color:#17222d;margin:0}main{max-width:670px;margin:32px auto;padding:0 16px}section{background:white;border-radius:16px;padding:22px;margin:16px 0;box-shadow:0 3px 16px #17222d12}h1{font-size:1.7rem}h2{font-size:1.2rem}label{display:block;margin:14px 0 5px;font-weight:600}input,select,button{box-sizing:border-box;width:100%;padding:12px;border:1px solid #c4ccd5;border-radius:9px;font:inherit}button{border:0;background:#124d79;color:white;font-weight:700;margin-top:18px;cursor:pointer}.muted{color:#617182}.ok{color:#146739}.err{color:#a72424}code{word-break:break-all}small{display:block;margin-top:8px}</style><main>") +
    F("<h1>Yale BLE · ") + escapeHtml(title) + F("</h1>") + body + F("</main></html>");
}

class YaleClient {
 public:
  bool connected = false;
  bool authenticated = false;
  uint8_t state = 3;  // HomeKit: unknown
  uint32_t lastSeen = 0;
  String error;

  void begin() {
    NimBLEDevice::init("Yale BLE HomeKit");
    mbedtls_aes_init(&secureEnc);
    mbedtls_aes_init(&secureDec);
    mbedtls_aes_init(&normalEnc);
    mbedtls_aes_init(&normalDec);
    frames = xQueueCreate(12, sizeof(Notification));
  }

  bool connect() {
    if ((lockAddress.length() != 17 && lockName.isEmpty()) || offlineKey.length() != 32) {
      error = "Ange BLE-adress eller namn samt offline key";
      return false;
    }
    uint8_t key[16];
    if (!hexDecode(offlineKey, key, 16)) { error = "Ogiltig offline key"; return false; }
    disconnect();
    if (!lockName.isEmpty()) {
      NimBLEScan *scan = NimBLEDevice::getScan();
      scan->setActiveScan(true);
      NimBLEScanResults results = scan->getResults(5000, false);
      bool found = false;
      for (int i = 0; i < results.getCount(); ++i) {
        const NimBLEAdvertisedDevice *device = results.getDevice(i);
        if (device && String(device->getName().c_str()) == lockName) {
          lockAddress = String(device->getAddress().toString().c_str());
          addressType = device->getAddress().getType();
          found = true;
          break;
        }
      }
      scan->clearResults();
      if (!found) { error = "Hittade inte BLE-namnet " + lockName; return false; }
    }
    client = NimBLEDevice::createClient();
    if (!client) { error = "Kunde inte skapa BLE-klient"; return false; }
    NimBLEAddress address(lockAddress.c_str(), addressType);
    if (!client->connect(address)) { error = "BLE-anslutning misslyckades"; disconnect(); return false; }
    NimBLERemoteService *service = client->getService(Yale::SERVICE);
    if (!service) { error = "FE24-tjänsten saknas"; disconnect(); return false; }
    writeChar = service->getCharacteristic(Yale::WRITE);
    readChar = service->getCharacteristic(Yale::READ);
    secureWriteChar = service->getCharacteristic(Yale::SECURE_WRITE);
    secureReadChar = service->getCharacteristic(Yale::SECURE_READ);
    if (!writeChar || !readChar || !secureWriteChar || !secureReadChar) {
      error = "Yale BLE-karakteristik saknas"; disconnect(); return false;
    }
    active = this;
    if (!secureReadChar->subscribe(true, secureNotification)) {
      error = "Kunde inte prenumerera på secure notify"; disconnect(); return false;
    }
    if (!handshake(key)) { disconnect(); return false; }
    if (!readChar->subscribe(true, normalNotification)) {
      error = "Kunde inte prenumerera på status notify"; disconnect(); return false;
    }
    connected = authenticated = true;
    error = "";
    if (!pollStatus()) { disconnect(); return false; }
    return true;
  }

  void disconnect() {
    connected = authenticated = false;
    state = 3;
    active = nullptr;
    if (client) {
      if (client->isConnected()) client->disconnect();
      NimBLEDevice::deleteClient(client);
      client = nullptr;
    }
    writeChar = readChar = secureWriteChar = secureReadChar = nullptr;
    if (frames) xQueueReset(frames);
  }

  bool pollStatus() {
    if (!client || !client->isConnected() || !authenticated) return false;
    uint8_t command[Yale::FRAME] = {0};
    command[0] = 0xEE; command[1] = 0x02; command[4] = 0x02; command[16] = 0x02;
    uint8_t response[Yale::FRAME];
    if (!exchange(false, command, response, 0xBB, 0x02, 0x02, 10000)) return false;
    updateState(response[8]);
    return true;
  }

  bool setLocked(bool locked) {
    if (!client || !client->isConnected() || !authenticated) {
      error = "Låset är inte anslutet"; return false;
    }
    uint8_t opcode = locked ? 0x0B : 0x0A;
    uint8_t command[Yale::FRAME] = {0};
    command[0] = 0xEE; command[1] = opcode; command[16] = 0x02;
    uint8_t response[Yale::FRAME];
    if (!exchange(false, command, response, 0xBB, opcode, 0xFF, 12000)) return false;
    if (response[15] != 0) {
      error = "Låset rapporterade felkod " + String(response[15]);
      state = 2;
      return false;
    }
    return pollStatus();
  }

 private:
  struct Notification { uint8_t secure; uint8_t length; uint8_t bytes[Yale::FRAME]; };
  static YaleClient *active;
  QueueHandle_t frames = nullptr;
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *writeChar = nullptr, *readChar = nullptr;
  NimBLERemoteCharacteristic *secureWriteChar = nullptr, *secureReadChar = nullptr;
  mbedtls_aes_context secureEnc, secureDec, normalEnc, normalDec;
  uint8_t sendIv[16] = {0}, receiveIv[16] = {0};

  static void enqueue(bool secure, uint8_t *data, size_t length) {
    if (!active || length != Yale::FRAME) return;
    Notification n{};
    n.secure = secure;
    n.length = length;
    memcpy(n.bytes, data, length);
    xQueueSend(active->frames, &n, 0);
  }
  static void secureNotification(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) { enqueue(true, data, length); }
  static void normalNotification(NimBLERemoteCharacteristic *, uint8_t *data, size_t length, bool) { enqueue(false, data, length); }

  static uint8_t simpleChecksum(const uint8_t *buffer) {
    uint32_t sum = 0;
    for (size_t i = 0; i < Yale::FRAME; ++i) sum += buffer[i];
    return static_cast<uint8_t>(-sum);
  }
  static uint32_t secureChecksum(const uint8_t *buffer) {
    // util._security_checksum in yalexs-ble adds three little-endian
    // 32-bit words. Its final 10-byte operand is reduced modulo 2^32.
    uint32_t sum = 0;
    for (size_t i = 0; i < 12; ++i) sum += static_cast<uint32_t>(buffer[i]) << ((i % 4) * 8);
    return -sum;
  }
  static uint32_t checksumField(const uint8_t *buffer) {
    return static_cast<uint32_t>(buffer[12]) |
           (static_cast<uint32_t>(buffer[13]) << 8) |
           (static_cast<uint32_t>(buffer[14]) << 16) |
           (static_cast<uint32_t>(buffer[15]) << 24);
  }
  void updateState(uint8_t yaleState) {
    switch (yaleState) {
      case 0x03: case 0x0A: state = 0; break;
      case 0x05: case 0x0C: state = 1; break;
      case 0x07: state = 2; break;
      default: state = 3; break;
    }
    lastSeen = millis();
  }
  bool receive(bool secure, uint8_t *out, uint32_t timeoutMs) {
    uint32_t start = millis();
    Notification n;
    while (millis() - start < timeoutMs) {
      if (xQueueReceive(frames, &n, pdMS_TO_TICKS(50)) != pdTRUE) continue;
      uint8_t plain[16];
      if (n.secure) {
        mbedtls_aes_crypt_ecb(&secureDec, MBEDTLS_AES_DECRYPT, n.bytes, plain);
      } else {
        mbedtls_aes_crypt_cbc(&normalDec, MBEDTLS_AES_DECRYPT, 16, receiveIv, n.bytes, plain);
      }
      memcpy(n.bytes, plain, 16);
      bool valid = n.secure ? checksumField(n.bytes) == secureChecksum(n.bytes) : simpleChecksum(n.bytes) == 0;
      if (!valid) continue;
      if (!n.secure && n.bytes[0] == 0xBB && n.bytes[1] == 0x02 && n.bytes[4] == 0x02)
        updateState(n.bytes[8]);
      if (n.secure == secure) { memcpy(out, n.bytes, Yale::FRAME); return true; }
    }
    error = "Timeout från Yale-låset";
    return false;
  }
  bool exchange(bool secure, uint8_t *command, uint8_t *out, uint8_t flag, uint8_t opcode,
                uint8_t subtype, uint32_t timeoutMs) {
    if (secure) {
      uint32_t check = secureChecksum(command);
      for (int i = 0; i < 4; ++i) command[12 + i] = (check >> (8 * i)) & 0xff;
    } else {
      command[3] = 0;
      command[3] = simpleChecksum(command);
    }
    uint8_t ciphertext[16];
    if (secure) mbedtls_aes_crypt_ecb(&secureEnc, MBEDTLS_AES_ENCRYPT, command, ciphertext);
    else mbedtls_aes_crypt_cbc(&normalEnc, MBEDTLS_AES_ENCRYPT, 16, sendIv, command, ciphertext);
    memcpy(command, ciphertext, 16);
    NimBLERemoteCharacteristic *target = secure ? secureWriteChar : writeChar;
    if (!target || !target->writeValue(command, Yale::FRAME, true)) {
      error = "BLE-skrivning misslyckades";
      return false;
    }
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
      if (!receive(secure, out, timeoutMs - (millis() - start))) return false;
      if (out[0] == flag && (opcode == 0xFF || out[1] == opcode) &&
          (subtype == 0xFF || out[4] == subtype)) return true;
      // ACK and unrelated status frames can arrive before the response.
    }
    error = "Inget svar på kommando";
    return false;
  }
  bool handshake(const uint8_t *key) {
    if (mbedtls_aes_setkey_enc(&secureEnc, key, 128) || mbedtls_aes_setkey_dec(&secureDec, key, 128)) {
      error = "AES-nyckeln kunde inte sättas"; return false;
    }
    uint8_t randomBytes[16], sessionKey[16], response[Yale::FRAME], command[Yale::FRAME];
    esp_fill_random(randomBytes, sizeof(randomBytes));
    memset(command, 0, sizeof(command));
    command[0] = 0x01; memcpy(command + 4, randomBytes, 8);
    command[16] = 0x0F; command[17] = keyIndex;
    if (!exchange(true, command, response, 0x02, 0xFF, 0xFF, 10000)) return false;
    memcpy(sessionKey, randomBytes, 8);
    memcpy(sessionKey + 8, response + 4, 8);
    if (mbedtls_aes_setkey_enc(&secureEnc, sessionKey, 128) ||
        mbedtls_aes_setkey_dec(&secureDec, sessionKey, 128) ||
        mbedtls_aes_setkey_enc(&normalEnc, sessionKey, 128) ||
        mbedtls_aes_setkey_dec(&normalDec, sessionKey, 128)) {
      error = "Sessionsnyckeln kunde inte sättas"; return false;
    }
    memset(sendIv, 0, sizeof(sendIv)); memset(receiveIv, 0, sizeof(receiveIv));
    memset(command, 0, sizeof(command));
    command[0] = 0x03; memcpy(command + 4, randomBytes + 8, 8);
    command[16] = 0x0F; command[17] = keyIndex;
    if (!exchange(true, command, response, 0x04, 0xFF, 0xFF, 10000)) return false;
    return true;
  }
};
YaleClient *YaleClient::active = nullptr;
YaleClient yale;

// HomeKit callbacks enqueue requests. BLE I/O runs later in loop(), so HAP stays responsive.
int pendingLock = -1;
struct YaleLockService : Service::LockMechanism {
  SpanCharacteristic *current, *target;
  YaleLockService() : Service::LockMechanism() {
    current = new Characteristic::LockCurrentState(3);
    target = new Characteristic::LockTargetState(1);
  }
  boolean update() override {
    pendingLock = target->getNewVal() == 1 ? 1 : 0;
    return true;
  }
};
YaleLockService *lockService = nullptr;

bool isAuthenticated() {
  if (sessionToken.isEmpty() || !adminServer.hasHeader("Cookie")) return false;
  return adminServer.header("Cookie").indexOf("yale_session=" + sessionToken) >= 0;
}
bool requireAuth() {
  if (isAuthenticated()) return true;
  adminServer.sendHeader("Location", "/login");
  adminServer.send(303);
  return false;
}
void sendHtml(const String &title, const String &body) {
  adminServer.sendHeader("Cache-Control", "no-store");
  adminServer.send(200, "text/html; charset=utf-8", page(title, body));
}

void showLogin() {
  sendHtml("Logga in", F("<section><form method='post' action='/login'><label>Adminlösenord</label><input type='password' name='password' required autocomplete='current-password'><button>Logga in</button></form></section>"));
}
void postLogin() {
  String supplied = passwordHash(adminSalt, adminServer.arg("password"));
  if (supplied != adminHash || adminHash.isEmpty()) {
    adminServer.send(403, "text/html; charset=utf-8", page("Fel lösenord", F("<section><p>Inloggningen misslyckades.</p><a href='/login'>Försök igen</a></section>")));
    return;
  }
  sessionToken = randomHex(24);
  adminServer.sendHeader("Set-Cookie", "yale_session=" + sessionToken + "; HttpOnly; SameSite=Strict; Path=/");
  adminServer.sendHeader("Location", "/");
  adminServer.send(303);
}

void showAdmin() {
  if (!requireAuth()) return;
  String status = yale.state == 0 ? "Olåst" : yale.state == 1 ? "Låst" : yale.state == 2 ? "Fel/jam" : "Okänt";
  String body = F("<section><h2>Status</h2><p>Wi‑Fi: <strong>");
  body += escapeHtml(WiFi.isConnected() ? WiFi.SSID() : String("Frånkopplat"));
  body += F("</strong><br>IP: "); body += WiFi.localIP().toString();
  body += F("<br>Bluetooth: "); body += yale.connected ? "Ansluten" : "Frånkopplad";
  body += F("<br>Lås: <strong>"); body += status; body += F("</strong><br>Senaste fel: ");
  body += escapeHtml(yale.error.isEmpty() ? String("Inget") : yale.error);
  body += F("</p></section><section><h2>Yale Access BLE</h2><form method='get' action='/scan'><button>Sök BLE-enheter</button></form><form method='post' action='/lock-config'><label>BLE-adress</label><input name='address' maxlength='17' placeholder='AA:BB:CC:DD:EE:FF' value='");
  body += escapeHtml(lockAddress);
  body += F("'><label>BLE-namn (valfritt; används vid roterande adress)</label><input name='name' maxlength='32' value='");
  body += escapeHtml(lockName);
  body += F("'><label>Adresstyp</label><select name='type'><option value='0'");
  if (addressType == 0) body += " selected";
  body += F(">Public</option><option value='1'");
  if (addressType == 1) body += " selected";
  body += F(">Random</option></select><label>Offline key (32 hextecken)</label><input type='password' name='key' minlength='32' maxlength='32' placeholder='Lämna tomt för att behålla nyckeln'><small>Nyckel sparad: ");
  body += offlineKey.length() == 32 ? "ja" : "nej";
  body += F("</small><label>Key index / slot (0–255)</label><input type='number' name='slot' min='0' max='255' value='");
  body += String(keyIndex);
  body += F("' required><button>Spara låskonfiguration</button></form><p><a href='https://github.com/Yale-Libs/yalexs-ble'>Om offline key och slot</a>: se projektets installationsguide på GitHub.</p></section><section><h2>HomeKit</h2><p>Parkod: <code>");
  body += escapeHtml(pairingCode);
  body += F("</code></p></section><section><h2>Firmware</h2><p>Installerad version: <strong>");
  body += YALE_FW_VERSION;
  body += F("</strong><br>Status: "); body += escapeHtml(ota.status);
  body += F("</p><form method='post' action='/ota-repo'><label>Offentligt GitHub-repo (ägare/namn)</label><input name='repository' maxlength='100' value='");
  body += escapeHtml(ota.repository);
  body += F("' placeholder='användare/Yale-ble-esp'><button>Spara repo</button></form><form method='post' action='/ota-check'><button>Sök uppdatering</button></form><form method='post' action='/ota-install'><button>Installera senaste versionen</button></form><small>Sidan svarar direkt. Ladda om efter några sekunder för status. En uppdatering startar om enheten.</small></section><section><h2>Administration</h2><form method='post' action='/password'><label>Nytt adminlösenord</label><input type='password' name='password' minlength='12' required><button>Byt lösenord</button></form><form method='post' action='/wifi-reset'><button>Välj nytt Wi‑Fi</button></form><form method='post' action='/logout'><button>Logga ut</button></form></section>");
  sendHtml("Admin", body);
}

void postLockConfig() {
  if (!requireAuth()) return;
  String address = adminServer.arg("address"); address.toLowerCase();
  String name = adminServer.arg("name"); name.trim();
  bool validAddress = address.isEmpty() || address.length() == 17;
  if (address.length() == 17) {
    for (int i = 0; i < 17; ++i) {
      if ((i + 1) % 3 == 0) validAddress &= address[i] == ':';
      else validAddress &= isxdigit((unsigned char)address[i]);
    }
  }
  String newKey = adminServer.arg("key");
  uint8_t keyBytes[16];
  bool validKey = newKey.isEmpty() || hexDecode(newKey, keyBytes, 16);
  int slot = adminServer.arg("slot").toInt();
  if (!validAddress || (address.isEmpty() && name.isEmpty()) || name.length() > 32 ||
      !validKey || slot < 0 || slot > 255 || (offlineKey.isEmpty() && newKey.isEmpty())) {
    adminServer.send(400, "text/plain; charset=utf-8", "Ogiltig adress, nyckel eller slot"); return;
  }
  lockAddress = address; lockName = name;
  addressType = adminServer.arg("type") == "1" ? 1 : 0; keyIndex = slot;
  settings.putString("address", lockAddress);
  settings.putString("name", lockName);
  settings.putUChar("addr_type", addressType);
  settings.putUChar("slot", keyIndex);
  if (!newKey.isEmpty()) { offlineKey = newKey; offlineKey.toLowerCase(); settings.putString("offline", offlineKey); }
  yale.disconnect();
  adminServer.sendHeader("Location", "/"); adminServer.send(303);
}

void showScan() {
  if (!requireAuth()) return;
  yale.disconnect();
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults results = scan->getResults(5000, false);
  String body = F("<section><p>Välj låsets annonserade BLE-namn. Namnet används för att hitta en ny adress vid varje anslutning.</p>");
  for (int i = 0; i < results.getCount(); ++i) {
    const NimBLEAdvertisedDevice *device = results.getDevice(i);
    if (!device || device->getName().empty()) continue;
    String name = String(device->getName().c_str());
    String address = String(device->getAddress().toString().c_str());
    body += F("<form method='post' action='/select'><input type='hidden' name='address' value='");
    body += escapeHtml(address);
    body += F("'><input type='hidden' name='name' value='");
    body += escapeHtml(name);
    body += F("'><input type='hidden' name='type' value='");
    body += String(device->getAddress().getType());
    body += F("'><button>"); body += escapeHtml(name); body += F(" · "); body += escapeHtml(address);
    body += F("</button></form>");
  }
  body += F("<p><a href='/'>Tillbaka till admin</a></p></section>");
  scan->clearResults();
  sendHtml("BLE-sökning", body);
}

void postSelect() {
  if (!requireAuth()) return;
  String address = adminServer.arg("address"), name = adminServer.arg("name");
  if (address.length() != 17 || name.isEmpty() || name.length() > 32) {
    adminServer.send(400, "text/plain", "Ogiltig BLE-enhet"); return;
  }
  lockAddress = address; lockName = name;
  addressType = adminServer.arg("type").toInt() == 1 ? 1 : 0;
  settings.putString("address", lockAddress);
  settings.putString("name", lockName);
  settings.putUChar("addr_type", addressType);
  adminServer.sendHeader("Location", "/"); adminServer.send(303);
}

void postPassword() {
  if (!requireAuth()) return;
  String password = adminServer.arg("password");
  if (password.length() < 12) { adminServer.send(400, "text/plain", "Minst 12 tecken krävs"); return; }
  adminSalt = randomHex(16); adminHash = passwordHash(adminSalt, password);
  settings.putString("salt", adminSalt); settings.putString("hash", adminHash);
  sessionToken = "";
  adminServer.sendHeader("Set-Cookie", "yale_session=; Max-Age=0; Path=/");
  adminServer.sendHeader("Location", "/login"); adminServer.send(303);
}

void postWifiReset() {
  if (!requireAuth()) return;
  adminServer.send(200, "text/html; charset=utf-8", page("Startar om", F("<section>Anslut till Yale-BLE-Setup efter omstart.</section>")));
  wifiRestartRequested = true;
  wifiRestartAt = millis() + 1500;
}

void postOtaRepo() {
  if (!requireAuth()) return;
  String repository = adminServer.arg("repository"); repository.trim();
  if (!GitHubOta::validRepository(repository)) {
    adminServer.send(400, "text/plain; charset=utf-8", "Ange ägare/namn för ett offentligt GitHub-repo"); return;
  }
  ota.repository = repository;
  settings.putString("github_repo", repository);
  ota.status = "GitHub-repo sparat";
  adminServer.sendHeader("Location", "/"); adminServer.send(303);
}

void postOtaAction(GitHubOta::Action action) {
  if (!requireAuth()) return;
  ota.pending = action;
  ota.status = action == GitHubOta::CHECK ? "Kontroll väntar…" : "Installation väntar…";
  adminServer.sendHeader("Location", "/"); adminServer.send(303);
}

void startWeb() {
  if (webStarted) return;
  const char *headers[] = {"Cookie"};
  adminServer.collectHeaders(headers, 1);
  adminServer.on("/", HTTP_GET, showAdmin);
  adminServer.on("/login", HTTP_GET, showLogin);
  adminServer.on("/login", HTTP_POST, postLogin);
  adminServer.on("/lock-config", HTTP_POST, postLockConfig);
  adminServer.on("/scan", HTTP_GET, showScan);
  adminServer.on("/select", HTTP_POST, postSelect);
  adminServer.on("/password", HTTP_POST, postPassword);
  adminServer.on("/wifi-reset", HTTP_POST, postWifiReset);
  adminServer.on("/ota-repo", HTTP_POST, postOtaRepo);
  adminServer.on("/ota-check", HTTP_POST, [] { postOtaAction(GitHubOta::CHECK); });
  adminServer.on("/ota-install", HTTP_POST, [] { postOtaAction(GitHubOta::INSTALL); });
  adminServer.on("/logout", HTTP_POST, [] {
    if (!requireAuth()) return;
    sessionToken = "";
    adminServer.sendHeader("Set-Cookie", "yale_session=; Max-Age=0; Path=/");
    adminServer.sendHeader("Location", "/login"); adminServer.send(303);
  });
  adminServer.onNotFound([] { adminServer.send(404, "text/plain", "Not found"); });
  adminServer.begin();
  webStarted = true;
  Serial.printf("Admin: http://%s/\n", WiFi.localIP().toString().c_str());
}

// HomeSpan calls this when no Wi-Fi credentials are stored. The SoftAP is
// intentionally open as requested, and offers no lock controls or offline key.
void setupPortal() {
  String ssid = "Yale-BLE-Setup-" + WiFi.macAddress().substring(9);
  ssid.replace(":", "");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ssid.c_str());
  setupDns.start(53, "*", WiFi.softAPIP());
  WebServer portal(80);
  auto form = [&]() {
    String body = F("<section><p>Välj ditt 2,4 GHz Wi‑Fi. Nätet här är öppet under installationen.</p><form method='post' action='/save'><label>Wi‑Fi</label><select name='ssid'>");
    int found = WiFi.scanNetworks();
    for (int i = 0; i < found; ++i) {
      String name = WiFi.SSID(i);
      if (name.isEmpty()) continue;
      body += F("<option value='"); body += escapeHtml(name); body += F("'>"); body += escapeHtml(name); body += F("</option>");
    }
    WiFi.scanDelete();
    body += F("</select><label>Wi‑Fi-lösenord</label><input type='password' name='wifi_password'><label>Adminlösenord (minst 12 tecken)</label><input type='password' name='admin_password' minlength='12' required><button>Spara och anslut</button></form></section>");
    portal.send(200, "text/html; charset=utf-8", page("Installation", body));
  };
  portal.on("/", HTTP_GET, form);
  portal.on("/generate_204", HTTP_GET, form);
  portal.on("/hotspot-detect.html", HTTP_GET, form);
  portal.on("/save", HTTP_POST, [&]() {
    String wifiName = portal.arg("ssid"), wifiPassword = portal.arg("wifi_password"), password = portal.arg("admin_password");
    if (wifiName.isEmpty() || wifiName.length() > 32 || wifiPassword.length() > 64 || password.length() < 12) {
      portal.send(400, "text/plain; charset=utf-8", "Kontrollera nätverksnamn och lösenord."); return;
    }
    if (!adminHash.isEmpty() && passwordHash(adminSalt, password) != adminHash) {
      portal.send(403, "text/plain; charset=utf-8", "Fel adminlösenord."); return;
    }
    WiFi.begin(wifiName.c_str(), wifiPassword.c_str());
    uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) delay(100);
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      portal.send(400, "text/plain; charset=utf-8", "Kunde inte ansluta till Wi‑Fi. Kontrollera lösenordet och försök igen.");
      return;
    }
    if (adminHash.isEmpty()) {
      adminSalt = randomHex(16); adminHash = passwordHash(adminSalt, password);
      settings.putString("salt", adminSalt); settings.putString("hash", adminHash);
    }
    if (pairingCode.isEmpty()) {
      uint32_t code = 10000000 + (esp_random() % 90000000);
      pairingCode = String(code);
      settings.putString("pair_code", pairingCode);
    }
    homeSpan.setWifiCredentials(wifiName.c_str(), wifiPassword.c_str());
    String body = F("<section><p>Spara denna HomeKit-kod innan du lämnar sidan:</p><h2><code>");
    body += pairingCode;
    body += F("</code></h2><p>ESP32 startar om om några sekunder. Anslut sedan telefonen till ditt vanliga Wi‑Fi.</p></section>");
    portal.send(200, "text/html; charset=utf-8", page("Klart", body));
    delay(5000);
    ESP.restart();
  });
  portal.onNotFound(form);
  portal.begin();
  Serial.printf("Installationsnät: %s, portal: http://192.168.4.1\n", ssid.c_str());
  while (true) { setupDns.processNextRequest(); portal.handleClient(); delay(5); }
}

void setup() {
  Serial.begin(115200);
  settings.begin("yale-bridge", false);
  lockAddress = settings.getString("address", "");
  lockName = settings.getString("name", "");
  offlineKey = settings.getString("offline", "");
  addressType = settings.getUChar("addr_type", 0);
  keyIndex = settings.getUChar("slot", 0);
  adminSalt = settings.getString("salt", "");
  adminHash = settings.getString("hash", "");
  pairingCode = settings.getString("pair_code", "");
  ota.repository = settings.getString("github_repo", "");
  yale.begin();
  homeSpan.setPortNum(8080);
  homeSpan.setHostNameSuffix("");
  homeSpan.setApFunction(setupPortal);
  homeSpan.enableAutoStartAP();
  homeSpan.setWifiCallback(startWeb);
  if (pairingCode.length() == 8) homeSpan.setPairingCode(pairingCode.c_str());
  homeSpan.begin(Category::Locks, "Yale BLE", "yale-ble");
  new SpanAccessory();
  new Service::AccessoryInformation();
  new Characteristic::Identify();
  new Characteristic::Name("Yale BLE");
  new Characteristic::Manufacturer("Yale BLE Bridge");
  new Characteristic::SerialNumber("ESP32-YALE-01");
  new Characteristic::Model("BLE HomeKit Bridge");
  new Characteristic::FirmwareRevision(YALE_FW_VERSION);
  lockService = new YaleLockService();
}

void loop() {
  homeSpan.poll();
  if (webStarted) adminServer.handleClient();
  ota.run(YALE_FW_VERSION);
  if (wifiRestartRequested && millis() >= wifiRestartAt) {
    homeSpan.processSerialCommand("X"); // HomeSpan removes saved Wi-Fi credentials
    ESP.restart();
  }
  if (!WiFi.isConnected() || (lockAddress.isEmpty() && lockName.isEmpty()) || offlineKey.isEmpty()) return;
  static uint32_t nextAttempt = 0, nextPoll = 0;
  if (!yale.connected || !yale.authenticated) {
    if (millis() < nextAttempt) return;
    if (!yale.connect()) Serial.printf("Yale BLE: %s\n", yale.error.c_str());
    nextAttempt = millis() + 15000;
    nextPoll = millis() + 30000;
  } else if (pendingLock >= 0) {
    int desired = pendingLock; pendingLock = -1;
    if (!yale.setLocked(desired == 1)) Serial.printf("Yale operation: %s\n", yale.error.c_str());
    nextPoll = millis() + 30000;
  } else if (millis() >= nextPoll) {
    if (!yale.pollStatus()) { Serial.printf("Yale status: %s\n", yale.error.c_str()); yale.disconnect(); }
    nextPoll = millis() + 30000;
  }
  if (lockService && lockService->current->getVal<int>() != yale.state)
    lockService->current->setVal(yale.state);
  if (lockService && (yale.state == 0 || yale.state == 1) && lockService->target->getVal<int>() != yale.state)
    lockService->target->setVal(yale.state);
}
