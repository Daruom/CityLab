#include "PlanSol.h"

#include "Algo/Reverse.h"
#include "CompGeom/Delaunay2.h"
#include "Containers/StaticArray.h"
#include "VectorTypes.h"

namespace
{
	using UE::Geometry::FDelaunay2;
	using UE::Geometry::FIndex2i;
	using UE::Geometry::FIndex3i;
	using FVec2d = UE::Math::TVector2<double>;

	/** Carre de la distance 2D, en m2 — ecrit a la main : aucune dependance
	 *  a un nom de methode d'un type geometrique qui change de version. */
	double D2(const FVec2d& A, const FVec2d& B)
	{
		const double dx = A.X - B.X, dy = A.Y - B.Y;
		return dx * dx + dy * dy;
	}

	/** Aire signee d'un anneau (m2). > 0 = sens trigonometrique (CCW). */
	double AireSignee(const TArray<FVector2D>& P)
	{
		double A = 0.0;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FVector2D& U = P[i];
			const FVector2D& V = P[(i + 1) % P.Num()];
			A += U.X * V.Y - V.X * U.Y;
		}
		return A * 0.5;
	}

	/**
	 * LA LOI DE Z, evaluee en un point — et UNIQUEMENT celle de la parcelle.
	 * Rend le Z MOTEUR en cm (NGF x 100 - altitude du Capitole).
	 */
	struct FLoiZ
	{
		const FPlanLoiZ* Loi = nullptr;
		float AltCapCm = 0.f;
		TFunctionRef<float(double, double)>* Drapage = nullptr;

		/** Abscisses cumulees de l'axe, precalculees une fois par parcelle. */
		TArray<double> S;

		void Prepare()
		{
			S.Reset();
			if (!Loi || Loi->Forme != EPlanForme::ProfilTroncon || Loi->Axe.Num() < 2)
			{
				return;
			}
			S.Reserve(Loi->Axe.Num());
			double Cumul = 0.0;
			S.Add(0.0);
			for (int32 i = 1; i < Loi->Axe.Num(); ++i)
			{
				Cumul += (Loi->Axe[i] - Loi->Axe[i - 1]).Size();
				S.Add(Cumul);
			}
		}

		/** (Xm, Ym) -> Z moteur en cm. */
		float At(double Xm, double Ym) const
		{
			switch (Loi->Forme)
			{
			case EPlanForme::Constante:
				return (float)(Loi->ZM * 100.0) - AltCapCm;

			case EPlanForme::Drapage:
				// Le MNT tel que le pipeline actuel le lit : MEME fonction, donc
				// zero divergence possible sur ces parcelles.
				return (*Drapage)(Xm * 100.0, Ym * 100.0);

			case EPlanForme::ProfilTroncon:
			{
				// Projection du point sur la POLYLIGNE d'axe -> abscisse curviligne,
				// puis lecture du profil en long PUBLIE (deja regularise par le
				// compilateur : le moteur ne re-lisse rien).
				const TArray<FVector2D>& Axe = Loi->Axe;
				const FVector2D P(Xm, Ym);
				double MeilleureD2 = TNumericLimits<double>::Max();
				double MeilleureS = 0.0;
				for (int32 i = 0; i + 1 < Axe.Num(); ++i)
				{
					const FVector2D A = Axe[i];
					const FVector2D B = Axe[i + 1];
					const FVector2D AB = B - A;
					const double L2 = AB.SizeSquared();
					double T = 0.0;
					if (L2 > 1e-12)
					{
						T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / L2, 0.0, 1.0);
					}
					const FVector2D Q = A + AB * T;
					const double Dist2 = (P - Q).SizeSquared();
					if (Dist2 < MeilleureD2)
					{
						MeilleureD2 = Dist2;
						MeilleureS = S[i] + T * FMath::Sqrt(L2);
					}
				}
				return (float)(ZDuProfil(MeilleureS) * 100.0) - AltCapCm;
			}

			default:
				// Loi inconnue : le lot ne devine pas. L'appelant a deja refuse la
				// parcelle en amont ; ce chemin ne sert que de garde.
				return (*Drapage)(Xm * 100.0, Ym * 100.0);
			}
		}

		/** Le profil en long : [(abscisse_m, cote_NGF_m)], interpole LINEAIREMENT
		 *  entre deux echantillons — c'est la definition meme d'un profil en long
		 *  par troncon (pente constante par segment), pas un lissage. */
		double ZDuProfil(double SM) const
		{
			const TArray<FVector2D>& Pr = Loi->Profil;
			if (Pr.Num() == 0) { return 0.0; }
			if (SM <= Pr[0].X) { return Pr[0].Y; }
			if (SM >= Pr.Last().X) { return Pr.Last().Y; }
			for (int32 i = 0; i + 1 < Pr.Num(); ++i)
			{
				if (SM <= Pr[i + 1].X)
				{
					const double D = Pr[i + 1].X - Pr[i].X;
					const double T = (D > 1e-9) ? (SM - Pr[i].X) / D : 0.0;
					return FMath::Lerp(Pr[i].Y, Pr[i + 1].Y, T);
				}
			}
			return Pr.Last().Y;
		}
	};
}

