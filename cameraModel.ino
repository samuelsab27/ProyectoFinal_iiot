/* Includes ---------------------------------------------------------------- */
#include <Arduino.h>
#include <ObjectDetector_In_Fridge_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>

// ================= CONFIGURACIÓN WIFI Y BACKEND =================
const char* ssid = "toe";
const char* password = "javito123";

// IP de la PC donde corre app.py (Puerto 5000, ruta /api/inventory)
const char* BACKEND_URL = "http://172.31.40.182:5000/api/inventory";

WebServer server(80);

// ================= PINES ESP32-S3 CAM =================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       10
#define Y4_GPIO_NUM       8
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       11
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13

/* Constant defines -------------------------------------------------------- */
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS           320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS           240
#define EI_CAMERA_FRAME_BYTE_SIZE                 3

/* Parámetros de filtro de confianza y votación de 5 fotogramas ------------ */
const float MIN_CONFIDENCE = 0.60f;      // Umbral mínimo de 60%
const int VOTE_TOTAL_FRAMES = 5;         // 5 fotogramas (~1 segundo)
const int VOTE_MAJORITY_THRESHOLD = 3;   // Al menos 3 de 5 para confirmar 1

/* Private variables ------------------------------------------------------- */
static bool debug_nn = false;
static bool is_initialised = false;
uint8_t *snapshot_buf = nullptr; // Buffer RGB888 reservado una sola vez en PSRAM

// Buffers y sincronización para el servidor web
uint8_t *latest_jpg_buf = nullptr;
size_t latest_jpg_len = 0;
const size_t MAX_JPG_SIZE = 65536; // 64KB máximo para un frame QVGA JPEG
SemaphoreHandle_t jpg_mutex;
String latest_boxes_json = "{\"w\":96,\"h\":96,\"slots\":[0,0,0],\"boxes\":[]}";

// Acumuladores para la votación de 5 fotogramas y control de envío HTTP POST
int vote_frame_count = 0;
int slot_votes[3] = {0, 0, 0};
int confirmed_slots[3] = {0, 0, 0};
int last_sent_slots[3] = {-1, -1, -1}; // -1 fuerza el primer envío al arrancar

static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,

    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,

    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA, // 320x240

    .jpeg_quality = 12,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

/* Function definitions ------------------------------------------------------- */
bool ei_camera_init(void);
void ei_camera_deinit(void);
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf);
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr);
bool post_matrix_to_backend(const int slots[3]);

