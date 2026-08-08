/* mq_vue.js — LA MAQUETTE BLANCHE, rendu WebGL2 autonome.
   Aucune bibliotheque externe : la page marche en DOUBLE-CLIC (file://), ou
   fetch() est interdit — les cellules arrivent par balise <script> injectee.
   Repere du plan : x = est, y = SUD, z = altitude NGF. Ici : X=x, Y=z, Z=y. */
'use strict';

var CV = document.getElementById('gl');
var gl = CV.getContext('webgl2', {antialias: true, depth: true,
                                  powerPreference: 'high-performance'});
if (!gl) { document.body.innerHTML =
  '<p style="padding:24px">WebGL2 indisponible dans ce navigateur.</p>'; }

/* ------------------------------------------------------------- PALETTE ---- */
/* maquette blanche : teintes PLATES, claires, une par famille du plan */
var PAL = {
  chaussee:[0.72,0.72,0.74], trottoir:[0.82,0.81,0.79], carrefour:[0.66,0.67,0.70],
  voie_ferree:[0.60,0.58,0.56], canal:[0.55,0.70,0.80], piste_aero:[0.70,0.70,0.72],
  rond_point:[0.68,0.69,0.71], echangeur:[0.66,0.66,0.68], pont:[0.86,0.72,0.52],
  /* dalot : famille NEUVE du contrat final (règle `ouvrage_affleurant`).
     Teinte volontairement PROCHE de celle du pont mais plus sourde — on doit
     voir d'un coup d'œil que ce n'est pas un tablier. */
  dalot:[0.80,0.62,0.44],
  escalier:[0.88,0.80,0.62], gradins:[0.86,0.78,0.60], mur_sout:[0.78,0.70,0.62],
  ouvrage_hydro:[0.62,0.74,0.82], aqueduc:[0.80,0.72,0.60], edicule:[0.84,0.74,0.66],
  tremie:[0.70,0.66,0.62], sol_mineral:[0.87,0.86,0.84], sol_vegetal:[0.72,0.80,0.66],
  eau_surface:[0.48,0.64,0.78], parking:[0.78,0.78,0.77], terrain_sport:[0.74,0.82,0.70],
  batiment:[0.93,0.92,0.90], terrassement:[0.80,0.74,0.64],
  terrain_naturel:[0.76,0.80,0.70], semis:[0.55,0.70,0.50],
  breakline:[0.70,0.66,0.60], sous_sol:[0.50,0.50,0.52]
};
/* classes de dZ pour la CARTE DES MARCHES (bornes du registre : ressaut 2 cm,
   bordure <= 20 cm) */
var DZC = [
  {max:0.02, c:[0.55,0.80,0.55], t:'≤ 2 cm — ressaut admis'},
  {max:0.14, c:[0.70,0.85,0.45], t:'2 – 14 cm — bordure nominale'},
  {max:0.20, c:[0.95,0.88,0.40], t:'14 – 20 cm — plafond de bordure'},
  {max:0.50, c:[0.97,0.72,0.35], t:'20 – 50 cm'},
  {max:1.20, c:[0.95,0.52,0.32], t:'0,50 – 1,20 m'},
  {max:3.00, c:[0.87,0.34,0.34], t:'1,20 – 3 m'},
  {max:1e9,  c:[0.72,0.22,0.45], t:'> 3 m — marche franche'}
];
var COUCHES = [
  ['sol','sol résolu',true], ['itf','interfaces (pièces)',true],
  ['bati','bâtiments',true], ['ouvr','ouvrages',true],
  ['eau','eau (biefs)',true], ['terr','terrassements',true],
  ['veg','végétation',true]
];

/* ------------------------------------------------------------ SHADERS ----- */
function sh(t, src){ var s = gl.createShader(t); gl.shaderSource(s, src);
  gl.compileShader(s);
  if (!gl.getShaderParameter(s, gl.COMPILE_STATUS))
    throw new Error(gl.getShaderInfoLog(s) + '\n' + src);
  return s; }
function prog(v, f){ var p = gl.createProgram();
  gl.attachShader(p, sh(gl.VERTEX_SHADER, v));
  gl.attachShader(p, sh(gl.FRAGMENT_SHADER, f));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS))
    throw new Error(gl.getProgramInfoLog(p));
  return p; }

var VS = `#version 300 es
layout(location=0) in vec3 a_p;      // position quantifiee [0,1]
layout(location=1) in uvec2 a_c;     // (famille, classe de dZ)
layout(location=2) in uint a_h;      // 1 = sommet de l'arete HAUTE
uniform mat4 u_vp; uniform vec3 u_lo, u_span; uniform float u_fin;
uniform int u_mode;                  // 0 famille, 1 dZ, 2 blanc
uniform int u_itf;                   // 1 si la couche est celle des interfaces
uniform vec3 u_pal[40];
out vec3 v_w; flat out vec3 v_c;
void main(){
  vec3 w = u_lo + a_p * u_span;
  // CARTE DES MARCHES : l'arete haute d'une face d'interface est dressee en
  // ailette pour que la face, souvent haute de quelques centimetres, devienne
  // lisible depuis le ciel. Le dZ mesure, lui, n'est pas touche.
  w.z += float(a_h) * u_fin;
  v_w = w;
  if (u_mode == 2)                       v_c = vec3(0.90,0.89,0.87);
  else if (u_mode == 1)                  // CARTE DES MARCHES : seules les
    v_c = (u_itf == 1)                   // faces d'interface portent la classe
        ? u_pal[32 + min(int(a_c.y), 7)] //  de dZ ; le reste s'efface
        : vec3(0.84,0.84,0.85);
  else                                   v_c = u_pal[min(int(a_c.x), 31)];
  gl_Position = u_vp * vec4(w.x, w.z, w.y, 1.0);
}`;
/* ECLAIRAGE DE MAQUETTE, AVEC PLANCHER D'AMBIANCE GARANTI.
   La normale est prise aux derivees d'ecran. Sur une face vue par la TRANCHE
   (les faces de quai, de soutenement, les bandes d'interface verticales), ces
   derivees sont degenerees : cross() rend un vecteur quasi nul et normalize()
   rend alors des NaN, que le materiel ecrit en NOIR PUR. C'est ce qui rendait
   les faces de quai illisibles sur les captures B4/B5 — le 0,42 d'ambiant
   etait bien la, mais un NaN traverse une somme.
   Correctif : on ne normalise QUE si la normale a une longueur exploitable,
   sinon on prend la verticale ; puis on BORNE l'eclairement au plancher
   d'ambiance. Le clamp final garantit qu'AUCUNE face ne peut rendre noir. */
var AMB = 0.42;                     // plancher d'ambiance (aussi lu en JS)

/* LE FOND, et le REVELATEUR DE TROUS.
   Mesure faite le 08/08 : les bandes « noires » des captures B4/B5 ne sont PAS
   des faces mal eclairees — ce sont des TROUS du maillage, a travers lesquels
   on voit le fond, qui est presque noir (0,055 ; 0,065 ; 0,08). Compte : 3316
   pixels sur C4, 3846 sur l'ancienne B4, soit ~0,3 % de l'image. Un plancher
   d'ambiance ne peut rien pour eux : il n'y a aucune face a eclairer.
   MQ_TROUS(true) peint le fond en magenta : tout ce qui reste magenta au
   milieu de la ville est un trou, et se voit immediatement. */
