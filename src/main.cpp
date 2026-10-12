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

// --- Mock / Simulated STM32 Metrology & Pricing (VND) ---
#define UNIT_PRICE_APPLE    60000.0f  // 60,000 VND / kg
#define UNIT_PRICE_ORANGE   40000.0f  // 40,000 VND / kg
#define UNIT_PRICE_PEAR     70000.0f  // 70,000 VND / kg

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
    bool stm32_live;
};
static LiveScaleTelemetry g_telemetry = {"none", 0.0f, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, false, false, 0, 0, 0, 0.0f, 0, 4, false};

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
bool runBurstInference(float override_weight_g = -1.0f);
void processStm32Uart();
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr);
void startWebServer();
void handleSerialCommands();
void printBenchmarkReport();

// ----------------------------------------------------------------------------
// Embedded HTML Dashboard
// ----------------------------------------------------------------------------
static const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>Cân Thông Minh AI - Edge Retail Scale</title>
  <style>
    :root {
      --bg: #090d14;
      --panel: #111722;
      --panel-tint: #161f2e;
      --subtle: #1c2738;
      --text: #f8fafc;
      --text-muted: #8b99ad;
      --text-dim: #4b586d;
      --accent: #38bdf8;
      --accent-glow: rgba(56, 189, 248, 0.2);
      --green: #22c55e;
      --green-glow: rgba(34, 197, 94, 0.2);
      --amber: #f59e0b;
      --red: #ef4444;
      --mono: ui-monospace, "SF Mono", "Cascadia Code", "Courier New", monospace;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
    body {
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", sans-serif;
      background: var(--bg);
      color: var(--text);
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 14px;
    }

    .cockpit {
      width: 100%;
      max-width: 1060px;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }

    header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 6px 2px;
      flex-wrap: wrap;
      gap: 8px;
    }
    .brand-title {
      font-size: 1.15rem;
      font-weight: 700;
      letter-spacing: -0.3px;
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .brand-badge {
      font-size: 0.65rem;
      background: rgba(56, 189, 248, 0.15);
      color: var(--accent);
      padding: 2px 7px;
      border-radius: 12px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    .header-pills {
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 5px 12px;
      border-radius: 20px;
      font-size: 0.75rem;
      font-weight: 600;
      background: var(--panel);
      color: var(--text-muted);
    }
    .conn-dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: var(--green);
      box-shadow: 0 0 6px var(--green);
    }
    .pill-btn {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 5px 12px;
      border-radius: 20px;
      font-size: 0.75rem;
      font-weight: 600;
      background: var(--panel);
      border: 1px solid rgba(255, 255, 255, 0.08);
      color: var(--text);
      cursor: pointer;
      transition: all 0.15s ease;
    }
    .pill-btn:hover { background: var(--panel-tint); }
    .pill-btn.active {
      background: rgba(56, 189, 248, 0.15);
      border-color: var(--accent);
      color: var(--accent);
    }
    .demo-toggle-label {
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      font-size: 0.75rem;
      color: var(--text-dim);
      user-select: none;
    }
    .demo-toggle-label input { cursor: pointer; accent-color: var(--accent); }

    .cockpit-grid {
      display: grid;
      grid-template-columns: 1.15fr 1fr;
      gap: 16px;
      align-items: stretch;
    }
    @media (max-width: 820px) {
      .cockpit-grid { grid-template-columns: 1fr; gap: 14px; }
    }

    .monitor-col {
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .viewfinder {
      position: relative;
      width: 100%;
      aspect-ratio: 4 / 3;
      background: #020408;
      border-radius: 14px;
      overflow: hidden;
      display: flex;
      align-items: center;
      justify-content: center;
      box-shadow: 0 8px 24px rgba(0, 0, 0, 0.4);
    }
    #cam-feed {
      width: 100%;
      height: 100%;
      object-fit: contain;
      display: block;
    }
    
    .bracket {
      position: absolute;
      width: 20px;
      height: 20px;
      border: 2px solid rgba(56, 189, 248, 0.6);
      pointer-events: none;
    }
    .br-tl { top: 12px; left: 12px; border-right: none; border-bottom: none; }
    .br-tr { top: 12px; right: 12px; border-left: none; border-bottom: none; }
    .br-bl { bottom: 12px; left: 12px; border-right: none; border-top: none; }
    .br-br { bottom: 12px; right: 12px; border-left: none; border-top: none; }

    .shutter-flash {
      position: absolute;
      inset: 0;
      background: white;
      opacity: 0;
      pointer-events: none;
      transition: opacity 0.25s ease-out;
    }
    .shutter-flash.active { opacity: 0.85; transition: opacity 0.03s ease-in; }

    .hud-top {
      position: absolute;
      top: 10px;
      left: 10px;
      right: 10px;
      display: flex;
      justify-content: space-between;
      pointer-events: none;
    }
    .hud-tag {
      background: rgba(9, 13, 20, 0.75);
      backdrop-filter: blur(8px);
      padding: 4px 10px;
      border-radius: 6px;
      font-size: 0.72rem;
      font-family: var(--mono);
      font-weight: 600;
      color: var(--text-muted);
    }
    .hud-bottom {
      position: absolute;
      bottom: 10px;
      left: 10px;
      right: 10px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      pointer-events: none;
    }
    .live-target-badge {
      display: flex;
      align-items: center;
      gap: 6px;
      background: rgba(9, 13, 20, 0.85);
      backdrop-filter: blur(10px);
      padding: 6px 12px;
      border-radius: 8px;
      font-size: 0.85rem;
      font-weight: 700;
      color: var(--text);
    }

    .action-strip {
      display: grid;
      grid-template-columns: 2fr 1fr 1fr;
      gap: 8px;
    }
    .act-btn {
      padding: 12px 10px;
      border-radius: 10px;
      border: none;
      background: var(--panel);
      color: var(--text);
      font-size: 0.86rem;
      font-weight: 600;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      cursor: pointer;
      transition: all 0.15s ease;
      user-select: none;
    }
    .act-btn:hover { background: var(--panel-tint); transform: translateY(-1px); }
    .act-btn:active { transform: translateY(1px); }
    .act-btn.primary {
      background: linear-gradient(135deg, #0284c7, #0369a1);
      color: white;
      box-shadow: 0 4px 12px rgba(2, 132, 199, 0.35);
    }
    .act-btn.primary:hover { background: linear-gradient(135deg, #0369a1, #0284c7); }
    .act-btn.flash-active {
      background: rgba(245, 158, 11, 0.2);
      color: #fde68a;
    }

    .console-col {
      display: flex;
      flex-direction: column;
      gap: 12px;
      justify-content: space-between;
    }

    .metrology-block {
      background: var(--panel);
      border-radius: 14px;
      padding: 16px 18px;
      display: flex;
      flex-direction: column;
      gap: 12px;
    }
    .metrology-row-top {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
    }
    .metrology-title {
      font-size: 0.72rem;
      text-transform: uppercase;
      letter-spacing: 0.8px;
      font-weight: 700;
      color: var(--text-dim);
    }
    .mass-hero-wrap {
      display: flex;
      align-items: baseline;
      gap: 8px;
    }
    .mass-val {
      font-family: var(--mono);
      font-size: 3.4rem;
      font-weight: 800;
      line-height: 1;
      letter-spacing: -1.5px;
      font-variant-numeric: tabular-nums;
      color: var(--text);
    }
    .mass-unit {
      font-size: 1.1rem;
      font-weight: 700;
      color: var(--text-muted);
    }
    .mass-kg-sub {
      font-family: var(--mono);
      font-size: 1rem;
      font-weight: 600;
      color: var(--text-dim);
      text-align: right;
    }

    .price-line {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      padding-top: 10px;
      border-top: 1px solid rgba(255, 255, 255, 0.06);
    }
    .price-total-wrap {
      display: flex;
      flex-direction: column;
    }
    .price-total-val {
      font-family: var(--mono);
      font-size: 2.3rem;
      font-weight: 800;
      color: var(--accent);
      line-height: 1;
    }
    .price-rate-wrap {
      text-align: right;
      font-size: 0.85rem;
      color: var(--text-muted);
    }
    .edit-price-inline {
      color: var(--accent);
      font-size: 0.76rem;
      cursor: pointer;
      text-decoration: underline;
      margin-left: 4px;
    }

    .price-editor-tray {
      display: none;
      background: #141b26;
      border: 1px solid rgba(56, 189, 248, 0.25);
      border-radius: 12px;
      padding: 12px 14px;
      flex-direction: column;
      gap: 10px;
      animation: fadeIn 0.2s ease;
    }
    .price-editor-tray.open { display: flex; }
    @keyframes fadeIn {
      from { opacity: 0; transform: translateY(-4px); }
      to { opacity: 1; transform: translateY(0); }
    }
    .price-tray-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
      font-size: 0.75rem;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
      color: var(--text-muted);
    }
    .unit-switch-group {
      display: flex;
      gap: 4px;
      background: rgba(0, 0, 0, 0.3);
      padding: 2px;
      border-radius: 6px;
    }
    .unit-opt-btn {
      padding: 3px 8px;
      font-size: 0.7rem;
      font-weight: 700;
      border: none;
      background: none;
      color: var(--text-dim);
      border-radius: 4px;
      cursor: pointer;
    }
    .unit-opt-btn.selected {
      background: var(--accent);
      color: #0b0f17;
    }
    .price-inputs-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
    }
    @media (max-width: 480px) {
      .price-inputs-grid { grid-template-columns: 1fr; }
    }
    .price-card {
      background: rgba(255, 255, 255, 0.02);
      border-radius: 8px;
      padding: 8px 10px;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }
    .price-card-label {
      font-size: 0.75rem;
      font-weight: 600;
      display: flex;
      align-items: center;
      gap: 4px;
    }
    .price-stepper {
      display: flex;
      align-items: center;
      gap: 4px;
    }
    .step-btn {
      width: 26px;
      height: 26px;
      border-radius: 6px;
      border: 1px solid rgba(255, 255, 255, 0.1);
      background: var(--panel);
      color: var(--text);
      font-size: 0.95rem;
      font-weight: 700;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    .step-btn:hover { background: var(--panel-tint); }
    .price-input-box {
      flex: 1;
      width: 100%;
      min-width: 50px;
      background: #090d14;
      border: 1px solid rgba(255, 255, 255, 0.15);
      border-radius: 6px;
      color: var(--text);
      font-family: var(--mono);
      font-size: 0.88rem;
      font-weight: 700;
      padding: 4px 6px;
      text-align: center;
    }
    .price-tray-footer {
      display: flex;
      justify-content: space-between;
      align-items: center;
      font-size: 0.72rem;
      color: var(--text-dim);
    }
    .reset-link {
      color: var(--text-muted);
      cursor: pointer;
      text-decoration: underline;
    }
    .reset-link:hover { color: var(--accent); }

    .fruit-line {
      display: flex;
      align-items: center;
      justify-content: space-between;
      background: var(--panel);
      border-radius: 12px;
      padding: 12px 14px;
    }
    .fruit-info-left {
      display: flex;
      align-items: center;
      gap: 10px;
    }
    .fruit-icon { font-size: 1.5rem; line-height: 1; }
    .fruit-name { font-size: 1rem; font-weight: 700; }
    .fruit-consensus-txt { font-size: 0.74rem; color: var(--text-muted); }
    .sanity-tag {
      font-size: 0.75rem;
      font-weight: 700;
      padding: 4px 10px;
      border-radius: 6px;
    }
    .sanity-pass { background: rgba(34, 197, 94, 0.15); color: var(--green); }
    .sanity-idle { background: rgba(255, 255, 255, 0.05); color: var(--text-dim); }
    .sanity-fail { background: rgba(239, 68, 68, 0.15); color: var(--red); }

    .telemetry-block {
      background: var(--panel);
      border-radius: 14px;
      padding: 14px 16px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .telem-header {
      display: flex;
      align-items: center;
      justify-content: space-between;
    }
    .telem-title {
      font-size: 0.72rem;
      text-transform: uppercase;
      font-weight: 700;
      letter-spacing: 0.8px;
      color: var(--text-dim);
    }
    .telem-pulse {
      font-size: 0.7rem;
      font-family: var(--mono);
      color: var(--accent);
      display: flex;
      align-items: center;
      gap: 4px;
    }

    .telem-grid {
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      gap: 8px;
    }
    @media (max-width: 480px) {
      .telem-grid { grid-template-columns: repeat(2, 1fr); }
    }
    .telem-cell {
      background: rgba(255, 255, 255, 0.02);
      border-radius: 8px;
      padding: 8px 10px;
      display: flex;
      flex-direction: column;
      gap: 2px;
      transition: background 0.3s ease;
    }
    .telem-cell.flash-update {
      background: rgba(56, 189, 248, 0.15);
    }
    .telem-k {
      font-size: 0.68rem;
      color: var(--text-dim);
      font-weight: 600;
      text-transform: uppercase;
    }
    .telem-v {
      font-family: var(--mono);
      font-size: 0.88rem;
      font-weight: 700;
      color: var(--text);
    }

    .consensus-bar-wrap {
      width: 100%;
      height: 4px;
      background: rgba(255, 255, 255, 0.05);
      border-radius: 2px;
      overflow: hidden;
      margin-top: 2px;
    }
    .consensus-bar-fill {
      height: 100%;
      background: linear-gradient(90deg, var(--accent), var(--green));
      width: 0%;
      transition: width 0.3s ease;
    }

    footer {
      text-align: center;
      font-size: 0.72rem;
      color: var(--text-dim);
      padding: 6px 0;
    }
  </style>
</head>
<body>

  <div class="cockpit">
    <header>
      <div class="brand-title">
        <span>Cân Thông Minh AI</span>
        <span class="brand-badge">ESP32 + STM32</span>
      </div>
      <div class="header-pills">
        <button class="pill-btn" id="btn-price-menu" onclick="togglePriceMenu()">
          <span>🏷️</span>
          <span>Bảng Giá (VND)</span>
        </button>
        <div class="status-pill">
          <span class="conn-dot" id="conn-dot"></span>
          <span id="conn-status">SoftAP Live</span>
        </div>
        <label class="demo-toggle-label">
          <input type="checkbox" id="demo-check" onchange="toggleDemo(this.checked)">
          <span>Mô phỏng</span>
        </label>
      </div>
    </header>

    <div class="cockpit-grid">
      <div class="monitor-col">
        <div class="viewfinder" onclick="triggerInference()">
          <img id="cam-feed" src="/capture" alt="Camera Stream" onerror="handleImageError()">
          <div class="shutter-flash" id="shutter-fx"></div>

          <div class="bracket br-tl"></div>
          <div class="bracket br-tr"></div>
          <div class="bracket br-bl"></div>
          <div class="bracket br-br"></div>

          <div class="hud-top">
            <div class="hud-tag">OV2640 • 320x240</div>
            <div class="hud-tag" id="cam-fps-hud">-- FPS</div>
          </div>

          <div class="hud-bottom">
            <div class="live-target-badge">
              <span id="target-icon">⚖️</span>
              <span id="target-name">Đang quét đĩa cân...</span>
            </div>
            <div class="hud-tag" id="target-conf">-- %</div>
          </div>
        </div>

        <div class="action-strip">
          <button class="act-btn primary" onclick="triggerInference()">
            <span>⚡</span>
            <span>Quét Nhận Diện (AI)</span>
          </button>
          <button class="act-btn" id="btn-flash" onclick="toggleFlash()">
            <span>💡</span>
            <span id="lbl-flash">Đèn Flash</span>
          </button>
          <button class="act-btn" onclick="cycleWeight()">
            <span>⚖️</span>
            <span>Đổi Khối Lượng</span>
          </button>
        </div>
      </div>

      <div class="console-col">
        <div class="metrology-block">
          <div class="metrology-row-top">
            <span class="metrology-title">Khối lượng cân (Mass)</span>
            <span id="val-source-pill" style="font-size:0.7rem; padding:2px 8px; border-radius:10px; background:rgba(255,255,255,0.08); color:#94a3b8; font-weight:600;">Mô phỏng</span>
            <span class="mass-kg-sub" id="val-mass-kg">0,000 kg</span>
          </div>
          <div class="mass-hero-wrap">
            <span class="mass-val" id="val-mass">0.0</span>
            <span class="mass-unit">g</span>
          </div>
          
          <div class="price-line">
            <div class="price-total-wrap">
              <span class="metrology-title">Thành tiền (VND)</span>
              <span class="price-total-val" id="val-total">0 ₫</span>
            </div>
            <div class="price-rate-wrap">
              <div id="val-unit-rate">
                Đơn giá: 0 ₫ / kg
                <span class="edit-price-inline" onclick="togglePriceMenu()">[Sửa]</span>
              </div>
              <div style="font-size:0.75rem; color:var(--text-dim);" id="val-tare-state">Đã trừ bì (Tare 0g)</div>
            </div>
          </div>
        </div>

        <div class="price-editor-tray" id="price-tray">
          <div class="price-tray-header">
            <span>Cài Đặt Đơn Giá Nhanh (VND)</span>
            <div class="unit-switch-group">
              <button class="unit-opt-btn selected" id="unit-kg-btn" onclick="setPriceUnit('kg')">/ kg</button>
              <button class="unit-opt-btn" id="unit-100g-btn" onclick="setPriceUnit('100g')">/ 100g</button>
              <button class="unit-opt-btn" id="unit-g-btn" onclick="setPriceUnit('g')">/ g</button>
            </div>
          </div>

          <div class="price-inputs-grid">
            <div class="price-card">
              <span class="price-card-label">🍎 Táo (Apple)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('apple', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-apple" oninput="onCustomPriceChange('apple', this.value)">
                <button class="step-btn" onclick="stepPrice('apple', 1000)">+</button>
              </div>
            </div>

            <div class="price-card">
              <span class="price-card-label">🍊 Cam (Orange)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('orange', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-orange" oninput="onCustomPriceChange('orange', this.value)">
                <button class="step-btn" onclick="stepPrice('orange', 1000)">+</button>
              </div>
            </div>

            <div class="price-card">
              <span class="price-card-label">🍐 Lê (Pear)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('pear', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-pear" oninput="onCustomPriceChange('pear', this.value)">
                <button class="step-btn" onclick="stepPrice('pear', 1000)">+</button>
              </div>
            </div>
          </div>

          <div class="price-tray-footer">
            <span>Tự động cập nhật thành tiền tức thì</span>
            <span class="reset-link" onclick="resetDefaultPrices()">Giá mặc định</span>
          </div>
        </div>

        <div class="fruit-line">
          <div class="fruit-info-left">
            <span class="fruit-icon" id="fruit-emoji">⚖️</span>
            <div>
              <div class="fruit-name" id="fruit-title">Cân trống (Empty)</div>
              <div class="fruit-consensus-txt" id="fruit-sub">Đĩa cân trống • Sẵn sàng đặt quả</div>
              <div class="consensus-bar-wrap">
                <div class="consensus-bar-fill" id="consensus-bar"></div>
              </div>
            </div>
          </div>
          <span class="sanity-tag sanity-idle" id="sanity-pill">ĐÃ BÌ (TARE)</span>
        </div>

        <div class="telemetry-block">
          <div class="telem-header">
            <span class="telem-title">Thông số phần cứng & TinyML AI</span>
            <span class="telem-pulse" id="telem-live-pill">● HOẠT ĐỘNG</span>
          </div>

          <div class="telem-grid">
            <div class="telem-cell" id="c-infer">
              <span class="telem-k">Độ trễ AI</span>
              <span class="telem-v" id="t-infer">-- ms</span>
            </div>
            <div class="telem-cell" id="c-dsp">
              <span class="telem-k">Xử lý ảnh DSP</span>
              <span class="telem-v" id="t-dsp">-- ms</span>
            </div>
            <div class="telem-cell" id="c-fps">
              <span class="telem-k">Tốc độ khung</span>
              <span class="telem-v" id="t-fps">-- FPS</span>
            </div>
            <div class="telem-cell" id="c-consensus">
              <span class="telem-k">Đồng thuận mẫu</span>
              <span class="telem-v" id="t-votes">--</span>
            </div>
            <div class="telem-cell" id="c-temp">
              <span class="telem-k">Nhiệt độ chip</span>
              <span class="telem-v" id="t-temp">-- °C</span>
            </div>
            <div class="telem-cell" id="c-psram">
              <span class="telem-k">PSRAM trống</span>
              <span class="telem-v" id="t-psram">-- KB</span>
            </div>
          </div>
        </div>
      </div>
    </div>

    <footer>
      Hệ Thống Cân Tính Tiền Thông Minh Edge AI • ESP32-CAM (FOMO INT8) & STM32 (HX711)
    </footer>
  </div>

  <script>
    let isLiveConnected = false;
    let isDemoMode = false;
    let flashActive = false;
    let demoIndex = 0;
    let priceUnit = 'kg';

    const DEFAULT_PRICES = {
      apple: 60000,
      orange: 40000,
      pear: 70000
    };

    let customPrices = { ...DEFAULT_PRICES };
    try {
      const saved = localStorage.getItem('scale_prices_vnd_kg');
      if (saved) customPrices = JSON.parse(saved);
      const savedUnit = localStorage.getItem('scale_price_vnd_unit');
      if (savedUnit) priceUnit = savedUnit;
    } catch (e) {}

    const FRUITS = {
      apple: { name: "Táo (Apple)", emoji: "🍎" },
      orange: { name: "Cam (Orange)", emoji: "🍊" },
      pear: { name: "Lê (Pear)", emoji: "🍐" },
      none: { name: "Cân trống", emoji: "⚖️" }
    };

    const DEMO_SAMPLES = [
      { label: "apple", confidence: 0.945, weight: 218.4, votes: 4, dsp_ms: 8, infer_ms: 42, temp: 48.2, psram_kb: 3840, heap_kb: 172 },
      { label: "orange", confidence: 0.892, weight: 184.2, votes: 4, dsp_ms: 9, infer_ms: 41, temp: 48.6, psram_kb: 3840, heap_kb: 171 },
      { label: "pear", confidence: 0.961, weight: 235.8, votes: 4, dsp_ms: 8, infer_ms: 44, temp: 49.1, psram_kb: 3840, heap_kb: 170 },
      { label: "none", confidence: 0.000, weight: 0.0, votes: 0, dsp_ms: 8, infer_ms: 39, temp: 47.9, psram_kb: 3840, heap_kb: 174 }
    ];

    let currentTelemetry = DEMO_SAMPLES[demoIndex];

    function formatVND(val) {
      return Math.round(val).toLocaleString('vi-VN') + ' ₫';
    }

    function syncPriceInputs() {
      ['apple', 'orange', 'pear'].forEach(k => {
        const inp = document.getElementById('inp-price-' + k);
        if (inp) {
          const displayedPrice = convertRateForUnit(customPrices[k], priceUnit);
          inp.value = Math.round(displayedPrice);
        }
      });
      ['kg', '100g', 'g'].forEach(u => {
        const btn = document.getElementById('unit-' + u + '-btn');
        if (btn) btn.classList.toggle('selected', u === priceUnit);
      });
    }

    function convertRateForUnit(pricePerKg, unit) {
      if (unit === 'g') return pricePerKg / 1000.0;
      if (unit === '100g') return pricePerKg / 10.0;
      return pricePerKg;
    }

    function convertToPricePerKg(val, unit) {
      if (unit === 'g') return val * 1000.0;
      if (unit === '100g') return val * 10.0;
      return val;
    }

    function togglePriceMenu() {
      const tray = document.getElementById('price-tray');
      const btn = document.getElementById('btn-price-menu');
      const isOpen = tray.classList.toggle('open');
      btn.classList.toggle('active', isOpen);
      if (isOpen) syncPriceInputs();
    }

    function setPriceUnit(u) {
      priceUnit = u;
      try { localStorage.setItem('scale_price_vnd_unit', u); } catch(e){}
      syncPriceInputs();
      renderData(currentTelemetry);
    }

    function onCustomPriceChange(fruit, val) {
      const num = parseFloat(val);
      if (!isNaN(num) && num >= 0) {
        customPrices[fruit] = convertToPricePerKg(num, priceUnit);
        savePrices();
        renderData(currentTelemetry);
      }
    }

    function stepPrice(fruit, delta) {
      const currentRate = convertRateForUnit(customPrices[fruit], priceUnit);
      let stepDelta = delta;
      if (priceUnit === '100g') stepDelta = delta / 10.0;
      if (priceUnit === 'g') stepDelta = Math.max(1, delta / 1000.0);
      
      const newRate = Math.max(0, currentRate + stepDelta);
      customPrices[fruit] = convertToPricePerKg(newRate, priceUnit);
      savePrices();
      syncPriceInputs();
      renderData(currentTelemetry);
    }

    function resetDefaultPrices() {
      customPrices = { ...DEFAULT_PRICES };
      savePrices();
      syncPriceInputs();
      renderData(currentTelemetry);
    }

    function savePrices() {
      try { localStorage.setItem('scale_prices_vnd_kg', JSON.stringify(customPrices)); } catch(e){}
    }

    function createSvgView(label) {
      const colors = { apple: "#ef4444", orange: "#f97316", pear: "#84cc16", none: "#334155" };
      const c = colors[label] || "#334155";
      const icon = FRUITS[label] ? FRUITS[label].emoji : "⚖️";
      return `data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="320" height="240" viewBox="0 0 320 240"><rect width="320" height="240" fill="%23020408"/><circle cx="160" cy="120" r="52" fill="${encodeURIComponent(c)}" opacity="0.3"/><circle cx="160" cy="120" r="38" fill="${encodeURIComponent(c)}" opacity="0.75"/><text x="160" y="132" font-size="38" text-anchor="middle" font-family="sans-serif">${encodeURIComponent(icon)}</text></svg>`;
    }

    function updateFrame() {
      if (isDemoMode) return;
      const view = document.getElementById('cam-feed');
      const img = new Image();
      img.onload = () => {
        view.src = img.src;
        setConnectionState(true);
        setTimeout(updateFrame, 80);
      };
      img.onerror = () => {
        handleImageError();
        setTimeout(updateFrame, 1500);
      };
      img.src = '/capture?t=' + Date.now();
    }

    function handleImageError() {
      if (!isDemoMode) {
        setConnectionState(false);
        renderData(DEMO_SAMPLES[demoIndex]);
      }
    }

    function poll() {
      if (isDemoMode) {
        renderData(DEMO_SAMPLES[demoIndex]);
        return;
      }
      fetch('/status')
        .then(r => r.json())
        .then(d => { setConnectionState(true); renderData(d); })
        .catch(() => { setConnectionState(false); renderData(DEMO_SAMPLES[demoIndex]); });
    }

    function renderData(d) {
      currentTelemetry = d;
      const label = (d.label || "none").toLowerCase();
      const meta = FRUITS[label] || FRUITS["none"];
      const isFruit = (label !== "none" && label !== "empty_scale");
      const massG = d.weight > 0 ? d.weight : 0.0;

      const basePricePerKg = isFruit ? (customPrices[label] || DEFAULT_PRICES[label] || 60000) : 0;
      const displayRate = convertRateForUnit(basePricePerKg, priceUnit);
      const totalDue = (massG / 1000.0) * basePricePerKg;

      document.getElementById('val-mass').innerText = massG.toFixed(1);
      document.getElementById('val-mass-kg').innerText = (massG / 1000.0).toFixed(3).replace('.', ',') + " kg";
      document.getElementById('val-total').innerText = totalDue > 0 ? formatVND(totalDue) : "0 ₫";
      
      const srcPill = document.getElementById('val-source-pill');
      if (srcPill) {
        if (d.stm32_live) {
          srcPill.innerText = "STM32 HX711";
          srcPill.style.color = "var(--green)";
          srcPill.style.background = "rgba(34, 197, 94, 0.18)";
        } else {
          srcPill.innerText = "Mô phỏng";
          srcPill.style.color = "#94a3b8";
          srcPill.style.background = "rgba(255, 255, 255, 0.08)";
        }
      }

      const rateSuffix = priceUnit === 'g' ? '/ g' : (priceUnit === '100g' ? '/ 100g' : '/ kg');
      document.getElementById('val-unit-rate').innerHTML = 
        `Đơn giá: ${formatVND(displayRate)} ${rateSuffix} <span class="edit-price-inline" onclick="togglePriceMenu()">[Sửa]</span>`;
      document.getElementById('val-tare-state').innerText = isFruit ? "Cảm biến khối lượng đang đo" : "Đã trừ bì (Tare 0g)";

      document.getElementById('target-icon').innerText = meta.emoji;
      document.getElementById('target-name').innerText = isFruit ? meta.name : "Đang quét đĩa cân...";
      document.getElementById('target-conf').innerText = isFruit ? (d.confidence * 100).toFixed(0) + "%" : "--";
      const totalTime = (d.dsp_ms || 8) + (d.infer_ms || 42);
      const fps = totalTime > 0 ? (1000 / totalTime).toFixed(0) : "20";
      document.getElementById('cam-fps-hud').innerText = fps + " FPS";

      document.getElementById('fruit-emoji').innerText = meta.emoji;
      document.getElementById('fruit-title').innerText = isFruit ? meta.name : "Cân trống";
      const votes = d.votes || (isFruit ? 4 : 0);
      const burstTotal = d.burst_total || 4;
      const pct = Math.round((votes / burstTotal) * 100);
      document.getElementById('fruit-sub').innerText = isFruit 
        ? `${votes}/${burstTotal} mẫu đồng thuận (${pct}%)`
        : "Đĩa cân trống • Sẵn sàng đặt quả";
      document.getElementById('consensus-bar').style.width = isFruit ? pct + "%" : "0%";

      const sanityPill = document.getElementById('sanity-pill');
      if (!isFruit) {
        sanityPill.className = "sanity-tag sanity-idle";
        sanityPill.innerText = "ĐÃ BÌ (TARE)";
      } else if (massG >= 50 && massG <= 600) {
        sanityPill.className = "sanity-tag sanity-pass";
        sanityPill.innerText = "HỢP LỆ";
      } else {
        sanityPill.className = "sanity-tag sanity-fail";
        sanityPill.innerText = "SAI PHẠM VI";
      }

      document.getElementById('t-infer').innerText = (d.infer_ms || 42) + " ms";
      document.getElementById('t-dsp').innerText = (d.dsp_ms || 8) + " ms";
      document.getElementById('t-fps').innerText = fps + " FPS";
      document.getElementById('t-votes').innerText = `${votes}/${burstTotal} (${pct}%)`;
      document.getElementById('t-temp').innerText = (d.temp ? d.temp.toFixed(1) : "48.2") + " °C";
      document.getElementById('t-psram').innerText = (d.psram_kb || 3840) + " KB";

      if (isDemoMode) {
        document.getElementById('cam-feed').src = createSvgView(label);
      }
    }

    function setConnectionState(connected) {
      isLiveConnected = connected;
      const dot = document.getElementById('conn-dot');
      const text = document.getElementById('conn-status');
      if (connected) {
        dot.style.background = "var(--green)";
        dot.style.boxShadow = "0 0 6px var(--green)";
        text.innerText = "SoftAP Live";
        isDemoMode = false;
        document.getElementById('demo-check').checked = false;
      } else {
        dot.style.background = "var(--amber)";
        dot.style.boxShadow = "0 0 6px var(--amber)";
        text.innerText = "Mô phỏng";
      }
    }

    function toggleDemo(enabled) {
      isDemoMode = enabled;
      if (enabled) {
        renderData(DEMO_SAMPLES[demoIndex]);
      } else {
        updateFrame();
        poll();
      }
    }

    function triggerInference() {
      const fx = document.getElementById('shutter-fx');
      fx.classList.add('active');
      setTimeout(() => fx.classList.remove('active'), 120);

      const cells = document.querySelectorAll('.telem-cell');
      cells.forEach(c => c.classList.add('flash-update'));
      setTimeout(() => cells.forEach(c => c.classList.remove('flash-update')), 350);

      if (isDemoMode) {
        demoIndex = (demoIndex + 1) % DEMO_SAMPLES.length;
        renderData(DEMO_SAMPLES[demoIndex]);
      } else {
        fetch('/trigger').catch(() => {});
        setTimeout(poll, 250);
      }
    }

    function toggleFlash() {
      flashActive = !flashActive;
      const btn = document.getElementById('btn-flash');
      const lbl = document.getElementById('lbl-flash');
      btn.classList.toggle('flash-active', flashActive);
      lbl.innerText = flashActive ? "Đèn BẬT" : "Đèn Flash";
      if (!isDemoMode) fetch('/flash').catch(() => {});
    }

    function cycleWeight() {
      if (isDemoMode) {
        demoIndex = (demoIndex + 1) % DEMO_SAMPLES.length;
        renderData(DEMO_SAMPLES[demoIndex]);
      } else {
        fetch('/cycle_weight').then(poll).catch(() => {});
      }
    }

    syncPriceInputs();
    setInterval(poll, 350);
    poll();
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
    char json[512];
    snprintf(json, sizeof(json),
             "{\"label\":\"%s\",\"confidence\":%.3f,\"x\":%u,\"y\":%u,\"weight\":%.1f,"
             "\"unit_price\":%.2f,\"total_price\":%.2f,\"sanity_ok\":%s,\"detected\":%s,"
             "\"dsp_ms\":%u,\"infer_ms\":%u,\"total_ms\":%u,\"fps\":%.1f,"
             "\"votes\":%u,\"burst_total\":%u,\"stm32_live\":%s,"
             "\"temp\":%.1f,\"psram_kb\":%u,\"heap_kb\":%u}",
             g_telemetry.label, g_telemetry.confidence, g_telemetry.x, g_telemetry.y,
             g_telemetry.weight_g, g_telemetry.unit_price, g_telemetry.total_price,
             g_telemetry.sanity_ok ? "true" : "false",
             g_telemetry.detected ? "true" : "false",
             g_telemetry.dsp_ms, g_telemetry.infer_ms, g_telemetry.total_ms, g_telemetry.fps,
             g_telemetry.votes, g_telemetry.burst_total,
             g_telemetry.stm32_live ? "true" : "false",
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
bool runBurstInference(float override_weight_g) {
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
        if (override_weight_g > 0.0f) {
            weight_g = override_weight_g;
            mode_str = "STM32 HX711 Live";
            g_telemetry.stm32_live = true;
        } else if (g_weight_mode == MOCK_WEIGHT_FIXED_200G) {
            weight_g = 200.0f;
            mode_str = "Fixed 200g (Mock)";
            g_telemetry.stm32_live = false;
        } else if (g_weight_mode == MOCK_WEIGHT_TOO_LIGHT) {
            weight_g = 15.0f;
            mode_str = "Fault Test 15g (Mock)";
            g_telemetry.stm32_live = false;
        } else {
            // Realistic random weight with 0.1g resolution
            if (strcmp(winner_label, "apple") == 0)       weight_g = (float)random(1600, 2300) / 10.0f;
            else if (strcmp(winner_label, "orange") == 0) weight_g = (float)random(1400, 2000) / 10.0f;
            else if (strcmp(winner_label, "pear") == 0)   weight_g = (float)random(1800, 2700) / 10.0f;
            else                                          weight_g = (float)random(1500, 2200) / 10.0f;
            mode_str = "Random Realistic (Mock)";
            g_telemetry.stm32_live = false;
        }

        unit_price = 50000.0f;
        if (strcmp(winner_label, "apple") == 0)       unit_price = UNIT_PRICE_APPLE;
        else if (strcmp(winner_label, "orange") == 0) unit_price = UNIT_PRICE_ORANGE;
        else if (strcmp(winner_label, "pear") == 0)   unit_price = UNIT_PRICE_PEAR;

        total_price = (weight_g / 1000.0f) * unit_price;
        sanity_ok = (weight_g >= 50.0f && weight_g <= 600.0f);
    } else {
        g_telemetry.stm32_live = (override_weight_g > 0.0f);
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
        Serial.printf("  Mass:               %.1f g (%.3f kg) [%s]\n", weight_g, weight_g / 1000.0f, mode_str);
        Serial.printf("  Unit Price:         %.0f VND / kg\n", unit_price);
        Serial.printf("  TOTAL CHARGE:       %.0f VND\n", total_price);
        Serial.printf("  Sanity Check:       %s\n", sanity_ok ? "PASSED (Valid fruit weight)" : "REJECTED (Weight out of range)");
    } else {
        Serial.println("  >> RESULT:          EMPTY SCALE (0/4 detections)");
    }
    Serial.printf("  Burst Duration:     %u ms (avg infer: %u ms/frame)\n",
                  t_burst_total, valid_frames ? (total_infer_ms / valid_frames) : 0);
    Serial.println("=======================================================\n");

    // 10. Forward Result to STM32 Master via UART
    if (detected_any) {
        // Binary framed packet for embedded STM32 parsers
        STM32_Serial.write(0xAA);
        STM32_Serial.write(winner_label[0]); // 'a'=apple, 'o'=orange, 'p'=pear
        STM32_Serial.write((uint8_t)(winner_avg_conf * 100.0f));
        STM32_Serial.write(winner_votes); // Send consensus vote count
        STM32_Serial.write(0x55);

        // Human-readable ASCII string for string-based STM32 firmware
        STM32_Serial.printf("RES:%s,%d,%.1f\r\n", winner_label, (int)(winner_avg_conf * 100.0f), weight_g);

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
// Robust Multi-Format STM32 Inter-Chip UART Parser
// Supports:
//   1. Binary framed packets with weight in decigrams or grams
//   2. Human-readable ASCII lines: "W:245.3\n" or "245.3\n"
//   3. Single trigger byte: 0xAA
// ----------------------------------------------------------------------------
static uint8_t  s_stm32_rx_buf[64];
static size_t   s_stm32_rx_len = 0;
static uint32_t s_stm32_last_rx_ms = 0;
static float    s_last_live_weight_g = -1.0f;
static bool     s_has_live_stm32 = false;

void processStm32Uart() {
    while (STM32_Serial.available()) {
        uint8_t b = STM32_Serial.read();
        s_stm32_last_rx_ms = millis();

        // 1. Binary Packet Starting with 0xAA
        if (s_stm32_rx_len == 0 && b == 0xAA) {
            s_stm32_rx_buf[s_stm32_rx_len++] = b;
            continue;
        }

        if (s_stm32_rx_len > 0 && s_stm32_rx_buf[0] == 0xAA) {
            if (s_stm32_rx_len < sizeof(s_stm32_rx_buf)) {
                s_stm32_rx_buf[s_stm32_rx_len++] = b;
            }

            // Check for Binary Packet End (0x55)
            if (b == 0x55) {
                float measured_weight = -1.0f;
                bool valid = false;

                // Format 1: [0xAA][CMD][MSB][LSB][CHK][0x55] (6 bytes)
                if (s_stm32_rx_len == 6) {
                    uint8_t cmd = s_stm32_rx_buf[1];
                    uint16_t raw_w = ((uint16_t)s_stm32_rx_buf[2] << 8) | s_stm32_rx_buf[3];
                    uint8_t chk = s_stm32_rx_buf[4];
                    uint8_t expected_chk = cmd ^ s_stm32_rx_buf[2] ^ s_stm32_rx_buf[3];
                    if (chk == expected_chk || chk == 0) {
                        measured_weight = (float)raw_w / 10.0f;
                        valid = true;
                    }
                }
                // Format 2: [0xAA][MSB][LSB][0x55] (4 bytes)
                else if (s_stm32_rx_len == 4) {
                    uint16_t raw_w = ((uint16_t)s_stm32_rx_buf[1] << 8) | s_stm32_rx_buf[2];
                    measured_weight = (raw_w > 10000) ? (float)raw_w : ((float)raw_w / 10.0f);
                    valid = true;
                }

                if (valid && measured_weight > 0.0f) {
                    s_has_live_stm32 = true;
                    s_last_live_weight_g = measured_weight;
                    Serial.printf("\n[STM32 UART] Binary packet parsed! Live HX711 Mass: %.1f g\n", measured_weight);
                    s_stm32_rx_len = 0;
                    runBurstInference(measured_weight);
                    return;
                }
                s_stm32_rx_len = 0;
            }
            continue;
        }

        // 2. ASCII text string line (ends with '\n' or '\r')
        if (b == '\n' || b == '\r') {
            if (s_stm32_rx_len > 0) {
                s_stm32_rx_buf[s_stm32_rx_len] = '\0';
                char* str = (char*)s_stm32_rx_buf;
                // Search for numeric digits (e.g. "W:245.3" or "245.3")
                char* p = str;
                while (*p && !isdigit(*p) && *p != '.') p++;
                if (*p) {
                    float w = atof(p);
                    if (w > 0.0f) {
                        s_has_live_stm32 = true;
                        s_last_live_weight_g = w;
                        Serial.printf("\n[STM32 UART] ASCII packet parsed: '%s' -> Live HX711 Mass: %.1f g\n", str, w);
                        s_stm32_rx_len = 0;
                        runBurstInference(w);
                        return;
                    }
                }
                s_stm32_rx_len = 0;
            }
        } else if (b >= 32 && b <= 126) {
            // Printable ASCII character
            if (s_stm32_rx_len < sizeof(s_stm32_rx_buf) - 1) {
                s_stm32_rx_buf[s_stm32_rx_len++] = b;
            }
        }
    }

    // 3. Timeout check for Single-Byte Trigger:
    // If we received just 0xAA and nothing else arrived within 25ms, it's a single-byte trigger!
    if (s_stm32_rx_len == 1 && s_stm32_rx_buf[0] == 0xAA && (millis() - s_stm32_last_rx_ms >= 25)) {
        Serial.println("\n[STM32 UART] Single-byte trigger (0xAA) received from STM32 Master!");
        s_stm32_rx_len = 0;
        runBurstInference(s_has_live_stm32 ? s_last_live_weight_g : -1.0f);
        return;
    }

    // Clear buffer if stalled with incomplete data (>200ms)
    if (s_stm32_rx_len > 0 && (millis() - s_stm32_last_rx_ms > 200)) {
        s_stm32_rx_len = 0;
    }
}

// ----------------------------------------------------------------------------
// Main Loop
// ----------------------------------------------------------------------------
void loop() {
    handleSerialCommands();

    // Process inter-chip UART stream from STM32 (Binary, ASCII, or single 0xAA byte)
    processStm32Uart();

    // In continuous mode, run a burst periodically (every 2.5 seconds)
    if (g_continuous_mode) {
        static uint32_t s_last_burst_time = 0;
        if (millis() - s_last_burst_time >= 2500) {
            s_last_burst_time = millis();
            runBurstInference();
        }
    }

    delay(20); // Generous yield to FreeRTOS HTTP server and Wi-Fi stack
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
