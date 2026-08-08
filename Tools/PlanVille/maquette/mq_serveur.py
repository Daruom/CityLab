# -*- coding: utf-8 -*-
"""mq_serveur.py — serveur local de MISE AU POINT de la maquette.

Sert `maquette\\web\\` en HTTP et accepte `POST /capture/<nom>.png` dont le
corps est le PNG brut : c'est ainsi que la page se photographie elle-meme sur
le disque, pour la verification visuelle (doctrine du chantier : on ne declare
jamais un resultat visuel sans capture).

La maquette LIVREE n'a pas besoin de ce serveur : `web\\index.html` marche en
double-clic (les cellules arrivent par balise <script>).

    C:\\LidarPoC\\venv\\Scripts\\python.exe mq_serveur.py [port]
"""
import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

WEB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
CAP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "captures")


class H(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=WEB, **k)

    def do_POST(self):
        if not self.path.startswith("/capture/"):
            self.send_error(404)
            return
        nom = os.path.basename(self.path)
        if not nom.endswith(".png"):
            nom += ".png"
        n = int(self.headers.get("Content-Length") or 0)
        data = self.rfile.read(n)
        if not os.path.isdir(CAP):
            os.makedirs(CAP)
        p = os.path.join(CAP, nom)
        with open(p, "wb") as f:
            f.write(data)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(("%s %d octets" % (p, len(data))).encode())

    def end_headers(self):
        # JAMAIS DE CACHE — c'est un serveur de MISE AU POINT.
        # Piege paye le 08/08 : apres correction de mq_vue.js, le navigateur a
        # resservi la version d'AVANT depuis son cache (la balise <script src>
        # n'a pas de version, et un rechargement force ne l'a pas emporte). La
        # page affichait donc l'ancien code pendant que le disque portait le
        # nouveau : une verification qui porte sur les mauvais octets ne vaut
        # rien, et rien dans la page ne le signale.
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def send_head(self):
        # neutralise la revalidation 304 : avec un cache interdit, une reponse
        # « non modifie » ferait rentrer l'ancien fichier par la fenetre
        self.headers.replace_header("If-Modified-Since", "") \
            if self.headers.get("If-Modified-Since") else None
        if self.headers.get("If-None-Match"):
            del self.headers["If-None-Match"]
        return super().send_head()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8791
    print("maquette servie sur http://127.0.0.1:%d/  (captures -> %s)"
          % (port, CAP), flush=True)
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
