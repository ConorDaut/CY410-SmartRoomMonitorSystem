// ============================================================
// app_httpd.cpp  –  Smart Room Monitor HTTP Server
//
// Handles all HTTP endpoints:
//   GET  /           → login page (if not authenticated)
//                      or dashboard (if authenticated)
//   POST /login      → authenticate user, start session
//   GET  /logout     → destroy session
//   GET  /stream     → MJPEG live video stream  (port 81)
//   GET  /capture    → capture JPEG, store to SPIFFS, return JSON
//   GET  /gallery    → JSON array of images for current user
//   GET  /image?f=X  → serve a stored JPEG from SPIFFS
//   GET  /delete?f=X → delete a single image (owner or admin only)
//   GET  /status     → JSON camera connection status
//   POST /cleardata  → delete ALL stored images (password protected)
//
// Session management:
//   Sessions are stored in a small in-memory table keyed by a
//   random 16-hex-char token held in a cookie called "srm_sess".
//   Sessions expire after SESSION_TIMEOUT_S seconds of inactivity.
//
// User accounts:
//   Defined at compile time in USER_TABLE below.  Passwords are
//   stored as plain strings suitable for an embedded system with
//   no external network exposure (AP-only mode).
//
// Image metadata:
//   Each captured image is saved to SPIFFS as /img/<token>.jpg
//   A companion CSV line is appended to /metadata.csv:
//     filename,username,unix_timestamp_ms
//   On startup the CSV is parsed to rebuild the in-memory index.
//
// Requirements addressed (see Functional Requirements doc):
//   R-IOT-01..05, R-UI-01..05, R-USER-01..03,
//   R-IMG-01..05, R-NOTIF-01..02, R-DATA-01..02
// ============================================================

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"
#include "SPIFFS.h"
#include "FS.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

// ---- Compile-time configuration ----
#define MAX_STORED_IMAGES   20          // R-IMG-04: cap on total stored images
#define MAX_SESSIONS        8           // concurrent login sessions
#define SESSION_TIMEOUT_S   1800        // 30 minutes inactivity → logout
#define ADMIN_CLEAR_PASS    "clearall"  // R-DATA-02: password for /cleardata
#define METADATA_FILE       "/metadata.csv"
#define IMAGE_DIR           "/img"

// ---- MJPEG stream constants ----
#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ---- HTTP server handles ----
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

// =============================================================
//  User account table  (R-USER-01)
// =============================================================
typedef struct {
    const char *username;
    const char *password;
} UserAccount;

static const UserAccount USER_TABLE[] = {
    {"admin",  "admin123"},
    {"guest",  "guest123"},
    // Add more accounts here as needed
};
static const int USER_COUNT = sizeof(USER_TABLE) / sizeof(USER_TABLE[0]);

// =============================================================
//  Session table  (R-USER-01 / R-USER-02)
// =============================================================
typedef struct {
    char   token[33];        // 32 hex chars + null
    char   username[32];
    time_t last_active;      // unix epoch seconds
    bool   active;
} Session;

static Session g_sessions[MAX_SESSIONS];

// Simple pseudo-random token generator (good enough for LAN use)
static void generate_token(char *out, size_t len) {
    // len should be 33 (32 hex chars + '\0')
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < len - 1; i++) {
        out[i] = hex[esp_random() & 0x0F];
    }
    out[len - 1] = '\0';
}

static Session *find_session(const char *token) {
    if (!token || token[0] == '\0') return NULL;
    time_t now = time(NULL);
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].active &&
            strncmp(g_sessions[i].token, token, 32) == 0) {
            if (now - g_sessions[i].last_active > SESSION_TIMEOUT_S) {
                // Expired – invalidate
                g_sessions[i].active = false;
                return NULL;
            }
            g_sessions[i].last_active = now;
            return &g_sessions[i];
        }
    }
    return NULL;
}

static Session *create_session(const char *username) {
    time_t now = time(NULL);
    // Reuse expired slot if available
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].active ||
            (now - g_sessions[i].last_active > SESSION_TIMEOUT_S)) {
            g_sessions[i].active = true;
            generate_token(g_sessions[i].token, sizeof(g_sessions[i].token));
            strncpy(g_sessions[i].username, username, sizeof(g_sessions[i].username) - 1);
            g_sessions[i].username[sizeof(g_sessions[i].username) - 1] = '\0';
            g_sessions[i].last_active = now;
            return &g_sessions[i];
        }
    }
    return NULL; // all slots taken
}