var CIEL = [0.055, 0.065, 0.08];
window.MQ_TROUS = function(on){
  CIEL = on ? [1.0, 0.0, 0.85] : [0.055, 0.065, 0.08];
  SOCLE_ON = !on;      // le revelateur DOIT voir a travers : socle retire
  return CIEL;
};

/* LE SOCLE. Mesure du 08/08 : le domaine du plan est FINI (4000 x 3500 m) et
   les parcelles y pavent le sol a 100,00 % (vide residuel 1 m2 par cellule,
   en eclats tous sous le m2). Le « noir » d'une vue a l'horizon n'est donc pas
   un trou du modele : c'est ce qu'il y a AU-DELA du domaine, plus quelques
   eclats sous-metriques que la rasterisation laisse passer. Une ville qui
   flotte dans le vide se lit mal, et chaque manque se lit comme une face
   noire. On pose donc une dalle neutre sous tout le domaine, DEBORDANTE.
   Elle n'est PAS de la geometrie du contrat : elle ne compte dans aucun
   total, et MQ_TROUS la retire pour que le revelateur reste juste. */
var SOCLE_ON = true, SOCLE = null;
window.MQ_SOCLE = function(on){ SOCLE_ON = !!on; return SOCLE_ON; };
var VSS = `#version 300 es
layout(location=0) in vec2 a_p;
uniform mat4 u_vp; uniform float u_z;
out vec3 v_w;
void main(){ v_w = vec3(a_p.x, a_p.y, u_z);
  gl_Position = u_vp * vec4(a_p.x, u_z, a_p.y, 1.0); }`;
var FSS = `#version 300 es
precision highp float;
in vec3 v_w; out vec4 o;
uniform vec3 u_cam;
void main(){
  float f = clamp(length(v_w - u_cam) / 5200.0, 0.0, 1.0);
  o = vec4(mix(vec3(0.62,0.63,0.65), vec3(0.09,0.11,0.14), f * f * 0.85), 1.0);
}`;
function socleInit(){
  var xs = MQ_INDEX.cellules.map(function(c){ return c.o[0]; });
  var ys = MQ_INDEX.cellules.map(function(c){ return c.o[1]; });
  var M = MQ_INDEX.cellule_m, D = 12000;      // large debord
  var x0 = Math.min.apply(null, xs) - D, x1 = Math.max.apply(null, xs) + M + D;
  var y0 = Math.min.apply(null, ys) - D, y1 = Math.max.apply(null, ys) + M + D;
  var p = prog(VSS, FSS);
  var b = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, b);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(
    [x0,y0, x1,y0, x1,y1, x0,y0, x1,y1, x0,y1]), gl.STATIC_DRAW);
  var va = gl.createVertexArray(); gl.bindVertexArray(va);
  gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
  gl.bindVertexArray(null);
  SOCLE = { prog: p, vao: va,
            u_vp: gl.getUniformLocation(p, 'u_vp'),
            u_z: gl.getUniformLocation(p, 'u_z'),
            u_cam: gl.getUniformLocation(p, 'u_cam'), z: 0 };
}
function socleZ(){
  var z = 1e9;
  for (var c in CELLS){ var C = CELLS[c];
    if (C && !C.vide && C.bbox) z = Math.min(z, C.bbox[2]); }
  return (z < 1e8 ? z : 130) - 3.0;
}
var FS = `#version 300 es
precision highp float;
in vec3 v_w; flat in vec3 v_c; out vec4 o;
uniform vec3 u_cam;
const float AMB = ` + AMB.toFixed(3) + `;
void main(){
  vec3 g = cross(dFdx(v_w), dFdy(v_w));
  float g2 = dot(g, g);
  vec3 n = (g2 > 1e-16) ? g * inversesqrt(g2) : vec3(0.0, 0.0, 1.0);
  vec3 L = normalize(vec3(0.42, 0.30, 0.86));   // soleil de maquette
  float d = clamp(AMB + (1.0 - AMB) * abs(dot(n, L)), AMB, 1.0);
  float f = clamp(length(v_w - u_cam) / 5200.0, 0.0, 1.0);
  vec3 c = mix(v_c * d, vec3(0.09,0.11,0.14), f * f * 0.85);
  /* filet de securite : quoi qu'il arrive en amont (NaN compris), une face
     rend au moins le plancher d'ambiance sur sa teinte la plus sombre. */
  c = max(c, v_c * AMB * 0.30);
  o = vec4(c, 1.0);
}`;
/* passe de DESIGNATION : on encode (etiquette de troncon, gl_VertexID) */
var VSP = `#version 300 es
layout(location=0) in vec3 a_p;
uniform mat4 u_vp; uniform vec3 u_lo, u_span; uniform uint u_tag;
flat out uvec2 v_id;
void main(){
  vec3 w = u_lo + a_p * u_span;
  v_id = uvec2(u_tag, uint(gl_VertexID));
  gl_Position = u_vp * vec4(w.x, w.z, w.y, 1.0);
}`;
var FSP = `#version 300 es
precision highp float; precision highp int;
flat in uvec2 v_id; out uvec4 o;
void main(){ o = uvec4(v_id.x, v_id.y, 0u, 1u); }`;

var VSV = `#version 300 es
layout(location=0) in vec3 a_b;      // sommet du gabarit
layout(location=1) in float a_k;     // 0 = tronc, 1 = houppier
layout(location=2) in vec3 a_i;      // position de l'instance
layout(location=3) in vec2 a_sy;     // (echelle, lacet)
uniform mat4 u_vp; out vec3 v_w; flat out vec3 v_c;
void main(){
  float s = a_sy.x, y = a_sy.y * 0.017453293;
  vec3 p = a_b * vec3(s, s, s);
  float c = cos(y), n = sin(y);
  vec3 r = vec3(p.x*c - p.y*n, p.x*n + p.y*c, p.z) + a_i;
  v_w = r;
  v_c = (a_k > 0.5) ? vec3(0.60,0.74,0.55) : vec3(0.60,0.53,0.45);
  gl_Position = u_vp * vec4(r.x, r.z, r.y, 1.0);
}`;

var P = prog(VS, FS), PP = prog(VSP, FSP), PV = prog(VSV, FS);
var U = {}, UP = {}, UV = {};
['u_vp','u_lo','u_span','u_mode','u_itf','u_pal','u_cam','u_fin'].forEach(function(k){
  U[k] = gl.getUniformLocation(P, k); });
['u_vp','u_lo','u_span','u_tag'].forEach(function(k){
  UP[k] = gl.getUniformLocation(PP, k); });
['u_vp','u_cam'].forEach(function(k){ UV[k] = gl.getUniformLocation(PV, k); });

