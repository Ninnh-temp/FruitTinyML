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
#include "esp_heap_caps.h"

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
#define UNIT_PRICE_APPLE    60000.0f  // Default 60,000 VND / kg
#define UNIT_PRICE_ORANGE   40000.0f  // Default 40,000 VND / kg
#define UNIT_PRICE_PEAR     70000.0f  // Default 70,000 VND / kg

static float g_price_apple  = UNIT_PRICE_APPLE;
static float g_price_orange = UNIT_PRICE_ORANGE;
static float g_price_pear   = UNIT_PRICE_PEAR;

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
// --- Burst Voting Configuration (Temporal Consensus Ensemble) ---
#define BURST_SAMPLE_COUNT     3    // Max 3 frames with 2-frame early exit
#define BURST_FRAME_DELAY_MS  10    // Minimal 10ms yield between frames

// --- Camera & Inference Resolution ---
#define CAMERA_RAW_WIDTH      320
#define CAMERA_RAW_HEIGHT     240
#define MAX_JPEG_BUF_SIZE     (48 * 1024) // 48 KB in PSRAM (supports high-contrast Q12 QVGA frames)

static LiveScaleTelemetry g_telemetry = {"none", 0.0f, 0, 0, 0, 0, 0.0f, 0.0f, 0.0f, false, false, 0, 0, 0, 0.0f, 0, BURST_SAMPLE_COUNT, false};

