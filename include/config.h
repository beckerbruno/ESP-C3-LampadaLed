#pragma once

// ---------------------------------------------------------------------------
// Pinos do ESP32-C3 Super Mini -> ponto de gate de cada transistor da placa antiga
// (lado do resistor de 100 ohm onde antes ligava o MCU).
//
// Mapeamento confirmado no teste com a placa real.
// Rode "test" na serial (ou /test no navegador) e veja qual cor acende; se estiver
// trocado, basta inverter os numeros aqui. Nao precisa mexer na solda.
// ---------------------------------------------------------------------------
#define PIN_R   7    // vermelho (confirmado no teste)
#define PIN_G   6    // verde (confirmado no teste)
#define PIN_B   10   // azul (confirmado no teste)
#define PIN_WW  4    // branco quente (confirmado no teste)
#define PIN_CW  5    // branco frio (confirmado no teste)

// PWM
#define PWM_FREQ_HZ  25000   // acima do audivel e sem cintilar em camera
#define PWM_BITS     10      // 0..1023 (12 bits nao cabe: o clock do LEDC no C3 e 40 MHz, e a 10 kHz o maximo sao 11 bits)
#define GAMMA_EXP    2.0f    // correcao simples de brilho percebido

// Limite de brilho (% do duty maximo). Comece baixo ate medir a corrente do painel
// e confirmar que os transistores da placa aguentam. Ajustavel em tempo de execucao.
#define DEFAULT_LIMIT_PCT  25
#define MAX_LIMIT_PCT      100

// Demo de cores (comando "demo" na serial, /demo na web)
#define DEMO_STEP_MS  2000   // tempo de cada cor
#ifndef DEMO_ON_BOOT
#define DEMO_ON_BOOT  0      // 1 = comeca a demo ao ligar (o env teste_cores faz isso)
#endif

// Rede
#define HOSTNAME  "lampada"        // acessivel em http://lampada.local
#define AP_SSID   "LampadaLed"     // usado se nao conectar no Wi-Fi
#define AP_PASS   "lampada1234"
