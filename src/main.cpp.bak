// Controle da lampada LED RGB + branco quente/frio com ESP32-C3 Super Mini.
// O ESP so envia PWM para o gate dos transistores da placa antiga; a corrente
// dos LEDs sai da fonte/bateria e nao passa pelo ESP.
//
// Modos (um por vez): MANUAL (sliders/comandos), TESTE (um canal por vez, em loop)
// e DEMO (percorre cores, em loop). Voltar ao MANUAL restaura as cores manuais.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <math.h>
#include "config.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// ---------------------------------------------------------------- PWM ----
constexpr uint8_t  NUM_CH  = 5;
constexpr uint32_t PWM_MAX = (1u << PWM_BITS) - 1;

struct Channel {
  const char* name;
  uint8_t     pin;
  uint8_t     value;  // 0..255 (antes de gama e limite): o que esta saindo agora
};

static Channel ch[NUM_CH] = {
  {"r",  PIN_R,  0},
  {"g",  PIN_G,  0},
  {"b",  PIN_B,  0},
  {"ww", PIN_WW, 0},
  {"cw", PIN_CW, 0},
};

static uint8_t manualVal[NUM_CH] = {0, 0, 0, 0, 0};  // cores do modo manual
static uint8_t limitPct = DEFAULT_LIMIT_PCT;

enum Mode : uint8_t { MODE_MANUAL, MODE_TEST, MODE_DEMO };
static Mode curMode = MODE_MANUAL;

static const char* modeName() {
  return curMode == MODE_DEMO ? "demo" : curMode == MODE_TEST ? "test" : "manual";
}

static void pwmAttach(uint8_t idx, uint8_t pin) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)idx;
  if (!ledcAttach(pin, PWM_FREQ_HZ, PWM_BITS))
    Serial.printf("ERRO: PWM falhou no GPIO %u (reduza PWM_BITS ou PWM_FREQ_HZ em config.h)\n", pin);
#else
  ledcSetup(idx, PWM_FREQ_HZ, PWM_BITS);
  ledcAttachPin(pin, idx);
#endif
}

static void pwmWrite(uint8_t idx, uint8_t pin, uint32_t duty) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)idx;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(idx, duty);
#endif
}

static void applyChannel(uint8_t i) {
  float x    = ch[i].value / 255.0f;
  float duty = powf(x, GAMMA_EXP) * (limitPct / 100.0f);
  pwmWrite(i, ch[i].pin, (uint32_t)(duty * PWM_MAX + 0.5f));
}

static void applyAll() {
  for (uint8_t i = 0; i < NUM_CH; i++) applyChannel(i);
}

// -------------------------------------------------------- Modo TESTE ----
// Acende um canal por vez em brilho medio, em loop, e imprime o GPIO.
constexpr uint32_t TEST_ON_MS  = 1500;
constexpr uint32_t TEST_OFF_MS = 300;

static uint8_t  testPhase = 0;  // 0..2*NUM_CH-1 (par = aceso, impar = apagado)
static uint32_t testT     = 0;

static void testShow(uint8_t phase) {
  uint8_t c   = phase / 2;
  bool    lit = (phase % 2 == 0);
  for (uint8_t i = 0; i < NUM_CH; i++) ch[i].value = (lit && i == c) ? 160 : 0;
  if (lit) Serial.printf("Teste: canal '%s' no GPIO %u\n", ch[c].name, (unsigned)ch[c].pin);
  applyAll();
}

static void testStart() {
  Serial.println("Modo teste: um canal por vez, em loop. Envie 'manual' para parar.");
  testPhase = 0;
  testT     = millis();
  testShow(0);
}

static void testTick() {
  uint32_t dur = (testPhase % 2 == 0) ? TEST_ON_MS : TEST_OFF_MS;
  if (millis() - testT >= dur) {
    testT     = millis();
    testPhase = (testPhase + 1) % (2 * NUM_CH);
    testShow(testPhase);
  }
}

// --------------------------------------------------------- Modo DEMO ----
// Percorre cores sozinho e imprime na serial o que esta aceso. Os 5 primeiros
// passos acendem um canal isolado; o resto sao misturas.
struct Step {
  const char* name;
  uint8_t     v[NUM_CH];  // r, g, b, ww, cw
};

