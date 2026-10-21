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
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Controle da lâmpada</title>
<style>
:root{font-family:system-ui,-apple-system,sans-serif;color:#18202a;background:#f5f7fa;color-scheme:light}
*{box-sizing:border-box}body{margin:0 auto;max-width:520px;padding:20px 16px 36px}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}h1{font-size:1.35rem;margin:0}p{color:#647080;margin:.35rem 0 0;font-size:.9rem}
.card{background:#fff;border:1px solid #e3e8ef;border-radius:20px;padding:16px;box-shadow:0 5px 18px #2538580b;margin-bottom:14px}
.preview{height:112px;border-radius:14px;background:#000;box-shadow:inset 0 0 0 1px #ffffff30;display:grid;place-items:center;color:#fff;text-shadow:0 1px 3px #000;font-weight:600;margin-bottom:14px}
.section-title{font-size:.78rem;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:#667386;margin:0 0 10px}
.presets{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}.preset{border:1px solid #dfe5ec;background:#fff;border-radius:12px;padding:8px 3px;font:inherit;font-size:.72rem;color:#354052;cursor:pointer}.preset:active{transform:scale(.97)}.swatch{height:31px;width:31px;border-radius:50%;margin:0 auto 5px;box-shadow:inset 0 0 0 1px #0001}.preset.selected{border:2px solid #3478f6;padding:7px 2px;background:#eef4ff}
.manual-head{display:flex;justify-content:space-between;align-items:center;cursor:pointer}.manual-head button{border:0;background:none;color:#3478f6;font-weight:600;font-size:.9rem}.manual-body{margin-top:14px}.manual-body[hidden]{display:none}
label{display:grid;grid-template-columns:105px 1fr 34px;gap:8px;align-items:center;margin:14px 0;font-size:.9rem}label b{text-align:right;font-size:.78rem;color:#697586}input[type=range]{width:100%;accent-color:#3478f6}input[type=color]{width:100%;height:42px;border:0;border-radius:10px;padding:0;background:none;cursor:pointer}
.actions{display:flex;gap:8px}.actions button,.mode button{flex:1;border:2px solid #d9e0e8;border-radius:11px;padding:11px 8px;background:#fff;color:#293444;font:inherit;font-weight:600;cursor:pointer}.actions .danger{background:#fff1f1;color:#c43131;border-color:#ffd3d3}.mode{display:flex;gap:8px}.mode button.on{background:#eaf2ff;color:#2364c8;border-color:#3478f6}
input:disabled,.preset:disabled{opacity:.4;cursor:not-allowed}
@media(prefers-color-scheme:dark){:root{color:#edf2f7;background:#11161d;color-scheme:dark}.card,.preset,.actions button,.mode button{background:#1b232d;border-color:#303b49;color:#e4eaf1}.preset.selected{background:#1b355e}.mode button.on{background:#1b355e;color:#cfe0ff;border-color:#6aa3ff}.preview{box-shadow:inset 0 0 0 1px #fff3}.actions .danger{background:#3b2023;color:#ff9b9b;border-color:#623337}}
</style></head><body>
<header><div><h1>Minha lâmpada</h1><p>Escolha uma cena ou ajuste manualmente</p></div><span id="status">● Manual</span></header>
<section class="card"><h2 class="section-title">Modo</h2><div class="mode"><button id="m_manual" onclick="setMode('manual')">Manual</button><button id="m_test" onclick="setMode('test')">Teste dos canais</button><button id="m_demo" onclick="setMode('demo')">Demo de cores</button></div></section>
<section class="card"><div class="preview" id="preview">Desligada</div><h2 class="section-title">Cores prontas</h2><div class="presets" id="presets"></div></section>
<section class="card"><div class="manual-head" onclick="toggleManual()"><div><h2 class="section-title" style="margin:0">Ajuste manual</h2><p>RGB e temperatura do branco</p></div><button id="toggle">Mostrar</button></div><div class="manual-body" id="manual" hidden><input type="color" id="picker" aria-label="Escolher cor"><div id="sliders"></div></div></section>
<section class="card"><h2 class="section-title">Brilho máximo</h2><label><span>Limite</span><input type="range" id="i_limit" min="0" max="100" oninput="changeLimit()"><b id="v_limit">0%</b></label></section>
<div class="actions"><button class="danger" onclick="off()">Apagar</button></div>
<script>
const N=['r','g','b','ww','cw'];
const labels={r:'Vermelho',g:'Verde',b:'Azul',ww:'Branco quente',cw:'Branco frio'};
const names={manual:'Manual',demo:'Demo',test:'Teste'};
const presets=[['Quente','#ff7a24',[255,70,0,0,0]],['Relaxar','#ffb347',[255,150,0,30,0]],['Natureza','#51c878',[20,255,80,0,0]],['Oceano','#2389da',[0,100,255,0,0]],['Noite','#7d4dcc',[80,0,255,0,0]],['Neutro','#ffffff',[0,0,0,110,145]],['Leitura','#fff5d6',[0,0,0,210,70]],['Festa','#ff3b81',[255,0,120,0,0]],['Céu','#6ec6ff',[0,90,255,0,0]],['Luz baixa','#ffd6a0',[100,40,0,25,0]]];
const $=id=>document.getElementById(id);
const el=k=>$('i_'+k);
let mode='manual',tCh,tLim,hold=0;
$('presets').innerHTML=presets.map((p,i)=>`<button class="preset" onclick="applyPreset(${i})"><span class="swatch" style="background:${p[1]}"></span>${p[0]}</button>`).join('');
$('sliders').innerHTML=N.map(k=>`<label><span>${labels[k]}</span><input type="range" id="i_${k}" min="0" max="255" oninput="changeColor()"><b id="v_${k}">0</b></label>`).join('');
function api(q){return fetch(q+(q.includes('?')?'&':'?')+'_='+Date.now(),{cache:'no-store'}).then(r=>{if(!r.ok)throw new Error(r.status);return r.json()})}
function lab(k){$('v_'+k).textContent=k==='limit'?el(k).value+'%':el(k).value}
function offline(){$('status').textContent='● Sem conexão'}
function touch(){hold=Date.now()+1500}
function paintMode(m){
  mode=m;
  $('status').textContent='● '+(names[m]||m);
  document.querySelectorAll('.mode button').forEach(b=>{const on=b.id==='m_'+m;b.classList.toggle('on',on);b.setAttribute('aria-pressed',on)});
  document.querySelectorAll('.preset,#picker,#sliders input').forEach(e=>e.disabled=m!=='manual');
}
function paintPreview(s){
  const warm=[255,214,170],cool=[201,226,255];
  const c=[0,1,2].map(i=>Math.min(255,Math.round(s[N[i]]+warm[i]*s.ww/255+cool[i]*s.cw/255)));
  $('preview').style.background=`rgb(${c.join(',')})`;
  $('preview').textContent=N.every(k=>s[k]===0)?'Desligada':'Ativa';
  document.querySelectorAll('.preset').forEach((b,i)=>b.classList.toggle('selected',presets[i][2].every((v,j)=>v===s[N[j]])));
}
function localState(){const s={};N.forEach(k=>s[k]=+el(k).value);return s}
function paintColors(s){
  N.forEach(k=>{el(k).value=s[k];lab(k)});
  el('limit').value=s.limit;lab('limit');
  $('picker').value='#'+N.slice(0,3).map(k=>Number(s[k]).toString(16).padStart(2,'0')).join('');
  paintPreview(s);
}
function show(s,force){
  if(!force&&Date.now()<hold)return;
  paintMode(s.mode);
  paintColors(s);
}
function send(){clearTimeout(tCh);tCh=setTimeout(()=>api('/set?'+N.map(k=>k+'='+el(k).value).join('&')).then(s=>show(s)).catch(offline),70)}
function changeColor(){N.forEach(lab);touch();paintPreview(localState());send()}
function changeLimit(){lab('limit');touch();clearTimeout(tLim);tLim=setTimeout(()=>api('/set?limit='+el('limit').value).then(s=>show(s)).catch(offline),70)}
function applyPreset(i){const v=presets[i][2];N.forEach((k,j)=>el(k).value=v[j]);changeColor()}
function setMode(m){
  if(m===mode&&m!=='manual')m='manual';
  if(m===mode)return;
  paintMode(m);hold=Date.now()+2000;
  api('/mode?m='+m).then(s=>{hold=0;show(s,true)}).catch(()=>{hold=0;offline()});
}
function off(){hold=Date.now()+2000;api('/off').then(s=>{hold=0;show(s,true)}).catch(()=>{hold=0;offline()})}
function toggleManual(){const x=$('manual');x.hidden=!x.hidden;$('toggle').textContent=x.hidden?'Mostrar':'Ocultar'}
$('picker').oninput=e=>{N.slice(0,3).forEach((k,i)=>el(k).value=parseInt(e.target.value.substr(1+i*2,2),16));changeColor()};
api('/state').then(s=>show(s,true)).catch(offline);
setInterval(()=>api('/state').then(s=>show(s)).catch(offline),1500);
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
  server.sendHeader("Cache-Control", "no-store");
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
