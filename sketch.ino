#include <WiFiS3.h>
#include <ArduinoHttpClient.h>
#include <Servo.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

/* =========================
   WIFI
   ========================= */
const char* ssid = "hotspot";
const char* password = "1234567a";

/* =========================
   FIREBASE
   ========================= */
const char* firebaseHost = "appgasiot-default-rtdb.firebaseio.com";
const char* firebaseAuth = "4sj2ui1yE544LRJCOeWzK0qZogYL1jN8GRWEArFF";

WiFiSSLClient wifi;
HttpClient client(wifi, firebaseHost, 443);

/* =========================
   NTP
   ========================= */
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", -3 * 3600, 60000);

/* =========================
   HARDWARE
   ========================= */
#define GAS_PIN A0
#define SERVO_PIN 9

Servo compuerta;

/* =========================
   ESTADO
   ========================= */
String modo = "manual";        // manual | automatico
String estado = "cerrado";     // abierto | cerrado
String ultimoEstadoDB = "";

int rangoMin = 200;
int rangoMax = 800;

int eventoMin = 200;
int eventoMax = 400;

unsigned long lastSend = 0;
unsigned long lastEvent = 0;

// intervalo de envío
const unsigned long INTERVALO_ENVIO = 30000; // 30s

/* =========================
   SETUP
   ========================= */
void setup() {
  Serial.begin(115200);

  compuerta.attach(SERVO_PIN);
  detenerCompuerta();

  conectarWiFi();

  timeClient.begin();
  timeClient.update();
}

/* =========================
   LOOP
   ========================= */
void loop() {

  leerControlCompuerta();
  leerConfigGas();

  int gas = analogRead(GAS_PIN);
  Serial.println(gas);

  if (gas < 5) {
    delay(3000);
    return;
  }

  enviarLecturaFirebase(gas);

  /* ===== AUTOMÁTICO ===== */
  if (modo == "automatico") {

    if (gas >= rangoMax && estado != "cerrado") {
      moverCerrar();
    }

    if (gas <= rangoMin && estado != "abierto") {
      moverAbrir();
    }
  }

  /* ===== EVENTOS CRÍTICOS ===== */
  if (millis() - lastEvent > 15000) {

    if (gas >= eventoMax) {
      registrarEventoCritico(gas, "Gas muy alto");
      lastEvent = millis();
    }

    if (gas <= eventoMin) {
      registrarEventoCritico(gas, "Gas muy bajo");
      lastEvent = millis();
    }
  }

  delay(3000);
}

/* =========================
   WIFI
   ========================= */
void conectarWiFi() {
  while (WiFi.begin(ssid, password) != WL_CONNECTED) {
    delay(1000);
  }
}

/* =========================
   CONTROL COMPUERTA
   ========================= */
void leerControlCompuerta() {

  client.get("/control_compuerta.json?auth=" + String(firebaseAuth));
  if (client.responseStatusCode() != 200) return;

  String body = client.responseBody();

  if (body.indexOf("\"manual\"") > 0) modo = "manual";
  if (body.indexOf("\"automatico\"") > 0) modo = "automatico";

  rangoMin = getIntField(body, "rango_min", rangoMin);
  rangoMax = getIntField(body, "rango_max", rangoMax);

  if (body.indexOf("\"abierto\"") > 0) ultimoEstadoDB = "abierto";
  if (body.indexOf("\"cerrado\"") > 0) ultimoEstadoDB = "cerrado";

  if (modo == "manual" && ultimoEstadoDB != estado) {
    if (ultimoEstadoDB == "abierto") moverAbrir();
    if (ultimoEstadoDB == "cerrado") moverCerrar();
  }
}

/* =========================
   CONFIG GAS
   ========================= */
void leerConfigGas() {

  client.get("/config_gas.json?auth=" + String(firebaseAuth));
  if (client.responseStatusCode() != 200) return;

  String body = client.responseBody();
  eventoMin = getIntField(body, "minimo", eventoMin);
  eventoMax = getIntField(body, "maximo", eventoMax);
}

/* =========================
   HISTORIAL (CONTROLADO)
   ========================= */
void enviarLecturaFirebase(int gas) {

  if (millis() - lastSend < INTERVALO_ENVIO) return;
  lastSend = millis();

  String data =
    "{"
    "\"valor\":" + String(gas) + ","
    "\"fecha\":\"" + obtenerFecha() + "\","
    "\"hora\":\"" + obtenerHora() + "\","
    "\"compuerta\":\"" + estado + "\""
    "}";

  client.post("/historial_lecturas.json?auth=" + String(firebaseAuth),
              "application/json", data);
}

/* =========================
   EVENTOS CRÍTICOS
   ========================= */
void registrarEventoCritico(int gas, String descripcion) {

  String id = String(millis());
  String path = "/eventos_criticos/" + id + ".json?auth=" + String(firebaseAuth);

  String data =
    "{"
    "\"descripcion\":\"" + descripcion + "\","
    "\"valor\":" + String(gas) + ","
    "\"fecha\":\"" + obtenerFecha() + "\","
    "\"hora\":\"" + obtenerHora() + "\""
    "}";

  client.put(path, "application/json", data);
}

/* =========================
   MOVIMIENTOS SERVO
   ========================= */
void moverAbrir() {
  abrirCompuerta();
  delay(800);
  detenerCompuerta();
  actualizarEstado("abierto");
}

void moverCerrar() {
  cerrarCompuerta();
  delay(800);
  detenerCompuerta();
  actualizarEstado("cerrado");
}

/* =========================
   ESTADO DB
   ========================= */
void actualizarEstado(String nuevoEstado) {
  estado = nuevoEstado;

  client.put("/control_compuerta/estado.json?auth=" + String(firebaseAuth),
             "application/json",
             "\"" + nuevoEstado + "\"");
}

/* =========================
   SERVO
   ========================= */
void abrirCompuerta()   { compuerta.write(70);  }
void cerrarCompuerta()  { compuerta.write(110); }
void detenerCompuerta() { compuerta.write(90);  }

/* =========================
   FECHA / HORA
   ========================= */
String obtenerHora() {
  timeClient.update();
  char b[9];
  sprintf(b, "%02d:%02d:%02d",
          timeClient.getHours(),
          timeClient.getMinutes(),
          timeClient.getSeconds());
  return String(b);
}

String obtenerFecha() {
  timeClient.update();
  unsigned long epoch = timeClient.getEpochTime();
  unsigned long days = epoch / 86400;

  int year = 1970;
  while (days >= 365) {
    days -= 365;
    year++;
  }

  char b[11];
  sprintf(b, "%04d-01-%02lu", year, days + 1);
  return String(b);
}

/* =========================
   JSON SIMPLE
   ========================= */
int getIntField(String body, String key, int def) {
  int p = body.indexOf(key);
  if (p < 0) return def;
  int c = body.indexOf(":", p);
  int e = body.indexOf(",", c);
  if (e < 0) e = body.indexOf("}", c);
  return body.substring(c + 1, e).toInt();
}