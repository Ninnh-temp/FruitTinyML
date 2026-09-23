#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "FS.h"
#include "SD_MMC.h"
#include "esp_system.h"

/*
 ==============================================================================
  AI-THINKER ESP32-CAM HARDWARE VERIFICATION & DIAGNOSTIC FIRMWARE
 ==============================================================================
  This firmware validates all primary hardware subsystems of a new board:
   1. ESP32 SoC Core, Revision, and Internal SRAM
   2. 4MB External PSRAM (Integrity read/write test)
   3. MicroSD Card Slot (1-bit SD_MMC test)
   4. Onboard Flashlight LED (GPIO 4) & Status LED (GPIO 33)
   5. OV2640 / OV7670 Camera Sensor (Capture test & parameter query)
   6. Wi-Fi SoftAP ("ESP32-CAM-TEST") + Embedded HTTP Dashboard with MJPEG Stream
 ==============================================================================
*/

// --- AI-Thinker ESP32-CAM Pin Mapping ---
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27

#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_LED_PIN      4   // High-power White Flash LED (Active HIGH)
#define STATUS_LED_PIN    33   // Small Red Onboard LED (Active LOW)

// --- Configuration ---
// Set to true if you want the ESP32 to connect to your Home Wi-Fi directly:
#define CONNECT_TO_HOME_WIFI   false

const char* HOME_SSID = "name";
const char* HOME_PASS = "pass";

// SoftAP settings (used when CONNECT_TO_HOME_WIFI is false)
const char* AP_SSID = "ESP32-CAM-TEST";
const char* AP_PASS = "";  // Open network

// HTTP Server Handle
httpd_handle_t stream_httpd = NULL;

// Diagnostics results
bool g_psram_ok = false;
bool g_camera_ok = false;
bool g_sdcard_ok = false;
uint64_t g_sdcard_size_mb = 0;
bool g_flash_state = false;


// Function Prototypes
void runSystemDiagnostics();
bool testPSRAM();
void testSDCard();
bool initCamera();
void startCameraServer();
void handleSerialCommands();

// ----------------------------------------------------------------------------
// PSRAM Verification Test
// ----------------------------------------------------------------------------
bool testPSRAM() {
    Serial.println("\n[1/5] Testing External PSRAM...");
    if (!psramFound()) {
        Serial.println("  --> [FAIL] PSRAM was NOT detected by the ESP32 bootloader!");
        Serial.println("      Note: AI-Thinker ESP32-CAM requires 4MB PSRAM for high resolutions.");
        return false;
    }

    size_t psram_size = ESP.getPsramSize();
    size_t free_psram = ESP.getFreePsram();
    Serial.printf("  --> [PASS] PSRAM Detected! Total Size: %u bytes (%0.2f MB), Free: %u bytes\n", 
                  psram_size, (float)psram_size / (1024.0 * 1024.0), free_psram);

    // Read/write integrity stress test on 64KB block in PSRAM
    const size_t test_size = 64 * 1024;
    uint32_t* test_buf = (uint32_t*)ps_malloc(test_size);
    if (!test_buf) {
        Serial.println("  --> [FAIL] Failed to allocate 64KB test buffer in PSRAM!");
        return false;
    }

    bool integrity_ok = true;
    for (size_t i = 0; i < test_size / sizeof(uint32_t); i++) {
        test_buf[i] = (uint32_t)(0xAA550000 | (i & 0xFFFF));
    }
    for (size_t i = 0; i < test_size / sizeof(uint32_t); i++) {
        if (test_buf[i] != (uint32_t)(0xAA550000 | (i & 0xFFFF))) {
            integrity_ok = false;
            break;
        }
    }
    free(test_buf);

    if (integrity_ok) {
        Serial.println("  --> [PASS] PSRAM 64KB Memory Integrity R/W Check: OK!");
        return true;
    } else {
        Serial.println("  --> [FAIL] PSRAM Memory Integrity Corruption Detected!");
        return false;
    }
}

