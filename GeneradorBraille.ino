//Librerias
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>

// Wifi AP
const char* AP_SSID = "Generador-Braille";
const char* AP_PASS = "braille123";

ESP8266WebServer server(80);

// Estado STA
volatile bool    sta_connecting = false;
String           sta_ssid_req, sta_pass_req;
unsigned long    sta_t0 = 0;
const uint32_t   STA_TIMEOUT_MS = 15000; // 15s

// Punto 1..6
const uint8_t DOT_PINS[6] = { D1, D2, D5, D6, D7, D8 };
const bool INVERT_OUTPUT = false;

// Tiempos por defecto
const uint16_t PULSE_MS_DEFAULT = 250;
const uint16_t GAP_MS_DEFAULT   = 40;
uint16_t pulse_ms = PULSE_MS_DEFAULT;
uint16_t gap_ms   = GAP_MS_DEFAULT;

// Tabla Braille a-z
static const uint8_t BRAILLE_AZ[26] = {
  0x01,0x03,0x09,0x19,0x11,0x0B,0x1B,0x13,0x0A,0x1A,
  0x05,0x07,0x0D,0x1D,0x15,0x0F,0x1F,0x17,0x0E,0x1E,
  0x25,0x27,0x3A,0x2D,0x3D,0x35
};

//Adicional para #, MAYUSCULA y ñ
const uint8_t NUM_SIGN     = 0x3C; // 3-4-5-6
const uint8_t CAP_SIGN     = 0x20; // punto 6
const uint8_t BRAILLE_ENYE = 0x3B; // ñ: 1-2-4-5-6

// Puntuación esencial
const uint8_t DOT_COMMA  = 0x02; // ,
const uint8_t DOT_PERIOD = 0x32; // .
const uint8_t DOT_SEMI   = 0x06; // ;
const uint8_t DOT_COLON  = 0x12; // :
const uint8_t DOT_EXCL   = 0x16; // ¡ !
const uint8_t DOT_QUEST  = 0x22; // ¿ ?

// Estado y cola
bool numMode = false;

#define QSIZE 512
uint8_t qbuf[QSIZE];
volatile uint16_t qHead = 0, qTail = 0;
inline bool qEmpty(){ return qHead==qTail; }
inline bool qFull(){ return (uint16_t)(qHead+1)%QSIZE==qTail; }
bool enqueueMask(uint8_t m){ uint16_t n=(qHead+1)%QSIZE; if(n==qTail) return false; qbuf[qHead]=m; qHead=n; return true; }
bool dequeueMask(uint8_t &m){ if(qEmpty()) return false; m=qbuf[qTail]; qTail=(qTail+1)%QSIZE; return true; }

// Utilidades
inline bool isDigitC(char c){ return c>='0' && c<='9'; }
inline bool isNumTerminator(char c){
  switch(c){
    case ' ': case '\n': case '\r': case '\t':
    case ',': case '.': case ';': case ':':
    case '!': case '¡': case '?': case '¿': return true;
    default: return false;
  }
}

//mascara braille
void setDots(uint8_t mask){
  for(uint8_t i=0;i<6;i++){
    bool on=(mask>>i)&1;
    digitalWrite(DOT_PINS[i], (INVERT_OUTPUT ? !on : on) ? HIGH : LOW);
  }
}

//convertir caracter a mascara braille
uint8_t mapCharToMask(char c){
  if(c>='a' && c<='z') return BRAILLE_AZ[c-'a'];
  if((uint8_t)c==0xF1) return BRAILLE_ENYE; // ñ (Latin-1)
  if(c==' '||c=='\n'||c=='\r'||c=='\t') return 0x00;
  switch(c){
    case ',': return DOT_COMMA;
    case '.': return DOT_PERIOD;
    case ';': return DOT_SEMI;
    case ':': return DOT_COLON;
    case '!': case (char)0xA1: return DOT_EXCL;
    case '?': case (char)0xBF: return DOT_QUEST;
    default:  return 0x00;
  }
}

