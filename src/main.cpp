/*
 ==============================================================================
  EDGE AI SMART RETAIL SCALE - ESP32-CAM TINYML COPROCESSOR & WEB DASHBOARD
 ==============================================================================
  Runs on-device quantized INT8 FOMO (Faster Objects, More Objects) neural 
  network to identify fruit varieties (Apple, Orange, Pear) and benchmark 
  the hardware performance of the ESP32-CAM.

  Features:
   - Real-time Edge Impulse FOMO Object Detection (Apple, Orange, Pear)
   - Embedded Wi-Fi Web Server (SoftAP "ESP32-CAM-SCALE" @ 192.168.4.1)
   - Live Browser Video Stream with Real-Time Metrology & Pricing HUD
   - Simulated STM32 Metrology (Realistic weight, unit price, total price, sanity check)
   - Thread-safe FreeRTOS camera access (zero DMA collision / freeze)
   - Inter-Chip UART Link to STM32 Master (Serial2 on GPIO 14/15)
   - Interactive Serial Testing Commands ('f'=flash, 'm'=mode, 'w'=weight, 'b'=benchmark)
 ==============================================================================
*/

#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "esp_system.h"

// Edge Impulse TinyML Vision Model
#include <a3FruitVision_V1_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"

// --- AI-Thinker ESP32-CAM Pin Configuration ---
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

#define FLASH_LED_PIN      4   // High-power White Flashlight LED (Active HIGH)
#define STATUS_LED_PIN    33   // Small Red Onboard LED (Active LOW)

// --- Wi-Fi Configuration ---
#define AP_SSID           "ESP32-CAM-SCALE"
#define AP_PASS           ""   // Open network (no password needed)

// --- Inter-Chip UART to STM32 Master (Serial2) ---
#define STM32_RX_PIN      14   // ESP32 RX from STM32 TX
#define STM32_TX_PIN      15   // ESP32 TX to STM32 RX
#define STM32_BAUD    115200
HardwareSerial STM32_Serial(2);

// --- Mock / Simulated STM32 Metrology & Pricing ---
#define UNIT_PRICE_APPLE    2.50f  // $2.50 / kg
#define UNIT_PRICE_ORANGE   3.00f  // $3.00 / kg
#define UNIT_PRICE_PEAR     3.80f  // $3.80 / kg

enum MockWeightMode {
    MOCK_WEIGHT_RANDOM,      // Realistic random weight per detected fruit
    MOCK_WEIGHT_FIXED_200G,  // Fixed 200.0g test value
    MOCK_WEIGHT_TOO_LIGHT    // 15.0g (fails sanity bounds check)
};
static MockWeightMode g_weight_mode = MOCK_WEIGHT_RANDOM;

// --- Live Telemetry Structure for Web & Serial ---
struct LiveScaleTelemetry {
    char label[16];
    float confidence;
    uint32_t x, y, w, h;
    float weight_g;
    float unit_price;
    float total_price;
    bool sanity_ok;
    bool detected;
    uint32_t dsp_ms;
    uint32_t infer_ms;
    uint32_t total_ms;
    float fps;
    uint8_t votes;
    uint8_t burst_total;
};
static LiveScaleTelemetry g_telemetry = {"none", 0.0f, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, false, false, 0, 0, 0, 0.0f, 0, 4};

// --- Burst Voting Configuration (Temporal Consensus Ensemble) ---
#define BURST_SAMPLE_COUNT     4    // Frames captured per inference event
#define BURST_FRAME_DELAY_MS  25    // Yield interval between burst frames for exposure/FreeRTOS

// --- Camera & Inference Resolution ---
#define CAMERA_RAW_WIDTH   320
#define CAMERA_RAW_HEIGHT  240

// Buffers & Sync
static uint8_t *snapshot_buf = nullptr;
static bool g_flash_state = false;
static bool g_continuous_mode = false; // Default: Triggered burst mode for high-FPS live stream
static SemaphoreHandle_t s_camera_mutex = NULL;
static httpd_handle_t s_httpd = NULL;

// Benchmark Statistics
static uint32_t g_inference_count = 0;
static uint32_t g_total_infer_time_ms = 0;
static uint32_t g_min_infer_time_ms = 99999;
static uint32_t g_max_infer_time_ms = 0;

// Function Prototypes
bool initCamera();
bool runBurstInference();
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr);
void startWebServer();
void handleSerialCommands();
void printBenchmarkReport();

