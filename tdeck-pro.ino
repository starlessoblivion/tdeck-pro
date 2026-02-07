#include <GxEPD2_BW.h>
#include <Adafruit_TCA8418.h>
#include <WiFi.h>
#include <Preferences.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/Picopixel.h>

// hyn_touch driver (from factory firmware)
extern "C" {
    int hyn_touch_init(void);
    uint8_t hyn_touch_get_point(int16_t *x_array, int16_t *y_array, uint8_t get_point);
}

// --- Pin definitions (T-Deck Pro v1.0) ---

#define BOARD_SPI_SCK   36
#define BOARD_SPI_MOSI  33
#define BOARD_EPD_CS    34
#define BOARD_EPD_DC    35
#define BOARD_EPD_BUSY  37
#define BOARD_EPD_RST   -1
#define BOARD_LORA_CS   3
#define BOARD_LORA_RST  4
#define BOARD_SD_CS     48

#define BOARD_I2C_SDA   13
#define BOARD_I2C_SCL   14
#define BOARD_KB_ADDR   0x34
#define BOARD_KB_LED    42

#define BOARD_LORA_EN   46
#define BOARD_GPS_EN    39
#define BOARD_1V8_EN    38

#define KB_ROWS  4
#define KB_COLS  10

#define DISPLAY_ROTATION 0

// Special key codes
#define KEY_ALT   '\1'
#define KEY_SHIFT '\2'
#define KEY_SYM   '\3'
#define KEY_MIC   '\4'

// Sym key mapping (BB Q10 layout): letter -> number/symbol
char symMap[128];

void initSymMap() {
    memset(symMap, 0, sizeof(symMap));
    // Row 0: # 1 2 3 ( ) _ - + @
    symMap['q'] = '#'; symMap['w'] = '1'; symMap['e'] = '2'; symMap['r'] = '3';
    symMap['t'] = '('; symMap['y'] = ')'; symMap['u'] = '_'; symMap['i'] = '-';
    symMap['o'] = '+'; symMap['p'] = '@';
    // Row 1: * 4 5 6 / : ; ' "
    symMap['a'] = '*'; symMap['s'] = '4'; symMap['d'] = '5'; symMap['f'] = '6';
    symMap['g'] = '/'; symMap['h'] = ':'; symMap['j'] = ';'; symMap['k'] = '\'';
    symMap['l'] = '"';
    // Row 2: 7 8 9 ? | , .
    symMap['z'] = '7'; symMap['x'] = '8'; symMap['c'] = '9'; symMap['v'] = '?';
    symMap['b'] = '|'; symMap['n'] = ','; symMap['m'] = '.';
    // Mic key: SYM+MIC = 0
    symMap[KEY_MIC] = '0';
}

// --- Objects ---

GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(
    GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY)
);

Adafruit_TCA8418 keypad;
Preferences prefs;
bool touchReady = false;

const char keymap[KB_ROWS][KB_COLS] = {
    {'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p'},
    {'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', '\b'},
    {KEY_ALT, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '$', '\n'},
    {' ', ' ', ' ', ' ', ' ', KEY_SYM, ' ', KEY_SHIFT, KEY_MIC, KEY_SHIFT},
};

// --- Layout ---

#define GRID_COLS 3
#define GRID_ROWS 3
#define ICON_COUNT 9

#define TOP_BAR_H  20
#define ICON_SIZE  58
#define LABEL_GAP  4
#define LABEL_H    16

const char* appNames[ICON_COUNT] = {
    "Settings", "", "", "", "", "", "", "", ""
};

const char appShortcuts[ICON_COUNT] = {
    's', 0, 0, 0, 0, 0, 0, 0, 0
};

int selectedApp = 0;

// Screens
enum Screen { SCREEN_HOME, SCREEN_SETTINGS, SCREEN_WIFI_POPUP, SCREEN_WIFI_PASSWORD };
Screen currentScreen = SCREEN_HOME;

bool altMode = false;

// --- Settings state ---

bool touchEnabled = true;

int kbBrightness = 0;
#define KB_BRIGHT_STEPS 5
const int kbPwmValues[KB_BRIGHT_STEPS] = { 0, 64, 128, 192, 255 };

#define KB_AUTO_OFF_STEPS 7
const int kbAutoOffValues[KB_AUTO_OFF_STEPS] = { 2, 4, 8, 16, 32, 64, 128 };
int kbAutoOffIdx = 2;
unsigned long kbLastActivity = 0;
bool kbLightIsOff = false;

bool wifiEnabled = true;
bool btEnabled = true;
bool loraEnabled = true;
bool gpsEnabled = true;
bool gyroEnabled = true;

#define SETTINGS_ITEM_COUNT 8
#define SETTINGS_VISIBLE 7
#define SETTINGS_ITEM_H 34

int settingsSelected = 0;
int settingsScrollOffset = 0;

