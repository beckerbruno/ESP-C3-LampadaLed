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
#include <Preferences.h>
#include <time.h>
#include <math.h>
#include "config.h"

#if __has_include(<esp_random.h>)
#include <esp_random.h>
#else
#include <esp_system.h>
#endif

#if __has_include("secrets.h")
#include "secrets.h"
#endif

// Segredos e acesso ficam no secrets.h (fora do Git). Valores padrao seguros:
//  - AP_PASS: se nao definida, o ponto de acesso usa uma senha sorteada a cada boot
//    (mostrada na serial), nunca uma senha fixa publica.
//  - API_KEY: se definida, /set, /off e /claude exigem ?key=...
#ifndef AP_SSID
#define AP_SSID "LampadaLed"
#endif
#ifndef API_KEY
#define API_KEY ""
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

// ------------------------------------------------- Aviso do Claude (hooks) ----
// O Claude Code manda "ok" (verde), "ask" (amarelo), "busy" (azul) ou "err" (vermelho)
// para /claude. O aviso cobre a cor atual por alguns segundos e depois some: o que
// estava antes (cor manual, demo, teste ou apagado) volta sozinho, porque nada disso
// foi alterado. Qualquer acao manual (slider, modo, apagar) cancela o aviso.
// Para mudar tempos ou a chave, defina estas macros no config.h.
#ifndef CLAUDE_OK_SECS
#define CLAUDE_OK_SECS    20     // verde: terminou
#endif
#ifndef CLAUDE_ASK_SECS
#define CLAUDE_ASK_SECS   120    // amarelo: precisa de voce
#endif
#ifndef CLAUDE_BUSY_SECS
#define CLAUDE_BUSY_SECS  300    // azul: trabalhando (limite de seguranca)
#endif
#ifndef CLAUDE_ERR_SECS
#define CLAUDE_ERR_SECS   30     // vermelho: erro
#endif

static bool     ovActive = false;
static uint8_t  ovVal[NUM_CH] = {0, 0, 0, 0, 0};
static uint32_t ovUntil = 0;     // 0 = sem prazo (so some com "off")
static uint32_t ovTotal = 0;     // duracao total do aviso em ms (0 = sem prazo)
static char     ovName[8] = "";

// ------------------------------------------- Automacao por horario (NTP) ----
// Hora vem da internet (NTP) quando o ESP esta no Wi-Fi; sem Wi-Fi nao ha hora e a
// agenda fica inativa. A agenda fica gravada na memoria (Preferences).
#ifndef TZ_POSIX
#define TZ_POSIX "<-03>3"        // Brasilia, sem horario de verao
#endif
#ifndef NIGHT_RAMP_MIN
#define NIGHT_RAMP_MIN 30        // transicao suave do brilho da noite (minutos)
#endif

struct Sched {
  bool     aEn;  uint16_t aMin; uint8_t aDays; uint8_t aRamp; uint8_t aScene[NUM_CH];  // acender
  bool     pEn;  uint16_t pMin; uint8_t pDays; uint8_t pFade;                          // apagar
  bool     nEn;  uint16_t nFrom; uint16_t nTo; uint8_t nPct;                           // noite
};
static Sched sc = {false, 7 * 60, 0x7F, 20, {255, 150, 0, 30, 0},
                   false, 23 * 60 + 30, 0x7F, 0,
                   false, 22 * 60, 6 * 60, 30};

enum AutoKind : uint8_t { AUTO_NONE, AUTO_WAKE, AUTO_SLEEP };
static AutoKind autoKind   = AUTO_NONE;
static uint32_t autoStart  = 0, autoDur = 0;  // ms
static float    autoFactor = 1.0f;            // 0..1 sobre a cor atual (acordar/dormir)
static float    nightFactor = 1.0f;           // 0..1 do brilho da noite

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
  float   base = ovActive ? (float)ovVal[i] : ch[i].value * autoFactor;
  float   x   = base / 255.0f * nightFactor;
  if (x > 1.0f) x = 1.0f;
  uint8_t lim = limitPct;
  if (ovActive && lim == 0) lim = DEFAULT_LIMIT_PCT;  // aviso aparece mesmo com limite 0
  float duty = powf(x, GAMMA_EXP) * (lim / 100.0f);
  pwmWrite(i, ch[i].pin, (uint32_t)(duty * PWM_MAX + 0.5f));
}

static void applyAll() {
  for (uint8_t i = 0; i < NUM_CH; i++) applyChannel(i);
}

static void ovClear() {
  if (!ovActive) return;
  ovActive = false;
  ovName[0] = 0;
  applyAll();  // volta ao que estava por baixo
}

static void ovTick() {
  if (ovActive && ovUntil && (int32_t)(millis() - ovUntil) >= 0) {
    Serial.println("Aviso do Claude terminou.");
    ovClear();
  }
}

struct ClaudeState {
  const char* name;
  uint8_t     v[NUM_CH];  // r, g, b, ww, cw
  uint16_t    secs;
};

static const ClaudeState CLAUDE_STATES[] = {
  {"ok",   {  0, 255,   0, 0, 0}, CLAUDE_OK_SECS},
  {"ask",  {255, 160,   0, 0, 0}, CLAUDE_ASK_SECS},
  {"busy", {  0,  60, 255, 0, 0}, CLAUDE_BUSY_SECS},
  {"err",  {255,   0,   0, 0, 0}, CLAUDE_ERR_SECS},
};

