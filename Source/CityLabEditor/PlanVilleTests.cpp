#include "PlanVille.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * =============================================================================
 * LES TESTS DU LECTEUR-VALIDATEUR DU PLAN (lot E2-0)
 * =============================================================================
 *
 * Spec DISTINCTE (`CityLab.PlanVille`) : elle ne partage rien avec
 * `CityLab.CityImportTools`, qui REGENERE le monde courant (Playbook S5). Ces
 * tests-ci ne touchent ni au monde, ni a un asset, ni au disque du projet
 * ailleurs que dans `Saved/Tests/PlanVille` — et JAMAIS a `SourceData`, qui est
 * en lecture seule pour ce lot.
 *
 * Ce qu'ils prouvent :
 *   1. le plan REEL passe, et les comptes reconstitues EGALENT le manifeste ;
 *   2. le district-first : une cellule n'ouvre que ses trois side-cars ;
 *   3. SABOTAGE : un seul octet change dans une copie -> REFUS (et l'original
 *      reste intact) ;
 *   4. MANQUE : un fichier declare par l'index mais absent -> REFUS.
 */

BEGIN_DEFINE_SPEC(
	FPlanVilleSpec,
	"CityLab.PlanVille",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

	/** Le bac a sable des tests. JAMAIS sous SourceData (verifie a l'usage). */
	FString BacASable() const
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Tests/PlanVille")));
	}

	void VideBacASable() const
	{
		IFileManager::Get().DeleteDirectory(*BacASable(), false, true);
	}

	/** Copie plan.json + l'index + les side-cars demandes vers le bac a sable. */
	bool Miroir(const FIntPoint& Cell, const TArray<FString>& Familles, FString& OutDir);

	/** Change UN chiffre du fichier (le JSON reste valide : seule l'empreinte
	 *  peut motiver le refus). */
	bool SaboteUnOctet(const FString& Chemin, FString& OutAvant, FString& OutApres);

	/** Les cellules extremes du domaine — choisies DANS LES DONNEES, jamais
	 *  ecrites dans le code. La plus legere sert aux tests de garde (copies
	 *  rapides), la plus lourde donne le chrono qui compte pour E2-1. */
	static FIntPoint CelluleExtreme(const FPlanIndex& Idx, bool bLaPlusLourde);
	static FIntPoint CelluleLaPlusLegere(const FPlanIndex& Idx)
	{
		return CelluleExtreme(Idx, false);
	}

	int32 CompteDefauts(const FPlanRapport& R, const TCHAR* Motif) const;

END_DEFINE_SPEC(FPlanVilleSpec)

FIntPoint FPlanVilleSpec::CelluleExtreme(const FPlanIndex& Idx, bool bLaPlusLourde)
{
	FIntPoint Best = Idx.Cellules.Num() > 0 ? Idx.Cellules[0] : FIntPoint::ZeroValue;
	int64 BestPoids = bLaPlusLourde ? -1 : MAX_int64;
	for (const auto& KV : Idx.ParCellule)
	{
		const int64 Poids = (int64)KV.Value.Parcelles + KV.Value.Interfaces + KV.Value.Instances;
		const bool bMieux = bLaPlusLourde ? (Poids > BestPoids) : (Poids < BestPoids);
		// Departage stable (l'ordre d'un TMap ne l'est pas) : le plus petit
		// couple (x, y) l'emporte, pour que le test soit reproductible.
		const bool bEgalEtAvant = (Poids == BestPoids)
			&& (KV.Key.X < Best.X || (KV.Key.X == Best.X && KV.Key.Y < Best.Y));
		if (bMieux || bEgalEtAvant)
		{
			BestPoids = Poids;
			Best = KV.Key;
		}
	}
	return Best;
}

int32 FPlanVilleSpec::CompteDefauts(const FPlanRapport& R, const TCHAR* Motif) const
{
	int32 N = 0;
	for (const FPlanDefaut& D : R.Defauts)
	{
		if (D.Motif.Contains(Motif)) { ++N; }
	}
	return N;
}

