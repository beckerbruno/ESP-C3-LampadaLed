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
2. Copie `include/secrets.example.h` para `include/secrets.h` e preencha o Wi-Fi (sem ele o ESP cria o ponto de acesso `LampadaLed`, com uma senha sorteada que aparece na serial).
3. Compile e grave com PlatformIO:
   ```
   pio run -t upload
   pio device monitor
   ```
   Se o ESP não entrar em modo de gravação, segure BOOT, aperte RST e solte BOOT.
4. Na serial, rode `test`: cada canal acende um por vez, com o GPIO impresso (`manual` para sair).
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

A página web é só de controle manual. Os modos de diagnóstico (`test` e `demo`) existem apenas na serial; `manual` volta ao controle manual, e mexer em qualquer cor também volta.

## Controle

- **Navegador:** `http://lampada.local` (ou o IP mostrado na serial): cor, cada canal e limite de brilho.
- **HTTP:** `/set?r=255&g=0&b=80&ww=0&cw=0&limit=30`, `/off`, `/state`, `/claude?state=...`.
- **Serial (115200):** `r 128`, `g 0`, `b 255`, `ww 100`, `cw 100`, `all 0`, `limit 40`, `off`, `manual`, `test`, `demo`, `status`.

## Automação por horário

O ESP pega a hora na internet (NTP, fuso de Brasília) quando está no seu Wi-Fi. **Sem Wi-Fi/internet não há hora e a agenda fica parada** (no modo AP `LampadaLed` ela não roda). A agenda fica salva na memória do ESP e sobrevive a reinicializações. Tudo se configura no card "Automação" da página.

- **Acender (acordar aos poucos):** num horário e dias da semana, a luz sobe de 0 até a cena escolhida em 0–120 min.
- **Apagar:** num horário, apaga na hora ou diminui por 10/20/30 min.
- **Modo dormir:** botão que diminui o brilho aos poucos (10/20/30/60 min) e apaga. Sem nada aceso, não faz nada.
- **Brilho da noite:** entre dois horários o brilho cai para a % escolhida, com transição suave de 30 min nas bordas (vale também para os avisos do Claude).
- Qualquer ajuste manual cancela um acordar/dormir em andamento. A barra no topo mostra o progresso, com botão Cancelar/Pular.

Endpoints (exigem `key` se `API_KEY` estiver definida): `/sched`, `/sched/set?a_en=1&a_t=07:00&a_d=127&a_ramp=20&a_s=r,g,b,ww,cw&p_en&p_t&p_d&p_fade&n_en&n_from&n_to&n_pct`, `/sleep?min=20` (0 cancela), `/auto/cancel`. Serial: `sleep 20`, `hora`. Para outro fuso, mude `TZ_POSIX` em `main.cpp`.

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

- **HTTP:** `/claude?state=ok|ask|busy|err|off&secs=N` (`secs=0` = sem prazo; omita para usar o tempo padrão). Com `API_KEY` definida, acrescente `&key=...`.
- **Serial:** `claude ok`, `claude ask 30`, `claude off`.
- **Tempos:** defina `CLAUDE_OK_SECS`, `CLAUDE_ASK_SECS`, `CLAUDE_BUSY_SECS` e `CLAUDE_ERR_SECS` no `config.h`.
- **Hooks:** copie o bloco `"hooks"` de `claude-hooks.json` para `~/.claude/settings.json`. Se usar `API_KEY`, guarde a chave fora do repositório, no mesmo arquivo: `"env": { "LAMPADA_KEY": "sua-chave" }` (os hooks leem `$LAMPADA_KEY`). O Claude Code precisa estar na mesma rede Wi-Fi da lâmpada.

A página mostra, ao lado do título, uma etiqueta **Manual** ou **Claude · estado**. Quando há um aviso do Claude ativo, aparece também uma barra de progresso com o tempo restante, e a prévia mostra a cor do aviso. Sem prazo (`secs=0`), a barra pisca até o aviso ser cancelado.

## Segurança

- **Wi-Fi:** `include/secrets.h` guarda a rede e a senha e está no `.gitignore`. Nunca faça commit dele; antes de publicar, confira com `git ls-files | grep -i secrets` (deve listar só o `secrets.example.h`). Prefira uma rede de convidados ou de IoT.
- **Ponto de acesso de emergência:** defina `AP_PASS` (8 a 63 caracteres) no `secrets.h`. Sem ela, o ESP sorteia uma senha a cada boot e a mostra na serial. Não existe senha fixa no código.
- **Chave de acesso:** defina `API_KEY` no `secrets.h` e `/set`, `/off` e `/claude` passam a exigir `?key=...`. A página pede a chave na primeira vez e a guarda no navegador. A comunicação é HTTP simples: a chave protege contra uso casual, não contra quem escuta a rede.
- **Nunca** encaminhe a porta 80 do roteador para o ESP. Ele deve ficar acessível só dentro da sua rede.
- Quem tiver o aparelho fisicamente consegue ler o Wi-Fi gravado na memória dele pelo USB.