// ----------------------------------------------------------------------------
// Embedded HTML Dashboard
// ----------------------------------------------------------------------------
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Edge AI Smart Scale</title>
  <style>
    :root { --bg: #0f172a; --card: #1e293b; --text: #f8fafc; --accent: #38bdf8; --green: #22c55e; --red: #ef4444; }
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 16px; }
    .container { max-width: 760px; margin: 0 auto; }
    h1 { text-align: center; color: var(--accent); margin-bottom: 4px; font-size: 1.5rem; }
    p.sub { text-align: center; color: #94a3b8; margin-top: 0; margin-bottom: 16px; font-size: 0.85rem; }
    .card { background: var(--card); border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 0 4px 12px rgba(0,0,0,0.3); }
    .viewport-wrap { position: relative; width: 100%; border-radius: 8px; overflow: hidden; background: #000; text-align: center; min-height: 240px; }
    #cam-view { width: 100%; max-height: 480px; object-fit: contain; display: block; }
    .hud-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(140px, 1fr)); gap: 10px; margin-top: 14px; }
    .hud-box { background: rgba(255,255,255,0.05); padding: 10px; border-radius: 8px; text-align: center; }
    .hud-label { font-size: 0.75rem; color: #94a3b8; text-transform: uppercase; letter-spacing: 0.5px; }
    .hud-val { font-size: 1.15rem; font-weight: bold; margin-top: 4px; }
    .badge-fruit { color: var(--green); }
    .badge-empty { color: #64748b; }
    .badge-pass { color: var(--green); }
    .badge-fail { color: var(--red); }
    .btn-row { display: flex; gap: 10px; margin-top: 14px; flex-wrap: wrap; }
    button { flex: 1; padding: 12px; font-size: 0.95rem; font-weight: 600; border: none; border-radius: 8px; cursor: pointer; min-width: 130px; transition: 0.15s; }
    .btn-flash { background: #eab308; color: #0f172a; }
    .btn-weight { background: #8b5cf6; color: white; }
    .btn-mode { background: #3b82f6; color: white; }
    button:active { transform: scale(0.97); }
    .telemetry-row { font-size: 0.8rem; color: #94a3b8; text-align: center; margin-top: 12px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Edge AI Smart Retail Scale</h1>
    <p class="sub">TinyML Vision Coprocessor & Simulated Metrology Dashboard</p>

    <div class="card">
      <div class="viewport-wrap">
        <img id="cam-view" src="/capture" alt="Live Camera View">
      </div>
      <div class="btn-row">
        <button class="btn-flash" onclick="toggleFlash()">💡 Toggle Flashlight</button>
        <button class="btn-weight" onclick="cycleWeight()">⚖️ Cycle Mock Weight</button>
        <button class="btn-mode" onclick="triggerInference()">📸 Trigger Burst AI</button>
      </div>
    </div>

    <div class="card">
      <h3 style="margin-top:0; font-size: 1.1rem; color: var(--accent);">Real-Time Checkout Slip</h3>
      <div class="hud-grid">
        <div class="hud-box">
          <div class="hud-label">Detected Fruit</div>
          <div class="hud-val badge-fruit" id="val-item">Waiting...</div>
          <div style="font-size:0.75rem; color:#94a3b8; margin-top:4px;" id="val-votes">Ready</div>
        </div>
        <div class="hud-box">
          <div class="hud-label">Confidence</div>
          <div class="hud-val" id="val-conf">-- %</div>
        </div>
        <div class="hud-box">
          <div class="hud-label">Mass (Grams)</div>
          <div class="hud-val" id="val-mass">-- g</div>
        </div>
        <div class="hud-box">
          <div class="hud-label">Unit Price</div>
          <div class="hud-val" id="val-unit">$-- / kg</div>
        </div>
        <div class="hud-box" style="border: 1px solid var(--accent);">
          <div class="hud-label">Total Charge</div>
          <div class="hud-val" style="color:var(--accent);" id="val-total">$--</div>
        </div>
        <div class="hud-box">
          <div class="hud-label">Sanity Check</div>
          <div class="hud-val badge-pass" id="val-sanity">OK</div>
        </div>
      </div>
      <div class="telemetry-row" id="val-telemetry">
        Timing: DSP -- ms | Infer -- ms | Total -- ms (-- FPS) | Die Temp: -- &deg;C
      </div>
    </div>
  </div>

  <script>
    function updateFrame() {
      const view = document.getElementById('cam-view');
      const img = new Image();
      img.onload = () => {
        view.src = img.src;
        setTimeout(updateFrame, 80);
      };
      img.onerror = () => {
        setTimeout(updateFrame, 200);
      };
      img.src = '/capture?t=' + Date.now();
    }

    function pollTelemetry() {
      fetch('/status').then(r => r.json()).then(d => {
        const itemEl = document.getElementById('val-item');
        const votesEl = document.getElementById('val-votes');
        if (d.detected) {
          itemEl.innerText = d.label.toUpperCase();
          itemEl.className = 'hud-val badge-fruit';
          votesEl.innerText = d.votes + '/' + d.burst_total + ' frames (' + Math.round((d.votes / d.burst_total) * 100) + '% consensus)';
        } else {
          itemEl.innerText = 'EMPTY SCALE';
          itemEl.className = 'hud-val badge-empty';
          votesEl.innerText = '0/' + d.burst_total + ' (No Fruit)';
        }
        document.getElementById('val-conf').innerText = d.confidence > 0 ? (d.confidence * 100).toFixed(1) + '%' : '--';
        document.getElementById('val-mass').innerText = d.weight > 0 ? d.weight.toFixed(1) + ' g' : '0.0 g';
        document.getElementById('val-unit').innerText = d.unit_price > 0 ? '$' + d.unit_price.toFixed(2) + ' / kg' : '--';
        document.getElementById('val-total').innerText = d.total_price > 0 ? '$' + d.total_price.toFixed(2) : '$0.00';
        
        const sanityEl = document.getElementById('val-sanity');
        if (!d.detected) {
          sanityEl.innerText = 'IDLE';
          sanityEl.className = 'hud-val badge-empty';
        } else if (d.sanity_ok) {
          sanityEl.innerText = 'PASSED';
          sanityEl.className = 'hud-val badge-pass';
        } else {
          sanityEl.innerText = 'REJECTED';
          sanityEl.className = 'hud-val badge-fail';
        }

        document.getElementById('val-telemetry').innerHTML =
          'Timing: DSP ' + d.dsp_ms + ' ms | Infer ' + d.infer_ms + ' ms | Total ' + d.total_ms + ' ms (' + d.fps.toFixed(1) + ' FPS) | Die Temp: ' + d.temp.toFixed(1) + ' &deg;C | PSRAM Free: ' + d.psram_kb + ' KB';
      }).catch(e => console.log(e));
    }

    function toggleFlash() { fetch('/flash').catch(e => console.log(e)); }
    function cycleWeight() { fetch('/cycle_weight').then(pollTelemetry).catch(e => console.log(e)); }
    function triggerInference() { fetch('/trigger').catch(e => console.log(e)); }

    setInterval(pollTelemetry, 300);
    pollTelemetry();
    updateFrame();
  </script>
</body>
</html>
)rawliteral";

// ----------------------------------------------------------------------------
// HTTP Server Handlers
// ----------------------------------------------------------------------------
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t *req) {
    if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(150)) == pdTRUE) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            xSemaphoreGive(s_camera_mutex);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"capture.jpg\"");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
        esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        xSemaphoreGive(s_camera_mutex);
        return res;
    }
    httpd_resp_send_500(req);
    return ESP_FAIL;
}