//Encolar caracteres
void enqueueChar(char c){
  // Mayúsculas ASCII
  if(c>='A' && c<='Z'){ enqueueMask(CAP_SIGN); c = char(c - 'A' + 'a'); }
  // Ñ mayúscula Latin-1
  if((uint8_t)c==0xD1){ enqueueMask(CAP_SIGN); c=(char)0xF1; }

  if(isDigitC(c)){
    if(!numMode){ enqueueMask(NUM_SIGN); numMode=true; }
    uint8_t m = (c=='0') ? BRAILLE_AZ['j'-'a'] : BRAILLE_AZ[(c-'1') + ('a'-'a')];
    enqueueMask(m);
    return;
  }
  
  if(isNumTerminator(c)) numMode=false;

  enqueueMask(mapCharToMask(c));
}

//decodificar UTF-8 Y ENCOLARLO
void decodeAndEnqueueUTF8(const String& s){
  for(size_t i=0;i<s.length();){
    uint8_t b0=(uint8_t)s[i]; char c=0;
    if(b0<0x80){ c=(char)b0; i++; }
    else if(b0==0xC3 && i+1<s.length()){
      uint8_t b1=(uint8_t)s[i+1];
      if(b1==0xB1){ c=(char)0xF1; i+=2; } // ñ
      else if(b1==0x91){ c=(char)0xD1; i+=2; } // Ñ
      else { i++; continue; }
    } else if(b0==0xC2 && i+1<s.length()){
      uint8_t b1=(uint8_t)s[i+1];
      if(b1==0xA1){ c=(char)0xA1; i+=2; } // ¡
      else if(b1==0xBF){ c=(char)0xBF; i+=2; } // ¿
      else { i++; continue; }
    } else { i++; continue; }
    enqueueChar(c);
    yield();
  }
}

//para manejar las salidas
enum PlayState { IDLE, PULSING, GAP };
PlayState playState = IDLE;
uint8_t currentMask = 0;
unsigned long stateTs = 0;

void pumpOutput(){
  unsigned long now = millis();
  switch(playState){
    case IDLE:
      if(!qEmpty()){
        dequeueMask(currentMask);
        setDots(currentMask);
        stateTs = now;
        playState = PULSING;
      }
      break;
    case PULSING:
      if(now - stateTs >= pulse_ms){
        setDots(0x00);
        stateTs = now;
        playState = GAP;
      }
      break;
    case GAP:
      if(now - stateTs >= gap_ms){
        playState = IDLE;
      }
      break;
  }
}

// Paginas HTML
// Pagina principal
const char PAGE_MAIN[] PROGMEM = R"HTML(
<!-- Página principal: envío de texto/teclas y ajuste de tiempos -->
<!doctype html><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>

