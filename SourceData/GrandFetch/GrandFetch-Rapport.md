# GRAND FETCH Toulouse — rapport d'acquisition (campagne du 2026-07-25)

Campagne unique d'acquisition de données ouvertes IGN/OSM pour Survol (UE 5.8 desktop).

## Conventions
- Origine locale : lat0=43.6045, lon0=1.4442 (place du Capitole).
- Projection équirectangulaire locale : X_m = (lon−lon0)×80608.782 (X=est) ;
  Y_m = (lat0−lat)×110540 (**NORD = −Y**, chiralité Unreal main gauche). Mètres, 2 décimales.
- Bbox standard ±5000 m autour de l'origine (aérodrome : ±9000 m, voir sa section).
- Sources : WFS Géoplateforme `https://data.geopf.fr/wfs/ows` (BD TOPO v3, ADMIN EXPRESS,
  RPG, BD Forêt) sous **Licence Ouverte 2.0** (Etalab/IGN) ; OpenStreetMap via Overpass
  sous **ODbL 1.0** (attribution « © les contributeurs d'OpenStreetMap » obligatoire).
- Inventaire des couches serveur : `wfs_couches.txt` (794 couches, GetCapabilities du 2026-07-25).

## Découvertes de l'inventaire (étape 0)
- Pas de couche BD TOPO « gare » ni « parking » dédiée : c'est `BDTOPO_V3:equipement_de_transport`
  (filtrée par `nature`) qui sert aux deux (ferre.json et equipements.json).
- RPG disponible en `RPG.LATEST:parcelles_graphiques` ; BD Forêt v2 en
  `LANDCOVER.FORESTINVENTORY.V2:formation_vegetale` ; haies en `BDTOPO_V3:haie` (couche récente confirmée).
- Communes : `ADMINEXPRESS-COG.LATEST:commune` (millésimes 2017→2026 disponibles, LATEST retenu).
- Aérodrome : seules `BDTOPO_V3:aerodrome` et `BDTOPO_V3:piste_d_aerodrome` existent —
  pas de couche « zone » associée côté BD TOPO.

Les sections ci-dessous sont ajoutées AU FIL DE L'EAU par les scripts `Fetch-GF-*.ps1`
(ordre = ordre de fin d'exécution, chaînes WFS et OSM en parallèle). Prose des sections
sans accents (scripts ASCII pur, piège PowerShell 5.1) ; les valeurs témoins, elles,
sont les données brutes UTF-8.

## rues_nommees.json -- rues nommees
- Source : OpenStreetMap via Overpass (way[highway][name]), fetch du 2026-07-25, licence ODbL 1.0.
- Objets : 14054 ways nommes (30 proposed/construction ecartes).
- Taille : 2.47 Mo.
- Taux de remplissage :
  - name : 14054 / 14054 (100.0 %) (filtre de la requete)
  - ref : 1564 / 14054 (11.1 %)
- Bornes : X [-5451.01 ; 5732.02] m, Y [-5759.79 ; 6713.9] m (98512 pts)
- Temoins :
  - name=Périphérique Extérieur t=motorway pts=2 X=-1805.37 Y=3933.91
  - name=Périphérique Extérieur t=motorway pts=2 X=-1828.53 Y=3940.87
  - (accent) name=Périphérique Extérieur t=motorway ref=A 620 X=-1805.37 Y=3933.91

## eclairage.json -- lampadaires
- Source : OpenStreetMap via Overpass (node[highway=street_lamp]), fetch du 2026-07-25, licence ODbL 1.0.
- Objets : 3625 lampadaires (points seuls, dedup par id). Densite moyenne 36.2 / km2.
- Taille : 0.06 Mo.
- Taux de remplissage : sans objet (points nus par design, aucun attribut conserve).
- Bornes : X [-4999.86 ; 4991.73] m, Y [-4995.42 ; 4992.07] m (3625 pts)
- Temoins : 3 premiers points du fichier (pas de champ nominal sur ce theme).
- ATTENTION builders : la couverture street_lamp d'OSM est VOLONTAIRE (crowdsourcee), donc heterogene par quartier -- une densite faible dans un secteur ne veut pas dire absence de lampadaires reels.

## tunnels.json -- tunnels et passages couverts
- Source : OpenStreetMap via Overpass (way highway tunnel/covered=yes + way railway tunnel=yes), fetch du 2026-07-25, licence ODbL 1.0.
- Objets : 702 ways dont 47 ferroviaires (34 tags subway -- metro lignes A/B).
- Taille : 0.1 Mo.
- Taux de remplissage :
  - layer != 0 : 272 / 702 (38.7 %)
  - name : 132 / 702 (18.8 %)
- Bornes : X [-8780.87 ; 4941] m, Y [-4601.61 ; 5401.79] m (3528 pts)
- Temoins :
  - cat=railway t=rail layer=-1 name=Ligne de Bordeaux-Saint-Jean à Sète-Ville pts=3
  - cat=highway t=primary layer=-1 name=Rue du Faubourg Bonnefoy pts=2
  - (accent) cat=railway t=rail layer=-1 name=Ligne de Bordeaux-Saint-Jean à Sète-Ville
- Ces ways sont l'exact complement du filtre du fetch routes historique (toulouse10.json ne les contient pas).

## bati_enrichi.json -- batiments enrichis
- Source : WFS Geoplateforme BDTOPO_V3:batiment (BD TOPO v3), fetch du 2026-07-25, Licence Ouverte 2.0 (IGN).
- Objets : 131357 batiments En service retenus sur 131575 lus (94 hors service ecartes, 124 sans geometrie exploitable, 21 anneaux decimes au plafond de 120 pts).
- Taille : 58.9 Mo.
- Taux de remplissage des attributs (batiments retenus) :
  - nature : 131357 / 131357 (100.0 %)
  - usage_1 : 131357 / 131357 (100.0 %)
  - usage_2 : 19574 / 131357 (14.9 %)
  - construction_legere : 131357 / 131357 (100.0 %)
  - etat_de_l_objet : 131357 / 131357 (100.0 %)
  - nombre_de_logements : 71612 / 131357 (54.5 %)
  - nombre_d_etages : 71612 / 131357 (54.5 %)
  - materiaux_des_murs : 62889 / 131357 (47.9 %)
  - materiaux_de_la_toiture : 62893 / 131357 (47.9 %)
  - hauteur : 130236 / 131357 (99.1 %)
  - altitude_minimale_sol : 130465 / 131357 (99.3 %)
  - altitude_minimale_toit : 125229 / 131357 (95.3 %)
  - altitude_maximale_toit : 120907 / 131357 (92.0 %)
  - altitude_maximale_sol : 119885 / 131357 (91.3 %)
  - origine_du_batiment : 131357 / 131357 (100.0 %)
- Bornes : X [-5235.41 ; 5078.49] m, Y [-5148.69 ; 5198.38] m (946920 pts)
- Temoins :
  - nature=Indifférenciée usage_1=Résidentiel hauteur=3.3 alt_toit_min= alt_toit_max=139.4 mat_toit=10 mat_murs=40 pts=8 X=2809.89 Y=-4538.97
  - nature=Indifférenciée usage_1=Résidentiel hauteur=4.3 alt_toit_min= alt_toit_max=139.7 mat_toit=19 mat_murs=49 pts=6 X=3056.87 Y=-4232.13
  - (accent) nature=Indifférenciée usage_1=Indifférencié usage_2= hauteur=5.3 etages= X=2168.33 Y=-5001.36
- Note : plafond 120 pts par decimation (le script historique ECARTAIT les anneaux > 80 pts ; ici on ne perd aucun objet, campagne d'acquisition oblige).

## aerodrome.json -- aerodromes (bbox ETENDUE +-9000 m)
- Sources : WFS Geoplateforme BDTOPO_V3:aerodrome et BDTOPO_V3:piste_d_aerodrome (BD TOPO v3), fetch du 2026-07-25, Licence Ouverte 2.0 (IGN).
- Objets : 3 emprises d'aerodrome, 60 surfaces de piste.
- Taille : 0.07 Mo.
- Taux de remplissage aerodromes :
  - categorie : 3 / 3 (100.0 %)
  - nature : 3 / 3 (100.0 %)
  - usage : 3 / 3 (100.0 %)
  - toponyme : 3 / 3 (100.0 %)
  - statut_du_toponyme : 3 / 3 (100.0 %)
  - fictif : 3 / 3 (100.0 %)
  - etat_de_l_objet : 3 / 3 (100.0 %)
  - altitude : 3 / 3 (100.0 %)
  - code_icao : 3 / 3 (100.0 %)
  - code_iata : 1 / 3 (33.3 %)
- Taux de remplissage pistes :
  - nature : 60 / 60 (100.0 %)
  - fonction : 0 / 60 (0.0 %)
  - etat_de_l_objet : 60 / 60 (100.0 %)
- Bornes : X [-8482.67 ; 4857.75] m, Y [-6080.98 ; 7514.13] m (3580 pts)
- ASSUME : bbox +-9000 m, des coordonnees SORTENT de +-5000 m (Blagnac est hors map 10 km, donnees voulues quand meme).
- Aucune couche de zone associee aux aerodromes dans les capabilities (verifie a l etape 0).

## electrique.json -- reseau electrique
- Sources : WFS Geoplateforme BDTOPO_V3:pylone et BDTOPO_V3:ligne_electrique (BD TOPO v3), fetch du 2026-07-25, Licence Ouverte 2.0 (IGN).
- Objets : 104 pylones, 25 polylignes de ligne electrique.
- Taille : 0.02 Mo.
- Taux de remplissage pylones :
  - etat_de_l_objet : 104 / 104 (100.0 %)
  - numero : 94 / 104 (90.4 %)
  - hauteur : 99 / 104 (95.2 %)
- Taux de remplissage lignes :
  - voltage : 25 / 25 (100.0 %)
  - gestionnaire : 19 / 25 (76.0 %)
  - siren_gestionnaire : 19 / 25 (76.0 %)
  - etat_de_l_objet : 25 / 25 (100.0 %)
- Bornes : X [-74540.77 ; 18831.64] m, Y [-21598.77 ; 47565.57] m (626 pts)
- Temoins :
  - pylone hauteur=43.5 nature= X=-2461.58 Y=-4327.73
  - ligne voltage=63 kV nature= pts=2 X=4344.05 Y=-2404.52
  - (pas de valeur accentuee rencontree sur ce theme : attributs surtout numeriques/enumeres)

## hydro_axes.json -- axes des cours d eau
- Source : WFS Geoplateforme BDTOPO_V3:troncon_hydrographique (BD TOPO v3), fetch du 2026-07-25, Licence Ouverte 2.0 (IGN).
- Objets : 280 polylignes (0 features sans polyligne exploitable) ; 178 features avec toponyme de cours d eau.
- Taille : 0.31 Mo.
- Taux de remplissage :
  - code_hydrographique : 280 / 280 (100.0 %)
  - code_du_pays : 280 / 280 (100.0 %)
  - nature : 280 / 280 (100.0 %)
  - fictif : 280 / 280 (100.0 %)
  - etat_de_l_objet : 280 / 280 (100.0 %)
  - position_par_rapport_au_sol : 280 / 280 (100.0 %)
  - mode_d_obtention_des_coordonnees : 280 / 280 (100.0 %)
  - mode_d_obtention_de_l_altitude : 280 / 280 (100.0 %)
  - statut : 280 / 280 (100.0 %)
  - persistance : 280 / 280 (100.0 %)
  - fosse : 280 / 280 (100.0 %)
  - navigabilite : 280 / 280 (100.0 %)
  - salinite : 280 / 280 (100.0 %)
  - numero_d_ordre : 279 / 280 (99.6 %)
  - strategie_de_classement : 0 / 280 (0.0 %)
  - origine : 280 / 280 (100.0 %)
  - perimetre_d_utilisation_ou_origine : 272 / 280 (97.1 %)
  - sens_de_l_ecoulement : 280 / 280 (100.0 %)
  - reseau_principal_coulant : 278 / 280 (99.3 %)
  - trace_connu : 280 / 280 (100.0 %)
  - classe_de_largeur : 280 / 280 (100.0 %)
  - type_de_bras : 280 / 280 (100.0 %)
  - commentaire_sur_l_objet_hydro : 0 / 280 (0.0 %)
  - code_du_cours_d_eau_bdcarthage : 193 / 280 (68.9 %)
  - inventaire_police_de_l_eau : 236 / 280 (84.3 %)
  - identifiant_police_de_l_eau : 247 / 280 (88.2 %)
  - lien_vers_noeud_hydrographique_ini : 280 / 280 (100.0 %)
  - lien_vers_noeud_hydrographique_fin : 280 / 280 (100.0 %)
  - lien_vers_entite_de_transition : 0 / 280 (0.0 %)
  - cpx_toponyme_de_cours_d_eau : 178 / 280 (63.6 %)
  - cpx_toponyme_d_entite_de_transition : 0 / 280 (0.0 %)
- Bornes : X [-6095.39 ; 5974.42] m, Y [-6841.72 ; 5220.47] m (3823 pts)
- Temoins :
  - nom=le Fossé Mère nature=Canal pts=32 X=-4886.4 Y=3655.47
  - nom=Canal de Saint-Martory nature=Conduit buse pts=22 X=-6095.39 Y=3013.46
  - (accent) nom=le Fossé Mère nature=Canal X=-4886.4 Y=3655.47

## admin.json -- communes
- Source : WFS Geoplateforme ADMINEXPRESS-COG.LATEST:commune (ADMIN EXPRESS COG), fetch du 2026-07-25, Licence Ouverte 2.0 (IGN).
- Objets : 8 communes intersectant la bbox (0 sans anneau exploitable). Anneaux amincis 8 m / plafond 400 pts.
- Taille : 0.04 Mo.
- Taux de remplissage :
  - nom_officiel : 8 / 8 (100.0 %)
  - nom_officiel_en_majuscules : 8 / 8 (100.0 %)
  - statut : 8 / 8 (100.0 %)
  - code_insee : 8 / 8 (100.0 %)
  - population : 8 / 8 (100.0 %)
  - organisme_recenseur : 8 / 8 (100.0 %)
  - code_insee_du_canton : 8 / 8 (100.0 %)
  - code_insee_de_l_arrondissement : 8 / 8 (100.0 %)
  - code_insee_du_departement : 8 / 8 (100.0 %)
  - code_insee_de_la_region : 8 / 8 (100.0 %)
  - code_siren : 8 / 8 (100.0 %)
  - codes_siren_des_epci : 8 / 8 (100.0 %)
  - code_postal : 8 / 8 (100.0 %)
  - superficie_cadastrale : 8 / 8 (100.0 %)
- Bornes : X [-7778.51 ; 10105.63] m, Y [-8985.49 ; 7931.86] m (2300 pts)
- ASSUME : les bornes DEBORDENT de +-5000 m (les communes intersectantes sont gardees entieres).
- Communes retenues (INSEE/population relus depuis admin.json — le champ s'appelle `code_insee`,
  pas `insee_com` ; l'affichage témoin initial du script était vide mais la DONNÉE est complète à 100 %) :
  - Balma (INSEE 31044, pop 17772)
  - Blagnac (INSEE 31069, pop 27604)
  - Launaguet (INSEE 31282, pop 9173)
  - Montrabé (INSEE 31389, pop 4473)
  - Quint-Fonsegrives (INSEE 31445, pop 6133)
  - Saint-Jean (INSEE 31488, pop 11261)
  - Toulouse (INSEE 31555, pop 514819)
  - L'Union (INSEE 31561, pop 12638)
  - (accent) Montrabé (INSEE 31389, pop 4473)
- Seulement 8 communes : plausible — la commune de Toulouse (11 800 ha) couvre à elle seule
  la quasi-totalité de la bbox sud et ouest ; Colomiers/Tournefeuille/Ramonville n'atteignent pas ±5000 m.

## echantillons_j5/ -- echantillons de format pour J5 (RPG, BD Foret, haies)
- Sources : WFS Geoplateforme, fetch du 2026-07-25, Licence Ouverte 2.0 (IGN).
- BUT : valider le format des couches pour J5, PAS une exploitation immediate.
- Bbox standard 10x10 urbaine : peu d objets agricoles/forestiers attendus, les franges suffisent (assume).
- rpg.json : 137 objets (72 Ko), source RPG.LATEST:parcelles_graphiques.
  Remplissage :
  - id_parcel : 137 / 137 (100.0 %)
  - surf_parc : 137 / 137 (100.0 %)
  - code_cultu : 137 / 137 (100.0 %)
  - code_group : 137 / 137 (100.0 %)
  - culture_d1 : 0 / 137 (0.0 %)
  - culture_d2 : 0 / 137 (0.0 %)
  - cat_cult_p : 118 / 137 (86.1 %)
  Bornes : X [-4390.01 ; 5748.7] m, Y [-5338.78 ; 4851.03] m (3133 pts)
  Temoin : id_parcel=5532351 surf_parc=0.02 code_cultu=BOR code_group=28
- foret.json : 85 objets (101 Ko), source LANDCOVER.FORESTINVENTORY.V2:formation_vegetale.
  Remplissage :
  - id : 85 / 85 (100.0 %)
  - code_tfv : 85 / 85 (100.0 %)
  - tfv : 85 / 85 (100.0 %)
  - tfv_g11 : 85 / 85 (100.0 %)
  - essence : 85 / 85 (100.0 %)
  Bornes : X [-4395.02 ; 5386.41] m, Y [-5291.24 ; 5047.5] m (4867 pts)
  Temoin : id=FORESTIE0000000003101118 code_tfv=FO1 tfv=Forêt ouverte de feuillus purs tfv_g11=Forêt ouverte feuillus essence=Feuillus
  Temoin accent : tfv=Forêt ouverte de feuillus purs
- haie.json : 479 objets (48 Ko), source BDTOPO_V3:haie.
  Remplissage :
  - hauteur : 0 / 479 (0.0 %)
  - largeur : 0 / 479 (0.0 %)
  Bornes : X [-4401.17 ; 5253.32] m, Y [-5082.41 ; 5033.84] m (2390 pts)

## ferre.json -- reseau ferre
- Sources : WFS Geoplateforme BDTOPO_V3:troncon_de_voie_ferree + BDTOPO_V3:equipement_de_transport (Licence Ouverte 2.0 IGN) ; OSM Overpass stations (ODbL 1.0). Fetch du 2026-07-25.
- Objets : 312 polylignes ferrees ; 77 equipements BD TOPO nature gare/station/halte (sur 1476 equipements de transport de la bbox) ; 181 stations OSM nommees (halt=3, station=44, subway_entrance=94, tram_stop=40).
- Taille : 0.12 Mo.
- Taux de remplissage troncons :
  - nature : 312 / 312 (100.0 %)
  - position_par_rapport_au_sol : 312 / 312 (100.0 %)
  - etat_de_l_objet : 312 / 312 (100.0 %)
  - electrifie : 312 / 312 (100.0 %)
  - largeur : 312 / 312 (100.0 %)
  - nombre_de_voies : 312 / 312 (100.0 %)
  - usage : 7 / 312 (2.2 %)
  - vitesse_maximale : 0 / 312 (0.0 %)
  - cpx_toponyme : 0 / 312 (0.0 %)
- Taux de remplissage equipements BD TOPO :
  - nature : 77 / 77 (100.0 %)
  - nature_detaillee : 0 / 77 (0.0 %)
  - toponyme : 73 / 77 (94.8 %)
  - statut_du_toponyme : 73 / 77 (94.8 %)
  - importance : 77 / 77 (100.0 %)
  - numero : 0 / 77 (0.0 %)
  - fictif : 77 / 77 (100.0 %)
  - adresse_postale : 7 / 77 (9.1 %)
  - etat_de_l_objet : 77 / 77 (100.0 %)
  - insee_commune : 77 / 77 (100.0 %)
  - commune : 77 / 77 (100.0 %)
  - identifiant_voie_ban : 2 / 77 (2.6 %)
  - id_ban_odonyme : 2 / 77 (2.6 %)
- Stations OSM : name 100 % (filtre requete). ATTENTION : les subway_entrance sans tag name (frequent) sont ABSENTES par design.
- Bornes : X [-6529.69 ; 6009.24] m, Y [-6417.69 ; 6029.62] m (2737 pts)
- Temoins :
  - station Rangueil X=1425.31 Y=3291.79
  - station Jean Jaurès X=345.36 Y=-123.87
  - (accent) station Jean Jaurès X=345.36 Y=-123.87

## equipements.json -- terrains de sport, cimetieres, parkings
- Sources : WFS Geoplateforme BDTOPO_V3:terrain_de_sport, BDTOPO_V3:cimetiere, BDTOPO_V3:equipement_de_transport filtre nature~parking (Licence Ouverte 2.0 IGN) ; OSM way[amenity=parking] (ODbL 1.0). Fetch du 2026-07-25.
- Objets : 459 terrains de sport, 13 cimetieres, 248 parkings BD TOPO, 2156 parkings OSM (0 ways ouverts ecartes).
- Taille : 0.6 Mo.
- Taux de remplissage terrains de sport :
  - nature : 459 / 459 (100.0 %)
  - nature_detaillee : 277 / 459 (60.3 %)
  - etat_de_l_objet : 459 / 459 (100.0 %)
- Taux de remplissage cimetieres :
  - nature : 13 / 13 (100.0 %)
  - nature_detaillee : 0 / 13 (0.0 %)
  - toponyme : 3 / 13 (23.1 %)
  - statut_du_toponyme : 3 / 13 (23.1 %)
  - importance : 13 / 13 (100.0 %)
  - etat_de_l_objet : 13 / 13 (100.0 %)
- Taux de remplissage parkings BD TOPO :
  - nature : 248 / 248 (100.0 %)
  - nature_detaillee : 37 / 248 (14.9 %)
  - toponyme : 49 / 248 (19.8 %)
  - statut_du_toponyme : 49 / 248 (19.8 %)
  - importance : 241 / 248 (97.2 %)
  - numero : 0 / 248 (0.0 %)
  - fictif : 248 / 248 (100.0 %)
  - adresse_postale : 0 / 248 (0.0 %)
  - etat_de_l_objet : 248 / 248 (100.0 %)
  - insee_commune : 248 / 248 (100.0 %)
  - commune : 241 / 248 (97.2 %)
  - identifiant_voie_ban : 13 / 248 (5.2 %)
  - id_ban_odonyme : 13 / 248 (5.2 %)
- Parkings OSM : name 124 / 2156 (5.8 %), access 1591 / 2156 (73.8 %).
- Bornes : X [-5325.26 ; 5102.49] m, Y [-5146.44 ; 5116.08] m (24572 pts)
- Temoin (accent) : parking OSM name=P+R Métro Arènes 1 X=-2142.61 Y=1235.84
- RAPPEL : pas de couche parking dediee dans BD TOPO -- les parkings IGN viennent d equipement_de_transport (couverture partielle, surtout grands parkings).

## poi.json -- zones et points d interet
- Sources : WFS Geoplateforme BDTOPO_V3:zone_d_activite_ou_d_interet (Licence Ouverte 2.0 IGN) ; OSM nodes+ways nommes amenity|tourism|historic, centroides via out center (ODbL 1.0). Fetch du 2026-07-25.
- Objets : 1487 ZAI (0 sans geometrie) ; 5546 POI OSM (amenity=4887, historic=78, tourism=581).
- Taille : 1.2 Mo.
- Taux de remplissage ZAI :
  - categorie : 1487 / 1487 (100.0 %)
  - nature : 1487 / 1487 (100.0 %)
  - nature_detaillee : 1005 / 1487 (67.6 %)
  - toponyme : 1304 / 1487 (87.7 %)
  - statut_du_toponyme : 1304 / 1487 (87.7 %)
  - importance : 1485 / 1487 (99.9 %)
  - fictif : 1487 / 1487 (100.0 %)
  - etat_de_l_objet : 1487 / 1487 (100.0 %)
  - insee_commune : 1487 / 1487 (100.0 %)
  - commune : 1473 / 1487 (99.1 %)
  - adresse_postale : 1119 / 1487 (75.3 %)
  - identifiant_voie_ban : 194 / 1487 (13.0 %)
  - nom_commercial : 2 / 1487 (0.1 %)
  - id_ban_odonyme : 194 / 1487 (13.0 %)
- POI OSM : name 100 % (filtre requete) ; cle unique par priorite amenity > tourism > historic.
- Bornes : X [-5436.55 ; 5226.75] m, Y [-6891.04 ; 6278.92] m (22873 pts)
- Temoins :
  - ZAI Sport / Piscine : Complexe Sportif Piscine
  - historic=memorial : Monument aux morts X=-1310.4 Y=2152.39
  - (accent) ZAI Sport / Autre équipement sportif : Centre Ligue Midi-Pyrénées de Tennis
  - (accent) amenity=bank : Crédit Agricole X=-777.43 Y=651.61

## Addendum superviseur — anomalies transverses et sanity check final (2026-07-25, fin de campagne)

### Sanity check final (vérifié après coup, hors scripts)
- Les 13 JSON (12 thèmes + 3 échantillons) sont **valides** (reparse complet) et **UTF-8 sans BOM**.
- **Zéro mojibake** sur l'ensemble des sorties (scan « Ã© » : 0 occurrence sur 63 Mo).
- bati_enrichi.json : 131 357 objets `pts` comptés dans le fichier = le compte du rapport ;
  120 907 `altitude_maximale_toit` présents = les 92,0 % annoncés. Cohérence interne OK.

### Anomalies et pièges pour les builders (LES 3 DÉCOUVERTES QUI COMPTENT en tête)
1. **materiaux_des_murs / materiaux_de_la_toiture sont des CODES numériques** (ex. mat_toit=10,
   mat_murs=40), pas des libellés — prévoir la table de nomenclature BD TOPO v3 côté builder J3b.
   Et ils ne sont remplis qu'à ~48 % : le builder toits DOIT avoir un fallback sans matériau.
2. **Le trio altitudes toit est bon mais pas total** : altitude_maximale_toit 92,0 %,
   altitude_minimale_toit 95,3 %, hauteur 99,1 %. Le chantier J3b (égout/faîtage) a sa matière
   première sur ~9 bâtiments sur 10 ; fallback hauteur seule pour le reste.
   Bonus inattendu : nombre_d_etages ET nombre_de_logements à 54,5 % (utile LOD/gameplay).
3. **Géométries débordantes par design WFS** : le serveur renvoie les objets qui INTERSECTENT
   la bbox, gardés entiers. Cas extrême : electrique.json borne X à −74,5 km (une ligne THT
   entière). Idem, plus modéré : hydro_axes (−6,8 km), tunnels (−8,8 km, tunnel ferroviaire),
   admin (communes entières), aerodrome (±9 km assumé), rues/poi (ways de bord). Les builders
   doivent CLIPPER à la map — l'acquisition n'a volontairement rien tronqué.

### Autres trous de données à connaître (ne pas les découvrir dans trois semaines)
- ferre.json : vitesse_maximale 0 %, cpx_toponyme 0 % sur les tronçons ; nature_detaillee 0 %
  sur les équipements. Les noms de lignes ferrées viendront d'OSM (cf. tunnels.json témoin).
- haie.json (échantillon J5) : géométries OK (479) mais hauteur/largeur 0 % — la couche haie
  BD TOPO est purement géométrique ici, prévoir hauteur procédurale.
- rpg.json (échantillon J5) : culture_d1/d2 0 % (normal : cultures dérobées rares), code_cultu
  et code_group 100 % — le format J5 est validé.
- eclairage.json : 3 625 lampadaires seulement, couverture OSM crowdsourcée hétérogène —
  jeu de données INDICATIF, pas exhaustif (déjà noté dans sa section).
- equipements.json : parkings OSM name 5,8 % ; parkings BD TOPO = grands parkings surtout
  (248 contre 2 156 OSM). Les deux sources se complètent, dédup à prévoir côté builder.
- poi.json : nom_commercial ZAI 0,1 % — ne pas compter dessus ; toponyme 87,7 % est la clé utile.
- aerodrome.json : 3 emprises (Blagnac code ICAO LFBO + Lasbordes LFCL + un héliport),
  60 surfaces de piste ; champ `fonction` des pistes 0 %.
- admin.json : témoins INSEE corrigés dans la section (champ `code_insee`, donnée 100 %).

### Écarts au plan de mission (assumés, documentés)
- Plafond bâtiments : 120 pts par DÉCIMATION (21 anneaux touchés) au lieu de l'écartement
  du script historique — aucun objet perdu, esprit « l'acquisition ne décide rien ».
- `capabilities_raw.xml` (5 Mo, brut du GetCapabilities) supprimé en fin de campagne ;
  l'inventaire utile est `wfs_couches.txt` (794 couches).
## routes_bdtopo.json -- troncons de route BD TOPO (verrou 2 : largeurs MESUREES)
- Source : WFS Geoplateforme BDTOPO_V3:troncon_de_route, fetch du 2026-07-27, Licence Ouverte 2.0 (IGN).
- Objets : 35861 features -> 35861 polylignes (0 sans polyligne exploitable).
- Taille : 15.2 Mo. Duree : 2.6 min.
- VERROU largeur_de_chaussee : 30261 / 35861 features renseignes (84.4 %), moyenne 4.47 m, 27 valeurs a zero.
- Taux de remplissage brut par attribut :
  - nature : 35861 / 35861 (100.0 %)
  - importance : 35861 / 35861 (100.0 %)
  - largeur_de_chaussee : 30288 / 35861 (84.5 %)
  - nombre_de_voies : 30271 / 35861 (84.4 %)
  - sens_de_circulation : 35861 / 35861 (100.0 %)
  - position_par_rapport_au_sol : 35861 / 35861 (100.0 %)
  - fictif : 35861 / 35861 (100.0 %)
  - prive : 35686 / 35861 (99.5 %)
  - urbain : 35861 / 35861 (100.0 %)
  - etat_de_l_objet : 35861 / 35861 (100.0 %)
  - acces_vehicule_leger : 35861 / 35861 (100.0 %)
  - acces_pieton : 1942 / 35861 (5.4 %)
  - reserve_aux_bus : 438 / 35861 (1.2 %)
  - vitesse_moyenne_vl : 35861 / 35861 (100.0 %)
  - cpx_classement_administratif : 2560 / 35861 (7.1 %)
  - cpx_numero : 2560 / 35861 (7.1 %)
  - nom_voie_ban_gauche : 22146 / 35861 (61.8 %)
  - nom_collaboratif_gauche : 22773 / 35861 (63.5 %)
- Croisement par importance (N features / largeur % / nb_voies % / largeur moyenne m) :
  - importance 1 : 379 / 98.2 % / 98.2 % / 10.09 m
  - importance 2 : 428 / 97.2 % / 97.2 % / 7.02 m
  - importance 3 : 3283 / 99.9 % / 99.9 % / 6.12 m
  - importance 4 : 3409 / 99.9 % / 99.9 % / 4.99 m
  - importance 5 : 23282 / 97.4 % / 97.4 % / 4.01 m
  - importance 6 : 5080 / 2.0 % / 1.7 % / 3.74 m
- Croisement par nature :
  - Bretelle : 416 / 96.2 % / 96.2 % / 5.16 m
  - Chemin : 187 / 0.0 % / 0.0 % / 0.00 m
  - Escalier : 98 / 0.0 % / 0.0 % / 0.00 m
  - Rond-point : 2032 / 100.0 % / 100.0 % / 4.89 m
  - Route à 1 chaussée : 27092 / 95.5 % / 95.4 % / 4.32 m
  - Route à 2 chaussées : 1656 / 100.0 % / 100.0 % / 4.83 m
  - Route empierrée : 432 / 0.0 % / 0.0 % / 0.00 m
  - Sentier : 3635 / 0.0 % / 0.0 % / 0.00 m
  - Type autoroutier : 313 / 98.1 % / 98.1 % / 11.18 m
- Bornes : X [-5690.85 ; 5352.59] m, Y [-5782.88 ; 5458.26] m (167804 pts)

## parcelles.json -- parcelles cadastrales (verrou 1 des SOLS PAR LE CADASTRE)
- Source : WFS Geoplateforme CADASTRALPARCELS.PARCELLAIRE_EXPRESS:parcelle (PCI Express), fetch du 2026-07-27, Licence Ouverte (IGN/DGFiP).
- Objets : 87244 features -> 87092 polygones emis (0 features multipolygones, 152 sans polygone exploitable), 721 anneaux interieurs conserves.
- Taille : 25.36 Mo. Duree : 4.5 min.
- Sections distinctes : 52.
- Bornes : X [-6864.95 ; 5355.24] m, Y [-5546.32 ; 5508.08] m (1042489 pts)

## cours.json -- cours interieures des batiments
- Source : WFS Geoplateforme BDTOPO_V3:batiment (BD TOPO v3), fetch du 2026-07-27, Licence Ouverte 2.0 (IGN).
- Batiments a cour : 580 (sur 131575 lus), 748 cours retenues (>= 15 m2), 182 ecartees.
- Aire de cours cumulee : 145899 m2. Taille : 283.1 Ko.