static esp_err_t status_handler(httpd_req_t *req) {
    char json[448];
    snprintf(json, sizeof(json),
             "{\"label\":\"%s\",\"confidence\":%.3f,\"x\":%u,\"y\":%u,\"weight\":%.1f,"
             "\"unit_price\":%.2f,\"total_price\":%.2f,\"sanity_ok\":%s,\"detected\":%s,"
             "\"dsp_ms\":%u,\"infer_ms\":%u,\"total_ms\":%u,\"fps\":%.1f,"
             "\"votes\":%u,\"burst_total\":%u,"
             "\"temp\":%.1f,\"psram_kb\":%u,\"heap_kb\":%u}",
             g_telemetry.label, g_telemetry.confidence, g_telemetry.x, g_telemetry.y,
             g_telemetry.weight_g, g_telemetry.unit_price, g_telemetry.total_price,
             g_telemetry.sanity_ok ? "true" : "false",
             g_telemetry.detected ? "true" : "false",
             g_telemetry.dsp_ms, g_telemetry.infer_ms, g_telemetry.total_ms, g_telemetry.fps,
             g_telemetry.votes, g_telemetry.burst_total,
             temperatureRead(), ESP.getFreePsram() / 1024, ESP.getFreeHeap() / 1024);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t flash_handler(httpd_req_t *req) {
    g_flash_state = !g_flash_state;
    digitalWrite(FLASH_LED_PIN, g_flash_state ? HIGH : LOW);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, g_flash_state ? "ON" : "OFF", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t cycle_weight_handler(httpd_req_t *req) {
    if (g_weight_mode == MOCK_WEIGHT_RANDOM) g_weight_mode = MOCK_WEIGHT_FIXED_200G;
    else if (g_weight_mode == MOCK_WEIGHT_FIXED_200G) g_weight_mode = MOCK_WEIGHT_TOO_LIGHT;
    else g_weight_mode = MOCK_WEIGHT_RANDOM;
    runBurstInference();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t trigger_handler(httpd_req_t *req) {
    runBurstInference();
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

void startWebServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 5;

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t uri_capture = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    httpd_uri_t uri_flash = { .uri = "/flash", .method = HTTP_GET, .handler = flash_handler, .user_ctx = NULL };
    httpd_uri_t uri_cycle = { .uri = "/cycle_weight", .method = HTTP_GET, .handler = cycle_weight_handler, .user_ctx = NULL };
    httpd_uri_t uri_trigger = { .uri = "/trigger", .method = HTTP_GET, .handler = trigger_handler, .user_ctx = NULL };

    if (httpd_start(&s_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(s_httpd, &uri_index);
        httpd_register_uri_handler(s_httpd, &uri_capture);
        httpd_register_uri_handler(s_httpd, &uri_status);
        httpd_register_uri_handler(s_httpd, &uri_flash);
        httpd_register_uri_handler(s_httpd, &uri_cycle);
        httpd_register_uri_handler(s_httpd, &uri_trigger);
        Serial.println("[WEB] Embedded HTTP Server active on Port 80");
    }
}

// ----------------------------------------------------------------------------
// Camera Hardware Initialization
// ----------------------------------------------------------------------------
bool initCamera() {
    Serial.println("\n[1/4] Initializing OV2640 Camera Sensor...");

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
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240 for fast JPEG decoding
    config.jpeg_quality = 12;              // High quality
    config.fb_count     = 2;               // Double-buffered for speed
    config.fb_location  = CAMERA_FB_IN_PSRAM;
    config.grab_mode    = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("  --> [FAIL] Camera init error: 0x%x\n", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s != NULL) {
        s->set_vflip(s, 1);    // Invert if mounted downward on scale bracket
        s->set_hmirror(s, 0);
        s->set_brightness(s, 0);
        s->set_contrast(s, 1);
        s->set_saturation(s, 1);
    }

    Serial.println("  --> [PASS] OV2640 camera ready!");
    return true;
}

// ----------------------------------------------------------------------------
// Setup Function
// ----------------------------------------------------------------------------
void setup() {
    delay(500); // 3.3V rail cold power stabilization delay

    Serial.begin(115200);
    Serial.setRxBufferSize(1024);
    delay(300);

    Serial.println("\n=======================================================");
    Serial.println("  EDGE AI SMART SCALE: TINYML & WEB SERVER DASHBOARD   ");
    Serial.println("=======================================================");

    // Pin setup
    pinMode(STATUS_LED_PIN, OUTPUT);
    pinMode(FLASH_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, HIGH); // Red LED OFF (Active LOW)
    digitalWrite(FLASH_LED_PIN, LOW);   // Flashlight OFF

    // Create FreeRTOS camera access mutex
    s_camera_mutex = xSemaphoreCreateMutex();

    // 1. Verify PSRAM
    if (psramFound()) {
        Serial.printf("[PSRAM] Detected! Total: %u KB, Free: %u KB\n",
                      ESP.getPsramSize() / 1024, ESP.getFreePsram() / 1024);
    } else {
        Serial.println("[PSRAM] WARNING: External PSRAM not detected! Model may fail.");
    }

    // 2. Allocate RGB888 Snapshot Buffer in PSRAM (230 KB for 320x240x3)
    size_t rgb_buf_size = CAMERA_RAW_WIDTH * CAMERA_RAW_HEIGHT * 3;
    snapshot_buf = (uint8_t*)ps_malloc(rgb_buf_size);
    if (!snapshot_buf) {
        Serial.println("[MEMORY] Failed to allocate snapshot buffer in PSRAM! Falling back to malloc...");
        snapshot_buf = (uint8_t*)malloc(rgb_buf_size);
    }

    if (!snapshot_buf) {
        Serial.println("[CRITICAL ERROR] Insufficient memory for RGB snapshot buffer!");
    } else {
        Serial.printf("[MEMORY] Allocated %u KB snapshot buffer successfully.\n", rgb_buf_size / 1024);
    }

    // 3. Start Wi-Fi SoftAP FIRST (Guarantees Wi-Fi is NEVER blocked by cold camera)
    Serial.println("\n[WIFI] Initializing SoftAP...");
    WiFi.mode(WIFI_AP);
    WiFi.setTxPower(WIFI_POWER_15dBm);
    WiFi.softAP(AP_SSID, AP_PASS, 6);
    Serial.printf("  --> SoftAP SSID: %s\n", AP_SSID);
    Serial.printf("  --> Web URL:     http://%s\n", WiFi.softAPIP().toString().c_str());

    // 4. Start Embedded Web Server
    startWebServer();

    // 5. Initialize Camera with Cold-Boot Retry Loop
    delay(200);
    bool cam_ok = false;
    for (int retry = 0; retry < 3; retry++) {
        if (initCamera()) {
            cam_ok = true;
            break;
        }
        Serial.printf("[CAMERA] Cold boot retry %d/3 initializing OV2640...\n", retry + 1);
        delay(300);
    }
    if (!cam_ok) {
        Serial.println("[CRITICAL ERROR] Camera failed after 3 attempts! Check ribbon connector.");
    }

    // 6. Initialize STM32 UART Link
    STM32_Serial.begin(STM32_BAUD, SERIAL_8N1, STM32_RX_PIN, STM32_TX_PIN);
    Serial.printf("[UART] STM32 inter-chip link ready on RX: GPIO %d, TX: GPIO %d @ %d baud\n",
                  STM32_RX_PIN, STM32_TX_PIN, STM32_BAUD);

    // 7. Model Overview
    Serial.println("\n[MODEL SPECS]");
    Serial.printf("  Project Name:     %s (Ver %d)\n", EI_CLASSIFIER_PROJECT_NAME, EI_CLASSIFIER_PROJECT_DEPLOY_VERSION);
    Serial.printf("  Input Resolution: %d x %d (RGB888, %d features)\n",
                  EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT, EI_CLASSIFIER_NN_INPUT_FRAME_SIZE);
    Serial.printf("  Classes (%d):      ", EI_CLASSIFIER_LABEL_COUNT);
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        Serial.printf("%s%s", ei_classifier_inferencing_categories[i], (i + 1 < EI_CLASSIFIER_LABEL_COUNT) ? ", " : "\n");
    }
    Serial.printf("  Architecture:     FOMO (Object Detection) INT8 Quantized\n");
    Serial.printf("  Ensemble Mode:    %d-Frame Burst Consensus Voting\n", BURST_SAMPLE_COUNT);
    Serial.printf("  Tensor Arena:     %d KB required\n", EI_CLASSIFIER_TFLITE_LARGEST_ARENA_SIZE / 1024);

    Serial.println("\n>> System Ready! Open http://192.168.4.1 in your browser.");
    Serial.println(">> Commands: [f] Flash | [m] Mode | [w] Weight Mode | [b] Benchmark | [Space] Trigger Burst\n");
}

// ----------------------------------------------------------------------------
// Burst Voting Inference (Temporal Consensus Ensemble)
// ----------------------------------------------------------------------------
bool runBurstInference() {
    if (!snapshot_buf) {
        Serial.println("[ERROR] Cannot infer: Snapshot buffer not allocated.");
        return false;
    }

    uint32_t t_burst_start = millis();
    Serial.println("\n=======================================================");
    Serial.printf(" [BURST INFERENCE] Capturing %d frames for consensus...\n", BURST_SAMPLE_COUNT);
    Serial.println("=======================================================");

    // Vote counters and confidence accumulators per class
    uint8_t votes_apple = 0, votes_orange = 0, votes_pear = 0;
    float conf_sum_apple = 0.0f, conf_sum_orange = 0.0f, conf_sum_pear = 0.0f;
    uint32_t best_x_apple = 0, best_y_apple = 0;
    uint32_t best_x_orange = 0, best_y_orange = 0;
    uint32_t best_x_pear = 0, best_y_pear = 0;

    uint32_t total_dsp_ms = 0;
    uint32_t total_infer_ms = 0;
    uint8_t valid_frames = 0;

    for (int frame = 0; frame < BURST_SAMPLE_COUNT; frame++) {
        // 1. Thread-safe snapshot grab (releases mutex immediately after copy)
        camera_fb_t *fb = NULL;
        if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            fb = esp_camera_fb_get();
            if (fb) {
                fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);
                esp_camera_fb_return(fb);
            }
            xSemaphoreGive(s_camera_mutex);
        }

        if (!fb) {
            Serial.printf("  ├─ Frame %d/%d: [FAIL] Camera buffer acquire timeout\n", frame + 1, BURST_SAMPLE_COUNT);
            delay(10);
            continue;
        }

        valid_frames++;

        // 2. Crop and interpolate down to 96x96 (runs outside camera mutex)
        ei::image::processing::crop_and_interpolate_rgb888(
            snapshot_buf,
            CAMERA_RAW_WIDTH,
            CAMERA_RAW_HEIGHT,
            snapshot_buf,
            EI_CLASSIFIER_INPUT_WIDTH,
            EI_CLASSIFIER_INPUT_HEIGHT
        );

        // 3. Construct Edge Impulse signal
        ei::signal_t signal;
        signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
        signal.get_data = &ei_camera_get_data;

        // 4. Run Edge Impulse FOMO Model
        ei_impulse_result_t result = { 0 };
        EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);

        if (err != EI_IMPULSE_OK) {
            Serial.printf("  ├─ Frame %d/%d: [ERROR] Classifier returned code %d\n", frame + 1, BURST_SAMPLE_COUNT, err);
            continue;
        }

        total_dsp_ms += result.timing.dsp;
        total_infer_ms += result.timing.classification;

        // Track global benchmark numbers
        g_inference_count++;
        g_total_infer_time_ms += result.timing.classification;
        if (result.timing.classification < g_min_infer_time_ms) g_min_infer_time_ms = result.timing.classification;
        if (result.timing.classification > g_max_infer_time_ms) g_max_infer_time_ms = result.timing.classification;

        // 5. Parse bounding boxes for this frame
        bool detected_in_frame = false;
        const char* frame_best_label = "none";
        float frame_best_conf = 0.0f;
        uint32_t frame_x = 0, frame_y = 0;

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
        for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
            ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
            if (bb.value >= 0.45f) { // FOMO threshold
                detected_in_frame = true;
                if (bb.value > frame_best_conf) {
                    frame_best_conf = bb.value;
                    frame_best_label = bb.label;
                    frame_x = bb.x;
                    frame_y = bb.y;
                }
            }
        }
#endif

        if (detected_in_frame) {
            if (strcasecmp(frame_best_label, "apple") == 0) {
                votes_apple++;
                conf_sum_apple += frame_best_conf;
                best_x_apple = frame_x; best_y_apple = frame_y;
            } else if (strcasecmp(frame_best_label, "orange") == 0) {
                votes_orange++;
                conf_sum_orange += frame_best_conf;
                best_x_orange = frame_x; best_y_orange = frame_y;
            } else if (strcasecmp(frame_best_label, "pear") == 0) {
                votes_pear++;
                conf_sum_pear += frame_best_conf;
                best_x_pear = frame_x; best_y_pear = frame_y;
            }

            Serial.printf("  ├─ Frame %d/%d: '%s' (%.1f%%) @ [%u, %u] (%u ms)\n",
                          frame + 1, BURST_SAMPLE_COUNT, frame_best_label,
                          frame_best_conf * 100.0f, frame_x, frame_y, result.timing.classification);
        } else {
            Serial.printf("  ├─ Frame %d/%d: No fruit / Below threshold (%u ms)\n",
                          frame + 1, BURST_SAMPLE_COUNT, result.timing.classification);
        }

        // Yield between frames to let sensor exposure settle and Wi-Fi service packets
        if (frame + 1 < BURST_SAMPLE_COUNT) {
            delay(BURST_FRAME_DELAY_MS);
        }
    }

    uint32_t t_burst_total = millis() - t_burst_start;

    // 6. Tally Ensemble Votes & Determine Consensus Winner
    const char* winner_label = "none";
    uint8_t winner_votes = 0;
    float winner_avg_conf = 0.0f;
    uint32_t winner_x = 0, winner_y = 0;
    bool detected_any = false;

    // Evaluate Apple
    if (votes_apple > winner_votes || (votes_apple == winner_votes && conf_sum_apple > 0.0f && winner_votes > 0)) {
        winner_votes = votes_apple;
        winner_label = "apple";
        winner_avg_conf = conf_sum_apple / votes_apple;
        winner_x = best_x_apple; winner_y = best_y_apple;
        detected_any = true;
    }
    // Evaluate Orange
    if (votes_orange > winner_votes || (votes_orange == winner_votes && conf_sum_orange > (winner_avg_conf * winner_votes))) {
        winner_votes = votes_orange;
        winner_label = "orange";
        winner_avg_conf = conf_sum_orange / votes_orange;
        winner_x = best_x_orange; winner_y = best_y_orange;
        detected_any = true;
    }
    // Evaluate Pear
    if (votes_pear > winner_votes || (votes_pear == winner_votes && conf_sum_pear > (winner_avg_conf * winner_votes))) {
        winner_votes = votes_pear;
        winner_label = "pear";
        winner_avg_conf = conf_sum_pear / votes_pear;
        winner_x = best_x_pear; winner_y = best_y_pear;
        detected_any = true;
    }

    // Visual LED Pulse on detection
    if (detected_any) {
        digitalWrite(STATUS_LED_PIN, LOW); // Red LED ON
    } else {
        digitalWrite(STATUS_LED_PIN, HIGH); // OFF
    }

    // 7. Calculate Simulated Metrology & Pricing
    float weight_g = 0.0f;
    float unit_price = 0.0f;
    float total_price = 0.0f;
    bool sanity_ok = false;
    const char* mode_str = "Random";

    if (detected_any) {
        if (g_weight_mode == MOCK_WEIGHT_FIXED_200G) {
            weight_g = 200.0f;
            mode_str = "Fixed 200g";
        } else if (g_weight_mode == MOCK_WEIGHT_TOO_LIGHT) {
            weight_g = 15.0f;
            mode_str = "Fault Test 15g";
        } else {
            // Realistic random weight with 0.1g resolution
            if (strcmp(winner_label, "apple") == 0)       weight_g = (float)random(1600, 2300) / 10.0f;
            else if (strcmp(winner_label, "orange") == 0) weight_g = (float)random(1400, 2000) / 10.0f;
            else if (strcmp(winner_label, "pear") == 0)   weight_g = (float)random(1800, 2700) / 10.0f;
            else                                          weight_g = (float)random(1500, 2200) / 10.0f;
            mode_str = "Random Realistic";
        }

        unit_price = 2.00f;
        if (strcmp(winner_label, "apple") == 0)       unit_price = UNIT_PRICE_APPLE;
        else if (strcmp(winner_label, "orange") == 0) unit_price = UNIT_PRICE_ORANGE;
        else if (strcmp(winner_label, "pear") == 0)   unit_price = UNIT_PRICE_PEAR;

        total_price = (weight_g / 1000.0f) * unit_price;
        sanity_ok = (weight_g >= 50.0f && weight_g <= 600.0f);
    }

    // 8. Update Live Telemetry for Web Dashboard
    strncpy(g_telemetry.label, winner_label, sizeof(g_telemetry.label) - 1);
    g_telemetry.confidence = winner_avg_conf;
    g_telemetry.x = winner_x;
    g_telemetry.y = winner_y;
    g_telemetry.weight_g = weight_g;
    g_telemetry.unit_price = unit_price;
    g_telemetry.total_price = total_price;
    g_telemetry.sanity_ok = sanity_ok;
    g_telemetry.detected = detected_any;
    g_telemetry.votes = winner_votes;
    g_telemetry.burst_total = BURST_SAMPLE_COUNT;
    g_telemetry.dsp_ms = valid_frames ? (total_dsp_ms / valid_frames) : 0;
    g_telemetry.infer_ms = valid_frames ? (total_infer_ms / valid_frames) : 0;
    g_telemetry.total_ms = t_burst_total;
    g_telemetry.fps = t_burst_total > 0 ? (1000.0f / (float)t_burst_total) : 0.0f;

    // 9. Output Result Summary to Serial Monitor
    Serial.println("  -----------------------------------------------------");
    if (detected_any) {
        Serial.printf("  >> BURST WINNER:   [%s] (%u/%u votes, %.0f%% consensus)\n",
                      winner_label, winner_votes, BURST_SAMPLE_COUNT,
                      ((float)winner_votes / BURST_SAMPLE_COUNT) * 100.0f);
        Serial.printf("  Average Confidence: %.1f%%\n", winner_avg_conf * 100.0f);
        Serial.printf("  Simulated Mass:     %.1f g (%.3f kg) [%s]\n", weight_g, weight_g / 1000.0f, mode_str);
        Serial.printf("  Unit Price:         $%.2f / kg\n", unit_price);
        Serial.printf("  TOTAL CHARGE:       $%.2f\n", total_price);
        Serial.printf("  Sanity Check:       %s\n", sanity_ok ? "PASSED (Valid fruit weight)" : "REJECTED (Weight out of range)");
    } else {
        Serial.println("  >> RESULT:          EMPTY SCALE (0/4 detections)");
    }
    Serial.printf("  Burst Duration:     %u ms (avg infer: %u ms/frame)\n",
                  t_burst_total, valid_frames ? (total_infer_ms / valid_frames) : 0);
    Serial.println("=======================================================\n");

    // 10. Forward Result to STM32 Master via UART
    if (detected_any) {
        STM32_Serial.write(0xAA);
        STM32_Serial.write(winner_label[0]); // 'a'=apple, 'o'=orange, 'p'=pear
        STM32_Serial.write((uint8_t)(winner_avg_conf * 100.0f));
        STM32_Serial.write(winner_votes); // Send consensus vote count
        STM32_Serial.write(0x55);
        delay(15);
        digitalWrite(STATUS_LED_PIN, HIGH);
    }

    return true;
}