<!-- Estilos  -->
<title>Braille</title>
<style>
body{font:16px system-ui;margin:0;padding:1rem;max-width:720px}
h1{font-size:18px;margin:0 0 .5rem}
textarea{width:100%;height:9rem;padding:.6rem;border-radius:.6rem;border:1px solid #ccc}
.row{display:flex;gap:.5rem;margin-top:.5rem;align-items:center;flex-wrap:wrap}
button{padding:.6rem 1rem;border:0;border-radius:.6rem;box-shadow:0 1px 3px rgba(0,0,0,.15)}
#status{opacity:.75;margin-top:.5rem;font-size:.9rem;min-height:1em}
input[type=number]{width:6rem}
.mono{font-family:ui-monospace,Menlo,Consolas,monospace}
</style>

<h1>Generador Braille (Grupo C)</h1>

<!-- Muestra IPs AP/STA (se rellena con /ips) -->
<p class="mono" id="ips">IPs…</p>

<!-- Área de entrada de texto -->
<textarea id="t" placeholder="Escribe aquí…"></textarea>

<!-- Controles principales -->
<div class="row">
  <button id="send">Enviar todo</button>
  <button id="clear">Limpiar</button>
  <label style="margin-left:auto"><input type="checkbox" id="live" checked> Envío por carácter</label>
</div>

<!-- Controles para ajustar tiempos, detener la traduccion y configurar wifi -->
<div class="row">
  <label>Pulso (ms) <input id="pw" type="number" min="30" max="2000" value="250"></label>
  <label>Espacio (ms) <input id="gap" type="number" min="0" max="1000" value="40"></label>
  <button id="apply">Aplicar</button>
  <button id="stop" style="background:#fee">Detener</button>
  <button id="test">Test</button>
  <button id="wifi" style="margin-left:auto">Wi-Fi…</button>
</div>

<!-- Mostrar el estado actual -->
<div id="status"></div>

<script>
const q=s=>document.querySelector(s); 
const st=q('#status'); 
const ta=q('#t'); 
let prev="";
//Escribe mensaje en la zona de estado
function ping(m){ st.textContent=m; }

//Ignorar errores
async function api(p,o){ try{ const r=await fetch(p,o); if(!r.ok) throw 0; return await r.text(); }catch(e){} }

//enviar caracteres en tiempo real
async function sendKey(c){ await api('/key?c='+encodeURIComponent(c)); ping('Último: '+c); }

//Enviar el texto completo
async function sendText(txt){ await api('/text',{method:'POST',headers:{'Content-Type':'text/plain'},body:txt}); ping('Texto ('+txt.length+') en cola'); }

//Leer la configuracion de los tiempos
async function getCfg(){ try{ const r=await fetch('/cfg'); if(!r.ok) return; const j=await r.json(); q('#pw').value=j.pw; q('#gap').value=j.gap; }catch(e){} }

//Enviar los nuevos tiempos
async function setCfg(){ await api('/cfg?pw='+encodeURIComponent(q('#pw').value)+'&gap='+encodeURIComponent(q('#gap').value)); ping('Tiempos actualizados'); }

//Obtener IPs y mostrarlas
async function getIPs(){ try{ const s=await api('/ips'); if(s) q('#ips').textContent=s; }catch(e){} }

//Acciones para la UI
q('#send').onclick=()=>sendText(ta.value);
q('#clear').onclick=()=>{ ta.value=""; prev=""; ping(""); };
q('#apply').onclick=()=>setCfg();
q('#stop').onclick =()=>api('/stop').then(()=>ping('Detenido'));
q('#test').onclick =()=>api('/test');
q('#wifi').onclick =()=>location.href='/wifi';

//Detectar lo ultimo que se añadio para enviar en tiempo real
ta.addEventListener('input',()=>{ const v=ta.value; if(q('#live').checked && v.length>prev.length){ const add=v.slice(prev.length); for(const ch of add) sendKey(ch); } prev=v; });

//Inicializacion
getCfg(); getIPs();
</script>
)HTML";

//Pagina Wifi
const char PAGE_WIFI[] PROGMEM = R"HTML(
  <!-- Pagina para configuracion Wifi -->
<!doctype html><meta charset="utf-8"/>
<meta name="viewport" content="width=device-width, initial-scale=1"/>
<title>Wi-Fi</title>