// Buffers & Sync
static uint8_t  *snapshot_buf = nullptr;     // 320x240x3 RGB888 buffer in PSRAM
static uint8_t  *ai_input_buf = nullptr;     // 96x96x3 RGB888 buffer in Fast Internal SRAM (27.6 KB)
static uint32_t *resize_lut   = nullptr;     // Pre-computed 96x96 byte offset Lookup Table in RAM (36.8 KB)
static uint8_t  *jpeg_stage_buf = nullptr;   // 48 KB dedicated AI burst staging buffer in PSRAM
static uint8_t  *web_jpeg_buf   = nullptr;   // 48 KB dedicated live web stream cache in PSRAM
static SemaphoreHandle_t s_web_buf_mutex = NULL; // Mutex guarding web_jpeg_buf & g_cached_jpeg_len
static volatile size_t g_cached_jpeg_len = 0;
static volatile bool   g_burst_active = false;
static volatile bool   g_web_trigger_pending = false;
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
void initResizeLut();
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
  <title>Cân Thông Minh AI - Smart Grocer POS Cockpit</title>
  <style>
    /* ==========================================================================
       Catppuccin Theme Engine (Mocha 🌙 vs. Latte Daylight ☀️)
       ========================================================================== */
    :root {
      /* Catppuccin Mocha (Night Default) */
      --bg: #11111b;          /* Crust */
      --mantle: #181825;
      --panel: #1e1e2e;       /* Base */
      --panel-tint: #25273a;
      --panel-surface: #313244; /* Surface0 */
      --border-color: rgba(137, 180, 250, 0.28);
      --text: #cdd6f4;        /* Text */
      --text-muted: #a6adc8;  /* Subtext0 */
      --text-dim: #6c7086;    /* Overlay0 */
      --key-shadow: #09090f;
      --dot-color: rgba(108, 112, 134, 0.22);

      /* Chameleon Fruit Accents (Dynamic) */
      --accent: #89b4fa;      /* Blue (Standby) */
      --accent-glow: rgba(137, 180, 250, 0.22);
      --green: #a6e3a1;
      --green-glow: rgba(166, 227, 161, 0.22);
      --amber: #f9e2af;
      --red: #f38ba8;
      --mauve: #cba6f7;
      --peach: #fab387;

      --font-sans: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
      --font-mono: "JetBrains Mono", "Fira Code", Consolas, "Cascadia Mono", "Segoe UI Mono", monospace;
      --ease-spring: cubic-bezier(0.34, 1.56, 0.64, 1);
      --ease-smooth: cubic-bezier(0.22, 1, 0.36, 1);
    }

    [data-theme="day"] {
      /* Catppuccin Latte (Daylight Warm) */
      --bg: #dce0e8;          /* Crust / Mantle */
      --mantle: #e6e9ef;
      --panel: #eff1f5;       /* Base */
      --panel-tint: #e6e9ef;
      --panel-surface: #ccd0da; /* Surface0 */
      --border-color: rgba(30, 102, 245, 0.25);
      --text: #4c4f69;        /* Text */
      --text-muted: #5c5f77;  /* Subtext0 */
      --text-dim: #8c8fa1;    /* Overlay0 */
      --key-shadow: #bcc0cc;
      --dot-color: rgba(140, 143, 161, 0.3);

      --accent: #1e66f5;      /* Sapphire / Blue */
      --accent-glow: rgba(30, 102, 245, 0.18);
      --green: #40a02b;
      --green-glow: rgba(64, 160, 43, 0.2);
      --amber: #df8e1d;
      --red: #d20f39;
      --mauve: #8839ef;
      --peach: #fe640b;
    }

    * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }

    body {
      font-family: var(--font-mono);
      background-color: var(--bg);
      background-image: radial-gradient(var(--dot-color) 1.5px, transparent 1.5px);
      background-size: 18px 18px;
      color: var(--text);
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 14px;
      transition: background-color 0.4s ease, color 0.4s ease;
    }

    .cockpit {
      width: 100%;
      max-width: 1080px;
      display: flex;
      flex-direction: column;
      gap: 14px;
    }

    /* ==========================================================================
       Header Bar
       ========================================================================== */
    .header-bar {
      width: 100%;
      background: var(--panel);
      border: 2px solid var(--border-color);
      border-radius: 14px;
      padding: 8px 14px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 10px;
      box-shadow: 0 6px 20px rgba(0, 0, 0, 0.25);
      backdrop-filter: blur(12px);
      transition: all 0.3s ease;
    }
    .brand-title {
      font-size: 1.1rem;
      font-weight: 800;
      letter-spacing: -0.3px;
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .brand-badge {
      font-size: 0.65rem;
      background: rgba(137, 180, 250, 0.15);
      color: var(--accent);
      padding: 2px 8px;
      border-radius: 10px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
      border: 1px solid var(--border-color);
      transition: all 0.3s ease;
    }
    .header-right {
      display: flex;
      align-items: center;
      gap: 8px;
      flex-wrap: wrap;
    }

    /* Tactile 3D Keycap Buttons */
    .pill-btn {
      position: relative;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 6px 12px;
      border-radius: 9px;
      font-size: 0.75rem;
      font-family: var(--font-mono);
      font-weight: 700;
      background: var(--panel-tint);
      border: 1.5px solid var(--border-color);
      border-bottom: 3.5px solid var(--key-shadow);
      color: var(--text);
      cursor: pointer;
      user-select: none;
      transform: translateY(0);
      box-shadow: 0 3px 6px rgba(0,0,0,0.25);
      transition: transform 0.1s var(--ease-spring), border-bottom-width 0.1s ease, background 0.2s ease, border-color 0.2s ease;
    }
    .pill-btn:hover {
      transform: translateY(-2px);
      border-bottom-width: 5px;
      border-color: var(--accent);
      color: var(--accent);
    }
    .pill-btn:active {
      transform: translateY(2px);
      border-bottom-width: 1.5px;
      box-shadow: 0 1px 2px rgba(0,0,0,0.4);
    }
    .pill-btn.active {
      background: rgba(137, 180, 250, 0.15);
      border-color: var(--accent);
      color: var(--accent);
    }

    .status-pill {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      padding: 6px 12px;
      border-radius: 9px;
      font-size: 0.75rem;
      font-weight: 700;
      background: var(--mantle);
      border: 1.5px solid var(--border-color);
      color: var(--text-muted);
    }
    .conn-dot {
      width: 7px;
      height: 7px;
      border-radius: 50%;
      background: var(--green);
      box-shadow: 0 0 8px var(--green);
      transition: all 0.3s ease;
    }
    .demo-toggle-label {
      cursor: pointer;
      display: inline-flex;
      align-items: center;
      gap: 6px;
      font-size: 0.75rem;
      font-weight: 700;
      color: var(--text-dim);
      user-select: none;
    }
    .demo-toggle-label input { cursor: pointer; accent-color: var(--accent); }

    /* ==========================================================================
       Tiled Cockpit Window Grid
       ========================================================================== */
    .cockpit-grid {
      display: grid;
      grid-template-columns: 1.18fr 1fr;
      gap: 16px;
      align-items: stretch;
    }
    @media (max-width: 840px) {
      .cockpit-grid { grid-template-columns: 1fr; gap: 14px; }
    }

    /* Common Cockpit Window Styling */
    .tiled-window {
      background: var(--panel);
      border: 2px solid var(--border-color);
      border-radius: 14px;
      overflow: hidden;
      display: flex;
      flex-direction: column;
      box-shadow: 0 8px 24px rgba(0, 0, 0, 0.28);
      transition: transform 0.35s var(--ease-spring), border-color 0.35s ease, box-shadow 0.35s ease;
    }
    .tiled-window:hover {
      transform: translateY(-2px);
    }
    .tiled-window.accent-border {
      border-color: var(--accent);
      box-shadow: 0 0 20px var(--accent-glow), 0 8px 24px rgba(0, 0, 0, 0.3);
    }

    /* Monospace Terminal Window Title Bar */
    .win-titlebar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 7px 12px;
      background: var(--mantle);
      border-bottom: 1.5px solid var(--border-color);
      font-size: 0.72rem;
      font-weight: 700;
      color: var(--text-muted);
      user-select: none;
      transition: border-color 0.3s ease;
    }
    .win-controls {
      display: flex;
      align-items: center;
      gap: 6px;
    }
    .win-dot {
      width: 9px;
      height: 9px;
      border-radius: 50%;
      display: inline-block;
    }
    .dot-close { background: var(--red); }
    .dot-min { background: var(--amber); }
    .dot-max { background: var(--green); }
    .win-title-text {
      letter-spacing: 0.4px;
      color: var(--text);
    }
    .win-tag {
      font-size: 0.65rem;
      background: var(--panel-tint);
      border: 1px solid var(--border-color);
      padding: 1px 6px;
      border-radius: 4px;
      color: var(--text-dim);
    }

    /* Left Column: Viewfinder & Controls */
    .monitor-col {
      display: flex;
      flex-direction: column;
      gap: 12px;
    }

    .viewfinder {
      position: relative;
      width: 100%;
      aspect-ratio: 4 / 3;
      background: #020408;
      overflow: hidden;
      display: flex;
      align-items: center;
      justify-content: center;
      cursor: pointer;
    }
    #cam-feed {
      width: 100%;
      height: 100%;
      object-fit: contain;
      display: block;
    }

    /* CRT Scanline Texture Overlay */
    .crt-scanlines {
      position: absolute;
      inset: 0;
      background: linear-gradient(rgba(18, 16, 16, 0) 50%, rgba(0, 0, 0, 0.35) 50%);
      background-size: 100% 4px;
      z-index: 3;
      pointer-events: none;
      opacity: 0.55;
    }

    /* Camera Viewfinder Glowing Corner Brackets */
    .bracket {
      position: absolute;
      width: 28px;
      height: 28px;
      border: 3.5px solid var(--accent);
      filter: drop-shadow(0 0 10px var(--accent-glow));
      pointer-events: none;
      z-index: 4;
      transition: all 0.35s var(--ease-spring);
    }
    .br-tl { top: 12px; left: 12px; border-right: none; border-bottom: none; border-top-left-radius: 6px; }
    .br-tr { top: 12px; right: 12px; border-left: none; border-bottom: none; border-top-right-radius: 6px; }
    .br-bl { bottom: 12px; left: 12px; border-right: none; border-top: none; border-bottom-left-radius: 6px; }
    .br-br { bottom: 12px; right: 12px; border-left: none; border-top: none; border-bottom-right-radius: 6px; }

    .viewfinder.scanning .bracket {
      border-color: #ffffff;
      filter: drop-shadow(0 0 12px var(--accent));
      animation: bracketPulse 0.8s ease-in-out;
    }
    @keyframes bracketPulse {
      0%, 100% { transform: scale(1); }
      50% { transform: scale(1.18); }
    }

    /* AI Horizontal Scanning Laser Bar */
    .ai-scan-laser {
      position: absolute;
      left: 0;
      right: 0;
      height: 3px;
      background: linear-gradient(90deg, transparent 0%, var(--accent) 25%, #ffffff 50%, var(--accent) 75%, transparent 100%);
      box-shadow: 0 0 16px var(--accent), 0 0 6px #ffffff;
      opacity: 0;
      pointer-events: none;
      top: 0;
      z-index: 5;
    }
    .ai-scan-laser.scanning {
      animation: laserSweep 0.8s cubic-bezier(0.4, 0, 0.2, 1) forwards;
    }
    @keyframes laserSweep {
      0% { top: 0%; opacity: 0; }
      15% { opacity: 1; }
      85% { opacity: 1; }
      100% { top: 100%; opacity: 0; }
    }

    /* Shutter Flash */
    .shutter-flash {
      position: absolute;
      inset: 0;
      background: white;
      opacity: 0;
      pointer-events: none;
      z-index: 6;
      transition: opacity 0.25s ease-out;
    }
    .shutter-flash.active { opacity: 0.85; transition: opacity 0.03s ease-in; }

    /* Viewfinder HUD Overlays - Inset to ensure corner brackets are completely visible */
    .hud-top {
      position: absolute;
      top: 16px;
      left: 52px;
      right: 52px;
      display: flex;
      justify-content: space-between;
      pointer-events: none;
      z-index: 5;
    }
    .hud-tag {
      background: rgba(17, 17, 27, 0.85);
      backdrop-filter: blur(8px);
      padding: 4px 10px;
      border-radius: 6px;
      font-size: 0.72rem;
      font-variant-numeric: tabular-nums;
      font-weight: 700;
      color: var(--text-muted);
      border: 1px solid rgba(255, 255, 255, 0.08);
      box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
    }
    .hud-bottom {
      position: absolute;
      bottom: 16px;
      left: 52px;
      right: 52px;
      display: flex;
      justify-content: space-between;
      align-items: center;
      pointer-events: none;
      z-index: 5;
    }
    .live-target-badge {
      display: flex;
      align-items: center;
      gap: 6px;
      background: rgba(17, 17, 27, 0.9);
      backdrop-filter: blur(10px);
      padding: 6px 12px;
      border-radius: 8px;
      font-size: 0.85rem;
      font-weight: 700;
      color: var(--text);
      border: 1.5px solid var(--border-color);
      box-shadow: 0 4px 12px rgba(0,0,0,0.4);
    }

    /* Action Mechanical Keys */
    .action-strip {
      display: grid;
      grid-template-columns: 1.5fr 1fr;
      gap: 10px;
    }
    .act-btn {
      position: relative;
      padding: 12px 14px;
      border-radius: 12px;
      border: 2px solid var(--border-color);
      border-bottom: 4px solid var(--key-shadow);
      background: var(--panel);
      color: var(--text);
      font-family: var(--font-mono);
      font-size: 0.86rem;
      font-weight: 700;
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      cursor: pointer;
      user-select: none;
      transform: translateY(0);
      box-shadow: 0 4px 10px rgba(0, 0, 0, 0.25);
      transition: transform 0.1s var(--ease-spring), border-bottom-width 0.1s ease, background 0.15s ease, border-color 0.2s ease, box-shadow 0.2s ease;
    }
    .act-btn:hover {
      transform: translateY(-2px);
      border-bottom-width: 6px;
      border-color: var(--accent);
      color: var(--accent);
      box-shadow: 0 8px 18px var(--accent-glow);
    }
    .act-btn:active {
      transform: translateY(2px);
      border-bottom-width: 2px;
      box-shadow: 0 2px 4px rgba(0, 0, 0, 0.4);
    }
    .act-btn.primary {
      background: var(--accent);
      border-color: var(--accent);
      border-bottom: 4px solid var(--key-shadow);
      color: #11111b;
      box-shadow: 0 4px 14px var(--accent-glow);
    }
    .act-btn.primary:hover {
      filter: brightness(1.1);
      box-shadow: 0 8px 22px var(--accent-glow);
      color: #11111b;
    }
    .act-btn.flash-active {
      background: var(--amber);
      border-color: var(--amber);
      color: #11111b;
      box-shadow: 0 4px 14px rgba(249, 226, 175, 0.35);
    }

    /* Right Column: Metrology, Price Tray & Hardware Telemetry */
    .console-col {
      display: flex;
      flex-direction: column;
      gap: 14px;
      justify-content: space-between;
    }

    /* Metrology POS Terminal Window */
    .metrology-block {
      padding: 14px 16px;
      display: flex;
      flex-direction: column;
      gap: 10px;
    }
    .metrology-row-top {
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .metrology-title {
      font-size: 0.72rem;
      text-transform: uppercase;
      letter-spacing: 0.8px;
      font-weight: 700;
      color: var(--text-dim);
    }
    .source-pill {
      font-size: 0.65rem;
      background: var(--mantle);
      border: 1px solid var(--border-color);
      color: var(--text-dim);
      padding: 2px 8px;
      border-radius: 6px;
      font-weight: 700;
    }
    .mass-hero-wrap {
      display: flex;
      align-items: baseline;
      gap: 8px;
    }
    .mass-val {
      font-variant-numeric: tabular-nums;
      font-size: 3.4rem;
      font-weight: 800;
      line-height: 1;
      letter-spacing: -1px;
      color: var(--text);
    }
    .mass-unit {
      font-size: 1.1rem;
      font-weight: 700;
      color: var(--text-muted);
    }
    .mass-kg-sub {
      font-variant-numeric: tabular-nums;
      font-size: 1rem;
      font-weight: 700;
      color: var(--text-dim);
      text-align: right;
    }

    /* Dashed Receipt Line */
    .receipt-divider {
      height: 1px;
      border: none;
      border-top: 1.5px dashed var(--border-color);
      margin: 4px 0;
    }

    .price-line {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      padding-top: 2px;
    }
    .price-total-wrap {
      display: flex;
      flex-direction: column;
    }
    .price-total-val {
      font-variant-numeric: tabular-nums;
      font-size: 2.3rem;
      font-weight: 800;
      letter-spacing: -0.5px;
      color: var(--accent);
      line-height: 1;
      transition: color 0.35s ease;
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
      font-weight: 700;
    }

    /* Spring Pop Animation */
    @keyframes springPop {
      0% { transform: scale(0.6) rotate(-8deg); opacity: 0.5; }
      50% { transform: scale(1.3) rotate(5deg); opacity: 1; }
      75% { transform: scale(0.9) rotate(-2deg); }
      100% { transform: scale(1) rotate(0deg); opacity: 1; }
    }
    .spring-pop {
      display: inline-block;
      animation: springPop 0.5s var(--ease-spring) forwards;
    }

    /* Price Editor Tray (Non-blocking modal replacement) */
    .price-editor-tray {
      display: none;
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
      color: var(--text-muted);
    }
    .unit-switch-group {
      display: flex;
      gap: 4px;
      background: var(--mantle);
      padding: 2px;
      border-radius: 8px;
      border: 1px solid var(--border-color);
    }
    .unit-opt-btn {
      padding: 3px 8px;
      font-size: 0.7rem;
      font-family: var(--font-mono);
      font-weight: 700;
      border: none;
      background: none;
      color: var(--text-dim);
      border-radius: 6px;
      cursor: pointer;
      transition: all 0.15s ease;
    }
    .unit-opt-btn.selected {
      background: var(--accent);
      color: var(--bg);
      box-shadow: 0 1px 4px rgba(0,0,0,0.3);
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
      background: var(--mantle);
      border: 1px solid var(--border-color);
      border-radius: 9px;
      padding: 8px 10px;
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    .price-card-label {
      font-size: 0.75rem;
      font-weight: 700;
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
      width: 28px;
      height: 28px;
      border-radius: 7px;
      border: 1.5px solid var(--border-color);
      border-bottom: 3px solid var(--key-shadow);
      background: var(--panel-tint);
      color: var(--text);
      font-family: var(--font-mono);
      font-size: 1rem;
      font-weight: 700;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      user-select: none;
      transform: translateY(0);
      transition: transform 0.1s var(--ease-spring), border-bottom-width 0.1s ease;
    }
    .step-btn:hover {
      transform: translateY(-1px);
      border-bottom-width: 4px;
      border-color: var(--accent);
    }
    .step-btn:active {
      transform: translateY(1.5px);
      border-bottom-width: 1.5px;
    }
    .price-input-box {
      flex: 1;
      width: 100%;
      min-width: 50px;
      background: var(--bg);
      border: 1.5px solid var(--border-color);
      border-radius: 7px;
      color: var(--text);
      font-family: var(--font-mono);
      font-variant-numeric: tabular-nums;
      font-size: 0.88rem;
      font-weight: 700;
      padding: 4px 6px;
      text-align: center;
      outline: none;
    }
    .price-input-box:focus {
      border-color: var(--accent);
      box-shadow: 0 0 8px var(--accent-glow);
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

    /* Fruit Variety Strip */
    .fruit-line {
      padding: 12px 14px;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
    }
    .fruit-info-left {
      display: flex;
      align-items: center;
      gap: 12px;
    }
    .fruit-icon {
      font-size: 1.6rem;
      line-height: 1;
      user-select: none;
    }
    .fruit-name {
      font-size: 0.98rem;
      font-weight: 800;
      color: var(--text);
    }
    .fruit-consensus-txt {
      font-size: 0.74rem;
      color: var(--text-muted);
      margin-top: 2px;
    }
    .consensus-bar-wrap {
      width: 100%;
      height: 4px;
      background: var(--mantle);
      border-radius: 2px;
      overflow: hidden;
      margin-top: 4px;
    }
    .consensus-bar-fill {
      height: 100%;
      background: var(--accent);
      width: 0%;
      transition: width 0.3s ease, background-color 0.35s ease;
    }


    /* ==========================================================================
       Hardware & AI Telemetry Monitor
       ========================================================================== */
    .telemetry-block {
      display: flex;
      flex-direction: column;
    }
    .telem-inner {
      padding: 12px 14px;
      display: flex;
      flex-direction: column;
      gap: 10px;
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
      background: var(--mantle);
      border: 1px solid var(--border-color);
      border-radius: 8px;
      padding: 8px 10px;
      display: flex;
      flex-direction: column;
      gap: 5px;
      transition: background 0.3s ease, border-color 0.3s ease;
    }
    .telem-cell.flash-update {
      background: rgba(137, 180, 250, 0.2);
      border-color: var(--accent);
    }
    .telem-meta-row {
      display: flex;
      justify-content: space-between;
      align-items: baseline;
    }
    .telem-k {
      font-size: 0.65rem;
      color: var(--text-dim);
      font-weight: 700;
      text-transform: uppercase;
    }
    .telem-v {
      font-variant-numeric: tabular-nums;
      font-size: 0.85rem;
      font-weight: 800;
      color: var(--text);
    }
    .meter-bar {
      width: 100%;
      height: 3px;
      background: var(--panel-tint);
      border-radius: 2px;
      overflow: hidden;
    }
    .meter-fill {
      height: 100%;
      background: var(--accent);
      width: 50%;
      transition: width 0.35s ease, background 0.35s ease;
    }

    .telem-footer {
      font-size: 0.68rem;
      color: var(--text-dim);
      text-align: center;
      padding-top: 4px;
      border-top: 1px solid var(--border-color);
      display: flex;
      justify-content: space-between;
      align-items: center;
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
    <!-- Header Bar -->
    <header class="header-bar">
      <div class="brand-title">
        <span>Cân Thông Minh AI</span>
        <span class="brand-badge">ESP32 + STM32</span>
      </div>
      <div class="header-right">
        <button class="pill-btn" id="btn-price-menu" onclick="togglePriceMenu()">
          <span>🏷️</span>
          <span>Bảng Giá (VND)</span>
        </button>
        <button class="pill-btn" id="btn-theme-toggle" onclick="toggleRiceTheme()">
          <span id="lbl-theme-icon">🌙</span>
          <span id="lbl-theme-text">Tối</span>
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

    <!-- Cockpit Main Area: Window Grid -->
    <div class="cockpit-grid">
      <!-- Left Column: Viewfinder & Primary Triggers -->
      <div class="monitor-col">
        <!-- Viewfinder Tiled Window -->
        <div class="tiled-window accent-border">
          <div class="win-titlebar">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
            </div>
            <div class="win-title-text">Camera Giám Sát (OV2640 • 320x240)</div>
            <span class="win-tag">CAMERA LIVE</span>
          </div>

          <div class="viewfinder" onclick="triggerInference()">
            <img id="cam-feed" src="/capture" alt="Camera Stream" onerror="handleImageError()">
            <div class="shutter-flash" id="shutter-fx"></div>
            <div class="crt-scanlines"></div>
            <div class="ai-scan-laser" id="ai-scan-laser"></div>

            <!-- Viewfinder Brackets -->
            <div class="bracket br-tl"></div>
            <div class="bracket br-tr"></div>
            <div class="bracket br-bl"></div>
            <div class="bracket br-br"></div>

            <!-- Top HUD -->
            <div class="hud-top">
              <div class="hud-tag">FOMO INT8 • 96x96</div>
              <div class="hud-tag" id="cam-fps-hud">-- FPS</div>
            </div>

            <!-- Bottom HUD Status -->
            <div class="hud-bottom">
              <div class="live-target-badge">
                <span id="target-icon">⚖️</span>
                <span id="target-name">Đang quét đĩa cân...</span>
              </div>
              <div class="hud-tag" id="target-conf">-- %</div>
            </div>
          </div>
        </div>

        <!-- Controls (2-Column Action Strip) -->
        <div class="action-strip">
          <button class="act-btn primary" onclick="triggerInference()">
            <span>⚡</span>
            <span>Quét Nhận Diện (AI)</span>
          </button>
          <button class="act-btn" id="btn-flash" onclick="toggleFlash()">
            <span>💡</span>
            <span id="lbl-flash">Đèn Flash</span>
          </button>
        </div>
      </div>

      <!-- Right Column: Metrology POS, Price Tray & Telemetry -->
      <div class="console-col">
        <!-- Metrology Tiled Window -->
        <div class="tiled-window accent-border">
          <div class="win-titlebar">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
            </div>
            <div class="win-title-text">Bảng Tính Tiền & Cân Khối Lượng (HX711)</div>
            <span class="win-tag">POS THÀNH TIỀN</span>
          </div>

          <div class="metrology-block">
            <div class="metrology-row-top">
              <div style="display:flex; align-items:center; gap:8px;">
                <span class="metrology-title">Khối lượng cân</span>
                <span class="source-pill" id="val-source-pill">STM32 HX711</span>
              </div>
              <span class="mass-kg-sub" id="val-mass-kg">0,000 kg</span>
            </div>
            <div class="mass-hero-wrap">
              <span class="mass-val" id="val-mass">0.0</span>
              <span class="mass-unit">g</span>
            </div>
            
            <div class="receipt-divider"></div>

            <div class="price-line">
              <div class="price-total-wrap">
                <span class="metrology-title">Thành tiền</span>
                <span class="price-total-val" id="val-total">0 ₫</span>
              </div>
              <div class="price-rate-wrap">
                <div id="val-unit-rate">
                  Đơn giá: 0 ₫ / kg
                  <span class="edit-price-inline" onclick="togglePriceMenu()">[Sửa]</span>
                </div>
                <div style="font-size:0.75rem; color:var(--text-dim);" id="val-tare-state">Đã trừ bì (Tare 0g) • Sensor HX711 sẵn sàng</div>
              </div>
            </div>
          </div>
        </div>

        <!-- Non-Blocking Price Change Tray -->
        <div class="tiled-window price-editor-tray" id="price-tray">
          <div class="win-titlebar" style="margin:-12px -14px 10px -14px;">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
            </div>
            <div class="win-title-text">Cài Đặt Đơn Giá Nhanh (VND)</div>
            <span class="win-tag">CÀI ĐẶT GIÁ</span>
          </div>

          <div class="price-tray-header">
            <span>Cài Đặt Đơn Giá Nhanh (VND)</span>
            <div class="unit-switch-group">
              <button class="unit-opt-btn selected" id="unit-kg-btn" onclick="setPriceUnit('kg')">/ kg</button>
              <button class="unit-opt-btn" id="unit-100g-btn" onclick="setPriceUnit('100g')">/ 100g</button>
              <button class="unit-opt-btn" id="unit-g-btn" onclick="setPriceUnit('g')">/ g</button>
            </div>
          </div>

          <div class="price-inputs-grid">
            <!-- Apple (Táo) -->
            <div class="price-card">
              <span class="price-card-label">🍎 Táo (Apple)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('apple', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-apple" oninput="onCustomPriceChange('apple', this.value)">
                <button class="step-btn" onclick="stepPrice('apple', 1000)">+</button>
              </div>
            </div>

            <!-- Orange (Cam) -->
            <div class="price-card">
              <span class="price-card-label">🍊 Cam (Orange)</span>
              <div class="price-stepper">
                <button class="step-btn" onclick="stepPrice('orange', -1000)">−</button>
                <input type="number" step="100" min="0" class="price-input-box" id="inp-price-orange" oninput="onCustomPriceChange('orange', this.value)">
                <button class="step-btn" onclick="stepPrice('orange', 1000)">+</button>
              </div>
            </div>

            <!-- Pear (Lê) -->
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

        <!-- Detected Fruit Variety Strip -->
        <div class="tiled-window fruit-line">
          <div class="fruit-info-left">
            <span class="fruit-icon" id="fruit-emoji">⚖️</span>
            <div>
              <div class="fruit-name" id="fruit-title">Cân trống</div>
              <div class="fruit-consensus-txt" id="fruit-sub">Đĩa cân trống • Sẵn sàng đặt quả</div>
              <div class="consensus-bar-wrap">
                <div class="consensus-bar-fill" id="consensus-bar"></div>
              </div>
            </div>
          </div>
        </div>

        <!-- Telemetry Monitor Window -->
        <div class="tiled-window telemetry-block">
          <div class="win-titlebar">
            <div class="win-controls">
              <span class="win-dot dot-close"></span>
              <span class="win-dot dot-min"></span>
              <span class="win-dot dot-max"></span>
            </div>
            <div class="win-title-text">Thông Số Phần Cứng & TinyML AI</div>
            <span class="win-tag" id="telem-live-pill">TRỰC TIẾP ●</span>
          </div>

          <div class="telem-inner">
            <div class="telem-grid">
              <div class="telem-cell" id="c-infer">
                <div class="telem-meta-row">
                  <span class="telem-k">AI_LATENCY</span>
                  <span class="telem-v" id="t-infer">-- ms</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-infer" style="width: 42%;"></div></div>
              </div>
              <div class="telem-cell" id="c-dsp">
                <div class="telem-meta-row">
                  <span class="telem-k">DSP_IMAGE</span>
                  <span class="telem-v" id="t-dsp">-- ms</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-dsp" style="width: 16%;"></div></div>
              </div>
              <div class="telem-cell" id="c-fps">
                <div class="telem-meta-row">
                  <span class="telem-k">FRAME_RATE</span>
                  <span class="telem-v" id="t-fps">-- FPS</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-fps" style="width: 66%;"></div></div>
              </div>
              <div class="telem-cell" id="c-consensus">
                <div class="telem-meta-row">
                  <span class="telem-k">VOTES_BURST</span>
                  <span class="telem-v" id="t-votes">--</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-votes" style="width: 100%;"></div></div>
              </div>
              <div class="telem-cell" id="c-temp">
                <div class="telem-meta-row">
                  <span class="telem-k">DIE_TEMP</span>
                  <span class="telem-v" id="t-temp">-- °C</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-temp" style="width: 60%;"></div></div>
              </div>
              <div class="telem-cell" id="c-psram">
                <div class="telem-meta-row">
                  <span class="telem-k">PSRAM_FREE</span>
                  <span class="telem-v" id="t-psram">-- KB</span>
                </div>
                <div class="meter-bar"><div class="meter-fill" id="bar-psram" style="width: 92%;"></div></div>
              </div>
            </div>

            <div class="telem-footer">
              <span>ESP32-CAM (FOMO INT8) + STM32 (HX711)</span>
              <span>Bộ nhớ: 4MB PSRAM • SRAM</span>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- Footer -->
    <footer>
      Hệ Thống Cân Tính Tiền Thông Minh Edge AI • ESP32-CAM (FOMO INT8) & STM32 (HX711)
    </footer>
  </div>

  <script>
    let isLiveConnected = false;
    let isDemoMode = false;
    let flashActive = false;
    let demoIndex = 0;
    let priceUnit = 'kg'; // 'kg', '100g', 'g'
    let prevFruitLabel = '';
    let currentTheme = 'night';

    // Day / Night Catppuccin Palettes
    const PALETTES = {
      night: {
        apple:  { accent: '#f38ba8', glow: 'rgba(243, 139, 168, 0.28)' },
        orange: { accent: '#fab387', glow: 'rgba(250, 179, 135, 0.28)' },
        pear:   { accent: '#a6e3a1', glow: 'rgba(166, 227, 161, 0.28)' },
        none:   { accent: '#89b4fa', glow: 'rgba(137, 180, 250, 0.22)' }
      },
      day: {
        apple:  { accent: '#d20f39', glow: 'rgba(210, 15, 57, 0.25)' },
        orange: { accent: '#fe640b', glow: 'rgba(254, 100, 11, 0.25)' },
        pear:   { accent: '#40a02b', glow: 'rgba(64, 160, 43, 0.25)' },
        none:   { accent: '#1e66f5', glow: 'rgba(30, 102, 245, 0.2)' }
      }
    };

    // Load Theme Preference
    try {
      const savedTheme = localStorage.getItem('scale_rice_theme');
      if (savedTheme === 'day') {
        currentTheme = 'day';
        document.documentElement.dataset.theme = 'day';
      }
    } catch (e) {}

    function updateThemeButton() {
      const icon = document.getElementById('lbl-theme-icon');
      const text = document.getElementById('lbl-theme-text');
      if (currentTheme === 'day') {
        if (icon) icon.innerText = '☀️';
        if (text) text.innerText = 'Sáng';
      } else {
        if (icon) icon.innerText = '🌙';
        if (text) text.innerText = 'Tối';
      }
    }

    function toggleRiceTheme() {
      currentTheme = (currentTheme === 'night') ? 'day' : 'night';
      if (currentTheme === 'day') {
        document.documentElement.dataset.theme = 'day';
      } else {
        delete document.documentElement.dataset.theme;
      }
      try { localStorage.setItem('scale_rice_theme', currentTheme); } catch(e){}
      updateThemeButton();
      renderData(currentTelemetry);
    }

    // Default prices per kg in VND
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
      { label: "apple", confidence: 0.945, weight: 218.4, votes: 4, burst_total: 4, dsp_ms: 8, infer_ms: 42, temp: 48.2, psram_kb: 3840, heap_kb: 172, stm32_live: true },
      { label: "orange", confidence: 0.892, weight: 184.2, votes: 4, burst_total: 4, dsp_ms: 9, infer_ms: 41, temp: 48.6, psram_kb: 3840, heap_kb: 171, stm32_live: true },
      { label: "pear", confidence: 0.961, weight: 235.8, votes: 4, burst_total: 4, dsp_ms: 8, infer_ms: 44, temp: 49.1, psram_kb: 3840, heap_kb: 170, stm32_live: true },
      { label: "none", confidence: 0.000, weight: 0.0, votes: 0, burst_total: 4, dsp_ms: 8, infer_ms: 39, temp: 47.9, psram_kb: 3840, heap_kb: 174, stm32_live: true }
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
      return pricePerKg; // 'kg'
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
      if (!isDemoMode) {
        fetch(`/set_prices?apple=${customPrices.apple}&orange=${customPrices.orange}&pear=${customPrices.pear}`).catch(() => {});
      }
    }

    function createSvgView(label) {
      const themeSet = PALETTES[currentTheme] || PALETTES.night;
      const c = (themeSet[label] ? themeSet[label].accent : '#89b4fa');
      const icon = FRUITS[label] ? FRUITS[label].emoji : "⚖️";
      const bg = (currentTheme === 'day') ? "%23eff1f5" : "%23020408";
      return `data:image/svg+xml;utf8,<svg xmlns="http://www.w3.org/2000/svg" width="320" height="240" viewBox="0 0 320 240"><rect width="320" height="240" fill="${bg}"/><circle cx="160" cy="120" r="54" fill="${encodeURIComponent(c)}" opacity="0.25"/><circle cx="160" cy="120" r="40" fill="${encodeURIComponent(c)}" opacity="0.75"/><text x="160" y="133" font-size="38" text-anchor="middle" font-family="sans-serif">${encodeURIComponent(icon)}</text></svg>`;
    }

    let lastFrameTime = performance.now();
    let rollingStreamFps = 12.0;
    let currentBlobUrl = null;
    let isStreaming = false;
    let connFailCount = 0;

    async function updateFrame() {
      if (isDemoMode) return;
      if (isStreaming) return;
      isStreaming = true;

      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 2500);

      try {
        const resp = await fetch('/capture?t=' + Date.now(), {
          cache: 'no-store',
          signal: controller.signal
        });
        clearTimeout(timeoutId);

        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        const blob = await resp.blob();
        if (blob.size < 100) throw new Error('Truncated frame');

        const newUrl = URL.createObjectURL(blob);
        const view = document.getElementById('cam-feed');
        view.src = newUrl;
        if (currentBlobUrl) URL.revokeObjectURL(currentBlobUrl);
        currentBlobUrl = newUrl;

        const now = performance.now();
        const dt = now - lastFrameTime;
        lastFrameTime = now;
        if (dt > 10 && dt < 2000) {
          rollingStreamFps = (rollingStreamFps * 0.75) + ((1000.0 / dt) * 0.25);
          document.getElementById('cam-fps-hud').innerText = Math.round(rollingStreamFps) + " FPS";
        }
        connFailCount = 0;
        setConnectionState(true);
        isStreaming = false;
        setTimeout(updateFrame, 60);
      } catch (err) {
        clearTimeout(timeoutId);
        handleImageError();
        isStreaming = false;
        setTimeout(updateFrame, 300);
      }
    }

    function handleImageError() {
      if (!isDemoMode) {
        connFailCount++;
        if (connFailCount >= 6) {
          setConnectionState(false);
        }
      }
    }

    let isPolling = false;
    async function poll() {
      if (isDemoMode) {
        renderData(DEMO_SAMPLES[demoIndex]);
        return;
      }
      if (isPolling) return;
      isPolling = true;

      const controller = new AbortController();
      const timeoutId = setTimeout(() => controller.abort(), 2500);

      try {
        const resp = await fetch('/status', { signal: controller.signal });
        clearTimeout(timeoutId);
        if (!resp.ok) throw new Error('HTTP ' + resp.status);
        const d = await resp.json();
        connFailCount = 0;
        setConnectionState(true);
        renderData(d);
      } catch (e) {
        clearTimeout(timeoutId);
        connFailCount++;
        if (connFailCount >= 6) {
          setConnectionState(false);
        }
      } finally {
        isPolling = false;
        if (!isDemoMode) setTimeout(poll, 350);
      }
    }
    function triggerSpring(el) {
      if (!el) return;
      el.classList.remove('spring-pop');
      void el.offsetWidth;
      el.classList.add('spring-pop');
    }

    function renderData(d) {
      currentTelemetry = d;
      const rawLabel = (d.label || "none").toLowerCase();
      const isFruit = (rawLabel === "apple" || rawLabel === "orange" || rawLabel === "pear");
      const label = isFruit ? rawLabel : "none";
      const meta = FRUITS[label] || FRUITS["none"];
      const massG = d.weight > 0 ? d.weight : 0.0;

      // 1. Dynamic Fruit Accent Theming
      const themeSet = PALETTES[currentTheme] || PALETTES.night;
      const fruitTheme = themeSet[label] || themeSet.none;
      document.documentElement.style.setProperty('--accent', fruitTheme.accent);
      document.documentElement.style.setProperty('--accent-glow', fruitTheme.glow);

      // 2. Spring Physics on Class Change
      if (rawLabel !== prevFruitLabel) {
        prevFruitLabel = rawLabel;
        triggerSpring(document.getElementById('fruit-emoji'));
        triggerSpring(document.getElementById('target-icon'));
        triggerSpring(document.getElementById('val-total'));
      }

      // Base price in VND/kg
      const basePricePerKg = isFruit ? (customPrices[label] || DEFAULT_PRICES[label] || 60000) : 0;
      const displayRate = convertRateForUnit(basePricePerKg, priceUnit);
      const totalDue = (massG / 1000.0) * basePricePerKg;

      // 3. Metrology Readout
      document.getElementById('val-mass').innerText = massG.toFixed(1);
      document.getElementById('val-mass-kg').innerText = (massG / 1000.0).toFixed(3).replace('.', ',') + " kg";
      document.getElementById('val-total').innerText = totalDue > 0 ? formatVND(totalDue) : "0 ₫";
      
      const rateSuffix = priceUnit === 'g' ? '/ g' : (priceUnit === '100g' ? '/ 100g' : '/ kg');
      document.getElementById('val-unit-rate').innerHTML = 
        `Đơn giá: ${formatVND(displayRate)} ${rateSuffix} <span class="edit-price-inline" onclick="togglePriceMenu()">[Sửa]</span>`;
      document.getElementById('val-tare-state').innerText = isFruit 
        ? "Đang cân • Cảm biến HX711 (80Hz)" 
        : "Đã trừ bì (Tare 0g) • Cảm biến HX711 sẵn sàng";

      const srcPill = document.getElementById('val-source-pill');
      if (srcPill) {
        srcPill.innerText = d.stm32_live ? "STM32 HX711 • Live" : "STM32 HX711";
        srcPill.style.color = d.stm32_live ? "var(--green)" : "var(--text-dim)";
      }

      // 4. Viewfinder HUD
      document.getElementById('target-icon').innerText = meta.emoji;
      document.getElementById('target-name').innerText = isFruit ? meta.name : "Đang quét đĩa cân...";
      document.getElementById('target-conf').innerText = isFruit ? (d.confidence * 100).toFixed(0) + "%" : "--";
      const totalTime = (d.dsp_ms || 8) + (d.infer_ms || 42);
      const fps = totalTime > 0 ? (1000 / totalTime).toFixed(0) : "20";
      if (isDemoMode) {
        document.getElementById('cam-fps-hud').innerText = fps + " FPS";
      }

      // 5. Fruit Variety Strip Status
      document.getElementById('fruit-emoji').innerText = meta.emoji;
      document.getElementById('fruit-title').innerText = isFruit ? meta.name : "Cân trống";
      const votes = d.votes !== undefined ? d.votes : (isFruit ? 4 : 0);
      const burstTotal = d.burst_total || 4;
      const pct = Math.round((votes / burstTotal) * 100);
      document.getElementById('fruit-sub').innerText = isFruit 
        ? `Đồng thuận ${votes}/${burstTotal} (${pct}%) • Nhận diện chuẩn xác ✨`
        : "Đĩa cân trống • Sẵn sàng đặt quả";
      document.getElementById('consensus-bar').style.width = isFruit ? pct + "%" : "0%";

      // 6. Hardware Telemetry Grid & Meter Bars
      const inferVal = d.infer_ms || 42;
      const dspVal = d.dsp_ms || 8;
      const tempVal = d.temp ? d.temp.toFixed(1) : "48.2";
      const psramVal = d.psram_kb || 3840;

      document.getElementById('t-infer').innerText = inferVal + " ms";
      document.getElementById('t-dsp').innerText = dspVal + " ms";
      document.getElementById('t-fps').innerText = fps + " FPS";
      document.getElementById('t-votes').innerText = `${votes}/${burstTotal}`;
      document.getElementById('t-temp').innerText = tempVal + " °C";
      document.getElementById('t-psram').innerText = psramVal + " KB";

      // Animated Meter Bar Fills
      const barInfer = document.getElementById('bar-infer');
      if (barInfer) barInfer.style.width = Math.min(100, Math.round((inferVal / 80) * 100)) + "%";

      const barDsp = document.getElementById('bar-dsp');
      if (barDsp) barDsp.style.width = Math.min(100, Math.round((dspVal / 25) * 100)) + "%";

      const barFps = document.getElementById('bar-fps');
      if (barFps) barFps.style.width = Math.min(100, Math.round((parseFloat(fps) / 25) * 100)) + "%";

      const barVotes = document.getElementById('bar-votes');
      if (barVotes) barVotes.style.width = pct + "%";

      const barTemp = document.getElementById('bar-temp');
      if (barTemp) barTemp.style.width = Math.min(100, Math.round((parseFloat(tempVal) / 75) * 100)) + "%";

      const barPsram = document.getElementById('bar-psram');
      if (barPsram) barPsram.style.width = Math.min(100, Math.round((psramVal / 4096) * 100)) + "%";

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
        dot.style.boxShadow = "0 0 8px var(--green)";
        text.innerText = "SoftAP Live";
        isDemoMode = false;
        document.getElementById('demo-check').checked = false;
      } else {
        dot.style.background = "var(--amber)";
        dot.style.boxShadow = "0 0 8px var(--amber)";
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
      // 1. Shutter Flash
      const fx = document.getElementById('shutter-fx');
      fx.classList.add('active');
      setTimeout(() => fx.classList.remove('active'), 120);

      // 2. AI Horizontal Laser Sweep & Bracket Pulse
      const vf = document.querySelector('.viewfinder');
      const laser = document.getElementById('ai-scan-laser');
      if (laser) {
        laser.classList.remove('scanning');
        void laser.offsetWidth;
        laser.classList.add('scanning');
        setTimeout(() => laser.classList.remove('scanning'), 850);
      }
      if (vf) {
        vf.classList.add('scanning');
        setTimeout(() => vf.classList.remove('scanning'), 850);
      }

      // 3. Telemetry Cell Flash Feedback
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

    // Init
    updateThemeButton();
    syncPriceInputs();
    savePrices();

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
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t capture_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=\"capture.jpg\"");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");

    // 1. If AI burst is active, serve directly from dedicated web cache (never touch camera mutex)
    if (g_burst_active) {
        if (web_jpeg_buf && s_web_buf_mutex && xSemaphoreTake(s_web_buf_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            size_t len = g_cached_jpeg_len;
            esp_err_t res = ESP_FAIL;
            if (len > 0) {
                res = httpd_resp_send(req, (const char *)web_jpeg_buf, len);
            }
            xSemaphoreGive(s_web_buf_mutex);
            if (res == ESP_OK) return ESP_OK;
        }
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // 2. Grab fresh camera frame with minimal mutex hold time (< 1ms copy, never hold across TCP)
    if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(60)) == pdTRUE) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (web_jpeg_buf && s_web_buf_mutex && xSemaphoreTake(s_web_buf_mutex, pdMS_TO_TICKS(150)) == pdTRUE) {
                if (fb->len <= MAX_JPEG_BUF_SIZE) {
                    memcpy(web_jpeg_buf, fb->buf, fb->len);
                    g_cached_jpeg_len = fb->len;
                }
                xSemaphoreGive(s_web_buf_mutex);
            }
            esp_camera_fb_return(fb);
            xSemaphoreGive(s_camera_mutex);

            // Send over network OUTSIDE camera mutex from web buffer
            if (web_jpeg_buf && s_web_buf_mutex && xSemaphoreTake(s_web_buf_mutex, pdMS_TO_TICKS(150)) == pdTRUE) {
                esp_err_t res = httpd_resp_send(req, (const char *)web_jpeg_buf, g_cached_jpeg_len);
                xSemaphoreGive(s_web_buf_mutex);
                return res;
            }
        } else {
            xSemaphoreGive(s_camera_mutex);
        }
    }

    // 3. Fallback to web stream cache if camera was busy (prevents HTTP 500)
    if (web_jpeg_buf && s_web_buf_mutex && xSemaphoreTake(s_web_buf_mutex, pdMS_TO_TICKS(150)) == pdTRUE) {
        size_t len = g_cached_jpeg_len;
        esp_err_t res = ESP_FAIL;
        if (len > 0) {
            res = httpd_resp_send(req, (const char *)web_jpeg_buf, len);
        }
        xSemaphoreGive(s_web_buf_mutex);
        if (res == ESP_OK) return ESP_OK;
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

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t flash_handler(httpd_req_t *req) {
    g_flash_state = !g_flash_state;
    digitalWrite(FLASH_LED_PIN, g_flash_state ? HIGH : LOW);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, g_flash_state ? "ON" : "OFF", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t trigger_handler(httpd_req_t *req) {
    g_web_trigger_pending = true;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t set_prices_handler(httpd_req_t *req) {
    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[24];
        if (httpd_query_key_value(query, "apple", val, sizeof(val)) == ESP_OK) {
            float p = atof(val);
            if (p >= 0.0f) g_price_apple = p;
        }
        if (httpd_query_key_value(query, "orange", val, sizeof(val)) == ESP_OK) {
            float p = atof(val);
            if (p >= 0.0f) g_price_orange = p;
        }
        if (httpd_query_key_value(query, "pear", val, sizeof(val)) == ESP_OK) {
            float p = atof(val);
            if (p >= 0.0f) g_price_pear = p;
        }
        // Recalculate current telemetry price if a fruit is already on the scale
        if (g_telemetry.detected && g_telemetry.weight_g > 0.0f) {
            if (strcmp(g_telemetry.label, "apple") == 0)       g_telemetry.unit_price = g_price_apple;
            else if (strcmp(g_telemetry.label, "orange") == 0) g_telemetry.unit_price = g_price_orange;
            else if (strcmp(g_telemetry.label, "pear") == 0)   g_telemetry.unit_price = g_price_pear;
            g_telemetry.total_price = (g_telemetry.weight_g / 1000.0f) * g_telemetry.unit_price;
            STM32_Serial.printf("\r\nRES:%s,%d,%.1f,%.0f\r\n",
                                g_telemetry.label,
                                (int)(g_telemetry.confidence * 100.0f),
                                g_telemetry.weight_g,
                                g_telemetry.total_price);
        }
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

void startWebServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 7;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 2;
    config.lru_purge_enable = true;

    httpd_uri_t uri_index = { .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL };
    httpd_uri_t uri_capture = { .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL };
    httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    httpd_uri_t uri_flash = { .uri = "/flash", .method = HTTP_GET, .handler = flash_handler, .user_ctx = NULL };
    httpd_uri_t uri_trigger = { .uri = "/trigger", .method = HTTP_GET, .handler = trigger_handler, .user_ctx = NULL };
    httpd_uri_t uri_set_prices = { .uri = "/set_prices", .method = HTTP_GET, .handler = set_prices_handler, .user_ctx = NULL };

    if (httpd_start(&s_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(s_httpd, &uri_index);
        httpd_register_uri_handler(s_httpd, &uri_capture);
        httpd_register_uri_handler(s_httpd, &uri_status);
        httpd_register_uri_handler(s_httpd, &uri_flash);
        httpd_register_uri_handler(s_httpd, &uri_trigger);
        httpd_register_uri_handler(s_httpd, &uri_set_prices);
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
// Pre-Compute Center-Crop Resize Lookup Table
// ----------------------------------------------------------------------------
void initResizeLut() {
    if (!resize_lut) return;
    const int crop_dim = (CAMERA_RAW_WIDTH < CAMERA_RAW_HEIGHT) ? CAMERA_RAW_WIDTH : CAMERA_RAW_HEIGHT; // 240
    const int offset_x = (CAMERA_RAW_WIDTH - crop_dim) / 2;  // 40
    const int offset_y = (CAMERA_RAW_HEIGHT - crop_dim) / 2; // 0

    for (int y = 0; y < EI_CLASSIFIER_INPUT_HEIGHT; y++) {
        int src_y = offset_y + (y * crop_dim) / EI_CLASSIFIER_INPUT_HEIGHT;
        for (int x = 0; x < EI_CLASSIFIER_INPUT_WIDTH; x++) {
            int src_x = offset_x + (x * crop_dim) / EI_CLASSIFIER_INPUT_WIDTH;
            resize_lut[y * EI_CLASSIFIER_INPUT_WIDTH + x] = (uint32_t)((src_y * CAMERA_RAW_WIDTH + src_x) * 3);
        }
    }
    Serial.println("[LUT] Pre-computed 96x96 center-crop resize lookup table in RAM.");
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
    s_web_buf_mutex = xSemaphoreCreateMutex();

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

    jpeg_stage_buf = (uint8_t*)ps_malloc(MAX_JPEG_BUF_SIZE);
    if (!jpeg_stage_buf) jpeg_stage_buf = (uint8_t*)malloc(MAX_JPEG_BUF_SIZE);
    if (jpeg_stage_buf) {
        Serial.printf("[MEMORY] Allocated %u KB AI JPEG staging buffer in PSRAM.\n", MAX_JPEG_BUF_SIZE / 1024);
    } else {
        Serial.println("[CRITICAL ERROR] Insufficient memory for AI JPEG staging buffer!");
    }

    web_jpeg_buf = (uint8_t*)ps_malloc(MAX_JPEG_BUF_SIZE);
    if (!web_jpeg_buf) web_jpeg_buf = (uint8_t*)malloc(MAX_JPEG_BUF_SIZE);
    if (web_jpeg_buf) {
        Serial.printf("[MEMORY] Allocated %u KB web stream buffer in PSRAM.\n", MAX_JPEG_BUF_SIZE / 1024);
    } else {
        Serial.println("[CRITICAL ERROR] Insufficient memory for web stream buffer!");
    }


    // Allocate ai_input_buf (96 * 96 * 3 = 27,648 bytes) in Fast Internal SRAM
    size_t ai_buf_size = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * 3;
    ai_input_buf = (uint8_t*)heap_caps_malloc(ai_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ai_input_buf) {
        ai_input_buf = (uint8_t*)ps_malloc(ai_buf_size);
    }
    if (ai_input_buf) {
        Serial.printf("[MEMORY] Allocated %u KB AI input buffer in fast Internal SRAM.\n", ai_buf_size / 1024);
    } else {
        Serial.println("[CRITICAL ERROR] Insufficient memory for AI input buffer!");
    }

    // Allocate resize_lut (96 * 96 * sizeof(uint32_t) = 36,864 bytes)
    size_t lut_size = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT * sizeof(uint32_t);
    resize_lut = (uint32_t*)heap_caps_malloc(lut_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!resize_lut) {
        resize_lut = (uint32_t*)ps_malloc(lut_size);
    }
    if (resize_lut) {
        initResizeLut();
    } else {
        Serial.println("[WARNING] Insufficient memory for Resize LUT, will use software fallback.");
    }

    // 3. Start Wi-Fi SoftAP FIRST (Guarantees Wi-Fi is NEVER blocked by cold camera)
    Serial.println("\n[WIFI] Initializing SoftAP...");
    WiFi.mode(WIFI_AP);
    WiFi.setSleep(false);
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
    STM32_Serial.setRxBufferSize(512);
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
    if (!snapshot_buf || !jpeg_stage_buf || !ai_input_buf) {
        Serial.println("[ERROR] Cannot infer: Required buffers not allocated.");
        return false;
    }

    g_burst_active = true;
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
        // 1. Fast Thread-safe snapshot grab (< 1ms lock duration)
        size_t jpeg_len = 0;
        if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            camera_fb_t *fb = esp_camera_fb_get();
            if (fb) {
                if (fb->len <= MAX_JPEG_BUF_SIZE) {
                    memcpy(jpeg_stage_buf, fb->buf, fb->len);
                    jpeg_len = fb->len;
                }
                // Keep live web stream cache fresh under s_web_buf_mutex
                if (web_jpeg_buf && s_web_buf_mutex && xSemaphoreTake(s_web_buf_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    if (fb->len <= MAX_JPEG_BUF_SIZE) {
                        memcpy(web_jpeg_buf, fb->buf, fb->len);
                        g_cached_jpeg_len = fb->len;
                    }
                    xSemaphoreGive(s_web_buf_mutex);
                }
                esp_camera_fb_return(fb);
            }
            xSemaphoreGive(s_camera_mutex);
        }

        if (jpeg_len == 0) {
            Serial.printf("  ├─ Frame %d/%d: [FAIL] Camera buffer acquire timeout\n", frame + 1, BURST_SAMPLE_COUNT);
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        // Decode JPEG -> RGB888 OUTSIDE camera mutex so /capture never blocks
        if (!fmt2rgb888(jpeg_stage_buf, jpeg_len, PIXFORMAT_JPEG, snapshot_buf)) {
            Serial.printf("  ├─ Frame %d/%d: [FAIL] fmt2rgb888 decode failed\n", frame + 1, BURST_SAMPLE_COUNT);
            continue;
        }

        valid_frames++;

        // 2. Fast Cache-Friendly RAM LUT resize: 320x240 RGB888 -> 96x96 RGB888 in Internal SRAM (< 1 ms)
        if (resize_lut) {
            const size_t total_pixels = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
            const uint8_t * __restrict src_base = snapshot_buf;
            uint8_t * __restrict dst = ai_input_buf;
            const uint32_t * __restrict lut = resize_lut;
            for (size_t p = 0; p < total_pixels; p++) {
                const uint8_t *s = src_base + lut[p];
                dst[0] = s[0];
                dst[1] = s[1];
                dst[2] = s[2];
                dst += 3;
            }
        } else {
            ei::image::processing::crop_and_interpolate_rgb888(
                snapshot_buf,
                CAMERA_RAW_WIDTH,
                CAMERA_RAW_HEIGHT,
                ai_input_buf,
                EI_CLASSIFIER_INPUT_WIDTH,
                EI_CLASSIFIER_INPUT_HEIGHT
            );
        }

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

        // 2-of-3 Early-Exit Consensus check
        if (valid_frames == 2) {
            bool strong_apple  = (votes_apple == 2  && (conf_sum_apple / 2.0f) >= 0.60f);
            bool strong_orange = (votes_orange == 2 && (conf_sum_orange / 2.0f) >= 0.60f);
            bool strong_pear   = (votes_pear == 2   && (conf_sum_pear / 2.0f) >= 0.60f);
            bool both_empty    = (votes_apple == 0 && votes_orange == 0 && votes_pear == 0);

            if (strong_apple || strong_orange || strong_pear || both_empty) {
                Serial.println("  └─ [EARLY EXIT] 2/2 frames reached unanimous consensus!");
                break;
            }
        }

        // Yield between frames to let sensor exposure settle and Wi-Fi service packets
        if (frame + 1 < BURST_SAMPLE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(BURST_FRAME_DELAY_MS));
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

    if (override_weight_g >= 0.0f) {
        // Real HX711 data from STM32 (including 0g on an empty pan) ALWAYS wins,
        // regardless of whether the camera recognised a fruit or not.
        weight_g = override_weight_g;
        mode_str = "STM32 HX711 Live";
        g_telemetry.stm32_live = true;
    } else if (detected_any) {
        // No STM32 link yet -> fall back to simulated metrology (demo / bench test only)
        if (strcmp(winner_label, "apple") == 0)       weight_g = (float)random(1600, 2300) / 10.0f;
        else if (strcmp(winner_label, "orange") == 0) weight_g = (float)random(1400, 2000) / 10.0f;
        else if (strcmp(winner_label, "pear") == 0)   weight_g = (float)random(1800, 2700) / 10.0f;
        else                                          weight_g = (float)random(1500, 2200) / 10.0f;
        mode_str = "Random Realistic (Mock)";
        g_telemetry.stm32_live = false;
    } else {
        // No STM32 link and nothing detected -> empty reading
        g_telemetry.stm32_live = false;
    }

    // Pricing only applies when a fruit is recognised AND there is a positive mass
    if (detected_any && weight_g > 0.0f) {
        unit_price = 50000.0f;
        if (strcmp(winner_label, "apple") == 0)       unit_price = g_price_apple;
        else if (strcmp(winner_label, "orange") == 0) unit_price = g_price_orange;
        else if (strcmp(winner_label, "pear") == 0)   unit_price = g_price_pear;

        total_price = (weight_g / 1000.0f) * unit_price;
        sanity_ok = (weight_g > 0.0f);
    }

    uint8_t evaluated_frames = valid_frames > 0 ? valid_frames : BURST_SAMPLE_COUNT;

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
    g_telemetry.burst_total = evaluated_frames;
    g_telemetry.dsp_ms = valid_frames ? (total_dsp_ms / valid_frames) : 0;
    g_telemetry.infer_ms = valid_frames ? (total_infer_ms / valid_frames) : 0;
    g_telemetry.total_ms = t_burst_total;
    g_telemetry.fps = t_burst_total > 0 ? (1000.0f / (float)t_burst_total) : 0.0f;

    // 9. Output Result Summary to Serial Monitor
    Serial.println("  -----------------------------------------------------");
    if (detected_any) {
        Serial.printf("  >> BURST WINNER:   [%s] (%u/%u votes, %.0f%% consensus)\n",
                      winner_label, winner_votes, evaluated_frames,
                      ((float)winner_votes / evaluated_frames) * 100.0f);
        Serial.printf("  Average Confidence: %.1f%%\n", winner_avg_conf * 100.0f);
        Serial.printf("  Mass:               %.1f g (%.3f kg) [%s]\n", weight_g, weight_g / 1000.0f, mode_str);
        Serial.printf("  Unit Price:         %.0f VND / kg\n", unit_price);
        Serial.printf("  TOTAL CHARGE:       %.0f VND\n", total_price);
        Serial.printf("  Weight Check:       %s\n", sanity_ok ? "PASSED (Positive weight)" : "ZERO WEIGHT");
    } else {
        Serial.printf("  >> RESULT:          EMPTY SCALE (0/%u detections)\n", evaluated_frames);
    }
    Serial.printf("  Burst Duration:     %u ms (avg infer: %u ms/frame)\n",
                  t_burst_total, valid_frames ? (total_infer_ms / valid_frames) : 0);
    Serial.println("=======================================================\n");

    // 10. Forward Result to STM32 Master via UART
    if (detected_any) {
        // Send 4-field ASCII response for STM32 LCD: RES:<fruit>,<conf>,<weight>,<price>
        STM32_Serial.printf("\r\nRES:%s,%d,%.1f,%.0f\r\n",
                            winner_label,
                            (int)(winner_avg_conf * 100.0f),
                            weight_g,
                            total_price);

        // NOTE: Only the ASCII RES line is sent. The legacy binary frame
        // (0xAA ... 0x55 '\n') was removed because its printable bytes corrupted
        // the STM32's ASCII line buffer (uart_rx_buf).

        vTaskDelay(pdMS_TO_TICKS(15));
        digitalWrite(STATUS_LED_PIN, HIGH);
    } else {
        STM32_Serial.printf("\r\nRES:none,0,%.1f,0\r\n", weight_g);
    }

    g_burst_active = false;
    return true;
}

// ----------------------------------------------------------------------------
// Signal Callback: Converts Fast Internal SRAM RGB888 to Edge Impulse Floats
// ----------------------------------------------------------------------------
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        out_ptr[out_ptr_ix] = (ai_input_buf[pixel_ix + 2] << 16) +
                              (ai_input_buf[pixel_ix + 1] << 8) +
                              ai_input_buf[pixel_ix];
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
                    } else if (w == 0.0f) {
                        s_has_live_stm32 = true;
                        s_last_live_weight_g = 0.0f;
                        strncpy(g_telemetry.label, "none", sizeof(g_telemetry.label) - 1);
                        g_telemetry.confidence = 0.0f;
                        g_telemetry.weight_g = 0.0f;
                        g_telemetry.unit_price = 0.0f;
                        g_telemetry.total_price = 0.0f;
                        g_telemetry.sanity_ok = false;
                        g_telemetry.detected = false;
                        g_telemetry.votes = 0;
                        g_telemetry.stm32_live = true;
                        digitalWrite(STATUS_LED_PIN, HIGH);
                        Serial.println("\n[STM32 UART] 'W:0' received -> Scale emptied (Tare reset).");
                        s_stm32_rx_len = 0;
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

    if (g_web_trigger_pending) {
        g_web_trigger_pending = false;
        runBurstInference(s_has_live_stm32 ? s_last_live_weight_g : -1.0f);
    }

    // In continuous mode, run a burst periodically (every 2.5 seconds)
    if (g_continuous_mode) {
        static uint32_t s_last_burst_time = 0;
        if (millis() - s_last_burst_time >= 2500) {
            s_last_burst_time = millis();
            runBurstInference(s_has_live_stm32 ? s_last_live_weight_g : -1.0f);
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
        } else if (ch == 'b' || ch == 'B') {
            printBenchmarkReport();
        } else if (ch == '?') {
            Serial.println("\n--- INTERACTIVE COMMANDS ---");
            Serial.println("  [f] Toggle High-Power Flashlight LED");
            Serial.println("  [m] Toggle Continuous vs. Triggered mode");
            Serial.println("  [Space] Trigger Burst Inference (in Triggered mode)");
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