static const Step STEPS[] = {
  {"Vermelho",      {255,   0,   0,   0,   0}},
  {"Verde",         {  0, 255,   0,   0,   0}},
  {"Azul",          {  0,   0, 255,   0,   0}},
  {"Branco quente", {  0,   0,   0, 255,   0}},
  {"Branco frio",   {  0,   0,   0,   0, 255}},
  {"Amarelo",       {255, 160,   0,   0,   0}},
  {"Ciano",         {  0, 255, 255,   0,   0}},
  {"Magenta",       {255,   0, 255,   0,   0}},
  {"Laranja",       {255,  80,   0,   0,   0}},
  {"Branco WW+CW",  {  0,   0,   0, 255, 255}},
};
constexpr uint8_t DEMO_STEPS = sizeof(STEPS) / sizeof(STEPS[0]);

static uint8_t  demoIdx = 0;
static uint32_t demoT   = 0;

static void demoShow(uint8_t i) {
  const Step& s = STEPS[i];
  Serial.printf("[%u/%u] %-14s", (unsigned)(i + 1), (unsigned)DEMO_STEPS, s.name);
  for (uint8_t c = 0; c < NUM_CH; c++) {
    ch[c].value = s.v[c];
    if (s.v[c]) Serial.printf(" %s=%u(GPIO%u)", ch[c].name, (unsigned)s.v[c], (unsigned)ch[c].pin);
  }
  Serial.printf("  limite %u%%\n", (unsigned)limitPct);
  applyAll();
}

static void demoStart() {
  Serial.printf("Modo demo: troca a cada %u ms. Envie 'manual' para parar.\n",
                (unsigned)DEMO_STEP_MS);
  demoIdx = 0;
  demoT   = millis();
  demoShow(0);
}

static void demoTick() {
  if (millis() - demoT >= DEMO_STEP_MS) {
    demoT   = millis();
    demoIdx = (demoIdx + 1) % DEMO_STEPS;
    demoShow(demoIdx);
  }
}

static void modeTick() {
  if (curMode == MODE_DEMO) demoTick();
  else if (curMode == MODE_TEST) testTick();
}

// ------------------------------------------------------------- Modos ----
static void setMode(Mode m) {
  if (m == curMode) return;
  curMode = m;
  switch (m) {
    case MODE_TEST:
      testStart();
      break;
    case MODE_DEMO:
      demoStart();
      break;
    case MODE_MANUAL:
      for (uint8_t i = 0; i < NUM_CH; i++) ch[i].value = manualVal[i];
      applyAll();
      Serial.println("Modo manual.");
      break;
  }
}

// Qualquer ajuste manual de cor leva ao modo manual.
static void setValue(uint8_t i, int v) {
  setMode(MODE_MANUAL);
  manualVal[i] = ch[i].value = (uint8_t)constrain(v, 0, 255);
  applyChannel(i);
}

static void setLimit(int pct) {
  limitPct = (uint8_t)constrain(pct, 0, MAX_LIMIT_PCT);
  applyAll();
}

static void allOff() {
  curMode = MODE_MANUAL;
  for (uint8_t i = 0; i < NUM_CH; i++) manualVal[i] = ch[i].value = 0;
  applyAll();
}

static int indexOfName(const String& n) {
  for (uint8_t i = 0; i < NUM_CH; i++)
    if (n == ch[i].name) return i;
  return -1;
}

static void printStatus() {
  Serial.printf("modo = %s\n", modeName());
  for (uint8_t i = 0; i < NUM_CH; i++)
    Serial.printf("%-2s (GPIO %2u) = %u\n", ch[i].name, (unsigned)ch[i].pin, (unsigned)ch[i].value);
  Serial.printf("limite = %u%%\n", (unsigned)limitPct);
}

// ------------------------------------------------------------- Serial ----
static const char* HELP =
    "Comandos: <r|g|b|ww|cw|all> 0-255 | limit 0-100 | off | manual | test | demo | status";