<!-- Estilos -->
<style>
body{font:16px system-ui;margin:0;padding:1rem;max-width:720px}
h1{font-size:18px;margin:0 0 .75rem}
button,input,select{padding:.6rem;border-radius:.6rem;border:1px solid #ccc}
.row{display:flex;gap:.5rem;align-items:center;flex-wrap:wrap;margin:.5rem 0}
ul{list-style:none;padding:0}
li{margin:.25rem 0}
.mono{font-family:ui-monospace,Menlo,Consolas,monospace}
small{opacity:.7}
</style>

<h1>Configurar Wi-Fi (STA)</h1>

<!-- Acciones generales: escaneo, olvidar credenciales, volver a la principal -->
<div class="row">
  <button id="scan">Buscar redes</button>
  <button id="forget">Olvidar credenciales</button>
  <button onclick="location.href='/'">← Volver</button>
</div>

<!-- Seleccion/entrada de SSID y contraseña; boton de conexión -->
<div class="row">
  <select id="ssids"><option>(sin escanear)</option></select>
  <input id="ssid" placeholder="SSID manual">
  <input id="pass" placeholder="Contraseña" type="password">
  <button id="connect">Conectar</button>
</div>

<!-- Estado actual (conectado/no, SSID, IPs) -->
<p id="status" class="mono">Estado…</p>

<script>
// Helper JSON (GET)
const q=s=>document.querySelector(s); const st=q('#status');

async function jget(p){ const r=await fetch(p); if(!r.ok) throw 0; return await r.json(); }

// Ejecuta un escaneo y rellena el select con SSIDs y su RSSI/seguridad
async function scan(){ st.textContent='Buscando...'; const j=await jget('/wifi_scan'); const sel=q('#ssids'); sel.innerHTML=''; j.list.forEach(o=>{ const opt=document.createElement('option'); opt.value=o.ssid; opt.textContent=`${o.ssid}  (RSSI ${o.rssi} dBm${o.sec?' 🔒':''})`; sel.appendChild(opt); }); st.textContent=`Encontradas: ${j.list.length}`; }

// Conectar a una red: usa el SSID del select o el escrito en el input
async function connect(){ const sel=q('#ssids'); const ssid = q('#ssid').value || sel.value || ''; const pass = q('#pass').value || ''; if(!ssid){ st.textContent='Elige/ingresa SSID'; return; } st.textContent='Conectando...'; await fetch('/wifi_connect?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)); poll(); }

// Consulta periodica del estado STA y muestra IPs AP/STA
async function poll(){ try{ const j=await jget('/wifi_status'); st.textContent = j.connected ? (`Conectado a ${j.ssid}  |  IP STA: http://${j.ip_sta}  |  AP: http://${j.ip_ap}`) : (`Estado: ${j.status_txt}  |  AP: http://${j.ip_ap}`); if(!j.connected) setTimeout(poll, 1000); }catch(e){ setTimeout(poll,1000); } }

//Borrar credenciales guardadas
async function forget(){ await fetch('/wifi_forget'); st.textContent='Credenciales borradas. Sigue en modo AP.'; }

// enlaces de botonwa
q('#scan').onclick = ()=>scan();
q('#connect').onclick = ()=>connect();
q('#forget').onclick = ()=>forget();

//empezar mostrando el estado actual
poll();
</script>
)HTML";

// Handlers HTTP
// encolar caracter en tiempo real
void handleKey(){ if(!server.hasArg("c")){ server.send(400,"text/plain","falta c"); return; } decodeAndEnqueueUTF8(server.arg("c")); server.send(200,"text/plain","ok"); }

// en colar bloque de texto
void handleText(){ decodeAndEnqueueUTF8(server.arg("plain")); server.send(200,"application/json","{\"ok\":true}"); }

//ajustar los tiempos y devolver json
void handleCfg(){
  if(server.hasArg("pw")){ int v=server.arg("pw").toInt(); if(v>=30 && v<=2000) pulse_ms=v; }
  if(server.hasArg("gap")){ int v=server.arg("gap").toInt(); if(v>=0 && v<=1000) gap_ms=v; }
  String json = String("{\"pw\":")+pulse_ms+",\"gap\":"+gap_ms+"}";
  server.send(200,"application/json",json);
}

// recorrer los puntos para test
void handleTest(){ for(uint8_t i=0;i<6;i++) enqueueMask(1<<i); server.send(200,"text/plain","test"); }

// detener todo, vaciar cola, apagar los puntos
void handleStop(){ qHead=qTail=0; numMode=false; playState=IDLE; setDots(0x00); server.send(200,"text/plain","stopped"); }

// mostrar las urls de acceso para AP y STA
void handleIPs(){
  IPAddress ipSTA=WiFi.localIP(), ipAP=WiFi.softAPIP();
  char buf[160]; snprintf(buf,sizeof(buf),"AP: http://%s   |   STA: http://%s",
    ipAP.toString().c_str(), ipSTA.toString().c_str());
  server.send(200,"text/plain",buf);
}

// Handlers Wi-Fi (STA)
void beginSTAConnect(const String& ssid, const String& pass){
  WiFi.persistent(true);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());
  sta_connecting = true;
  sta_t0 = millis();
  sta_ssid_req = ssid;
  sta_pass_req = pass;
}

const char* wlStatusTxt(wl_status_t s){
  switch(s){
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "FAILED";
    case WL_WRONG_PASSWORD: return "WRONG_PASS";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

void handleWifiPage(){ server.send_P(200,"text/html",PAGE_WIFI); }

// escaneo wifi
void handleWifiScan(){
  int n = WiFi.scanNetworks(); // bloquea unos segundos (ok en accion manual)
  String json = "{\"list\":[";
  for(int i=0;i<n;i++){
    if(i) json += ",";
    json += "{\"ssid\":\""+String(WiFi.SSID(i))+"\",\"rssi\":"+String(WiFi.RSSI(i))+",\"sec\":"+(WiFi.encryptionType(i)==ENC_TYPE_NONE?"false":"true")+"}";
  }
  json += "]}";
  server.send(200,"application/json",json);
}

//iniciar conexion STA
void handleWifiConnect(){
  String ssid = server.hasArg("ssid") ? server.arg("ssid") : "";
  String pass = server.hasArg("pass") ? server.arg("pass") : "";
  if(!ssid.length()){ server.send(400,"text/plain","falta ssid"); return; }
  beginSTAConnect(ssid, pass);
  server.send(200,"text/plain","connecting");
}

//Estado actual de la conexion wifi
void handleWifiStatus(){
  wl_status_t st = WiFi.status();
  IPAddress ipSTA=WiFi.localIP(), ipAP=WiFi.softAPIP();
  String json = String("{\"connected\":") + (st==WL_CONNECTED?"true":"false")
    + ",\"status\":" + (int)st
    + ",\"status_txt\":\"" + wlStatusTxt(st) + "\""
    + ",\"ssid\":\"" + WiFi.SSID() + "\""
    + ",\"ip_sta\":\"" + ipSTA.toString() + "\""
    + ",\"ip_ap\":\""  + ipAP.toString()  + "\"}";
  server.send(200,"application/json",json);
}

//borrar credenciales guardadas de red wifi
void handleWifiForget(){
  // Desconecta y borra credenciales almacenadas
  WiFi.disconnect(true); // true = wipe creds
  sta_connecting=false;
  server.send(200,"text/plain","forgotten");
}

//configuracion 
void setup(){
  for(uint8_t i=0;i<6;i++){ pinMode(DOT_PINS[i],OUTPUT); digitalWrite(DOT_PINS[i], INVERT_OUTPUT?HIGH:LOW); }
  Serial.begin(115200);

  // AP siempre activo
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);

  // Intentar reconectar con credenciales guardadas (si existían)
  WiFi.setAutoReconnect(true);
  WiFi.begin(); // usa credenciales previas si las hay

  MDNS.begin("braille");
  MDNS.addService("http","tcp",80);

  // Rutas app
  server.on("/",      [](){ server.send_P(200,"text/html",PAGE_MAIN); });
  server.on("/key",   HTTP_GET,  handleKey);
  server.on("/text",  HTTP_POST, handleText);
  server.on("/cfg",   HTTP_GET,  handleCfg);
  server.on("/test",  HTTP_GET,  handleTest);
  server.on("/stop",  HTTP_GET,  handleStop);
  server.on("/ips",   HTTP_GET,  handleIPs);

  // Rutas Wi-Fi
  server.on("/wifi",         HTTP_GET, handleWifiPage);
  server.on("/wifi_scan",    HTTP_GET, handleWifiScan);
  server.on("/wifi_connect", HTTP_GET, handleWifiConnect);
  server.on("/wifi_status",  HTTP_GET, handleWifiStatus);
  server.on("/wifi_forget",  HTTP_GET, handleWifiForget);

  server.begin();

  Serial.printf("AP: %s  IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop(){
  server.handleClient();
  MDNS.update();
  pumpOutput();

  // Gestiona intento de conexión STA con timeout sin bloquear
  if(sta_connecting){
    wl_status_t st = WiFi.status();
    if(st==WL_CONNECTED){
      sta_connecting=false;
      Serial.printf("STA conectado a %s  IP: %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    }else if(millis() - sta_t0 > STA_TIMEOUT_MS){
      sta_connecting=false;
      Serial.println("STA timeout");
    }
  }
}