// ================= SERVIDOR WEB CON BOUNDING BOXES Y 3 POSICIONES =================
void handleRoot() {
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset='utf-8'>
  <title>ESP32-S3 Edge Impulse Cam</title>
  <style>
    body { background: #121212; color: #eee; font-family: sans-serif; text-align: center; margin-top: 20px; }
    .container { position: relative; display: inline-block; border: 3px solid #333; border-radius: 8px; overflow: hidden; box-shadow: 0 4px 15px rgba(0,0,0,0.5); }
    canvas { display: block; background: #000; width: 640px; height: 480px; }
    .info { color: #aaa; font-size: 14px; margin-top: 10px; }
    .slots-banner { color: #00e676; font-size: 18px; font-weight: bold; margin-bottom: 10px; }
    .badge { display: inline-block; background: #1e1e1e; border: 1px solid #00e676; color: #00e676; padding: 4px 10px; border-radius: 4px; margin: 4px; font-size: 13px; }
  </style>
</head>
<body>
  <h2>Vista en Vivo + Detecciones + 3 Posiciones (Edge Impulse)</h2>
  <div id="slots-banner" class="slots-banner">Matriz Confirmada (5 frames): [0, 0, 0]</div>
  <div class="container">
    <canvas id="canvas" width="320" height="240"></canvas>
  </div>
  <div id="det-list" class="info">Cargando cámara...</div>
  <div class="info">Las líneas verticales dividen las 3 posiciones [Pos 1 | Pos 2 | Pos 3] dentro del área del modelo.</div>

  <script>
    const canvas = document.getElementById('canvas');
    const ctx = canvas.getContext('2d');
    const detList = document.getElementById('det-list');
    const slotsBanner = document.getElementById('slots-banner');
    const img = new Image();
    let boxes = [];
    let slots = [0, 0, 0];
    let votedSlots = [0, 0, 0];
    let modelW = 96;
    let modelH = 96;

    async function fetchBoxes() {
      try {
        const res = await fetch('/boxes?t=' + Date.now());
        const data = await res.json();
        boxes = data.boxes || [];
        slots = data.slots || [0, 0, 0];
        votedSlots = data.voted || slots;
        modelW = data.w || 96;
        modelH = data.h || 96;
      } catch (e) {}
    }

    img.onload = async () => {
      await fetchBoxes();
      
      // 1. Dibujar el frame de la cámara (320x240)
      ctx.drawImage(img, 0, 0, 320, 240);

      // 2. Calcular el recorte central que hace Edge Impulse
      const cropW = (modelW === modelH) ? 240 : 320;
      const cropH = 240;
      const offsetX = (320 - cropW) / 2;
      const offsetY = (240 - cropH) / 2;

      // Dibujar guía del área activa del modelo y las 3 divisiones
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.45)';
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 4]);
      ctx.strokeRect(offsetX, offsetY, cropW, cropH);

      const zoneW = cropW / 3;
      ctx.beginPath();
      ctx.moveTo(offsetX + zoneW, offsetY); ctx.lineTo(offsetX + zoneW, offsetY + cropH);
      ctx.moveTo(offsetX + zoneW * 2, offsetY); ctx.lineTo(offsetX + zoneW * 2, offsetY + cropH);
      ctx.stroke();
      ctx.setLineDash([]);

      slotsBanner.innerText = `Matriz Confirmada (5 frames): [${votedSlots.join(', ')}] | Frame actual: [${slots.join(', ')}]`;

      // 3. Dibujar cada Bounding Box escalado al área recortada
      const scaleX = cropW / modelW;
      const scaleY = cropH / modelH;

      if (boxes.length === 0) {
        detList.innerHTML = "Sin objetos detectados (>= 60%) en este frame";
      } else {
        detList.innerHTML = boxes.map(b => 
          `<span class="badge">${b.label}: ${(b.val * 100).toFixed(1)}% [x:${b.x}, y:${b.y}]</span>`
        ).join('');
      }

      boxes.forEach(b => {
        const x = offsetX + (b.x * scaleX);
        const y = offsetY + (b.y * scaleY);
        const w = b.w * scaleX;
        const h = b.h * scaleY;

        ctx.strokeStyle = '#00e676';
        ctx.lineWidth = 2;
        ctx.strokeRect(x, y, w, h);

        ctx.fillStyle = '#ff1744';
        ctx.beginPath();
        ctx.arc(x + w / 2, y + h / 2, 3, 0, 2 * Math.PI);
        ctx.fill();

        const label = `${b.label} ${(b.val * 100).toFixed(0)}%`;
        ctx.font = 'bold 11px sans-serif';
        const textW = ctx.measureText(label).width;
        ctx.fillStyle = 'rgba(0, 0, 0, 0.75)';
        ctx.fillRect(x, Math.max(0, y - 15), textW + 6, 15);

        ctx.fillStyle = '#00e676';
        ctx.fillText(label, x + 3, Math.max(11, y - 4));
      });

      setTimeout(() => { img.src = '/jpg?t=' + Date.now(); }, 80);
    };

    img.onerror = () => {
      setTimeout(() => { img.src = '/jpg?t=' + Date.now(); }, 500);
    };

    img.src = '/jpg?t=' + Date.now();
  </script>
</body>
</html>
)rawliteral";
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "text/html", html);
}

void handleJpg() {
    if (latest_jpg_buf != nullptr && latest_jpg_len > 0) {
        if (xSemaphoreTake(jpg_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
            server.send_P(200, "image/jpeg", (const char*)latest_jpg_buf, latest_jpg_len);
            xSemaphoreGive(jpg_mutex);
            return;
        }
    }
    server.send(503, "text/plain", "Camara ocupada");
}

void handleBoxes() {
    String json;
    if (xSemaphoreTake(jpg_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        json = latest_boxes_json;
        xSemaphoreGive(jpg_mutex);
    } else {
        json = "{\"w\":96,\"h\":96,\"slots\":[0,0,0],\"boxes\":[]}";
    }
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    server.send(200, "application/json", json);
}

// ================= ENVÍO HTTP POST AL BACKEND PYTHON (app.py) =================
bool post_matrix_to_backend(const int slots[3]) {
    if (WiFi.status() != WL_CONNECTED) {
        return false;
    }

    int count = slots[0] + slots[1] + slots[2];
    bool detected = (count > 0);

    // Incluye "matrix" y "slots" para compatibilidad total con app.py
    String payload = "{";
    payload += "\"w\":" + String(EI_CLASSIFIER_INPUT_WIDTH) + ",";
    payload += "\"h\":" + String(EI_CLASSIFIER_INPUT_HEIGHT) + ",";
    payload += "\"detected\":" + String(detected ? "true" : "false") + ",";
    payload += "\"count\":" + String(count) + ",";
    payload += "\"matrix\":[" + String(slots[0]) + "," + String(slots[1]) + "," + String(slots[2]) + "],";
    payload += "\"slots\":[" + String(slots[0]) + "," + String(slots[1]) + "," + String(slots[2]) + "]";
    payload += "}";

    HTTPClient http;
    http.begin(BACKEND_URL);
    http.setTimeout(2000);
    http.addHeader("Content-Type", "application/json");

    ei_printf("[HTTP POST] Enviando a %s -> %s\n", BACKEND_URL, payload.c_str());
    int code = http.POST(payload);
    if (code > 0) {
        ei_printf("[HTTP POST] Respuesta HTTP: %d\n", code);
    } else {
        ei_printf("[HTTP POST] Error enviando al backend (%d)\n", code);
    }
    http.end();

    return (code >= 200 && code < 300);
}

// ================= SETUP =================
void setup()
{
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n--- Edge Impulse Inferencing + Web Server + Backend POST (ESP32-S3) ---");

    // Crear mutex y reservar buffers en PSRAM una sola vez
    jpg_mutex = xSemaphoreCreateMutex();
    latest_jpg_buf = (uint8_t*)ps_malloc(MAX_JPG_SIZE);
    snapshot_buf = (uint8_t*)ps_malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);

    if (!latest_jpg_buf || !snapshot_buf) {
        Serial.println("ERR: No se pudo alojar memoria en PSRAM (Verifica 'OPI PSRAM' en Tools)");
    }

    // Inicializar cámara
    if (ei_camera_init() == false) {
        ei_printf("Failed to initialize Camera!\r\n");
    } else {
        ei_printf("Camera initialized\r\n");
    }

    // Conectar a Wi-Fi
    // Conectar a Wi-Fi de forma obligatoria
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
    Serial.print("Conectando a Wi-Fi");

    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        intentos++;
        if (intentos > 40) { // Si pasan 20 segundos sin conectar, reintenta explícitamente
            Serial.println("\n[Wi-Fi] Reintentando asociacion...");
            WiFi.disconnect();
            WiFi.begin(ssid, password);
            intentos = 0;
        }
    }

    Serial.println("\nWi-Fi conectado con exito!");
    Serial.print("Direccion IP del ESP32: http://");
    Serial.println(WiFi.localIP());

    server.on("/", handleRoot);
    server.on("/jpg", handleJpg);
    server.on("/boxes", handleBoxes);
    server.begin();
    

    ei_printf("\nStarting continuous inference in 2 seconds...\n");
    ei_sleep(2000);
}

// ================= LOOP =================
void loop()
{
    server.handleClient();

    if (ei_sleep(5) != EI_IMPULSE_OK) {
        return;
    }

    if (snapshot_buf == nullptr) {
        ei_printf("ERR: snapshot_buf es nulo!\n");
        return;
    }

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &ei_camera_get_data;

    if (ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf) == false) {
        ei_printf("Failed to capture image\r\n");
        return;
    }

    server.handleClient();

    // Ejecutar el clasificador
    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", err);
        return;
    }

    server.handleClient();

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
    // 1. Evaluar las 3 posiciones en este fotograma (solo confianza >= 0.60)
    int frame_slots[3] = {0, 0, 0};
    const int zone_width = EI_CLASSIFIER_INPUT_WIDTH / 3; // 96 / 3 = 32 px por zona

    String boxes_str = "[";
    bool first = true;

    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value < MIN_CONFIDENCE) {
            continue;
        }

        // Calcular en cuál de las 3 zonas cae el centro X del objeto
        //int center_x = bb.x + (bb.width / 2);
        //int slot_index = center_x / zone_width;
        int center_x = bb.x + (bb.width / 2);
        int slot_index = -1;

        //if (slot_index < 0) slot_index = 0;
        //if (slot_index > 2) slot_index = 2;
        if (center_x < 45) 
        {
            slot_index = 0;  // POS 1 (Frasco blanco: X ~ 30 a 44)
        } 
        else if (center_x < 60) 
        {
            slot_index = 1;  // POS 2 (Frasco oscuro grande: X ~ 45 a 59)
        }else{
            slot_index = 2;  // POS 3 (Frasco de gotas: X >= 60)
        }  
        frame_slots[slot_index] = 1;

        ei_printf("  %s (%.2f) [ x: %u, y: %u, w: %u, h: %u ] -> Pos %d\r\n",
                bb.label, bb.value, bb.x, bb.y, bb.width, bb.height, slot_index + 1);

        if (!first) boxes_str += ",";
        first = false;
        boxes_str += "{\"label\":\"" + String(bb.label) + "\"," +
                     "\"val\":" + String(bb.value, 2) + "," +
                     "\"x\":" + String(bb.x) + "," +
                     "\"y\":" + String(bb.y) + "," +
                     "\"w\":" + String(bb.width) + "," +
                     "\"h\":" + String(bb.height) + "}";
    }
    boxes_str += "]";

    // 2. Acumular votos para la ventana de 5 fotogramas
    slot_votes[0] += frame_slots[0];
    slot_votes[1] += frame_slots[1];
    slot_votes[2] += frame_slots[2];
    vote_frame_count++;

    if (vote_frame_count >= VOTE_TOTAL_FRAMES) {
        confirmed_slots[0] = (slot_votes[0] >= VOTE_MAJORITY_THRESHOLD) ? 1 : 0;
        confirmed_slots[1] = (slot_votes[1] >= VOTE_MAJORITY_THRESHOLD) ? 1 : 0;
        confirmed_slots[2] = (slot_votes[2] >= VOTE_MAJORITY_THRESHOLD) ? 1 : 0;

        ei_printf(">> VOTACION (5 frames): Votos[%d,%d,%d] -> Matriz Final: [%d, %d, %d]\r\n",
                  slot_votes[0], slot_votes[1], slot_votes[2],
                  confirmed_slots[0], confirmed_slots[1], confirmed_slots[2]);

        // Reiniciar contadores para los siguientes 5 fotogramas
        vote_frame_count = 0;
        slot_votes[0] = 0;
        slot_votes[1] = 0;
        slot_votes[2] = 0;

        // Enviar por HTTP POST al backend Python si cambió alguna de las 3 posiciones
        bool changed = (confirmed_slots[0] != last_sent_slots[0]) ||
                       (confirmed_slots[1] != last_sent_slots[1]) ||
                       (confirmed_slots[2] != last_sent_slots[2]);

        if (changed) {
            if (post_matrix_to_backend(confirmed_slots)) {
                last_sent_slots[0] = confirmed_slots[0];
                last_sent_slots[1] = confirmed_slots[1];
                last_sent_slots[2] = confirmed_slots[2];
            }
        }
    }

    // 3. Actualizar el JSON para el streaming web en vivo
    String json = "{\"w\":" + String(EI_CLASSIFIER_INPUT_WIDTH) +
                  ",\"h\":" + String(EI_CLASSIFIER_INPUT_HEIGHT) +
                  ",\"slots\":[" + String(frame_slots[0]) + "," + String(frame_slots[1]) + "," + String(frame_slots[2]) + "]" +
                  ",\"voted\":[" + String(confirmed_slots[0]) + "," + String(confirmed_slots[1]) + "," + String(confirmed_slots[2]) + "]" +
                  ",\"boxes\":" + boxes_str + "}";

    if (xSemaphoreTake(jpg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        latest_boxes_json = json;
        xSemaphoreGive(jpg_mutex);
    }
#endif
}

