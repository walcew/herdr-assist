// Mod de controle do herdr-assist, em duas formas:
// - indicador: a cópia que o mod.json encaixa no layout ("place") na 1ª vez
//   que o Herdr vê a pasta. Vive enquanto o layout mostra o bloco, então é ela
//   quem garante a Ponte ao abrir o app; o clique abre o dialog.
// - dialog: `mod.open herdr-assist` (a tecla do keys.json chama o mesmo
//   comando) abre o overlay que o mod.json descreve, uma cópia nova a cada vez,
//   com as abas Status (Reiniciar e aviso de porta ocupada), Token (mostrar e
//   rotacionar) e Parear (descoberta na LAN, envio da configuração e QR do manual).
// O JS só desenha; as operações são o ctl.py desta pasta, por exec.

import { View, div, image } from "gpui-kit";
import { v_flex, h_flex } from "gpui-base";
import { exec, appHome, theme, props, run, terminals } from "herdr";

// chave: [en, pt]
const STRINGS = {
  bridge: ["Bridge", "Ponte"],
  "bridge.up": ["running ({t}, pid {pid})", "ativa ({t}, pid {pid})"],
  "bridge.down": ["stopped", "parada"],
  version: ["Version", "Versão"],
  "version.pending": ["{run} running, {disk} on disk: restart to apply",
    "{run} rodando, {disk} em disco: reinicie para aplicar"],
  port: ["Port", "Porta"],
  ip: ["LAN IP", "IP na LAN"],
  panels: ["Panels", "Painéis"],
  "panels.some": ["{n} connected: {ips}", "{n} conectado(s): {ips}"],
  "panels.none": ["none connected", "nenhum conectado"],
  restart: ["Restart bridge", "Reiniciar ponte"],
  restarting: ["restarting the bridge...", "reiniciando a ponte..."],
  busy: ["Port {port} is taken by another bridge (probably the Herdr TUI one): pid {pid}, {cmd}.",
    "Porta {port} ocupada por outra ponte (provavelmente a do Herdr TUI): pid {pid}, {cmd}."],
  stop: ["Stop it", "Encerrar"],
  "stop.ask": ["Stop process {pid}? Panels connected to it will drop.",
    "Encerrar o processo {pid}? Os painéis ligados a ele caem."],
  confirm: ["Confirm", "Confirmar"],
  cancel: ["Cancel", "Cancelar"],
  "tab.status": ["Status", "Status"],
  "tab.token": ["Token", "Token"],
  "tab.pair": ["Pair", "Parear"],
  token: ["Token", "Token"],
  "token.none": ["no token yet: start the bridge", "sem token ainda: suba a ponte"],
  show: ["Show", "Mostrar"],
  hide: ["Hide", "Ocultar"],
  rotate: ["Rotate token", "Rotacionar token"],
  "rotate.ask": ["Every paired panel stops connecting until it is paired again. Rotate?",
    "Todos os painéis pareados param de conectar até serem pareados de novo. Rotacionar?"],
  "rotate.done": ["New token saved; the bridge already uses it.", "Token novo gravado; a ponte já usa ele."],
  "pair.where": ["On the panel: Settings → Pair with a host", "No painel: Configurações → Parear com um host"],
  "pair.listening": ["listening... {n} found", "escutando... {n} encontrado(s)"],
  "pair.check": ["Check the code shown on the panel screen.", "Confira o código mostrado na tela do painel."],
  pair: ["Pair", "Parear"],
  sending: ["sending the configuration...", "enviando a configuração..."],
  "pair.ok": ["Paired with {id}: the panel restarts and connects to this host.",
    "Pareado com {id}: o painel reinicia e conecta neste host."],
  manual: ["Pairing manual", "Manual de pareamento"],
  rotating: ["rotating the token...", "rotacionando o token..."],
};