var PALV = new Float32Array(40 * 3);
(function(){ var F = MQ_INDEX.familles;
  for (var i = 0; i < F.length && i < 32; i++){ var c = PAL[F[i]] || [0.85,0.85,0.85];
    PALV[i*3] = c[0]; PALV[i*3+1] = c[1]; PALV[i*3+2] = c[2]; }
  for (var j = 0; j < 8; j++){ var d = DZC[Math.min(j, DZC.length - 1)].c;
    PALV[(32+j)*3] = d[0]; PALV[(32+j)*3+1] = d[1]; PALV[(32+j)*3+2] = d[2]; }
})();

/* --------------------------------------------------------- LES DONNEES ---- */
function unb64(s){ var b = atob(s), n = b.length, u = new Uint8Array(n);
  for (var i = 0; i < n; i++) u[i] = b.charCodeAt(i); return u; }

var CELLS = {};          // cle -> {etat, chunks:[], veg, F, ids}
var TAGS = [];           // etiquette -> {chunk, cell}
var enCours = 0, octets = 0, tRestant = 0;
var IDX = MQ_INDEX;
var CINFO = {}; IDX.cellules.forEach(function(c){ CINFO[c.c] = c; });

function classeDz(d){ for (var i = 0; i < DZC.length; i++)
  if (d <= DZC[i].max) return i; return 7; }

function MQ_CELLULE(d){
  var t0 = performance.now();
  var C = { cle: d.c, o: d.o, ids: d.ids, F: d.F || {}, couches: {}, veg: null,
            bbox: null, tri: 0, som: 0 };
  var zmin = 1e9, zmax = -1e9;
  for (var nom in d.L){
    if (nom === 'veg') continue;
    var L = d.L[nom], out = [];
    var dz = L.dz ? new Float32Array(unb64(L.dz).buffer) : null;
    var gp = 0;                            // compteur global de plage
    for (var ci = 0; ci < L.k.length; ci++){
      var k = L.k[ci];
      var pos = new Uint16Array(unb64(k.p).buffer);
      var idx = new Uint16Array(unb64(k.i).buffer);
      var rid = new Uint32Array(unb64(k.rid).buffer);
      var rs  = new Uint32Array(unb64(k.rs).buffer);
      var rc  = new Uint32Array(unb64(k.rc).buffer);
      var rf  = unb64(k.rf);
      var hv  = k.hv ? unb64(k.hv) : null;
      var nv = k.nv;
      /* plage -> etendue de SOMMETS (les geometries sont contigues) */
      var vs = new Uint32Array(rid.length), ve = new Uint32Array(rid.length);
      var attr = new Uint8Array(nv * 2);
      for (var r = 0; r < rid.length; r++){
        var a = 0x7fffffff, b = 0;
        var i0 = rs[r] * 3, i1 = i0 + rc[r] * 3;
        for (var q = i0; q < i1; q++){ var v = idx[q];
          if (v < a) a = v; if (v > b) b = v; }
        vs[r] = a; ve[r] = b + 1;
        var g = (nom === 'itf' && dz) ? classeDz(dz[gp + r]) : rf[r];
        for (var w = a; w <= b; w++){ attr[w*2] = rf[r]; attr[w*2+1] = g; }
      }
      gp += rid.length;
      var vao = gl.createVertexArray(); gl.bindVertexArray(vao);
      var bp = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, bp);
      gl.bufferData(gl.ARRAY_BUFFER, pos, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 3, gl.UNSIGNED_SHORT, true, 0, 0);
      var ba = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, ba);
      gl.bufferData(gl.ARRAY_BUFFER, attr, gl.STATIC_DRAW);
      gl.enableVertexAttribArray(1);
      gl.vertexAttribIPointer(1, 2, gl.UNSIGNED_BYTE, 0, 0);
      var bh = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, bh);
      gl.bufferData(gl.ARRAY_BUFFER, hv || new Uint8Array(nv), gl.STATIC_DRAW);
      gl.enableVertexAttribArray(2);
      gl.vertexAttribIPointer(2, 1, gl.UNSIGNED_BYTE, 0, 0);
      var bi = gl.createBuffer();
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, bi);
      gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, idx, gl.STATIC_DRAW);
      gl.bindVertexArray(null);
      var tag = TAGS.length;
      var ch = { vao: vao, n: idx.length, lo: k.q.slice(0,3), span: k.q.slice(3,6),
                 tag: tag, rid: rid, vs: vs, ve: ve, cell: C, couche: nom };
      TAGS.push(ch);
      out.push(ch);
      C.tri += k.nt; C.som += nv;
      zmin = Math.min(zmin, k.q[2]); zmax = Math.max(zmax, k.q[2] + k.q[5]);
    }
    C.couches[nom] = out;
  }
  if (d.L.veg && d.L.veg.n){
    var V = d.L.veg;
    var m = new Float32Array(unb64(V.m).buffer);
    var s = new Float32Array(unb64(V.s).buffer);
    var y = new Float32Array(unb64(V.y).buffer);
    var sy = new Float32Array(V.n * 2);
    for (var i2 = 0; i2 < V.n; i2++){ sy[i2*2] = s[i2]; sy[i2*2+1] = y[i2]; }
    var vao2 = gl.createVertexArray(); gl.bindVertexArray(vao2);
    gl.bindBuffer(gl.ARRAY_BUFFER, GAB.b);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 16, 0);
    gl.enableVertexAttribArray(1);
    gl.vertexAttribPointer(1, 1, gl.FLOAT, false, 16, 12);
    var bi2 = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, bi2);
    gl.bufferData(gl.ARRAY_BUFFER, m, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(2);
    gl.vertexAttribPointer(2, 3, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(2, 1);
    var bs2 = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, bs2);
    gl.bufferData(gl.ARRAY_BUFFER, sy, gl.STATIC_DRAW);
    gl.enableVertexAttribArray(3);
    gl.vertexAttribPointer(3, 2, gl.FLOAT, false, 0, 0);
    gl.vertexAttribDivisor(3, 1);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, GAB.i);
    gl.bindVertexArray(null);
    C.veg = { vao: vao2, n: V.n };
  }
  C.bbox = [d.o[0], d.o[1], zmin, d.o[0] + d.s, d.o[1] + d.s, zmax];
  C.ms = performance.now() - t0;
  CELLS[d.c] = C; enCours--; tRestant--;
  majHud();
}
window.MQ_CELLULE = MQ_CELLULE;

/* gabarit de vegetation : prisme de tronc + bloc de houppier (16 triangles) */
var GAB = (function(){
  var V = [], I = [], n = 0;
  function box(x0,y0,z0,x1,y1,z1,k){
    var p=[[x0,y0,z0],[x1,y0,z0],[x1,y1,z0],[x0,y1,z0],
           [x0,y0,z1],[x1,y0,z1],[x1,y1,z1],[x0,y1,z1]];
    var f=[[0,1,5],[0,5,4],[1,2,6],[1,6,5],[2,3,7],[2,7,6],[3,0,4],[3,4,7],
           [4,5,6],[4,6,7]];
    for(var i=0;i<8;i++){V.push(p[i][0],p[i][1],p[i][2],k);}
    for(var j=0;j<f.length;j++){I.push(n+f[j][0],n+f[j][1],n+f[j][2]);}
    n+=8;
  }
  var t = 0.18, h = 2.2, r = 1.9, ht = 5.0;
  box(-t,-t,0, t,t,h, 0);
  box(-r,-r,h, r,r,ht, 1);
  var b = gl.createBuffer(); gl.bindBuffer(gl.ARRAY_BUFFER, b);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(V), gl.STATIC_DRAW);
  var i2 = gl.createBuffer(); gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, i2);
  gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, new Uint16Array(I), gl.STATIC_DRAW);
  return { b: b, i: i2, n: I.length };
})();

