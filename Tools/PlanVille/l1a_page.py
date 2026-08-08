# -*- coding: utf-8 -*-
"""L1a — LA PAGE DE REVUE (HTML autonome, patron du recensement).

Donnees embarquees dans un `.js` (`var DATA = {...}`), aucun `fetch` (interdit
en `file://`), aucune dependance externe. La page porte : les cases ARBITRAGE
en tete, la matrice cliquable (case -> contrat, invariant, mesure, regles), le
registre trie par provenance, les compteurs.
"""
import io
import json
import os
import sys
import time

sys.path.insert(0, r"C:\LidarPoC\work\PLAN")
from c0_socle import CACHE, PLAN, chrono, jalon

OUT = os.path.join(PLAN, "matrice")

PAGE = u"""<!doctype html>
<html lang="fr">
<head>
<meta charset="utf-8">
<title>L1a — Matrice de coherence + registre des regles</title>
<style>
 html,body{margin:0;background:#14161a;color:#e8e8ea;
   font:13px/1.5 system-ui,Segoe UI,Roboto,sans-serif}
 .wrap{max-width:1560px;margin:0 auto;padding:18px 22px 60px}
 h1{font-size:19px;margin:0 0 4px} h2{font-size:13px;margin:26px 0 8px;
   color:#8d97a6;text-transform:uppercase;letter-spacing:.09em}
 .sub{color:#8d97a6;font-size:12px;margin-bottom:14px}
 .cards{display:flex;gap:10px;flex-wrap:wrap;margin:10px 0}
 .card{background:#1b1f25;border:1px solid #2b313a;border-radius:8px;
   padding:9px 14px;min-width:120px}
 .card b{display:block;font-size:21px} .card span{color:#8d97a6;font-size:11px}
 .nc b{color:#ff8a6a} .ok b{color:#5fd18a} .ni b{color:#8d97a6}
 .grid{overflow:auto;border:1px solid #2b313a;border-radius:8px;max-height:78vh}
 table.m{border-collapse:separate;border-spacing:0;font-size:11px}
 table.m th{position:sticky;background:#181c22;z-index:2;font-weight:600;
   color:#9fb4cc;padding:3px 5px;white-space:nowrap}
 table.m thead th{top:0;height:104px;vertical-align:bottom}
 table.m thead th div{writing-mode:vertical-rl;transform:rotate(180deg);
   text-align:left}
 table.m tbody th{left:0;text-align:left;max-width:230px;overflow:hidden;
   text-overflow:ellipsis}
 table.m td{width:17px;height:17px;border:1px solid #14161a;cursor:pointer}
 .c-CONTRAT{background:#1f6b45} .c-ARBITRAGE{background:#a8412c}
 .c-AUCUNE{background:#3a4048}
 .m-vert{background:#1f6b45} .m-rouge{background:#c0392b}
 .m-sansobjet{background:#262c34} .m-nonmesurable{background:#6b5b1f}
 .m-nonapplicable{background:#3a4048}
 table.m td:hover{outline:2px solid #7fb6e8;outline-offset:-2px}
 #det{background:#1b1f25;border:1px solid #2b313a;border-radius:8px;
   padding:12px 15px;margin-top:12px;min-height:96px}
 .kv{display:flex;gap:10px;padding:2px 0;border-bottom:1px solid #23272e;
   font-size:12.2px} .kv i{color:#8d97a6;font-style:normal;min-width:112px}
 .tag{display:inline-block;padding:1px 7px;border-radius:10px;font-size:11px}
 .t-reel{background:#153a26;color:#5fd18a}
 .t-donnee{background:#17304a;color:#7fb6e8}
 .t-arbitrage{background:#4a1f18;color:#ff8a6a}
 table.r{border-collapse:collapse;width:100%;font-size:12.2px}
 table.r th,table.r td{padding:6px 8px;border-bottom:1px solid #23272e;
   text-align:left;vertical-align:top}
 table.r th{background:#181c22;position:sticky;top:0;cursor:pointer}
 .note{color:#9aa4b2;font-size:11.5px} .leg{display:flex;gap:14px;
   flex-wrap:wrap;margin:8px 0;font-size:11.5px;color:#c3c9d2}
 .sw{display:inline-block;width:11px;height:11px;border-radius:2px;
   margin-right:5px;vertical-align:-1px}
 input,select{background:#20242b;color:#e8e8ea;border:1px solid #39404b;
   border-radius:5px;padding:5px 8px;font:inherit}
</style>
</head>
<body><div class="wrap">
<h1>L1a — Matrice de coherence &amp; registre des regles</h1>
<div class="sub" id="sub"></div>
<div class="cards" id="cards"></div>

<h2>⚠ Cases ROUGES — mesurees sur le plan resolu (en tete)</h2>
<div class="note">Chaque case CONTRAT a ete MESUREE : verte = invariant a 0,
rouge = invariant > 0 avec ses pires cas localises. Un rouge est un chiffre a
corriger a la source, pas un echec.</div>
<table class="r" id="trouge"><thead><tr><th>case</th><th>contrat</th>
<th class="num">cas / total</th><th class="num">%</th><th class="num">m
fautifs</th><th>invariant</th><th>pires cas (x ; y)</th></tr></thead>
<tbody></tbody></table>

<h2>⚠ Cases ARBITRAGE — la liste bornee</h2>
<div class="note">Deux contrats plausibles, sans que le reel ni la donnee ne
tranchent. Ce sont des questions, pas des echecs.</div>
<table class="r" id="tarb"><thead><tr><th>case</th><th>pourquoi c'est un
arbitrage</th><th>regles concernees</th></tr></thead><tbody></tbody></table>

<h2>La matrice — toutes les familles contre toutes</h2>
<div class="leg">
 <span><span class="sw c-CONTRAT"></span>CONTRAT</span>
 <span><span class="sw c-ARBITRAGE"></span>ARBITRAGE</span>
 <span><span class="sw c-AUCUNE"></span>AUCUNE INTERACTION</span>
 <span class="note">clic sur une case pour la lire</span>
</div>
<div class="leg">
 <label><input type="radio" name="vue" value="declare" checked> vue DECLAREE
 (statut de la case)</label>
 <label><input type="radio" name="vue" value="mesure"> vue MESUREE (vert /
 rouge / sans objet / non mesurable)</label></div>
<div class="grid"><table class="m" id="mat"></table></div>
<div id="det"><span class="note">Selectionne une case de la matrice.</span></div>

<h2>Le registre des regles — parti VIDE, rempli par les cases</h2>
<div class="leg">
 <input id="q" placeholder="filtrer une regle" size="30">
 <select id="fp"><option value="">toutes provenances</option>
  <option>reel</option><option>donnee</option><option>arbitrage</option></select>
 <span class="note" id="rc"></span>
</div>
<table class="r" id="treg"><thead><tr><th>regle</th><th>enonce</th>
<th>provenance</th><th>reference</th><th>invariant (cible 0)</th>
<th>comment on le mesure</th><th>cases qui l'exigent</th></tr></thead>
<tbody></tbody></table>
</div>
<script src="data_matrice.js"></script>
<script>
"use strict";
function esc(s){return String(s==null?'':s).replace(/[&<>]/g,function(c){
  return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
var F=DATA.familles, CL=F.map(function(f){return f.cle;});
var IDX={}; F.forEach(function(f,i){IDX[f.cle]=i;});
var CELL={};
DATA.cases.forEach(function(c){CELL[c.a+'|'+c.b]=c; CELL[c.b+'|'+c.a]=c;});
document.getElementById('sub').textContent = DATA.sous_titre;
var K=DATA.compteurs_cases, P=DATA.compteurs_provenance;
document.getElementById('cards').innerHTML =
  '<div class="card"><b>'+F.length+' x '+F.length+'</b><span>familles croisees</span></div>'
 +'<div class="card"><b>'+DATA.cases.length+'</b><span>cases uniques</span></div>'
 +'<div class="card ok"><b>'+(K.CONTRAT||0)+'</b><span>CONTRAT</span></div>'
 +'<div class="card nc"><b>'+(K.ARBITRAGE||0)+'</b><span>ARBITRAGE</span></div>'
 +'<div class="card ni"><b>'+(K['AUCUNE INTERACTION']||0)+'</b><span>aucune interaction</span></div>'
 +'<div class="card"><b>'+DATA.registre.length+'</b><span>regles ('
 +(P.reel||0)+' reel · '+(P.donnee||0)+' donnee · '+(P.arbitrage||0)+' arbitrage)</span></div>'
 +'<div class="card"><b>'+DATA.cases_sans_statut+'</b><span>case sans statut</span></div>'
 +(DATA.compteurs_mesure?'<div class="card ok"><b>'+(DATA.compteurs_mesure.vert||0)
   +'</b><span>cases MESUREES vertes</span></div>'
   +'<div class="card nc"><b>'+(DATA.compteurs_mesure.rouge||0)
   +'</b><span>cases MESUREES rouges</span></div>':'');

var h='<thead><tr><th></th>';
CL.forEach(function(k){h+='<th><div>'+esc(k)+'</div></th>';});
h+='</tr></thead><tbody>';
CL.forEach(function(a){
  h+='<tr><th title="'+esc(FAMNOM(a))+'">'+esc(a)+'</th>';
  CL.forEach(function(b){
    var c=CELL[a+'|'+b];
    var cls = classe(c);
    h+='<td class="'+cls+'" data-a="'+a+'" data-b="'+b+'" title="'+esc(a+' x '+b+' : '+c.statut)+'"></td>';
  });
  h+='</tr>';
});
document.getElementById('mat').innerHTML=h+'</tbody>';
Array.prototype.forEach.call(document.querySelectorAll('input[name=vue]'),
  function(r){r.onchange=function(){VUE=r.value;repeint();};});
repeint();
function FAMNOM(k){var f=F[IDX[k]];return f?f.nom:k;}
var VUE='declare';
function classe(c){
  if(VUE==='declare')
    return c.statut==='CONTRAT'?'c-CONTRAT':(c.statut==='ARBITRAGE'?'c-ARBITRAGE':'c-AUCUNE');
  var m=c.mesure_resultat; if(!m) return 'm-sansobjet';
  return 'm-'+m.statut.replace(/ /g,'');
}
function repeint(){
  Array.prototype.forEach.call(document.querySelectorAll('#mat td'),function(td){
    var c=CELL[td.dataset.a+'|'+td.dataset.b];
    td.className=classe(c);
    td.title=td.dataset.a+' x '+td.dataset.b+' : '+c.statut
      +(c.mesure_resultat?' | mesure : '+c.mesure_resultat.statut
        +(c.mesure_resultat.n_cas?' ('+c.mesure_resultat.n_cas+' cas)':''):'');
  });
}
function kv(k,v){return '<div class="kv"><i>'+k+'</i><span>'+v+'</span></div>';}
function REG(cle){for(var i=0;i<DATA.registre.length;i++)
  if(DATA.registre[i].cle===cle) return DATA.registre[i]; return null;}
document.getElementById('mat').addEventListener('click',function(e){
  var td=e.target.closest('td'); if(!td) return;
  var c=CELL[td.dataset.a+'|'+td.dataset.b];
  var h='<div style="font-size:14px;margin-bottom:6px"><b>'+esc(FAMNOM(c.a))
   +'</b> &nbsp;×&nbsp; <b>'+esc(FAMNOM(c.b))+'</b> — <span class="tag t-'
   +(c.statut==='ARBITRAGE'?'arbitrage':(c.statut==='CONTRAT'?'reel':'donnee'))
   +'">'+esc(c.statut)+'</span></div>';
  if(c.contrat) h+=kv('contrat', '<b>'+esc(c.contrat)+'</b> — '+esc(c.contrat_def));
  if(c.invariant) h+=kv('invariant (cible 0)', esc(c.invariant));
  if(c.mesure) h+=kv('mesure', esc(c.mesure));
  if(c.justification) h+=kv('justification', esc(c.justification));
  var M=c.mesure_resultat;
  if(M){
    h+=kv('MESURE sur le plan','<b>'+esc(M.statut)+'</b>'
      +(M.n_total?' — '+M.n_cas+' cas fautifs sur '+M.n_total
        +(M.part_pc!=null?' ('+M.part_pc+' %)':'')
        +(M.m_fautif!=null?', '+M.m_fautif+' m':''):''));
    if(M.note) h+=kv('note de mesure', esc(M.note));
    if(M.pires && M.pires.length) h+=kv('pires cas', M.pires.map(function(x){
      return esc(x.a)+' | '+esc(x.b)+' — dZ '+x.dz_m+' m sur '+x.m
        +' m ('+x.x+' ; '+x.y+')';}).join('<br>'));
  }
  if(c.regles && c.regles.length) h+=kv('regles exigees', c.regles.map(function(r){
    var R=REG(r); return '<span class="tag t-'+(R?R.provenance:'reel')+'">'+esc(r)+'</span>';
  }).join(' '));
  document.getElementById('det').innerHTML=h;
});
document.querySelector('#trouge tbody').innerHTML =
  (DATA.rouges||[]).map(function(r){
    var m=r.mesure;
    return '<tr><td><b>'+esc(r.a)+' × '+esc(r.b)+'</b></td><td>'+esc(r.contrat)
     +'</td><td class="num">'+m.n_cas+' / '+m.n_total+'</td><td class="num">'
     +(m.part_pc!=null?m.part_pc:'-')+'</td><td class="num">'
     +(m.m_fautif!=null?m.m_fautif:'-')+'</td><td class="note">'
     +esc(r.invariant)+'</td><td class="note">'
     +(m.pires||[]).map(function(x){return 'dZ '+x.dz_m+' m ('+x.x+' ; '+x.y+')';}).join('<br>')
     +'</td></tr>';}).join('') || '<tr><td colspan="7" class="note">aucune</td></tr>';
document.querySelector('#tarb tbody').innerHTML = DATA.arbitrages.map(function(c){
  return '<tr><td><b>'+esc(c.a)+' × '+esc(c.b)+'</b></td><td>'
   +esc(c.justification)+'</td><td class="note">'+esc((c.regles||[]).join(', '))
   +'</td></tr>';}).join('') || '<tr><td colspan="3" class="note">aucune</td></tr>';

var tri={k:'provenance',s:1};
function vreg(){
  var q=document.getElementById('q').value.toLowerCase();
  var fp=document.getElementById('fp').value;
  var r=DATA.registre.filter(function(x){
    if(fp&&x.provenance!==fp) return false;
    if(!q) return true;
    return (x.cle+' '+x.enonce+' '+x.reference).toLowerCase().indexOf(q)>=0;});
  r.sort(function(a,b){return String(a[tri.k]).localeCompare(String(b[tri.k]))*tri.s
    || a.cle.localeCompare(b.cle);});
  document.querySelector('#treg tbody').innerHTML = r.map(function(x){
    return '<tr><td><b>'+esc(x.cle)+'</b>'+(x.re_derivee?
      '<br><span class="note">re-derivee, non importee</span>':'')+'</td><td>'
     +esc(x.enonce)+'</td><td><span class="tag t-'+x.provenance+'">'
     +esc(x.provenance)+'</span></td><td class="note">'+esc(x.reference)
     +'</td><td>'+esc(x.invariant)+'</td><td class="note">'+esc(x.mesure)
     +'</td><td class="note">'+esc(x.cellules.length)+' case(s)<br>'
     +esc(x.cellules.slice(0,4).join(' · '))+'</td></tr>';}).join('');
  document.getElementById('rc').textContent=r.length+' / '+DATA.registre.length+' regles';
}
Array.prototype.forEach.call(document.querySelectorAll('#treg th'),function(th,i){
  th.onclick=function(){var k=['cle','enonce','provenance','reference','invariant','mesure','cellules'][i];
    tri.s=(tri.k===k?-tri.s:1); tri.k=k; vreg();};});
document.getElementById('q').oninput=vreg;
document.getElementById('fp').onchange=vreg;
vreg();
</script>
</body></html>
"""