// secs < 0 = tempo padrao do estado; secs == 0 = sem prazo.
static bool claudeSet(const String& st, int secs) {
  if (st == "off" || st == "clear") {
    ovClear();
    return true;
  }
  for (const ClaudeState& c : CLAUDE_STATES) {
    if (!st.equals(c.name)) continue;
    for (uint8_t i = 0; i < NUM_CH; i++) ovVal[i] = c.v[i];
    int t   = secs >= 0 ? secs : c.secs;
    ovTotal = (uint32_t)t * 1000UL;
    ovUntil = t ? millis() + ovTotal : 0;
    if (t && ovUntil == 0) ovUntil = 1;
    strncpy(ovName, c.name, sizeof(ovName) - 1);
    ovName[sizeof(ovName) - 1] = 0;
    ovActive = true;
    applyAll();
    Serial.printf("Claude: %s por %d s\n", c.name, t);
    return true;
  }
  return false;
}


// ---------------------------------------------------- Acordar / dormir ----
static void zeroColors() {  // apaga a cor atual sem mexer no aviso do Claude
  curMode = MODE_MANUAL;
  for (uint8_t i = 0; i < NUM_CH; i++) manualVal[i] = ch[i].value = 0;
  applyAll();
}

static void autoCancel() {  // volta ao brilho total da cor atual
  if (autoKind == AUTO_NONE) return;
  autoKind   = AUTO_NONE;
  autoFactor = 1.0f;
  applyAll();
}

static void autoWake(const uint8_t* scene, uint8_t rampMin) {
  curMode = MODE_MANUAL;
  for (uint8_t i = 0; i < NUM_CH; i++) manualVal[i] = ch[i].value = scene[i];
  if (rampMin == 0) {
    autoKind   = AUTO_NONE;
    autoFactor = 1.0f;
  } else {
    autoKind   = AUTO_WAKE;
    autoStart  = millis();
    autoDur    = (uint32_t)rampMin * 60000UL;
    autoFactor = 0.0f;
  }
  applyAll();
  Serial.printf("Acender: subindo em %u min\n", (unsigned)rampMin);
}

static void autoSleep(uint8_t minutes) {  // 0 = apaga na hora
  if (minutes == 0) {
    autoKind   = AUTO_NONE;
    autoFactor = 1.0f;
    zeroColors();
    return;
  }
  bool any = false;
  for (uint8_t i = 0; i < NUM_CH; i++) any = any || ch[i].value;
  if (!any) return;  // nada aceso para apagar
  autoKind   = AUTO_SLEEP;
  autoStart  = millis();
  autoDur    = (uint32_t)minutes * 60000UL;
  autoFactor = 1.0f;
  Serial.printf("Dormir: apagando em %u min\n", (unsigned)minutes);
}

static void autoTick() {
  if (autoKind == AUTO_NONE) return;
  uint32_t el = millis() - autoStart;
  if (el >= autoDur) {
    bool sleeping = autoKind == AUTO_SLEEP;
    autoKind   = AUTO_NONE;
    autoFactor = 1.0f;
    if (sleeping) {
      zeroColors();
      Serial.println("Dormir: apagada.");
    } else {
      applyAll();
      Serial.println("Acender: cena completa.");
    }
    return;
  }
  float f  = (float)el / (float)autoDur;
  float nf = autoKind == AUTO_WAKE ? f : 1.0f - f;
  if (fabsf(nf - autoFactor) > 0.003f) {
    autoFactor = nf;
    applyAll();
  }
}

// ------------------------------------------------------ Agenda por horario ----
static Preferences prefs;

static void schedLoad() {
  if (!prefs.begin("lampada", false)) return;
  sc.aEn   = prefs.getUChar("aEn", 0) != 0;
  sc.aMin  = prefs.getUShort("aMin", sc.aMin);
  sc.aDays = prefs.getUChar("aDays", sc.aDays);
  sc.aRamp = prefs.getUChar("aRamp", sc.aRamp);
  if (prefs.getBytesLength("aScene") == NUM_CH) prefs.getBytes("aScene", sc.aScene, NUM_CH);
  sc.pEn   = prefs.getUChar("pEn", 0) != 0;
  sc.pMin  = prefs.getUShort("pMin", sc.pMin);
  sc.pDays = prefs.getUChar("pDays", sc.pDays);
  sc.pFade = prefs.getUChar("pFade", sc.pFade);
  sc.nEn   = prefs.getUChar("nEn", 0) != 0;
  sc.nFrom = prefs.getUShort("nFrom", sc.nFrom);
  sc.nTo   = prefs.getUShort("nTo", sc.nTo);
  sc.nPct  = prefs.getUChar("nPct", sc.nPct);
  prefs.end();
  if (sc.aMin >= 1440) sc.aMin = 7 * 60;
  if (sc.pMin >= 1440) sc.pMin = 23 * 60 + 30;
  if (sc.nFrom >= 1440) sc.nFrom = 22 * 60;
  if (sc.nTo >= 1440) sc.nTo = 6 * 60;
  sc.aDays &= 0x7F;
  sc.pDays &= 0x7F;
  if (sc.aRamp > 120) sc.aRamp = 120;
  if (sc.pFade > 120) sc.pFade = 120;
  if (sc.nPct > 100) sc.nPct = 100;
}