function charger(cle){
  if (CELLS[cle] || CELLS['~' + cle]) return;
  CELLS['~' + cle] = 1; enCours++; tRestant++;
  var s = document.createElement('script');
  s.src = 'cells/mq_' + cle + '.js';
  s.onerror = function(){ enCours--; tRestant--; CELLS[cle] = {vide:1,couches:{},cle:cle}; };
  document.head.appendChild(s);
}

/* ------------------------------------------------------------- CAMERA ----- */
var cam = { x: 0, y: 0, z: 260, yaw: 0, pit: -0.55, v: 60 };
var touches = {}, capt = false;
CV.addEventListener('click', function(e){
  if (!capt){ CV.requestPointerLock(); } else { designer(e); } });
document.addEventListener('pointerlockchange', function(){
  capt = (document.pointerLockElement === CV); });
document.addEventListener('mousemove', function(e){
  if (!capt) return;
  cam.yaw -= e.movementX * 0.0022;
  cam.pit = Math.max(-1.55, Math.min(1.55, cam.pit - e.movementY * 0.0022));
});
document.addEventListener('keydown', function(e){ touches[e.code] = 1;
  if (e.code === 'Escape') document.exitPointerLock(); });
document.addEventListener('keyup', function(e){ touches[e.code] = 0; });
CV.addEventListener('wheel', function(e){
  cam.v = Math.max(4, Math.min(1400, cam.v * (e.deltaY > 0 ? 0.85 : 1.18)));
  e.preventDefault(); }, {passive:false});

function avance(dt){
  var s = cam.v * dt;
  if (touches['ShiftLeft'] || touches['ShiftRight']) s *= 5;
  if (touches['ControlLeft']) s *= 0.2;
  var cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw), cp = Math.cos(cam.pit);
  var fx = sy * cp, fy = cy * cp, fz = Math.sin(cam.pit);
  var rx = cy, ry = -sy;
  var a = 0, b = 0, c = 0;
  if (touches['KeyW'] || touches['KeyZ'] || touches['ArrowUp']) a += 1;
  if (touches['KeyS'] || touches['ArrowDown']) a -= 1;
  if (touches['KeyD'] || touches['ArrowRight']) b += 1;
  if (touches['KeyA'] || touches['KeyQ'] || touches['ArrowLeft']) b -= 1;
  if (touches['KeyE'] || touches['Space']) c += 1;
  if (touches['KeyC']) c -= 1;
  cam.x += (fx * a + rx * b) * s;
  cam.y += (fy * a + ry * b) * s;
  cam.z += (fz * a + c) * s;
}

function matVP(w, h){
  var cy = Math.cos(cam.yaw), sy = Math.sin(cam.yaw);
  var cp = Math.cos(cam.pit), sp = Math.sin(cam.pit);
  /* monde rendu : X=est, Y=altitude, Z=sud */
  var fx = sy * cp, fy = sp, fz = cy * cp;
  var ux = 0, uy = 1, uz = 0;
  var sx = fy*uz - fz*uy, sy2 = fz*ux - fx*uz, sz = fx*uy - fy*ux;
  var l = Math.hypot(sx, sy2, sz) || 1; sx/=l; sy2/=l; sz/=l;
  var vx = sy2*fz - sz*fy, vy = sz*fx - sx*fz, vz = sx*fy - sy2*fx;
  var ex = cam.x, ey = cam.z, ez = cam.y;
  var V = [sx, vx, -fx, 0,  sy2, vy, -fy, 0,  sz, vz, -fz, 0,
           -(sx*ex+sy2*ey+sz*ez), -(vx*ex+vy*ey+vz*ez), (fx*ex+fy*ey+fz*ez), 1];
  var n = 0.6, f = 14000, fov = 1.15, t = 1 / Math.tan(fov/2), as = w/h;
  var Pm = [t/as,0,0,0, 0,t,0,0, 0,0,(f+n)/(n-f),-1, 0,0,2*f*n/(n-f),0];
  var M = new Float32Array(16);
  for (var i = 0; i < 4; i++) for (var j = 0; j < 4; j++){
    var s2 = 0; for (var k = 0; k < 4; k++) s2 += V[i*4+k] * Pm[k*4+j];
    M[i*4+j] = s2; }
  return M;
}

function plans(M){
  var p = [], m = M;
  function pl(a,b,c,d){ var l = Math.hypot(a,b,c) || 1;
    p.push([a/l,b/l,c/l,d/l]); }
  pl(m[3]+m[0], m[7]+m[4], m[11]+m[8], m[15]+m[12]);
  pl(m[3]-m[0], m[7]-m[4], m[11]-m[8], m[15]-m[12]);
  pl(m[3]+m[1], m[7]+m[5], m[11]+m[9], m[15]+m[13]);
  pl(m[3]-m[1], m[7]-m[5], m[11]-m[9], m[15]-m[13]);
  pl(m[3]-m[2], m[7]-m[6], m[11]-m[10], m[15]-m[14]);
  return p;
}
function visible(pl, bb){
  /* bb en repere plan (x,y=sud,z=alt) -> rendu (x, z, y) */
  var x0=bb[0], y0=bb[2], z0=bb[1], x1=bb[3], y1=bb[5], z1=bb[4];
  for (var i = 0; i < pl.length; i++){
    var q = pl[i];
    var px = q[0] > 0 ? x1 : x0, py = q[1] > 0 ? y1 : y0, pz = q[2] > 0 ? z1 : z0;
    if (q[0]*px + q[1]*py + q[2]*pz + q[3] < 0) return false;
  }
  return true;
}

/* ------------------------------------------------------------ INTERFACE --- */
var actif = {}; COUCHES.forEach(function(c){ actif[c[0]] = c[2]; });
var mode = 0, densite = 1.0, rayon = 1200, autoc = true;
var FIN_M = 2.5;   // hauteur d'ailette de la carte des marches (AFFICHAGE seul)