void applyKbBrightness() {
    analogWrite(BOARD_KB_LED, kbPwmValues[kbBrightness]);
    kbLightIsOff = false;
    kbLastActivity = millis();
}

// --- WiFi popup state ---

#define MAX_WIFI_NETWORKS 12
#define MAX_SAVED_WIFI 5
#define MAX_PASS_LEN 63
#define WIFI_VISIBLE 6

struct WifiNet {
    char ssid[33];
    int rssi;
    bool encrypted;
    bool saved;
};

WifiNet wifiNets[MAX_WIFI_NETWORKS];
int wifiNetCount = 0;
int wifiSelected = 0;
int wifiScrollOffset = 0;

char savedSSID[MAX_SAVED_WIFI][33];
char savedPass[MAX_SAVED_WIFI][64];
int savedWifiCount = 0;

char wifiPassBuf[MAX_PASS_LEN + 1];
int wifiPassLen = 0;
char wifiConnSSID[33];
bool shiftNext = false;
bool symNext = false;
bool micMuted = false;

// --- WiFi credential helpers ---

void loadSavedWifi() {
    prefs.begin("wifi", true);
    savedWifiCount = prefs.getInt("count", 0);
    if (savedWifiCount > MAX_SAVED_WIFI) savedWifiCount = MAX_SAVED_WIFI;
    for (int i = 0; i < savedWifiCount; i++) {
        char sk[8], pk[8];
        snprintf(sk, sizeof(sk), "s%d", i);
        snprintf(pk, sizeof(pk), "p%d", i);
        String s = prefs.getString(sk, "");
        String p = prefs.getString(pk, "");
        strncpy(savedSSID[i], s.c_str(), 32); savedSSID[i][32] = 0;
        strncpy(savedPass[i], p.c_str(), 63); savedPass[i][63] = 0;
    }
    prefs.end();
}

void saveWifiCred(const char* ssid, const char* pass) {
    // Check if already saved, update if so
    for (int i = 0; i < savedWifiCount; i++) {
        if (strcmp(savedSSID[i], ssid) == 0) {
            strncpy(savedPass[i], pass, 63); savedPass[i][63] = 0;
            prefs.begin("wifi", false);
            char pk[8]; snprintf(pk, sizeof(pk), "p%d", i);
            prefs.putString(pk, pass);
            prefs.end();
            return;
        }
    }
    // Add new
    if (savedWifiCount >= MAX_SAVED_WIFI) {
        // Shift out oldest
        for (int i = 0; i < MAX_SAVED_WIFI - 1; i++) {
            strcpy(savedSSID[i], savedSSID[i + 1]);
            strcpy(savedPass[i], savedPass[i + 1]);
        }
        savedWifiCount = MAX_SAVED_WIFI - 1;
    }
    strncpy(savedSSID[savedWifiCount], ssid, 32); savedSSID[savedWifiCount][32] = 0;
    strncpy(savedPass[savedWifiCount], pass, 63); savedPass[savedWifiCount][63] = 0;
    savedWifiCount++;
    // Write all to NVS
    prefs.begin("wifi", false);
    prefs.putInt("count", savedWifiCount);
    for (int i = 0; i < savedWifiCount; i++) {
        char sk[8], pk[8];
        snprintf(sk, sizeof(sk), "s%d", i);
        snprintf(pk, sizeof(pk), "p%d", i);
        prefs.putString(sk, savedSSID[i]);
        prefs.putString(pk, savedPass[i]);
    }
    prefs.end();
}

const char* getSavedPass(const char* ssid) {
    for (int i = 0; i < savedWifiCount; i++) {
        if (strcmp(savedSSID[i], ssid) == 0) return savedPass[i];
    }
    return NULL;
}

// --- Grid layout ---

int gridStartX, gridStartY, gridSpacingX, gridSpacingY;

void computeGrid() {
    display.setRotation(DISPLAY_ROTATION);
    int screenW = display.width();
    int screenH = display.height();
    int cellH = ICON_SIZE + LABEL_GAP + LABEL_H;
    int totalW = GRID_COLS * ICON_SIZE;
    int totalH = GRID_ROWS * cellH;
    gridSpacingX = (screenW - totalW) / (GRID_COLS + 1);
    gridSpacingY = (screenH - TOP_BAR_H - totalH) / (GRID_ROWS + 1);
    gridStartX = gridSpacingX;
    gridStartY = TOP_BAR_H + gridSpacingY;
}

void getIconRect(int idx, int &x, int &y) {
    int col = idx % GRID_COLS;
    int row = idx / GRID_COLS;
    int cellH = ICON_SIZE + LABEL_GAP + LABEL_H;
    x = gridStartX + col * (ICON_SIZE + gridSpacingX);
    y = gridStartY + row * (cellH + gridSpacingY);
}

// --- Icon drawing ---

