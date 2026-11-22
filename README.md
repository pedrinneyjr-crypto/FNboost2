# FNBoost

Otimizador e macros para Fortnite em C++20 (Win32 + Dear ImGui + DirectX 11), tema escuro, 89 tweaks, backup automático e reversão.

> **Status honesto:** o código foi escrito e revisado à mão, mas **não foi compilado** (o ambiente onde foi gerado é Linux, sem MSVC). Se o build reclamar de algo, mande o log do compilador que a correção é rápida.

## Estrutura

| Arquivo | Função |
|---|---|
| `src/main.cpp` | Janela Win32, device D3D11, loop do ImGui |
| `src/ui.cpp/.hpp` | Interface: hardware, tweaks por categoria, macros (rolando para baixo), log |
| `src/hardware.cpp/.hpp` | CPU (CPUID + `GetLogicalProcessorInformation`), GPU/VRAM (DXGI), RAM (`GlobalMemoryStatusEx`), SO (registro), disco/SSD (`IOCTL_STORAGE_QUERY_PROPERTY`), Hz do monitor, caminho do `.ini` e do `.exe` |
| `src/tweaks.cpp/.hpp` | 89 tweaks, recomendação por PC, backup e reversão |
| `src/macros.cpp/.hpp` | Motor de macros (`SendInput`, `GetAsyncKeyState`, `std::thread`, `std::atomic`) e Kill Switch |
| `src/util.cpp/.hpp` | Registro, execução de comandos, log, admin |
| `CMakeLists.txt` | Baixa o ImGui v1.91.5 via FetchContent e gera `FNBoost.exe` |

## Como compilar (Windows)

**Visual Studio 2022 (recomendado)**

1. Instale o Visual Studio 2022 com a carga **Desenvolvimento para desktop com C++** e o **CMake** (já vem junto).
2. Abra um *Developer PowerShell for VS 2022* na pasta do projeto.
3. Execute:
   ```powershell
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   ```
   (O primeiro `cmake -S` precisa de internet para baixar o ImGui e o Git instalado.)
4. O executável fica em `build\Release\FNBoost.exe`.
5. Opcional: `-DFNBOOST_REQUIRE_ADMIN=ON` no primeiro comando faz o `.exe` pedir UAC ao abrir.

**MinGW-w64 (MSYS2 UCRT64)**

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja git
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Como testar com segurança

1. Crie um **ponto de restauração** do Windows antes do primeiro uso.
2. Abra o `.exe` **sem** administrador e use só tweaks do `.ini` e de usuário (HKCU). Confira o log.
3. Feche o Fortnite antes de mexer no `.ini` (ele pode sobrescrever o arquivo ao sair). O programa também salva uma cópia completa em `%APPDATA%\FNBoost\backups`.
4. Aplique **um tweak por vez**, teste uma partida no modo Criativo e só depois use "Aplicar TODOS os recomendados".
5. Para voltar atrás: botão **Reverter** em cada tweak ou **Reverter TUDO**. O estado anterior fica em `%APPDATA%\FNBoost\backup.tsv`.
6. Para testar as macros, use o modo Criativo ou um editor de texto. Mantenha a tecla de emergência (padrão `F12`) à mão.

## Tweaks (89 no total, com recomendação por PC)

**REAL** = altera `.ini`, registro ou executa comandos. **SIMULADO** = só mostra a orientação (exige painel do driver/kernel). **Ação única** = limpeza, sem desfazer. Cada tweak mostra o valor recomendado para o PC detectado; o que for arriscado ou inútil para o seu hardware aparece como "Não recomendado" e **nunca entra nos presets**.

| Categoria | O que tem |
|---|---|
| **Fortnite (.ini)** | RHI DX11/DX12, modo desempenho (ES31), resolução 3D, distância de visão, sombras, AA, texturas, efeitos, pós-processamento, folhagem, GI, reflexos, sombreamento, V-Sync, motion blur, limite de FPS, grama, FPS no menu, aceleração do mouse no jogo, travar `.ini` como somente leitura |
| **Resolução e tela esticada** | Resolução do jogo (nativa, 1680x1050, 1440x1080, 1280x960, 1024x768, 1280x1024...), modo de tela, resolução do Windows (área de trabalho), dimensionamento por GPU (simulado), criar resolução personalizada (simulado) |
| **CPU, threads e energia** | Plano de energia, Game Mode, Game Bar, Game DVR, tela cheia (global e só no exe), prioridade de CPU e de I/O do Fortnite, `Win32PrioritySeparation`, MMCSS, `SystemResponsiveness`, core parking, USB suspend, boost agressivo, estado mínimo 100%, resfriamento ativo, Power Throttling, inicialização rápida, apps em segundo plano, notificações, precisão do ponteiro, sensibilidade 6/11, Sticky Keys, fechar apps pesados, transparência, telemetria, tick dinâmico (opcional), HVCI (opcional/risco), exclusão do Defender (opcional/risco) |
| **GPU** | HAGS, PCIe ASPM, jogos em janela, GPU de alto desempenho (notebook), limpar cache de shaders, MPO (opcional), Reflex/Anti-Lag e energia máxima do driver (simulados) |
| **Memória e disco** | Working set, temporários, logs do Fortnite, WSearch, SysMain, retrim de SSD, NTFS last-access, compressão de memória, hibernação, cache do Windows Update, lixeira, cache do Epic Launcher |
| **Rede e latência** | Flush DNS, DNS (Cloudflare/Google/Quad9), NetworkThrottling, TCP autotuning, Nagle, energia do adaptador, ARP, Delivery Optimization, IPv4 preferencial, reset de Winsock, LSO |
| **Avançado (simulado)** | MSI mode/afinidade de interrupções, timer resolution/standby list |

