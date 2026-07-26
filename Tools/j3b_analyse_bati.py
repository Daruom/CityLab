# Verrou 1 du chantier J3b (toits en pente) : preuve par les donnees.
# Analyse bati_enrichi.json AVANT d'ecrire le builder : combien de toits sont
# reellement en pente (delta faitage-egout), quels codes materiaux dominent,
# quelle matrice de fallback prevoir, quelle complexite d'emprise pour le
# squelette droit. Lecture seule, aucun effet de bord.
# Usage : python.exe j3b_analyse_bati.py
import json, sys, os
from collections import Counter

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

PATH = os.path.join(os.path.dirname(__file__), "..", "SourceData", "GrandFetch", "bati_enrichi.json")

with open(PATH, encoding="utf-8") as f:
    data = json.load(f)
B = data["buildings"]
n = len(B)
print(f"Batiments : {n}")

# ---- Matrice de fallback (ce que le builder aura sous la main) ----
full = both = maxonly = honly = nothing = 0
for b in B:
    amin = b.get("altitude_minimale_toit")
    amax = b.get("altitude_maximale_toit")
    h = b.get("hauteur")
    if amin is not None and amax is not None:
        both += 1
    elif amax is not None:
        maxonly += 1
    elif h is not None:
        honly += 1
    else:
        nothing += 1
def pc(x): return f"{100.0*x/n:.1f} %"
print(f"\n-- Matrice de fallback --")
print(f"A egout+faitage mesures : {both} ({pc(both)})  -> pente exacte possible")
print(f"B faitage seul          : {maxonly} ({pc(maxonly)}) -> egout = faitage - pente inferee")
print(f"C hauteur seule         : {honly} ({pc(honly)})  -> toit plat ou pente par defaut")
print(f"D rien                  : {nothing} ({pc(nothing)})")

# ---- Delta faitage - egout (la vraie question : plat ou pente ?) ----
buckets = [(0.3, "quasi plat  <0,3 m"), (1.0, "0,3-1 m"), (2.0, "1-2 m"),
           (4.0, "2-4 m"), (8.0, "4-8 m"), (1e9, ">8 m")]
cnt = Counter(); neg = 0; deltas = []
for b in B:
    amin = b.get("altitude_minimale_toit"); amax = b.get("altitude_maximale_toit")
    if amin is None or amax is None: continue
    d = amax - amin
    if d < -0.05: neg += 1; continue
    deltas.append(d)
    for lim, name in buckets:
        if d <= lim: cnt[name] += 1; break
tot = len(deltas)
print(f"\n-- Delta faitage-egout ({tot} batiments mesures, {neg} deltas negatifs ecartes) --")
for _, name in buckets:
    c = cnt.get(name, 0)
    print(f"  {name:20s} : {c:7d} ({100.0*c/tot:.1f} %)")
deltas.sort()
print(f"  mediane {deltas[tot//2]:.1f} m, p90 {deltas[int(tot*0.9)]:.1f} m, max {deltas[-1]:.1f} m")

# ---- Hauteur de mur (egout - sol) : sanity ----
weird = 0; wall = []
for b in B:
    amin = b.get("altitude_minimale_toit"); asol = b.get("altitude_minimale_sol")
    if amin is None or asol is None: continue
    w = amin - asol
    wall.append(w)
    if w < -0.5: weird += 1
wall.sort()
print(f"\n-- Hauteur de mur egout-sol ({len(wall)} mesures) : mediane {wall[len(wall)//2]:.1f} m, "
      f"negatifs {weird} ({100.0*weird/len(wall):.2f} %)")

# ---- Codes materiaux toiture ----
mt = Counter(); mt_delta = {}
for b in B:
    c = b.get("materiaux_de_la_toiture")
    if c is None: continue
    mt[c] += 1
    amin = b.get("altitude_minimale_toit"); amax = b.get("altitude_maximale_toit")
    if amin is not None and amax is not None and amax - amin >= -0.05:
        mt_delta.setdefault(c, []).append(amax - amin)
nm = sum(mt.values())
print(f"\n-- Codes materiaux toiture ({nm} renseignes, {pc(nm)} du total) --")
for code, c in mt.most_common(12):
    ds = sorted(mt_delta.get(code, []))
    med = f"{ds[len(ds)//2]:.1f}" if ds else "-"
    print(f"  code {code:>4s} : {c:7d} ({100.0*c/nm:.1f} % des renseignes)  delta median {med} m")

# ---- Nature (pour les clochers etc.) ----
nat = Counter(b.get("nature", "?") for b in B)
print(f"\n-- Nature du bati (top 12) --")
for name, c in nat.most_common(12):
    print(f"  {name:45s} : {c:7d} ({pc(c)})")

# ---- Complexite des emprises (pour le squelette droit) ----
vc = Counter()
for b in B:
    p = len(b["pts"])
    if p <= 4: vc["3-4 pts (rect)"] += 1
    elif p <= 6: vc["5-6 pts"] += 1
    elif p <= 12: vc["7-12 pts"] += 1
    elif p <= 30: vc["13-30 pts"] += 1
    else: vc[">30 pts"] += 1
print(f"\n-- Complexite des emprises --")
for name in ["3-4 pts (rect)", "5-6 pts", "7-12 pts", "13-30 pts", ">30 pts"]:
    c = vc.get(name, 0)
    print(f"  {name:15s} : {c:7d} ({pc(c)})")

# ---- Temoins : le plus proche de l'origine (Capitole) + 2 plus grosses emprises ----
def area(pts):
    a = 0.0
    for i in range(len(pts)):
        x1, y1 = pts[i]; x2, y2 = pts[(i+1) % len(pts)]
        a += x1*y2 - x2*y1
    return abs(a) / 2
def center(pts):
    return (sum(p[0] for p in pts)/len(pts), sum(p[1] for p in pts)/len(pts))
near = min(B, key=lambda b: (lambda c: c[0]**2 + c[1]**2)(center(b["pts"])))
big = sorted(B, key=lambda b: -area(b["pts"]))[:2]
print(f"\n-- Temoins --")
for label, b in [("proche origine (Capitole)", near)] + [(f"grosse emprise #{i+1}", x) for i, x in enumerate(big)]:
    c = center(b["pts"])
    print(f"  [{label}] centre=({c[0]:.0f},{c[1]:.0f}) aire={area(b['pts']):.0f} m2 nature={b.get('nature')} "
          f"usage={b.get('usage_1')} h={b.get('hauteur')} egout={b.get('altitude_minimale_toit')} "
          f"faitage={b.get('altitude_maximale_toit')} mat_toit={b.get('materiaux_de_la_toiture')}")