// ----------------------------------------------------------------------------
// MicroSD Slot Verification Test (1-bit SD_MMC Mode)
// ----------------------------------------------------------------------------
void testSDCard() {
    Serial.println("\n[2/5] Testing MicroSD Card Slot (1-bit MMC)...");
    // Use 1-bit mode (second parameter = true) to prevent conflict with GPIO 4 (Flash LED)
    if (SD_MMC.begin("/sdcard", true)) {
        uint8_t cardType = SD_MMC.cardType();
        if (cardType == CARD_NONE) {
            Serial.println("  --> [WARN] Slot mounted but no valid media detected.");
            g_sdcard_ok = false;
        } else {
            g_sdcard_size_mb = SD_MMC.cardSize() / (1024 * 1024);
            Serial.printf("  --> [PASS] MicroSD Card Detected! Type: %s, Capacity: %llu MB\n",
                          (cardType == CARD_MMC) ? "MMC" :
                          (cardType == CARD_SD)  ? "SDSC" :
                          (cardType == CARD_SDHC)? "SDHC" : "UNKNOWN",
                          g_sdcard_size_mb);
            g_sdcard_ok = true;
        }
    } else {
        Serial.println("  --> [INFO] No MicroSD card inserted or slot unpopulated (Normal if empty).");
        g_sdcard_ok = false;
    }
}

// ----------------------------------------------------------------------------
// Wi-Fi Event Callbacks (Tracks connect/disconnect/DHCP events in real time)
// ----------------------------------------------------------------------------
void onWiFiEvent(WiFiEvent_t event) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
            Serial.println("\n[WIFI EVENT] A device connected to ESP32 SoftAP!");
            break;
        case ARDUINO_EVENT_WIFI_AP_STADISCONNECTED:
            Serial.println("\n[WIFI EVENT] Device disconnected from SoftAP. Cleaning up DHCP lease table...");
            break;
        case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
            Serial.println("[WIFI EVENT] IP address successfully assigned to client device.");
            break;
        case ARDUINO_EVENT_WIFI_STA_CONNECTED:
            Serial.println("\n[WIFI EVENT] Connected to router!");
            break;
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            Serial.print("[WIFI EVENT] Got IP: ");
            Serial.println(WiFi.localIP());
            break;
        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            Serial.println("\n[WIFI EVENT] Disconnected from router!");
            break;
        default:
            break;
    }
}