static void destroy_session(const char *token) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].active &&
            strncmp(g_sessions[i].token, token, 32) == 0) {
            g_sessions[i].active = false;
            break;
        }
    }
}

// =============================================================
//  Cookie helper: extract "srm_sess" value from Cookie header
// =============================================================
static void extract_cookie_token(httpd_req_t *req, char *out, size_t out_len) {
    out[0] = '\0';
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (hdr_len == 0) return;

    char *hdr = (char *)malloc(hdr_len + 1);
    if (!hdr) return;
    httpd_req_get_hdr_value_str(req, "Cookie", hdr, hdr_len + 1);

    // Find "srm_sess="
    const char *key = "srm_sess=";
    char *pos = strstr(hdr, key);
    if (pos) {
        pos += strlen(key);
        size_t i = 0;
        while (*pos && *pos != ';' && i < out_len - 1) {
            out[i++] = *pos++;
        }
        out[i] = '\0';
    }
    free(hdr);
}

// =============================================================
//  Image metadata  (R-DATA-01 / R-IMG-05)
// =============================================================
// Stored in SPIFFS as /metadata.csv:
//   filename,username,timestamp_ms
// In-memory index (newest first by sorting on load)

#define MAX_META_ENTRIES (MAX_STORED_IMAGES + 5)

typedef struct {
    char   filename[32];    // e.g. "abc123de.jpg"
    char   username[32];    //Security solution -> username is stored with the image, so check the username
    long long timestamp_ms;
} ImageMeta;

static ImageMeta g_images[MAX_META_ENTRIES];
static int g_image_count = 0;

// Append one record to the CSV file
static void meta_append(const ImageMeta *m) {
    File f = SPIFFS.open(METADATA_FILE, FILE_APPEND);
    if (!f) return;
    f.printf("%s,%s,%lld\n", m->filename, m->username, m->timestamp_ms);
    f.close();
}

// Rewrite the entire CSV from the in-memory array (used after deletions)
static void meta_rewrite_all() {
    File f = SPIFFS.open(METADATA_FILE, FILE_WRITE);
    if (!f) return;
    for (int i = 0; i < g_image_count; i++) {
        f.printf("%s,%s,%lld\n",
                 g_images[i].filename,
                 g_images[i].username,
                 g_images[i].timestamp_ms);
    }
    f.close();
}

// Parse the CSV into g_images[] on startup (newest first)
static void meta_load() {
    g_image_count = 0;
    if (!SPIFFS.exists(METADATA_FILE)) return;

    File f = SPIFFS.open(METADATA_FILE, FILE_READ);
    if (!f) return;

    while (f.available() && g_image_count < MAX_META_ENTRIES) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;

        // Parse comma-separated tokens
        int c1 = line.indexOf(',');
        int c2 = line.lastIndexOf(',');
        if (c1 < 0 || c2 < 0 || c1 == c2) continue;

        String fname = line.substring(0, c1);
        String uname = line.substring(c1 + 1, c2);
        String ts    = line.substring(c2 + 1);

        // Verify the image file actually exists
        String path = String(IMAGE_DIR) + "/" + fname;
        if (!SPIFFS.exists(path)) continue;

        strncpy(g_images[g_image_count].filename, fname.c_str(), 31);
        strncpy(g_images[g_image_count].username, uname.c_str(), 31);
        g_images[g_image_count].filename[31] = '\0';
        g_images[g_image_count].username[31] = '\0';
        g_images[g_image_count].timestamp_ms = atoll(ts.c_str());
        g_image_count++;
    }
    f.close();

    // Sort newest first (bubble sort – small N)
    for (int i = 0; i < g_image_count - 1; i++) {
        for (int j = 0; j < g_image_count - i - 1; j++) {
            if (g_images[j].timestamp_ms < g_images[j+1].timestamp_ms) {
                ImageMeta tmp = g_images[j];
                g_images[j]   = g_images[j+1];
                g_images[j+1] = tmp;
            }
        }
    }
    Serial.printf("[META] Loaded %d image records from SPIFFS\n", g_image_count);
}

// Remove the oldest image when over the limit  (R-IMG-04)
static void enforce_image_limit() {
    while (g_image_count >= MAX_STORED_IMAGES) {
        // Oldest is at the end (array is newest-first)
        int oldest = g_image_count - 1;
        String path = String(IMAGE_DIR) + "/" + g_images[oldest].filename;
        SPIFFS.remove(path);
        Serial.printf("[META] Limit reached – deleted oldest: %s\n", path.c_str());
        g_image_count--;
    }
}