bool ConstruirePlanSol(
	const FPlanCellule& Cellule,
	float AltCapCm,
	TFunctionRef<float(double, double)> ZDrapageCm,
	double PasM,
	const FPlanSolPerimetre& Perimetre,
	TFunctionRef<int32(const FPlanParcelle&)> ClasseDe,
	TArray<FPlanSolLot>& OutLots,
	FPlanSolStats& Stats)
{
	OutLots.Reset();
	// Un lot par classe de surface, cree a la premiere rencontre : jamais de lot vide.
	TMap<int32, int32> LotDe;
	auto LotPour = [&](int32 Classe) -> FPlanSolLot&
	{
		if (const int32* I = LotDe.Find(Classe)) { return OutLots[*I]; }
		FPlanSolLot L;
		L.Classe = Classe;
		const int32 I = OutLots.Add(MoveTemp(L));
		LotDe.Add(Classe, I);
		return OutLots[I];
	};

	// Parcelles sauvees par le repli de remplissage `Solid` : journalisees en fin
	// de passe (aucun champ de structure, pour rester patchable a chaud).
	int32 NbRemplissageSolide = 0;
	const double PasCm = FMath::Max(PasM, 0.25) * 100.0;
	const double PasCm2 = PasCm * PasCm;

	for (const FPlanParcelle& P : Cellule.Parcelles)
	{
		if (!Perimetre.Accepte(P.Proprietaire))
		{
			++Stats.HorsPerimetre;
			continue;
		}
		if (P.Anneaux.Num() == 0 || P.Anneaux[0].Num() < 3)
		{
			++Stats.Refusees;
			Stats.IdsRefuses.Add(P.Id + TEXT(" (anneau exterieur vide)"));
			continue;
		}
		if (P.Loi.Forme == EPlanForme::Inconnue)
		{
			// Le plan n'a pas dit A QUELLE COTE : on ne construit PAS. C'est un
			// trou de contrat, pas une occasion de deviner.
			++Stats.Refusees;
			Stats.IdsRefuses.Add(P.Id + TEXT(" (loi de Z absente ou hors des trois formes)"));
			continue;
		}

		// --- 1. LES ANNEAUX -> sommets + aretes contraintes -------------------
		// Sens impose : exterieur CCW, trous CW. Le remplissage se fait ensuite
		// par NOMBRE DE TOURS POSITIF — les trous s'annulent d'eux-memes, sans
		// aucun test d'appartenance a inventer.
		TArray<FVec2d> Sommets;
		TArray<FIndex2i> Aretes;
		bool bAnneauInvalide = false;
		for (int32 R = 0; R < P.Anneaux.Num(); ++R)
		{
			TArray<FVector2D> Anneau = P.Anneaux[R];
			// Le plan ferme ses anneaux (dernier point = premier) : on retire la
			// repetition, la fermeture est portee par l'arete.
			while (Anneau.Num() >= 2 && Anneau.Last().Equals(Anneau[0], 1e-9))
			{
				Anneau.Pop(EAllowShrinking::No);
			}
			// ⛔ SEMANTIQUE DU CONTRAT — PAS UNE TOLERANCE DE CONFORT.
			// LES POINTS QUE LE PLAN NE SAIT PAS DISTINGUER.
			// Le contrat declare ses geometries « en metres, arrondies au
			// MILLIMETRE » : deux points a moins d'un millimetre l'un de l'autre
			// sont, pour le plan lui-meme, le MEME point. Les garder fabrique des
			// aretes de 1 mm qui font echouer la triangulation contrainte — mesure :
			// 138 parcelles refusees sur le district, toutes a anneau simple, toutes
			// porteuses d'un doublon de ce genre (ex. voi/4965#0 : deux sommets a
			// 1,4 mm ; voi/7569#0 : deux sommets IDENTIQUES).
			// Ce n'est pas une simplification de forme : c'est lire le plan a la
			// precision ou il est ecrit.
			{
				const double TolM = 0.001;              // le millimetre du contrat
				const double Tol2 = TolM * TolM;
				TArray<FVector2D> Net;
				Net.Reserve(Anneau.Num());
				for (const FVector2D& Q : Anneau)
				{
					if (Net.Num() > 0 && FVector2D::DistSquared(Net.Last(), Q) < Tol2)
					{
						++Stats.PointsFondus;
						continue;
					}
					Net.Add(Q);
				}
				while (Net.Num() >= 2 && FVector2D::DistSquared(Net.Last(), Net[0]) < Tol2)
				{
					Net.Pop(EAllowShrinking::No);
					++Stats.PointsFondus;
				}
				Anneau = MoveTemp(Net);
			}
			// ⛔ MEME SEMANTIQUE DE CONTRAT — LES POINTS QUI NE DISENT RIEN.
			// Un sommet situe a moins d un millimetre de la corde de ses deux
			// voisins est, a la precision ou le plan est ecrit, SUR cette corde :
			// il ne porte aucune forme. Garde, il fabrique un triangle degenere
			// et fait echouer la triangulation contrainte (mesure : 37 des 44
			// refus restants, tous a anneau simple ; ex. voi/4965#0, dont trois
			// sommets consecutifs sont alignes a 1 mm pres). Le retirer ne change
			// pas le contour d un millimetre.
			if (Anneau.Num() >= 3)
			{
				const double TolM = 0.001;
				bool bEncore = true;
				while (bEncore && Anneau.Num() > 3)
				{
					bEncore = false;
					for (int32 i = 0; i < Anneau.Num(); ++i)
					{
						const FVector2D& A0 = Anneau[(i + Anneau.Num() - 1) % Anneau.Num()];
						const FVector2D& B0 = Anneau[i];
						const FVector2D& C0 = Anneau[(i + 1) % Anneau.Num()];
						const FVector2D AC = C0 - A0;
						const double L = AC.Size();
						if (L < TolM) { continue; }
						const double Dist = FMath::Abs(
							(AC.X * (A0.Y - B0.Y) - (A0.X - B0.X) * AC.Y) / L);
						if (Dist < TolM)
						{
							Anneau.RemoveAt(i, EAllowShrinking::No);
							++Stats.PointsFondus;
							bEncore = true;
							break;
						}
					}
				}
			}
			if (Anneau.Num() < 3)
			{
				if (R == 0) { bAnneauInvalide = true; }
				continue;   // un trou degenere n'est pas un trou
			}
			const double A = AireSignee(Anneau);
			const bool bVeutCCW = (R == 0);
			if ((A < 0.0) == bVeutCCW)
			{
				Algo::Reverse(Anneau);
			}
			if (R == 0)
			{
				Stats.AireM2 += FMath::Abs(A);
			}
			const int32 Base = Sommets.Num();
			for (const FVector2D& Q : Anneau)
			{
				Sommets.Add(FVec2d(Q.X, Q.Y));
			}
			for (int32 i = 0; i < Anneau.Num(); ++i)
			{
				Aretes.Add(FIndex2i(Base + i, Base + (i + 1) % Anneau.Num()));
			}
		}
		if (bAnneauInvalide || Sommets.Num() < 3)
		{
			++Stats.Refusees;
			Stats.IdsRefuses.Add(P.Id + TEXT(" (anneaux degeneres)"));
			continue;
		}
		if (P.Anneaux.Num() > 1) { ++Stats.AvecTrous; }

		// --- 2. TRIANGULATION CONTRAINTE (trous compris) ----------------------
		FDelaunay2 Delaunay;
		// Les anneaux d'une meme parcelle peuvent partager un sommet a l'identique
		// (contact exterieur/trou) : on demande a la triangulation de recoller ces
		// doublons plutot que de refuser la parcelle.
		Delaunay.bAutomaticallyFixEdgesToDuplicateVertices = true;
		TArray<FIndex3i> Tris;
		bool bOk = Delaunay.Triangulate(Sommets, Aretes);
		if (bOk)
		{
			bOk = Delaunay.GetFilledTriangles(Tris, Aretes, FDelaunay2::EFillMode::PositiveWinding);
		}
		if (!bOk || Tris.Num() == 0)
		{
			// REPLI DE REMPLISSAGE — pas une approximation : le MEME jeu d aretes
			// contraintes, rempli par la regle `Solid` (est retenu tout triangle
			// qu on ne peut atteindre depuis l exterieur sans traverser une arete
			// contrainte) au lieu du nombre de tours. Elle ignore l ORIENTATION
			// des anneaux, et c est exactement ce qui manque aux polygones de
			// carrefour du v2, dont les anneaux se touchent. Le contour reste
			// celui du plan, sommet pour sommet.
			TArray<FIndex3i> Repli;
			if (Delaunay.GetFilledTriangles(Repli, Aretes, FDelaunay2::EFillMode::Solid)
				&& Repli.Num() > 0)
			{
				Tris = MoveTemp(Repli);
				bOk = true;
				++NbRemplissageSolide;
			}
		}
		if (!bOk || Tris.Num() == 0)
		{
			// Aucun repli, aucune approximation : la parcelle est REFUSEE et
			// nommee. Un sol devine serait pire qu'un sol manquant.
			++Stats.Refusees;
			Stats.IdsRefuses.Add(P.Id + FString::Printf(TEXT(" (triangulation KO, %d sommets, %d anneaux)"),
				Sommets.Num(), P.Anneaux.Num()));
			continue;
		}

		// --- 3. LA LOI DE Z ---------------------------------------------------
		FLoiZ Loi{ &P.Loi, AltCapCm, &ZDrapageCm };
		Loi.Prepare();
		if (P.Loi.Forme == EPlanForme::ProfilTroncon
			&& (P.Loi.Axe.Num() < 2 || P.Loi.Profil.Num() < 2))
		{
			++Stats.Refusees;
			Stats.IdsRefuses.Add(P.Id + TEXT(" (profil_troncon sans axe ni profil exploitable)"));
			continue;
		}

		// NB : surtout pas `PI` — c'est une MACRO du moteur (3,14159), et l'indexer
		// donne un "indice non integral" incomprehensible. Paye ici meme.
		const int32 IdxProprio = (int32)P.Proprietaire;
		auto Sommet = [&](const FVec2d& Q) -> FVector3f
		{
			const float Z = Loi.At(Q.X, Q.Y);
			// LA MESURE DE COPLANARITE, prise ou le sommet est pose.
			if (IdxProprio >= 0 && IdxProprio < 6)
			{
				const double E = FMath::Abs((double)Z - (double)ZDrapageCm(Q.X * 100.0, Q.Y * 100.0));
				++Stats.EcartN[IdxProprio];
				Stats.EcartSommeCm[IdxProprio] += E;
				Stats.EcartMaxCm[IdxProprio] = FMath::Max(Stats.EcartMaxCm[IdxProprio], E);
				if (E < 2.0) { ++Stats.EcartSous2cm[IdxProprio]; }
			}
			return FVector3f((float)(Q.X * 100.0), (float)(Q.Y * 100.0), Z);
		};

		// --- 4. SUBDIVISION — seulement la ou la loi VARIE --------------------
		// `constante` est plate : la subdiviser n'ajouterait que des triangles.
		// `drapage` et `profil_troncon` varient dans la parcelle : on echantillonne
		// LEUR loi jusqu'a l'arete visee (la maille de sol existante). Ce n'est pas
		// une nappe (13.1) : rien n'est raccorde a rien, on lit la meme loi plus
		// finement.
		const bool bSubdivise = (P.Loi.Forme != EPlanForme::Constante);
		FPlanSolLot& Lot = LotPour(ClasseDe(P));
		const int32 TrisAvant = Lot.Tris.Num();

		// Pile de triangles EN 2D (m) : on ne calcule le Z qu'a l'emission, pour
		// que la subdivision ne transporte jamais un Z interpole.
		TArray<TStaticArray<FVec2d, 3>> Pile;
		Pile.Reserve(Tris.Num());
		int32 TrisHorsBornes = 0;
		for (const FIndex3i& T : Tris)
		{
			if (!Sommets.IsValidIndex(T.A) || !Sommets.IsValidIndex(T.B) || !Sommets.IsValidIndex(T.C))
			{
				++TrisHorsBornes;   // la triangulation a introduit un sommet : on ne devine pas
				continue;
			}
			TStaticArray<FVec2d, 3> Tri;
			Tri[0] = Sommets[T.A];
			Tri[1] = Sommets[T.B];
			Tri[2] = Sommets[T.C];
			Pile.Add(Tri);
		}
		if (TrisHorsBornes > 0)
		{
			++Stats.Refusees;
			Stats.IdsRefuses.Add(P.Id + FString::Printf(
				TEXT(" (%d triangles a sommet inconnu, ecartes)"), TrisHorsBornes));
		}

		int32 Garde = 0;
		const int32 GardeMax = 400000;   // borne dure : jamais de subdivision folle
		bool bGardeAtteinte = false;
		while (Pile.Num() > 0)
		{
			TStaticArray<FVec2d, 3> T = Pile.Pop(EAllowShrinking::No);
			++Garde;
			if (Garde >= GardeMax && !bGardeAtteinte)
			{
				// ⛔ Jamais de sortie MUETTE : on cesse de subdiviser, on EMET tout
				// ce qui reste (le vide serait pire, 13.2) et on le DIT.
				bGardeAtteinte = true;
				++Stats.Refusees;
				Stats.IdsRefuses.Add(P.Id + TEXT(" (subdivision bornee : sol pose plus grossier)"));
			}
			if (bSubdivise && !bGardeAtteinte)
			{
				const double L0 = D2(T[1], T[0]) * 10000.0;
				const double L1 = D2(T[2], T[1]) * 10000.0;
				const double L2 = D2(T[0], T[2]) * 10000.0;
				if (FMath::Max3(L0, L1, L2) > PasCm2)
				{
					// Subdivision 1 -> 4 par les MILIEUX D'ARETE : les milieux sont
					// SUR les aretes, donc le contour de la parcelle ne bouge pas
					// d'un micron, et aucun T-joint n'apparait a l'interieur.
					const FVec2d M01 = (T[0] + T[1]) * 0.5;
					const FVec2d M12 = (T[1] + T[2]) * 0.5;
					const FVec2d M20 = (T[2] + T[0]) * 0.5;
					auto Empile = [&Pile](const FVec2d& A, const FVec2d& B, const FVec2d& C)
					{
						TStaticArray<FVec2d, 3> N;
						N[0] = A; N[1] = B; N[2] = C;
						Pile.Add(N);
					};
					Empile(T[0], M01, M20);
					Empile(M01, T[1], M12);
					Empile(M20, M12, T[2]);
					Empile(M01, M12, M20);
					continue;
				}
			}
			FPlanSolTri Out;
			Out.A = Sommet(T[0]);
			Out.B = Sommet(T[1]);
			Out.C = Sommet(T[2]);
			Lot.Tris.Add(Out);
		}

		Stats.Triangles += Lot.Tris.Num() - TrisAvant;
		++Stats.Parcelles;
		if (IdxProprio >= 0 && IdxProprio < 6) { ++Stats.ParProprio[IdxProprio]; }
		switch (P.Loi.Forme)
		{
		case EPlanForme::Constante:      ++Stats.Constante; break;
		case EPlanForme::ProfilTroncon:  ++Stats.ProfilTroncon; break;
		case EPlanForme::Drapage:        ++Stats.Drapage; break;
		default: break;
		}
	}

	if (NbRemplissageSolide > 0)
	{
		UE_LOG(LogPlanVille, Display,
			TEXT("PLAN SOL : %d parcelle(s) rendue(s) par le repli de remplissage Solid ")
			TEXT("(le remplissage par nombre de tours avait echoue sur leurs anneaux)."),
			NbRemplissageSolide);
	}
	return Stats.Refusees == 0;
}