// ----------------------------------------------------------------------------
// Signal Callback: Converts RGB888 Snapshot to Edge Impulse Floats
// ----------------------------------------------------------------------------
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) +
                              (snapshot_buf[pixel_ix + 1] << 8) +
                              snapshot_buf[pixel_ix];
        out_ptr_ix++;
        pixel_ix += 3;
        pixels_left--;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------
void loop() {
    handleSerialCommands();

    // Check for capture command from STM32 UART (0xAA)
    if (STM32_Serial.available()) {
        uint8_t byte = STM32_Serial.read();
        if (byte == 0xAA) {
            Serial.println("\n[UART EVENT] Received capture trigger (0xAA) from STM32 Master!");
            runBurstInference();
            return;
        }
    }

    // In continuous mode, run a burst periodically (every 2.5 seconds)
    if (g_continuous_mode) {
        static uint32_t s_last_burst_time = 0;
        if (millis() - s_last_burst_time >= 2500) {
            s_last_burst_time = millis();
            runBurstInference();
        }
    }

    delay(30); // Generous yield to FreeRTOS HTTP server and Wi-Fi stack
}

// ----------------------------------------------------------------------------
// Interactive Serial Commands
// ----------------------------------------------------------------------------
void handleSerialCommands() {
    if (Serial.available()) {
        char ch = Serial.read();
        if (ch == 'f' || ch == 'F') {
            g_flash_state = !g_flash_state;
            digitalWrite(FLASH_LED_PIN, g_flash_state ? HIGH : LOW);
            Serial.printf("[COMMAND] Flash LED is now %s\n", g_flash_state ? "ON" : "OFF");
        } else if (ch == 'm' || ch == 'M') {
            g_continuous_mode = !g_continuous_mode;
            Serial.printf("[COMMAND] Mode switched to: %s\n",
                          g_continuous_mode ? "CONTINUOUS (Burst every 2.5s)" : "TRIGGERED MODE (Press Space/trigger for burst)");
        } else if (ch == ' ' && !g_continuous_mode) {
            Serial.println("[COMMAND] Spacebar trigger: Running burst inference...");
            runBurstInference();
        } else if (ch == 'w' || ch == 'W') {
            if (g_weight_mode == MOCK_WEIGHT_RANDOM) g_weight_mode = MOCK_WEIGHT_FIXED_200G;
            else if (g_weight_mode == MOCK_WEIGHT_FIXED_200G) g_weight_mode = MOCK_WEIGHT_TOO_LIGHT;
            else g_weight_mode = MOCK_WEIGHT_RANDOM;

            const char* str = (g_weight_mode == MOCK_WEIGHT_RANDOM) ? "RANDOM REALISTIC" :
                              (g_weight_mode == MOCK_WEIGHT_FIXED_200G) ? "FIXED 200.0g" : "FAULT TEST 15.0g (fails sanity check)";
            Serial.printf("[COMMAND] Simulated Weight Mode: %s\n", str);
            runBurstInference();
        } else if (ch == 'b' || ch == 'B') {
            printBenchmarkReport();
        } else if (ch == '?') {
            Serial.println("\n--- INTERACTIVE COMMANDS ---");
            Serial.println("  [f] Toggle High-Power Flashlight LED");
            Serial.println("  [m] Toggle Continuous vs. Triggered mode");
            Serial.println("  [w] Cycle Mock Weight Mode (Random -> Fixed 200g -> Fault 15g)");
            Serial.println("  [Space] Trigger 4-frame Burst Inference (in Triggered mode)");
            Serial.println("  [b] Print comprehensive Benchmark Performance Summary");
            Serial.println("  [?] Print this help menu\n");
        }
    }
}