// Delete image by filename; return true if found
static bool meta_delete(const char *filename) {
    for (int i = 0; i < g_image_count; i++) {
        if (strcmp(g_images[i].filename, filename) == 0) {
            String path = String(IMAGE_DIR) + "/" + filename;
            SPIFFS.remove(path);
            // Shift array left
            for (int j = i; j < g_image_count - 1; j++) {
                g_images[j] = g_images[j+1];
            }
            g_image_count--;
            meta_rewrite_all();
            return true;
        }
    }
    return false;
}

// =============================================================
//  URL query-string helper
// =============================================================
static bool get_query_param(httpd_req_t *req,
                            const char *key,
                            char *out, size_t out_len) {
    size_t qlen = httpd_req_get_url_query_len(req) + 1;
    if (qlen <= 1) return false;
    char *buf = (char *)malloc(qlen);
    if (!buf) return false;
    bool found = false;
    if (httpd_req_get_url_query_str(req, buf, qlen) == ESP_OK) {
        found = (httpd_query_key_value(buf, key, out, out_len) == ESP_OK);
    }
    free(buf);
    return found;
}

// =============================================================
//  POST body reader helper
// =============================================================
static bool read_post_body(httpd_req_t *req, char *out, size_t out_len) {
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= out_len) return false;
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, out + received, total - received);
        if (ret <= 0) return false;
        received += ret;
    }
    out[received] = '\0';
    return true;
}

// Extract value of key from URL-encoded body: "key=value&..."
static bool parse_form_field(const char *body,
                             const char *key,
                             char *out, size_t out_len) {
    // Build search prefix "key="
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s=", key);
    const char *pos = strstr(body, prefix);
    if (!pos) return false;
    pos += strlen(prefix);
    size_t i = 0;
    while (*pos && *pos != '&' && i < out_len - 1) {
        // Basic URL-decode for '+' (space) only; good enough for passwords
        out[i++] = (*pos == '+') ? ' ' : *pos;
        pos++;
    }
    out[i] = '\0';
    return true;
}

// =============================================================
//  Simple HTML helpers
// =============================================================
static const char *HTML_HEADER =
    "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Smart Room Monitor</title>"
    "<style>"
    "  body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;margin:0;padding:0;}"
    "  h1{color:#e94560;text-align:center;padding:20px 0 0;}"
    "  .subtitle{text-align:center;color:#aaa;margin:0 0 20px;font-size:0.9em;}"
    "  .container{max-width:960px;margin:0 auto;padding:0 16px 40px;}"
    "  .card{background:#16213e;border-radius:10px;padding:20px;margin-bottom:20px;"
    "        box-shadow:0 4px 12px rgba(0,0,0,0.4);}"
    "  .status-bar{display:flex;align-items:center;gap:10px;padding:10px 20px;"
    "              background:#0f3460;justify-content:space-between;}"
    "  .status-dot{width:12px;height:12px;border-radius:50%;display:inline-block;}"
    "  .dot-green{background:#4caf50;} .dot-red{background:#f44336;}"
    "  .stream-wrap{text-align:center;}"
    "  .stream-wrap img{max-width:100%;border-radius:8px;border:2px solid #e94560;}"
    "  .btn{padding:10px 22px;border:none;border-radius:6px;cursor:pointer;"
    "       font-size:1em;transition:opacity 0.2s;}"
    "  .btn-primary{background:#e94560;color:#fff;}"
    "  .btn-secondary{background:#0f3460;color:#eee;}"
    "  .btn-danger{background:#b71c1c;color:#fff;}"
    "  .btn:hover{opacity:0.85;}"
    "  .btn:disabled{opacity:0.5;cursor:not-allowed;}"
    "  .gallery{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:16px;}"
    "  .gallery-item{background:#0f3460;border-radius:8px;overflow:hidden;}"
    "  .gallery-item img{width:100%;display:block;}"
    "  .gallery-item .meta{padding:8px;font-size:0.78em;color:#aaa;}"
    "  .gallery-item .del-btn{margin:4px 8px 8px;}"
    "  .notif{padding:10px 16px;border-radius:6px;margin:10px 0;font-weight:bold;"
    "         display:none;}"
    "  .notif-success{background:#1b5e20;color:#a5d6a7;border:1px solid #4caf50;}"
    "  .notif-error  {background:#b71c1c;color:#ef9a9a;border:1px solid #f44336;}"
    "  .login-box{max-width:380px;margin:80px auto;}"
    "  .login-box input{width:100%;padding:10px;margin:8px 0;border-radius:6px;"
    "                   border:1px solid #0f3460;background:#0f3460;color:#eee;"
    "                   box-sizing:border-box;font-size:1em;}"
    "  .login-box .btn{width:100%;margin-top:10px;}"
    "  .err-msg{color:#ef9a9a;text-align:center;font-size:0.9em;}"
    "  nav{display:flex;justify-content:flex-end;gap:10px;}"
    "  #reconnect-btn{display:none;}"
    "</style></head><body>";