static void schedSave() {
  if (!prefs.begin("lampada", false)) return;
  prefs.putUChar("aEn", sc.aEn);
  prefs.putUShort("aMin", sc.aMin);
  prefs.putUChar("aDays", sc.aDays);
  prefs.putUChar("aRamp", sc.aRamp);
  prefs.putBytes("aScene", sc.aScene, NUM_CH);
  prefs.putUChar("pEn", sc.pEn);
  prefs.putUShort("pMin", sc.pMin);
  prefs.putUChar("pDays", sc.pDays);
  prefs.putUChar("pFade", sc.pFade);
  prefs.putUChar("nEn", sc.nEn);
  prefs.putUShort("nFrom", sc.nFrom);
  prefs.putUShort("nTo", sc.nTo);
  prefs.putUChar("nPct", sc.nPct);
  prefs.end();
}

static bool localNow(struct tm& t) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;  // o NTP ainda nao sincronizou
  localtime_r(&now, &t);
  return true;
}

// Fator de brilho da noite (1 = normal). Na janela [nFrom, nTo) (pode cruzar a
// meia-noite) o brilho percebido vai ate nPct%, com rampa suave nas bordas.
static float nightFactorAt(const struct tm& t) {
  if (!sc.nEn || sc.nFrom == sc.nTo) return 1.0f;
  float m = t.tm_hour * 60.0f + t.tm_min + t.tm_sec / 60.0f;
  float from = sc.nFrom, to = sc.nTo, since, until;
  if (from < to) {
    if (m < from || m >= to) return 1.0f;
    since = m - from;
    until = to - m;
  } else if (m >= from) {
    since = m - from;
    until = to + 1440.0f - m;
  } else if (m < to) {
    since = m + 1440.0f - from;
    until = to - m;
  } else {
    return 1.0f;
  }
  float s = fminf(1.0f, fminf(since, until) / (float)NIGHT_RAMP_MIN);
  return 1.0f - (1.0f - sc.nPct / 100.0f) * s;
}

static uint32_t lastA = 0, lastP = 0;
static void schedStep(const struct tm& t) {
  uint16_t m   = t.tm_hour * 60 + t.tm_min;
  uint32_t key = (uint32_t)t.tm_yday * 1440u + m + 1u;
  uint8_t  bit = (uint8_t)(1u << t.tm_wday);
  if (sc.aEn && m == sc.aMin && (sc.aDays & bit) && key != lastA) {
    lastA = key;
    autoWake(sc.aScene, sc.aRamp);
  }
  if (sc.pEn && m == sc.pMin && (sc.pDays & bit) && key != lastP) {
    lastP = key;
    autoSleep(sc.pFade);
  }
}

static uint32_t schedT = 0;
static void schedTick() {
  if (millis() - schedT < 1000) return;
  schedT = millis();
  struct tm t;
  float nf = 1.0f;
  if (localNow(t)) {
    schedStep(t);
    nf = nightFactorAt(t);
  }
  if (fabsf(nf - nightFactor) > 0.004f) {
    nightFactor = nf;
    applyAll();
  }
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
  ovTick();
  autoTick();
  schedTick();
  if (curMode == MODE_DEMO) demoTick();
  else if (curMode == MODE_TEST) testTick();
}

