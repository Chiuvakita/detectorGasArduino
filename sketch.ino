#include <WiFiS3.h>
#include <Servo.h>

const int STOP_US = 1515;  
int US_CW   = 1240;        // <<< HORARIO (abrir).
int US_CCW  = 1780;        // <<< ANTIHORARIO (cerrar)

/* ajuste de ° */
unsigned long T_90_MS = 900;  

const bool GAS_DO_ACTIVO_LOW = true;

/* Filtros anti-ruido y tiempos */
const unsigned long T_CONFIRM_GAS_MS   = 600;   // gas sostenido para confirmar
const unsigned long T_CONFIRM_CLEAR_MS = 900;   // limpio sostenido para confirmar
const unsigned long HOLD_CERRADO_MS    = 5000;  // 5 s quieto tras cerrar
const unsigned long SETTLE_MS          = 100;   // asentamiento corto tras stop (extra)

/* Pines */
const int PIN_SERVO  = 9;   // señal servo
const int PIN_GAS_DO = 2;   // DO MQ-6

/* Wi-Fi AP (control desde el móvil) */
const char* AP_SSID = "MQ6-Servo";
const char* AP_PASS = "12345678";
WiFiServer server(80);

/* Estado lógico */
Servo s;
bool compuertaCerrada = false;  // false=abierta; true=cerrada
bool paused = false;

enum Estado { ABIERTO, CERRADO_HOLD, PROBANDO_ABIERTO };
Estado estado = ABIERTO;

unsigned long tGasStart=0, tOkStart=0;
unsigned long tEstadoDesde=0, tProbeDesde=0;

/* ================== HELPERS SERVO ================== */
inline void servoStop() { s.writeMicroseconds(STOP_US); delay(SETTLE_MS); }

/* Tope a los 90*/
void pulseLimited(int us_cmd, unsigned long ms) {
  if (ms > T_90_MS) ms = T_90_MS;  // tope de 90°
  if (ms == 0) { servoStop(); return; }
  s.writeMicroseconds(us_cmd);
  delay(ms);
  servoStop();
}

/* Movimientos a 90° por tiempo*/
void abrir_CW_90()  { pulseLimited(US_CW,  T_90_MS); compuertaCerrada = false; }
void cerrar_CCW_90(){ pulseLimited(US_CCW, T_90_MS); compuertaCerrada = true;  }

/* ================== MQ-6 con confirmación ================== */
bool leerGasConfirmado() {
  int doRaw = digitalRead(PIN_GAS_DO);
  bool gasRaw = GAS_DO_ACTIVO_LOW ? (doRaw == LOW) : (doRaw == HIGH);
  unsigned long now = millis();

  if (gasRaw) { if (!tGasStart) tGasStart = now; tOkStart = 0; }
  else        { if (!tOkStart) tOkStart = now; tGasStart = 0; }

  bool gasOK = (tGasStart && (now - tGasStart >= T_CONFIRM_GAS_MS));
  bool okOK  = (tOkStart  && (now - tOkStart  >= T_CONFIRM_CLEAR_MS));

  if (gasOK) return true;
  if (okOK)  return false;
  return gasRaw; 
}

/* ================== HTTP  ================== */
String readLine(WiFiClient& c) {
  String line=""; 
  while (c.connected()) {
    int ch=c.read();
    if (ch<0){delay(1);continue;}
    if (ch=='\r') continue;
    if (ch=='\n') break;
    line+=(char)ch;
  }
  return line;
}

void sendText(WiFiClient& c, const String& body) {
  c.println("HTTP/1.1 200 OK");
  c.println("Content-Type: text/plain; charset=utf-8");
  c.println("Connection: close");
  c.print("Content-Length: "); c.println(body.length());
  c.println();
  c.print(body);
}

void sendHTML(WiFiClient& c) {
  String html =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Servo 90° Tiempo</title>"
"<style>body{font-family:system-ui;margin:16px}button{padding:12px 16px;border-radius:10px;border:0;margin:6px;font-weight:600}"
".open{background:#10b981;color:#fff}.close{background:#ef4444;color:#fff}.pause{background:#6b7280;color:#fff}"
".pill{display:inline-block;padding:4px 10px;border-radius:999px;background:#eee;margin-left:6px}"
"input[type=number]{width:100px;padding:6px;margin-left:8px}"
"</style></head><body>"
"<h2>Bisagra por tiempo (≤90°)</h2>"
"<p>Estado: <span id='st' class='pill'>...</span> · <span id='pa' class='pill'>...</span></p>"
"<button class='open'  onclick='go(1)'>Abrir 90° (CW) [1]</button>"
"<button class='close' onclick='go(2)'>Cerrar 90° (CCW) [2]</button>"
"<button class='pause' onclick='go(0)'>Pausa/Resume [0]</button>"
"<p>Tope 90° (ms): <input id='t' type='number' min='100' max='4000' step='10'><button onclick='setT()'>Set</button></p>"
"<script>"
"async function go(b){await fetch('/cmd?b='+b); setTimeout(refresh,200);} "
"async function refresh(){let r=await fetch('/status'); let j=await r.json(); "
"document.getElementById('st').textContent=j.closed?'CERRADO':'ABIERTO'; "
"document.getElementById('pa').textContent=j.paused?'PAUSA':'AUTO'; "
"document.getElementById('t').value=j.t90; } "
"async function setT(){let v=+document.getElementById('t').value; await fetch('/set?t='+v); setTimeout(refresh,300);} "
"refresh(); setInterval(refresh,1500);"
"</script></body></html>";
  c.println("HTTP/1.1 200 OK");
  c.println("Content-Type: text/html; charset=utf-8");
  c.println("Connection: close");
  c.print("Content-Length: "); c.println(html.length());
  c.println();
  c.print(html);
}