static const char *HTML_FOOTER = "</body></html>";

// =============================================================
//  Login page handler  (R-USER-01)
// =============================================================
static esp_err_t login_page_handler(httpd_req_t *req, bool bad_creds) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEADER);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "<div class='container'>"
        "<div class='card login-box'>"
        "<h1>&#128247; Smart Room Monitor</h1>"
        "<p class='subtitle'>Please log in to continue</p>"
        "%s"
        "<input type='text'     id='u' placeholder='Username'>"
        "<input type='password' id='p' placeholder='Password'>"
        "<button class='btn btn-primary' onclick='doLogin()'>Log In</button>"
        "</div></div>"
        "<script>"
        "function doLogin(){"
        "  var u=document.getElementById('u').value;"
        "  var p=document.getElementById('p').value;"
        "  fetch('/login',{method:'POST',"
        "    headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "    body:'username='+encodeURIComponent(u)+'&password='+encodeURIComponent(p)"
        "  }).then(r=>{"
        "    if(r.ok){window.location.href='/';}"
        "    else{document.querySelector('.err-msg').textContent='Invalid credentials.';}"
        "  });"
        "}"
        "document.addEventListener('keydown',e=>{if(e.key==='Enter')doLogin();});"
        "</script>",
        bad_creds ? "<p class='err-msg'>Invalid username or password.</p>" : "");
    httpd_resp_sendstr_chunk(req, buf);
    httpd_resp_sendstr_chunk(req, HTML_FOOTER);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// =============================================================