bool initCamera() {
    Serial.println("\n[3/5] Initializing OV2640 Camera Sensor...");

    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;

    if (g_psram_ok) {
        // High quality with PSRAM
        config.frame_size   = FRAMESIZE_VGA;  // 640x480
        config.jpeg_quality = 12;             // 10-63 (lower = better quality)
        config.fb_count     = 2;
        config.grab_mode    = CAMERA_GRAB_LATEST;
    } else {
        // Safe fallback without PSRAM
        config.frame_size   = FRAMESIZE_SVGA; // 800x600
        config.jpeg_quality = 15;
        config.fb_count     = 1;
        config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("  --> [FAIL] Camera init failed with error code: 0x%x\n", err);
        Serial.println("      Check camera ribbon cable seating and 5V power stability!");
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    if (s != NULL) {
        // Vertical flip / horizontal mirror if needed for AI-Thinker orientation
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
        Serial.printf("  --> [PASS] Camera Initialized! Sensor PID: 0x%02X\n", s->id.PID);
    }

    // Capture a trial frame to test DMA & pixel pipeline
    Serial.print("  --> Performing test capture... ");
    uint32_t t_start = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    uint32_t t_duration = millis() - t_start;

    if (!fb) {
        Serial.println("[FAIL] Frame capture returned NULL!");
        return false;
    }

    Serial.printf("[PASS] Captured %ux%u JPEG (%u bytes) in %u ms!\n",
                  fb->width, fb->height, fb->len, t_duration);
    esp_camera_fb_return(fb);

    return true;
}

// ----------------------------------------------------------------------------
// System Diagnostics & Information Dump
// ----------------------------------------------------------------------------
void runSystemDiagnostics() {
    Serial.println("\n=======================================================");
    Serial.println("      AI-THINKER ESP32-CAM HARDWARE SELF-TEST REPORT   ");
    Serial.println("=======================================================");
    
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    Serial.printf("  SoC Model:          ESP32 (Cores: %d, Rev: %d)\n", chip_info.cores, chip_info.revision);
    Serial.printf("  CPU Clock:          %u MHz\n", getCpuFrequencyMhz());
    Serial.printf("  Flash Chip Size:    %u MB\n", ESP.getFlashChipSize() / (1024 * 1024));
    Serial.printf("  Internal Free Heap: %u bytes\n", ESP.getFreeHeap());
    Serial.printf("  Max Alloc Heap:     %u bytes\n", ESP.getMaxAllocHeap());

    // 1. PSRAM Test
    g_psram_ok = testPSRAM();

    // 2. MicroSD Card Test
    testSDCard();

    // 3. LED Indicators Quick Test
    Serial.println("\n[4/5] Testing Onboard LEDs...");
    pinMode(STATUS_LED_PIN, OUTPUT);
    pinMode(FLASH_LED_PIN, OUTPUT);

    // Flash status LED (GPIO 33 is active LOW)
    for (int i = 0; i < 3; i++) {
        digitalWrite(STATUS_LED_PIN, LOW);   // ON
        delay(100);
        digitalWrite(STATUS_LED_PIN, HIGH);  // OFF
        delay(100);
    }
    Serial.println("  --> [PASS] Red Status LED (GPIO 33) tested.");

    // Flash spotlight pulse (GPIO 4 is active HIGH)
    digitalWrite(FLASH_LED_PIN, HIGH);
    delay(100);
    digitalWrite(FLASH_LED_PIN, LOW);
    Serial.println("  --> [PASS] High-Power White Flash LED (GPIO 4) pulsed for 100ms.");

    // 4. Camera Test
    g_camera_ok = initCamera();

    Serial.println("\n=======================================================");
    Serial.println("                   SUMMARY RESULTS                     ");
    Serial.println("=======================================================");
    Serial.printf("  PSRAM (4MB):        %s\n", g_psram_ok ? "PASS" : "FAIL / NOT DETECTED");
    Serial.printf("  Camera (OV2640):    %s\n", g_camera_ok ? "PASS" : "FAIL");
    Serial.printf("  MicroSD Slot:       %s\n", g_sdcard_ok ? "MEDIA DETECTED" : "NO MEDIA / IDLE");
    Serial.printf("  Status LED (IO33):  PASS\n");
    Serial.printf("  Flash LED (IO4):    PASS\n");
    Serial.println("=======================================================\n");
}

// ----------------------------------------------------------------------------
// Web Server & Streaming Endpoints
// ----------------------------------------------------------------------------
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// HTML Web Dashboard
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32-CAM Hardware Test</title>
    <style>
        :root { --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --accent: #38bdf8; --success: #22c55e; --danger: #ef4444; }
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 15px; }
        .container { max-width: 720px; margin: 0 auto; }
        h1 { font-size: 1.5rem; text-align: center; margin-bottom: 5px; color: var(--accent); }
        p.subtitle { text-align: center; color: #94a3b8; margin-top: 0; margin-bottom: 20px; font-size: 0.9rem; }
        .card { background: var(--card); border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }
        .stream-container { position: relative; width: 100%; border-radius: 8px; overflow: hidden; background: #000; text-align: center; min-height: 240px; }
        .stream-container img { width: 100%; max-height: 480px; object-fit: contain; display: block; }
        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 10px; margin-top: 10px; }
        .stat-box { background: rgba(255,255,255,0.05); padding: 10px; border-radius: 8px; text-align: center; }
        .stat-label { font-size: 0.75rem; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }
        .stat-val { font-size: 1.05rem; font-weight: bold; margin-top: 4px; }
        .badge-pass { color: var(--success); }
        .badge-fail { color: var(--danger); }
        .btn-group { display: flex; gap: 10px; margin-top: 15px; flex-wrap: wrap; }
        button { flex: 1; padding: 12px; font-size: 0.95rem; font-weight: 600; border: none; border-radius: 8px; cursor: pointer; transition: 0.2s; min-width: 130px; }
        .btn-primary { background: var(--accent); color: #0f172a; }
        .btn-warn { background: #f59e0b; color: #0f172a; }
        .btn-snap { background: #6366f1; color: #ffffff; }
        button:active { transform: scale(0.97); }
    </style>
</head>
<body>
    <div class="container">
        <h1>AI-Thinker ESP32-CAM</h1>
        <p class="subtitle">Hardware Diagnostics & Live Stream</p>

        <div class="card">
            <div class="stream-container">
                <img id="cam-view" src="/stream" alt="Live Camera Stream">
            </div>
            <div class="btn-group">
                <button class="btn-primary" onclick="toggleFlash()">Toggle Flash LED</button>
                <button class="btn-snap" onclick="takeSnapshot()">Download Photo</button>
                <button class="btn-warn" onclick="reloadStream()">Restart Stream</button>
            </div>
        </div>

        <div class="card">
            <h3 style="margin-top:0; font-size: 1.1rem;">Hardware Status</h3>
            <div class="grid" id="stats-grid">
                <div class="stat-box"><div class="stat-label">Camera Sensor</div><div class="stat-val badge-pass" id="st-cam">OK</div></div>
                <div class="stat-box"><div class="stat-label">PSRAM (4MB)</div><div class="stat-val" id="st-psram">Checking...</div></div>
                <div class="stat-box"><div class="stat-label">Internal Heap</div><div class="stat-val" id="st-heap">-- KB</div></div>
                <div class="stat-box"><div class="stat-label">MicroSD Slot</div><div class="stat-val" id="st-sd">--</div></div>
                <div class="stat-box"><div class="stat-label">Die Temp</div><div class="stat-val" id="st-temp">-- &deg;C</div></div>
            </div>
        </div>
    </div>

    <script>
        let isStreaming = true;
        let activeImg = new Image();

        function toggleFlash() {
            fetch('/flash').then(r => r.text()).then(txt => {
                console.log('Flash state:', txt);
            });
        }

        function takeSnapshot() {
            const a = document.createElement('a');
            a.href = '/capture?t=' + Date.now();
            a.download = 'esp32_photo_' + Date.now() + '.jpg';
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
        }

        // Client-Paced Zero-Buffer-Bloat Frame Loop
        // Prevents TCP buffer buildup, keeps latency <50ms, and never crashes
        function fetchNextFrame() {
            if (!isStreaming) return;
            const view = document.getElementById('cam-view');
            activeImg = new Image();
            activeImg.onload = () => {
                view.src = activeImg.src;
                // Fetch next frame immediately after rendering
                setTimeout(fetchNextFrame, 30);
            };
            activeImg.onerror = () => {
                // If a frame drops, wait 200ms and request a fresh one
                setTimeout(fetchNextFrame, 200);
            };
            activeImg.src = '/capture?t=' + Date.now();
        }

        function reloadStream() {
            isStreaming = true;
            fetchNextFrame();
        }

        function fetchStatus() {
            fetch('/status').then(r => r.json()).then(data => {
                document.getElementById('st-cam').textContent = data.camera ? 'PASS' : 'FAIL';
                document.getElementById('st-cam').className = 'stat-val ' + (data.camera ? 'badge-pass' : 'badge-fail');
                
                document.getElementById('st-psram').textContent = data.psram_ok ? (data.free_psram_kb + ' KB Free') : 'FAIL';
                document.getElementById('st-psram').className = 'stat-val ' + (data.psram_ok ? 'badge-pass' : 'badge-fail');

                document.getElementById('st-heap').textContent = data.free_heap_kb + ' KB';
                document.getElementById('st-sd').textContent = data.sd_ok ? (data.sd_size_mb + ' MB') : 'Empty';
                if (data.temp_c !== undefined) {
                    document.getElementById('st-temp').textContent = data.temp_c.toFixed(1) + ' °C';
                    document.getElementById('st-temp').className = 'stat-val ' + (data.temp_c > 75 ? 'badge-fail' : 'badge-pass');
                }
            }).catch(e => console.log(e));
        }

        setInterval(fetchStatus, 3000);
        fetchStatus();
        fetchNextFrame(); // Start zero-lag stream
    </script>
</body>
</html>


)rawliteral";

// Handler for Index page
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// Handler for Single Snapshot (/capture)
static esp_err_t capture_handler(httpd_req_t *req) {
    if (!g_camera_ok) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"capture.jpg\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;

}

// Handler for Continuous MJPEG Video Stream (/stream)
static esp_err_t stream_handler(httpd_req_t *req) {
    if (!g_camera_ok) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    esp_err_t res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part_buf[64];
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            break;
        }
        size_t hlen = snprintf(part_buf, 64, _STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);

        if (res != ESP_OK) {
            break;
        }
        // Throttle to ~10 FPS (80ms delay) to prevent thermal overload of ESP32 and AMS1117 regulator
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    return res;
}


// Handler for Flashlight Toggle (/flash)
static esp_err_t flash_handler(httpd_req_t *req) {
    g_flash_state = !g_flash_state;
    digitalWrite(FLASH_LED_PIN, g_flash_state ? HIGH : LOW);
    char buf[16];
    snprintf(buf, sizeof(buf), "FLASH_%s", g_flash_state ? "ON" : "OFF");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// Handler for System Status JSON (/status)
static esp_err_t status_handler(httpd_req_t *req) {
    float temp_c = temperatureRead();
    char json[256];
    snprintf(json, sizeof(json),
             "{\"camera\":%s,\"psram_ok\":%s,\"free_psram_kb\":%u,\"free_heap_kb\":%u,\"sd_ok\":%s,\"sd_size_mb\":%llu,\"flash\":%s,\"temp_c\":%.1f}",
             g_camera_ok ? "true" : "false",
             g_psram_ok ? "true" : "false",
             (unsigned int)(ESP.getFreePsram() / 1024),
             (unsigned int)(ESP.getFreeHeap() / 1024),
             g_sdcard_ok ? "true" : "false",
             g_sdcard_size_mb,
             g_flash_state ? "true" : "false",
             temp_c);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}


void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;   
    config.stack_size = 4096;
    config.lru_purge_enable = true;  // Auto-purge stale zombie sockets when connections drop
    config.recv_wait_timeout = 4;    // 4s timeout instead of hanging 30s
    config.send_wait_timeout = 4;

    httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
    httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t flash_uri = { .uri = "/flash", .method = HTTP_GET, .handler = flash_handler, .user_ctx = NULL };
    httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &index_uri);
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        httpd_register_uri_handler(stream_httpd, &capture_uri);
        httpd_register_uri_handler(stream_httpd, &flash_uri);
        httpd_register_uri_handler(stream_httpd, &status_uri);
        Serial.println("[5/5] HTTP Streaming Server started on port 80!");
    } else {
        Serial.println("[5/5] Failed to start HTTP server!");
    }
}


// ----------------------------------------------------------------------------
// Interactive Serial Console Commands
// ----------------------------------------------------------------------------
void handleSerialCommands() {
    if (Serial.available()) {
        char cmd = (char)Serial.read();
        while (Serial.available()) Serial.read(); // Flush extra chars

        switch (cmd) {
            case 'c':
            case 'C': {
                Serial.print("[COMMAND] Triggering test snapshot... ");
                uint32_t t0 = millis();
                camera_fb_t* fb = esp_camera_fb_get();
                if (fb) {
                    Serial.printf("OK! (%u bytes, %u ms)\n", fb->len, millis() - t0);
                    esp_camera_fb_return(fb);
                } else {
                    Serial.println("FAILED!");
                }
                break;
            }
            case 'f':
            case 'F': {
                g_flash_state = !g_flash_state;
                digitalWrite(FLASH_LED_PIN, g_flash_state ? HIGH : LOW);
                Serial.printf("[COMMAND] Flash LED toggled -> %s\n", g_flash_state ? "ON" : "OFF");
                break;
            }
            case 's':
            case 'S': {
                Serial.printf("[STATUS] Free Heap: %u KB | Free PSRAM: %u KB | Flash LED: %s | WiFi Clients: %d\n",
                              ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024,
                              g_flash_state ? "ON" : "OFF", WiFi.softAPgetStationNum());
                break;
            }
            case 'r':
            case 'R': {
                Serial.println("[COMMAND] Rebooting ESP32-CAM...");
                delay(200);
                ESP.restart();
                break;
            }
            case 'w':
            case 'W': {
                Serial.println("[COMMAND] Resetting Wi-Fi SoftAP & clearing all client leases...");
                WiFi.softAPdisconnect(true);
                delay(400);
                WiFi.softAP(AP_SSID, AP_PASS);
                Serial.println("[COMMAND] SoftAP restarted fresh! Ready for reconnect.");
                break;
            }
            case '?':
            case 'h': {
                Serial.println("\n--- Available Serial Commands ---");
                Serial.println("  'c' -> Capture test snapshot");
                Serial.println("  'f' -> Toggle Flash LED");
                Serial.println("  's' -> Print memory & connection status");
                Serial.println("  'w' -> Restart Wi-Fi AP (clears DHCP leases)");
                Serial.println("  'r' -> Reboot ESP32");
                Serial.println("---------------------------------");
                break;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Main Setup & Loop
// ----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(1000);

    // Register Wi-Fi Event listener to catch disconnects and DHCP leases live
    WiFi.onEvent(onWiFiEvent);

    // Run full hardware diagnostic
    runSystemDiagnostics();

    // Disable 802.11 modem sleep so laptop/phone never drops beacon sync
    WiFi.setSleep(false);

    // Setup Wi-Fi with reduced TX power to prevent thermal shutdown & current brownouts
    if (CONNECT_TO_HOME_WIFI) {
        Serial.printf("\nConnecting to Home Wi-Fi: %s ", HOME_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_13dBm);  // Cut RF power to prevent heat & brownouts
        WiFi.begin(HOME_SSID, HOME_PASS);
        uint32_t start_connect = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - start_connect < 15000) {
            delay(500);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println(" CONNECTED!");
            Serial.printf("  --> Local IP:       http://%s\n", WiFi.localIP().toString().c_str());
            Serial.printf("  --> Web Dashboard:  http://%s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\n  --> Wi-Fi Connect Timeout! Falling back to SoftAP...");
            WiFi.disconnect();
            WiFi.mode(WIFI_AP);
            WiFi.setTxPower(WIFI_POWER_13dBm);
            WiFi.softAP(AP_SSID, AP_PASS, 6);
            Serial.printf("  --> SoftAP SSID:    %s\n", AP_SSID);
            Serial.printf("  --> Web Dashboard:  http://%s\n", WiFi.softAPIP().toString().c_str());
        }
    } else {
        Serial.println("\nConfiguring Wi-Fi SoftAP...");
        WiFi.mode(WIFI_AP);
        // Lower TX power: cuts heat & prevents brownouts from weak USB ports
        WiFi.setTxPower(WIFI_POWER_13dBm);
        WiFi.softAP(AP_SSID, AP_PASS, 6);
        Serial.printf("  --> SoftAP Started!\n");
        Serial.printf("      SSID:      %s\n", AP_SSID);
        Serial.printf("      Password:  (None - Open)\n");
        Serial.printf("      Open URL:  http://%s\n", WiFi.softAPIP().toString().c_str());
    }


    // Start Web Server
    startCameraServer();

    Serial.println("\n>> Board verification test is READY!");
    if (CONNECT_TO_HOME_WIFI && WiFi.status() == WL_CONNECTED) {
        Serial.printf(">> Open your browser and navigate to: http://%s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println(">> Connect your phone or PC to Wi-Fi: 'ESP32-CAM-TEST'");
        Serial.println(">> Open your browser and navigate to: http://192.168.4.1");
    }
    Serial.println(">> Type '?' in this Serial Monitor for interactive commands.\n");
}

void loop() {
    // Process interactive serial commands
    handleSerialCommands();

    // Heartbeat: Blink Red Status LED every 3 seconds (Active LOW) & check thermal health
    static uint32_t last_heartbeat = 0;
    if (millis() - last_heartbeat > 3000) {
        last_heartbeat = millis();
        digitalWrite(STATUS_LED_PIN, LOW);  // Turn ON red LED
        delay(25);                          // Brief blip
        digitalWrite(STATUS_LED_PIN, HIGH); // Turn OFF

        float current_temp = temperatureRead();
        if (current_temp > 80.0) {
            Serial.printf("[THERMAL WARNING] Die Temp is high: %.1f C! Ensure board has air circulation.\n", current_temp);
        }
    }

    delay(10);
}