### Presets

- **Competitivo**: valores recomendados para o seu PC.
- **FPS Máximo**: o mesmo, mas com qualidade mínima, resolução 3D 67%, DX11 e Modo Desempenho (ES31).

### Resolução esticada (4:3, 16:10)

1. *Resolução do jogo* grava a resolução no `.ini` (e *Modo de tela* em tela cheia exclusiva).
2. Se a resolução não aparece no Windows, crie-a no driver (tweak "Criar resolução personalizada") ou use *Resolução do Windows* quando o monitor já suporta o modo.
3. Para a imagem encher a tela (sem barras pretas), ligue o dimensionamento por GPU no painel NVIDIA/AMD (tweak simulado com o passo a passo).
4. Opcional: *Travar .ini como somente leitura* impede o jogo de voltar a resolução.

### Sobre "100 para 1000 FPS"

Isso não existe como promessa de software. Os tweaks tiram carga e estabilizam o frametime; o teto de FPS é do CPU/GPU. O ganho típico em PC fraco com qualidade mínima + resolução menor é grande (+30% a +100%), mas não é 10x.

Observação sobre as chaves do `.ini`: os nomes seguem o que o Fortnite costuma gravar, mas podem mudar entre versões. Se uma chave não existir, o programa a cria; confira no jogo se pegou.

## Como funcionam as otimizações

**`GameUserSettings.ini`** controla o que o engine renderiza: resolução 3D, sombras, efeitos, distância de visão. Reduzir isso diminui o trabalho da GPU (e parte da CPU) *dentro do jogo*. É a maior fonte de FPS.

**Windows (energia, prioridade, serviços)** não faz o jogo desenhar menos; faz o sistema atrapalhar menos. Plano de energia e core parking evitam que a CPU baixe clock ou "durma" entre rajadas; prioridade e MMCSS dão fatias de CPU mais previsíveis; desligar overlays e tarefas de fundo reduz interrupções. O efeito aparece principalmente nos **1% low** e no frametime, não no FPS médio.

**Rede**: DNS e flush ajudam na resolução de nomes (login, matchmaking). Ajustes de TCP quase não afetam o Fortnite, que joga em UDP. Nada aqui muda a rota do seu provedor, então não esperam milagres no ping.

**Por que frametime estável (1% low) importa mais que FPS máximo:** mira e edição dependem de intervalos regulares entre frames. 240 FPS com picos de 30 ms parecem piores que 144 FPS cravados, porque o input é amostrado de forma irregular.

**Backup e reversão:** antes de cada alteração, o valor antigo (chave do `.ini`, valor de registro ou comando de desfazer) é gravado em `%APPDATA%\FNBoost\backup.tsv`. "Reverter" restaura em ordem inversa. Limpezas (cache, temporários) são ações únicas e não têm reversão.

## Como funcionam as macros

1. Uma thread (`std::thread`) consulta `GetAsyncKeyState` a cada ~1 ms. Ela detecta a **borda** da tecla de disparo (apertou agora, não estava apertada antes) e só dispara de novo depois que você soltar a tecla.
2. A sequência é executada com `SendInput`, usando **scancodes** (os jogos costumam ler scancode/raw input) para teclado e eventos `MOUSEEVENTF_*` para o mouse. Um arrasto usa `MOUSEEVENTF_MOVE` (movimento relativo, afetado pela aceleração do ponteiro do Windows).
3. Cada pausa usa `QueryPerformanceCounter` + `timeBeginPeriod(1)` para ter precisão de ~1 ms.
4. **Por que o delay em ms importa:** o jogo só enxerga input a cada tick/frame. Se duas ações chegam mais rápido que isso, uma pode ser ignorada ou fundida (edição que não confirma, troca de slot que não acontece). O delay mínimo depende do seu FPS e ping; comece com 10-20 ms e reduza aos poucos.
5. **Por que o Kill Switch é crítico:** uma macro que trava com uma tecla "para baixo" deixa o personagem andando, atirando ou editando sozinho. O Kill Switch (padrão `F12`, configurável; botão "PARAR TUDO AGORA") é verificado **dentro de cada espera**, aborta a sequência, **solta todas as teclas e botões** que o programa pressionou e desativa os três macros até você clicar em "Rearmar".
6. Os três macros: **Ghost bind** (A → B, opcional volta a A, repetições), **Double edit** (editar → selecionar → confirmar → resetar → editar → selecionar → confirmar, com delay por etapa) e **Drag edit** (tecla de editar → espera → segura LMB → espera assentar → move X/Y em passos → fica parado → solta → confirma).
7. **Por que a macro "às vezes não funciona":** o jogo lê o input uma vez por frame. Antes, a tecla ficava pressionada por 8 ms, menos que 1 frame a 100 FPS (10 ms), e o drag começava a mover o mouse no mesmo instante do clique. Agora, com *Sincronizar com o FPS do jogo* ligado, a tecla fica pressionada por pelo menos 1,5 frame, toda pausa dura pelo menos 1 frame, e o drag espera o clique "assentar" antes de mover e antes de soltar. Ajuste "FPS que você faz no jogo" para o valor real. Use "Testar em 3 s" para validar no Criativo.

### Avisos objetivos

- **Regras e anti-cheat:** automação de input que executa várias ações com um único aperto pode violar os Termos/regras da Epic e ser detectada pelo Easy Anti-Cheat (eventos de `SendInput` são marcados como "injetados"). O risco de suspensão ou banimento é seu. Use em Criativo/offline e para acessibilidade ou teste.
- Se o Fortnite estiver como administrador e o FNBoost não, o Windows (UIPI) bloqueia o input. Rode os dois no mesmo nível.
- Antes de mexer em registro/serviços, crie um ponto de restauração.