export default class HerdrAssist extends View {
  init(_props, cx) {
    this.dir = appHome() + "/mods/herdr-assist";
    this.st = null;
    this.busy = false;
    this.askPid = null;
    this.error = "";
    this.tab = "status";
    this.busyKey = "restarting";
    this.note = "";        // resultado da última ação (rotação, pareamento)
    this.token = null;
    this.showToken = false;
    this.askRotate = false;
    this.found = [];       // painéis da última escuta
    this.scanning = false;
    this.scanError = "";
    this.ensuredAt = 0;
    // Toda cópia garante a Ponte. Viva/parada vem do terminals(), ao vivo; o
    // status (painéis, porta ocupada) é poll: o indicador segue com o timer
    // mesmo oculto, então consulta devagar; o dialog só vive enquanto aberto.
    this.ensure(cx);
    cx.timer.every(props()?.overlay ? 3000 : 60000, (cx) => {
      if (this.tab === "pair") cx.spawn(async (cx) => this.scan(cx));
      cx.spawn(async (cx) => this.refresh(cx));
    });
  }

  // Sobe a Ponte se ela sumiu; no máximo uma vez a cada 30 s, porque o render
  // que chama isto roda a cada mudança de terminal e a subida leva segundos.
  ensure(cx) {
    if (Date.now() - this.ensuredAt < 30000) return;
    this.ensuredAt = Date.now();
    cx.spawn(async (cx) => {
      await this.ctl("ensure");
      await this.refresh(cx);
    });
  }

  // Ponte viva segundo o core, sem esperar o poll.
  bridgeAlive() {
    // Só a Ponte desta máquina: o core também espelha a de outras (`maquina/t2b`).
    return terminals().some((t) => t.labels?.["herdr-assist"] === "bridge" && t.state === "live"
      && !String(t.address ?? "").includes("/") && !t.machine);
  }

  // Uma escuta de ~2,5 s; o timer emenda a próxima enquanto a aba Parear estiver aberta.
  async scan(cx) {
    if (this.scanning || this.busy) return;
    this.scanning = true;
    const r = await this.ctl("discover");
    this.scanning = false;
    this.found = r.panels ?? [];
    this.scanError = r.error ?? "";
    cx.notify();
  }

  open(tab, cx) {
    this.tab = tab;
    this.note = "";
    this.error = "";
    this.askRotate = false;
    cx.notify();
    if (tab === "token") cx.spawn(async (cx) => { this.token = (await this.ctl("token")).token ?? ""; cx.notify(); });
    if (tab === "pair") cx.spawn(async (cx) => this.scan(cx));
  }

  async ctl(...args) {
    try {
      const r = await exec("python3", [this.dir + "/ctl.py", ...args], { cwd: this.dir, timeout: 20000 });
      return JSON.parse(r.stdout);
    } catch (e) {
      return { error: "ctl.py " + args[0] + ": " + e };
    }
  }

  async refresh(cx) {
    if (this.busy) return;
    const st = await this.ctl("status");
    if (st.error) this.error = st.error;
    else this.st = st;
    cx.notify();
  }

  // Ação longa: trava o poll, roda, mostra o erro (ou a nota de sucesso) e relê o status.
  act(cx, work, busyKey = "restarting") {
    this.busy = true;
    this.busyKey = busyKey;
    this.error = "";
    this.note = "";
    cx.notify();
    cx.spawn(async (cx) => {
      const r = await work();
      this.error = r.error ?? "";
      this.note = r.note ?? "";
      this.busy = false;
      await this.refresh(cx);
    });
  }

  t(key, vars = {}) {
    const s = STRINGS[key][this.st?.lang === "pt" ? 1 : 0];
    return s.replace(/\{(\w+)\}/g, (_, k) => String(vars[k] ?? ""));
  }

