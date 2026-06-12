/*
 * MULTI CLOCK - ESP32 CYD
 * Unifica BillboardClock + F1Clock
 *
 * - Toque corto:        mute/unmute audio
 * - Toque largo (3s):   modo subida WAV
 * - Pantalla subida:    botón táctil switch Billboard ↔ F1 (persiste en NVS)
 *
 * Modo Billboard: muestra la canción #1 del día del año
 *                 y la reproduce cada vez que cambia la hora
 * Modo F1:        igual que antes (próxima carrera + chime del auto)
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <PNGdec.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen_TT.h>
#include <SD.h>
#include <vector>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/dac.h"
#include "driver/gptimer.h"
#include "secrets.h"
#include <Update.h>
#include <Wire.h>
#include <RTClib.h>
#include "esp_sntp.h"

#include "Mute.h"
#include "Mute2.h"    // icono verde volumen bajo

#include <Fonts/GFXFF/FreeSansBold48pt7b.h>
#include <Fonts/GFXFF/Roboto_Bold60pt7b.h>
#include <Fonts/GFXFF/Exo2_SemiBold60pt7b.h>
#include "FreeSans18.h"
#include "FreeSansBold18.h"

// ══════════════════════════════════════════════
//  MODO ACTIVO
// ══════════════════════════════════════════════
bool modoF1 = false;   // false = Billboard, true = F1

// ══════════════════════════════════════════════
//  BACKLIGHT
// ══════════════════════════════════════════════
const int16_t lightSensorPin = 34;
const int16_t backlightPin   = 21;
#define BRILLO_MINIMO  1
#define BRILLO_MAXIMO  240

// ══════════════════════════════════════════════
//  WIFI / SERVIDOR
// ══════════════════════════════════════════════
WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

const char* AP_SSID = "RelojClock";
const char* AP_PASS = "12345678";

bool inAPMode   = false;
bool modoSubida = false;
String savedSSID     = "";
String savedPassword = "";

long tzOffsetSegundos = -10800;
const unsigned long FIXTURE_INTERVAL = 3UL * 60UL * 60UL * 1000UL;

// ══════════════════════════════════════════════
//  COLORES BILLBOARD
// ══════════════════════════════════════════════
// #define BB_FONDO      0x0000   // Negro
// #define BB_ACENTO     0x1EAC   // Verde Spotify elegante anterior0x04E0
// #define BB_TEXTO      0xFFFF   // Blanco
// #define BB_SUBTITULO  0x9CF3   // Gris claro suave
// #define BB_BARRA      0x3186   // Gris oscuro premium

// #define BB_TITULO     BB_TEXTO
// #define BB_HIGHLIGHT  BB_ACENTO

#define BB_FONDO         0x0000   // Negro

#define BB_ACENTO         0x1EAC   // Verde Spotify
#define BB_ACENTO_MEDIO   0x14C8
#define BB_ACENTO_BRILLO  0x07E0   // Verde brillante
#define BB_ACENTO_OSCURO  0x8669   // Verde grisáceo oscuro

#define BB_TEXTO         0xFFFF   // Blanco
#define BB_SUBTITULO     0x9CF3   // Gris claro suave
#define BB_BARRA         0x3186   // Gris oscuro premium

#define BB_TITULO        BB_TEXTO
#define BB_HIGHLIGHT     BB_ACENTO

// ══════════════════════════════════════════════
//  COLORES F1
// ══════════════════════════════════════════════
#define F1_ROJO         0xF800
#define F1_ROJO_OSCURO  0x8000
#define F1_NEGRO        0x0000
#define F1_BLANCO       0xFFFF
#define F1_GRIS         0x8410
#define F1_GRIS_OSCURO  0x2104
#define F1_GRIS_CLARO   0xC618

// ══════════════════════════════════════════════
//  COLORES COMUNES
// ══════════════════════════════════════════════
#define BLANCO      TFT_WHITE
#define NEGRO       TFT_BLACK
#define GRIS_OSCURO 0x2104
#define GRIS_CLARO  0xC618

// ══════════════════════════════════════════════
//  TFT
// ══════════════════════════════════════════════
TFT_eSPI tft = TFT_eSPI();

// ══════════════════════════════════════════════
//  OTA
// ══════════════════════════════════════════════
#define OTA_VERSION  "1.2.0"
#define OTA_OWNER    "Sig2018"
#define OTA_REPO     "MultiClockV2.0"
#define OTA_TOKEN    OTA_TOKEN_SECRET

// ══════════════════════════════════════════════
//  VARIABLES UI COMPARTIDAS
// ══════════════════════════════════════════════
bool mostrarAlternado = true;
unsigned long ultimoCambio = 0;
const int INTERVALO_CAMBIO = 5000;
unsigned long ultimaActualizacionFixture = 0;
String horaActual    = "--:--";
int    segundoActual = 0;

char msg[256];   // Buffer reutilizable para Syslog

static fs::File flagSaveFile;
static bool flagGuardando = false;

// ══════════════════════════════════════════════
//  STRUCT BILLBOARD
// ══════════════════════════════════════════════
struct CancionDia {
  String titulo;
  String artista;
  int    anio;          // año en que llegó al #1
  String archivoWav;   // nombre del WAV en la SD (solo nombre, sin ruta)
  bool   cargada;
};
CancionDia cancionHoy;

// ══════════════════════════════════════════════
//  STRUCTS F1
// ══════════════════════════════════════════════
struct SesionF1 {
  String nombre, nombreCorto;
  time_t timestamp;
  bool   valido;
};
struct GranPremio {
  String nombre, nombreCorto, circuito, pais, ciudad, countryCode;
  int    meetingKey;
  time_t timestampCarrera;
  std::vector<SesionF1> sesiones;
  bool   cargado;
};
GranPremio proximoGP;
GranPremio siguienteGP;

std::vector<SesionF1> sesionesCurrent;
std::vector<SesionF1> sesionesNext;
SesionF1* proximaSesion = nullptr;

// ── Estado de audio ──
enum EstadoAudio { AUDIO_NORMAL = 0, AUDIO_LOW = 1, AUDIO_MUTE = 2 };
EstadoAudio estadoAudio = AUDIO_MUTE;

// ── Prototipos ──
void actualizarF1();
void actualizarProximaSesion();
void avanzarAGPSiguiente();
void dibujarLayout();
void dibujarPartido();
void dibujarPartidoBillboard();
void dibujarPartidoF1();
void dibujarReloj(bool completo);
void dibujarSegundos(int seg);
void dibujarTextoInferior();
void dibujarTextoInferiorBillboard();
void dibujarTextoInferiorF1();
void dibujarLogoF1(int x, int y, int ancho, int alto);
void dibujarBandera(String countryCode, int x, int y, int ancho, int alto);
void dibujarIconoMute(int x, int y, EstadoAudio estado);
void reproducirChime();
bool detenerAudio(uint32_t timeoutMs);
void cargarCancionDelDia();
void iniciarServidorSubida();
void salirModoSubida();
void pantallaCargando(const char* msg);
void centrarTexto(String texto, int y, uint16_t color, uint16_t bg);
void guardarCacheF1();
void cargarCacheF1();
bool guardarSesionesJSON(const char* path, std::vector<SesionF1>& sesiones);
bool cargarSesionesJSON(const char* path, std::vector<SesionF1>& sesiones);
void ntpCallback(struct timeval *tv);
void sincronizarHora();
void cargarListaWAV();

// ══════════════════════════════════════════════
//  AUDIO
// ══════════════════════════════════════════════
#define SD_CS_PIN   5
#define BUFFER_SIZE 16384

const int SOUND_START_HOUR = 9;
const int SOUND_END_HOUR   = 23;

bool audioMuteado            = true;
EstadoAudio ultimoEstadoMute = AUDIO_NORMAL;
SPIClass sdSPI(HSPI);

volatile bool    enRampaInicio     = false;
volatile bool    enRampaFin        = false;
volatile uint8_t nivelRampa        = 0;
volatile uint8_t ultimoNivelDAC    = 0;
static uint8_t   audioBuffer[BUFFER_SIZE];
static volatile int  bufHead       = 0;
static volatile int  bufTail       = 0;
static volatile bool audioTerminado      = false;
volatile bool        audioReproduciendo  = false;

gptimer_handle_t audioTimer = NULL;
TaskHandle_t audioTaskHandle = NULL;
volatile bool    audioCancelar = false;
volatile bool    audioParando  = false;
const uint16_t   AUDIO_STOP_TIMEOUT_MS = 800;

// Fade out
volatile uint16_t volumenGlobal = 256;  // 256 = volumen completo, 0 = silencio

// Listas WAV — modo Billboard: archivos del día (nombrados por día)
// Modo F1: igual que antes
std::vector<String> listaWAVF1;
std::vector<String> listaWAVF1Low;

String archivoActual      = "";
int    ultimoIndiceNormal = -1;



// Para la envolvente de audio en los segundos
int alturas[80];
int ultimoMinuto = -1;
int ultimoSeg    = -1;  // ← agregar esta línea
bool envolventeGenerada = false;

// ══════════════════════════════════════════════
//  TOUCH
// ══════════════════════════════════════════════
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
unsigned long ultimoTouch = 0;
unsigned long inicioTouch = 0;
bool touchActivo = false;
File uploadFile;

// ══════════════════════════════════════════════
//  RTC
// ══════════════════════════════════════════════
RTC_DS3231 rtc;
bool rtcDisponible = false;

void ntpCallback(struct timeval *tv) {
  Serial.println("[NTP] CALLBACK: sincronizacion REAL");
  if (rtcDisponible) {
    DateTime nowUTC(time(nullptr));
    rtc.adjust(nowUTC);
    Serial.printf("[RTC] Sincronizado en UTC: %02d:%02d:%02d\n", 
                  nowUTC.hour(), nowUTC.minute(), nowUTC.second());
  }
}

// ══════════════════════════════════════════════
//  PNG Banderas F1
// ══════════════════════════════════════════════
static int16_t logoX, logoY;
PNG png;

void pngDrawBandera(PNGDRAW* pDraw) {
  uint16_t lineBuffer[200];
  if (pDraw->iWidth > 200) return;
  png.getLineAsRGB565(pDraw, lineBuffer, PNG_RGB565_BIG_ENDIAN, 0x000000);
  tft.pushImage(logoX, logoY + pDraw->y, pDraw->iWidth, 1, lineBuffer);
  if (flagGuardando && flagSaveFile) {
    if (pDraw->y == 0) {
      uint16_t anchoReal = pDraw->iWidth;
      flagSaveFile.write((uint8_t*)&anchoReal, 2);
    }
    flagSaveFile.write((uint8_t*)lineBuffer, pDraw->iWidth * 2);
  }
}

// ══════════════════════════════════════════════
//  WIFI
// ══════════════════════════════════════════════
bool conectarWiFi() {
  Serial.println("Probando redes de secrets.h...");
  for (int i = 0; i < WIFI_CANTIDAD; i++) {
    Serial.println("Intentando: " + String(WIFI_REDES[i][0]));
    WiFi.begin(WIFI_REDES[i][0], WIFI_REDES[i][1]);
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 18) {
      delay(500); intentos++; Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✓ Conectado a: " + String(WIFI_REDES[i][0]));
      Serial.print("   IP: "); Serial.println(WiFi.localIP());
      snprintf(msg, sizeof(msg), "WiFi conectado a %s | IP: %s", WIFI_REDES[i][0], WiFi.localIP().toString().c_str());
      Serial.println(msg);
      slog_info(msg);
      return true;
    }
    WiFi.disconnect(); delay(200);
  }

  Serial.println("\nFallo con secrets.h. Probando credenciales guardadas...");

  preferences.begin("wifi", true);
  String ss = preferences.getString("ssid", "");
  String pp = preferences.getString("pass", "");
  preferences.end();
  if (ss.length() > 0) {
    Serial.println("Intentando con SSID guardado: " + ss);
    WiFi.begin(ss.c_str(), pp.c_str());
    int intentos = 0;
    while (WiFi.status() != WL_CONNECTED && intentos < 18) {
      delay(500); intentos++; Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✓ Conectado usando credenciales guardadas");
      Serial.print("   IP: "); Serial.println(WiFi.localIP());
      snprintf(msg, sizeof(msg), "WiFi conectado con credenciales de NVM a %s | IP: %s", ss.c_str(), WiFi.localIP().toString().c_str());
      slog_info(msg);
      return true;
    }
  }
  Serial.println("\n✗ No se pudo conectar con ninguna credencial.");
  return false;
}

void startAPMode() {
  inAPMode = true;
  WiFi.disconnect(true); delay(200);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.println("[AP] Modo AP iniciado. IP: " + WiFi.softAPIP().toString());
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html",
      "<!DOCTYPE html><html><head><title>MultiClock</title>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'></head><body>"
      "<h1>Configurar WiFi</h1>"
      "<form action='/save' method='POST'>"
      "SSID: <input type='text' name='ssid'><br><br>"
      "Pass: <input type='password' name='pass'><br><br>"
      "<input type='submit' value='Guardar'></form></body></html>");
  });
  server.on("/save", HTTP_POST, []() {
    savedSSID = server.arg("ssid");
    savedPassword = server.arg("pass");
    preferences.begin("wifi", false);
    preferences.putString("ssid", savedSSID);
    preferences.putString("pass", savedPassword);
    preferences.end();
    Serial.println("[AP] WiFi guardado: " + savedSSID);
    server.send(200, "text/html", "<h2>Guardado. Reiniciando...</h2>");
    delay(2000); ESP.restart();
  });
  server.begin();
}

// ══════════════════════════════════════════════
//  HELPERS
// ══════════════════════════════════════════════
String limpiarAcentos(String texto) {
  String resultado = "";
  int i = 0;
  while (i < (int)texto.length()) {
    uint8_t c = texto[i];
    if (c == 0xC3 && i + 1 < (int)texto.length()) {
      uint8_t c2 = texto[i + 1];
      switch (c2) {
        case 0xA1: resultado += "a"; break; case 0x81: resultado += "A"; break;
        case 0xA9: resultado += "e"; break; case 0x89: resultado += "E"; break;
        case 0xAD: resultado += "i"; break; case 0x8D: resultado += "I"; break;
        case 0xB3: resultado += "o"; break; case 0x93: resultado += "O"; break;
        case 0xBA: resultado += "u"; break; case 0x9A: resultado += "U"; break;
        case 0xB1: resultado += "n"; break; case 0x91: resultado += "N"; break;
        default:   resultado += "?"; break;
      }
      i += 2;
    } else { resultado += (char)c; i++; }
  }
  return resultado;
}

bool leerHastaPatron(WiFiClientSecure& client, const String& patron, int timeoutMs = 8000) {
  String ventana = "";
  ventana.reserve(patron.length() * 2);
  unsigned long t0 = millis();
  while (millis() - t0 < (unsigned long)timeoutMs) {
    if (client.available()) {
      char c = client.read();
      ventana += c;
      if ((int)ventana.length() > (int)patron.length() * 2)
        ventana = ventana.substring(ventana.length() - patron.length() * 2);
      if (ventana.endsWith(patron)) return true;
    } else if (!client.connected()) break;
  }
  return false;
}

String leerNBytes(WiFiClientSecure& client, int n, int timeoutMs = 5000) {
  String resultado = "";
  resultado.reserve(n);
  unsigned long t0 = millis();
  while ((int)resultado.length() < n && millis() - t0 < (unsigned long)timeoutMs) {
    if (client.available()) resultado += (char)client.read();
    else if (!client.connected()) break;
  }
  return resultado;
}

void centrarTexto(String texto, int y, uint16_t color, uint16_t bg) {
  tft.setTextColor(color, bg);
  int ancho = tft.textWidth(texto);
  int x = max(0, (320 - ancho) / 2);
  tft.drawString(texto, x, y);
}

// ══════════════════════════════════════════════
//  BILLBOARD — CARGAR CANCIÓN DEL DÍA
// ══════════════════════════════════════════════
void cargarCancionDelDia() {
  Serial.println("\n=== cargarCancionDelDia ===");
  Serial.printf("Heap antes: %d\n", ESP.getFreeHeap());

  // Guardar valores anteriores para restaurar si falla
  String tituloAnterior   = cancionHoy.titulo;
  String artistaAnterior  = cancionHoy.artista;
  int    anioAnterior     = cancionHoy.anio;
  String wavAnterior      = cancionHoy.archivoWav;
  bool   cargadaAnterior  = cancionHoy.cargada;

  cancionHoy.cargada = false;

  struct tm ti;
  if (!getLocalTime(&ti)) {
    Serial.println("ERROR: No hay hora local");
    cancionHoy.cargada = cargadaAnterior;
    return;
  }
  int doy = ti.tm_yday + 1;
  Serial.printf("Buscando día: %d\n", doy);

  if (!LittleFS.exists("/billboard_esp32.json")) {
    Serial.println("ERROR: billboard_esp32.json no existe en LittleFS!");
    cancionHoy.cargada = cargadaAnterior;
    return;
  }

  File f = LittleFS.open("/billboard_esp32.json", "r");
  if (!f) {
    Serial.println("ERROR: No se pudo abrir billboard_esp32.json");
    cancionHoy.cargada = cargadaAnterior;
    return;
  }

  Serial.printf("Tamaño JSON: %d bytes\n", f.size());
  // Debug — ver primeros 50 bytes del archivo
  Serial.print("Primeros bytes: ");
  for (int i = 0; i < 50 && f.available(); i++) {
    char c = f.read();
    Serial.printf("[%d]", (int)c);
  }
  Serial.println();
  f.seek(0);  // volver al inicio
  Serial.printf("Heap antes de buscar: %d\n", ESP.getFreeHeap());

  // Buscar el registro del día leyendo línea por línea — sin cargar todo en RAM
  String target = "\"d\":" + String(doy) + ",";
  String lineaEncontrada = "";
  int lineasLeidas = 0;

  while (f.available()) {
    String line = f.readStringUntil('\n');
    lineasLeidas++;
    if (line.indexOf(target) >= 0) {
      lineaEncontrada = line;
      break;
    }
  }
  f.close();

  Serial.printf("Líneas leídas: %d\n", lineasLeidas);
  Serial.printf("Heap después de buscar: %d\n", ESP.getFreeHeap());

  if (lineaEncontrada == "") {
    Serial.printf("No se encontró canción para día %d\n", doy);
    cancionHoy.cargada = false;
    if (cargadaAnterior) {
      Serial.println("Restaurando canción anterior");
      cancionHoy.titulo     = tituloAnterior;
      cancionHoy.artista    = artistaAnterior;
      cancionHoy.anio       = anioAnterior;
      cancionHoy.archivoWav = wavAnterior;
      cancionHoy.cargada    = cargadaAnterior;
    }
    Serial.printf("Heap final: %d\n", ESP.getFreeHeap());
    Serial.println("=== Fin cargarCancionDelDia ===\n");
    return;
  }

  // Parsear solo esa línea — ~150 bytes, entra perfectamente en RAM
  Serial.printf("Heap antes de parsear línea: %d\n", ESP.getFreeHeap());
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, lineaEncontrada);
  Serial.printf("Heap después de parsear línea: %d\n", ESP.getFreeHeap());

  if (err) {
    Serial.printf("ERROR parseando línea: %s\n", err.c_str());
    cancionHoy.cargada = cargadaAnterior;
    if (cargadaAnterior) {
      cancionHoy.titulo     = tituloAnterior;
      cancionHoy.artista    = artistaAnterior;
      cancionHoy.anio       = anioAnterior;
      cancionHoy.archivoWav = wavAnterior;
    }
    Serial.printf("Heap final: %d\n", ESP.getFreeHeap());
    Serial.println("=== Fin cargarCancionDelDia ===\n");
    return;
  }

  cancionHoy.titulo     = limpiarAcentos(doc["t"].as<String>());
  cancionHoy.artista    = limpiarAcentos(doc["ar"].as<String>());
  cancionHoy.anio       = doc["an"].as<int>();
  cancionHoy.archivoWav = doc["w"].as<String>();
  cancionHoy.cargada    = true;

  Serial.printf("ENCONTRADA: %s - %s (WAV: %s)\n",
    cancionHoy.titulo.c_str(),
    cancionHoy.artista.c_str(),
    cancionHoy.archivoWav.c_str());

  Serial.printf("Heap final: %d\n", ESP.getFreeHeap());
  Serial.printf("cancionHoy.cargada = %s\n", cancionHoy.cargada ? "true" : "false");
  Serial.println("=== Fin cargarCancionDelDia ===\n");
}

// ══════════════════════════════════════════════
//  BILLBOARD — DIBUJAR
// ══════════════════════════════════════════════
void dibujarPartidoBillboard() {
  Serial.println("\n=== dibujarPartidoBillboard ===");
  Serial.printf("cancionHoy.cargada = %s\n", cancionHoy.cargada ? "true" : "false");
  Serial.printf("Heap libre: %d\n", ESP.getFreeHeap());

  tft.fillRect(0, 5, 320, 90, BB_FONDO);
  //tft.fillRect(5, 5, 310, 3, BB_ACENTO); //Linea debajo de la principal
  dibujarIconoMute(3, 50, estadoAudio);
  ultimoEstadoMute = estadoAudio;

  if (!cancionHoy.cargada) {
    Serial.println("!!! ERROR: cancionHoy NO CARGADA !!!");
    Serial.println("Mostrando mensaje de error en pantalla");
    tft.setFreeFont(&FreeSans9pt7b);
    centrarTexto("Sin datos de Billboard", 40, GRIS_CLARO, BB_FONDO);
    tft.setTextFont(1);
    return;
  }

  Serial.printf("Dibujando: %s - %s\n", cancionHoy.titulo.c_str(), cancionHoy.artista.c_str());

  tft.setTextDatum(TC_DATUM);

  // Etiqueta #1 DEL DIA
  tft.loadFont(FreeSans18);
  //tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(BB_ACENTO, BB_FONDO);
  tft.drawString("CANCION N°1 DEL DIA", 160, 8);
  tft.unloadFont();

  // Título con fuente adaptativa
  String titulo = cancionHoy.titulo;
  tft.setTextColor(BB_TITULO, BB_FONDO);

  tft.setFreeFont(&FreeSansBold12pt7b);
  if (tft.textWidth(titulo) <= 315) {
    // Entra con fuente grande
    tft.drawString(titulo, 160, 28);
    Serial.println("Título: fuente grande");
  } else {
    tft.setFreeFont(&FreeSansBold9pt7b);
    if (tft.textWidth(titulo) <= 315) {
      // Entra con fuente mediana
      tft.drawString(titulo, 160, 30);
      Serial.println("Título: fuente mediana");
    } else {
      // Truncar con puntos suspensivos
      while (tft.textWidth(titulo + "...") > 315 && titulo.length() > 3)
        titulo = titulo.substring(0, titulo.length() - 1);
      titulo += "...";
      tft.drawString(titulo, 160, 30);
      Serial.println("Título: truncado → " + titulo);
    }
  }

  // Artista
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(BB_SUBTITULO, BB_FONDO);
  String artista = cancionHoy.artista;
  while (tft.textWidth(artista) > 280 && artista.length() > 3)
    artista = artista.substring(0, artista.length() - 1);
  if (artista != cancionHoy.artista) artista += "...";
  tft.drawString(artista, 160, 53);

  // Año
  tft.loadFont(FreeSansBold18);
  //tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(BB_HIGHLIGHT, BB_FONDO);
  if (cancionHoy.anio > 0)
    tft.drawString("Llegó al N°1 en " + String(cancionHoy.anio), 160, 73);

  tft.unloadFont();
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  Serial.println("=== Fin dibujarPartidoBillboard ===\n");
}

void dibujarTextoInferiorBillboard() {
  tft.fillRect(0, 68, 320, 22, BB_FONDO);
}

// ══════════════════════════════════════════════
//  F1 — FETCH
// ══════════════════════════════════════════════
String abreviarSesion(String nombre) {
  nombre.toLowerCase();
  if (nombre.indexOf("practice 1") >= 0 || nombre == "fp1") return "P1";
  if (nombre.indexOf("practice 2") >= 0 || nombre == "fp2") return "P2";
  if (nombre.indexOf("practice 3") >= 0 || nombre == "fp3") return "P3";
  if (nombre.indexOf("sprint qualifying") >= 0)              return "SQ";
  if (nombre.indexOf("sprint") >= 0)                        return "SPRINT";
  if (nombre.indexOf("qualifying") >= 0)                    return "QUALI";
  if (nombre.indexOf("race") >= 0)                          return "CARRERA";
  return nombre.substring(0, min((int)nombre.length(), 6));
}

String paisACodigoBandera(String pais) {
  pais.toLowerCase();
  if (pais == "australia")      return "au";
  if (pais == "china")          return "cn";
  if (pais == "japan")          return "jp";
  if (pais == "bahrain")        return "bh";
  if (pais == "saudi arabia")   return "sa";
  if (pais == "united states" || pais == "miami" || pais == "las vegas") return "us";
  if (pais == "emilia romagna" || pais == "italy" || pais == "monza") return "it";
  if (pais == "monaco")         return "mc";
  if (pais == "spain")          return "es";
  if (pais == "canada")         return "ca";
  if (pais == "austria")        return "at";
  if (pais == "great britain" || pais == "united kingdom") return "gb";
  if (pais == "hungary")        return "hu";
  if (pais == "belgium")        return "be";
  if (pais == "netherlands")    return "nl";
  if (pais == "singapore")      return "sg";
  if (pais == "azerbaijan")     return "az";
  if (pais == "mexico")         return "mx";
  if (pais == "brazil")         return "br";
  if (pais == "qatar")          return "qa";
  if (pais == "abu dhabi" || pais == "united arab emirates") return "ae";
  return "un";
}

void actualizarProximaSesion() {
  static unsigned long ultimoprint = 0;
  time_t ahora; time(&ahora);
  proximaSesion = nullptr;
  for (int i = 0; i < (int)sesionesCurrent.size(); i++) {
    if (sesionesCurrent[i].timestamp > ahora - 3600) {
      proximaSesion = &sesionesCurrent[i]; break;
    }
  }
  if (millis() - ultimoprint > 1800000) {
      ultimoprint = millis();
      if (proximaSesion)
          Serial.println("[F1] Proxima sesion: " + proximaSesion->nombreCorto);
      else
        Serial.println("[F1] No hay proxima sesion pendiente");
  }
}

void avanzarAGPSiguiente() {
  if (!siguienteGP.cargado) return;
  Serial.println("[F1] Avanzando automaticamente al siguiente GP");
  slog_info("F1: Avanzando al siguiente GP");
  proximoGP = siguienteGP;
  sesionesCurrent = sesionesNext;
  actualizarProximaSesion();
}

void actualizarF1() {
  if (WiFi.status() != WL_CONNECTED) conectarWiFi();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[F1] Sin WiFi, abortando fetch");
    slog_warning("F1: Sin WiFi, abortando actualización");
    return;
  }

  time_t ahora; time(&ahora);
  ahora -= 7500;
  struct tm* t = gmtime(&ahora);
  char fechaHoy[25];
  sprintf(fechaHoy, "%04d-%02d-%02dT00:00:00", t->tm_year+1900, t->tm_mon+1, t->tm_mday);

  Serial.println("[F1] Buscando próxima carrera...");
  Serial.printf("[F1] RAM libre antes de fetch: %d bytes\n", ESP.getFreeHeap());
  slog_info("Iniciando actualización de carreras F1");

  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure(); client.setTimeout(10);

  int meetingKey = 0, meetingKeyNext = 0;
  String gpNombre, gpPais, gpCiudad, gpCircuito, dateStr;
  String gpNombreNext, gpPaisNext, gpCiudadNext, gpCircuitoNext, dateStrNext;
  bool haySiguienteGP = false;

  // ========== 1. OBTENER LISTA DE CARRERAS ==========
  {
    String url = "https://api.openf1.org/v1/sessions?session_name=Race&date_start>=" + String(fechaHoy);
    Serial.println("[F1] URL: " + url);
    http.begin(client, url); http.setTimeout(10000);
    int code = http.GET();
    if (code != 200) {
      Serial.println("[F1] Error HTTP race: " + String(code));
      slog_error(("[F1] Error HTTP race: " + String(code)).c_str());
      http.end(); return;
    }
    String body = http.getString(); http.end();
    Serial.println("[F1] Race response length: " + String(body.length()));

    DynamicJsonDocument doc(3072);
    if (deserializeJson(doc, body) || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
      Serial.println("[F1] Error parseando race JSON");
      slog_error("F1: Error parseando race JSON");
      return;
    }
    body = String();

    JsonObject raceObj = doc[0];
    Serial.printf("[F1] Carreras encontradas: %d\n", doc.size());
    haySiguienteGP = (doc.size() > 1);
    JsonObject nextRaceObj;
    if (haySiguienteGP) nextRaceObj = doc[1];

    meetingKey = raceObj["meeting_key"] | 0;
    gpNombre   = raceObj["meeting_name"]       | "Gran Premio";
    gpPais     = raceObj["country_name"]       | "Unknown";
    gpCiudad   = raceObj["location"]           | "";
    gpCircuito = raceObj["circuit_short_name"] | "";
    dateStr    = raceObj["date_start"]         | "";

    if (haySiguienteGP) {
      meetingKeyNext = nextRaceObj["meeting_key"] | 0;
      gpNombreNext   = nextRaceObj["meeting_name"]       | "Gran Premio";
      gpPaisNext     = nextRaceObj["country_name"]       | "Unknown";
      gpCiudadNext   = nextRaceObj["location"]           | "";
      gpCircuitoNext = nextRaceObj["circuit_short_name"] | "";
      dateStrNext    = nextRaceObj["date_start"]         | "";
    }
  }

  gpNombre   = limpiarAcentos(gpNombre);
  gpPais     = limpiarAcentos(gpPais);
  gpCiudad   = limpiarAcentos(gpCiudad);
  gpCircuito = limpiarAcentos(gpCircuito);
  gpNombreNext   = limpiarAcentos(gpNombreNext);
  gpPaisNext     = limpiarAcentos(gpPaisNext);
  gpCiudadNext   = limpiarAcentos(gpCiudadNext);
  gpCircuitoNext = limpiarAcentos(gpCircuitoNext);

  auto parseTS = [](String ds, long offset) -> time_t {
    if (ds.length() < 19) return 0;
    struct tm tmEv = {};
    tmEv.tm_year = ds.substring(0, 4).toInt() - 1900;
    tmEv.tm_mon  = ds.substring(5, 7).toInt() - 1;
    tmEv.tm_mday = ds.substring(8, 10).toInt();
    tmEv.tm_hour = ds.substring(11, 13).toInt() + (offset / 3600);
    tmEv.tm_min  = ds.substring(14, 16).toInt();
    return mktime(&tmEv);
  };

  time_t tsCarrera     = parseTS(dateStr, tzOffsetSegundos);
  time_t tsCarreraNext = parseTS(dateStrNext, tzOffsetSegundos);

  Serial.printf("[F1] GP: %s | Pais: %s | Meeting: %d\n", gpNombre.c_str(), gpPais.c_str(), meetingKey);
  if (haySiguienteGP) {
    Serial.printf("[F1] NEXT GP: %s | Pais: %s | Meeting: %d\n", gpNombreNext.c_str(), gpPaisNext.c_str(), meetingKeyNext);
  }
  Serial.printf("[F1] RAM libre tras doc1 liberado: %d bytes\n", ESP.getFreeHeap());

  proximoGP = { gpNombre, gpPais, gpCircuito, gpPais, gpCiudad, paisACodigoBandera(gpPais), meetingKey, tsCarrera, {}, true };
  if (haySiguienteGP)
    siguienteGP = { gpNombreNext, gpPaisNext, gpCircuitoNext, gpPaisNext, gpCiudadNext, paisACodigoBandera(gpPaisNext), meetingKeyNext, tsCarreraNext, {}, true };

  // ========== 2. SESIONES DEL GP ACTUAL ==========
  Serial.println("[F1] Obteniendo sesiones del fin de semana...");
  {
    String url2 = "https://api.openf1.org/v1/sessions?meeting_key=" + String(meetingKey);
    Serial.println("[F1] URL2: " + url2);
    http.begin(client, url2); http.setTimeout(10000);
    int code = http.GET();
    if (code == 200) {
      String body2 = http.getString(); http.end();
      Serial.println("[F1] Sessions response length: " + String(body2.length()));
      DynamicJsonDocument doc2(6144);
      if (!deserializeJson(doc2, body2) && doc2.is<JsonArray>()) {
        for (JsonObject s : doc2.as<JsonArray>()) {
          SesionF1 ses;
          ses.nombre      = limpiarAcentos(String(s["session_name"] | "Unknown"));
          ses.nombreCorto = abreviarSesion(ses.nombre);
          ses.valido      = false;
          String sd       = s["date_start"] | "";
          if (sd.length() >= 19) {
            ses.timestamp = parseTS(sd, tzOffsetSegundos);
            ses.valido    = true;
          }
          proximoGP.sesiones.push_back(ses);
          Serial.printf("[F1] Sesion: %s → %s\n", ses.nombre.c_str(), ses.nombreCorto.c_str());
        }
      }
    } else {
      Serial.println("[F1] Error HTTP sessions: " + String(code));
      http.end();
    }
  }

  // ========== 3. SESIONES DEL SIGUIENTE GP (NUEVO - ESTO ES LO QUE FALTABA) ==========
  if (haySiguienteGP && meetingKeyNext != 0) {
    Serial.printf("[F1] Descargando sesiones del siguiente GP (meeting: %d)\n", meetingKeyNext);
    String url3 = "https://api.openf1.org/v1/sessions?meeting_key=" + String(meetingKeyNext);
    Serial.println("[F1] URL3: " + url3);
    http.begin(client, url3);
    http.setTimeout(10000);
    int code = http.GET();
    
    if (code == 200) {
      String body3 = http.getString();
      http.end();
      Serial.println("[F1] Next sessions response length: " + String(body3.length()));
      
      DynamicJsonDocument doc3(6144);
      if (!deserializeJson(doc3, body3) && doc3.is<JsonArray>()) {
        sesionesNext.clear();
        for (JsonObject s : doc3.as<JsonArray>()) {
          SesionF1 ses;
          ses.nombre      = limpiarAcentos(String(s["session_name"] | "Unknown"));
          ses.nombreCorto = abreviarSesion(ses.nombre);
          ses.valido      = false;
          String sd       = s["date_start"] | "";
          if (sd.length() >= 19) {
            struct tm tmS = {};
            tmS.tm_year = sd.substring(0, 4).toInt() - 1900;
            tmS.tm_mon  = sd.substring(5, 7).toInt() - 1;
            tmS.tm_mday = sd.substring(8, 10).toInt();
            tmS.tm_hour = sd.substring(11, 13).toInt() + (tzOffsetSegundos / 3600);
            tmS.tm_min  = sd.substring(14, 16).toInt();
            ses.timestamp = mktime(&tmS);
            ses.valido    = true;
          }
          sesionesNext.push_back(ses);
          Serial.printf("[F1] NEXT Sesion: %s → %s\n", ses.nombre.c_str(), ses.nombreCorto.c_str());
        }
        // Actualizar siguienteGP con las sesiones
        siguienteGP.sesiones = sesionesNext;
      }
    } else {
      Serial.printf("[F1] Error HTTP siguiente GP: %d\n", code);
      http.end();
    }
  }

  // ========== 4. ORDENAR Y GUARDAR ==========
  std::sort(proximoGP.sesiones.begin(), proximoGP.sesiones.end(),
    [](const SesionF1& a, const SesionF1& b) { return a.timestamp < b.timestamp; });

  sesionesCurrent = proximoGP.sesiones;
  guardarSesionesJSON("/f1sessions_current.json", sesionesCurrent);
  guardarSesionesJSON("/f1sessions_next.json", sesionesNext);
  
  actualizarProximaSesion();
  guardarCacheF1();

  Serial.printf("[F1] OK: %s | %d sesiones\n", gpNombre.c_str(), proximoGP.sesiones.size());
  Serial.printf("[F1] Siguiente GP: %s | %d sesiones\n", 
                siguienteGP.nombre.c_str(), (int)sesionesNext.size());
  Serial.printf("[F1] RAM libre tras fetch completo: %d bytes\n", ESP.getFreeHeap());
  snprintf(msg, sizeof(msg), "F1 GP: %s sesiones=%d", gpCircuito.c_str(), proximoGP.sesiones.size());
  slog_info(msg);
}

// ══════════════════════════════════════════════
//  BANDERA / LOGO F1
// ══════════════════════════════════════════════
void dibujarLogoF1(int x, int y, int ancho, int alto) {
  if (LittleFS.exists("/f1logo.bin")) {
    fs::File f = LittleFS.open("/f1logo.bin", "r");
    if (f) {
      uint16_t lineBuffer[100];
      for (int row = 0; row < alto; row++) {
        f.read((uint8_t*)lineBuffer, ancho * 2);
        tft.pushImage(x, y + row, ancho, 1, lineBuffer, 1);
      }
      f.close();
      return;
    }
  }
  Serial.println("[F1] f1logo.bin no encontrado, usando fallback texto");
  tft.setTextColor(F1_ROJO, F1_NEGRO);
  tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("F1", x + 5, y + 8);
  tft.setTextFont(1);
}

void dibujarBandera(String countryCode, int x, int y, int ancho, int alto) {
  String path = "/flag_" + countryCode + ".bin";
  if (LittleFS.exists(path)) {
    fs::File f = LittleFS.open(path, "r");
    if (f) {
      uint16_t anchoReal;
      f.read((uint8_t*)&anchoReal, 2);
      uint16_t lineBuffer[200];
      for (int row = 0; row < alto; row++) {
        f.read((uint8_t*)lineBuffer, anchoReal * 2);
        tft.pushImage(x, y + row, anchoReal, 1, lineBuffer, 1);
      }
      f.close();
      Serial.println("[F1] Bandera desde LittleFS: " + countryCode);
      return;
    }
  }

  Serial.println("[F1] Descargando bandera: " + countryCode);
  String url = "https://wsrv.nl/?url=flagcdn.com/56x42/" + countryCode + ".png&output=png&w=" + String(ancho) + "&h=" + String(alto);
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(10);
  HTTPClient http; http.begin(client, url); http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) {
    Serial.println("[F1] Bandera HTTP error: " + String(code) + " para " + countryCode);
    http.end();
    tft.fillRect(x, y, ancho, alto, F1_ROJO);
    tft.setTextFont(2); tft.setTextColor(F1_BLANCO, F1_ROJO);
    tft.setTextDatum(TL_DATUM); tft.drawString(countryCode, x+2, y+8);
    return;
  }
  int size = http.getSize(); if (size <= 0 || size > 20000) { http.end(); return; }
  uint8_t* buf = (uint8_t*)malloc(size); if (!buf) { http.end(); return; }
  WiFiClient* stream = http.getStreamPtr();
  int leido = 0; unsigned long t0 = millis();
  while (leido < size && millis()-t0 < 5000) { if (stream->available()) buf[leido++] = stream->read(); }
  http.end();

  logoX = x; logoY = y;
  size_t espacioLibre = LittleFS.totalBytes() - LittleFS.usedBytes();
  size_t tamArchivo = ancho * alto * 2;
  if (espacioLibre > tamArchivo + 10240) {
    flagSaveFile = LittleFS.open(path, "w");
    flagGuardando = flagSaveFile ? true : false;
  } else {
    flagGuardando = false;
    Serial.println("[F1] LittleFS sin espacio para bandera: " + countryCode);
  }

  int rc = png.openRAM(buf, leido, pngDrawBandera);
  if (rc == PNG_SUCCESS) {
    png.decode(NULL, 0); png.close();
    if (flagGuardando && flagSaveFile) {
      flagSaveFile.close();
      Serial.println("[F1] Bandera guardada en LittleFS: " + path);
    }
  } else {
    if (flagGuardando && flagSaveFile) {
      flagSaveFile.close();
      LittleFS.remove(path);
    }
    Serial.println("[F1] Bandera PNG error: " + String(rc));
  }
  flagGuardando = false;
  free(buf);
}

// ══════════════════════════════════════════════
//  COUNTDOWN F1
// ══════════════════════════════════════════════
String calcularCountdownF1() {
  if (!proximoGP.cargado) return "";
  time_t ahora; time(&ahora);
  time_t tsRef = (proximaSesion != nullptr) ? proximaSesion->timestamp : proximoGP.timestampCarrera;
  long diff = (long)difftime(tsRef, ahora);
  if (diff <= -7200) {
    if (siguienteGP.cargado) { 
      avanzarAGPSiguiente(); 
      if (proximaSesion) diff = (long)difftime(proximaSesion->timestamp, ahora); 
    } else return "FINALIZADO";
  }
  if (diff <= 0) return "EN VIVO";
  long totalMin = (diff + 59) / 60;
  long dias = totalMin / 1440, horas = (totalMin % 1440) / 60, minutos = totalMin % 60;
  long segs = diff % 60;
  char buf[32];
  if (dias > 1)      sprintf(buf, "FALTAN %ld dias %02ld hs", dias, horas);
  else if (dias == 1) sprintf(buf, "FALTA 1 dia %02ld hs", horas);
  else if (horas > 1) sprintf(buf, "FALTAN %ld hs %02ld min", horas, minutos);
  else if (horas == 1) sprintf(buf, "FALTA 1 hora %02ld min", minutos);
  else if (minutos > 10) sprintf(buf, "FALTAN %ld minutos", minutos);
  else if (minutos == 1) sprintf(buf, "FALTA 1 min %02lds", segs);
  else sprintf(buf, "FALTAN %ld min %02lds", minutos, segs);
  return String(buf);
}

// ══════════════════════════════════════════════
//  CACHE F1
// ══════════════════════════════════════════════
void guardarCacheF1() {
  preferences.begin("f1cache", false);
  uint64_t tsGuardado = preferences.getULong64("ts", 0);
  if (tsGuardado == (uint64_t)proximoGP.timestampCarrera) {
    preferences.end();
    Serial.println("[CACHE] F1 sin cambios, no se guarda");
    return;
  }
  preferences.putString("nombre",   proximoGP.nombre);
  preferences.putString("circuito", proximoGP.circuito);
  preferences.putString("pais",     proximoGP.pais);
  preferences.putString("ciudad",   proximoGP.ciudad);
  preferences.putString("flag",     proximoGP.countryCode);
  preferences.putULong64("ts", (uint64_t)proximoGP.timestampCarrera);
  preferences.end();
  Serial.println("[CACHE] F1 guardado: " + proximoGP.nombre);
}

void cargarCacheF1() {
  preferences.begin("f1cache", true);
  proximoGP.nombre           = preferences.getString("nombre",   "");
  proximoGP.circuito         = preferences.getString("circuito", "");
  proximoGP.pais             = preferences.getString("pais",     "");
  proximoGP.ciudad           = preferences.getString("ciudad",   "");
  proximoGP.countryCode      = preferences.getString("flag",     "");
  proximoGP.timestampCarrera = preferences.getULong64("ts", 0);
  preferences.end();

  proximoGP.sesiones.clear();
  proximoGP.cargado = (proximoGP.nombre != "");

  if (proximoGP.cargado)
    Serial.println("[CACHE] F1 GP desde preferences: " + proximoGP.nombre);
  else
    Serial.println("[CACHE] F1 sin datos en preferences");

  std::vector<SesionF1> sesionesTemp;
  if (cargarSesionesJSON("/f1sessions_current.json", sesionesTemp) && sesionesTemp.size() > 0) {
    sesionesCurrent = sesionesTemp;
    Serial.println("[CACHE] Sesiones current desde JSON: " + String(sesionesCurrent.size()));
    for (auto& s : sesionesCurrent) Serial.println("  " + s.nombreCorto);
  } else {
    sesionesCurrent.clear();
    Serial.println("[CACHE] Sin sesiones current en JSON");
  }

  sesionesTemp.clear();
  if (cargarSesionesJSON("/f1sessions_next.json", sesionesTemp) && sesionesTemp.size() > 0) {
    sesionesNext = sesionesTemp;
    Serial.println("[CACHE] Sesiones next desde JSON: " + String(sesionesNext.size()));
    for (auto& s : sesionesNext) Serial.println("  " + s.nombreCorto);
  } else {
    sesionesNext.clear();
    Serial.println("[CACHE] Sin sesiones next en JSON");
  }

  if (proximoGP.cargado) actualizarProximaSesion();
}

bool guardarSesionesJSON(const char* path, std::vector<SesionF1>& sesiones) {
  DynamicJsonDocument doc(4096);
  JsonArray arr = doc.createNestedArray("sessions");
  for (auto& s : sesiones) {
    JsonObject o = arr.createNestedObject();
    o["name"]  = s.nombre;
    o["short"] = s.nombreCorto;
    o["ts"]    = (uint64_t)s.timestamp;
  }
  File f = LittleFS.open(path, "w");
  if (!f) {
    Serial.println("[F1] Error abriendo JSON para escribir");
    return false;
  }
  serializeJson(doc, f);
  f.close();
  Serial.println(String("[F1] JSON guardado: ") + path);
  return true;
}

bool cargarSesionesJSON(const char* path, std::vector<SesionF1>& sesiones) {
  sesiones.clear();
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.println(String("[F1] No existe JSON: ") + path);
    return false;
  }
  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("[F1] Error parseando JSON");
    return false;
  }
  for (JsonObject o : doc["sessions"].as<JsonArray>()) {
    SesionF1 s;
    s.nombre      = o["name"].as<String>();
    s.nombreCorto = o["short"].as<String>();
    s.timestamp   = o["ts"].as<uint64_t>();
    s.valido      = true;
    sesiones.push_back(s);
  }
  Serial.printf("[F1] Sesiones cargadas JSON: %d\n", sesiones.size());
  return true;
}

// ══════════════════════════════════════════════
//  DIBUJAR TEXTO INFERIOR F1
// ══════════════════════════════════════════════
void dibujarTextoInferiorF1() {
  tft.fillRect(0, 68, 320, 22, F1_NEGRO);
  if (!proximoGP.cargado) return;
  actualizarProximaSesion();
  time_t ahora; time(&ahora);
  time_t tsRef = (proximaSesion != nullptr) ? proximaSesion->timestamp : proximoGP.timestampCarrera;
  long diff = (long)difftime(tsRef, ahora);
  String texto;
  if (mostrarAlternado) {
    if (proximaSesion != nullptr) {
      const char* dias[]  = {"Dom","Lun","Mar","Mie","Jue","Vie","Sab"};
      const char* meses[] = {"Ene","Feb","Mar","Abr","May","Jun","Jul","Ago","Sep","Oct","Nov","Dic"};
      struct tm* tmS = localtime(&proximaSesion->timestamp);
      char fb[20]; char hb[6];
      sprintf(fb, "%s %02d %s", dias[tmS->tm_wday], tmS->tm_mday, meses[tmS->tm_mon]);
      sprintf(hb, "%02d:%02d", tmS->tm_hour, tmS->tm_min);
      texto = proximaSesion->nombreCorto + " — " + String(fb) + " " + String(hb) + "hs";
    } else {
      texto = proximoGP.ciudad != "" ? proximoGP.ciudad + ", " + proximoGP.pais : proximoGP.pais;
    }
  } else {
    texto = calcularCountdownF1();
  }
  uint16_t colorTexto = F1_BLANCO;
  if (diff >= 0 && diff < 3600) colorTexto = F1_ROJO;
  if (diff < 0 && diff > -7200) colorTexto = F1_ROJO;
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(colorTexto, F1_NEGRO);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(texto, 160, 72);
  tft.setTextDatum(TL_DATUM);
}

void dibujarTextoInferior() {
  if (modoF1) dibujarTextoInferiorF1();
}

// ══════════════════════════════════════════════
//  DIBUJAR PARTIDO / MODO ACTIVO
// ══════════════════════════════════════════════
void dibujarPartidoF1() {
  tft.fillRect(0, 5, 320, 90, F1_NEGRO);
  dibujarLogoF1(2, 5, 80, 35);
  tft.fillRect(85, 5, 175, 85, F1_NEGRO);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(F1_ROJO, F1_NEGRO);
  tft.setTextDatum(TC_DATUM);
  String titulo = proximoGP.nombre;
  if ((int)titulo.length() > 22) titulo = titulo.substring(0, 22);
  tft.drawString(titulo, 160, 9);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(F1_GRIS_CLARO, F1_NEGRO);
  String circ = proximoGP.circuito;
  if ((int)circ.length() > 24) circ = circ.substring(0, 24);
  tft.drawString(circ, 160, 28);
  struct tm* tmC = localtime(&proximoGP.timestampCarrera);
  const char* dias[]  = {"Dom","Lun","Mar","Mie","Jue","Vie","Sab"};
  const char* meses[] = {"Ene","Feb","Mar","Abr","May","Jun","Jul","Ago","Sep","Oct","Nov","Dic"};
  char fechaBuf[20]; char horaBuf[6];
  sprintf(fechaBuf, "%s %02d %s", dias[tmC->tm_wday], tmC->tm_mday, meses[tmC->tm_mon]);
  sprintf(horaBuf, "%02d:%02d", tmC->tm_hour, tmC->tm_min);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(F1_BLANCO, F1_NEGRO);
  tft.drawString(String(fechaBuf) + "  " + String(horaBuf) + " hs", 160, 50);
  dibujarIconoMute(3, 50, estadoAudio);
  ultimoEstadoMute = estadoAudio;
  tft.setFreeFont(&FreeSans9pt7b);
  dibujarTextoInferiorF1();
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  dibujarBandera(proximoGP.countryCode, 265, 5, 50, 35);
}

void dibujarPartido() {
  if (modoF1) dibujarPartidoF1();
  else        dibujarPartidoBillboard();
}

// ══════════════════════════════════════════════
//  LAYOUT
// ══════════════════════════════════════════════
void dibujarLayout() {
  Serial.println("[Layout] Dibujando layout modo: " + String(modoF1 ? "F1" : "Billboard"));
  tft.fillScreen(NEGRO);
  if (modoF1) {
    tft.fillRect(0,  0, 320,  5, F1_ROJO);
    tft.fillRect(0, 90, 320,  5, F1_ROJO);
    tft.fillRect(0, 95, 320,  5, F1_ROJO);
  } else {
    tft.fillRect(0,  0, 320,  3, BB_ACENTO);
    //tft.fillRect(0, 90, 320,  3, BB_ACENTO);//Segunda linea que quedo pegada
    tft.fillRect(0, 98, 320,  2, BB_ACENTO);
  }
  tft.fillRect(0, 100, 320, 140, GRIS_OSCURO);
  dibujarReloj(true);
  dibujarPartido();
}

// ══════════════════════════════════════════════
//  RELOJ
// ══════════════════════════════════════════════
void actualizarReloj() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  char hora[6];
  sprintf(hora, "%02d:%02d", ti.tm_hour, ti.tm_min);
  int seg = ti.tm_sec;
  bool cambioHora = (String(hora) != horaActual);
  bool cambioSeg  = (seg != segundoActual);
  if (!cambioHora && !cambioSeg) return;
  if (cambioHora) {
    char msgmin[64];
    snprintf(msgmin, sizeof(msgmin), "Reloj: Minuto cambiado a %d", ti.tm_min);
    slog_info(msgmin);
    Serial.printf("[Reloj] Minuto cambiado a: %d\n", ti.tm_min);

    // Resetear barra de segundos en cada cambio de minuto
    if (ti.tm_min != ultimoMinuto) {
      ultimoMinuto = ti.tm_min;
      generarEnvolventeAudio();
      tft.fillRect(0, 234 - 5, 320, 11, GRIS_OSCURO);
      ultimoSeg = -1;
    }

    if (ti.tm_min == 0) {
      Serial.println("\n========== MINUTO CERO ==========");
      Serial.printf("Hora: %02d:%02d:%02d\n", ti.tm_hour, ti.tm_min, ti.tm_sec);
      Serial.printf("Día del año: %d\n", ti.tm_yday + 1);
      Serial.printf("cancionHoy.cargada = %s\n", cancionHoy.cargada ? "true" : "false");
      Serial.printf("cancionHoy.titulo = %s\n", cancionHoy.titulo.c_str());
      Serial.printf("cancionHoy.artista = %s\n", cancionHoy.artista.c_str());
      Serial.printf("cancionHoy.archivoWav = %s\n", cancionHoy.archivoWav.c_str());
      Serial.printf("Heap libre: %d\n", ESP.getFreeHeap());

      reproducirChime();

      static int ultimoDia = -1;
      int doy = ti.tm_yday + 1;

      if (doy != ultimoDia) {
        Serial.println(">>> CAMBIO DE DÍA DETECTADO <<<");
        ultimoDia = doy;
        Serial.println("Antes de cargarCancionDelDia()");
        cargarCancionDelDia();
        Serial.printf("Después de cargarCancionDelDia: cargada=%s\n", cancionHoy.cargada ? "true" : "false");
        if (!modoF1) {
          Serial.println("Llamando a dibujarPartidoBillboard() por cambio de día");
          dibujarPartidoBillboard();
          Serial.println("Dibujo completado");
        }
      } else {
        Serial.println(">>> MISMO DÍA, NO RECARGAR <<<");
        if (!cancionHoy.cargada) {
          Serial.println("!!! ADVERTENCIA: cancionHoy perdida sin cambio de día !!!");
          Serial.println("Forzando recarga...");
          cargarCancionDelDia();
          if (!modoF1) dibujarPartidoBillboard();
        }
      }
      Serial.println("================================\n");
    }
  }
  horaActual    = String(hora);
  segundoActual = seg;
  if (cambioHora) dibujarReloj(false);
  else            dibujarSegundos(seg);
}

void dibujarReloj(bool completo) {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  char hora[6];
  sprintf(hora, "%02d:%02d", ti.tm_hour, ti.tm_min);

  TFT_eSprite sprReloj = TFT_eSprite(&tft);
  sprReloj.createSprite(320, 118);
  sprReloj.fillSprite(GRIS_OSCURO);
  sprReloj.loadFont("Exo2-SemiBold60", LittleFS);
  sprReloj.setTextColor(BLANCO);
  sprReloj.setTextDatum(TC_DATUM);
  sprReloj.drawString(hora, 160, 10);
  sprReloj.unloadFont();
  sprReloj.pushSprite(0, 100);
  sprReloj.deleteSprite();

  const char* dias[]  = {"Domingo","Lunes","Martes","Miercoles","Jueves","Viernes","Sabado"};
  const char* meses[] = {"Enero","Febrero","Marzo","Abril","Mayo","Junio","Julio","Agosto",
                          "Septiembre","Octubre","Noviembre","Diciembre"};
  char fechaBuf[30];
  sprintf(fechaBuf, "%s %d de %s", dias[ti.tm_wday], ti.tm_mday, meses[ti.tm_mon]);
  tft.fillRect(0, 208, 320, 20, GRIS_OSCURO);
  tft.setFreeFont(&FreeSans9pt7b);
  tft.setTextColor(GRIS_CLARO, GRIS_OSCURO);
  tft.setTextDatum(TC_DATUM);
  tft.drawString(fechaBuf, 160, 208);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(1);
  dibujarSegundos(ti.tm_sec);
}

// Función principal de dibujo de segundos (REEMPLAZA la que tienes)
void dibujarSegundos(int seg) {
  if (!envolventeGenerada) generarEnvolventeAudio();
  if (seg == ultimoSeg) return;

  const int anchoTotal = 320;
  const int anchoBarra = 3;
  const int gap        = 1;
  const int paso       = anchoBarra + gap;  // 4px
  const int numBarras  = anchoTotal / paso; // 80 barras

  if (modoF1) {
    //Bandera a cuadros
    const int alto = 10;
    const int y = 230;
    const int tam = 5;
    int currPx = map(seg, 0, 59, 0, anchoTotal);
    if (seg == 0) {
      tft.fillRect(0, y-1, anchoTotal, alto+1, F1_GRIS_OSCURO);
      ultimoSeg = seg; return;
    }
    tft.drawFastHLine(0, y-1, currPx, TFT_LIGHTGREY);
    for (int x = 0; x < currPx; x += tam) {
      bool blancoArriba = ((x / tam) % 2 == 0);
      tft.fillRect(x, y,     tam, tam, blancoArriba ? TFT_WHITE : TFT_BLACK);
      tft.fillRect(x, y+tam, tam, tam, blancoArriba ? TFT_BLACK : TFT_WHITE);
    }

  } else {
    // Billboard — Barras simétricas respecto a línea central
    const int yCentro = 234;
    const int altoMax = 6;

    if (seg == 0) {
      tft.fillRect(0, yCentro - altoMax, anchoTotal, altoMax * 2 + 1, GRIS_OSCURO);
      ultimoSeg = seg;
      return;
    }

    int pixelActual = map(seg, 0, 59, 0, anchoTotal);
    int barraActual = pixelActual / paso;

    for (int i = max(0, barraActual - 3); i <= barraActual; i++) {
      int x = i * paso;
      if (x >= anchoTotal) break;

      int anchoReal = min(anchoBarra, anchoTotal - x);

      // Borrar área de esta barra
      tft.fillRect(x, yCentro - altoMax, anchoReal, altoMax * 2 + 1, GRIS_OSCURO);

      // Altura proporcional
      int altura     = map(alturas[i % numBarras], 0, 14, 0, altoMax);
      uint16_t color = colorPorAltura(alturas[i % numBarras]);

      // Dibujar simétrico arriba y abajo solo si hay altura
      if (altura > 0) {
        tft.fillRect(x, yCentro - altura, anchoReal, altura, color);
        tft.fillRect(x, yCentro + 1,      anchoReal, altura, color);
      }
      // Si altura == 0 no se dibuja nada, solo queda el eje central
    }

    // Línea central dorada hasta donde llegamos
    tft.drawFastHLine(0, yCentro, pixelActual, BB_ACENTO);
  }

  ultimoSeg = seg;
}

// ══════════════════════════════════════════════
//  ICONO MUTE
// ══════════════════════════════════════════════
void dibujarIconoMute(int x, int y, EstadoAudio estado) {
  Serial.printf("[Mute] dibujarIconoMute estado=%d (0=NORMAL 1=LOW 2=MUTE)\n", (int)estado);
  uint16_t fondo = modoF1 ? F1_NEGRO : BB_FONDO;
  tft.fillRect(x, y, 20, 20, fondo);
  switch (estado) {
    case AUDIO_NORMAL: break;
    case AUDIO_LOW:
      tft.pushImage(x, y, 20, 20, no_sound_30dp_EA3323_FILL1_wght700_GRAD0_opsz24VERDE);
      break;
    case AUDIO_MUTE:
      tft.pushImage(x, y, 20, 20, no_sound_30dp_EA3323_FILL1_wght700_GRAD0_opsz24);
      break;
  }
}

// ══════════════════════════════════════════════
//  PANTALLAS DE ESTADO
// ══════════════════════════════════════════════
void pantallaCargando(const char* msg) {
  tft.fillScreen(NEGRO);
  if (modoF1) {
    tft.fillRect(0, 0, 320, 5, F1_ROJO); tft.fillRect(0, 235, 320, 5, F1_ROJO);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(F1_ROJO, NEGRO); tft.setTextSize(3);
    tft.drawString("F1", 160, 60);
    tft.setTextSize(2);
    tft.drawString("CLOCK V1.3", 160, 95);
  } else {
    tft.fillRect(0, 0, 320, 5, BB_ACENTO); tft.fillRect(0, 235, 320, 5, BB_ACENTO);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(BB_ACENTO, NEGRO); tft.setTextSize(3);
    tft.drawString("Music", 160, 55);
    tft.setTextSize(2);
    tft.drawString("CLOCK V1.3", 160, 95);
  }
  tft.setTextColor(GRIS_CLARO, NEGRO); tft.setTextSize(2);
  int x = (320 - (int)strlen(msg)*12) / 2; if (x < 5) x = 5;
  tft.setCursor(x, 150); tft.print(msg); tft.setTextSize(1);
}

void mostrarPantallaModoAP() {
  tft.fillScreen(NEGRO);
  tft.fillRect(0, 0, 320, 6, BB_ACENTO); tft.fillRect(0, 234, 320, 6, BB_ACENTO);
  tft.setTextColor(BB_ACENTO, NEGRO); tft.setFreeFont(&FreeSansBold12pt7b);
  tft.setTextDatum(TC_DATUM); tft.drawString("MODO CONFIGURACION", 160, 25);
  tft.setTextColor(BLANCO, NEGRO); tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("Conectate al WiFi:", 160, 70);
  tft.setTextColor(BB_ACENTO, NEGRO); tft.setFreeFont(&FreeSansBold12pt7b);
  tft.drawString(AP_SSID, 160, 95);
  tft.setTextColor(GRIS_CLARO, NEGRO); tft.setFreeFont(&FreeSans9pt7b);
  tft.drawString("Contrasena: " + String(AP_PASS), 160, 130);
  tft.drawString("Luego: 192.168.4.1", 160, 160);
  tft.setTextDatum(TL_DATUM);
}

void mostrarPantallaModoSubida() {
  uint16_t acento = modoF1 ? F1_ROJO : BB_ACENTO;
  tft.fillScreen(NEGRO);
  tft.fillRect(0, 0, 320, 5, acento); tft.fillRect(0, 235, 320, 5, acento);
  tft.setTextDatum(TC_DATUM);
  tft.setFreeFont(&FreeSansBold12pt7b); tft.setTextColor(acento, NEGRO);
  tft.drawString("MODO SUBIDA WAV", 160, 20);
  tft.setFreeFont(&FreeSans9pt7b); tft.setTextColor(BLANCO, NEGRO);
  tft.drawString("Conectate a la misma WiFi", 160, 60);
  tft.drawString("y abre el navegador en:", 160, 85);
  tft.setFreeFont(&FreeSansBold12pt7b); tft.setTextColor(GRIS_CLARO, NEGRO);
  tft.drawString(WiFi.localIP().toString(), 160, 115);
  tft.setFreeFont(&FreeSans9pt7b); tft.setTextColor(GRIS_CLARO, NEGRO);
  tft.drawString("Toque corto en pantalla para salir", 160, 147);
  tft.fillRoundRect(60, 190, 200, 35, 8, modoF1 ? BB_ACENTO_OSCURO : F1_ROJO);
  tft.drawRoundRect(60, 190, 200, 35, 8, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&FreeSansBold9pt7b);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(modoF1 ? "Cambiar a Music" : "Cambiar a F1", 160, 208);
  tft.setTextFont(1); tft.setTextDatum(TL_DATUM);
}

// ══════════════════════════════════════════════
//  SERVIDOR WEB
// ══════════════════════════════════════════════
void iniciarServidorSubida() {
  detenerAudio(AUDIO_STOP_TIMEOUT_MS);
  modoSubida = true;
  mostrarPantallaModoSubida();
  Serial.println("[Web] Servidor iniciado en " + WiFi.localIP().toString());
  slog_info(("[Web] Servidor iniciado en " + WiFi.localIP().toString()).c_str());

  server.on("/", HTTP_GET, []() {
    String modoActual = modoF1 ? "F1" : "Billboard";
    String modoOtro   = modoF1 ? "Billboard" : "F1";
    String bgColor    = modoF1 ? "#111" : "#0a0a0a";
    String btnColor   = modoF1 ? "#E10600" : "#FFE000";
    String html = "<!DOCTYPE html><html><head><title>MultiClock</title>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<style>body{font-family:Arial;background:" + bgColor + ";color:white;text-align:center;padding:20px;}"
      "h1{color:" + btnColor + ";} .btn{background:" + btnColor + ";color:black;"
      "padding:10px 20px;border:none;font-size:16px;cursor:pointer;margin:5px;border-radius:4px;}"
      ".archivos{margin-top:20px;text-align:left;display:inline-block;}</style></head>"
      "<body><h1>MULTI CLOCK</h1>"
      "<p>Modo actual: <b>" + modoActual + "</b></p>"
      "<form action='/modo' method='POST'>"
      "<button class='btn' type='submit'>Cambiar a " + modoOtro + "</button></form>"
      "<br><h2>Subir WAV</h2>"
      "<form method='POST' action='/upload' enctype='multipart/form-data'>"
      "<input type='file' name='wav' accept='.wav'><br><br>"
      "<button class='btn' type='submit'>Subir a SD</button></form>"
      "<div class='archivos'><h3>Archivos en SD:</h3>%LISTA%</div></body></html>";
    String lista = "";
    File root = SD.open("/"); File f = root.openNextFile();
    while (f) {
      String nombre = f.name();
      if (nombre.endsWith(".wav") || nombre.endsWith(".WAV"))
        lista += "<p>" + nombre + " (" + String(f.size()) + "b) "
              + "<a href='/delete?file=" + nombre + "' style='background:red;color:white;padding:2px 8px;'>Borrar</a></p>";
      f = root.openNextFile();
    }
    if (lista == "") lista = "<p>No hay archivos WAV</p>";
    html.replace("%LISTA%", lista);
    server.send(200, "text/html", html);
  });

  server.on("/modo", HTTP_POST, []() {
    modoF1 = !modoF1;
    preferences.begin("config", false);
    preferences.putBool("modoF1", modoF1);
    preferences.end();
    Serial.println("[Modo] Cambiado a: " + String(modoF1 ? "F1" : "Billboard"));
    slog_info(("[Modo] Cambiado a: " + String(modoF1 ? "F1" : "Billboard")).c_str());
    server.send(200, "text/html",
      "<html><body style='background:#111;color:white;text-align:center;font-family:Arial'>"
      "<h2 style='color:#E10600'>Modo cambiado! Reiniciando...</h2></body></html>");
    delay(2000); ESP.restart();
  });

  server.on("/upload", HTTP_POST,
    []() { server.send(200, "text/html",
      "<html><body style='background:#111;color:white;text-align:center'>"
      "<h2>Archivo subido!</h2><p><a href='/' style='color:#E10600'>Volver</a></p></body></html>"); },
    []() {
      HTTPUpload& upload = server.upload();
      if (upload.status == UPLOAD_FILE_START) {
        String fn = "/" + upload.filename;
        if (!fn.endsWith(".wav") && !fn.endsWith(".WAV")) return;
        uploadFile = SD.open(fn, FILE_WRITE);
        Serial.println("[Web] Subiendo archivo: " + fn);
      } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
      } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
          uploadFile.close();
          Serial.println("[Web] Archivo guardado: " + String(upload.totalSize) + " bytes");
          cargarListaWAV();
        }
      }
    }
  );

  server.on("/delete", HTTP_GET, []() {
    String fn = server.arg("file");
    if (!fn.startsWith("/")) fn = "/" + fn;
    Serial.println("[Web] Borrando: " + fn);
    if (SD.exists(fn)) {
      SD.remove(fn);
      Serial.println("[Web] Borrado OK");
      server.send(200, "text/html",
        "<html><body style='background:#111;color:white;text-align:center'>"
        "<h2>Borrado!</h2><p><a href='/'>Volver</a></p></body></html>");
    } else server.send(404, "text/plain", "No encontrado");
  });

  server.begin();
}

void salirModoSubida() {
  Serial.println("[Web] Saliendo modo subida, reiniciando...");
  slog_info("Web: Saliendo modo subida");
  server.stop(); delay(500); ESP.restart();
}

// ══════════════════════════════════════════════
//  OTA
// ══════════════════════════════════════════════
void verificarOTA() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("[OTA] Verificando version en GitHub...");
  WiFiClientSecure clientCheck; clientCheck.setInsecure();
  HTTPClient http;
  String urlCheck = "https://api.github.com/repos/" + String(OTA_OWNER) + "/" + String(OTA_REPO) + "/releases/latest";
  http.begin(clientCheck, urlCheck);
  http.addHeader("Authorization", String("Bearer ") + OTA_TOKEN);
  http.addHeader("User-Agent", "ESP32");
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[OTA] Error HTTP: %d\n", code);
    http.end();
    return;
  }
  String body = http.getString(); http.end();
  int tagIdx = body.indexOf("\"tag_name\":\""); if (tagIdx == -1) return;
  tagIdx += 12; int tagFin = body.indexOf('"', tagIdx);
  String latestTag = body.substring(tagIdx, tagFin);
  String cleanTag = latestTag;
  if (cleanTag.startsWith("v") || cleanTag.startsWith("V")) cleanTag = cleanTag.substring(1);
  cleanTag.trim();
  Serial.printf("[OTA] Version actual: %s | Ultima: %s\n", OTA_VERSION, cleanTag.c_str());

  char msgOTA[80];
  if (cleanTag == String(OTA_VERSION)) {
    Serial.println("[OTA] Firmware actualizado.");
    snprintf(msgOTA, sizeof(msgOTA), "OTA: version actual %s es la mas reciente", OTA_VERSION);
    slog_info(msgOTA);
    return;
  }

  snprintf(msgOTA, sizeof(msgOTA), "OTA: nueva version disponible %s", latestTag.c_str());
  slog_info(msgOTA);

  tft.fillScreen(NEGRO);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(BB_ACENTO, NEGRO);
  tft.drawString("NUEVA VERSION", 160, 15, 4);
  tft.drawString(latestTag, 160, 62, 4);
  tft.setTextColor(GRIS_CLARO, NEGRO);
  tft.drawString("Toca para actualizar", 160, 95, 4);
  tft.drawString("o espera 10 segundos", 160, 130, 4);

  bool actualizar = false;
  for (int i = 10; i > 0; i--) {
    tft.fillRect(0, 165, 320, 30, NEGRO);
    tft.setTextColor(TFT_RED, NEGRO);
    char countdown[16];
    snprintf(countdown, sizeof(countdown), "Saltando en %d...", i);
    tft.drawString(countdown, 160, 180, 4);
    unsigned long t0 = millis();
    while (millis() - t0 < 1000) {
      if (touchscreen.touched()) { actualizar = true; break; }
      delay(50);
    }
    if (actualizar) break;
  }
  if (!actualizar) {
    Serial.println("[OTA] Actualización omitida por timeout");
    slog_info("OTA: actualizacion omitida por timeout");
    return;
  }

  int nameIdx = body.indexOf(".ino.bin\""); if (nameIdx == -1) nameIdx = body.indexOf(".bin\"");
  if (nameIdx == -1) return;
  int urlIdx = body.lastIndexOf("\"url\":", nameIdx); if (urlIdx == -1) return;
  urlIdx += 7; int urlFin = body.indexOf('"', urlIdx);
  String assetApiUrl = body.substring(urlIdx, urlFin); body = "";

  Serial.println("[OTA] Descargando nueva version...");
  tft.fillScreen(NEGRO);
  tft.setTextColor(BB_ACENTO, NEGRO);
  tft.setTextDatum(TC_DATUM);
  tft.drawString("ACTUALIZANDO", 160, 60, 4);
  tft.drawString(latestTag, 160, 100, 2);

  WiFiClientSecure clientOTA; clientOTA.setInsecure();
  HTTPClient httpOTA; httpOTA.begin(clientOTA, assetApiUrl);
  httpOTA.addHeader("Authorization", String("Bearer ") + OTA_TOKEN);
  httpOTA.addHeader("User-Agent", "ESP32");
  httpOTA.addHeader("Accept", "application/octet-stream");
  httpOTA.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int codeOTA = httpOTA.GET(); if (codeOTA != 200) { httpOTA.end(); return; }
  int contentLength = httpOTA.getSize(); if (contentLength <= 0) { httpOTA.end(); return; }
  Serial.printf("[OTA] Tamaño firmware: %d bytes\n", contentLength);
  if (!Update.begin(contentLength)) { httpOTA.end(); return; }
  WiFiClient* stream = httpOTA.getStreamPtr(); Update.writeStream(*stream);
  if (Update.end() && Update.isFinished()) {
    Serial.println("[OTA] Actualización exitosa! Reiniciando...");
    snprintf(msgOTA, sizeof(msgOTA), "OTA: actualizacion exitosa a version %s", latestTag.c_str());
    slog_info(msgOTA);
    tft.drawString("REINICIANDO...", 160, 140, 2);
    delay(3000);
    ESP.restart();
  } else {
    Serial.println("[OTA] Error en la actualizacion");
    slog_warning("OTA: error en la actualizacion");
  }
  httpOTA.end();
}

// ══════════════════════════════════════════════
//  AUDIO
// ══════════════════════════════════════════════
void audioTask(void* param) {
  File wav = SD.open(archivoActual.c_str());
  if (!wav) { audioReproduciendo = false; audioParando = false; audioTaskHandle = NULL; vTaskDelete(NULL); return; }

  char chunkId[5]; chunkId[4] = 0;
  uint32_t chunkSize = 0, dataSize = 0;
  uint16_t numChannels = 0, bitsPerSample = 0;
  uint32_t sampleRate = 0;

  if (wav.read((uint8_t*)chunkId, 4) != 4 || strncmp(chunkId, "RIFF", 4) != 0) {
    wav.close(); audioReproduciendo = false; audioParando = false; audioTaskHandle = NULL; vTaskDelete(NULL); return;
  }
  wav.seek(8);
  if (wav.read((uint8_t*)chunkId, 4) != 4 || strncmp(chunkId, "WAVE", 4) != 0) {
    wav.close(); audioReproduciendo = false; audioParando = false; audioTaskHandle = NULL; vTaskDelete(NULL); return;
  }

  while (wav.available()) {
    if (wav.read((uint8_t*)chunkId, 4) != 4) break;
    if (wav.read((uint8_t*)&chunkSize, 4) != 4) break;
    if (strncmp(chunkId, "fmt ", 4) == 0) {
      uint16_t af;
      wav.read((uint8_t*)&af, 2);
      wav.read((uint8_t*)&numChannels, 2);
      wav.read((uint8_t*)&sampleRate, 4);
      wav.seek(wav.position() + 6);
      wav.read((uint8_t*)&bitsPerSample, 2);
      if (chunkSize > 16) wav.seek(wav.position() + (chunkSize - 16));
    } else if (strncmp(chunkId, "data", 4) == 0) {
      dataSize = chunkSize;
      break;
    } else {
      wav.seek(wav.position() + chunkSize);
    }
    if (chunkSize & 1) wav.seek(wav.position() + 1);
  }

  if (dataSize == 0) { wav.close(); audioReproduciendo = false; audioParando = false; audioTaskHandle = NULL; vTaskDelete(NULL); return; }

  uint32_t bytesPorSegundo = sampleRate * numChannels * (bitsPerSample / 8);
  uint32_t bytesParaFade   = bytesPorSegundo * 4;  // Tiempo de Fadeout , fade en los últimos 4 segundos

  Serial.printf("[WAV] SampleRate: %lu | Canales: %d | Bits: %d | Total: %lu bytes\n",
                sampleRate, numChannels, bitsPerSample, dataSize);
  Serial.printf("[WAV] Fade arranca cuando quedan: %lu bytes\n", bytesParaFade);

  gptimer_alarm_config_t alarm_config = {
    .alarm_count  = 1000000 / sampleRate,
    .reload_count = 0,
    .flags        = {.auto_reload_on_alarm = true}
  };
  gptimer_set_alarm_action(audioTimer, &alarm_config);

  dac_output_voltage(DAC_CHAN_1, 0);
  ultimoNivelDAC = 0;
  nivelRampa     = 0;
  enRampaInicio  = true;
  enRampaFin     = false;
  audioTerminado = false;
  gptimer_start(audioTimer);

  uint32_t bytesRestantes = dataSize;
  uint8_t  chunk[256];

  while (bytesRestantes > 0 && !audioCancelar) {
    int porLeer = min((uint32_t)sizeof(chunk), bytesRestantes);
    int leidos  = wav.read(chunk, porLeer);
    if (leidos <= 0) break;
    bytesRestantes -= leidos;

    // Fade — solo en modo Billboard, bajar volumenGlobal proporcionalmente
    if (!modoF1 && bytesRestantes < bytesParaFade && bytesParaFade > 0) {
      volumenGlobal = (uint16_t)((bytesRestantes * 256) / bytesParaFade);
    }

    for (int i = 0; i < leidos; i++) {
      if (audioCancelar) break;
      int nextTail = (bufTail + 1) % BUFFER_SIZE;
      int espera   = 0;
      while (nextTail == bufHead && !audioCancelar) {
        vTaskDelay(1);
        if (++espera > 2000) break;
      }
      if (audioCancelar) break;
      audioBuffer[bufTail] = chunk[i];
      bufTail = nextTail;
    }
  }

  wav.close();
  audioTerminado  = true;
  audioTaskHandle = NULL;
  vTaskDelete(NULL);
}

static bool IRAM_ATTR onAudioTimer(gptimer_handle_t timer, const gptimer_alarm_event_data_t* edata, void* user_ctx) {

  if (enRampaInicio) {
    dac_output_voltage(DAC_CHAN_1, nivelRampa);
    ultimoNivelDAC = nivelRampa;
    if (nivelRampa < 128) nivelRampa++; else enRampaInicio = false;
    return true;
  }

  if (!enRampaFin) {
    if (bufHead != bufTail) {
      uint8_t sample = audioBuffer[bufHead];
      bufHead = (bufHead + 1) % BUFFER_SIZE;
      if (!audioMuteado) {
        // Aplicar volumen global — cubre tanto fade como nivel LOW
        int16_t centrada = (int16_t)sample - 128;
        int16_t escalada = (centrada * volumenGlobal) >> 8;
        uint8_t volumen  = (uint8_t)(escalada + 128);
        dac_output_voltage(DAC_CHAN_1, volumen);
        ultimoNivelDAC = volumen;
      }
    }
    if (audioTerminado && bufHead == bufTail) {
      enRampaFin = true;
      nivelRampa = 128;
    }
    return true;
  }

  dac_output_voltage(DAC_CHAN_1, nivelRampa);
  ultimoNivelDAC = nivelRampa;
  if (nivelRampa > 0) nivelRampa--;
  else {
    gptimer_stop(timer);
    audioReproduciendo = false;
    audioParando = false;
    enRampaFin    = false;
    volumenGlobal = 256;  // restaurar para la próxima reproducción
  }
  return true;
}

void initAudio() {
  sdSPI.begin(18, 19, 23, 5);
  if (!SD.begin(SD_CS_PIN, sdSPI)) { 
    Serial.println("[Audio] SD no encontrada"); 
    slog_error("Audio: SD no encontrada");
    return; 
  }
  cargarListaWAV();
  dac_output_enable(DAC_CHAN_1);
  gptimer_config_t timer_config = { .clk_src = GPTIMER_CLK_SRC_DEFAULT, .direction = GPTIMER_COUNT_UP, .resolution_hz = 1000000 };
  gptimer_new_timer(&timer_config, &audioTimer);
  gptimer_alarm_config_t alarm_config = { .alarm_count = 62, .reload_count = 0, .flags = {.auto_reload_on_alarm = true} };
  gptimer_set_alarm_action(audioTimer, &alarm_config);
  gptimer_event_callbacks_t cbs = { .on_alarm = onAudioTimer };
  gptimer_register_event_callbacks(audioTimer, &cbs, NULL);
  gptimer_enable(audioTimer);
  Serial.println("[Audio] Listo");
  slog_info("Audio: Sistema de audio inicializado");
}

bool detenerAudio(uint32_t timeoutMs) {
  if (!audioReproduciendo && audioTaskHandle == NULL) {
    audioCancelar = false;
    audioParando = false;
    dac_output_voltage(DAC_CHAN_1, 0);
    ultimoNivelDAC = 0;
    return true;
  }

  audioParando = true;
  audioCancelar = true;

  unsigned long t0 = millis();
  while (audioTaskHandle != NULL && millis() - t0 < timeoutMs) {
    delay(5);
  }

  if (audioTaskHandle != NULL) {
    Serial.println("[Audio] No se pudo detener la tarea de audio a tiempo");
    slog_warning("Audio: timeout deteniendo tarea de audio");
    return false;
  }

  bufHead = 0;
  bufTail = 0;
  enRampaInicio = false;
  enRampaFin = true;
  audioTerminado = true;
  nivelRampa = ultimoNivelDAC;

  t0 = millis();
  while (audioReproduciendo && millis() - t0 < timeoutMs) {
    delay(5);
  }

  if (audioReproduciendo) {
    esp_err_t err = gptimer_stop(audioTimer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      Serial.printf("[Audio] gptimer_stop fallo: %d\n", err);
    }
    dac_output_voltage(DAC_CHAN_1, 0);
    ultimoNivelDAC = 0;
    audioReproduciendo = false;
    enRampaFin = false;
  }

  audioCancelar = false;
  audioTerminado = false;
  volumenGlobal = 256;
  audioParando = false;
  return true;
}

void cargarListaWAV() {
  unsigned long t0 = millis();
  Serial.println("[Audio] Escaneando carpeta /f1/...");
  
  listaWAVF1.clear();
  listaWAVF1Low.clear();
  
  // Verificar si existe la carpeta f1
  if (!SD.exists("/f1")) {
    Serial.println("[Audio] Carpeta /f1/ no existe, creándola...");
    SD.mkdir("/f1");
  }
  
  File f1Dir = SD.open("/f1");
  if (!f1Dir || !f1Dir.isDirectory()) {
    Serial.println("[Audio] Error: No se pudo abrir /f1/");
    return;
  }
  
  File f = f1Dir.openNextFile();
  int count = 0;
  
  while (f) {
    String nombre = String(f.name());
    String nombreL = nombre;
    nombreL.toLowerCase();
    
    if (nombreL.endsWith(".wav")) {
      count++;
      String ruta = "/f1/" + nombre;  // Ruta completa con carpeta
      
      if (nombreL.endsWith("_low.wav")) {
        listaWAVF1Low.push_back(ruta);
        Serial.printf("[Audio] F1_LOW: %s\n", nombre.c_str());
      } else {
        listaWAVF1.push_back(ruta);
        Serial.printf("[Audio] F1: %s\n", nombre.c_str());
      }
    }
    f = f1Dir.openNextFile();
  }
  f1Dir.close();
  
  Serial.printf("[Audio] Escaneo completado: %d archivos en %lu ms\n", 
                count, millis() - t0);
  Serial.printf("[Audio] F1: %d | F1_low: %d\n", 
                listaWAVF1.size(), listaWAVF1Low.size());
}

void reproducirChime() {
  if (audioReproduciendo) return;
  if (audioTaskHandle != NULL) return;
  if (audioParando) return;
  if (estadoAudio == AUDIO_MUTE) return;

  struct tm ti;
  if (getLocalTime(&ti)) {
    if (ti.tm_hour >= SOUND_END_HOUR || ti.tm_hour < SOUND_START_HOUR) return;
  }

  bool usarLow = (estadoAudio == AUDIO_LOW);

  if (modoF1) {
    std::vector<String>* lista;
    if (usarLow && listaWAVF1Low.size() > 0)
      lista = &listaWAVF1Low;
    else
      lista = &listaWAVF1;

    if (lista->size() == 0) {
      Serial.println("[WAV] No hay archivos F1 en /f1/");
      return;
    }
    int indice;
    if (lista->size() == 1) indice = 0;
    else do {
      indice = random(lista->size());
    } while (indice == ultimoIndiceNormal);
    ultimoIndiceNormal = indice;
    archivoActual = (*lista)[indice];
    Serial.printf("[F1] Reproduciendo: %s\n", archivoActual.c_str());

  } else {
    if (!cancionHoy.cargada || cancionHoy.archivoWav == "") {
      Serial.println("[BB] Sin archivo WAV para hoy");
      return;
    }
    archivoActual = "/billboard/" + cancionHoy.archivoWav;
    if (!SD.exists(archivoActual)) {
      Serial.printf("[BB] WAV no encontrado: %s\n", archivoActual.c_str());
      archivoActual = "/" + cancionHoy.archivoWav;
      if (!SD.exists(archivoActual)) {
        Serial.println("[BB] WAV no encontrado en /billboard/ ni en raíz");
        return;
      }
    }
    Serial.printf("[BB] Reproduciendo: %s\n", archivoActual.c_str());
  }

  snprintf(msg, sizeof(msg), "Chime: %s", archivoActual.c_str());
  slog_info(msg);

  // Resetear estado antes de lanzar la tarea
  audioTerminado = false;
  bufHead        = 0;
  bufTail        = 0;
  nivelRampa     = 0;
  enRampaInicio  = true;
  enRampaFin     = false;
  audioReproduciendo = true;
  audioCancelar  = false;
  volumenGlobal  = 256;  // siempre arrancar con volumen completo

  BaseType_t creada = xTaskCreatePinnedToCore(audioTask, "audioTask", 4096, NULL, 1, &audioTaskHandle, 0);
  if (creada != pdPASS) {
    Serial.println("[Audio] Error creando audioTask");
    slog_error("Audio: error creando audioTask");
    audioTaskHandle = NULL;
    audioReproduciendo = false;
    audioCancelar = false;
    audioTerminado = false;
    dac_output_voltage(DAC_CHAN_1, 0);
    ultimoNivelDAC = 0;
  }
}

// Función para generar envolvente pseudoaleatoria suave
void generarEnvolventeAudio() {
  Serial.println("[Audio] Generando envolvente para los segundos...");

  struct tm ti;
  if (getLocalTime(&ti)) {
    srand(ti.tm_min + ti.tm_hour * 60 + ti.tm_mday * 1440);
  } else {
    srand(millis());
  }

  // Más puntos de control para más variedad — 12 en lugar de 8
  const int numPuntos = 12;
  int puntosControl[numPuntos];
  for (int i = 0; i < numPuntos; i++) {
    puntosControl[i] = rand() % 15;  // ← altura entre 0 y 14, permite llegar a 0
  }

  // Interpolar suavemente entre los puntos de control
  for (int i = 0; i < 80; i++) {
    float pos  = (i / 79.0) * (numPuntos - 1);
    int   idx  = (int)pos;
    float frac = pos - idx;

    if (idx >= numPuntos - 1) {
      alturas[i] = puntosControl[numPuntos - 1];
    } else {
      alturas[i] = (int)(puntosControl[idx] * (1.0 - frac) + puntosControl[idx + 1] * frac);
    }

    // Variación pequeña
    alturas[i] += (rand() % 3) - 1;

    // ← permite llegar a 0 y hasta 14
    if (alturas[i] < 0)  alturas[i] = 0;
    if (alturas[i] > 14) alturas[i] = 14;
  }

  envolventeGenerada = true;
}

// Función para obtener color basado en altura
uint16_t colorPorAltura(int altura) {
  float progreso = (altura - 3) / 11.0;  // 0 = altura mínima, 1 = altura máxima
  
  if (modoF1) {
    if (progreso < 0.33) {
      return TFT_WHITE;      // Blanco
    } else if (progreso < 0.66) {
      return 0xFD20;          // Naranja
    } else {
      return F1_ROJO;         // Rojo F1
    }
  } else {
    //Billboard
    if (progreso < 0.33) {
      return BB_ACENTO_MEDIO;
    } else if (progreso < 0.66) {
      return BB_ACENTO;
    } else {
      return BB_ACENTO_BRILLO;
    }

  }
}




// ══════════════════════════════════════════════
//  BACKLIGHT
// ══════════════════════════════════════════════
void adjust_backlight() {
  static unsigned long lastUpdate = 0; static int16_t lastBrightness = 160;
  if (millis() - lastUpdate < 5000) return;
  lastUpdate = millis();
  int16_t lightLevel = analogRead(lightSensorPin), targetBrightness;
  if (lightLevel <= 60) targetBrightness = BRILLO_MAXIMO;
  else if (lightLevel >= 1900) targetBrightness = BRILLO_MINIMO;
  else {
    int percent = map(lightLevel, 60, 1900, 100, 0);
    float corrected = pow(percent / 100.0, 1.7);
    targetBrightness = BRILLO_MINIMO + (int)(corrected * (BRILLO_MAXIMO - BRILLO_MINIMO));
  }
  lastBrightness = constrain((int)(lastBrightness + (targetBrightness - lastBrightness) * 0.35), BRILLO_MINIMO, BRILLO_MAXIMO);
  analogWrite(backlightPin, lastBrightness);
}

// ══════════════════════════════════════════════
//  TIMEZONE
// ══════════════════════════════════════════════
long obtenerOffsetPorIP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TZ] Sin WiFi → imposible obtener offset");
    return -999999;
  }
  HTTPClient http;
  http.begin("http://ip-api.com/json/?fields=timezone,offset,dst");
  http.setTimeout(7000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("[TZ] Error HTTP %d al obtener offset\n", code);
    http.end();
    return -999999;
  }
  String body = http.getString(); http.end();
  int idx = body.indexOf("\"offset\":"); if (idx == -1) {
    Serial.println("[TZ] No se encontró campo 'offset'");
    return -999999;
  }
  idx += 9;
  int fin = body.indexOf(',', idx); if (fin == -1) fin = body.indexOf('}', idx);
  long offset = body.substring(idx, fin).toInt();
  Serial.printf("[TZ] Offset detectado desde internet: %ld segundos\n", offset);
  return offset;
}

void sincronizarHora() {
  Serial.println("[TZ] Sincronizando hora...");
  preferences.begin("timezone", true);
  long savedOffset = preferences.getLong("offset", -999999);
  preferences.end();

  tzOffsetSegundos = obtenerOffsetPorIP();
  if (tzOffsetSegundos != -999999) {
    preferences.begin("timezone", false);
    preferences.putLong("offset", tzOffsetSegundos);
    preferences.end();
    Serial.printf("[TZ] ✅ Offset ACTUALIZADO desde internet: %ld segundos\n", tzOffsetSegundos);
    slog_info("Timezone offset actualizado desde internet");
  } else if (savedOffset != -999999) {
    tzOffsetSegundos = savedOffset;
    Serial.printf("[TZ] 📦 Usando offset guardado en flash: %ld segundos\n", tzOffsetSegundos);
  } else {
    tzOffsetSegundos = -10800;
    Serial.println("[TZ] ⚠️ Sin datos guardados → usando fallback (-10800)");
  }

  sntp_set_time_sync_notification_cb(ntpCallback);
  Serial.println("[NTP] Sincronizando hora...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  struct tm ti = {};
  bool ntpOk = false;
  for (int i = 0; i < 6; i++) {
    if (getLocalTime(&ti)) {
      ntpOk = true;
      Serial.printf("[NTP] OK - Hora UTC: %02d:%02d:%02d\n", ti.tm_hour, ti.tm_min, ti.tm_sec);
      break;
    }
    delay(500); Serial.print(".");
  }
  Serial.println();

  if (ntpOk) {
    if (rtcDisponible) {
      DateTime nowUTC(time(nullptr));
      rtc.adjust(nowUTC);
      Serial.printf("[RTC] Sincronizado en UTC: %02d:%02d:%02d\n",
                    nowUTC.hour(), nowUTC.minute(), nowUTC.second());
    }
  } else if (rtcDisponible) {
    Serial.println("[TZ] NTP falló → usando RTC en UTC");
    DateTime rtcUTC = rtc.now();
    struct tm tmUTC = {};
    tmUTC.tm_year  = rtcUTC.year() - 1900;
    tmUTC.tm_mon   = rtcUTC.month() - 1;
    tmUTC.tm_mday  = rtcUTC.day();
    tmUTC.tm_hour  = rtcUTC.hour();
    tmUTC.tm_min   = rtcUTC.minute();
    tmUTC.tm_sec   = rtcUTC.second();
    tmUTC.tm_isdst = 0;
    time_t utcTime = mktime(&tmUTC);
    struct timeval tv = { .tv_sec = utcTime, .tv_usec = 0 };
    settimeofday(&tv, NULL);
  }

  int gmtHours = -(tzOffsetSegundos / 3600);
  char tzString[16];
  sprintf(tzString, "GMT%+d", gmtHours);
  setenv("TZ", tzString, 1); tzset();
  Serial.printf("[TZ] Timezone aplicado: %s (offset %ld)\n", tzString, tzOffsetSegundos);

  delay(200);
  if (getLocalTime(&ti)) {
    Serial.printf("[TZ] Hora LOCAL: %02d:%02d:%02d | Offset: %ld\n",
                  ti.tm_hour, ti.tm_min, ti.tm_sec, tzOffsetSegundos);
  }
  snprintf(msg, sizeof(msg), "TZ: Hora LOCAL: %02d:%02d:%02d | Offset: %ld\n", ti.tm_hour, ti.tm_min, ti.tm_sec, tzOffsetSegundos);
  slog_info(msg);
}

// ══════════════════════════════════════════════
//  SYSLOG
// ══════════════════════════════════════════════
WiFiUDP syslogUDP;
const char* SYSLOG_HOST = "192.168.0.190";
const int   SYSLOG_PORT = 5514;
char SYSLOG_HOSTNAME[32] = "esp32-reloj";
String deviceHostname = "esp32-reloj";

void syslog(int severity, const char* mensaje) {
  if (WiFi.status() != WL_CONNECTED) return;
  int pri = 8 + severity;
  time_t ahora; time(&ahora);
  struct tm* t = localtime(&ahora);
  const char* meses[] = {"Jan","Feb","Mar","Apr","May","Jun",
                          "Jul","Aug","Sep","Oct","Nov","Dec"};
  char packet[512];
  snprintf(packet, sizeof(packet),
    "<%d>%s %2d %02d:%02d:%02d %s %s",
    pri,
    meses[t->tm_mon],
    t->tm_mday,
    t->tm_hour, t->tm_min, t->tm_sec,
    SYSLOG_HOSTNAME,
    mensaje);
  syslogUDP.beginPacket(SYSLOG_HOST, SYSLOG_PORT);
  syslogUDP.print(packet);
  syslogUDP.endPacket();
}

void slog_info(const char* m)    { syslog(6, m); }
void slog_warning(const char* m) { syslog(4, m); }
void slog_error(const char* m)   { syslog(3, m); }

void setupHostname() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  char suffix[8]; snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  deviceHostname = "esp32-reloj-" + String(suffix);
  WiFi.setHostname(deviceHostname.c_str());
  strncpy(SYSLOG_HOSTNAME, deviceHostname.c_str(), 31);
  SYSLOG_HOSTNAME[31] = '\0';
  Serial.println("Hostname configurado: " + deviceHostname);
}

// ══════════════════════════════════════════════
//  SETUP
// ══════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n══════════════════════════════");
  Serial.println("   MULTI CLOCK v1.3 - Iniciando");
  Serial.println("══════════════════════════════");

  tft.init(); tft.setRotation(1); tft.fillScreen(NEGRO);
  pinMode(lightSensorPin, INPUT);
  pinMode(backlightPin, OUTPUT);
  analogWrite(backlightPin, 160);

  Wire.begin(27, 22);
  if (rtc.begin()) {
    rtcDisponible = true;
    Serial.println("[RTC] Detectado");
    slog_info("RTC detectado");
    DateTime now = rtc.now();
    Serial.printf("[RTC] %02d:%02d:%02d\n", now.hour(), now.minute(), now.second());
  } else {
    Serial.println("[RTC] No detectado");
    slog_warning("RTC no detectado");
  }

  if (!LittleFS.begin()) {
    Serial.println("LittleFS: error al montar");
    slog_error("Error al montar LittleFS");
  } else {
    Serial.println("LittleFS montado OK");
    slog_info("LittleFS montado correctamente");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f) {
      Serial.println("  Archivo: " + String(f.name()) + " (" + String(f.size()) + " bytes)");
      f = root.openNextFile();
    }
  }
  Serial.printf("[LittleFS] Total: %d bytes | Usado: %d bytes | Libre: %d bytes\n", 
                LittleFS.totalBytes(), LittleFS.usedBytes(), LittleFS.totalBytes() - LittleFS.usedBytes());

  preferences.begin("config", true);
  modoF1 = preferences.getBool("modoF1", false);
  preferences.end();
  snprintf(msg, sizeof(msg), "[Config] Modo guardado: %s", modoF1 ? "F1" : "Billboard");
  Serial.println(msg);
  slog_info(msg);
  if (modoF1) cargarCacheF1();

  pantallaCargando("Iniciando...");

  bool conectado = conectarWiFi();
  syslogUDP.begin(0);

  if (conectado) {
    setupHostname();
    snprintf(msg, sizeof(msg), "Dispositivo iniciado como: %s", SYSLOG_HOSTNAME);
    Serial.println(msg);
    slog_info(msg);
    snprintf(msg, sizeof(msg), "WiFi conectado SSID=%s IP=%s RSSI=%d", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.println(msg);
    slog_info(msg);
  }

  snprintf(msg, sizeof(msg), "LittleFS: total=%d usado=%d libre=%d", 
           LittleFS.totalBytes(), LittleFS.usedBytes(), LittleFS.totalBytes() - LittleFS.usedBytes());
  slog_info(msg);

  if (!conectado) {
    mostrarPantallaModoAP();
    startAPMode();
    while (inAPMode) { dnsServer.processNextRequest(); server.handleClient(); delay(10); }
  }

  pantallaCargando("Sincronizando hora...");
  sincronizarHora();
  slog_info("MultiClock iniciando");

  // AGREGAR ESTA LÍNEA:
  generarEnvolventeAudio();  // Generar envolvente inicial para los segundos

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  Serial.println("[Touch] Inicializado");

  Serial.println("[OTA] Verificando...");
  verificarOTA();
  Serial.println("[OTA] Listo");
  
  if (modoF1) {
    pantallaCargando("Buscando proxima carrera...");
    slog_info("Modo F1");
    actualizarF1();
  } else {
    pantallaCargando("Cargando cancion del dia...");
    slog_info("Modo Billboard");
  }

  ultimaActualizacionFixture = millis();

  initAudio();

  if (!modoF1) {
    cargarCancionDelDia();
  }

  dibujarLayout();
  slog_info("Layout dibujado");

  Serial.println("══════════════════════════════");
  Serial.printf("RAM libre: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Bloque max: %d bytes\n", ESP.getMaxAllocHeap());
  uint32_t intervaloSNTP = sntp_get_sync_interval();
  Serial.printf("Intervalo SNTP: %lu segundos (%lu horas)\n", 
                intervaloSNTP / 1000, 
                intervaloSNTP / 3600000);

  Serial.println("══════════════════════════════");
  snprintf(msg, sizeof(msg), "MultiClock iniciado correctamente. RAM libre: %d bytes", ESP.getFreeHeap());
  slog_info(msg);
}

// ══════════════════════════════════════════════
//  LOOP
// ══════════════════════════════════════════════
void loop() {
  unsigned long ahora = millis();

  if (!modoSubida) actualizarReloj();

  if (!modoSubida && modoF1 && ahora - ultimoCambio > INTERVALO_CAMBIO) {
    mostrarAlternado = !mostrarAlternado;
    dibujarTextoInferiorF1();
    ultimoCambio = ahora;
  }

  if (modoF1 && !modoSubida && ahora - ultimaActualizacionFixture >= FIXTURE_INTERVAL) {
    ultimaActualizacionFixture = ahora;
    Serial.println("[Loop] Actualizando fixture F1...");
    slog_info("Actualizando F1...");
    actualizarF1();
    dibujarPartido();
  }

  static unsigned long ultimaActSesion = 0;
  if (modoF1 && !modoSubida && ahora - ultimaActSesion > 60000) {
    actualizarProximaSesion(); dibujarTextoInferior(); ultimaActSesion = ahora;
  }

  if (touchscreen.touched()) {
    if (modoSubida) {
      TS_Point p = touchscreen.getPoint();
      int tx = map(p.x, 200, 3800, 0, 320);
      int ty = map(p.y, 200, 3800, 0, 240);
      Serial.printf("[Touch] Subida - x:%d y:%d\n", tx, ty);
      if (tx > 60 && tx < 260 && ty > 190 && ty < 225) {
        modoF1 = !modoF1;
        preferences.begin("config", false);
        preferences.putBool("modoF1", modoF1);
        preferences.end();
        Serial.println("[Touch] Modo cambiado a: " + String(modoF1 ? "F1" : "Billboard"));
        slog_info(("[Touch] Modo cambiado a: " + String(modoF1 ? "F1" : "Billboard")).c_str());
        tft.fillScreen(TFT_BLACK);
        tft.setTextDatum(MC_DATUM);
        tft.setFreeFont(&FreeSansBold9pt7b);
        tft.setTextColor(TFT_WHITE);
        tft.drawString(modoF1 ? "Cambiando a F1..." : "Cambiando a Music...", 160, 120);
        tft.setTextFont(1);
        delay(1500);
        ESP.restart();
      }
    }
    if (!touchActivo) { 
      touchActivo = true; inicioTouch = millis(); 
    } else {
      if (!modoSubida && millis() - inicioTouch > 3000) {
        Serial.println("[Touch] Toque largo detectado, iniciando modo subida");
        slog_info("Touch: Toque largo detectado, iniciando modo subida");
        iniciarServidorSubida();
      }
    }
  } else {
    if (touchActivo) {
      unsigned long duracion = millis() - inicioTouch;
      touchActivo = false;
      if (modoSubida) {
        if (duracion < 1000) salirModoSubida();
      } else {
        if (duracion < 1000 && millis() - ultimoTouch > 300) {
          ultimoTouch = millis();
          bool habiaAudioActivo = (audioReproduciendo || audioTaskHandle != NULL || audioParando);
          EstadoAudio estadoAnterior = estadoAudio;
          if (estadoAudio == AUDIO_MUTE)     estadoAudio = AUDIO_LOW;
          else if (estadoAudio == AUDIO_LOW) estadoAudio = AUDIO_NORMAL;
          else                               estadoAudio = AUDIO_MUTE;
          audioMuteado = (estadoAudio == AUDIO_MUTE);
          if (habiaAudioActivo && estadoAudio == AUDIO_MUTE && !detenerAudio(AUDIO_STOP_TIMEOUT_MS)) {
              Serial.println("[Audio] Toggle aplicado, esperando que termine la tarea anterior antes de reproducir otro audio");
          }
          Serial.printf("[Audio] Toggle → estado=%d (0=NORMAL 1=LOW 2=MUTE)\n", (int)estadoAudio);
          if (estadoAnterior == AUDIO_MUTE && estadoAudio == AUDIO_LOW && !audioReproduciendo && audioTaskHandle == NULL && !audioParando) {
            reproducirChime();
          }
        }
      }
    }
  }

  if (modoSubida) server.handleClient();

  adjust_backlight();

  if (estadoAudio != ultimoEstadoMute) {
    dibujarIconoMute(3, 50, estadoAudio);
    const char* estadoTxt = "UNKNOWN";
    switch (estadoAudio) {
      case AUDIO_NORMAL: estadoTxt = "NORMAL"; break;
      case AUDIO_LOW:    estadoTxt = "LOW"; break;
      case AUDIO_MUTE:   estadoTxt = "MUTE"; break;
    }
    snprintf(msg, sizeof(msg), "Audio cambiado a %s", estadoTxt);
    Serial.println(msg);
    slog_info(msg);
    ultimoEstadoMute = estadoAudio;
  }

  static unsigned long ultimaSincHora = 0;
  if (ahora - ultimaSincHora > 3540000UL) {
    Serial.println("[TZ] Chequeando si es hora de sincronizar zona horario");
    struct tm ti; getLocalTime(&ti);
    if (ti.tm_hour == 1) {
      Serial.println("[TZ] Resincronización diaria Offset...");
      slog_info("TZ Resincronización diaria Offset...");
      sincronizarHora();
    }
    ultimaSincHora = ahora;
  }

  delay(10);
}
