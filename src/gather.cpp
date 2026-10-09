#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_system.h"

/*
 ==============================================================================
        AI-THINKER ESP32-CAM: DEDICATED DATASET GATHERING FIRMWARE 
 ==============================================================================
  Designed specifically for high-speed, zero-cable dataset collection for
  TinyML / Edge Impulse model training.

  Features:
   - Wi-Fi SoftAP ("ESP32-CAM-TEST") 
   - Stable RF power profile (13 dBm) to eliminate USB-FTDI brownouts
   - High-quality VGA (640x480) JPEG capture via external 4MB PSRAM
   - Dual-buffer latest-frame grab pipeline (zero lag preview)
   - Visual shutter feedback (red status LED blinks on every capture)
   - Flash LED toggle (/flash) 
   - Endpoints: /capture, /stream, /flash, /status, and / (browser preview)
 ==============================================================================
*/

// --- AI-Thinker Pin Mapping ---
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

#define FLASH_LED_PIN      4   // High-power Flash LED (Active HIGH)
#define STATUS_LED_PIN    33   // Small Red Onboard LED (Active LOW)

// --- Wi-Fi Configuration ---
// Set to true if you want the ESP32 to connect to your home router:
#define CONNECT_TO_ROUTER false
const char* ROUTER_SSID = "Your_WiFi_Name";
const char* ROUTER_PASS = "Your_WiFi_Password";

// SoftAP Configuration (when CONNECT_TO_ROUTER is false)
const char* AP_SSID = "ESP32-CAM-TEST";
const char* AP_PASS = "";  // Open network (no password)

// --- Server & State ---
static httpd_handle_t s_camera_httpd = NULL;
static httpd_handle_t s_stream_httpd = NULL;