//  POST /login  (R-USER-01)
// =============================================================
static esp_err_t login_post_handler(httpd_req_t *req) {
    char body[256] = {0};
    if (!read_post_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char username[32] = {0};
    char password[64] = {0};
    parse_form_field(body, "username", username, sizeof(username));
    parse_form_field(body, "password", password, sizeof(password));

    // Validate credentials
    bool valid = false;
    for (int i = 0; i < USER_COUNT; i++) {
        if (strcmp(USER_TABLE[i].username, username) == 0 &&
            strcmp(USER_TABLE[i].password, password) == 0) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Invalid credentials");
        return ESP_OK;
    }

    Session *sess = create_session(username);
    if (!sess) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Session table full");
        return ESP_FAIL;
    }

    char cookie[80];
    snprintf(cookie, sizeof(cookie),
             "srm_sess=%s; Path=/; HttpOnly", sess->token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_sendstr(req, "OK");
    Serial.printf("[AUTH] Login: %s\n", username);
    return ESP_OK;
}

// =============================================================
//  GET /logout
// =============================================================
static esp_err_t logout_handler(httpd_req_t *req) {
    char token[33] = {0};
    extract_cookie_token(req, token, sizeof(token));
    destroy_session(token);
    // Expire the cookie
    httpd_resp_set_hdr(req, "Set-Cookie",
        "srm_sess=; Path=/; Max-Age=0; HttpOnly");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// =============================================================
//  GET /  – Dashboard  (R-UI-01..05, R-IOT-04)
// =============================================================
static esp_err_t index_handler(httpd_req_t *req) {
    char token[33] = {0};
    extract_cookie_token(req, token, sizeof(token));
    Session *sess = find_session(token);

    if (!sess) {
        return login_page_handler(req, false);
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr_chunk(req, HTML_HEADER);

    // Status bar (R-IOT-04)
    char status_bar[256];
    snprintf(status_bar, sizeof(status_bar),
        "<div class='status-bar'>"
        "  <div>"
        "    <span class='status-dot dot-green' id='cam-dot'></span>"
        "    <span id='cam-status'>Camera: Connected</span>"
        "  </div>"
        "  <nav>"
        "    <span style='color:#aaa'>Logged in as <b>%s</b></span>"
        "    <a href='/logout'><button class='btn btn-secondary'>Logout</button></a>"
        "  </nav>"
        "</div>", sess->username);
    httpd_resp_sendstr_chunk(req, status_bar);

    // Main content
    static const char *body =
        "<div class='container'>"
        "<h1>&#128247; Smart Room Monitor</h1>"
        "<p class='subtitle'>Live surveillance dashboard</p>"

        // Notification area (R-NOTIF-01, R-NOTIF-02)
        "<div id='notif' class='notif'></div>"

        // Live stream card (R-UI-01, R-IOT-02)
        "<div class='card'>"
        "  <h2>&#127909; Live Stream</h2>"
        "  <div class='stream-wrap'>"
        "    <img id='stream-img' src='http://'+window.location.hostname+':81/stream'"
        "         onerror='onStreamError()' onload='onStreamOk()' alt='Camera Feed'>"
        "  </div>"
        "  <div style='margin-top:14px;display:flex;gap:10px;flex-wrap:wrap;'>"
        "    <button class='btn btn-primary' id='capture-btn' onclick='captureImage()'>"
        "      &#128248; Capture Image"
        "    </button>"
        "    <button class='btn btn-secondary' id='reconnect-btn' onclick='reconnect()'>"
        "      &#128257; Reconnect"
        "    </button>"
        "  </div>"
        "</div>"

        // Gallery card (R-UI-03, R-UI-04, R-IMG-02, R-IMG-05)
        "<div class='card'>"
        "  <h2>&#128444; Captured Images</h2>"
        "  <div class='gallery' id='gallery'>"
        "    <p style='color:#aaa'>Loading gallery...</p>"
        "  </div>"
        "</div>"
        "</div>"; // .container

    httpd_resp_sendstr_chunk(req, body);

    // JavaScript
    static const char *scripts =
        "<script>"
        // Stream img points to port 81
        "var host = window.location.hostname;"
        "document.getElementById('stream-img').src = 'http://'+host+':81/stream';"

        // Connection status helpers (R-IOT-04, R-IOT-05)
        "function onStreamOk(){"
        "  document.getElementById('cam-dot').className='status-dot dot-green';"
        "  document.getElementById('cam-status').textContent='Camera: Connected';"
        "  document.getElementById('reconnect-btn').style.display='none';"
        "}"
        "function onStreamError(){"
        "  document.getElementById('cam-dot').className='status-dot dot-red';"
        "  document.getElementById('cam-status').textContent='Camera: Disconnected';"
        "  document.getElementById('reconnect-btn').style.display='inline-block';"
        "  showNotif('Camera stream lost. Click Reconnect to retry.','error');"
        "}"
        "function reconnect(){"
        "  var img=document.getElementById('stream-img');"
        "  img.src='';"
        "  setTimeout(function(){img.src='http://'+host+':81/stream';},500);"
        "}"

        // Notification helper (R-NOTIF-01, R-NOTIF-02)
        "function showNotif(msg,type){"
        "  var n=document.getElementById('notif');"
        "  n.textContent=msg;"
        "  n.className='notif '+(type==='error'?'notif-error':'notif-success');"
        "  n.style.display='block';"
        "  setTimeout(function(){n.style.display='none';},5000);"
        "}"

        // Capture (R-IOT-03, R-UI-02, R-NOTIF-01)
        "function captureImage(){"
        "  var btn=document.getElementById('capture-btn');"
        "  btn.disabled=true;"
        "  btn.textContent='Capturing...';"
        "  fetch('/capture')"
        "    .then(function(r){"
        "      if(r.ok) return r.json();"
        "      throw new Error('Capture failed (HTTP '+r.status+')');"
        "    })"
        "    .then(function(d){"
        "      showNotif('Image captured: '+d.filename,'success');"
        "      loadGallery();"
        "    })"
        "    .catch(function(e){"
        "      showNotif(e.message,'error');"
        "    })"
        "    .finally(function(){"
        "      btn.disabled=false;"
        "      btn.textContent='\\u{1F4F8} Capture Image';"
        "    });"
        "}"

        // Gallery loader (R-UI-03, R-UI-04, R-IMG-02, R-IMG-05)
        "function loadGallery(){"
        "  fetch('/gallery')"
        "    .then(function(r){return r.json();})"
        "    .then(function(imgs){"
        "      var g=document.getElementById('gallery');"
        "      if(!imgs||imgs.length===0){"
        "        g.innerHTML=\"<p style='color:#aaa'>No captured images yet.</p>\";"
        "        return;"
        "      }"
        "      var html='';"
        "      imgs.forEach(function(img){"
        "        var ts=new Date(img.timestamp_ms);"
        "        var label=ts.toLocaleString();"
        "        html+=\"<div class='gallery-item'>\";"
        "        html+=\"  <img src='/image?f=\"+encodeURIComponent(img.filename)+\"'"
        "              + \" alt='Captured image' loading='lazy'>\";"
        "        html+=\"  <div class='meta'>\";"
        "        html+=\"    <div>&#128100; \"+img.username+\"</div>\";"
        "        html+=\"    <div>&#128336; \"+label+\"</div>\";"
        "        html+=\"  </div>\";"
        "        html+=\"  <button class='btn btn-danger del-btn' style='font-size:0.8em;padding:5px 10px;'"
        "              + \" onclick=\\\"deleteImage('\"+img.filename+\"')\\\">&#128465; Delete</button>\";"
        "        html+=\"</div>\";"
        "      });"
        "      g.innerHTML=html;"
        "    })"
        "    .catch(function(){ });"
        "}"

        // Delete image (R-IMG-03)
        "function deleteImage(fname){"
        "  if(!confirm('Delete this image?')) return;"
        "  fetch('/delete?f='+encodeURIComponent(fname))"
        "    .then(function(r){"
        "      if(r.ok){showNotif('Image deleted.','success');loadGallery();}"
        "      else{showNotif('Delete failed.','error');}"
        "    });"
        "}"

        // Load gallery on page ready
        "window.onload=function(){loadGallery();};"
        "</script>";

    httpd_resp_sendstr_chunk(req, scripts);
    httpd_resp_sendstr_chunk(req, HTML_FOOTER);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

// =============================================================
//  GET /stream  – MJPEG stream  (R-IOT-02)
// =============================================================
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t  *fb       = NULL;
    esp_err_t     res      = ESP_OK;
    size_t        jpg_len  = 0;
    uint8_t      *jpg_buf  = NULL;
    char          part_buf[64];

    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK) return res;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[STREAM] Camera capture failed");
            res = ESP_FAIL;
            break;
        }

        if (fb->format != PIXFORMAT_JPEG) {
            bool ok = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
            esp_camera_fb_return(fb);
            fb = NULL;
            if (!ok) { res = ESP_FAIL; break; }
        } else {
            jpg_buf = fb->buf;
            jpg_len = fb->len;
        }

        // Send MJPEG boundary + part header
        res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY,
                                    strlen(_STREAM_BOUNDARY));
        if (res == ESP_OK) {
            size_t hlen = snprintf(part_buf, sizeof(part_buf),
                                   _STREAM_PART, jpg_len);
            res = httpd_resp_send_chunk(req, part_buf, hlen);
        }
        if (res == ESP_OK) {
            res = httpd_resp_send_chunk(req,
                                        (const char *)jpg_buf, jpg_len);
        }

        if (fb) {
            esp_camera_fb_return(fb);
            fb = NULL;
            jpg_buf = NULL;
        } else if (jpg_buf) {
            free(jpg_buf);
            jpg_buf = NULL;
        }
        if (res != ESP_OK) break;
    }
    return res;
}

