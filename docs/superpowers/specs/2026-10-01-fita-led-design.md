# Fita de LED de status — design

**Data:** 2026-10-01
**Estado:** implementado (branch feat/fita-led)

## Problem Statement

O painel mostra o estado dos agentes pelo avatar e pelo sino da home, mas só
quem está olhando para a tela percebe. Com o painel de lado, atrás do monitor
ou com a tela bloqueada, um agente bloqueado esperando ação passa despercebido,
e um agente que terminou fica parado até alguém lembrar de conferir. Falta um
sinal de luz que se leia de relance, à distância, sem desbloquear nem focar na
tela.

## Solution

Uma fita de 7 LEDs endereçáveis ligada ao conector P3 do painel reflete o
**mesmo estado global do avatar**, com uma cor e uma animação por estado:
vermelho pulsando rápido quando algum agente precisa de ação, ciano respirando
quando algum terminou, âmbar em movimento enquanto trabalham, verde respirando
devagar quando está tudo ocioso e um único ponto branco fraco piscando quando
não há host online. A fita continua viva com a tela bloqueada, pode ser
dimmada ou apagada por um slider no menu e faz um autoteste de cores ao ligar.

## User Stories

1. Como usuário do painel, quero que a fita fique vermelha pulsando rápido quando algum agente estiver bloqueado, para perceber de longe que preciso agir.
2. Como usuário, quero que o pulso vermelho siga o ritmo "pulsa-pulsa-pausa" do sino da tela, para que luz e tela contem a mesma história.
3. Como usuário, quero que a fita fique âmbar com um cometa correndo de ponta a ponta enquanto agentes trabalham, para saber que há trabalho em andamento sem ler a tela.
4. Como usuário, quero que a fita respire em ciano quando algum agente terminou e ninguém abriu o pane, para saber que há resultado esperando revisão.
5. Como usuário, quero que a fita respire devagar em verde quando tudo está ocioso, para confirmar que o painel está vivo e sem pendências.
6. Como usuário, quero que, sem nenhum host online, só um LED branco fraco pisque a cada ~3 s, para distinguir "sem host" de "painel desligado ou travado".
7. Como usuário, quero que a fita mostre o pior estado entre todos os agentes, na mesma prioridade do avatar (desconectado > bloqueado > finalizado > trabalhando > ocioso), para que fita e avatar nunca discordem.
8. Como usuário, quero que a troca de estado seja instantânea, para que um bloqueio chame atenção no momento em que acontece.
9. Como usuário, quero que a fita continue mostrando o estado com a tela bloqueada, para ler o status sem desbloquear.
10. Como usuário, quero um slider "LED" em Configurações → Dispositivo (0–100%, passos de 5%), para baixar a fita à noite ou apagá-la em 0 sem desconectar o cabo.
11. Como usuário, quero que o slider venha em 100% de fábrica, para que a fita funcione assim que for conectada.
12. Como usuário, quero que o nível sobreviva a reinício e a atualização OTA, para não reconfigurar a cada boot.
13. Como usuário, quero que o nível mude na hora enquanto arrasto, sem reiniciar o painel, e que em níveis baixos todo estado continue visível (só o 0 apaga).
13a. Como usuário, quero um slider "Brilho da tela" (10–100%) logo acima, para dimmar o backlight; o piso evita uma tela preta sem caminho de volta.
14. Como usuário, quero que a fita acenda vermelho, verde e azul por ~200 ms cada ao ligar o painel, para confirmar que os 7 LEDs estão vivos e que a ordem das cores está certa.
15. Como usuário, quero que o brilho fique limitado a um teto seguro, para que os 7 LEDs no regulador de 3.3V não resetem o painel.
16. Como mantenedor, quero o teto de brilho num único valor no código, para calibrar se a fita ficar fraca ou forte demais.
17. Como usuário, quero cores saturadas no mesmo tom da paleta da tela, para que a fita combine com o avatar e os cards sem sair esbranquiçada.
18. Como usuário sem fita conectada, quero que o painel funcione exatamente como antes, para que o recurso não custe nada a quem não o usa.
19. Como usuário, quero instruções no README (inglês e português) de como ligar a fita no P3 e conferir o cabo com multímetro, para não inverter VCC e GND e queimar a fita.
20. Como usuário, quero no README a tabela de estado → cor → animação, para saber o que cada luz significa.
21. Como mantenedor, quero que a lógica de cor/animação seja testável no host, sem placa, para mexer nas animações sem regredir.
22. Como mantenedor, quero que a animação rode fora da task da LVGL, para que a fita não engasgue a UI nem seja engasgada por ela.

## Implementation Decisions