def main():
    t0 = time.time()
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    d = json.load(io.open(os.path.join(CACHE, "l1a_matrice.json"),
                          encoding="utf-8"))
    data = dict(d)
    # la MESURE (L1b) si elle existe : la page passe de declaree a mesuree
    import pickle as _pk
    mp = os.path.join(CACHE, "l1b_matrice_mesuree.pkl")
    if os.path.exists(mp):
        with open(mp, "rb") as f:
            M = _pk.load(f)
        data["cases"] = M["cases"]
        data["compteurs_mesure"] = M["compteurs_mesure"]
        data["rouges"] = M["rouges"]
        data["population_familles"] = M["population_familles"]
        data["non_mesurables"] = M["non_mesurables"]
    data["sous_titre"] = (
        "%d familles croisees toutes contre toutes : %d cases uniques, "
        "0 sans statut. Le registre est parti VIDE ; ses %d regles ont ete "
        "TIREES par les cases, contenu cherche dans l'ordre reel -> donnee -> "
        "arbitrage. Aucune regle de l'ancien pipeline n'a ete importee."
        % (len(d["familles"]), len(d["cases"]), len(d["registre"])))
    js = "var DATA = %s;\n" % json.dumps(data, separators=(",", ":"),
                                         ensure_ascii=False)
    with io.open(os.path.join(OUT, "data_matrice.js"), "w", encoding="utf-8",
                 newline="\n") as f:
        f.write(js)
    with io.open(os.path.join(OUT, "index.html"), "w", encoding="utf-8",
                 newline="\n") as f:
        f.write(PAGE)
    o1 = os.path.getsize(os.path.join(OUT, "data_matrice.js"))
    o2 = os.path.getsize(os.path.join(OUT, "index.html"))
    jalon("L1a/⭐ PAGE DE REVUE : matrice/index.html (%.1f ko) + "
          "data_matrice.js (%.1f ko) — autonome, double-clic, aucune "
          "dependance ; ARBITRAGE en tete, matrice %dx%d cliquable, registre "
          "trie par provenance"
          % (o2 / 1e3, o1 / 1e3, len(d["familles"]), len(d["familles"])))
    chrono("L1a/page", time.time() - t0, "%.1f ko" % ((o1 + o2) / 1e3))


if __name__ == "__main__":
    main()
