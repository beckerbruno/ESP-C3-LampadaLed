# ESP-C3-LampadaLed

Firmware para controlar a lâmpada LED (painel M140-RGB-DB: R, G, B, branco quente e branco frio) com um **ESP32-C3 Super Mini** soldado na placa antiga (FH-RGB-5J-KZB). O MCU original é removido; o ESP envia PWM para o gate dos 5 transistores da placa. A corrente dos LEDs sai da bateria 18650 e **não passa pelo ESP**.

## Ligações (fase 1, ESP pelo USB)

| GPIO do ESP | Vai para | Canal (confirmado no teste) |
|---|---|---|
| 7  | gate do transistor do vermelho | R |
| 6  | gate do transistor do verde | G |
| 10 | gate do transistor do azul | B |
| 4  | gate do transistor do branco quente | WW |
| 5  | gate do transistor do branco frio | CW |
| G  | pad BAT− da placa antiga | GND comum (obrigatório) |

- Não ligue o pino 5V do ESP em nada da placa na fase 1.
- Se alguma cor sair trocada, ajuste os pinos em `include/config.h`.

## Primeiro uso

1. Remova o MCU de 16 pinos da placa antiga e confirme o GND comum.
2. Copie `include/secrets.example.h` para `include/secrets.h` e preencha o Wi-Fi (sem ele o ESP cria o ponto de acesso `LampadaLed`, senha `lampada1234`).
3. Compile e grave com PlatformIO:
   ```
   pio run -t upload
   pio device monitor
   ```
   Se o ESP não entrar em modo de gravação, segure BOOT, aperte RST e solte BOOT.
4. Na serial, rode `test` (ou use o botão "Teste dos canais" na página): cada canal acende um por vez, com o GPIO impresso.
5. **Limite de brilho:** começa em 25% (`DEFAULT_LIMIT_PCT`). Meça a corrente do painel com o multímetro em série e confira se os transistores ficam só mornos antes de subir com `limit 50`, `limit 100`.

Se preferir a Arduino IDE: copie `src/main.cpp` para um sketch `.ino` junto com a pasta `include/`, escolha a placa "ESP32C3 Dev Module" e ligue "USB CDC On Boot".

## Teste de todas as cores

Para conferir a montagem sem usar o Wi-Fi, grave o ambiente `teste_cores`:

```
pio run -e teste_cores -t upload
pio device monitor
```

Ele já começa percorrendo as cores, uma a cada 2 s, e imprime na serial o que está aceso e em qual GPIO:

```
[1/10] Vermelho       r=255(GPIO7)  limite 25%
[2/10] Verde          g=255(GPIO6)  limite 25%
...
[10/10] Branco WW+CW  ww=255(GPIO4) cw=255(GPIO5)  limite 25%
```

Os 5 primeiros passos acendem um canal isolado. Se a cor que acendeu não for a que a serial anunciou, troque os pinos em `include/config.h`. Depois vêm as misturas (amarelo, ciano, magenta, laranja e os dois brancos juntos).

No firmware normal há três modos, um por vez: **Manual**, **Teste dos canais** e **Demo de cores**. Na página web o botão do modo ativo fica destacado; clicar de novo no botão ativo (ou em "Manual") volta ao controle manual, que restaura as cores que estavam nos sliders. Fora do modo Manual os sliders ficam desabilitados. Na serial: `test`, `demo` e `manual` (`demo off` e `test off` também voltam ao manual). Mexer em qualquer cor manualmente também volta ao modo Manual. O tempo de cada cor da demo fica em `DEMO_STEP_MS`, no `config.h`.

## Controle

- **Navegador:** `http://lampada.local` (ou o IP mostrado na serial): cor, cada canal e limite de brilho.
- **HTTP:** `/set?r=255&g=0&b=80&ww=0&cw=0&limit=30`, `/off`, `/state` (inclui `"mode"`), `/mode?m=manual|test|demo`, `/test`, `/demo`.
- **Serial (115200):** `r 128`, `g 0`, `b 255`, `ww 100`, `cw 100`, `all 0`, `limit 40`, `off`, `manual`, `test`, `demo`, `status`.

## Fase 2 (bateria)

O ESP passa a ser alimentado pelo + da bateria, passando por um diodo Schottky (1N5819 ou SS14) até o pino 5V. O diodo impede que o USB do ESP empurre corrente para dentro da célula. O ESP não carrega a bateria: a carga continua com o IC da placa antiga.

## Aviso do Claude Code (hooks)

O ESP mostra o estado do Claude Code por cima da cor atual, por tempo limitado. Quando o aviso termina, volta sozinho o que estava antes (cor manual, demo, teste ou apagada). Qualquer ação manual (slider, modo, Apagar) cancela o aviso.

| Estado | Cor | Tempo padrão | Quando |
|---|---|---|---|
| `ok` | verde | 20 s | Claude terminou (hook `Stop`) |
| `ask` | amarelo | 120 s | precisa de permissão ou resposta (hook `Notification`) |
| `busy` | azul | 300 s | trabalhando (hooks `UserPromptSubmit` e `PostToolUse`) |
| `err` | vermelho | 30 s | erro (uso manual) |
| `off` | — | — | cancela o aviso |

- **HTTP:** `/claude?state=ok|ask|busy|err|off&secs=N` (`secs=0` = sem prazo; omita para usar o tempo padrão). Se definir `CLAUDE_KEY` no `config.h`, acrescente `&key=...`.
- **Serial:** `claude ok`, `claude ask 30`, `claude off`.
- **Tempos e chave:** defina `CLAUDE_OK_SECS`, `CLAUDE_ASK_SECS`, `CLAUDE_BUSY_SECS`, `CLAUDE_ERR_SECS` e `CLAUDE_KEY` no `config.h`.
- **Hooks:** copie o bloco `"hooks"` de `claude-hooks.json` para `~/.claude/settings.json` (ou `.claude/settings.json` do projeto). O Claude Code precisa estar na mesma rede Wi-Fi da lâmpada.