(function(){
  var d = document.getElementById('couches');
  COUCHES.forEach(function(c){
    var l = document.createElement('label');
    l.innerHTML = '<input type="checkbox" ' + (c[2]?'checked':'') + '> ' + c[1];
    l.firstChild.onchange = function(){ actif[c[0]] = this.checked; };
    d.appendChild(l);
  });
  document.getElementById('mode').onchange = function(){
    mode = {fam:0, dz:1, blanc:2}[this.value]; legende(); };
  document.getElementById('rayon').oninput = function(){
    rayon = +this.value; document.getElementById('rv').textContent = rayon; };
  document.getElementById('dens').oninput = function(){
    densite = (+this.value)/100; document.getElementById('dv').textContent = this.value; };
  document.getElementById('auto').onchange = function(){ autoc = this.checked; };
  document.getElementById('tout').onclick = function(){
    autoc = false; document.getElementById('auto').checked = false;
    IDX.cellules.forEach(function(c){ charger(c.c); }); };
  document.getElementById('banc').onclick = banc;
  var s = IDX.signets, f = document.getElementById('filtre'), vus = {};
  s.forEach(function(g){ if (!vus[g.regle]){ vus[g.regle] = 1;
    var o = document.createElement('option'); o.value = g.regle;
    o.textContent = g.regle; f.appendChild(o); } });
  f.onchange = listeSignets;
  document.getElementById('nsig').textContent = s.length;
  listeSignets();
  legende();
  document.getElementById('sub').innerHTML =
    IDX.plan + ' — ' + (IDX.totaux.triangles/1e6).toFixed(2) + ' M triangles, '
    + IDX.totaux.instances.toLocaleString('fr') + ' instances, '
    + IDX.cellules.length + ' cellules';
})();

function listeSignets(){
  var f = document.getElementById('filtre').value;
  var d = document.getElementById('sigl'); d.innerHTML = '';
  IDX.signets.forEach(function(g, i){
    if (f && g.regle !== f) return;
    var e = document.createElement('div');
    e.className = 'sig';
    e.innerHTML = '<b>' + (i+1) + '. ' + g.titre + '</b><i>' + g.regle
      + (g.detail ? ' — ' + g.detail : '') + '</i>';
    e.onclick = function(){ allerA(g); };
    d.appendChild(e);
  });
}
function allerA(g){
  cam.x = g.x - 55; cam.y = g.y + 55; cam.z = (g.z_m || 150) + 45;
  cam.yaw = Math.PI * 0.75; cam.pit = -0.42;
  var cx = Math.floor(g.x / IDX.cellule_m), cy = Math.floor(g.y / IDX.cellule_m);
  for (var a = -1; a <= 1; a++) for (var b = -1; b <= 1; b++)
    if (CINFO[(cx+a) + '_' + (cy+b)]) charger((cx+a) + '_' + (cy+b));
  fiche({t:'signet', g:g});
}
function legende(){
  var d = document.getElementById('leg'); d.innerHTML = '';
  function ligne(c, t){ var e = document.createElement('div'); e.className='leg';
    e.innerHTML = '<span class="sw" style="background:rgb(' +
      Math.round(c[0]*255)+','+Math.round(c[1]*255)+','+Math.round(c[2]*255)+
      ')"></span>' + t; d.appendChild(e); }
  if (mode === 1){ DZC.forEach(function(k){ ligne(k.c, k.t); }); }
  else if (mode === 0){
    ['batiment','chaussee','trottoir','carrefour','sol_mineral','sol_vegetal',
     'eau_surface','pont','dalot','mur_sout','escalier','voie_ferree','canal',
     'terrassement'].forEach(function(f){ ligne(PAL[f], f); });
  }
}

/* --------------------------------------------------------- DESIGNATION ---- */
var FBO = null, TEX = null, RBO = null;
function pickInit(){
  FBO = gl.createFramebuffer(); gl.bindFramebuffer(gl.FRAMEBUFFER, FBO);
  TEX = gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D, TEX);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32UI, 1, 1);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
                          gl.TEXTURE_2D, TEX, 0);
  RBO = gl.createRenderbuffer(); gl.bindRenderbuffer(gl.RENDERBUFFER, RBO);
  gl.renderbufferStorage(gl.RENDERBUFFER, gl.DEPTH_COMPONENT24, 1, 1);
  gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_ATTACHMENT,
                             gl.RENDERBUFFER, RBO);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
}
function designer(ev){
  if (!FBO) pickInit();
  var w = CV.width, h = CV.height;
  /* la designation doit suivre LA MEME echelle que le rendu : on prend le
     rapport tampon/boite affichee, et non `devicePixelRatio` en dur (le
     tampon peut etre borne par MAX_VIEWPORT_DIMS, et la boite peut etre
     decalee). */
  var r = CV.getBoundingClientRect();
  var sx = w / (r.width || w), sy = h / (r.height || h);
  var px = (ev.clientX - r.left) * sx, py = h - (ev.clientY - r.top) * sy;
  if (capt){ px = w/2; py = h/2; }
  gl.bindFramebuffer(gl.FRAMEBUFFER, FBO);
  gl.viewport(0, 0, 1, 1);
  gl.clearBufferuiv(gl.COLOR, 0, new Uint32Array([0xffffffff,0,0,0]));
  gl.clear(gl.DEPTH_BUFFER_BIT);
  gl.enable(gl.DEPTH_TEST);
  gl.useProgram(PP);
  var MP = matPick(w, h, px, py);
  gl.uniformMatrix4fv(UP.u_vp, false, MP);
  var pl = plans(MP);
  for (var c in CELLS){ var C = CELLS[c];
    if (!C || C.vide || !C.bbox || !visible(pl, C.bbox)) continue;
    for (var nom in C.couches){ if (!actif[nom]) continue;
      C.couches[nom].forEach(function(k){
        gl.uniform3fv(UP.u_lo, k.lo); gl.uniform3fv(UP.u_span, k.span);
        gl.uniform1ui(UP.u_tag, k.tag);
        gl.bindVertexArray(k.vao);
        gl.drawElements(gl.TRIANGLES, k.n, gl.UNSIGNED_SHORT, 0); });
    }
  }
  var out = new Uint32Array(4);
  gl.readPixels(0, 0, 1, 1, gl.RGBA_INTEGER, gl.UNSIGNED_INT, out);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.bindVertexArray(null);
  if (out[0] === 0xffffffff || out[0] >= TAGS.length){ fiche(null); return; }
  var ch = TAGS[out[0]], v = out[1];
  var lo = 0, hi = ch.rid.length - 1, hit = -1;
  while (lo <= hi){ var m = (lo + hi) >> 1;
    if (v < ch.vs[m]) hi = m - 1; else if (v >= ch.ve[m]) lo = m + 1;
    else { hit = m; break; } }
  if (hit < 0){ fiche(null); return; }
  var id = ch.cell.ids[ch.rid[hit]];
  fiche({ t:'objet', id:id, couche:ch.couche, cell:ch.cell,
          F: ch.cell.F[id] });
}
function matPick(w, h, px, py){
  /* meme camera, mais projection ramenee au pixel (px,py) : une fenetre 1x1 */
  var M = matVP(w, h), S = new Float32Array(16);
  var ax = w / 1, ay = h / 1;
  var tx = (w - 2 * px - 1) / 1, ty = (h - 2 * py - 1) / 1;
  for (var i = 0; i < 4; i++){
    S[i*4+0] = M[i*4+0] * ax + M[i*4+3] * tx;
    S[i*4+1] = M[i*4+1] * ay + M[i*4+3] * ty;
    S[i*4+2] = M[i*4+2];
    S[i*4+3] = M[i*4+3];
  }
  return S;
}

