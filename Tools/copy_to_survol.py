# Copie CityLab -> Survol des packages REFERENCES par la map + blocs, d'apres
# Saved/desktop_manifest.txt (ecrit par gen_desktop_toulouse10.py). Les orphelins
# ne voyagent jamais (c'est le role du manifeste). Verification par hash MD5.
# AUCUN editeur Survol ne doit etre ouvert pendant la copie.
# Usage : python copy_to_survol.py [--dry]
import hashlib
import os
import shutil
import sys

CITYLAB = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SURVOL = os.path.normpath(os.path.join(CITYLAB, "..", "Survol"))
MANIFEST = os.path.join(CITYLAB, "Saved", "desktop_manifest.txt")

sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    dry = "--dry" in sys.argv
    with open(MANIFEST, encoding="utf-8") as f:
        pkgs = [l.strip() for l in f if l.strip().startswith("/Game/")]
    print(f"manifeste : {len(pkgs)} packages /Game/")

    copied = same = missing = 0
    bad = []
    total = 0
    for pkg in pkgs:
        rel = pkg[len("/Game/"):]
        src_base = os.path.join(CITYLAB, "Content", rel.replace("/", os.sep))
        src = None
        for ext in (".uasset", ".umap"):
            if os.path.exists(src_base + ext):
                src = src_base + ext
                break
        if src is None:
            missing += 1
            if missing <= 5:
                print(f"  ABSENT côté CityLab : {pkg}")
            continue
        dst = os.path.join(SURVOL, "Content", rel.replace("/", os.sep)) + os.path.splitext(src)[1]
        total += os.path.getsize(src)
        if os.path.exists(dst) and md5(dst) == md5(src):
            same += 1
            continue
        if not dry:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            if md5(dst) != md5(src):
                bad.append(pkg)
        copied += 1

    print(f"copies : {copied}, deja identiques : {same}, absents CityLab : {missing}, "
          f"hash KO : {len(bad)}, volume manifeste : {total / 1e9:.2f} Go{' (DRY RUN)' if dry else ''}")
    for b in bad[:10]:
        print(f"  HASH KO : {b}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