// ------------------------------------------------------------- Modos ----
static void setMode(Mode m) {
  ovClear();     // acao manual cancela o aviso do Claude
  autoCancel();  // e o acordar/dormir em andamento
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
  ovActive   = false;  // applyAll() ja atualiza as saidas
  autoKind   = AUTO_NONE;
  autoFactor = 1.0f;
  zeroColors();
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
    "Comandos: <r|g|b|ww|cw|all> 0-255 | limit 0-100 | off | manual | test | demo | status | claude ok|ask|busy|err|off [seg] | sleep min | hora";

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
  if (line == "hora") {
    struct tm t;
    if (localNow(t)) Serial.printf("Hora: %02d:%02d (dia da semana %d)\n", t.tm_hour, t.tm_min, t.tm_wday);
    else Serial.println("Sem hora (o NTP nao sincronizou: precisa de Wi-Fi com internet).");
    return;
  }

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
  } else if (name == "claude") {
    String rest = line.substring(sp + 1);
    rest.trim();
    int    sp2  = rest.indexOf(' ');
    String st   = sp2 < 0 ? rest : rest.substring(0, sp2);
    int    secs = sp2 < 0 ? -1 : (int)rest.substring(sp2 + 1).toInt();
    Serial.println(claudeSet(st, secs) ? "ok" : "estado desconhecido (ok|ask|busy|err|off)");
  } else if (name == "sleep") {
    int m = constrain(v, 0, 180);
    if (m == 0) autoCancel();
    else autoSleep((uint8_t)m);
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
.section-title{font-size:.78rem;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:#667386;margin:0 0 10px}
.presets{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}
.preset{position:relative;aspect-ratio:1.15/1;border:1px solid #dfe5ec;background:#fff;border-radius:14px;padding:6px;font:inherit;font-size:.84rem;font-weight:500;color:#354052;cursor:pointer;display:grid;place-items:center;text-align:center}.preset:active{transform:scale(.97)}
.dot{position:absolute;top:8px;right:8px;width:14px;height:14px;border-radius:50%;box-shadow:inset 0 0 0 1px #0002}
.preset.selected{border-color:#3478f6;box-shadow:inset 0 0 0 1px #3478f6;background:#eef4ff}
.manual-head{display:flex;justify-content:space-between;align-items:center;cursor:pointer}.manual-head button{border:0;background:none;color:#3478f6;font-weight:600;font-size:.9rem}.manual-body{margin-top:14px}.manual-body[hidden]{display:none}
label.sl{display:grid;grid-template-columns:105px 1fr 34px;gap:8px;align-items:center;margin:14px 0;font-size:.9rem}label.sl b{text-align:right;font-size:.78rem;color:#697586}input[type=range]{width:100%;accent-color:#3478f6}input[type=color]{width:100%;height:42px;border:0;border-radius:10px;padding:0;background:none;cursor:pointer}
.actions{display:flex;gap:8px}.actions button{flex:1;border:2px solid #d9e0e8;border-radius:11px;padding:11px 8px;background:#fff;color:#293444;font:inherit;font-weight:600;cursor:pointer}.actions .danger{background:#fff1f1;color:#c43131;border-color:#ffd3d3}.flag{display:inline-flex;align-items:center;gap:7px;padding:6px 12px;border-radius:999px;font-size:.8rem;font-weight:700;background:#eef1f5;color:#3b4757;border:2px solid #dfe5ec;white-space:nowrap}
.flag::before{content:"";width:9px;height:9px;border-radius:50%;background:#98a2b3}
.flag.claude{border-color:var(--c);background:#fff;color:#18202a}.flag.claude::before{background:var(--c)}.flag.off::before{background:#d33}
.cbar{margin:-6px 0 14px}.cbar[hidden]{display:none}.cinfo{display:flex;justify-content:space-between;font-size:.78rem;font-weight:600;color:#647080;margin-bottom:5px}
.ctrack{height:8px;border-radius:4px;background:#e3e8ef;overflow:hidden}.cfill{height:100%;width:100%;background:var(--c,#888);border-radius:4px;transition:width .2s linear}.cfill.open{animation:pulse 1.4s ease-in-out infinite}@keyframes pulse{50%{opacity:.35}}
@media(prefers-color-scheme:dark){:root{color:#edf2f7;background:#11161d;color-scheme:dark}.card,.preset,.actions button{background:#1b232d;border-color:#303b49}.preset{color:#e4eaf1}.preset.selected{background:#1b355e;border-color:#6aa3ff;box-shadow:inset 0 0 0 1px #6aa3ff}.dot{box-shadow:inset 0 0 0 1px #fff4}.flag{background:#1b232d;border-color:#303b49;color:#e4eaf1}.flag.claude{background:#1b232d;color:#edf2f7}.ctrack{background:#303b49}.cinfo{color:#9aa7b8}.actions .danger{background:#3b2023;color:#ff9b9b;border-color:#623337}}

.arow{display:flex;gap:8px;align-items:center;margin:4px 0 2px}.arow>span{flex:1;font-weight:600;font-size:.9rem}
select,input[type=time]{font:inherit;font-size:.9rem;padding:7px 8px;border:1px solid #d9e0e8;border-radius:10px;background:#fff;color:inherit;min-width:0}
.abtn{border:1px solid #d9e0e8;background:#fff;border-radius:10px;padding:8px 12px;font:inherit;font-size:.85rem;font-weight:600;color:#293444;cursor:pointer}
.ab{border-top:1px solid #e3e8ef;padding-top:12px;margin-top:14px}.ah{display:flex;justify-content:space-between;align-items:center;font-weight:700;font-size:.95rem}
input.sw{appearance:none;-webkit-appearance:none;width:44px;height:26px;border-radius:13px;background:#cfd6df;position:relative;cursor:pointer;transition:background .2s;margin:0;flex:none}
input.sw::after{content:"";position:absolute;top:3px;left:3px;width:20px;height:20px;border-radius:50%;background:#fff;transition:left .2s;box-shadow:0 1px 3px #0003}
input.sw:checked{background:#3478f6}input.sw:checked::after{left:21px}
.fr{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:10px;font-size:.85rem;color:#647080}.fr .sp{flex:1}
.days{display:flex;gap:5px;margin-top:10px}.days button{flex:1;height:34px;border:1px solid #d9e0e8;background:#fff;border-radius:10px;font:inherit;font-size:.8rem;font-weight:700;color:#647080;cursor:pointer;padding:0}
.days button.on{background:#3478f6;border-color:#3478f6;color:#fff}
.nrow{display:flex;align-items:center;gap:8px;margin-top:10px;font-size:.85rem;color:#647080}.nrow input[type=range]{flex:1}.nrow b{min-width:38px;text-align:right;color:#697586;font-size:.78rem}
#clock{margin-top:.25rem}
@media(prefers-color-scheme:dark){select,input[type=time],.abtn,.days button{background:#1b232d;border-color:#303b49;color:#e4eaf1}.days button.on{background:#3478f6;border-color:#3478f6;color:#fff}.ab{border-color:#303b49}input.sw{background:#3b4757}input.sw:checked{background:#3478f6}.fr,.nrow{color:#9aa6b6}}
</style></head><body>
<header><div><h1>Minha lâmpada</h1><p>Escolha uma cena ou ajuste manualmente</p></div><span id="flag" class="flag">Manual</span></header>
<div class="cbar" id="claudebar" hidden><div class="cinfo"><span id="cname"></span><span id="ctime"></span></div><div class="ctrack"><div class="cfill" id="cfill"></div></div></div>
<div class="cbar" id="autobar" hidden><div class="cinfo"><span id="aname"></span><span><span id="atime"></span> <button class="abtn" id="acancel" onclick="autoCancel()" style="padding:3px 9px;margin-left:6px"></button></span></div><div class="ctrack"><div class="cfill" id="afill"></div></div></div>
<section class="card"><h2 class="section-title">Cores prontas</h2><div class="presets" id="presets"></div></section>
<section class="card"><div class="manual-head" onclick="toggleManual()"><div><h2 class="section-title" style="margin:0">Ajuste manual</h2><p>RGB e temperatura do branco</p></div><button id="toggle">Mostrar</button></div><div class="manual-body" id="manual" hidden><input type="color" id="picker" aria-label="Escolher cor"><div id="sliders"></div></div></section>
<section class="card"><div class="manual-head" onclick="toggleAuto()"><div><h2 class="section-title" style="margin:0">Automação</h2><p id="clock">Carregando…</p></div><button id="atoggle">Mostrar</button></div><div class="manual-body" id="auto" hidden>
<div class="arow"><span>Modo dormir</span><select id="sl_min"><option value="10">10 min</option><option value="20" selected>20 min</option><option value="30">30 min</option><option value="60">60 min</option></select><button class="abtn" onclick="sleepNow()">Iniciar</button></div>
<div class="ab"><div class="ah"><span>Acender</span><input type="checkbox" class="sw" id="a_en" onchange="sched()"></div>
<div class="fr"><span>às</span><input type="time" id="a_t" onchange="sched()"><span>cena</span><select id="a_scene" onchange="sched()"></select></div>
<div class="fr"><span>subir em</span><select id="a_ramp" onchange="sched()"><option value="0">na hora</option><option value="5">5 min</option><option value="10">10 min</option><option value="20">20 min</option><option value="30">30 min</option></select></div>
<div class="days" id="a_days"></div></div>
<div class="ab"><div class="ah"><span>Apagar</span><input type="checkbox" class="sw" id="p_en" onchange="sched()"></div>
<div class="fr"><span>às</span><input type="time" id="p_t" onchange="sched()"><span>apagar em</span><select id="p_fade" onchange="sched()"><option value="0">na hora</option><option value="10">10 min</option><option value="20">20 min</option><option value="30">30 min</option></select></div>
<div class="days" id="p_days"></div></div>
<div class="ab"><div class="ah"><span>Brilho da noite</span><input type="checkbox" class="sw" id="n_en" onchange="sched()"></div>
<div class="fr"><span>das</span><input type="time" id="n_from" onchange="sched()"><span>às</span><input type="time" id="n_to" onchange="sched()"></div>
<div class="nrow"><span>Brilho</span><input type="range" id="n_pct" min="5" max="100" oninput="$('v_npct').textContent=this.value+'%';sched()"><b id="v_npct">30%</b></div></div>
</div></section>
<section class="card"><h2 class="section-title">Brilho máximo</h2><label class="sl"><span>Limite</span><input type="range" id="i_limit" min="0" max="100" oninput="changeLimit()"><b id="v_limit">0%</b></label></section>
<div class="actions"><button class="danger" onclick="off()">Apagar</button></div>
<script>
const N=['r','g','b','ww','cw'];
const labels={r:'Vermelho',g:'Verde',b:'Azul',ww:'Branco quente',cw:'Branco frio'};
const presets=[
['Festa',[255,0,120,0,0]],['Quente',[255,70,0,0,0]],['Limão',[150,255,0,0,0]],['Natureza',[20,255,80,0,0]],
['Água',[0,255,255,0,0]],['Céu',[0,90,255,0,0]],['Oceano',[0,100,255,0,0]],['Noite',[80,0,255,0,0]],
['Luz baixa',[100,40,0,25,0]],['Relaxar',[255,150,0,30,0]],['Leitura',[0,0,0,210,70]],['Neutro',[0,0,0,110,145]]];
const ovInfo={ok:['concluído','#22a355',[0,255,0]],ask:['precisa de você','#e6a100',[255,160,0]],busy:['trabalhando','#2f7bf6',[0,60,255]],err:['erro','#d33636',[255,0,0]]};
const $=id=>document.getElementById(id);
const el=k=>$('i_'+k);
let tCh,tLim,hold=0,ov=null;
const warm=[255,214,170],cool=[201,226,255];
function rgbOf(v){return [0,1,2].map(i=>Math.min(255,Math.round(v[i]+warm[i]*v[3]/255+cool[i]*v[4]/255)))}
$('presets').innerHTML=presets.map((p,i)=>`<button class="preset" onclick="applyPreset(${i})"><span class="dot" style="background:rgb(${rgbOf(p[1])})"></span>${p[0]}</button>`).join('');
$('sliders').innerHTML=N.map(k=>`<label class="sl"><span>${labels[k]}</span><input type="range" id="i_${k}" min="0" max="255" oninput="changeColor()"><b id="v_${k}">0</b></label>`).join('');
let key='';try{key=localStorage.getItem('lk')||''}catch(e){}
function api(q,retry){
  const u=q+(q.includes('?')?'&':'?')+'_='+Date.now()+(key?'&key='+encodeURIComponent(key):'');
  return fetch(u,{cache:'no-store'}).then(r=>{
    if(r.status===403&&!retry){
      const k=prompt('Chave de acesso da lâmpada:');
      if(!k)throw new Error('sem chave');
      key=k;try{localStorage.setItem('lk',k)}catch(e){}
      return api(q,true);
    }
    if(r.status===403){key='';try{localStorage.removeItem('lk')}catch(e){}}
    if(!r.ok)throw new Error(r.status);
    return r.json();
  });
}
function lab(k){$('v_'+k).textContent=k==='limit'?el(k).value+'%':el(k).value}
function offline(){const f=$('flag');f.className='flag off';f.textContent='Sem conexão';f.style.removeProperty('--c');$('claudebar').hidden=true;$('autobar').hidden=true;ov=null;autoI=null}
function touch(){hold=Date.now()+1500}
function markPreset(s){
  document.querySelectorAll('.preset').forEach((b,i)=>b.classList.toggle('selected',presets[i][1].every((v,j)=>v===s[N[j]])));
}
function localState(){const s={};N.forEach(k=>s[k]=+el(k).value);return s}
function paintColors(s){
  N.forEach(k=>{el(k).value=s[k];lab(k)});
  el('limit').value=s.limit;lab('limit');
  $('picker').value='#'+N.slice(0,3).map(k=>Number(s[k]).toString(16).padStart(2,'0')).join('');
  markPreset(s);
}
function paintFlag(s){
  const f=$('flag'),bar=$('claudebar');
  if(!s.ov){ov=null;f.className='flag';f.textContent='Manual';f.style.removeProperty('--c');bar.hidden=true;return}
  const i=ovInfo[s.ov]||[s.ov,'#888',[128,128,128]];
  f.className='flag claude';f.style.setProperty('--c',i[1]);f.textContent='Claude · '+i[0];
  bar.hidden=false;bar.style.setProperty('--c',i[1]);$('cname').textContent='Claude · '+i[0];
  const end=Date.now()+s.ovMs;
  if(!ov||ov.name!==s.ov||ov.total!==s.ovTotal||Math.abs(ov.end-end)>700)ov={name:s.ov,end:end,total:s.ovTotal};
  tickBar();
}
function tickBar(){
  if(!ov)return;
  const fill=$('cfill');
  if(!ov.total){fill.classList.add('open');fill.style.width='100%';$('ctime').textContent='sem prazo';return}
  fill.classList.remove('open');
  const left=Math.max(0,ov.end-Date.now());
  fill.style.width=(left/ov.total*100)+'%';
  $('ctime').textContent=Math.ceil(left/1000)+' s';
}
function show(s,force){
  if(force||Date.now()>=hold)paintColors(s);
  paintFlag(s);
  paintAuto(s);
}
function send(){clearTimeout(tCh);tCh=setTimeout(()=>api('/set?'+N.map(k=>k+'='+el(k).value).join('&')).then(s=>show(s)).catch(offline),70)}
function changeColor(){N.forEach(lab);touch();markPreset(localState());send()}
function changeLimit(){lab('limit');touch();clearTimeout(tLim);tLim=setTimeout(()=>api('/set?limit='+el('limit').value).then(s=>show(s)).catch(offline),70)}
function applyPreset(i){const v=presets[i][1];N.forEach((k,j)=>el(k).value=v[j]);changeColor()}
function off(){hold=Date.now()+2000;api('/off').then(s=>{hold=0;show(s,true)}).catch(()=>{hold=0;offline()})}
function toggleManual(){const x=$('manual');x.hidden=!x.hidden;$('toggle').textContent=x.hidden?'Mostrar':'Ocultar'}
$('picker').oninput=e=>{N.slice(0,3).forEach((k,i)=>el(k).value=parseInt(e.target.value.substr(1+i*2,2),16));changeColor()};

// ---------- Automação ----------
const DAYS=['D','S','T','Q','Q','S','S'];
['a','p'].forEach(p=>{$(p+'_days').innerHTML=DAYS.map((d,i)=>`<button type="button" data-b="${i}" onclick="this.classList.toggle('on');sched()">${d}</button>`).join('')});
$('a_scene').innerHTML=presets.map((p,i)=>`<option value="${i}">${p[0]}</option>`).join('')+'<option value="-1" disabled>Personalizada</option>';
let tSch,schedHold=0,sch=null,autoI=null,nightPct=100;
function getDays(p){let m=0;$(p+'_days').querySelectorAll('button').forEach(b=>{if(b.classList.contains('on'))m|=1<<(+b.dataset.b)});return m}
function setDays(p,m){$(p+'_days').querySelectorAll('button').forEach(b=>b.classList.toggle('on',!!(m&(1<<(+b.dataset.b)))))}
function setSel(id,v){const s=$(id);v=String(v);if(![...s.options].some(o=>o.value===v)){const o=document.createElement('option');o.value=v;o.textContent=v+' min';s.appendChild(o)}s.value=v}
function sched(){
  schedHold=Date.now()+4000;clearTimeout(tSch);
  tSch=setTimeout(()=>{
    const q=new URLSearchParams({
      a_en:$('a_en').checked?1:0,a_t:$('a_t').value||'07:00',a_d:getDays('a'),a_ramp:$('a_ramp').value,
      p_en:$('p_en').checked?1:0,p_t:$('p_t').value||'23:30',p_d:getDays('p'),p_fade:$('p_fade').value,
      n_en:$('n_en').checked?1:0,n_from:$('n_from').value||'22:00',n_to:$('n_to').value||'06:00',n_pct:$('n_pct').value});
    const si=+$('a_scene').value;if(si>=0)q.set('a_s',presets[si][1].join(','));
    api('/sched/set?'+q.toString()).then(s=>showSched(s,true)).catch(()=>{});
  },400);
}
function paintClock(){
  if(!sch)return;
  let t=sch.synced?'Hora no ESP: '+sch.time:'Sem hora (precisa de internet) · agenda inativa';
  if(sch.synced&&nightPct<100)t+=' · noite '+nightPct+'%';
  $('clock').textContent=t;
}
function showSched(s,force){
  sch=s;paintClock();
  if(!force&&Date.now()<schedHold)return;
  $('a_en').checked=!!s.a.en;$('a_t').value=s.a.t;setDays('a',s.a.d);setSel('a_ramp',s.a.ramp);
  const pi=presets.findIndex(p=>p[1].every((v,j)=>v===s.a.s[j]));$('a_scene').value=pi>=0?pi:-1;
  $('p_en').checked=!!s.p.en;$('p_t').value=s.p.t;setDays('p',s.p.d);setSel('p_fade',s.p.fade);
  $('n_en').checked=!!s.n.en;$('n_from').value=s.n.from;$('n_to').value=s.n.to;$('n_pct').value=s.n.pct;$('v_npct').textContent=s.n.pct+'%';
}
function loadSched(){api('/sched').then(s=>showSched(s)).catch(()=>{})}
function toggleAuto(){const x=$('auto');x.hidden=!x.hidden;$('atoggle').textContent=x.hidden?'Mostrar':'Ocultar'}
function sleepNow(){api('/sleep?min='+$('sl_min').value).then(s=>show(s,true)).catch(()=>{})}
function autoCancel(){api('/auto/cancel').then(s=>show(s,true)).catch(()=>{})}
function paintAuto(s){
  nightPct=s.night===undefined?100:s.night;paintClock();
  const bar=$('autobar');
  if(!s.auto){autoI=null;bar.hidden=true;return}
  const wake=s.auto==='wake',c=wake?'#f5a623':'#6b5bd6';
  bar.hidden=false;bar.style.setProperty('--c',c);
  $('aname').textContent=wake?'Acendendo aos poucos':'Modo dormir';$('acancel').textContent=wake?'Pular':'Cancelar';
  const end=Date.now()+s.autoMs;
  if(!autoI||autoI.wake!==wake||autoI.total!==s.autoTotal||Math.abs(autoI.end-end)>2500)autoI={wake:wake,end:end,total:s.autoTotal};
  tickAuto();
}
function tickAuto(){
  if(!autoI)return;
  const left=Math.max(0,autoI.end-Date.now()),f=autoI.total?left/autoI.total:0;
  $('afill').style.width=((autoI.wake?1-f:f)*100)+'%';
  const m=Math.floor(left/60000),sec=Math.floor(left/1000)%60;
  $('atime').textContent=m+':'+String(sec).padStart(2,'0');
}
loadSched();setInterval(loadSched,30000);setInterval(tickAuto,500);
api('/state').then(s=>show(s,true)).catch(offline);
setInterval(()=>api('/state').then(s=>show(s)).catch(offline),1000);
setInterval(tickBar,200);
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
  j += "\",\"ov\":\"";
  j += ovActive ? ovName : "";
  j += "\",\"ovMs\":";
  int32_t left = (ovActive && ovUntil) ? (int32_t)(ovUntil - millis()) : 0;
  j += (unsigned int)(left > 0 ? left : 0);
  j += ",\"ovTotal\":";
  j += (unsigned int)(ovActive ? ovTotal : 0);
  j += ",\"auto\":\"";
  j += autoKind == AUTO_WAKE ? "wake" : autoKind == AUTO_SLEEP ? "sleep" : "";
  uint32_t used  = millis() - autoStart;
  uint32_t aleft = (autoKind != AUTO_NONE && autoDur > used) ? autoDur - used : 0;
  j += "\",\"autoMs\":";
  j += (unsigned int)aleft;
  j += ",\"autoTotal\":";
  j += (unsigned int)(autoKind != AUTO_NONE ? autoDur : 0);
  j += ",\"night\":";
  j += (unsigned int)(nightFactor * 100.0f + 0.5f);
  j += "}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

// Confere a chave (comparacao em tempo constante). Sem API_KEY definida, libera tudo.
static bool authOk() {
  const char* want = API_KEY;
  size_t      n    = strlen(want);
  if (n == 0) return true;
  String  k    = server.arg("key");
  uint8_t diff = (k.length() != n);
  for (size_t i = 0; i < n; i++) diff |= (uint8_t)((i < k.length() ? k.charAt(i) : 0) ^ want[i]);
  if (!diff) return true;
  delay(300);  // freia tentativas em sequencia
  server.send(403, "text/plain", "chave invalida");
  return false;
}

static void handleOff() {
  if (!authOk()) return;
  allOff();
  sendState();
}

static void handleSet() {
  if (!authOk()) return;
  for (uint8_t i = 0; i < NUM_CH; i++)
    if (server.hasArg(ch[i].name)) setValue(i, server.arg(ch[i].name).toInt());
  if (server.hasArg("limit")) setLimit(server.arg("limit").toInt());
  sendState();
}

static bool parseHM(const String& str, uint16_t& out) {
  int h, m;
  if (str.length() != 5 || sscanf(str.c_str(), "%d:%d", &h, &m) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  out = (uint16_t)(h * 60 + m);
  return true;
}

static void sendSched() {
  struct tm t;
  bool      ok = localNow(t);
  char      hm[6] = "";
  if (ok) snprintf(hm, sizeof hm, "%02d:%02d", t.tm_hour, t.tm_min);
  char b[480];
  snprintf(b, sizeof b,
           "{\"time\":\"%s\",\"synced\":%s,"
           "\"a\":{\"en\":%u,\"t\":\"%02u:%02u\",\"d\":%u,\"ramp\":%u,\"s\":[%u,%u,%u,%u,%u]},"
           "\"p\":{\"en\":%u,\"t\":\"%02u:%02u\",\"d\":%u,\"fade\":%u},"
           "\"n\":{\"en\":%u,\"from\":\"%02u:%02u\",\"to\":\"%02u:%02u\",\"pct\":%u}}",
           hm, ok ? "true" : "false",
           (unsigned)sc.aEn, sc.aMin / 60u, sc.aMin % 60u, (unsigned)sc.aDays, (unsigned)sc.aRamp,
           (unsigned)sc.aScene[0], (unsigned)sc.aScene[1], (unsigned)sc.aScene[2],
           (unsigned)sc.aScene[3], (unsigned)sc.aScene[4],
           (unsigned)sc.pEn, sc.pMin / 60u, sc.pMin % 60u, (unsigned)sc.pDays, (unsigned)sc.pFade,
           (unsigned)sc.nEn, sc.nFrom / 60u, sc.nFrom % 60u, sc.nTo / 60u, sc.nTo % 60u,
           (unsigned)sc.nPct);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", b);
}

static void handleSchedSet() {
  if (!authOk()) return;
  uint16_t v;
  if (server.hasArg("a_en"))   sc.aEn = server.arg("a_en").toInt() != 0;
  if (server.hasArg("a_t") && parseHM(server.arg("a_t"), v)) sc.aMin = v;
  if (server.hasArg("a_d"))    sc.aDays = (uint8_t)constrain((int)server.arg("a_d").toInt(), 0, 127);
  if (server.hasArg("a_ramp")) sc.aRamp = (uint8_t)constrain((int)server.arg("a_ramp").toInt(), 0, 120);
  if (server.hasArg("a_s")) {
    unsigned c[NUM_CH];
    if (sscanf(server.arg("a_s").c_str(), "%u,%u,%u,%u,%u", &c[0], &c[1], &c[2], &c[3], &c[4]) == 5)
      for (uint8_t i = 0; i < NUM_CH; i++) sc.aScene[i] = (uint8_t)(c[i] > 255 ? 255 : c[i]);
  }
  if (server.hasArg("p_en"))   sc.pEn = server.arg("p_en").toInt() != 0;
  if (server.hasArg("p_t") && parseHM(server.arg("p_t"), v)) sc.pMin = v;
  if (server.hasArg("p_d"))    sc.pDays = (uint8_t)constrain((int)server.arg("p_d").toInt(), 0, 127);
  if (server.hasArg("p_fade")) sc.pFade = (uint8_t)constrain((int)server.arg("p_fade").toInt(), 0, 120);
  if (server.hasArg("n_en"))   sc.nEn = server.arg("n_en").toInt() != 0;
  if (server.hasArg("n_from") && parseHM(server.arg("n_from"), v)) sc.nFrom = v;
  if (server.hasArg("n_to") && parseHM(server.arg("n_to"), v))     sc.nTo = v;
  if (server.hasArg("n_pct"))  sc.nPct = (uint8_t)constrain((int)server.arg("n_pct").toInt(), 0, 100);
  schedSave();
  sendSched();
}

static void handleSleep() {
  if (!authOk()) return;
  int m = constrain((int)server.arg("min").toInt(), 0, 180);
  if (m == 0) autoCancel();
  else autoSleep((uint8_t)m);
  sendState();
}

static void handleAutoCancel() {
  if (!authOk()) return;
  autoCancel();
  sendState();
}

static void handleClaude() {
  if (!authOk()) return;
  int secs = server.hasArg("secs") ? constrain((int)server.arg("secs").toInt(), 0, 86400) : -1;
  if (!claudeSet(server.arg("state"), secs)) {
    server.send(400, "text/plain", "state invalido: ok|ask|busy|err|off");
    return;
  }
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
    configTzTime(TZ_POSIX, "pool.ntp.org", "time.google.com");  // hora para a agenda
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
#ifdef AP_PASS
    const char* apPass = AP_PASS;
#else
    static char apPass[11];
    static const char cs[] = "abcdefghjkmnpqrstuvwxyz23456789";
    for (uint8_t i = 0; i < 10; i++) apPass[i] = cs[esp_random() % (sizeof(cs) - 1)];
    apPass[10] = 0;
    Serial.printf("AP_PASS nao definida no secrets.h. Senha do ponto de acesso nesta sessao: %s\n", apPass);
#endif
    if (!WiFi.softAP(AP_SSID, apPass))
      Serial.println("ERRO: ponto de acesso nao iniciou (a senha precisa ter 8 a 63 caracteres).");
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
  schedLoad();

#if DEMO_ON_BOOT
  setMode(MODE_DEMO);
#endif

  startWifi();

  server.on("/", []() { server.send_P(200, "text/html", PAGE); });
  server.on("/set", handleSet);
  server.on("/state", sendState);
  server.on("/claude", handleClaude);
  server.on("/sched", sendSched);
  server.on("/sched/set", handleSchedSet);
  server.on("/sleep", handleSleep);
  server.on("/auto/cancel", handleAutoCancel);
  server.on("/off", handleOff);
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