function nb(v, u, d){ return (v === undefined || v === null) ? '—'
  : (typeof v === 'number' ? v.toFixed(d === undefined ? 2 : d) : v)
    + (u ? ' ' + u : ''); }
function fiche(o){
  var d = document.getElementById('fiche');
  if (!o){ d.style.display = 'none'; return; }
  var h = '';
  if (o.t === 'signet'){
    h = '<h1>SIGNET DE TOURNEE</h1><p class="sub">' + o.g.regle + '</p>'
      + '<div class="kv"><span>titre</span><b>' + o.g.titre + '</b></div>'
      + '<div class="kv"><span>detail</span><b>' + (o.g.detail||'—') + '</b></div>'
      + '<div class="kv"><span>x, y</span><b>' + o.g.x.toFixed(1) + ', '
      + o.g.y.toFixed(1) + '</b></div>';
  } else {
    var F = o.F || {};
    h = '<h1>' + o.id + '</h1><p class="sub">couche ' + o.couche
      + ' — cellule ' + o.cell.cle + '</p>';
    function kv(k, v){ h += '<div class="kv"><span>' + k + '</span><b>' + v
      + '</b></div>'; }
    if (F.t === 'parcelle'){
      kv('famille', F.fam); kv('proprietaire', F.pro); kv('matiere', F.mat);
      kv('LOI DE Z', F.forme);
      if (F.forme === 'constante') kv('cote', nb(F.z, 'm NGF', 3));
      if (F.forme === 'profil_troncon'){ kv('cote moyenne', nb(F.z,'m NGF',3));
        kv('longueur', nb(F.L,'m')); kv('pente max', nb(F.pente,'%')); }
      if (F.forme === 'drapage') kv('cote', 'drapee sur le releve (le plan n\'ecrit aucun Z)');
      kv('aire (piece)', nb(F.a,'m²')); kv('aire (entiere)', nb(F.at,'m²'));
      kv('cellule porteuse', F.cel); kv('entiere', F.ent ? 'oui':'non');
      if (F.larg !== undefined) kv('largeur', nb(F.larg,'m'));
      if (F.her) kv('LOI HERITEE DE', F.her);
      if (F.ass) { h += '<h2>Assiette</h2>';
        kv('cote reglee', nb(F.ass[0],'m NGF',3)); kv('cote', F.ass[1]);
        h += '<div style="font-size:11px;color:#8d97a6;padding-top:4px">'
          + (F.ass[2]||'') + '</div>'; }
      if (F.prov){ h += '<h2>Provenance</h2><div style="font-size:11px;'
        + 'color:#8d97a6">' + F.prov + '</div>'; }
    } else if (F.t === 'interface'){
      kv('RESOLUTION', F.res); kv('dZ median', nb(F.dz,'m',3));
      kv('dZ max', nb(F.dzx,'m',3)); kv('hauteur de piece', nb(F.h,'m',3));
      kv('longueur', nb(F.m,'m')); kv('matieres', (F.mt||[]).join(' | '));
      kv('cote A', F.a); kv('cote B', F.b);
      h += '<div style="font-size:11px;color:#8d97a6;padding-top:6px">'
        + 'catalogue ferme du plan : rien / affleurement / bordure / '
        + 'emmarchement / mur / talus</div>';
    } else if (F.t === 'terrassement'){
      kv('PIECE', F.piece); kv('dZ', nb(F.dz,'m',3));
      kv('largeur', nb(F.larg,'m')); kv('longueur', nb(F.m,'m'));
      kv('place disponible', nb(F.place,'%',1));
      kv('element regle', F.a); kv('terrain', F.b); kv('famille', F.fam);
    } else { kv('(aucune fiche portee par le contrat)', ''); }
  }
  d.innerHTML = h; d.style.display = 'block';
}

/* ------------------------------------------------------------- RENDU ------ */
var fps = 0, acc = 0, cadres = 0, tPrec = performance.now(), dessines = 0;
var mesure = null;

function majHud(){
  var n = 0, tri = 0, ins = 0;
  for (var c in CELLS){ var C = CELLS[c];
    if (C && !C.vide && C.bbox){ n++; tri += C.tri; ins += C.veg ? C.veg.n : 0; } }
  document.getElementById('hud').innerHTML =
    '<b>' + fps.toFixed(0) + ' fps</b> &nbsp; ' + n + '/' + IDX.cellules.length
    + ' cellules &nbsp; ' + (tri/1e6).toFixed(2) + ' M triangles &nbsp; '
    + ins.toLocaleString('fr') + ' instances<br>'
    + 'x ' + cam.x.toFixed(0) + '  y ' + cam.y.toFixed(0) + '  z '
    + cam.z.toFixed(0) + ' m NGF &nbsp; vitesse ' + cam.v.toFixed(0) + ' m/s'
    + (enCours ? ' &nbsp; <span class="warn">chargement ' + enCours + '…</span>' : '')
    + (mesure ? '<br><span class="warn">' + mesure + '</span>' : '');
}

/* DIMENSIONNEMENT DE LA VUE — le defaut remonte par l'utilisateur.
   Le tampon de rendu (`width`/`height` de l'element) doit valoir la taille
   AFFICHEE en pixels CSS multipliee par le devicePixelRatio, sinon l'image
   est soit floue, soit confinee dans un coin. Trois precautions :
     * on mesure la boite REELLE (getBoundingClientRect, qui donne des
       fractions — clientWidth arrondit), avec repli sur la fenetre si la
       mise en page ne donne encore aucune taille ;
     * on suit le devicePixelRatio, qui change quand la fenetre passe d'un
       ecran a l'autre ou quand l'utilisateur zoome ;
     * on borne au maximum de texture du pilote, sinon un grand ecran en DPR 2
       depasse silencieusement et le rendu tombe.
   `gl.viewport` et la matrice de projection sont recalcules sur CES valEURS :
   `rendre(w, h)` les recoit, et `matVP(w, h)` en tire le rapport d'image. */
var MAXVP = null;
function dimensionner(){
  var r = CV.getBoundingClientRect();
  var cw = r.width || CV.clientWidth || window.innerWidth || 1600;
  var ch = r.height || CV.clientHeight || window.innerHeight || 900;
  var dpr = window.devicePixelRatio || 1;
  if (MAXVP === null){
    var v = gl.getParameter(gl.MAX_VIEWPORT_DIMS);
    MAXVP = Math.min(v[0], v[1], gl.getParameter(gl.MAX_TEXTURE_SIZE)) || 8192;
  }
  var w = Math.max(1, Math.min(MAXVP, Math.round(cw * dpr)));
  var h = Math.max(1, Math.min(MAXVP, Math.round(ch * dpr)));
  if (CV.width !== w || CV.height !== h){ CV.width = w; CV.height = h; }
  return [w, h];
}
window.addEventListener('resize', dimensionner);
/* un changement d'ecran change le DPR sans declencher `resize` */
if (window.matchMedia){
  var mq = window.matchMedia('(resolution: 1dppx)');
  if (mq.addEventListener) mq.addEventListener('change', dimensionner);
}
window.MQ_TAILLE = function(){
  var r = CV.getBoundingClientRect();
  return { css: [Math.round(r.width), Math.round(r.height)],
           tampon: [CV.width, CV.height],
           dpr: window.devicePixelRatio || 1,
           fenetre: [window.innerWidth, window.innerHeight],
           remplit: (Math.abs(r.width - window.innerWidth) <= 1 &&
                     Math.abs(r.height - window.innerHeight) <= 1) };
};