  // Chip: ponto (viva, parada, porta ocupada por outra), nome e nº de painéis.
  indicator(th, st, alive) {
    const color = !st ? th.unknown : st.holder && !st.holder.is_bridge ? th.blocked : alive ? th.done : th.idle;
    return h_flex().id("herdr-assist").size_full().bg(th.chrome).text_size(12).child(
      h_flex().id("chip").h_full().px(10).gap(6).items_center().cursor_pointer()
        .hover((s) => s.bg(th.hover)).on_click(() => run("mod.open", { name: "herdr-assist" }))
        .child(div().size(8).rounded(4).bg(color))
        .child(div().text_color(th.foreground).child("herdr-assist"))
        .child(div().text_color(th.dim).child(st ? "· " + st.panels.length : "")));
  }

  render(cx) {
    const th = theme();
    const st = this.st;
    if (!props()?.overlay) {
      const alive = this.bridgeAlive();
      if (!alive && st && !this.busy) this.ensure(cx);
      return this.indicator(th, st, alive);
    }
    const button = (id, label, primary, onClick) =>
      h_flex().id(id).h(30).px(12).items_center().rounded(8).border_1()
        .border_color(primary ? th.solid : th.input).bg(primary ? th.solid : th.raised)
        .text_color(primary ? th.solid_foreground : th.foreground)
        .when(!this.busy, (el) => el.cursor_pointer().hover((s) => s.opacity(0.88)).on_click(onClick))
        .when(this.busy, (el) => el.opacity(0.5))
        .child(label);
    const row = (label, value, color) =>
      h_flex().gap(10).child(div().w(90).text_color(th.dim).child(label))
        .child(div().flex_1().text_color(color ?? th.foreground).child(value));
    const title = h_flex().gap(4).items_center()
      .child(div().flex_1().text_size(15).font_semibold().text_color(th.foreground).child("herdr-assist"));
    const root = v_flex().id("herdr-assist").size_full().min_h(0).p(18).gap(10).bg(th.popover).text_size(13);
    if (!st) return root.child(title).child(div().text_color(th.dim).child(this.error || "…"));

    const tabs = ["status", "token", "pair"].map((tab) =>
      div().id("tab-" + tab).px(10).py(4).rounded(6).text_size(12).cursor_pointer()
        .bg(this.tab === tab ? th.selected : th.popover)
        .text_color(this.tab === tab ? th.foreground : th.muted)
        .hover((s) => s.bg(th.hover))
        .on_click((_e, cx) => this.open(tab, cx))
        .child(this.t("tab." + tab)));
    const body = this.tab === "token" ? this.tokenTab(th, row, button)
      : this.tab === "pair" ? this.pairTab(th, button)
      : this.statusTab(th, st, row, button);
    return root.child(title.children(tabs)).children(body)
      .when(!!this.note, (el) => el.child(div().text_color(th.done).child(this.note)))
      .when(!!this.error, (el) => el.child(div().text_color(th.blocked).child(this.error)))
      .when(this.busy, (el) => el.child(div().text_color(th.dim).child(this.t(this.busyKey))));
  }

  statusTab(th, st, row, button) {
    const other = st.holder && !st.holder.is_bridge ? st.holder : null;
    return [
      row(this.t("bridge"),
        st.alive ? this.t("bridge.up", { t: st.terminal, pid: st.pids.join(", ") || "?" }) : this.t("bridge.down"),
        st.alive ? th.done : th.blocked),
      st.restart_pending
        ? row(this.t("version"), this.t("version.pending", { run: st.running, disk: st.version }), th.blocked)
        : row(this.t("version"), st.version || "?"),
      row(this.t("port"), String(st.port)),
      row(this.t("ip"), st.ip || "?"),
      row(this.t("panels"),
        st.panels.length ? this.t("panels.some", { n: st.panels.length, ips: st.panels.join(", ") }) : this.t("panels.none"),
        st.panels.length ? th.foreground : th.dim),
      ...(other ? [v_flex().gap(8).p(10).rounded(8).border_1().border_color(th.blocked)
        .child(div().text_color(th.foreground).child(
          this.askPid ? this.t("stop.ask", { pid: this.askPid }) : this.t("busy", { port: st.port, pid: other.pid, cmd: other.command })))
        .child(h_flex().justify_end().gap(8).text_size(12).children(this.askPid ? [
          button("stop-no", this.t("cancel"), false, (_e, cx) => { this.askPid = null; cx.notify(); }),
          button("stop-yes", this.t("confirm"), true, (_e, cx) => {
            const pid = this.askPid;
            this.askPid = null;
            // depois de encerrar, o restart espera a porta soltar e sobe a Ponte do mod
            this.act(cx, async () => {
              const r = await this.ctl("kill-holder", String(pid));
              return r.ok ? this.ctl("restart") : r;
            });
          }),
        ] : [
          button("stop", this.t("stop"), false, (_e, cx) => { this.askPid = other.pid; cx.notify(); }),
        ]))] : []),
      h_flex().justify_end().gap(8).text_size(12)
        .child(button("restart", this.t("restart"), true, (_e, cx) => this.act(cx, () => this.ctl("restart")))),
    ];
  }

