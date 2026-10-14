#!/usr/bin/env python3
"""honor-fan-gui - minimal Qt control panel for the honor_fan hwmon driver.

Runs unprivileged. Reads are plain sysfs (no root needed); the Set/Auto
buttons perform one privileged helper call each via pkexec, which shows a
graphical password prompt.

Requires: python-pyqt5, pkexec, the honor_fan module loaded
          (enable_control=1 for the Set button to work).
"""
import os
import subprocess
import sys
import time

from PyQt5.QtCore import Qt, QTimer
from PyQt5.QtWidgets import (
    QApplication,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMessageBox,
    QPushButton,
    QSlider,
    QVBoxLayout,
    QWidget,
)

REFRESH_MS = 2000
KO_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "kernel", "honor_fan.ko")


def find_hwmon():
    for entry in sorted(os.listdir("/sys/class/hwmon")):
        base = f"/sys/class/hwmon/{entry}"
        try:
            with open(f"{base}/name") as fh:
                if fh.read().strip() == "honor_fan":
                    return base
        except OSError:
            continue
    return None


def read_int(path):
    try:
        with open(path) as fh:
            return int(fh.read().strip())
    except (OSError, ValueError):
        return None


class Panel(QWidget):
    def __init__(self, hwmon):
        super().__init__()
        self.hwmon = hwmon
        self.setWindowTitle("Honor Fan")
        self.setMinimumWidth(380)

        layout = QVBoxLayout(self)

        grid = QGridLayout()
        self.mode_val = QLabel("-")
        self.ctrl_val = QLabel("-")
        self.fan1_val = QLabel("-")
        self.fan2_val = QLabel("-")
        self.cpu_val = QLabel("-")
        self.hot_val = QLabel("-")
        for row, (name, widget) in enumerate(
            [("Mode", self.mode_val), ("Control", self.ctrl_val),
             ("Fan 1", self.fan1_val), ("Fan 2", self.fan2_val),
             ("CPU", self.cpu_val), ("Hottest", self.hot_val)]
        ):
            grid.addWidget(QLabel(name), row, 0)
            grid.addWidget(widget, row, 1)
        layout.addLayout(grid)

        self.pct_label = QLabel("70%")
        self.pct_label.setAlignment(Qt.AlignCenter)
        layout.addWidget(self.pct_label)

        self.slider = QSlider(Qt.Horizontal)
        self.slider.setRange(0, 100)
        self.slider.setValue(70)
        self.slider.setTickPosition(QSlider.TicksBelow)
        self.slider.setTickInterval(10)
        self.slider.valueChanged.connect(
            lambda v: self.pct_label.setText(f"{v}%"))
        layout.addWidget(self.slider)

        row = QHBoxLayout()
        self.set_btn = QPushButton("Set manual")
        self.set_btn.clicked.connect(self.set_manual)
        self.auto_btn = QPushButton("Auto (firmware)")
        self.auto_btn.clicked.connect(self.set_auto)
        row.addWidget(self.set_btn)
        row.addWidget(self.auto_btn)
        layout.addLayout(row)

        self.ctrl_btn = QPushButton("Enable control")
        self.ctrl_btn.clicked.connect(self.toggle_control)
        layout.addWidget(self.ctrl_btn)

        self.status = QLabel("0% = idle (~1600 rpm), never off.")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)

        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh)
        self.timer.start(REFRESH_MS)
        self.refresh()

    def refresh(self):
        h = self.hwmon
        en = read_int(f"{h}/pwm1_enable")
        pwm = read_int(f"{h}/pwm1")
        f1 = read_int(f"{h}/fan1_input")
        f2 = read_int(f"{h}/fan2_input")
        if en is None:
            self.status.setText("Driver gone — is honor_fan still loaded?")
            self.set_btn.setEnabled(False)
            self.auto_btn.setEnabled(False)
            self.ctrl_btn.setEnabled(False)
            return
        self.set_btn.setEnabled(True)
        self.auto_btn.setEnabled(True)
        self.ctrl_btn.setEnabled(True)
        try:
            with open("/sys/module/honor_fan/parameters/enable_control") as fh:
                controllable = fh.read().strip() in ("Y", "y", "1")
        except OSError:
            controllable = False
        self.ctrl_val.setText("on" if controllable else "off (monitoring only)")
        self.ctrl_btn.setText("Monitoring only" if controllable else "Enable control")
        # pwm writes are refused without enable_control=1; grey them out.
        self.slider.setEnabled(controllable)
        self.set_btn.setEnabled(controllable)
        self.mode_val.setText("manual" if en == 1 else "auto (firmware)")
        if pwm is not None:
            self.mode_val.setText(
                f"{self.mode_val.text()}  (pwm {pwm}/255)")
        self.fan1_val.setText(f"{f1} rpm" if f1 is not None else "?")
        self.fan2_val.setText(f"{f2} rpm" if f2 is not None else "?")

        cpu = hot = None
        for n in range(1, 12):
            t = read_int(f"{h}/temp{n}_input")
            if t is None:
                continue
            lbl = f"{h}/temp{n}_label"
            try:
                with open(lbl) as fh:
                    name = fh.read().strip()
            except OSError:
                name = f"temp{n}"
            if name == "CPU":
                cpu = t // 1000
            hot = t // 1000 if hot is None else max(hot, t // 1000)
        self.cpu_val.setText(f"{cpu} °C" if cpu is not None else "?")
        self.hot_val.setText(f"{hot} °C" if hot is not None else "?")

    def _priv_write(self, script, action):
        """Run a root shell snippet via pkexec. One auth prompt per action."""
        r = subprocess.run(["pkexec", "sh", "-c", script],
                           capture_output=True, text=True)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines()
            detail = err[-1] if err else f"exit code {r.returncode}"
            self.status.setText(f"{action} failed: {detail}")
            if "EPERM" in detail or "Permission" in detail:
                QMessageBox.warning(
                    self, "Not allowed",
                    "The driver refused the write.\n"
                    "Reload it with enable_control=1:\n"
                    "sudo rmmod honor_fan\n"
                    "sudo insmod honor_fan.ko enable_control=1")
        else:
            self.status.setText(f"{action} ok.")
            self.refresh()

    def set_manual(self):
        pct = self.slider.value()
        pwm = pct * 255 // 100
        h = self.hwmon
        # pwm BEFORE enable: a single programming pass (see tools/honor-fan).
        self._priv_write(
            f"echo {pwm} > {h}/pwm1; echo 1 > {h}/pwm1_enable",
            f"Manual {pct}%")

    def set_auto(self):
        self._priv_write(f"echo 2 > {self.hwmon}/pwm1_enable",
                         "Back to firmware control")

    def toggle_control(self):
        """Reload the module with/without enable_control=1.

        The parameter is load-time only, so switching means rmmod+insmod
        (one pkexec prompt). The hwmon index can change across reloads,
        so re-discover it afterwards.
        """
        try:
            with open("/sys/module/honor_fan/parameters/enable_control") as fh:
                controllable = fh.read().strip() in ("Y", "y", "1")
        except OSError:
            controllable = False
        if not os.path.isfile(KO_PATH):
            QMessageBox.critical(self, "Honor Fan",
                                 f"Module file not found:\n{KO_PATH}\n"
                                 "Build it first: make LLVM=1 in kernel/.")
            return
        if controllable:
            script = "rmmod honor_fan && insmod " + KO_PATH
            action = "Monitoring only"
        else:
            script = "rmmod honor_fan && insmod " + KO_PATH + " enable_control=1"
            action = "Control enabled"
        self.status.setText("Reloading module…")
        QApplication.processEvents()
        r = subprocess.run(["pkexec", "sh", "-c", script],
                           capture_output=True, text=True)
        if r.returncode != 0:
            err = (r.stderr or "").strip().splitlines()
            self.status.setText(
                f"Reload failed: {err[-1] if err else r.returncode}")
            return
        hwmon = None
        for _ in range(25):
            hwmon = find_hwmon()
            if hwmon is not None:
                break
            time.sleep(0.2)
        if hwmon is None:
            self.status.setText("Reloaded but hwmon did not reappear?")
            return
        self.hwmon = hwmon
        self.status.setText(f"{action} ok.")
        self.refresh()


def main():
    app = QApplication(sys.argv)
    hwmon = find_hwmon()
    if hwmon is None:
        QMessageBox.critical(None, "Honor Fan",
                             "honor_fan hwmon not found.\n"
                             "Load the module first:\n"
                             "sudo insmod honor_fan.ko [enable_control=1]")
        return 1
    panel = Panel(hwmon)
    panel.show()
    return app.exec_()


if __name__ == "__main__":
    sys.exit(main())