function boucle(){
  var t = performance.now(), dt = Math.min(0.1, (t - tPrec)/1000); tPrec = t;
  acc += dt; cadres++;
  if (acc > 0.5){ fps = cadres/acc; acc = 0; cadres = 0; majHud(); }
  avance(dt);
  if (autoc){
    var cx = Math.floor(cam.x / IDX.cellule_m), cy = Math.floor(cam.y / IDX.cellule_m);
    var r = Math.ceil(rayon / IDX.cellule_m);
    for (var a = -r; a <= r; a++) for (var b = -r; b <= r; b++){
      var k = (cx+a) + '_' + (cy+b);
      if (CINFO[k] && !CELLS[k] && !CELLS['~'+k] && enCours < 3) charger(k);
    }
  }
  var d = dimensionner();
  rendre(d[0], d[1]);
  if (banc.actif) bancPas(t);
  requestAnimationFrame(boucle);
}

function rendre(w, h){
  gl.viewport(0, 0, w, h);
  gl.clearColor(CIEL[0], CIEL[1], CIEL[2], 1); gl.enable(gl.DEPTH_TEST);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  var M = matVP(w, h), pl = plans(M);
  /* LE SOCLE d'abord : dalle neutre sous tout le domaine, pour qu'un manque
     ne se lise pas comme une face noire. Hors totaux du contrat. */
  if (SOCLE_ON){
    if (!SOCLE) socleInit();
    gl.useProgram(SOCLE.prog);
    gl.uniformMatrix4fv(SOCLE.u_vp, false, M);
    gl.uniform1f(SOCLE.u_z, socleZ());
    gl.uniform3f(SOCLE.u_cam, cam.x, cam.z, cam.y);
    gl.bindVertexArray(SOCLE.vao);
    gl.drawArrays(gl.TRIANGLES, 0, 6);
    gl.bindVertexArray(null);
  }
  gl.useProgram(P);
  gl.uniformMatrix4fv(U.u_vp, false, M);
  gl.uniform1i(U.u_mode, mode);
  gl.uniform3fv(U.u_pal, PALV);
  gl.uniform3f(U.u_cam, cam.x, cam.z, cam.y);
  dessines = 0;
  var vegs = [];
  for (var c in CELLS){ var C = CELLS[c];
    if (!C || C.vide || !C.bbox || !visible(pl, C.bbox)) continue;
    for (var nom in C.couches){ if (!actif[nom]) continue;
      var L = C.couches[nom];
      gl.uniform1i(U.u_itf, nom === 'itf' ? 1 : 0);
      gl.uniform1f(U.u_fin, (mode === 1 && nom === 'itf') ? FIN_M : 0.0);
      for (var i = 0; i < L.length; i++){ var k = L[i];
        gl.uniform3fv(U.u_lo, k.lo); gl.uniform3fv(U.u_span, k.span);
        gl.bindVertexArray(k.vao);
        gl.drawElements(gl.TRIANGLES, k.n, gl.UNSIGNED_SHORT, 0);
        dessines++;
      }
    }
    if (actif.veg && C.veg) vegs.push(C.veg);
  }
  if (vegs.length && densite > 0){
    gl.useProgram(PV);
    gl.uniformMatrix4fv(UV.u_vp, false, M);
    gl.uniform3f(UV.u_cam, cam.x, cam.z, cam.y);
    for (var v = 0; v < vegs.length; v++){
      gl.bindVertexArray(vegs[v].vao);
      gl.drawElementsInstanced(gl.TRIANGLES, GAB.n, gl.UNSIGNED_SHORT, 0,
                               Math.round(vegs[v].n * densite));
      dessines++;
    }
  }
  gl.bindVertexArray(null);
}

/* PRISE DE VUE REPRODUCTIBLE : on pose la camera par ses coordonnees du plan,
   on charge les cellules autour, on rend a la taille demandee, puis la page
   se photographie elle-meme sur le disque (POST /capture/<nom>.png, servi par
   mq_serveur.py). Aucune capture d'ecran a la main : la vue est CHIFFREE, donc
   rejouable a l'identique. */
window.MQ_VUE = function(o){
  o = o || {};
  if (o.x !== undefined) cam.x = o.x;
  if (o.y !== undefined) cam.y = o.y;
  if (o.z !== undefined) cam.z = o.z;
  if (o.yaw !== undefined) cam.yaw = o.yaw;
  if (o.pit !== undefined) cam.pit = o.pit;
  if (o.mode !== undefined){ mode = o.mode; legende(); }
  if (o.densite !== undefined) densite = o.densite;
  if (o.couches) for (var k in o.couches) actif[k] = !!o.couches[k];
  var r = o.rayon || 900;
  var cm = IDX.cellule_m;
  var n = Math.ceil(r / cm);
  var cx = Math.floor(cam.x / cm), cy = Math.floor(cam.y / cm), dem = 0;
  for (var a = -n; a <= n; a++) for (var b = -n; b <= n; b++)
    if (CINFO[(cx+a) + '_' + (cy+b)]){ charger((cx+a) + '_' + (cy+b)); dem++; }
  return { demandees: dem, enCours: enCours, restant: tRestant,
           cam: {x:cam.x, y:cam.y, z:cam.z, yaw:cam.yaw, pit:cam.pit} };
};
window.MQ_PRET = function(){ return enCours === 0 && tRestant === 0; };
window.MQ_CAPTURE = function(nom, w, h){
  w = w || 1400; h = h || 800;
  if (CV.width !== w || CV.height !== h){ CV.width = w; CV.height = h; }
  rendre(w, h);
  return new Promise(function(res, rej){
    CV.toBlob(function(b){
      var x = new XMLHttpRequest();
      x.open('POST', '/capture/' + nom + '.png');
      x.onload = function(){ res(x.responseText); };
      x.onerror = function(){ rej('POST refuse'); };
      x.send(b);
    }, 'image/png');
  });
};
/* AUDIT DU PLANCHER D'AMBIANCE : on relit le tampon rendu et on compte les
   pixels plus sombres que le CIEL. Le ciel est la couleur d'effacement
   (0.055,0.065,0.08) ; un pixel de GEOMETRIE plus sombre que lui ne peut venir
   que d'un eclairement passe sous le plancher (ou d'un NaN). Le compte doit
   etre ZERO. C'est la mesure qui remplace « ca a l'air mieux ». */