bool FPlanVilleSpec::Miroir(const FIntPoint& Cell, const TArray<FString>& Familles, FString& OutDir)
{
	const FString Src = FPaths::ConvertRelativePathToFull(FPlanVille::DossierParDefaut());
	OutDir = BacASable();

	// GARDE-FOU : on n'ecrit jamais dans SourceData, meme par accident.
	if (OutDir.Contains(TEXT("SourceData")))
	{
		AddError(TEXT("Le bac a sable des tests tombe sous SourceData — test avorte."));
		return false;
	}

	IFileManager& FM = IFileManager::Get();
	FM.DeleteDirectory(*OutDir, false, true);
	if (!FM.MakeDirectory(*FPaths::Combine(OutDir, TEXT("data")), true))
	{
		AddError(FString::Printf(TEXT("Bac a sable non creable : %s"), *OutDir));
		return false;
	}

	auto Copie = [&](const FString& Rel) -> bool
	{
		const uint32 R = FM.Copy(*FPaths::Combine(OutDir, Rel), *FPaths::Combine(Src, Rel));
		if (R != COPY_OK)
		{
			AddError(FString::Printf(TEXT("Copie impossible : %s"), *Rel));
			return false;
		}
		return true;
	};

	if (!Copie(TEXT("plan.json"))) { return false; }
	if (!Copie(TEXT("data/plan_index.json"))) { return false; }
	for (const FString& F : Familles)
	{
		if (!Copie(FString::Printf(TEXT("data/%s"), *FPlanVille::NomSideCar(*F, Cell)))) { return false; }
	}
	return true;
}

bool FPlanVilleSpec::SaboteUnOctet(const FString& Chemin, FString& OutAvant, FString& OutApres)
{
	TArray<uint8> Octets;
	if (!FFileHelper::LoadFileToArray(Octets, *Chemin) || Octets.Num() < 32) { return false; }

	// On cherche un CHIFFRE au dela de la moitie du fichier et on le change :
	// le JSON reste parfaitement valide, donc le seul motif de refus possible
	// est l'EMPREINTE. C'est ce qu'on veut prouver.
	for (int32 i = Octets.Num() / 2; i < Octets.Num(); ++i)
	{
		if (Octets[i] >= '0' && Octets[i] <= '9')
		{
			OutAvant = FString::Printf(TEXT("octet %d = '%c'"), i, (TCHAR)Octets[i]);
			Octets[i] = (Octets[i] == '9') ? '8' : (uint8)(Octets[i] + 1);
			OutApres = FString::Printf(TEXT("octet %d = '%c'"), i, (TCHAR)Octets[i]);
			return FFileHelper::SaveArrayToFile(Octets, *Chemin);
		}
	}
	return false;
}