void handleClient(WiFiClient& client) {
  String reqLine = readLine(client);
  if (!reqLine.length()) return;
  while (true) { String h=readLine(client); if (!h.length()) break; }

  bool isGET = reqLine.startsWith("GET ");
  int sp1=reqLine.indexOf(' '), sp2=reqLine.indexOf(' ', sp1+1);
  String path=reqLine.substring(sp1+1, sp2);

  if (isGET && (path=="/" || path.startsWith("/index.html"))) { sendHTML(client); return; }

  if (isGET && path.startsWith("/status")) {
    String body = String("{\"closed\":") + (compuertaCerrada ? "true" : "false")
                + ",\"paused\":" + (paused ? "true" : "false")
                + ",\"t90\":" + String(T_90_MS)
                + "}";
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/json; charset=utf-8");
    client.println("Cache-Control: no-store");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(body.length());
    client.println();
    client.print(body);
    return;
  }

  if (isGET && path.startsWith("/set")) {
    // /limites de seguridad
    int q=path.indexOf('?'), tIdx=path.indexOf("t=", q);
    if (tIdx>0) {
      int amp=path.indexOf('&', tIdx);
      String sVal=path.substring(tIdx+2, amp>0?amp:path.length());
      long v = sVal.toInt();
      if (v < 100) v = 100;
      if (v > 4000) v = 4000;
      T_90_MS = (unsigned long)v;
      sendText(client, "OK T_90_MS="+String(T_90_MS));
      return;
    }
    sendText(client, "Uso: /set?t=900");
    return;
  }

  if (isGET && path.startsWith("/cmd")) {
    // /cmd?b=0 → pausa/resume ; 1 → abrir 90° CW ; 2 → cerrar 90° CCW
    int q=path.indexOf('?'); int bIdx=path.indexOf("b=", q);
    int val=-1;
    if (bIdx>0){ int amp=path.indexOf('&', bIdx);
      String sVal=path.substring(bIdx+2, amp>0?amp:path.length());
      val=sVal.toInt();
    }

    if (val==0) { paused = !paused; sendText(client, paused? "PAUSA ON":"PAUSA OFF"); return; }
    if (paused) { sendText(client, "En PAUSA: ignoro orden."); return; }

    if (val==1) { abrir_CW_90();  sendText(client, "OK ABRIR 90° (CW)");  return; }
    if (val==2) { cerrar_CCW_90(); sendText(client, "OK CERRAR 90° (CCW)"); return; }

    sendText(client, "Uso: /cmd?b=0 pausa | /cmd?b=1 abrir | /cmd?b=2 cerrar");
    return;
  }

  client.println("HTTP/1.1 404 Not Found");
  client.println("Connection: close");
  client.println();
}

/* ================== SETUP/LOOP ================== */
void setup() {
  Serial.begin(115200);
  pinMode(PIN_GAS_DO, INPUT);

  s.attach(PIN_SERVO, 500, 2500);
  servoStop(); // quieto al inicio
  compuertaCerrada = false;  //arranque “abierto”
  estado = ABIERTO;
  tEstadoDesde = millis();

  int st = WiFi.beginAP(AP_SSID, AP_PASS, 1);
  if (st != WL_AP_LISTENING) { WiFi.end(); WiFi.beginAP(AP_SSID, AP_PASS, 1); }
  server.begin();
  Serial.print("AP: "); Serial.println(AP_SSID);
  Serial.print("IP: "); Serial.println(WiFi.localIP()); // usualmente 192.168.4.1
  Serial.println("Calentando MQ-6 (30–60 s)...");
  delay(60000);
  Serial.println("Listo.");
}

void loop() {
  // HTTP
  WiFiClient client = server.available();
  if (client) { client.setTimeout(2000); handleClient(client); client.stop(); }

  // Servo siempre quieto cuando no hay acción
  servoStop();

  if (paused) return;

  // Automatización por gas: cerrar 90°, esperar 5 s, abrir 90° para “probar”.
  bool hayGas = leerGasConfirmado();
  unsigned long now = millis();

  switch (estado) {
    case ABIERTO:
      if (hayGas && !compuertaCerrada) {
        cerrar_CCW_90();                      // CCW 90° máx.
        estado = CERRADO_HOLD;
        tEstadoDesde = now;
      }
      break;

    case CERRADO_HOLD:
      if (now - tEstadoDesde >= HOLD_CERRADO_MS) {
        abrir_CW_90();                        // CW 90° máx.
        estado = PROBANDO_ABIERTO;
        tProbeDesde = now;
        tEstadoDesde = now;
      }
      break;

    case PROBANDO_ABIERTO: {
      const unsigned long PROBE_WINDOW_MS = 1500;
      if (now - tProbeDesde <= PROBE_WINDOW_MS) {
        if (hayGas) {
          cerrar_CCW_90();                    // vuelve a cerrar si persiste gas
          estado = CERRADO_HOLD;
          tEstadoDesde = now;
        }
      } else {
        if (!hayGas) {
          estado = ABIERTO;                   // quedó abierto y quieto
          tEstadoDesde = now;
        } else {
          // seguridad: si justo hay gas al final de la ventana, cerramos
          cerrar_CCW_90();
          estado = CERRADO_HOLD;
          tEstadoDesde = now;
        }
      }
    } break;
  }
}
