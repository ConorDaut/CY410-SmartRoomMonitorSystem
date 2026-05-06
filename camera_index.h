// ============================================================
// camera_index.h  –  Smart Room Monitor
//
// In the reference template (CameraWebServerAP), this file
// contained large gzip-compressed HTML blobs for the OV2640
// and OV3660 camera interfaces (index_ov2640_html_gz and
// index_ov3660_html_gz), which were served directly from flash.
//
// In this project the entire web interface (login page,
// dashboard, gallery, JavaScript) is generated dynamically
// inside app_httpd.cpp using chunked HTTP responses built from
// C string literals.  This approach:
//
//   1. Eliminates the need for pre-compressed binary blobs.
//   2. Allows the HTML to embed session-specific data
//      (e.g., the logged-in username) at request time.
//   3. Keeps the project self-contained and easy to modify.
//
// If you need to add static assets in the future (CSS files,
// icons, etc.) you can store them in SPIFFS and serve them
// through a new URI handler registered in startCameraServer().
//
// This header is intentionally empty; it exists only to
// preserve the four-file project structure that mirrors the
// reference template.
// ============================================================

#pragma once

// (No symbols exported from this file)
