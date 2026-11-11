// Mod de controle do herdr-assist: status da Ponte, Reiniciar e aviso de
// porta ocupada. Abre como dialog pelo `mod.open herdr-assist` (a tecla do
// keys.json chama o mesmo comando): com "place": "none" não existe cópia
// visível para receber um `<mod>.<command>`, então o mod.json descreve o
// overlay e cada abertura é uma cópia nova.
// O JS só desenha; as operações são o ctl.py desta pasta, por exec.

import { View, div } from "gpui-kit";
import { v_flex, h_flex } from "gpui-base";
import { exec, appHome, theme, props } from "herdr";

// chave: [en, pt]
const STRINGS = {
  bridge: ["Bridge", "Ponte"],
  "bridge.up": ["running ({t}, pid {pid})", "ativa ({t}, pid {pid})"],
  "bridge.down": ["stopped", "parada"],
  version: ["Version", "Versão"],
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
};

export default class HerdrAssist extends View {
  init(_props, cx) {
    this.dir = appHome() + "/mods/herdr-assist";
    this.st = null;
    this.busy = false;
    this.askPid = null;
    this.error = "";
    const dialog = !!props()?.overlay;
    // Toda cópia garante a Ponte; só o dialog consulta status, e só enquanto aberto.
    cx.spawn(async (cx) => {
      await this.ctl("ensure");
      if (dialog) await this.refresh(cx);
    });
    if (dialog) cx.timer.every(3000, (cx) => cx.spawn(async (cx) => this.refresh(cx)));
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

  // Ação longa: trava o poll, roda, mostra o erro se houver e relê o status.
  act(cx, work) {
    this.busy = true;
    this.error = "";
    cx.notify();
    cx.spawn(async (cx) => {
      const r = await work();
      this.error = r.error ?? "";
      this.busy = false;
      await this.refresh(cx);
    });
  }

  t(key, vars = {}) {
    const s = STRINGS[key][this.st?.lang === "pt" ? 1 : 0];
    return s.replace(/\{(\w+)\}/g, (_, k) => String(vars[k] ?? ""));
  }

  render(cx) {
    if (!props()?.overlay) return div(); // cópia posta num layout: só mantém a Ponte
    const th = theme();
    const st = this.st;
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
    const root = v_flex().id("herdr-assist").size_full().min_h(0).p(18).gap(10).bg(th.popover).text_size(13)
      .child(div().text_size(15).font_semibold().text_color(th.foreground).child("herdr-assist"));
    if (!st) return root.child(div().text_color(th.dim).child(this.error || "…"));

    const other = st.holder && !st.holder.is_bridge ? st.holder : null;
    return root
      .child(row(this.t("bridge"),
        st.alive ? this.t("bridge.up", { t: st.terminal, pid: st.pids.join(", ") || "?" }) : this.t("bridge.down"),
        st.alive ? th.done : th.blocked))
      .child(row(this.t("version"), st.version || "?"))
      .child(row(this.t("port"), String(st.port)))
      .child(row(this.t("ip"), st.ip || "?"))
      .child(row(this.t("panels"),
        st.panels.length ? this.t("panels.some", { n: st.panels.length, ips: st.panels.join(", ") }) : this.t("panels.none"),
        st.panels.length ? th.foreground : th.dim))
      .when(!!other, (el) => el.child(
        v_flex().gap(8).p(10).rounded(8).border_1().border_color(th.blocked)
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
          ]))))
      .when(!!this.error, (el) => el.child(div().text_color(th.blocked).child(this.error)))
      .child(h_flex().justify_end().items_center().gap(8).text_size(12)
        .when(this.busy, (el) => el.child(div().text_color(th.dim).child(this.t("restarting"))))
        .child(button("restart", this.t("restart"), true, (_e, cx) => this.act(cx, () => this.ctl("restart")))));
  }
}