void drawGear(int cx, int cy, int outerR, int innerR, int holeR, int teeth, uint16_t color) {
    display.fillCircle(cx, cy, outerR, color);
    for (int i = 0; i < teeth; i++) {
        float angle = i * 2.0 * PI / teeth;
        int tx = cx + cos(angle) * (outerR + 2);
        int ty = cy + sin(angle) * (outerR + 2);
        display.fillRect(tx - 3, ty - 3, 6, 6, color);
    }
    display.fillCircle(cx, cy, innerR, (color == GxEPD_BLACK) ? GxEPD_WHITE : GxEPD_BLACK);
    display.fillCircle(cx, cy, holeR, color);
}

void drawAppIcon(int idx, int x, int y, bool selected) {
    uint16_t bg = selected ? GxEPD_BLACK : GxEPD_WHITE;
    uint16_t fg = selected ? GxEPD_WHITE : GxEPD_BLACK;
    if (selected) {
        display.fillRoundRect(x, y, ICON_SIZE, ICON_SIZE, 8, GxEPD_BLACK);
    } else {
        display.drawRoundRect(x, y, ICON_SIZE, ICON_SIZE, 8, GxEPD_BLACK);
    }
    int cx = x + ICON_SIZE / 2;
    int cy = y + ICON_SIZE / 2;
    if (idx == 0) {
        drawGear(cx, cy, 14, 10, 4, 8, fg);
        if (selected) {
            display.fillCircle(cx, cy, 10, bg);
            display.fillCircle(cx, cy, 4, fg);
        }
    }
    const char* name = appNames[idx];
    if (name[0] != '\0') {
        display.setFont(&FreeSans9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds(name, 0, 0, &tbx, &tby, &tbw, &tbh);
        int lx = x + (ICON_SIZE - tbw) / 2 - tbx;
        int ly = y + ICON_SIZE + LABEL_GAP + tbh + 1;
        display.setCursor(lx, ly);
        display.print(name);
        if (altMode && appShortcuts[idx] != 0) {
            char first[2] = { name[0], '\0' };
            int16_t fbx, fby; uint16_t fbw, fbh;
            display.getTextBounds(first, 0, 0, &fbx, &fby, &fbw, &fbh);
            display.drawLine(lx, ly + 2, lx + fbw, ly + 2, GxEPD_BLACK);
            display.drawLine(lx, ly + 3, lx + fbw, ly + 3, GxEPD_BLACK);
        }
    }
}

// --- Drawing helpers ---

void drawToggle(int x, int y, bool on, uint16_t fg) {
    int w = 40, h = 22;
    display.drawRoundRect(x, y, w, h, h / 2, fg);
    if (on) {
        display.fillCircle(x + w - h / 2, y + h / 2, 9, fg);
    } else {
        display.fillCircle(x + h / 2, y + h / 2, 9, fg);
    }
}

void drawSelector(const char* value, int textY, bool hasLeft, bool hasRight, uint16_t fg) {
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.setFont(&FreeSansBold9pt7b);
    display.setTextColor(fg);
    display.getTextBounds(value, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(display.width() - 60 - tbw / 2 - tbx, textY);
    display.print(value);
    display.setFont(&FreeSans9pt7b);
    if (hasLeft) { display.setCursor(display.width() - 96, textY); display.print("<"); }
    if (hasRight) { display.setCursor(display.width() - 26, textY); display.print(">"); }
}

// --- Screens ---

void drawHomeScreen() {
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawLine(0, TOP_BAR_H - 1, display.width() - 1, TOP_BAR_H - 1, GxEPD_BLACK);
        display.setFont(&Picopixel);
        display.setTextColor(GxEPD_BLACK);
        if (altMode) {
            display.setCursor(4, 12);
            display.print("ALT");
        }
        if (micMuted) {
            display.setCursor(altMode ? 24 : 4, 12);
            display.print("MUTE");
        }
        for (int i = 0; i < ICON_COUNT; i++) {
            int x, y;
            getIconRect(i, x, y);
            drawAppIcon(i, x, y, i == selectedApp);
        }
    } while (display.nextPage());
}

void drawSettingsScreen() {
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();

    // Adjust scroll so selected is visible
    if (settingsSelected < settingsScrollOffset) settingsScrollOffset = settingsSelected;
    if (settingsSelected >= settingsScrollOffset + SETTINGS_VISIBLE) settingsScrollOffset = settingsSelected - SETTINGS_VISIBLE + 1;

    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Title bar
        display.drawLine(0, TOP_BAR_H - 1, display.width() - 1, TOP_BAR_H - 1, GxEPD_BLACK);
        display.setFont(&Picopixel);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(4, 12);
        display.print("< BACK");

        display.setFont(&FreeSansBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds("Settings", 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((display.width() - tbw) / 2 - tbx, TOP_BAR_H + 24);
        display.print("Settings");

        int contentY = TOP_BAR_H + 38;
        int itemX = 12;

        // Scroll indicators
        if (settingsScrollOffset > 0) {
            display.setFont(&Picopixel);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(display.width() / 2 - 4, contentY - 2);
            display.print("...");
        }

        for (int v = 0; v < SETTINGS_VISIBLE; v++) {
            int i = v + settingsScrollOffset;
            if (i >= SETTINGS_ITEM_COUNT) break;

            int itemY = contentY + v * SETTINGS_ITEM_H;
            int textY = itemY + 22;
            bool hl = (i == settingsSelected);

            if (hl) display.fillRoundRect(4, itemY, display.width() - 8, SETTINGS_ITEM_H, 4, GxEPD_BLACK);

            uint16_t fg = hl ? GxEPD_WHITE : GxEPD_BLACK;

            display.setFont(&FreeSans9pt7b);
            display.setTextColor(fg);
            display.setCursor(itemX, textY);

            int toggleX = display.width() - 60;
            int toggleY = itemY + (SETTINGS_ITEM_H - 22) / 2;

            switch (i) {
                case 0: // Touchscreen
                    display.print("Touchscreen");
                    drawToggle(toggleX, toggleY, touchEnabled, fg);
                    break;
                case 1: { // KB Light
                    display.print("KB Light");
                    const char* kbLabels[] = { "Off", "25%", "50%", "75%", "100%" };
                    drawSelector(kbLabels[kbBrightness], textY,
                        kbBrightness > 0, kbBrightness < KB_BRIGHT_STEPS - 1, fg);
                    break;
                }
                case 2: { // KB Auto Off
                    display.print("KB Auto Off");
                    char buf[8]; snprintf(buf, sizeof(buf), "%ds", kbAutoOffValues[kbAutoOffIdx]);
                    drawSelector(buf, textY,
                        kbAutoOffIdx > 0, kbAutoOffIdx < KB_AUTO_OFF_STEPS - 1, fg);
                    break;
                }
                case 3: // WiFi
                    display.print("WiFi");
                    drawToggle(toggleX, toggleY, wifiEnabled, fg);
                    { // Enter arrow
                        int ax = display.width() - 14;
                        int ay = itemY + SETTINGS_ITEM_H / 2;
                        display.fillTriangle(ax, ay - 5, ax, ay + 5, ax + 7, ay, fg);
                    }
                    break;
                case 4: // Bluetooth
                    display.print("Bluetooth");
                    drawToggle(toggleX, toggleY, btEnabled, fg);
                    break;
                case 5: // LoRa
                    display.print("LoRa");
                    drawToggle(toggleX, toggleY, loraEnabled, fg);
                    break;
                case 6: // GPS
                    display.print("GPS");
                    drawToggle(toggleX, toggleY, gpsEnabled, fg);
                    break;
                case 7: // Gyroscope
                    display.print("Gyroscope");
                    drawToggle(toggleX, toggleY, gyroEnabled, fg);
                    break;
            }
        }

        // Bottom scroll indicator
        if (settingsScrollOffset + SETTINGS_VISIBLE < SETTINGS_ITEM_COUNT) {
            display.setFont(&Picopixel);
            display.setTextColor(GxEPD_BLACK);
            int bottomY = contentY + SETTINGS_VISIBLE * SETTINGS_ITEM_H + 4;
            display.setCursor(display.width() / 2 - 4, bottomY);
            display.print("...");
        }

    } while (display.nextPage());
}

void scanAndShowWifi() {
    // Show scanning message
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawLine(0, TOP_BAR_H - 1, display.width() - 1, TOP_BAR_H - 1, GxEPD_BLACK);
        display.setFont(&FreeSansBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds("Scanning...", 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((display.width() - tbw) / 2 - tbx, display.height() / 2);
        display.print("Scanning...");
    } while (display.nextPage());

    // Scan
    if (!wifiEnabled) { WiFi.mode(WIFI_STA); wifiEnabled = true; }
    int n = WiFi.scanNetworks();
    wifiNetCount = 0;
    for (int i = 0; i < n && wifiNetCount < MAX_WIFI_NETWORKS; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;
        // Skip duplicates
        bool dup = false;
        for (int j = 0; j < wifiNetCount; j++) {
            if (strcmp(wifiNets[j].ssid, ssid.c_str()) == 0) { dup = true; break; }
        }
        if (dup) continue;
        strncpy(wifiNets[wifiNetCount].ssid, ssid.c_str(), 32);
        wifiNets[wifiNetCount].ssid[32] = 0;
        wifiNets[wifiNetCount].rssi = WiFi.RSSI(i);
        wifiNets[wifiNetCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        wifiNets[wifiNetCount].saved = (getSavedPass(wifiNets[wifiNetCount].ssid) != NULL);
        wifiNetCount++;
    }
    WiFi.scanDelete();
    wifiSelected = 0;
    wifiScrollOffset = 0;
}

void drawWifiPopup() {
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();

    if (wifiSelected < wifiScrollOffset) wifiScrollOffset = wifiSelected;
    if (wifiSelected >= wifiScrollOffset + WIFI_VISIBLE) wifiScrollOffset = wifiSelected - WIFI_VISIBLE + 1;

    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Top bar
        display.drawLine(0, TOP_BAR_H - 1, display.width() - 1, TOP_BAR_H - 1, GxEPD_BLACK);
        display.setFont(&Picopixel);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(4, 12);
        display.print("< BACK");

        // Title
        display.setFont(&FreeSansBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds("WiFi Networks", 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((display.width() - tbw) / 2 - tbx, TOP_BAR_H + 24);
        display.print("WiFi Networks");

        if (wifiNetCount == 0) {
            display.setFont(&FreeSans9pt7b);
            display.setCursor(16, TOP_BAR_H + 60);
            display.print("No networks found");
        }

        int listY = TOP_BAR_H + 38;
        int itemH = 34;

        for (int v = 0; v < WIFI_VISIBLE; v++) {
            int i = v + wifiScrollOffset;
            if (i >= wifiNetCount) break;

            int y = listY + v * itemH;
            bool hl = (i == wifiSelected);

            if (hl) display.fillRoundRect(4, y, display.width() - 8, itemH, 4, GxEPD_BLACK);

            uint16_t fg = hl ? GxEPD_WHITE : GxEPD_BLACK;

            display.setFont(&FreeSans9pt7b);
            display.setTextColor(fg);
            display.setCursor(12, y + 22);
            display.print(wifiNets[i].ssid);

            // Signal bars (right side)
            int barX = display.width() - 40;
            int barY = y + 8;
            int bars = 1;
            if (wifiNets[i].rssi > -60) bars = 4;
            else if (wifiNets[i].rssi > -70) bars = 3;
            else if (wifiNets[i].rssi > -80) bars = 2;
            for (int b = 0; b < 4; b++) {
                int bh = 4 + b * 4;
                int bx = barX + b * 6;
                int by = barY + 16 - bh;
                if (b < bars) display.fillRect(bx, by, 4, bh, fg);
                else display.drawRect(bx, by, 4, bh, fg);
            }

            // Saved indicator
            if (wifiNets[i].saved) {
                display.setFont(&Picopixel);
                display.setTextColor(fg);
                display.setCursor(display.width() - 70, y + 12);
                display.print("SAVED");
            }

            // Lock indicator
            if (wifiNets[i].encrypted) {
                display.setFont(&Picopixel);
                display.setTextColor(fg);
                display.setCursor(display.width() - 70, y + 28);
                display.print("LOCK");
            }
        }

        // Scroll indicators
        if (wifiScrollOffset > 0) {
            display.setFont(&Picopixel);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(display.width() / 2 - 4, listY - 2);
            display.print("...");
        }
        if (wifiScrollOffset + WIFI_VISIBLE < wifiNetCount) {
            display.setFont(&Picopixel);
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(display.width() / 2 - 4, listY + WIFI_VISIBLE * itemH + 4);
            display.print("...");
        }

    } while (display.nextPage());
}

void drawWifiPassword() {
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Top bar
        display.drawLine(0, TOP_BAR_H - 1, display.width() - 1, TOP_BAR_H - 1, GxEPD_BLACK);
        display.setFont(&Picopixel);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(4, 12);
        display.print("< CANCEL");

        // Network name
        display.setFont(&FreeSansBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds(wifiConnSSID, 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((display.width() - tbw) / 2 - tbx, TOP_BAR_H + 30);
        display.print(wifiConnSSID);

        // "Password:" label
        display.setFont(&FreeSans9pt7b);
        display.setCursor(16, TOP_BAR_H + 65);
        display.print("Password:");

        // Password input box
        int boxX = 12, boxY = TOP_BAR_H + 78;
        int boxW = display.width() - 24, boxH = 30;
        display.drawRoundRect(boxX, boxY, boxW, boxH, 4, GxEPD_BLACK);

        // Show password text
        display.setFont(&FreeSans9pt7b);
        display.setCursor(boxX + 6, boxY + 21);
        // Show last 20 chars if password is longer
        int startChar = 0;
        if (wifiPassLen > 20) startChar = wifiPassLen - 20;
        display.print(&wifiPassBuf[startChar]);

        // Cursor
        display.getTextBounds(&wifiPassBuf[startChar], 0, 0, &tbx, &tby, &tbw, &tbh);
        display.fillRect(boxX + 6 + tbw + 2, boxY + 6, 2, boxH - 12, GxEPD_BLACK);

        // Hint
        if (shiftNext) {
            display.setFont(&Picopixel);
            display.setCursor(16, TOP_BAR_H + 130);
            display.print("SHIFT active");
        } else if (symNext) {
            display.setFont(&Picopixel);
            display.setCursor(16, TOP_BAR_H + 130);
            display.print("SYM active");
        }

        display.setFont(&Picopixel);
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(16, display.height() - 10);
        display.print("ENTER=connect  ALT+BKSP=back");

    } while (display.nextPage());
}

void drawConnecting(const char* ssid) {
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawLine(0, TOP_BAR_H - 1, display.width() - 1, TOP_BAR_H - 1, GxEPD_BLACK);
        display.setFont(&FreeSansBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds("Connecting...", 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((display.width() - tbw) / 2 - tbx, display.height() / 2 - 10);
        display.print("Connecting...");
        display.setFont(&FreeSans9pt7b);
        display.getTextBounds(ssid, 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((display.width() - tbw) / 2 - tbx, display.height() / 2 + 20);
        display.print(ssid);
    } while (display.nextPage());
}

void drawWifiResult(bool success) {
    display.setRotation(DISPLAY_ROTATION);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.drawLine(0, TOP_BAR_H - 1, display.width() - 1, TOP_BAR_H - 1, GxEPD_BLACK);
        display.setFont(&FreeSansBold9pt7b);
        display.setTextColor(GxEPD_BLACK);
        int16_t tbx, tby; uint16_t tbw, tbh;
        const char* msg = success ? "Connected!" : "Failed";
        display.getTextBounds(msg, 0, 0, &tbx, &tby, &tbw, &tbh);
        display.setCursor((display.width() - tbw) / 2 - tbx, display.height() / 2);
        display.print(msg);
    } while (display.nextPage());
}

void attemptWifiConnect(const char* ssid, const char* pass) {
    drawConnecting(ssid);
    WiFi.begin(ssid, pass);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
        delay(200);
    }
    bool ok = (WiFi.status() == WL_CONNECTED);
    if (ok) saveWifiCred(ssid, pass);
    drawWifiResult(ok);
    delay(1500);
    // Go back to settings
    currentScreen = SCREEN_SETTINGS;
    drawSettingsScreen();
}

void openApp(int index) {
    if (index == 0) {
        currentScreen = SCREEN_SETTINGS;
        settingsSelected = 0;
        settingsScrollOffset = 0;
        delay(150);
        drawSettingsScreen();
    }
}

// --- Setup ---

void setup() {
    Serial.begin(115200);
    initSymMap();

    pinMode(BOARD_LORA_CS, OUTPUT);  digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_LORA_RST, OUTPUT); digitalWrite(BOARD_LORA_RST, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT);   digitalWrite(BOARD_EPD_CS, HIGH);
    pinMode(BOARD_KB_LED, OUTPUT);   applyKbBrightness();

    pinMode(BOARD_LORA_EN, OUTPUT);  digitalWrite(BOARD_LORA_EN, HIGH);
    pinMode(BOARD_GPS_EN, OUTPUT);   digitalWrite(BOARD_GPS_EN, HIGH);
    pinMode(BOARD_1V8_EN, OUTPUT);   digitalWrite(BOARD_1V8_EN, HIGH);

    SPI.begin(BOARD_SPI_SCK, -1, BOARD_SPI_MOSI, BOARD_EPD_CS);
    display.init(115200, true, 2, false);

    computeGrid();

    // I2C + keyboard
    Wire.begin(BOARD_I2C_SDA, BOARD_I2C_SCL);
    if (!keypad.begin(BOARD_KB_ADDR, &Wire)) {
        Serial.println("Keyboard not found!");
    } else {
        keypad.matrix(KB_ROWS, KB_COLS);
        keypad.flush();
        Serial.println("Keyboard ready");
    }

    // Touch
    touchReady = hyn_touch_init();
    Serial.printf("Touch: %s\n", touchReady ? "ready" : "not found");

    // WiFi
    WiFi.mode(WIFI_STA);
    loadSavedWifi();

    drawHomeScreen();
}

// --- Loop ---

void handleKeyboard() {
    if (keypad.available() <= 0) return;

    int k = keypad.getEvent();
    if (!(k & 0x80)) return;

    k = (k & 0x7F) - 1;
    int row = k / KB_COLS;
    int col = (KB_COLS - 1) - k % KB_COLS;
    if (row < 0 || row >= KB_ROWS || col < 0 || col >= KB_COLS) return;

    char c = keymap[row][col];
    if (c == 0) return;

    // KB auto-off wake
    kbLastActivity = millis();
    if (kbLightIsOff && kbBrightness > 0) {
        analogWrite(BOARD_KB_LED, kbPwmValues[kbBrightness]);
        kbLightIsOff = false;
    }

    // --- Global modifier keys ---
    if (c == KEY_SHIFT) {
        shiftNext = !shiftNext;
        symNext = false;
        if (currentScreen == SCREEN_WIFI_PASSWORD) { delay(100); drawWifiPassword(); }
        return;
    }
    if (c == KEY_SYM) {
        symNext = !symNext;
        shiftNext = false;
        if (currentScreen == SCREEN_WIFI_PASSWORD) { delay(100); drawWifiPassword(); }
        return;
    }

    // Apply SYM/SHIFT to get final character
    if (symNext && c >= 0 && c < 128 && symMap[(int)c]) {
        c = symMap[(int)c];
        symNext = false;
    } else if (shiftNext && c >= 'a' && c <= 'z') {
        c = c - 'a' + 'A';
        shiftNext = false;
    }

    // --- Global MIC key (mute toggle) ---
    if (c == KEY_MIC) {
        micMuted = !micMuted;
        shiftNext = false;
        if (currentScreen == SCREEN_HOME) { delay(100); drawHomeScreen(); }
        return;
    }

    // --- HOME ---
    if (currentScreen == SCREEN_HOME) {
        if (c == KEY_ALT) {
            altMode = !altMode;
            delay(150); drawHomeScreen(); return;
        }
        if (altMode) {
            for (int i = 0; i < ICON_COUNT; i++) {
                if (appShortcuts[i] == c) {
                    altMode = false; selectedApp = i;
                    openApp(i);
                    if (currentScreen == SCREEN_HOME) drawHomeScreen();
                    return;
                }
            }
            if (c == '\b') { altMode = false; delay(150); drawHomeScreen(); }
            return;
        }
        int prev = selectedApp;
        switch (c) {
            case 'w': if (selectedApp >= GRID_COLS) selectedApp -= GRID_COLS; break;
            case 's': if (selectedApp + GRID_COLS < ICON_COUNT) selectedApp += GRID_COLS; break;
            case 'a': if (selectedApp % GRID_COLS > 0) selectedApp--; break;
            case 'd': if (selectedApp % GRID_COLS < GRID_COLS - 1) selectedApp++; break;
            case '\n': openApp(selectedApp); return;
        }
        if (selectedApp != prev) { delay(150); drawHomeScreen(); }

    // --- SETTINGS ---
    } else if (currentScreen == SCREEN_SETTINGS) {
        if (c == 'w') {
            if (settingsSelected > 0) { settingsSelected--; delay(150); drawSettingsScreen(); }
        } else if (c == 's') {
            if (settingsSelected < SETTINGS_ITEM_COUNT - 1) { settingsSelected++; delay(150); drawSettingsScreen(); }
        } else if (c == 'd') {
            bool changed = false;
            switch (settingsSelected) {
                case 0: if (!touchEnabled) { touchEnabled = true; changed = true; } break;
                case 1: if (kbBrightness < KB_BRIGHT_STEPS - 1) { kbBrightness++; applyKbBrightness(); changed = true; } break;
                case 2: if (kbAutoOffIdx < KB_AUTO_OFF_STEPS - 1) { kbAutoOffIdx++; changed = true; } break;
                case 3: if (!wifiEnabled) { WiFi.mode(WIFI_STA); wifiEnabled = true; changed = true; } break;
                case 4: if (!btEnabled) { btStart(); btEnabled = true; changed = true; } break;
                case 5: if (!loraEnabled) { digitalWrite(BOARD_LORA_EN, HIGH); loraEnabled = true; changed = true; } break;
                case 6: if (!gpsEnabled) { digitalWrite(BOARD_GPS_EN, HIGH); gpsEnabled = true; changed = true; } break;
                case 7: if (!gyroEnabled) { digitalWrite(BOARD_1V8_EN, HIGH); gyroEnabled = true; changed = true; } break;
            }
            if (changed) { delay(150); drawSettingsScreen(); }
        } else if (c == 'a') {
            bool changed = false;
            switch (settingsSelected) {
                case 0: if (touchEnabled) { touchEnabled = false; changed = true; } break;
                case 1: if (kbBrightness > 0) { kbBrightness--; applyKbBrightness(); changed = true; } break;
                case 2: if (kbAutoOffIdx > 0) { kbAutoOffIdx--; changed = true; } break;
                case 3: if (wifiEnabled) { WiFi.mode(WIFI_OFF); wifiEnabled = false; changed = true; } break;
                case 4: if (btEnabled) { btStop(); btEnabled = false; changed = true; } break;
                case 5: if (loraEnabled) { digitalWrite(BOARD_LORA_EN, LOW); loraEnabled = false; changed = true; } break;
                case 6: if (gpsEnabled) { digitalWrite(BOARD_GPS_EN, LOW); gpsEnabled = false; changed = true; } break;
                case 7: if (gyroEnabled) { digitalWrite(BOARD_1V8_EN, LOW); gyroEnabled = false; changed = true; } break;
            }
            if (changed) { delay(150); drawSettingsScreen(); }
        } else if (c == '\n') {
            switch (settingsSelected) {
                case 0: touchEnabled = !touchEnabled; delay(150); drawSettingsScreen(); break;
                case 3:
                    // WiFi: Enter opens popup
                    currentScreen = SCREEN_WIFI_POPUP;
                    scanAndShowWifi();
                    drawWifiPopup();
                    break;
                case 4: btEnabled ? btStop() : btStart(); btEnabled = !btEnabled; delay(150); drawSettingsScreen(); break;
                case 5: loraEnabled = !loraEnabled; digitalWrite(BOARD_LORA_EN, loraEnabled); delay(150); drawSettingsScreen(); break;
                case 6: gpsEnabled = !gpsEnabled; digitalWrite(BOARD_GPS_EN, gpsEnabled); delay(150); drawSettingsScreen(); break;
                case 7: gyroEnabled = !gyroEnabled; digitalWrite(BOARD_1V8_EN, gyroEnabled); delay(150); drawSettingsScreen(); break;
            }
        } else if (c == '\b') {
            currentScreen = SCREEN_HOME;
            settingsSelected = 0; settingsScrollOffset = 0;
            delay(150); drawHomeScreen();
        }

    // --- WIFI POPUP ---
    } else if (currentScreen == SCREEN_WIFI_POPUP) {
        if (c == 'w') {
            if (wifiSelected > 0) { wifiSelected--; delay(150); drawWifiPopup(); }
        } else if (c == 's') {
            if (wifiSelected < wifiNetCount - 1) { wifiSelected++; delay(150); drawWifiPopup(); }
        } else if (c == '\n' && wifiNetCount > 0) {
            strncpy(wifiConnSSID, wifiNets[wifiSelected].ssid, 32); wifiConnSSID[32] = 0;
            const char* sp = getSavedPass(wifiConnSSID);
            if (sp) {
                // Connect with saved password
                attemptWifiConnect(wifiConnSSID, sp);
            } else if (!wifiNets[wifiSelected].encrypted) {
                // Open network
                attemptWifiConnect(wifiConnSSID, "");
            } else {
                // Need password
                wifiPassLen = 0; wifiPassBuf[0] = 0;
                shiftNext = false; symNext = false;
                currentScreen = SCREEN_WIFI_PASSWORD;
                drawWifiPassword();
            }
        } else if (c == '\b') {
            currentScreen = SCREEN_SETTINGS;
            delay(150); drawSettingsScreen();
        } else if (c == 'r') {
            // Rescan
            scanAndShowWifi();
            drawWifiPopup();
        }

    // --- WIFI PASSWORD ---
    } else if (currentScreen == SCREEN_WIFI_PASSWORD) {
        if (c == KEY_ALT) {
            altMode = true;
        } else if (c == '\b') {
            if (altMode) {
                altMode = false;
                currentScreen = SCREEN_WIFI_POPUP;
                delay(150); drawWifiPopup();
            } else if (wifiPassLen > 0) {
                wifiPassLen--; wifiPassBuf[wifiPassLen] = 0;
                delay(100); drawWifiPassword();
            }
        } else if (c == '\n') {
            altMode = false;
            if (wifiPassLen > 0) {
                attemptWifiConnect(wifiConnSSID, wifiPassBuf);
            }
        } else if (c >= ' ' && wifiPassLen < MAX_PASS_LEN) {
            altMode = false;
            wifiPassBuf[wifiPassLen++] = c;
            wifiPassBuf[wifiPassLen] = 0;
            delay(100); drawWifiPassword();
        }
    }
}

void handleTouch() {
    if (!touchReady || !touchEnabled) return;

    int16_t tx, ty;
    uint8_t touched = hyn_touch_get_point(&tx, &ty, 1);
    if (!touched) return;

    static unsigned long lastTouch = 0;
    if (millis() - lastTouch < 300) return;
    lastTouch = millis();

    if (currentScreen == SCREEN_HOME) {
        for (int i = 0; i < ICON_COUNT; i++) {
            int ix, iy;
            getIconRect(i, ix, iy);
            if (tx >= ix && tx <= ix + ICON_SIZE && ty >= iy && ty <= iy + ICON_SIZE) {
                selectedApp = i;
                openApp(i);
                if (currentScreen == SCREEN_HOME) drawHomeScreen();
                return;
            }
        }
    }
}

void loop() {
    handleKeyboard();
    handleTouch();

    // KB backlight auto-off
    if (kbBrightness > 0 && !kbLightIsOff) {
        unsigned long timeout = (unsigned long)kbAutoOffValues[kbAutoOffIdx] * 1000UL;
        if (millis() - kbLastActivity > timeout) {
            analogWrite(BOARD_KB_LED, 0);
            kbLightIsOff = true;
        }
    }
}