// =============================================================
//  GET /capture  – Still image capture  (R-IOT-03, R-IMG-01)
// =============================================================
static esp_err_t capture_handler(httpd_req_t *req) {
    // Must be logged in
    char token[33] = {0};
    extract_cookie_token(req, token, sizeof(token));
    Session *sess = find_session(token);
    if (!sess) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"not authenticated\"}");
        return ESP_OK;
    }

    // Grab frame
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"camera capture failed\"}");
        Serial.println("[CAPTURE] Camera capture failed");
        return ESP_FAIL;
    }

    // Convert to JPEG if needed
    uint8_t *jpg_buf = NULL;
    size_t   jpg_len = 0;
    bool     allocated = false;
    if (fb->format == PIXFORMAT_JPEG) {
        jpg_buf = fb->buf;
        jpg_len = fb->len;
    } else {
        allocated = frame2jpg(fb, 80, &jpg_buf, &jpg_len);
        if (!allocated) {
            esp_camera_fb_return(fb);
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"error\":\"jpeg conversion failed\"}");
            return ESP_FAIL;
        }
    }

    // Enforce storage limit BEFORE saving  (R-IMG-04)
    enforce_image_limit();

    // Generate unique filename using esp_timer
    char filename[32];
    snprintf(filename, sizeof(filename), "%llx.jpg",
             (unsigned long long)esp_timer_get_time());

    // Save to SPIFFS
    String path = String(IMAGE_DIR) + "/" + filename;

    // Ensure /img directory exists (SPIFFS is flat, but we namespace the key)
    File imgFile = SPIFFS.open(path, FILE_WRITE);
    bool saved = false;
    if (imgFile) {
        size_t written = imgFile.write(jpg_buf, jpg_len);
        imgFile.close();
        saved = (written == jpg_len);
    }

    if (allocated) free(jpg_buf);
    esp_camera_fb_return(fb);

    if (!saved) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":\"storage write failed\"}");
        Serial.println("[CAPTURE] SPIFFS write failed");
        return ESP_FAIL;
    }

    // Record metadata  (R-DATA-01)
    long long ts_ms = (long long)(esp_timer_get_time() / 1000LL);

    ImageMeta m;
    strncpy(m.filename, filename, 31); m.filename[31] = '\0';
    strncpy(m.username, sess->username, 31); m.username[31] = '\0';
    m.timestamp_ms = ts_ms;

    // Insert at front (newest first)
    if (g_image_count < MAX_META_ENTRIES) {
        // Shift right
        for (int i = g_image_count; i > 0; i--) {
            g_images[i] = g_images[i-1];
        }
        g_images[0]  = m;
        g_image_count++;
    }
    meta_append(&m);

    Serial.printf("[CAPTURE] Saved %s for user %s (%u bytes)\n",
                  filename, sess->username, (unsigned)jpg_len);

    // Respond with JSON success  (R-NOTIF-01)
    char resp[256];
    snprintf(resp, sizeof(resp),
             "{\"status\":\"ok\",\"filename\":\"%s\","
             "\"username\":\"%s\",\"timestamp_ms\":%lld}",
             filename, sess->username, ts_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// =============================================================
//  GET /gallery  – JSON list for current user  (R-IMG-02, R-IMG-05)
// =============================================================
static esp_err_t gallery_handler(httpd_req_t *req) {
    char token[33] = {0};
    extract_cookie_token(req, token, sizeof(token));
    Session *sess = find_session(token);
    if (!sess) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "[]");
        return ESP_OK;
    }

    // Build JSON array of images belonging to this user, newest first
    // (R-USER-02: images are associated with username)
    // (R-USER-03: returning to same username shows their history)
    String json = "[";
    bool first = true;
    for (int i = 0; i < g_image_count; i++) {
        if (strcmp(g_images[i].username, sess->username) != 0) continue;
        if (!first) json += ",";
        first = false;
        char entry[200];
        snprintf(entry, sizeof(entry),
                 "{\"filename\":\"%s\",\"username\":\"%s\","
                 "\"timestamp_ms\":%lld}",
                 g_images[i].filename,
                 g_images[i].username,
                 g_images[i].timestamp_ms);
        json += entry;
    }
    json += "]";

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json.c_str());
    return ESP_OK;
}