// ----------------------------------------------------------------------------
// Benchmark Report Summary
// ----------------------------------------------------------------------------
void printBenchmarkReport() {
    Serial.println("\n=======================================================");
    Serial.println("          HARDWARE INFERENCE BENCHMARK REPORT          ");
    Serial.println("=======================================================");
    Serial.printf("  Total Inferences:        %u\n", g_inference_count);
    if (g_inference_count > 0) {
        float avg_infer = (float)g_total_infer_time_ms / (float)g_inference_count;
        Serial.printf("  Avg Inference Latency:   %.1f ms\n", avg_infer);
        Serial.printf("  Min Inference Latency:   %u ms\n", g_min_infer_time_ms);
        Serial.printf("  Max Inference Latency:   %u ms\n", g_max_infer_time_ms);
        Serial.printf("  Estimated Model FPS:     %.1f FPS\n", 1000.0f / avg_infer);
    }
    Serial.printf("  Internal SRAM Free:      %u KB / %u KB\n", ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024);
    Serial.printf("  External PSRAM Free:     %u KB / %u KB\n", ESP.getFreePsram() / 1024, ESP.getPsramSize() / 1024);
    Serial.printf("  CPU Clock Speed:         %u MHz\n", getCpuFrequencyMhz());
    Serial.printf("  SoC Die Temperature:     %.1f C\n", temperatureRead());
    Serial.println("=======================================================\n");
}