static void handleLine(String line) {
  line.trim();
  line.toLowerCase();
  if (!line.length()) return;

  if (line == "off")    { allOff(); Serial.println("ok"); return; }
  if (line == "test")   { setMode(MODE_TEST); return; }
  if (line == "demo")   { setMode(MODE_DEMO); return; }
  if (line == "manual" || line == "demo off" || line == "test off") {
    setMode(MODE_MANUAL);
    return;
  }
  if (line == "status") { printStatus(); return; }

  int sp = line.indexOf(' ');
  if (sp < 0) {
    Serial.println(HELP);
    return;
  }
  String name = line.substring(0, sp);
  int    v    = line.substring(sp + 1).toInt();

  if (name == "limit") {
    setLimit(v);
    Serial.printf("limite = %u%%\n", (unsigned)limitPct);
  } else if (name == "all") {
    for (uint8_t i = 0; i < NUM_CH; i++) setValue(i, v);
    Serial.println("ok");
  } else {
    int i = indexOfName(name);
    if (i < 0) {
      Serial.println("Canal desconhecido. Use r, g, b, ww, cw ou all.");
    } else {
      setValue(i, v);
      Serial.println("ok");
    }
  }
}

// ---------------------------------------------------------------- Web ----
static WebServer server(80);

static const char PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="pt-br"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Lampada LED</title>
<style>
:root{color-scheme:light dark}
body{font-family:system-ui,sans-serif;max-width:420px;margin:1rem auto;padding:0 1rem}
label{display:block;margin-top:.9rem}
input[type=range]{width:100%}
input[type=color]{width:100%;height:3rem;border:0;padding:0}
input:disabled{opacity:.4}
.seg{display:flex;gap:.5rem;margin:1rem 0}
.seg button{flex:1;padding:.6rem .3rem;border:2px solid #bbb;border-radius:8px;background:#fff;color:#222;font:inherit;cursor:pointer}
.seg button.on{border-color:#1a73e8;background:#e8f0fe;color:#0b4fb3;font-weight:600}
button.off{margin-top:1rem;padding:.6rem 1rem}
@media (prefers-color-scheme:dark){
.seg button{background:#222;color:#eee;border-color:#555}
.seg button.on{border-color:#6aa3ff;background:#12306b;color:#cfe0ff}}
</style></head><body>
<h2>Lampada LED</h2>
<div class="seg">
<button id="m_manual" onclick="setMode('manual')">Manual</button>
<button id="m_test" onclick="setMode('test')">Teste dos canais</button>
<button id="m_demo" onclick="setMode('demo')">Demo de cores</button>
</div>
<input type="color" id="c" value="#ffffff">
<div id="s"></div>
<button class="off" onclick="off()">Apagar</button>
<script>
const N=['r','g','b','ww','cw'];
const L={r:'Vermelho',g:'Verde',b:'Azul',ww:'Branco quente',cw:'Branco frio',limit:'Limite de brilho (%)'};
const el=k=>document.getElementById('i_'+k);
let mode='manual',tCh,tLim;
document.getElementById('s').innerHTML=[...N,'limit'].map(k=>
 '<label>'+L[k]+' <b id="v_'+k+'"></b><input type="range" id="i_'+k+'" min="0" max="'+(k=='limit'?100:255)+'" oninput="chg(\''+k+'\')"></label>').join('');
function lab(k){document.getElementById('v_'+k).textContent=el(k).value}
function api(q){return fetch(q).then(r=>r.json())}
function show(s,force){
 const changed=s.mode!==mode;
 mode=s.mode;
 ['manual','test','demo'].forEach(m=>{
  const b=document.getElementById('m_'+m);
  b.classList.toggle('on',m==mode);
  b.setAttribute('aria-pressed',m==mode);
 });
 const man=mode=='manual';
 N.forEach(k=>el(k).disabled=!man);
 document.getElementById('c').disabled=!man;
 if(!man||changed||force)N.forEach(k=>{el(k).value=s[k];lab(k)});
 if(changed||force){el('limit').value=s.limit;lab('limit')}
}
function sendCh(){
 const q=N.map(k=>k+'='+el(k).value).join('&');
 clearTimeout(tCh);tCh=setTimeout(()=>api('/set?'+q).then(show),60);
}
function sendLimit(){
 clearTimeout(tLim);tLim=setTimeout(()=>api('/set?limit='+el('limit').value),60);
}
function chg(k){lab(k);k=='limit'?sendLimit():sendCh()}
function setMode(m){if(m==mode&&m!='manual')m='manual';api('/mode?m='+m).then(s=>show(s))}
function off(){N.forEach(k=>{el(k).value=0;lab(k)});api('/off').then(s=>show(s))}
document.getElementById('c').oninput=e=>{
 const h=e.target.value;
 ['r','g','b'].forEach((k,i)=>{el(k).value=parseInt(h.substr(1+2*i,2),16);lab(k)});
 sendCh();
};
api('/state').then(s=>show(s,true));
setInterval(()=>api('/state').then(s=>show(s)),1000);
</script></body></html>)HTML";

static void sendState() {
  String j = "{";
  for (uint8_t i = 0; i < NUM_CH; i++) {
    j += "\"";
    j += ch[i].name;
    j += "\":";
    j += ch[i].value;
    j += ",";
  }
  j += "\"limit\":";
  j += limitPct;
  j += ",\"mode\":\"";
  j += modeName();
  j += "\"}";
  server.send(200, "application/json", j);
}

static void handleSet() {
  for (uint8_t i = 0; i < NUM_CH; i++)
    if (server.hasArg(ch[i].name)) setValue(i, server.arg(ch[i].name).toInt());
  if (server.hasArg("limit")) setLimit(server.arg("limit").toInt());
  sendState();
}

static void handleMode() {
  String m = server.arg("m");
  if (m == "demo")      setMode(MODE_DEMO);
  else if (m == "test") setMode(MODE_TEST);
  else                  setMode(MODE_MANUAL);
  sendState();
}

static void startWifi() {
  WiFi.setHostname(HOSTNAME);
  bool ok = false;
#ifdef WIFI_SSID
  WiFi.mode(WIFI_STA);
  // Varias placas Super Mini so conectam de forma estavel com potencia reduzida.
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(250);
    modeTick();  // demo/teste continuam rodando enquanto espera o Wi-Fi
  }
  ok = (WiFi.status() == WL_CONNECTED);
#endif
  if (ok) {
    Serial.printf("Wi-Fi ok. IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
#ifdef WIFI_SSID
    Serial.printf("Nao conectou em '%s' (status %d). Rede de 2,4 GHz? Senha correta?\n",
                  WIFI_SSID, (int)WiFi.status());
    int n = WiFi.scanNetworks();
    Serial.printf("Redes visiveis (%d):\n", n);
    for (int i = 0; i < n; i++)
      Serial.printf("  %s (%d dBm)\n", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i));
#else
    Serial.println("include/secrets.h nao encontrado: sem Wi-Fi configurado.");
#endif
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.printf("Modo AP '%s'. IP: %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  }
  if (MDNS.begin(HOSTNAME)) Serial.printf("Tambem em http://%s.local\n", HOSTNAME);
}

// -------------------------------------------------------- setup / loop ----
void setup() {
  // Primeiro: tudo em nivel baixo, para os transistores ficarem desligados.
  for (uint8_t i = 0; i < NUM_CH; i++) {
    pinMode(ch[i].pin, OUTPUT);
    digitalWrite(ch[i].pin, LOW);
  }
  Serial.begin(115200);
  for (uint8_t i = 0; i < NUM_CH; i++) pwmAttach(i, ch[i].pin);
  allOff();

#if DEMO_ON_BOOT
  setMode(MODE_DEMO);
#endif

  startWifi();

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/set", handleSet);
  server.on("/state", sendState);
  server.on("/mode", handleMode);
  server.on("/off", []() { allOff(); sendState(); });
  server.on("/test", []() { setMode(MODE_TEST); sendState(); });
  server.on("/demo", []() { setMode(MODE_DEMO); sendState(); });
  server.begin();

  Serial.println(HELP);
}

void loop() {
  server.handleClient();
  modeTick();

  static String rx;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      handleLine(rx);
      rx = "";
    } else if (rx.length() < 64) {
      rx += c;
    }
  }
}