// =============================================================
//  GET /image?f=<filename>  – Serve stored JPEG  (R-IMG-02) - Security Issue - Any logged in user can view any image, regardless if it is their own
// Solution: after checking token -> Check which user is logged in -> only return their results
// =============================================================
static esp_err_t image_handler(httpd_req_t *req) {
    char token[33] = {0};
    extract_cookie_token(req, token, sizeof(token));
    
    //if there is no session token - do not allow access
    if (!find_session(token)) { 
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "Not authenticated");
        return ESP_OK;
    }

    char fname[48] = {0};
    if (!get_query_param(req, "f", fname, sizeof(fname))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing f param");
        return ESP_FAIL;
    }

    // Sanitize: reject any path traversal attempts
    if (strstr(fname, "/") || strstr(fname, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    String path = String(IMAGE_DIR) + "/" + fname;
    if (!SPIFFS.exists(path)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Image not found");
        return ESP_FAIL;
    }

    File f = SPIFFS.open(path, FILE_READ);
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Open failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    uint8_t buf[1024];
    while (f.available()) {
        size_t n = f.read(buf, sizeof(buf));
        httpd_resp_send_chunk(req, (const char *)buf, n);
    }
    f.close();
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

// =============================================================
//  GET /delete?f=<filename>  – Delete one image  (R-IMG-03)
// =============================================================
static esp_err_t delete_handler(httpd_req_t *req) {
    char token[33] = {0};
    extract_cookie_token(req, token, sizeof(token));
    Session *sess = find_session(token);
    if (!sess) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"not authenticated\"}");
        return ESP_OK;
    }

    char fname[48] = {0};
    if (!get_query_param(req, "f", fname, sizeof(fname))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing f param");
        return ESP_FAIL;
    }

    // Only owner or admin may delete
    bool is_admin = (strcmp(sess->username, "admin") == 0);
    bool is_owner = false;
    for (int i = 0; i < g_image_count; i++) {
        if (strcmp(g_images[i].filename, fname) == 0) {
            is_owner = (strcmp(g_images[i].username, sess->username) == 0);
            break;
        }
    }
    if (!is_owner && !is_admin) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"permission denied\"}");
        return ESP_OK;
    }

    if (meta_delete(fname)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"deleted\"}");
    } else {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "{\"error\":\"not found\"}");
    }
    return ESP_OK;
}

