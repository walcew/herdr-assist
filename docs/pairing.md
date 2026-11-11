# Pairing manual

> 🇧🇷 **Este manual também existe em português:** [pairing.pt-BR.md](pairing.pt-BR.md)
>
> This is the guide the QR code on the panel's **Pair** screen opens.

Pairing connects the panel to a machine running [Herdr](https://herdr.dev)
(the *host*). Typing a 32-character token on a 3.5" touchscreen would be
unreasonable, so the flow is reversed: **the panel announces itself on the
network and the host sends the ready-made configuration** — name, address,
port and token. Nothing is typed on the panel.

## What you will need

- The panel **flashed and powered** — if you haven't flashed it yet, see
  [Flash the panel](../README.md#1-flash-the-panel).
- The panel **connected to Wi-Fi** (Settings → Wi-Fi network).
- A machine with the **Herdr desktop app installed and running**.
- Panel and host **on the same local network**: pairing relies on UDP
  broadcast, which does not cross VLANs or guest networks.

## 1. Install the bridge on the host

The bridge ships as a mod of the Herdr desktop app — it is what the panel talks
to over the network. Once per machine you want to monitor, copy the mod folder
into the app's home and open the control dialog (`cmd-alt-a`):

```sh
cp -R mod/herdr-assist ~/.config/herdr-app/mods/herdr-assist
```

Opening the app (or the dialog) starts the bridge. It is pure-stdlib Python:
the stock macOS `python3` (3.9+) is enough, no toolchain. Full install notes
(keys, the footer indicator, leaving the old Herdr TUI plugin) are in the
[mod README](../mod/herdr-assist/README.md).

## 2. Put the panel in pairing mode

On the panel: **Settings → Pair with a host** (the green row).

<img src="images/pair-settings.png" width="320" alt="Settings screen with the green Pair with a host row highlighted">

The pairing screen shows **this panel's code** (6 characters, derived from the
MAC address) and starts announcing itself over broadcast for **3 minutes**.
The same screen summarizes these steps and carries the QR code that points to
this manual:

<img src="images/pair-screen.png" width="320" alt="Full pairing screen: panel code, step by step and the manual QR code">

> If you see **"No Wi-Fi: connect to a network first"**, fix Wi-Fi first —
> without a network the announcement goes nowhere. The mode stays on: as soon
> as Wi-Fi connects, the announcement starts reaching the network by itself.

## 3. On the host, send the configuration

In the Herdr desktop app, press `cmd-alt-a` (or run `herdr-app do mod.open --name herdr-assist`) to
open the control dialog, and go to the **Pair** tab. The host starts listening for announcements and lists every
panel it finds, each with its code and IP.

Check that the listed code matches the one on the panel screen and click
**Pair** on that row. The host sends the configuration; the panel confirms
**"Paired with …"**, saves it and **restarts already connected** — your
sessions show up in the Sessions tab.

### From the command line (`ctl.py pair`)

Without the dialog (an SSH session, a script), the same protocol is in the
mod's `ctl.py`:

```sh
python3 ctl.py discover                 # panels announcing now: [{id, ip, port}]
python3 ctl.py pair 192.168.1.42 9376   # sends the configuration to that panel
```

The script sits in the mod directory (`~/.config/herdr-app/mods/herdr-assist`).

> The M5Stack Cardputer port pairs exactly the same way — same bridge, same
> flow, only the device screen differs.

## Manual setup (networks that filter broadcast)

If the panel never shows up on the host's list — a VLAN between panel and
host, AP isolation, a corporate network — register without discovery:
**Settings → Add manually**, filling in:

| Field | Value |
|---|---|
| Name | Any label (e.g. `mac`) |
| Address | Host IP or hostname |
| Port | `9375` (bridge default) |
| Token | 32 hex — see below |
| Auto discovery | **off** (a typed address is static) |

The token comes from the host, either way:

```sh
python3 ~/.config/herdr-app/mods/herdr-assist/ctl.py token   # via CLI: {"token": "..."}
# or the control dialog (cmd-alt-a) → Token tab → Show
```

Typing 32 hex on a touchscreen is tedious — that is exactly what automatic
pairing avoids. Use manual setup only when broadcast cannot reach the host.

## How it works (and what it does not do)

- With the mode on, the panel announces `{"t":"herdr-assist","id","port"}`
  over UDP broadcast and accepts **one** configuration over TCP on port 9376,
  for up to 180 s. Outside that window nothing is listening.
- The host replies with name, address, port and token; the panel validates,
  saves to NVS and restarts. The panel keeps **one token per host** (up to 4
  hosts).
- Paired through the automatic flow, the slot stays in **auto mode**: no IP is
  stored — the panel finds the bridge via broadcast on every boot or drop, so
  DHCP handing the host a new IP breaks nothing. A hand-typed address turns
  discovery off for that slot.
- **Re-pairing the same machine does not duplicate it**: the slot is matched
  by host name, and the new configuration replaces the old one.

## Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| The panel never shows up on the host's list | Different networks, VLAN/AP isolation, filtered broadcast | Put both on the same network; if it persists, use [manual setup](#manual-setup-networks-that-filter-broadcast) |
| "No Wi-Fi: connect to a network first" on the panel | Panel has no network | Settings → Wi-Fi network; then go back to pairing |
| "no token — start the bridge before pairing" on the host | The bridge never started (the token is created on first start) | Open the control dialog (`cmd-alt-a`) and use **Restart bridge** on the Status tab |
| macOS firewall asks whether `python3` may accept connections | Pairing listener blocked | Allow it — otherwise announcements never arrive |
| "Window closed" on the panel | The 3 minutes ran out | Go back and tap Pair again |
| "No room: remove a host before pairing" | All 4 slots are taken | Settings → tap a host → Remove |
| Paired, but the host stays Offline | Token rotated after pairing, or a static IP that changed | Pair again (it updates the slot); prefer auto mode for dynamic IPs |
| "the panel rejected the configuration" on the host | Panel firmware too old for the payload | Update the panel (Settings → Update firmware) and pair again |

## Rotating the token

**Rotate token** (Token tab of the control dialog) generates a new token that
the bridge applies on the spot — **every panel paired with that host stops
connecting until it is paired again**. Use it when you suspect the token leaked.

---

Back to the [README](../README.md#3-pair-the-panel).
