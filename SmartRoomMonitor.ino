// ============================================================
// Smart Room Monitor System
// ESP32-CAM with OV3660 Camera
// Authors: Jalal Akhoun, Conor Daut, Caleb Palmer,
//          Dawson Westnedge, Lauryn Holloway
// Course:  Cy410
// ============================================================
//
// Description:
//   This sketch starts the ESP32 as a WiFi Access Point and
//   serves a web-based Smart Room Monitor dashboard.
//   Features:
//     - Live MJPEG video stream
//     - Username/password login (session-based)
//     - Capture still images stored in SPIFFS with metadata
//     - Per-user image gallery with timestamps (newest first)
//     - Connection-status indicator
//     - Image limit enforcement (configurable, default 20)
//     - Clear-all-data endpoint (password protected)
//
// Hardware: AI-Thinker ESP32-CAM board with OV3660 sensor
//
// WiFi AP Credentials (change as desired):
//   SSID     : SmartRoomMonitor
//   Password : monitor123
//
// Default User Accounts (username : password):
//   admin    : admin123
//   guest    : guest123
//
// Access the dashboard at: http://192.168.4.1
// ============================================================

#include "esp_camera.h"
#include <WiFi.h>
#include "SPIFFS.h"
#include "camera_pins.h"

// ----- Camera Model Selection --------------------------------
// Using AI-Thinker board which mounts the OV3660 sensor.
// The OV3660 PID is detected at runtime and sensor settings
// are adjusted accordingly (see setup() below).
#define CAMERA_MODEL_WROVER_KIT  // Has PSRAM

// ----- AP Network Credentials --------------------------------
const char* ap_ssid     = "SmartRoomMonitor";
const char* ap_password = "monitor123";

// ----- Image Storage Config ----------------------------------
// Maximum number of images stored across ALL users.
// When the limit is reached, the oldest image is deleted
// before a new one is saved  (R-IMG-04).
#define MAX_STORED_IMAGES 20

// Forward declaration (implemented in app_httpd.cpp)
void startCameraServer();

// =============================================================
void setup() {
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    Serial.println();
    Serial.println("=== Smart Room Monitor System ===");

    // ---- Mount SPIFFS for persistent image/metadata storage ----
    // R-IMG-01 / R-DATA-01
    if (!SPIFFS.begin(true)) {
        Serial.println("[ERROR] SPIFFS mount failed! Images will not persist.");
    } else {
        Serial.printf("[INFO] SPIFFS mounted. Total: %u KB  Used: %u KB\n",
                      SPIFFS.totalBytes() / 1024,
                      SPIFFS.usedBytes()  / 1024);
    }

    // ---- Camera Configuration ----------------------------------
    camera_config_t config;
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = Y2_GPIO_NUM;
    config.pin_d1        = Y3_GPIO_NUM;
    config.pin_d2        = Y4_GPIO_NUM;
    config.pin_d3        = Y5_GPIO_NUM;
    config.pin_d4        = Y6_GPIO_NUM;
    config.pin_d5        = Y7_GPIO_NUM;
    config.pin_d6        = Y8_GPIO_NUM;
    config.pin_d7        = Y9_GPIO_NUM;
    config.pin_xclk      = XCLK_GPIO_NUM;
    config.pin_pclk      = PCLK_GPIO_NUM;
    config.pin_vsync     = VSYNC_GPIO_NUM;
    config.pin_href      = HREF_GPIO_NUM;
    config.pin_sscb_sda  = SIOD_GPIO_NUM;
    config.pin_sscb_scl  = SIOC_GPIO_NUM;
    config.pin_pwdn      = PWDN_GPIO_NUM;
    config.pin_reset     = RESET_GPIO_NUM;
    config.xclk_freq_hz  = 20000000;
    config.pixel_format  = PIXFORMAT_JPEG;

    // Use higher resolution / quality when PSRAM is available
    if (psramFound()) {
        config.frame_size   = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count     = 2;
        Serial.println("[INFO] PSRAM found – using UXGA resolution");
    } else {
        config.frame_size   = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count     = 1;
        Serial.println("[INFO] No PSRAM – using SVGA resolution");
    }

    // ---- Initialize Camera -------------------------------------
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[ERROR] Camera init failed: 0x%x\n", err);
        return;
    }
    Serial.println("[INFO] Camera initialized");

    // ---- OV3660-Specific Sensor Tuning -------------------------
    // R-IOT-01: system supports OV3660 camera; flip + brightness
    // adjustments corrected here so the image looks natural.
    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);       // flip back to correct orientation
        s->set_brightness(s, 1);  // slightly increase brightness
        s->set_saturation(s, -2); // reduce over-saturation
        Serial.println("[INFO] OV3660 sensor tuning applied");
    }
    // Start with QVGA for a higher initial frame rate on the stream
    s->set_framesize(s, FRAMESIZE_QVGA);

    // ---- Start WiFi Access Point -------------------------------
    // R-IOT-01: users connect to the ESP32 AP directly
    WiFi.softAP(ap_ssid, ap_password, 6 /* channel */);
    delay(500);  // give the AP time to come up
    IPAddress myIP = WiFi.softAPIP();
    Serial.printf("[INFO] AP started. SSID: %s  IP: %s\n", ap_ssid, myIP.toString().c_str());

    // ---- Start HTTP Servers ------------------------------------
    startCameraServer();

    Serial.printf("[INFO] Dashboard ready at http://%s\n", myIP.toString().c_str());
    Serial.println("[INFO] Stream server on port 81");
}

// =============================================================
void loop() {
    // Everything is handled by FreeRTOS tasks spun up by the
    // ESP-IDF HTTP server.  Nothing to do here except yield.
    delay(10000);
}
