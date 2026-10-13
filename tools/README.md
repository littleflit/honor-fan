# Tools

| File | Purpose |
|---|---|
| `honor-fan` | CLI: `status` / `auto` / `set <percent>` / `raw <addr> [val]` |
| `honor-fan-gui.py` | Qt (PyQt5) control panel: live RPM/temps, slider + Auto button (writes via pkexec) |
| `fan_curve.py` | Temperature-driven fan curve daemon |
| `honor-fan-curve.service` | systemd unit for the daemon |
| `fan_probe.sh` | Measures the `FSUD`/`FSVL` encoding; restores auto on any exit |
| `ec_watch.sh` | Diffs the 256-byte EC space over time (needs `ec_sys`) |

`fan_curve.py` hardcodes no paths beyond the hwmon lookup; edit `CURVE` at the top to
change the thresholds. The systemd unit references an absolute path — adjust it to wherever
you cloned the repo.