void FPlanVilleSpec::Define()
{
	Describe("Plan reel", [this]()
	{
		It("valide le plan COMPLET et reconstitue EXACTEMENT les comptes du manifeste", [this]()
		{
			FPlanVille Plan;
			FPlanRapport ROuvre;
			if (!TestTrue(TEXT("Ouvrir(SourceData/PlanVille)"),
				Plan.Ouvrir(FPlanVille::DossierParDefaut(), ROuvre)))
			{
				AddError(ROuvre.Texte());
				return;
			}
			TestEqual(TEXT("Ouvrir n'ouvre que le manifeste + l'index"), Plan.FichiersOuverts(), 2);

			const FPlanIndex& Idx = Plan.Index();
			const double TOuvre = ROuvre.Secondes;

			FPlanRapport R;
			const bool bOk = Plan.ValiderTout(R);
			UE_LOG(LogPlanVille, Display,
				TEXT("E2-0 CHRONO : ouverture de l'index %.3f s ; validation du plan COMPLET "
					 "%.2f s (%d fichiers verifies)."),
				TOuvre, R.Secondes, R.FichiersVerifies);

			if (!TestTrue(TEXT("ValiderTout"), bOk))
			{
				AddError(R.Texte());
				return;
			}

			// Les comptes du manifeste, confrontes a ce que le lecteur a recompte.
			TestEqual(TEXT("parcelles distinctes = manifeste"), R.ParcellesDistinctes, Idx.ParcellesDistinctes);
			TestEqual(TEXT("interfaces distinctes = manifeste"), R.InterfacesDistinctes, Idx.InterfacesDistinctes);
			TestEqual(TEXT("instances = manifeste"), R.Instances, Idx.Instances);
			TestEqual(TEXT("parcelles pieces = manifeste"), R.ParcellesPieces, Idx.ParcellesPieces);
			TestEqual(TEXT("interfaces pieces = manifeste"), R.InterfacesPieces, Idx.InterfacesPieces);
			TestEqual(TEXT("aucun defaut"), R.Defauts.Num(), 0);

			FString Refusees;
			for (const FIntPoint& C : R.CellulesRefusees)
			{
				Refusees += FString::Printf(TEXT("%s(%d, %d)"),
					Refusees.IsEmpty() ? TEXT("") : TEXT(", "), C.X, C.Y);
			}
			UE_LOG(LogPlanVille, Display,
				TEXT("E2-0 CHIFFRES : %d parcelles distinctes / %d pieces ; %d interfaces "
					 "distinctes / %d pieces ; %d instances ; %d cellules au domaine ; "
					 "cellules REFUSEES a la construction : %s."),
				R.ParcellesDistinctes, R.ParcellesPieces, R.InterfacesDistinctes,
				R.InterfacesPieces, R.Instances, Idx.Cellules.Num(),
				Refusees.IsEmpty() ? TEXT("aucune") : *Refusees);

			// Les secteurs non constructibles SORTENT des donnees : on ne verifie
			// pas QUELLES cellules, on verifie que la liste est celle des cellules
			// qui portent une interface `arbitrage_demande`, et rien d'autre.
			for (const FIntPoint& C : R.CellulesRefusees)
			{
				FPlanCellule Cel;
				FPlanRapport RC;
				TestTrue(FString::Printf(TEXT("cellule refusee (%d, %d) lisible"), C.X, C.Y),
					Plan.ChargerCellule(C, Cel, RC));
				TestFalse(FString::Printf(TEXT("cellule (%d, %d) NON constructible"), C.X, C.Y),
					Cel.bConstructible);
				TestTrue(FString::Printf(TEXT("cellule (%d, %d) porte au moins un arbitrage"), C.X, C.Y),
					Cel.ArbitragesN > 0);
			}
		});

		It("DISTRICT-FIRST : charger une cellule n'ouvre que ses trois side-cars", [this]()
		{
			FPlanVille Plan;
			FPlanRapport R;
			if (!TestTrue(TEXT("Ouvrir"), Plan.Ouvrir(FPlanVille::DossierParDefaut(), R))) { return; }

			const FPlanIndex& Idx = Plan.Index();
			if (!TestTrue(TEXT("le domaine n'est pas vide"), Idx.Cellules.Num() > 0)) { return; }

			// La cellule vient DES DONNEES (la plus legere du domaine), jamais
			// d'une coordonnee ecrite dans le code.
			const FIntPoint Cell = CelluleLaPlusLegere(Idx);
			const FPlanIndex::FComptesCellule& Att = Idx.ParCellule[Cell];

			const int32 Avant = Plan.FichiersOuverts();
			FPlanCellule C;
			FPlanRapport RC;
			const double T0 = FPlatformTime::Seconds();
			const bool bOk = Plan.ChargerCellule(Cell, C, RC);
			const double Secondes = FPlatformTime::Seconds() - T0;

			if (!TestTrue(FString::Printf(TEXT("ChargerCellule(%d, %d)"), Cell.X, Cell.Y), bOk))
			{
				AddError(RC.Texte());
				return;
			}

			TestEqual(TEXT("exactement 3 fichiers ouverts en plus"),
				Plan.FichiersOuverts() - Avant, 3);
			TestEqual(TEXT("parcelles de la cellule = index"), C.Parcelles.Num(), Att.Parcelles);
			TestEqual(TEXT("interfaces de la cellule = index"), C.Interfaces.Num(), Att.Interfaces);
			TestEqual(TEXT("instances de la cellule = index"), C.Semis.Num(), Att.Instances);
			TestTrue(TEXT("le catalogue des resolutions vient du fichier"), C.Catalogue.Num() > 0);

			UE_LOG(LogPlanVille, Display,
				TEXT("E2-0 CHRONO : cellule (%d, %d) SEULE en %.3f s — %d parcelles, "
					 "%d interfaces, %d instances, %d fichiers ouverts au total."),
				Cell.X, Cell.Y, Secondes, C.Parcelles.Num(), C.Interfaces.Num(),
				C.Semis.Num(), Plan.FichiersOuverts());

			// LE CHRONO QUI COMPTE POUR E2-1 : la cellule la plus LOURDE du
			// domaine, chargee SEULE depuis un lecteur neuf. La cellule legere
			// ci-dessus prouve le district-first ; celle-ci donne le cout reel.
			{
				const FIntPoint Lourde = CelluleExtreme(Idx, true);
				FPlanVille Neuf;
				FPlanRapport RN;
				if (TestTrue(TEXT("Ouvrir (lecteur neuf)"),
					Neuf.Ouvrir(FPlanVille::DossierParDefaut(), RN)))
				{
					FPlanCellule CL;
					FPlanRapport RL;
					const double T1 = FPlatformTime::Seconds();
					const bool bOkL = Neuf.ChargerCellule(Lourde, CL, RL);
					const double SecondesL = FPlatformTime::Seconds() - T1;
					if (TestTrue(FString::Printf(TEXT("ChargerCellule(%d, %d) la plus lourde"),
						Lourde.X, Lourde.Y), bOkL))
					{
						TestEqual(TEXT("la cellule lourde n'ouvre que 3 fichiers"),
							Neuf.FichiersOuverts(), 5);
						UE_LOG(LogPlanVille, Display,
							TEXT("E2-0 CHRONO : cellule LOURDE (%d, %d) SEULE en %.3f s — "
								 "%d parcelles, %d interfaces, %d instances."),
							Lourde.X, Lourde.Y, SecondesL, CL.Parcelles.Num(),
							CL.Interfaces.Num(), CL.Semis.Num());
					}
					else
					{
						AddError(RL.Texte());
					}
				}
			}

			// Une cellule hors domaine n'est pas devinee.
			FPlanCellule Hors;
			FPlanRapport RHors;
			FIntPoint Loin(MAX_int32 / 2, MAX_int32 / 2);
			TestFalse(TEXT("une cellule hors domaine est REFUSEE"),
				Plan.ChargerCellule(Loin, Hors, RHors));
		});
	});

	Describe("Garde", [this]()
	{
		It("SABOTAGE : un octet change dans une copie du plan -> REFUS (SourceData intact)", [this]()
		{
			FPlanVille Reel;
			FPlanRapport RR;
			if (!TestTrue(TEXT("Ouvrir le plan reel"),
				Reel.Ouvrir(FPlanVille::DossierParDefaut(), RR))) { return; }
			const FIntPoint Cell = CelluleLaPlusLegere(Reel.Index());

			FString Dir;
			if (!Miroir(Cell, { TEXT("qui"), TEXT("interfaces"), TEXT("semis") }, Dir)) { return; }

			// --- (a) un side-car sabote : l'index passe, la CELLULE est refusee.
			{
				const FString Cible = FPaths::Combine(Dir, TEXT("data"),
					FPlanVille::NomSideCar(TEXT("qui"), Cell));
				FString Avant, Apres;
				if (!TestTrue(TEXT("sabotage du side-car"), SaboteUnOctet(Cible, Avant, Apres))) { return; }
				UE_LOG(LogPlanVille, Display, TEXT("E2-0 SABOTAGE side-car : %s -> %s"), *Avant, *Apres);

				AddExpectedErrorPlain(TEXT("PLAN REFUSE"), EAutomationExpectedErrorFlags::Contains, 0);

				FPlanVille P;
				FPlanRapport R;
				TestTrue(TEXT("l'index de la copie s'ouvre (il n'a pas bouge)"), P.Ouvrir(Dir, R));

				FPlanCellule C;
				FPlanRapport RC;
				TestFalse(TEXT("la cellule sabotee est REFUSEE"), P.ChargerCellule(Cell, C, RC));
				TestTrue(TEXT("le motif est l'empreinte"),
					CompteDefauts(RC, TEXT("EMPREINTE FAUSSE")) >= 1);
				TestEqual(TEXT("aucune parcelle n'est retenue d'un fichier refuse"), C.Parcelles.Num(), 0);
			}

			// --- (b) l'INDEX sabote : plus rien ne s'ouvre du tout.
			{
				const FString Cible = FPaths::Combine(Dir, TEXT("data/plan_index.json"));
				FString Avant, Apres;
				if (!TestTrue(TEXT("sabotage de l'index"), SaboteUnOctet(Cible, Avant, Apres))) { return; }
				UE_LOG(LogPlanVille, Display, TEXT("E2-0 SABOTAGE index : %s -> %s"), *Avant, *Apres);

				FPlanVille P;
				FPlanRapport R;
				TestFalse(TEXT("un index a l'empreinte fausse REFUSE le plan"), P.Ouvrir(Dir, R));
				TestTrue(TEXT("le motif est l'empreinte"),
					CompteDefauts(R, TEXT("EMPREINTE FAUSSE")) >= 1);
			}

			VideBacASable();

			// --- (c) SourceData n'a pas bouge : le plan reel repasse.
			FPlanCellule Verif;
			FPlanRapport RV;
			TestTrue(TEXT("SourceData INTACT : la cellule reelle se recharge"),
				Reel.ChargerCellule(Cell, Verif, RV));
		});

		It("MANQUE : un fichier declare par l'index mais absent -> REFUS", [this]()
		{
			FPlanVille Reel;
			FPlanRapport RR;
			if (!TestTrue(TEXT("Ouvrir le plan reel"),
				Reel.Ouvrir(FPlanVille::DossierParDefaut(), RR))) { return; }
			const FIntPoint Cell = CelluleLaPlusLegere(Reel.Index());
			const int32 FichiersDeclares = Reel.Index().Fichiers.Num();

			// Le miroir ne recoit QUE deux des trois side-cars : le semis manque.
			FString Dir;
			if (!Miroir(Cell, { TEXT("qui"), TEXT("interfaces") }, Dir)) { return; }

			AddExpectedErrorPlain(TEXT("PLAN REFUSE"), EAutomationExpectedErrorFlags::Contains, 0);

			FPlanVille P;
			FPlanRapport R;
			TestTrue(TEXT("l'index s'ouvre (il est complet et juste)"), P.Ouvrir(Dir, R));

			FPlanCellule C;
			FPlanRapport RC;
			TestFalse(TEXT("la cellule amputee est REFUSEE"), P.ChargerCellule(Cell, C, RC));
			TestEqual(TEXT("un seul fichier manquant, un seul defaut d'absence"),
				CompteDefauts(RC, TEXT("FICHIER ABSENT")), 1);

			// Et la garde de build refuse l'ensemble, en listant tout ce qui manque.
			FPlanRapport RT;
			TestFalse(TEXT("ValiderTout REFUSE un plan incomplet"), P.ValiderTout(RT));
			TestTrue(FString::Printf(
				TEXT("au moins %d absences listees (side-cars declares non copies)"),
				FichiersDeclares - 2),
				CompteDefauts(RT, TEXT("FICHIER ABSENT")) >= FichiersDeclares - 2);
			TestTrue(TEXT("le refus est EXPLICITE (chaque defaut nomme son fichier)"),
				RT.Texte().Contains(TEXT("FICHIER ABSENT")));

			UE_LOG(LogPlanVille, Display,
				TEXT("E2-0 MANQUE : %d defauts listes, dont %d absences de fichier."),
				RT.Defauts.Num(), CompteDefauts(RT, TEXT("FICHIER ABSENT")));

			VideBacASable();
		});
	});
}