static bool s_flash_on = false;
static uint32_t s_total_captures = 0;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Embedded HTML UI for direct phone or browser access
static const char DATA_COLLECTOR_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta charset="utf-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>TinyFruitML - Data Gatherer</title>
    <style>
        :root { --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --accent: #38bdf8; --green: #22c55e; }
        body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 15px; }
        .box { max-width: 680px; margin: 0 auto; background: var(--card); border-radius: 12px; padding: 16px; box-shadow: 0 4px 10px rgba(0,0,0,0.4); text-align: center; }
        h1 { margin: 6px 0; font-size: 1.4rem; color: var(--accent); }
        p { color: #94a3b8; font-size: 0.9rem; margin-top: 0; }
        .view-wrap { position: relative; width: 100%; border-radius: 8px; overflow: hidden; background: #000; min-height: 240px; margin: 12px 0; }
        img { width: 100%; max-height: 480px; object-fit: contain; display: block; }
        .btn-row { display: flex; gap: 10px; flex-wrap: wrap; margin-top: 10px; }
        button { flex: 1; padding: 12px; font-size: 1rem; font-weight: bold; border: none; border-radius: 8px; cursor: pointer; min-width: 130px; }
        .btn-snap { background: var(--green); color: white; }
        .btn-flash { background: #a9e1fc; color: #0f172a; }
        .btn-stream { background: var(--accent); color: #0f172a; }
        .stat { color: #38bdf8; font-weight: bold; }
    </style>
</head>
<body>
    <div class="box">
        <h1>TinyFruitML - Wireless Data Gatherer</h1>
        <p>Live camera preview from ESP32-CAM</p>
        <div class="view-wrap">
            <img id="stream" src="/capture" alt="Stream">
        </div>
        <div class="btn-row">
            <button class="btn-snap" onclick="snap()">📸 Snapshot</button>
            <button class="btn-flash" onclick="toggleFlash()">💡 Toggle Flash</button>
            <button class="btn-stream" onclick="toggleLive()">▶ Live Feed</button>
        </div>
        <p style="margin-top: 15px; font-size: 0.85rem;">Tip: For automated labeling & fast bursts, run the PC script: <code style="color:var(--accent);">python scripts/collect_dataset_gui.py</code></p>
    </div>
    <script>
        let live = true;
        let view = document.getElementById('stream');
        function fetchFrame() {
            if (!live) return;
            let img = new Image();
            img.onload = () => { view.src = img.src; setTimeout(fetchFrame, 35); };
            img.onerror = () => { setTimeout(fetchFrame, 200); };
            img.src = '/capture?t=' + Date.now();
        }
        function snap() {
            let a = document.createElement('a');
            a.href = '/capture?t=' + Date.now();
            a.download = 'fruit_' + Date.now() + '.jpg';
            document.body.appendChild(a);
            a.click();
            document.body.removeChild(a);
        }
        function toggleFlash() {
            fetch('/flash').catch(e => console.log(e));
        }
        function toggleLive() {
            live = !live;
            if (live) fetchFrame();
        }
        fetchFrame();
    </script>
</body>
</html>
)rawliteral";

// --- HTTP Handlers ---

// 1. GET / -> Web Dashboard
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DATA_COLLECTOR_HTML, HTTPD_RESP_USE_STRLEN);
}

// 2. GET /capture -> Captures fresh JPEG frame
static esp_err_t capture_handler(httpd_req_t *req) {
    // Visual shutter feedback (blink status LED)
    digitalWrite(STATUS_LED_PIN, LOW); // ON (Active LOW)

    camera_fb_t *fb = esp_camera_fb_get();
    digitalWrite(STATUS_LED_PIN, HIGH); // OFF

    if (!fb) {
        Serial.println("[ERROR] Camera capture failed!");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    s_total_captures++;
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"capture.jpg\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return res;
}

// 3. GET /flash -> Toggle onboard Flash LED
static esp_err_t flash_handler(httpd_req_t *req) {
    s_flash_on = !s_flash_on;
    digitalWrite(FLASH_LED_PIN, s_flash_on ? HIGH : LOW);
    
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"flash\": %s}\n", s_flash_on ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// 4. GET /status -> Diagnostics & Health
static esp_err_t status_handler(httpd_req_t *req) {
    char buf[256];
    float temp_c = temperatureRead();
    snprintf(buf, sizeof(buf),
             "{\"status\":\"ok\",\"captures\":%u,\"flash\":%s,\"free_heap\":%u,\"free_psram\":%u,\"temp_c\":%.1f}",
             s_total_captures,
             s_flash_on ? "true" : "false",
             ESP.getFreeHeap(),
             ESP.getFreePsram(),
             temp_c);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

// 5. GET /stream -> Continuous MJPEG Video Feed
static esp_err_t stream_handler(httpd_req_t *req) { 
    esp_err_t res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    char part_buf[64];
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) break;

        size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);

        if (res != ESP_OK) break;
    }
    return res;
}

// Start HTTP Server
void startGatherServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 7;

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t uri_capture = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t uri_flash = { .uri = "/flash", .method = HTTP_GET, .handler = flash_handler, .user_ctx = NULL };
    httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };

    if (httpd_start(&s_camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(s_camera_httpd, &uri_index);
        httpd_register_uri_handler(s_camera_httpd, &uri_capture);
        httpd_register_uri_handler(s_camera_httpd, &uri_flash);
        httpd_register_uri_handler(s_camera_httpd, &uri_status);
        Serial.println("  --> Main HTTP server active on port 80");
    }

    // Separate stream server to prevent socket starvation
    config.server_port = 81;
    config.ctrl_port = 32769;
    httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
    if (httpd_start(&s_stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(s_stream_httpd, &uri_stream);
        Serial.println("  --> Stream server active on port 81");
    }
}

// --- Hardware Initialization ---
bool initGatherCamera() {
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

    // Use PSRAM for high-detail VGA (640x480) with JPEG quality 10
    if (psramFound()) {
        config.frame_size   = FRAMESIZE_VGA;  // 640x480 (ideal for ML downscaling)
        config.jpeg_quality = 10;             // 10-63 (lower = sharper)
        config.fb_count     = 2;
        config.grab_mode    = CAMERA_GRAB_LATEST;
    } else {
        config.frame_size   = FRAMESIZE_QVGA; // 320x240 safe fallback
        config.jpeg_quality = 12;
        config.fb_count     = 1;
        config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[FAIL] Camera init error: 0x%x\n", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        // AI-Thinker downward orientation adjustments
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
        // Slightly enhance contrast/saturation for fruit ML
        s->set_contrast(s, 1);
        s->set_saturation(s, 1);
    }

    return true;
}

void setup() {
    Serial.begin(115200);
    Serial.setRxBufferSize(1024);
    delay(500);

    Serial.println("\n==================================================");
    Serial.println("   TINYFRUITML: DATASET GATHERING FIRMWARE        ");
    Serial.println("==================================================");

    // Pin setup
    pinMode(STATUS_LED_PIN, OUTPUT);
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH); // OFF
    digitalWrite(FLASH_LED_PIN, LOW);  // OFF

    // Initialize Camera
    Serial.print("Initializing OV2640 camera... ");
    if (initGatherCamera()) {
        Serial.println("[OK]");
    } else {
        Serial.println("[FAIL] Please verify camera ribbon cable.");
    }

    // Configure Wi-Fi
    if (CONNECT_TO_ROUTER) {
        Serial.printf("Connecting to router: %s ", ROUTER_SSID);
        WiFi.mode(WIFI_STA);
        WiFi.setTxPower(WIFI_POWER_13dBm); // Prevent brownouts
        WiFi.begin(ROUTER_SSID, ROUTER_PASS);
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
            delay(400);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[CONNECTED] IP: http://%s\n", WiFi.localIP().toString().c_str());
        } else {
            Serial.println("\nRouter timeout. Falling back to SoftAP...");
            WiFi.disconnect();
            WiFi.mode(WIFI_AP);
            WiFi.setTxPower(WIFI_POWER_13dBm);
            WiFi.softAP(AP_SSID, AP_PASS, 6);
        }
    } else {
        Serial.println("Starting Wi-Fi SoftAP...");
        WiFi.mode(WIFI_AP);
        WiFi.setTxPower(WIFI_POWER_13dBm); // Crucial for cheap USB cables
        WiFi.softAP(AP_SSID, AP_PASS, 6);
        Serial.printf("  --> SoftAP SSID: %s\n", AP_SSID);
        Serial.printf("  --> IP Address:  http://%s\n", WiFi.softAPIP().toString().c_str());
    }

    // Start HTTP Server
    startGatherServer();

    Serial.println("\nReady for data gathering!");
    Serial.println("  1. Connect PC Wi-Fi to 'ESP32-CAM-TEST'");
    Serial.println("  2. Run PC GUI: 'python scripts/collect_dataset_gui.py'");
    Serial.println("  3. Or open browser: http://192.168.4.1\n");
}

void loop() {
    // Heartbeat: Blink red LED for 20ms every 3 seconds to indicate healthy operation
    static uint32_t last_hb = 0;
    if (millis() - last_hb > 3000) {
        last_hb = millis();
        digitalWrite(STATUS_LED_PIN, LOW); // ON
        delay(20);
        digitalWrite(STATUS_LED_PIN, HIGH); // OFF
    }

    // Handle interactive serial commands
    if (Serial.available()) {
        char ch = Serial.read();
        if (ch == 'f') {
            s_flash_on = !s_flash_on;
            digitalWrite(FLASH_LED_PIN, s_flash_on ? HIGH : LOW);
            Serial.printf("Flash LED is now %s\n", s_flash_on ? "ON" : "OFF");
        } else if (ch == 'c') {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                Serial.printf("Captured frame: %u x %u (%u bytes)\n", fb->width, fb->height, fb->len);
                esp_camera_fb_return(fb);
            } else {
                Serial.println("Capture failed!");
            }
        } else if (ch == '?') {
            Serial.println("Commands: [f] toggle flash, [c] test capture, [?] help");
        }
    }

    delay(10);
}
