"""Draws docs/outage_comparison.png from the two simulated timelines.

    ./build/linux/towerwatch -S -m -d 120 -s scenarios/outage.txt -q -c docs/outage_timeline.csv
    ./build/linux/towerwatch -S -m -n -d 120 -s scenarios/outage.txt -q -c docs/outage_no_control.csv
    python3 docs/make_chart.py
"""
import csv
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

plt.rcParams.update({'font.size': 11, 'axes.spines.top': False, 'axes.spines.right': False,
                     'axes.edgecolor': '#8b949e', 'xtick.color': '#57606a', 'ytick.color': '#57606a'})
try:
    from matplotlib import font_manager as fm
    for f in ['/usr/share/fonts/opentype/inter/Inter-Regular.otf', '/usr/share/fonts/opentype/inter/Inter-SemiBold.otf']:
        fm.fontManager.addfont(f)
    plt.rcParams['font.family'] = 'Inter'
except Exception:
    pass


def load(p):
    rows = list(csv.DictReader(open(p)))
    return {k: [r[k] for r in rows] for k in rows[0]}


A = load('docs/outage_timeline.csv')
B = load('docs/outage_no_control.csv')
t = [float(x) for x in A['t']]
WITH, WITHOUT, GREY = '#0969da', '#cf222e', '#57606a'

fig, ax = plt.subplots(3, 1, figsize=(11, 7.0), sharex=True, gridspec_kw={'height_ratios': [3, 3, 1.4]})
ax[0].plot(t, [float(x) * 100 for x in A['soc']], color=WITH, lw=2.2, label='with towerwatch')
ax[0].plot(t, [float(x) * 100 for x in B['soc']], color=WITHOUT, lw=2.2, ls='--', label='monitoring only, no control')
ax[0].set_ylabel('Battery charge (%)')
ax[0].set_ylim(-3, 108)
ax[0].legend(frameon=False, loc='upper right')
ax[0].annotate('cooling load shed when the generator failed to start', xy=(21.2, 81), xytext=(24, 99),
               fontsize=9.5, color=WITH, arrowprops=dict(arrowstyle='->', color=WITH, lw=1))

ax[1].plot(t, [float(x) for x in A['bus_v']], color=WITH, lw=2.0)
ax[1].plot(t, [float(x) for x in B['bus_v']], color=WITHOUT, lw=2.0, ls='--')
ax[1].axhline(43.2, color=GREY, lw=1, ls=':')
ax[1].text(103, 44.2, 'battery disconnect 43.2 V', color=GREY, fontsize=9)
ax[1].set_ylabel('DC bus (V)')
ax[1].set_ylim(-2, 60)
for x, label in [(10, 'mains lost,\ngenerator fails'), (60, 'generator\nrepaired'), (100, 'mains\nback')]:
    for a in ax[:2]:
        a.axvline(x, color='#d0d7de', lw=1, zorder=0)
    if x == 60:
        ax[1].text(x - 1, 22, label, fontsize=9.5, color=GREY, va='bottom', ha='right')
    else:
        ax[1].text(x + 1, 22, label, fontsize=9.5, color=GREY, va='bottom')
ax[1].annotate('site down 32 s', xy=(84, 1), xytext=(76, 8), fontsize=10, color=WITHOUT)

colors = {'GRID': '#8c959f', 'BATTERY': '#bf8700', 'GEN': '#1a7f37', 'DOWN': '#cf222e'}
names = {'GRID': 'mains', 'BATTERY': 'battery', 'GEN': 'generator', 'DOWN': 'site down'}


def bands(axis, y, src):
    start = 0
    for i in range(1, len(src) + 1):
        if i == len(src) or src[i] != src[start]:
            end = t[i] if i < len(t) else t[-1] + 0.1
            axis.barh(y, end - t[start] + 0.1, left=t[start] - 0.1, height=0.6, color=colors[src[start]], edgecolor='none')
            start = i


bands(ax[2], 1, A['src'])
bands(ax[2], 0, B['src'])
ax[2].set_yticks([1, 0])
ax[2].set_yticklabels(['with towerwatch', 'no control'])
ax[2].set_xlabel('Seconds (simulated plant with a demo-size battery, 1 kHz control loop)')
for k, c in colors.items():
    ax[2].barh(-5, 0, color=c, label=names[k])
ax[2].set_ylim(-0.6, 1.6)
ax[2].legend(frameon=False, ncol=4, loc='upper center', bbox_to_anchor=(0.5, -0.6))

fig.suptitle('Same outage, with and without towerwatch', x=0.07, y=0.985, ha='left', fontsize=16, fontweight='semibold', color='#24292f')
fig.text(0.07, 0.922, 'Without control the battery runs flat and the site goes down for 32 s. '
         'With load shedding and a generator retry it stays on air.', fontsize=10.5, color=GREY)
fig.tight_layout(rect=(0.02, 0.02, 1, 0.905))
fig.savefig('docs/outage_comparison.png', dpi=150)
print('saved docs/outage_comparison.png')