// ================= FUNCIONES DE CÁMARA (INTACTAS DE TU ARCHIVO FUNCIONAL) =================
bool ei_camera_init(void) {
    if (is_initialised) return true;

    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed with error 0x%x\n", err);
        return false;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
    }

    is_initialised = true;
    return true;
}

void ei_camera_deinit(void) {
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
        ei_printf("Camera deinit failed\n");
        return;
    }
    is_initialised = false;
}

bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    bool do_resize = false;

    if (!is_initialised) {
        ei_printf("ERR: Camera is not initialized\r\n");
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ei_printf("Camera capture failed\n");
        return false;
    }

    // Copiar el frame JPEG para la vista web antes de liberarlo
    if (latest_jpg_buf != nullptr && fb->len <= MAX_JPG_SIZE) {
        if (xSemaphoreTake(jpg_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            memcpy(latest_jpg_buf, fb->buf, fb->len);
            latest_jpg_len = fb->len;
            xSemaphoreGive(jpg_mutex);
        }
    }

    bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);
    esp_camera_fb_return(fb);

    if (!converted) {
        ei_printf("Conversion failed\n");
        return false;
    }

    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS)
        || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        do_resize = true;
    }

    if (do_resize) {
        ei::image::processing::crop_and_interpolate_rgb888(
            out_buf,
            EI_CAMERA_RAW_FRAME_BUFFER_COLS,
            EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
            out_buf,
            img_width,
            img_height);
    }

    return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];
        out_ptr_ix++;
        pixel_ix += 3;
        pixels_left--;
    }
    return 0;
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif
