"""Chart the two outage runs recorded on QNX 8.0 (docs/qnx/series_qemu.txt).

Same scenario both times: mains lost at 10 s, the generator fails to start
until it is repaired at 60 s, mains back at 100 s. One run with towerwatch in
control, one observing only. SPDX-License-Identifier: Apache-2.0
"""
import pathlib

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = pathlib.Path(__file__).parent


def load(name):
    text = (HERE / "series_qemu.txt").read_text()
    line = next(l for l in text.splitlines() if l.startswith(name + ":"))
    rows = [e.split() for e in line.split(":", 1)[1].split("|") if e.strip()]
    t = [int(r[0]) for r in rows]
    bus = [float(r[1]) if r[3] == "1" else float("nan") for r in rows]
    soc = [int(r[2]) for r in rows]
    up = [r[3] == "1" for r in rows]
    return t, bus, soc, up


def main():
    tp, bp, sp, upp = load("protect")
    to, bo, so, upo = load("observe")
    down = [t for t, u in zip(to, upo) if not u]

    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 13})
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(16, 9), sharex=True, dpi=120,
                                   gridspec_kw={"height_ratios": [1.15, 1]})
    fig.patch.set_facecolor("#0f1720")
    for ax in (ax1, ax2):
        ax.set_facecolor("#0f1720")
        ax.tick_params(colors="#c9d4df")
        for s in ax.spines.values():
            s.set_color("#3a4a5a")
        ax.grid(color="#22303d", linewidth=0.8)
        if down:
            ax.axvspan(min(down) - 0.5, max(down) + 0.5, color="#e5484d", alpha=0.16, lw=0)
        for x in (10, 60, 100):
            ax.axvline(x, color="#5b6b7b", lw=1, ls=":")

    ax1.plot(tp, bp, color="#3dd68c", lw=2.6, label="towerwatch in control: site stays up")
    ax1.plot(to, bo, color="#f5a524", lw=2.2, label="observe only: battery runs flat")
    ax1.axhline(43.2, color="#e5484d", lw=1.2, ls="--")
    ax1.text(110.5, 43.55, "battery disconnect 43.2 V", color="#e5484d", fontsize=11, ha="right")
    ax1.set_ylabel("DC bus (V)", color="#c9d4df")
    ax1.set_ylim(42, 55.5)
    ax1.legend(loc="lower left", facecolor="#16212c", edgecolor="#3a4a5a", labelcolor="#e6edf3", fontsize=12)
    if down:
        ax1.text((min(down) + max(down)) / 2, 54.6, f"site off air {len(down)} s", color="#ff8a8f",
                 ha="center", fontsize=14, fontweight="bold")

    ax2.plot(tp, sp, color="#3dd68c", lw=2.6)
    ax2.plot(to, so, color="#f5a524", lw=2.2)
    ax2.set_ylabel("battery charge (%)", color="#c9d4df")
    ax2.set_xlabel("time (s)", color="#c9d4df")
    ax2.set_ylim(-3, 105)
    ax2.set_xlim(0, 111)
    for x, label in ((10, "mains lost,\ngenerator fails"), (60, "generator\nrepaired"), (100, "mains\nback")):
        ax2.text(x + 0.8, 4, label, color="#9fb0c0", fontsize=11, va="bottom")

    fig.suptitle("Same outage, recorded on QNX 8.0 (x86_64, emulated CPU)", color="#e6edf3",
                 fontsize=20, fontweight="bold", x=0.06, ha="left", y=0.975)
    fig.text(0.06, 0.915, "1 kHz timer-pulse loop  ·  mains failure declared 4 ms after the fault  ·  "
             "load shed on generator failure", color="#9fb0c0", fontsize=13)
    fig.subplots_adjust(left=0.06, right=0.985, top=0.885, bottom=0.08, hspace=0.08)
    out = HERE / "outage_qnx.png"
    fig.savefig(out, facecolor=fig.get_facecolor())
    print(out)


if __name__ == "__main__":
    main()
