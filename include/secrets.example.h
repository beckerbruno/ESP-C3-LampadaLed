#pragma once
// Copie este arquivo para secrets.h (ele esta no .gitignore) e preencha.
// NUNCA faca commit do secrets.h.

// Wi-Fi (somente 2,4 GHz). Sem secrets.h o ESP sobe direto em modo ponto de acesso.
#define WIFI_SSID "SUA_REDE"
#define WIFI_PASS "SUA_SENHA"

// Ponto de acesso de emergencia (usado quando nao conecta no Wi-Fi).
// Se AP_PASS nao for definida, o ESP sorteia uma senha a cada boot e a mostra na serial.
// A senha precisa ter de 8 a 63 caracteres.
// #define AP_SSID "LampadaLed"
// #define AP_PASS "troque-esta-senha"

// Chave de acesso (opcional, recomendada). Se definida, /set, /off e /claude exigem ?key=...
// A pagina pede a chave na primeira vez e a guarda no navegador. Use algo longo e aleatorio.
// #define API_KEY "uma-chave-longa-e-aleatoria"