  tokenTab(th, row, button) {
    const tok = this.token;
    const shown = !tok ? (tok === null ? "…" : this.t("token.none"))
      : this.showToken ? tok : "••••••••" + tok.slice(-4);
    return [
      h_flex().gap(10).items_center()
        .child(div().flex_1().child(row(this.t("token"), shown, tok ? th.foreground : th.dim)))
        .when(!!tok, (el) => el.child(h_flex().text_size(12).child(
          button("show", this.t(this.showToken ? "hide" : "show"), false,
            (_e, cx) => { this.showToken = !this.showToken; cx.notify(); })))),
      // 2 passos, como o Encerrar: a rotação derruba todo painel pareado
      this.askRotate
        ? v_flex().gap(8).p(10).rounded(8).border_1().border_color(th.blocked)
          .child(div().text_color(th.foreground).child(this.t("rotate.ask")))
          .child(h_flex().justify_end().gap(8).text_size(12).children([
            button("rotate-no", this.t("cancel"), false, (_e, cx) => { this.askRotate = false; cx.notify(); }),
            button("rotate-yes", this.t("confirm"), true, (_e, cx) => {
              this.askRotate = false;
              this.act(cx, async () => {
                const r = await this.ctl("rotate-token");
                if (r.ok) { this.token = r.token; this.showToken = true; }
                return r.ok ? { note: this.t("rotate.done") } : r;
              }, "rotating");
            }),
          ]))
        : h_flex().justify_end().text_size(12).child(
          button("rotate", this.t("rotate"), false, (_e, cx) => { this.askRotate = true; this.note = ""; cx.notify(); })),
    ];
  }

  pairTab(th, button) {
    const list = v_flex().gap(6).children(this.found.map((p) =>
      h_flex().gap(10).items_center()
        .child(div().w(70).font_semibold().text_color(th.foreground).child(p.id))
        .child(div().flex_1().text_color(th.dim).child(p.ip))
        .child(h_flex().text_size(12).child(button("pair-" + p.id, this.t("pair"), true, (_e, cx) =>
          this.act(cx, async () => {
            const r = await this.ctl("pair", p.ip, String(p.port));
            if (!r.ok) return r;
            this.found = this.found.filter((f) => f.id !== p.id);
            return { note: this.t("pair.ok", { id: p.id }) };
          }, "sending"))))));
    return [h_flex().gap(14).items_start()
      .child(v_flex().flex_1().gap(8)
        .child(div().text_color(th.foreground).child(this.t("pair.where")))
        .child(div().text_color(this.scanError ? th.blocked : th.dim)
          .child(this.scanError || this.t("pair.listening", { n: this.found.length })))
        .child(list)
        .when(this.found.length > 0, (el) => el.child(div().text_color(th.dim).text_size(12).child(this.t("pair.check")))))
      .child(v_flex().gap(4).items_center()
        .child(image("mods/herdr-assist/qr-" + (this.st.lang === "pt" ? "pt" : "en") + ".svg").size(120))
        .child(div().text_size(11).text_color(th.dim).child(this.t("manual"))))];
  }
}