// =============================================================
//  GET /status  – Camera connection status  (R-IOT-04)
// =============================================================
static esp_err_t status_handler(httpd_req_t *req) {
    sensor_t *s = esp_camera_sensor_get();
    bool cam_ok = (s != NULL);
    char resp[128];
    snprintf(resp, sizeof(resp),
             "{\"camera\":\"%s\",\"images_stored\":%d,\"images_limit\":%d}",
             cam_ok ? "connected" : "disconnected",
             g_image_count,
             MAX_STORED_IMAGES);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

// =============================================================
//  POST /cleardata  – Delete ALL images  (R-DATA-02)
//  Body: password=<ADMIN_CLEAR_PASS>
// =============================================================
static esp_err_t cleardata_handler(httpd_req_t *req) {
    char token[33] = {0};
    extract_cookie_token(req, token, sizeof(token));
    if (!find_session(token)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_sendstr(req, "{\"error\":\"not authenticated\"}");
        return ESP_OK;
    }

    char body[128] = {0};
    if (!read_post_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }

    char pass[64] = {0};
    parse_form_field(body, "password", pass, sizeof(pass));
    if (strcmp(pass, ADMIN_CLEAR_PASS) != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "{\"error\":\"wrong password\"}");
        return ESP_OK;
    }

    // Delete all image files
    for (int i = 0; i < g_image_count; i++) {
        String path = String(IMAGE_DIR) + "/" + g_images[i].filename;
        SPIFFS.remove(path);
    }
    g_image_count = 0;
    // Truncate the CSV
    File f = SPIFFS.open(METADATA_FILE, FILE_WRITE);
    if (f) f.close();

    Serial.println("[DATA] All images cleared");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"cleared\"}");
    return ESP_OK;
}

// =============================================================
//  startCameraServer()  – called from SmartRoomMonitor.ino
// =============================================================
void startCameraServer() {
    // Load persisted metadata
    meta_load();

    // ---- Main HTTP server (port 80) ----
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = 80;
    config.max_uri_handlers = 16;
    config.stack_size       = 8192;

    // URI registrations
    httpd_uri_t uri_index = {
        .uri = "/", .method = HTTP_GET,
        .handler = index_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_login = {
        .uri = "/login", .method = HTTP_POST,
        .handler = login_post_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_logout = {
        .uri = "/logout", .method = HTTP_GET,
        .handler = logout_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_capture = {
        .uri = "/capture", .method = HTTP_GET,
        .handler = capture_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_gallery = {
        .uri = "/gallery", .method = HTTP_GET,
        .handler = gallery_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_image = {
        .uri = "/image", .method = HTTP_GET,
        .handler = image_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_delete = {
        .uri = "/delete", .method = HTTP_GET,
        .handler = delete_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_status = {
        .uri = "/status", .method = HTTP_GET,
        .handler = status_handler, .user_ctx = NULL
    };
    httpd_uri_t uri_cleardata = {
        .uri = "/cleardata", .method = HTTP_POST,
        .handler = cleardata_handler, .user_ctx = NULL
    };

    Serial.printf("[HTTP] Starting main server on port %d\n", config.server_port);
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &uri_index);
        httpd_register_uri_handler(camera_httpd, &uri_login);
        httpd_register_uri_handler(camera_httpd, &uri_logout);
        httpd_register_uri_handler(camera_httpd, &uri_capture);
        httpd_register_uri_handler(camera_httpd, &uri_gallery);
        httpd_register_uri_handler(camera_httpd, &uri_image);
        httpd_register_uri_handler(camera_httpd, &uri_delete);
        httpd_register_uri_handler(camera_httpd, &uri_status);
        httpd_register_uri_handler(camera_httpd, &uri_cleardata);
        Serial.println("[HTTP] Main server started");
    } else {
        Serial.println("[HTTP] Main server FAILED to start");
    }

    // ---- Stream HTTP server (port 81) ----
    config.server_port = 81;
    config.ctrl_port   = 32769;
    config.stack_size  = 8192;

    httpd_uri_t uri_stream = {
        .uri = "/stream", .method = HTTP_GET,
        .handler = stream_handler, .user_ctx = NULL
    };

    Serial.printf("[HTTP] Starting stream server on port %d\n", config.server_port);
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &uri_stream);
        Serial.println("[HTTP] Stream server started");
    } else {
        Serial.println("[HTTP] Stream server FAILED to start");
    }
}