- **Hardware:** 7 LEDs WS2812B (ordem GRB presumida, confirmada pelo autoteste) alimentados em 3.3V pelo P3: pino 1 GND, pino 2 3.3V, pino 3 IO17 como dado. IO18 (pino 4) fica sem uso. 3.3V dispensa level shifter. O P4 (SH 1,0 mm) tem a mesma pinagem no esquemático e também serve.
- **Módulo novo de efeito (puro):** função de renderização que recebe o estado do avatar e o tempo em ms e preenche 7 cores RGB já com o teto de brilho aplicado. C11 puro, sem ESP-IDF nem LVGL. É todo o comportamento visível e o único seam de teste.
- **Módulo novo de driver da fita:** dono do RMT no IO17, de uma task própria que chama o render a ~50 Hz e envia para a fita, do autoteste no boot e do liga/desliga. Interface mínima: iniciar, definir o estado, ligar/desligar.
- **Driver de LED:** componente oficial `espressif/led_strip` (backend RMT) pelo gerenciador de componentes do ESP-IDF, mantido pela Espressif, sem vendor de terceiros (preferido a um encoder RMT próprio).
- **Origem do estado:** o mesmo ponto que já decide o estado do avatar passa a avisar também a fita, com o mesmo `avatar_state_t`. O estado é publicado da task da LVGL e lido pela task da fita como um único valor atômico; troca é instantânea, sem fade.
- **Tabela de estados:**

  | Estado | Cor (saturada) | Animação |
  |---|---|---|
  | BLOCKED | vermelho | fita inteira, dois pulsos rápidos e pausa (≈ ciclo de 1,5 s, ritmo do sino) |
  | DONE | ciano | respiração, ciclo ~2 s |
  | WORKING | âmbar | cometa com rastro, ~1 volta/s |
  | IDLE | verde | respiração lenta, ciclo ~6 s |
  | DISCONNECTED | branco fraco | um único LED pisca a cada ~3 s |

- **Brilho:** teto fixo num único `#define` (= 100% do slider); o render recebe o nível 0..100 e escala por cima dele, com piso de 1/255 em todo canal aceso.
- **Sliders:** LED (0–100%, 0 apaga) e Brilho da tela (10–100%, PWM do backlight), em passos de 5%, cada um com chave própria na NVS (`ledstrip/level`, `backlight/level`; ausente = 100), no mesmo padrão do lockscreen — a configuração persistente do painel só vale após reiniciar e é compartilhada com o Cardputer, então não serve. Aplicam na hora ao arrastar e gravam na NVS ao soltar. Substituem o toggle da primeira versão (a chave `ledstrip/off` deixou de ser lida).
- **Brilho calibrado no hardware:** teto final 200 (~78%); o pior estado (ciano na fita inteira) fica em ~160 mA.
- **Lockscreen:** a fita ignora o bloqueio de tela; só o slider em 0 a apaga.
- **Autoteste:** no boot, R, G, B por ~200 ms cada em todos os LEDs, depois entra no estado corrente. Roda no nível do slider; em 0, não acende.
- **Escopo de alvo:** só o painel JC3248W535EN. Nada entra na lista do `sync_shared.py` do Cardputer.

## Testing Decisions

- **Seam único:** o render puro. Um bom teste verifica a saída (cores dos 7 LEDs) para estado e tempo dados: cor dominante por estado, teto de brilho nunca excedido, DISCONNECTED com só um LED aceso no pico e todos apagados fora dele, BLOCKED com fita inteira acesa no pulso e apagada na pausa, WORKING com o ponto mais brilhante avançando com o tempo, periodicidade (t e t + período dão a mesma saída). Não testa curvas exatas nem valores internos.
- **Prior art:** testes de host em C dos módulos puros, rodados no job `host-test` do CI (`term_parse_test`, `money_test`, `limits_merge_test`): um `*_test.c` com `CHECK`, compilado com `cc` junto do `.c` do módulo. O novo teste segue o mesmo molde e entra no CI.
- **Hardware:** driver, task e sliders são validados à mão no painel novo (gravação USB do `update.bin`, preservando a NVS): autoteste no boot, cada um dos 5 estados, slider dimmando e apagando em 0 sem reboot, brilho da tela em 10% legível, lockscreen ativo.
- **Build:** `pio run` do painel e do Cardputer verdes, para provar que o Cardputer não foi afetado.

## Out of Scope

- Um LED por agente ou por host; a fita mostra só o estado global.
- Escolha de cores ou de animações pelo usuário.
- Fade entre estados.
- Fita no Cardputer (ele não tem P3; o LED interno dele também fica de fora).
- Fitas de outros tamanhos ou pino configurável; 7 LEDs e IO17 são constantes.
- Alimentação em 5V e level shifter.
- Estados especiais durante OTA, pareamento ou boot além do autoteste.
- Publicar release (v0.12.0); fica para decisão posterior.

## Further Notes

- O chip do painel novo é ESP32-S3 rev v0.2 com 16 MB de flash; o firmware
  não usa nenhum GPIO do P3 hoje.
- 7 LEDs no branco máximo puxariam ~420 mA do mesmo regulador do ESP32 e do
  display; o teto de brilho existe por isso, não por estética.
- Se o autoteste mostrar as cores trocadas (ex.: vermelho saindo verde), a fita
  é RGB e não GRB: troca-se a ordem na configuração do driver.