window.MQ_AUDIT_NOIR = function(w, h){
  w = w || 1400; h = h || 800;
  if (CV.width !== w || CV.height !== h){ CV.width = w; CV.height = h; }
  rendre(w, h);
  var px = new Uint8Array(w * h * 4);
  gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
  var ciel = [Math.round(0.055*255), Math.round(0.065*255), Math.round(0.08*255)];
  var noir = 0, sousCiel = 0, cielN = 0, n = w * h, minL = 255;
  for (var i = 0; i < n; i++){
    var r = px[i*4], g = px[i*4+1], b = px[i*4+2];
    if (r === ciel[0] && g === ciel[1] && b === ciel[2]){ cielN++; continue; }
    var l = Math.max(r, Math.max(g, b));
    if (l < minL) minL = l;
    if (r === 0 && g === 0 && b === 0) noir++;
    if (l < ciel[2]) sousCiel++;
  }
  return { pixels: n, ciel: cielN, geometrie: n - cielN,
           noir_pur: noir, plus_sombre_que_le_ciel: sousCiel,
           luminance_min_geometrie: minL };
};

/* COMPTAGE DES TROUS — la cible posee par le coordinateur : ZERO pixel de
   fond entoure de geometrie. On rend avec le revelateur, puis colonne par
   colonne on cherche le premier pixel de geometrie en partant du haut ; tout
   pixel de FOND situe SOUS lui est un trou (le ciel, lui, est au-dessus de la
   ligne d'horizon de sa colonne). C'est la mesure qui fait foi, pas l'oeil. */
window.MQ_AUDIT_TROUS = function(w, h){
  w = w || 1400; h = h || 800;
  MQ_TROUS(true);
  if (CV.width !== w || CV.height !== h){ CV.width = w; CV.height = h; }
  rendre(w, h);
  var px = new Uint8Array(w * h * 4);
  gl.readPixels(0, 0, w, h, gl.RGBA, gl.UNSIGNED_BYTE, px);
  MQ_TROUS(false);
  /* readPixels rend la premiere ligne EN BAS : on balaie du haut (y=h-1). */
  var fond = 0, trous = 0, ciel = 0;
  for (var x = 0; x < w; x++){
    var vu = false;
    for (var y = h - 1; y >= 0; y--){
      var o = (y * w + x) * 4;
      if (px[o] > 240 && px[o+1] < 40 && px[o+2] > 200){
        fond++; if (vu) trous++; else ciel++;
      } else vu = true;
    }
  }
  return { fond_total: fond, ciel: ciel, TROUS: trous,
           pc_image: +(100 * trous / (w * h)).toFixed(3) };
};

/* MESURE EXPLICITE : on pilote nous-memes N images le long d'un vol, avec
   gl.finish() a chaque image. Le resultat est un TEMPS PAR IMAGE reel, sans
   le plafond de la synchro verticale et sans dependre de la compositions de
   la fenetre. C'est la mesure que rapporte le lot. */
window.MQ_BANC = function(n, secondes, w, h){
  n = n || 240; secondes = secondes || 20;
  w = w || 1600; h = h || 900;
  if (CV.width !== w || CV.height !== h){ CV.width = w; CV.height = h; }
  var ms = [], tri = 0, cel = 0, ins = 0;
  for (var c in CELLS){ var C = CELLS[c];
    if (C && !C.vide && C.bbox){ cel++; tri += C.tri; ins += C.veg ? C.veg.n : 0; } }
  var sx = cam.x, sy = cam.y, sz = cam.z, syaw = cam.yaw, spit = cam.pit;
  MQ_BANC.px = MQ_BANC.px || new Uint8Array(4);
  rendre(w, h); gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE,
                              MQ_BANC.px);          // amorcage
  for (var i = 0; i < n; i++){
    var a = i / n * Math.PI * 2;
    cam.x = Math.cos(a) * 900; cam.y = Math.sin(a) * 900; cam.z = 320;
    cam.yaw = -a + Math.PI / 2; cam.pit = -0.38;
    var t0 = performance.now();
    rendre(w, h);
    /* readPixels BLOQUE jusqu'a ce que le GPU ait fini : c'est lui qui rend
       la mesure honnete (gl.finish() n'est pas bloquant dans ce navigateur) */
    gl.readPixels(0, 0, 1, 1, gl.RGBA, gl.UNSIGNED_BYTE, MQ_BANC.px);
    ms.push(performance.now() - t0);
  }
  cam.x = sx; cam.y = sy; cam.z = sz; cam.yaw = syaw; cam.pit = spit;
  ms.sort(function(p, q){ return p - q; });
  var q = function(f){ return ms[Math.min(ms.length - 1,
                                          Math.floor(ms.length * f))]; };
  return { images: n, largeur: w, hauteur: h, cellules: cel,
           triangles: tri, instances: ins, appels: dessines,
           ms_median: +q(0.5).toFixed(2), ms_p95: +q(0.95).toFixed(2),
           ms_max: +ms[ms.length-1].toFixed(2),
           fps_median: +(1000/q(0.5)).toFixed(1),
           fps_1pc_bas: +(1000/q(0.99)).toFixed(1) };
};

/* ------------------------------------------------------- BANC DE MESURE --- */
function banc(){
  if (banc.actif) return;
  banc.actif = true; banc.t0 = performance.now(); banc.n = 0; banc.min = 1e9;
  banc.hist = []; banc.tp = banc.t0;
  banc.x0 = cam.x; banc.y0 = cam.y; banc.z0 = cam.z;
  mesure = 'banc en cours…';
}
function bancPas(t){
  var e = (t - banc.t0) / 1000;
  var dt = (t - banc.tp) / 1000; banc.tp = t;
  if (dt > 0){ banc.n++; banc.hist.push(1/dt); banc.min = Math.min(banc.min, 1/dt); }
  /* vol circulaire de 20 s au-dessus du domaine */
  var a = e / 20 * Math.PI * 2;
  cam.x = Math.cos(a) * 900; cam.y = Math.sin(a) * 900; cam.z = 320;
  cam.yaw = -a + Math.PI/2; cam.pit = -0.38;
  if (e >= 20){
    banc.actif = false;
    banc.hist.sort(function(p,q){ return p - q; });
    var md = banc.hist[banc.hist.length >> 1];
    var p1 = banc.hist[Math.floor(banc.hist.length * 0.01)];
    var tri = 0, n = 0;
    for (var c in CELLS){ var C = CELLS[c];
      if (C && !C.vide && C.bbox){ n++; tri += C.tri; } }
    mesure = 'BANC 20 s — median ' + md.toFixed(1) + ' fps, 1%% bas '
      + p1.toFixed(1) + ' fps, min ' + banc.min.toFixed(1)
      + ' — ' + n + ' cellules, ' + (tri/1e6).toFixed(2) + ' M triangles, '
      + dessines + ' appels de dessin';
    console.log(mesure);
    cam.x = banc.x0; cam.y = banc.y0; cam.z = banc.z0;
  }
}

/* ---------------------------------------------------------- DEMARRAGE ----- */
(function(){
  var s = IDX.signets[0];
  cam.x = (s ? s.x : 0); cam.y = (s ? s.y : 0); cam.z = 300;
  IDX.cellules.slice(0, 4).forEach(function(c){});
  charger('0_0'); charger('0_-1'); charger('-1_0'); charger('-1_-1');
  majHud();
  requestAnimationFrame(boucle);
})();
