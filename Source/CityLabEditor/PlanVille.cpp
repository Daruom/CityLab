#include "PlanVille.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogPlanVille);

// =============================================================================
// LES NOMS DU CONTRAT.
// Ce sont des MOTS DE SCHEMA (des noms de champs et de familles), jamais des
// identifiants ni des coordonnees de la ville : la liste des cellules, celle
// des fichiers, celle des resolutions licites et celle des secteurs refuses
// SORTENT toutes des donnees. Rien de la ville n'est ecrit ici.
// =============================================================================
namespace PlanVilleNoms
{
	static const TCHAR* Manifeste = TEXT("plan.json");
	static const TCHAR* DossierDonnees = TEXT("data");

	static const TCHAR* FamilleQui = TEXT("qui");
	static const TCHAR* FamilleInterfaces = TEXT("interfaces");
	static const TCHAR* FamilleSemis = TEXT("semis");

	static const FName ArbitrageDemande(TEXT("arbitrage_demande"));
}

namespace
{
	/** md5 hexadecimal minuscule d'un bloc d'octets. */
	FString Md5De(const uint8* Donnees, int64 Taille)
	{
		FMD5 H;
		H.Update(Donnees, Taille);
		uint8 D[16];
		H.Final(D);
		FString S;
		S.Reserve(32);
		for (int32 i = 0; i < 16; ++i)
		{
			S += FString::Printf(TEXT("%02x"), D[i]);
		}
		return S;
	}

	/** Les memes octets, CRLF normalises en LF (le « contenu logique »). */
	FString Md5LogiqueDe(const TArray<uint8>& Octets)
	{
		TArray<uint8> Normalise;
		Normalise.Reserve(Octets.Num());
		for (int32 i = 0; i < Octets.Num(); ++i)
		{
			if (Octets[i] == '\r' && i + 1 < Octets.Num() && Octets[i + 1] == '\n')
			{
				continue;   // CRLF -> LF
			}
			Normalise.Add(Octets[i]);
		}
		return Md5De(Normalise.GetData(), Normalise.Num());
	}

	EPlanForme FormeDe(const FString& S)
	{
		if (S == TEXT("constante")) { return EPlanForme::Constante; }
		if (S == TEXT("profil_troncon")) { return EPlanForme::ProfilTroncon; }
		if (S == TEXT("drapage")) { return EPlanForme::Drapage; }
		return EPlanForme::Inconnue;
	}

	EPlanMatiere MatiereDe(const FString& S)
	{
		if (S == TEXT("mineral")) { return EPlanMatiere::Mineral; }
		if (S == TEXT("vegetal")) { return EPlanMatiere::Vegetal; }
		if (S == TEXT("eau")) { return EPlanMatiere::Eau; }
		return EPlanMatiere::Inconnue;
	}

	EPlanProprio ProprioDe(const FString& S)
	{
		if (S == TEXT("batiment")) { return EPlanProprio::Batiment; }
		if (S == TEXT("voirie")) { return EPlanProprio::Voirie; }
		if (S == TEXT("ouvrage")) { return EPlanProprio::Ouvrage; }
		if (S == TEXT("zone")) { return EPlanProprio::Zone; }
		if (S == TEXT("organique")) { return EPlanProprio::Organique; }
		return EPlanProprio::Inconnu;
	}

	/** `[[x, y], ...]` -> points, en METRES (aucune conversion moteur ici). */
	void LitPoints(const TArray<TSharedPtr<FJsonValue>>* Src, TArray<FVector2D>& Out)
	{
		if (!Src) { return; }
		Out.Reserve(Src->Num());
		for (const TSharedPtr<FJsonValue>& V : *Src)
		{
			const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
			if (V.IsValid() && V->TryGetArray(P) && P->Num() >= 2)
			{
				Out.Add(FVector2D((*P)[0]->AsNumber(), (*P)[1]->AsNumber()));
			}
		}
	}

	/** `[[[x, y], ...], ...]` -> anneaux / polylignes, en METRES. */
	void LitLignes(const TSharedPtr<FJsonObject>& O, const TCHAR* Champ,
		TArray<TArray<FVector2D>>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!O->TryGetArrayField(Champ, Arr)) { return; }
		Out.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TArray<TSharedPtr<FJsonValue>>* L = nullptr;
			if (V.IsValid() && V->TryGetArray(L))
			{
				TArray<FVector2D> Ligne;
				LitPoints(L, Ligne);
				Out.Add(MoveTemp(Ligne));
			}
		}
	}
}

// =============================================================================
// FPlanRapport
// =============================================================================

void FPlanRapport::Ajoute(const FString& Fichier, const FString& Motif)
{
	FPlanDefaut D;
	D.Fichier = Fichier;
	D.Motif = Motif;
	Defauts.Add(MoveTemp(D));
	// Chaque defaut est journalise en AVERTISSEMENT : c'est un constat. Le REFUS
	// (le verdict) est journalise en ERREUR par l'appelant, une seule fois, avec
	// la liste complete — ainsi un test qui ATTEND un refus n'a qu'une erreur a
	// declarer, et le log de build reste lisible.
	UE_LOG(LogPlanVille, Warning, TEXT("PLAN, defaut — %s%s"),
		Fichier.IsEmpty() ? TEXT("") : *(Fichier + TEXT(" : ")), *Motif);
}

FString FPlanRapport::Texte(int32 MaxDefauts) const
{
	if (EstValide())
	{
		return FString::Printf(
			TEXT("PLAN VALIDE — %d fichiers verifies (dont %d sur le contenu logique), ")
			TEXT("%d parcelles distinctes / %d pieces, %d interfaces distinctes / %d pieces, ")
			TEXT("%d instances, %d cellule(s) REFUSEE(S) a la construction, %.2f s."),
			FichiersVerifies, FichiersAcceptesEnLogique,
			ParcellesDistinctes, ParcellesPieces,
			InterfacesDistinctes, InterfacesPieces,
			Instances, CellulesRefusees.Num(), Secondes);
	}

	FString S = FString::Printf(TEXT("PLAN REFUSE — %d defaut(s) :"), Defauts.Num());
	const int32 N = FMath::Min(Defauts.Num(), FMath::Max(MaxDefauts, 1));
	for (int32 i = 0; i < N; ++i)
	{
		S += FString::Printf(TEXT("\n  [%d] %s%s"), i + 1,
			Defauts[i].Fichier.IsEmpty() ? TEXT("") : *(Defauts[i].Fichier + TEXT(" : ")),
			*Defauts[i].Motif);
	}
	if (Defauts.Num() > N)
	{
		S += FString::Printf(TEXT("\n  ... et %d autre(s)."), Defauts.Num() - N);
	}
	return S;
}

// =============================================================================
// FPlanVille — outils de nommage
// =============================================================================

FString FPlanVille::DossierParDefaut()
{
	return FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/PlanVille"));
}

FString FPlanVille::TexteDeCellule(const FIntPoint& Cell)
{
	return FString::Printf(TEXT("%d_%d"), Cell.X, Cell.Y);
}

FString FPlanVille::NomSideCar(const TCHAR* Famille, const FIntPoint& Cell)
{
	return FString::Printf(TEXT("plan_%s_%s.json"), Famille, *TexteDeCellule(Cell));
}

bool FPlanVille::CelluleDepuisTexte(const FString& S, FIntPoint& Out)
{
	// `<cx>_<cy>` : cx ne contient jamais de '_', le PREMIER separe les deux.
	int32 Sep = INDEX_NONE;
	if (!S.FindChar(TEXT('_'), Sep) || Sep <= 0 || Sep + 1 >= S.Len())
	{
		return false;
	}
	const FString G = S.Left(Sep);
	const FString D = S.Mid(Sep + 1);
	if (!G.IsNumeric() || !D.IsNumeric())
	{
		return false;
	}
	Out = FIntPoint(FCString::Atoi(*G), FCString::Atoi(*D));
	return true;
}

// =============================================================================
// LA GARDE D'EMPREINTE — double, octets ET contenu logique LF.
// =============================================================================

bool FPlanVille::LireEtVerifier(const FString& Chemin, const FString& NomCourt,
	const FPlanIndex::FEmpreinte& Attendue, FString& OutTexte, FPlanRapport& Rapport)
{
	++NbFichiersOuverts;

	TArray<uint8> Octets;
	if (!FFileHelper::LoadFileToArray(Octets, *Chemin))
	{
		Rapport.Ajoute(NomCourt, FString::Printf(
			TEXT("FICHIER ABSENT OU ILLISIBLE ('%s'). Le manifeste le declare "
				 "(%lld octets, md5 %s) : le plan est INCOMPLET."),
			*Chemin, Attendue.Octets, *Attendue.Md5Octets));
		return false;
	}

	if (Attendue.Md5Octets.IsEmpty() && Attendue.Md5Logique.IsEmpty())
	{
		Rapport.Ajoute(NomCourt, TEXT("AUCUNE EMPREINTE PUBLIEE pour ce fichier : ")
			TEXT("un plan sans garde n'est pas un plan (garde etage 2)."));
		return false;
	}

	const FString CalcOctets = Md5De(Octets.GetData(), Octets.Num());
	if (CalcOctets == Attendue.Md5Octets)
	{
		++Rapport.FichiersVerifies;
		FFileHelper::BufferToString(OutTexte, Octets.GetData(), Octets.Num());
		return true;
	}

	// Les octets ont bouge : reste le CONTENU LOGIQUE (fins de ligne en LF).
	// Cas deja paye sur `carte_v2.json` : 660 913 fins de ligne traduites par le
	// systeme, fichier INTACT, empreinte d'octets fausse.
	const FString CalcLogique = Md5LogiqueDe(Octets);
	if (!Attendue.Md5Logique.IsEmpty() && CalcLogique == Attendue.Md5Logique)
	{
		++Rapport.FichiersVerifies;
		++Rapport.FichiersAcceptesEnLogique;
		UE_LOG(LogPlanVille, Display,
			TEXT("PLAN : '%s' verifie sur le CONTENU LOGIQUE (%s) — ses octets donnent %s ")
			TEXT("(fins de ligne traduites depuis la compilation). Le contenu n'a pas bouge."),
			*NomCourt, *CalcLogique, *CalcOctets);
		FFileHelper::BufferToString(OutTexte, Octets.GetData(), Octets.Num());
		return true;
	}

	Rapport.Ajoute(NomCourt, FString::Printf(
		TEXT("EMPREINTE FAUSSE — attendu octets %s / logique %s ; calcule octets %s / "
			 "logique %s. Taille attendue %lld, lue %d. Le fichier n'est pas celui que "
			 "le manifeste declare : rien n'est devine, le plan est REFUSE."),
		*Attendue.Md5Octets, *Attendue.Md5Logique, *CalcOctets, *CalcLogique,
		Attendue.Octets, Octets.Num()));
	return false;
}

// =============================================================================
// Ouvrir() — le manifeste et l'index, et RIEN d'autre (district-first).
// =============================================================================

bool FPlanVille::Ouvrir(const FString& Dossier, FPlanRapport& Rapport)
{
	const double T0 = FPlatformTime::Seconds();
	Idx = FPlanIndex();
	Racine = Dossier;
	NbFichiersOuverts = 0;

	// TOUT refus de la garde se journalise en ERREUR, avec la liste complete —
	// il n'existe aucun chemin de sortie muet (« jamais de passage partiel
	// silencieux »). C'est aussi ce qui rend le refus OBSERVABLE par un test.
	auto Refuse = [T0](FPlanRapport& R) -> bool
	{
		R.Secondes = FPlatformTime::Seconds() - T0;
		UE_LOG(LogPlanVille, Error, TEXT("%s"), *R.Texte());
		return false;
	};

	// --- 1. Le manifeste. C'est la RACINE DE CONFIANCE : il n'a pas d'empreinte
	//        de lui-meme, il porte celle de tous les autres.
	const FString CheminManifeste = FPaths::Combine(Dossier, PlanVilleNoms::Manifeste);
	FString TexteManifeste;
	++NbFichiersOuverts;
	if (!FFileHelper::LoadFileToString(TexteManifeste, *CheminManifeste))
	{
		Rapport.Ajoute(PlanVilleNoms::Manifeste, FString::Printf(
			TEXT("MANIFESTE ABSENT ('%s') : aucun plan a lire."), *CheminManifeste));
		return Refuse(Rapport);
	}

	TSharedPtr<FJsonObject> Man;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TexteManifeste), Man) || !Man.IsValid())
	{
		Rapport.Ajoute(PlanVilleNoms::Manifeste, TEXT("MANIFESTE ILLISIBLE (JSON invalide)."));
		return Refuse(Rapport);
	}

	Man->TryGetStringField(TEXT("version"), Idx.Version);

	const TSharedPtr<FJsonObject>* IndexDonnees = nullptr;
	if (!Man->TryGetObjectField(TEXT("index_donnees"), IndexDonnees) || !IndexDonnees->IsValid())
	{
		Rapport.Ajoute(PlanVilleNoms::Manifeste,
			TEXT("Pas de bloc `index_donnees` : le manifeste ne designe aucun index de donnees."));
		return Refuse(Rapport);
	}

	FString FichierIndex;
	FPlanIndex::FEmpreinte EmpIndex;
	(*IndexDonnees)->TryGetStringField(TEXT("fichier"), FichierIndex);
	(*IndexDonnees)->TryGetStringField(TEXT("md5_octets"), EmpIndex.Md5Octets);
	(*IndexDonnees)->TryGetStringField(TEXT("md5_logique"), EmpIndex.Md5Logique);
	{
		double O = 0.0;
		(*IndexDonnees)->TryGetNumberField(TEXT("octets"), O);
		EmpIndex.Octets = (int64)O;
	}
	if (FichierIndex.IsEmpty())
	{
		Rapport.Ajoute(PlanVilleNoms::Manifeste, TEXT("`index_donnees.fichier` vide."));
		return Refuse(Rapport);
	}

	// Les totaux DU MANIFESTE : c'est lui qui fait foi (S12).
	int32 ManParcellesDistinctes = 0, ManParcellesPieces = 0;
	int32 ManInterfacesDistinctes = 0, ManInterfacesPieces = 0, ManInstances = 0;
	const TSharedPtr<FJsonObject>* ManTotaux = nullptr;
	if ((*IndexDonnees)->TryGetObjectField(TEXT("totaux"), ManTotaux) && ManTotaux->IsValid())
	{
		(*ManTotaux)->TryGetNumberField(TEXT("parcelles_distinctes"), ManParcellesDistinctes);
		(*ManTotaux)->TryGetNumberField(TEXT("parcelles_pieces"), ManParcellesPieces);
		(*ManTotaux)->TryGetNumberField(TEXT("interfaces_distinctes"), ManInterfacesDistinctes);
		(*ManTotaux)->TryGetNumberField(TEXT("interfaces_pieces"), ManInterfacesPieces);
		(*ManTotaux)->TryGetNumberField(TEXT("instances"), ManInstances);
	}
	else
	{
		Rapport.Ajoute(PlanVilleNoms::Manifeste, TEXT("`index_donnees.totaux` absent."));
	}

	// Le domaine du manifeste : les cellules que le plan pretend couvrir.
	TSet<FString> DomaineManifeste;
	const TSharedPtr<FJsonObject>* ManDomaine = nullptr;
	if (Man->TryGetObjectField(TEXT("domaine"), ManDomaine) && ManDomaine->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Cs = nullptr;
		if ((*ManDomaine)->TryGetArrayField(TEXT("cellules"), Cs))
		{
			for (const TSharedPtr<FJsonValue>& V : *Cs) { DomaineManifeste.Add(V->AsString()); }
		}
	}

	// --- 2. L'index de donnees, sous garde d'empreinte.
	const FString CheminIndex = FPaths::Combine(Dossier, FichierIndex);
	FString TexteIndex;
	if (!LireEtVerifier(CheminIndex, FichierIndex, EmpIndex, TexteIndex, Rapport))
	{
		return Refuse(Rapport);
	}

	TSharedPtr<FJsonObject> Ix;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TexteIndex), Ix) || !Ix.IsValid())
	{
		Rapport.Ajoute(FichierIndex, TEXT("INDEX ILLISIBLE (JSON invalide) malgre une empreinte juste."));
		return Refuse(Rapport);
	}

	Ix->TryGetStringField(TEXT("version"), Idx.VersionDonnees);

	const TSharedPtr<FJsonObject>* IxDomaine = nullptr;
	if (Ix->TryGetObjectField(TEXT("domaine"), IxDomaine) && IxDomaine->IsValid())
	{
		(*IxDomaine)->TryGetNumberField(TEXT("cellule_m"), Idx.CelluleM);
		const TArray<TSharedPtr<FJsonValue>>* Cs = nullptr;
		if ((*IxDomaine)->TryGetArrayField(TEXT("cellules"), Cs))
		{
			for (const TSharedPtr<FJsonValue>& V : *Cs)
			{
				const FString S = V->AsString();
				FIntPoint C;
				if (!CelluleDepuisTexte(S, C))
				{
					Rapport.Ajoute(FichierIndex, FString::Printf(
						TEXT("Cellule '%s' du domaine illisible."), *S));
					continue;
				}
				Idx.Cellules.Add(C);
				if (DomaineManifeste.Num() > 0 && !DomaineManifeste.Contains(S))
				{
					Rapport.Ajoute(FichierIndex, FString::Printf(
						TEXT("INDEX INCOHERENT AVEC LE MANIFESTE : la cellule '%s' est dans "
							 "l'index mais pas dans le domaine du manifeste."), *S));
				}
			}
		}
	}
	if (Idx.Cellules.Num() == 0)
	{
		Rapport.Ajoute(FichierIndex, TEXT("Domaine VIDE : l'index ne declare aucune cellule."));
	}
	if (DomaineManifeste.Num() > 0 && DomaineManifeste.Num() != Idx.Cellules.Num())
	{
		Rapport.Ajoute(FichierIndex, FString::Printf(
			TEXT("INDEX INCOHERENT AVEC LE MANIFESTE : %d cellules au manifeste, %d a l'index."),
			DomaineManifeste.Num(), Idx.Cellules.Num()));
	}

	// --- 3. Les totaux de l'index, confrontes a ceux du manifeste.
	const TSharedPtr<FJsonObject>* IxTotaux = nullptr;
	if (Ix->TryGetObjectField(TEXT("totaux"), IxTotaux) && IxTotaux->IsValid())
	{
		(*IxTotaux)->TryGetNumberField(TEXT("parcelles_distinctes"), Idx.ParcellesDistinctes);
		(*IxTotaux)->TryGetNumberField(TEXT("parcelles_pieces"), Idx.ParcellesPieces);
		(*IxTotaux)->TryGetNumberField(TEXT("interfaces_distinctes"), Idx.InterfacesDistinctes);
		(*IxTotaux)->TryGetNumberField(TEXT("interfaces_pieces"), Idx.InterfacesPieces);
		(*IxTotaux)->TryGetNumberField(TEXT("instances"), Idx.Instances);
	}
	else
	{
		Rapport.Ajoute(FichierIndex, TEXT("`totaux` absent de l'index."));
	}

	auto Confronte = [&](const TCHAR* Quoi, int32 AuManifeste, int32 ALIndex)
	{
		if (AuManifeste != ALIndex)
		{
			Rapport.Ajoute(FichierIndex, FString::Printf(
				TEXT("INDEX INCOHERENT AVEC LE MANIFESTE sur `%s` : manifeste %d, index %d."),
				Quoi, AuManifeste, ALIndex));
		}
	};
	Confronte(TEXT("parcelles_distinctes"), ManParcellesDistinctes, Idx.ParcellesDistinctes);
	Confronte(TEXT("parcelles_pieces"), ManParcellesPieces, Idx.ParcellesPieces);
	Confronte(TEXT("interfaces_distinctes"), ManInterfacesDistinctes, Idx.InterfacesDistinctes);
	Confronte(TEXT("interfaces_pieces"), ManInterfacesPieces, Idx.InterfacesPieces);
	Confronte(TEXT("instances"), ManInstances, Idx.Instances);

	// --- 4. L'inventaire des side-cars, avec leur double empreinte.
	const TSharedPtr<FJsonObject>* IxFichiers = nullptr;
	if (Ix->TryGetObjectField(TEXT("fichiers"), IxFichiers) && IxFichiers->IsValid())
	{
		for (const auto& KV : (*IxFichiers)->Values)
		{
			const TSharedPtr<FJsonObject>* E = nullptr;
			if (!KV.Value.IsValid() || !KV.Value->TryGetObject(E) || !E->IsValid()) { continue; }
			FPlanIndex::FEmpreinte Emp;
			(*E)->TryGetStringField(TEXT("md5_octets"), Emp.Md5Octets);
			(*E)->TryGetStringField(TEXT("md5_logique"), Emp.Md5Logique);
			double O = 0.0;
			(*E)->TryGetNumberField(TEXT("octets"), O);
			Emp.Octets = (int64)O;
			// UE 5.8 : les cles de FJsonObject sont des UE::FSharedString.
			Idx.Fichiers.Add(FString(*KV.Key), Emp);
		}
	}
	if (Idx.Fichiers.Num() == 0)
	{
		Rapport.Ajoute(FichierIndex, TEXT("`fichiers` absent ou vide : l'index n'inventorie rien."));
	}

	// --- 5. Les comptes attendus par cellule.
	const TSharedPtr<FJsonObject>* IxParCellule = nullptr;
	if (Ix->TryGetObjectField(TEXT("par_cellule"), IxParCellule) && IxParCellule->IsValid())
	{
		for (const auto& KV : (*IxParCellule)->Values)
		{
			FIntPoint C;
			if (!CelluleDepuisTexte(FString(*KV.Key), C)) { continue; }
			const TSharedPtr<FJsonObject>* E = nullptr;
			if (!KV.Value.IsValid() || !KV.Value->TryGetObject(E) || !E->IsValid()) { continue; }
			FPlanIndex::FComptesCellule Cc;
			(*E)->TryGetNumberField(TEXT("parcelles"), Cc.Parcelles);
			(*E)->TryGetNumberField(TEXT("interfaces"), Cc.Interfaces);
			(*E)->TryGetNumberField(TEXT("instances"), Cc.Instances);
			Idx.ParCellule.Add(C, Cc);
		}
	}
	if (Idx.ParCellule.Num() != Idx.Cellules.Num())
	{
		Rapport.Ajoute(FichierIndex, FString::Printf(
			TEXT("INDEX INCOHERENT : %d cellules au domaine, %d dans `par_cellule`."),
			Idx.Cellules.Num(), Idx.ParCellule.Num()));
	}

	// --- 6. L'inventaire couvre-t-il EXACTEMENT les 3 familles de chaque cellule ?
	//        Un fichier declare en trop est aussi grave qu'un fichier manquant :
	//        l'index doit dire la verite, toute la verite.
	{
		TSet<FString> Attendus;
		for (const FIntPoint& C : Idx.Cellules)
		{
			Attendus.Add(NomSideCar(PlanVilleNoms::FamilleQui, C));
			Attendus.Add(NomSideCar(PlanVilleNoms::FamilleInterfaces, C));
			Attendus.Add(NomSideCar(PlanVilleNoms::FamilleSemis, C));
		}
		for (const FString& A : Attendus)
		{
			if (!Idx.Fichiers.Contains(A))
			{
				Rapport.Ajoute(FichierIndex, FString::Printf(
					TEXT("INDEX INCOMPLET : '%s' n'est pas inventorie alors que sa cellule "
						 "est au domaine."), *A));
			}
		}
		for (const auto& KV : Idx.Fichiers)
		{
			if (!Attendus.Contains(KV.Key))
			{
				Rapport.Ajoute(FichierIndex, FString::Printf(
					TEXT("INDEX INCOHERENT : '%s' est inventorie mais ne correspond a aucune "
						 "cellule du domaine."), *KV.Key));
			}
		}
	}

	Idx.bCharge = Rapport.EstValide();
	Rapport.Secondes = FPlatformTime::Seconds() - T0;

	if (!Idx.bCharge)
	{
		return Refuse(Rapport);
	}

	UE_LOG(LogPlanVille, Display,
		TEXT("PLAN '%s' (%s) ouvert : %d cellules de %.0f m, %d side-cars inventories ; "
			 "manifeste = %d parcelles / %d interfaces / %d instances. %.3f s, %d fichiers ouverts."),
		*Idx.Version, *Idx.VersionDonnees, Idx.Cellules.Num(), Idx.CelluleM, Idx.Fichiers.Num(),
		Idx.ParcellesDistinctes, Idx.InterfacesDistinctes, Idx.Instances,
		Rapport.Secondes, NbFichiersOuverts);
	return true;
}

// =============================================================================
// Le chargement d'un side-car : empreinte PUIS parse. Jamais l'inverse.
// =============================================================================

bool FPlanVille::ChargeSideCar(const TCHAR* Famille, const FIntPoint& Cell,
	TSharedPtr<FJsonObject>& OutRoot, FPlanRapport& Rapport)
{
	const FString Nom = NomSideCar(Famille, Cell);
	const FPlanIndex::FEmpreinte* Emp = Idx.Fichiers.Find(Nom);
	if (!Emp)
	{
		Rapport.Ajoute(Nom, TEXT("Fichier NON INVENTORIE par l'index : rien ne dit ce qu'il ")
			TEXT("devrait contenir, il ne peut pas etre lu."));
		return false;
	}

	const FString Chemin = FPaths::Combine(Racine, PlanVilleNoms::DossierDonnees, Nom);
	FString Texte;
	if (!LireEtVerifier(Chemin, Nom, *Emp, Texte, Rapport))
	{
		return false;
	}

	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Texte), OutRoot) || !OutRoot.IsValid())
	{
		Rapport.Ajoute(Nom, TEXT("JSON invalide malgre une empreinte juste."));
		return false;
	}

	// Le side-car dit-il bien de QUELLE cellule il parle ?
	const TArray<TSharedPtr<FJsonValue>>* CellArr = nullptr;
	if (OutRoot->TryGetArrayField(TEXT("cell"), CellArr) && CellArr->Num() >= 2)
	{
		const FIntPoint Dit((int32)(*CellArr)[0]->AsNumber(), (int32)(*CellArr)[1]->AsNumber());
		if (Dit != Cell)
		{
			Rapport.Ajoute(Nom, FString::Printf(
				TEXT("Le side-car declare la cellule (%d, %d) alors qu'il porte le nom de "
					 "(%d, %d)."), Dit.X, Dit.Y, Cell.X, Cell.Y));
			return false;
		}
	}
	else
	{
		Rapport.Ajoute(Nom, TEXT("Champ `cell` absent : le side-car ne dit pas de quelle cellule il parle."));
		return false;
	}

	if (Idx.CelluleM > 0.0)
	{
		double Taille = 0.0;
		if (OutRoot->TryGetNumberField(TEXT("cellSizeM"), Taille)
			&& !FMath::IsNearlyEqual(Taille, Idx.CelluleM))
		{
			Rapport.Ajoute(Nom, FString::Printf(
				TEXT("`cellSizeM` = %.3f m alors que le domaine declare %.3f m."),
				Taille, Idx.CelluleM));
			return false;
		}
	}

	return true;
}

// =============================================================================
// ChargerCellule() — DISTRICT-FIRST : trois fichiers, pas un de plus.
// =============================================================================

bool FPlanVille::ChargerCellule(const FIntPoint& Cell, FPlanCellule& Out, FPlanRapport& Rapport)
{
	const double T0 = FPlatformTime::Seconds();
	Out = FPlanCellule();
	Out.Cell = Cell;
	Out.CelluleM = Idx.CelluleM;

	if (!Idx.bCharge)
	{
		Rapport.Ajoute(TEXT(""), TEXT("ChargerCellule() sans index charge : Ouvrir() d'abord."));
		return false;
	}
	if (!Idx.ParCellule.Contains(Cell))
	{
		Rapport.Ajoute(TEXT(""), FString::Printf(
			TEXT("La cellule (%d, %d) n'est PAS au domaine du plan : elle ne sera pas devinee."),
			Cell.X, Cell.Y));
		return false;
	}
	const FPlanIndex::FComptesCellule& Attendus = Idx.ParCellule[Cell];
	const int32 DefautsAvant = Rapport.Defauts.Num();

	// --- QUI : les parcelles, leur proprietaire, leur matiere, leur loi de Z.
	{
		TSharedPtr<FJsonObject> Root;
		if (!ChargeSideCar(PlanVilleNoms::FamilleQui, Cell, Root, Rapport)) { return false; }
		const FString Nom = NomSideCar(PlanVilleNoms::FamilleQui, Cell);

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("parcelles"), Arr))
		{
			Rapport.Ajoute(Nom, TEXT("Champ `parcelles` absent."));
			return false;
		}
		Out.Parcelles.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) { continue; }
			FPlanParcelle P;
			O->TryGetStringField(TEXT("id"), P.Id);
			FString S;
			if (O->TryGetStringField(TEXT("proprietaire"), S)) { P.Proprietaire = ProprioDe(S); }
			if (O->TryGetStringField(TEXT("matiere"), S)) { P.Matiere = MatiereDe(S); }
			O->TryGetNumberField(TEXT("aire_m2"), P.AireM2);
			O->TryGetNumberField(TEXT("aire_totale_m2"), P.AireTotaleM2);
			O->TryGetBoolField(TEXT("entiere"), P.bEntiere);
			O->TryGetBoolField(TEXT("bande"), P.bBande);
			O->TryGetBoolField(TEXT("heritee"), P.bHeritee);
			O->TryGetBoolField(TEXT("trou_comble"), P.bTrouComble);
			O->TryGetNumberField(TEXT("largeur_m"), P.LargeurM);
			O->TryGetStringField(TEXT("provenance"), P.Provenance);
			if (O->TryGetStringField(TEXT("cellule_porteuse"), S))
			{
				if (!CelluleDepuisTexte(S, P.CellulePorteuse))
				{
					Rapport.Ajoute(Nom, FString::Printf(
						TEXT("Parcelle '%s' : `cellule_porteuse` = '%s' illisible."), *P.Id, *S));
				}
			}
			else
			{
				Rapport.Ajoute(Nom, FString::Printf(
					TEXT("Parcelle '%s' sans `cellule_porteuse` : les comptes distincts ne "
						 "seraient plus reconstituables."), *P.Id));
			}
			LitLignes(O, TEXT("anneaux"), P.Anneaux);

			const TSharedPtr<FJsonObject>* Loi = nullptr;
			if (O->TryGetObjectField(TEXT("loi"), Loi) && Loi->IsValid())
			{
				FString F;
				(*Loi)->TryGetStringField(TEXT("forme"), F);
				P.Loi.Forme = FormeDe(F);
				if (P.Loi.Forme == EPlanForme::Inconnue)
				{
					Rapport.Ajoute(Nom, FString::Printf(
						TEXT("Parcelle '%s' : loi de Z '%s' hors des TROIS formes du plan."),
						*P.Id, *F));
				}
				(*Loi)->TryGetNumberField(TEXT("z_m"), P.Loi.ZM);
				(*Loi)->TryGetNumberField(TEXT("L_m"), P.Loi.LM);
				(*Loi)->TryGetNumberField(TEXT("pente_max_pc"), P.Loi.PenteMaxPc);
				(*Loi)->TryGetStringField(TEXT("loi_heritee_de"), P.Loi.HeriteeDe);
				const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
				if ((*Loi)->TryGetArrayField(TEXT("axe"), A)) { LitPoints(A, P.Loi.Axe); }
				if ((*Loi)->TryGetArrayField(TEXT("profil"), A)) { LitPoints(A, P.Loi.Profil); }
				if (P.Loi.Forme == EPlanForme::ProfilTroncon
					&& (P.Loi.Axe.Num() < 2 || P.Loi.Profil.Num() < 2))
				{
					Rapport.Ajoute(Nom, FString::Printf(
						TEXT("Parcelle '%s' : loi PROFIL_TRONCON avec un axe de %d point(s) et "
							 "un profil de %d point(s) — inexploitable."),
						*P.Id, P.Loi.Axe.Num(), P.Loi.Profil.Num()));
				}
			}
			else
			{
				Rapport.Ajoute(Nom, FString::Printf(
					TEXT("Parcelle '%s' sans loi de Z : le niveau ne se devine pas."), *P.Id));
			}

			if (P.CellulePorteuse == Cell) { ++Out.ParcellesDistinctesPortees; }
			Out.Parcelles.Add(MoveTemp(P));
		}
	}

	// --- COMMENT : les interfaces et leur resolution du catalogue ferme.
	{
		TSharedPtr<FJsonObject> Root;
		if (!ChargeSideCar(PlanVilleNoms::FamilleInterfaces, Cell, Root, Rapport)) { return false; }
		const FString Nom = NomSideCar(PlanVilleNoms::FamilleInterfaces, Cell);

		// LE CATALOGUE SORT DES DONNEES : la liste des resolutions licites est
		// celle que le side-car publie, jamais une constante du moteur.
		TSet<FName> Catalogue;
		const TArray<TSharedPtr<FJsonValue>>* Cat = nullptr;
		if (Root->TryGetArrayField(TEXT("catalogue"), Cat))
		{
			for (const TSharedPtr<FJsonValue>& V : *Cat)
			{
				const FName N(*V->AsString());
				Catalogue.Add(N);
				Out.Catalogue.Add(N);
			}
		}
		if (Catalogue.Num() == 0)
		{
			Rapport.Ajoute(Nom, TEXT("Champ `catalogue` absent ou vide : sans catalogue, ")
				TEXT("aucune resolution n'est verifiable."));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("interfaces"), Arr))
		{
			Rapport.Ajoute(Nom, TEXT("Champ `interfaces` absent."));
			return false;
		}
		Out.Interfaces.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) { continue; }
			FPlanInterface I;
			O->TryGetStringField(TEXT("a"), I.A);
			O->TryGetStringField(TEXT("b"), I.B);
			FString R;
			O->TryGetStringField(TEXT("resolution"), R);
			I.Resolution = FName(*R);
			if (!Catalogue.Contains(I.Resolution))
			{
				Rapport.Ajoute(Nom, FString::Printf(
					TEXT("Interface %s|%s : resolution '%s' HORS du catalogue publie."),
					*I.A, *I.B, *R));
			}
			I.bArbitrageDemande = (I.Resolution == PlanVilleNoms::ArbitrageDemande);
			O->TryGetNumberField(TEXT("dz_m"), I.DzM);
			O->TryGetNumberField(TEXT("dz_max_m"), I.DzMaxM);
			I.bHauteurPubliee = O->TryGetNumberField(TEXT("h_m"), I.HM);
			O->TryGetNumberField(TEXT("longueur_m"), I.LongueurM);
			O->TryGetNumberField(TEXT("longueur_totale_m"), I.LongueurTotaleM);
			O->TryGetStringField(TEXT("motif"), I.Motif);
			const TArray<TSharedPtr<FJsonValue>>* Mats = nullptr;
			if (O->TryGetArrayField(TEXT("matieres"), Mats) && Mats->Num() >= 2)
			{
				I.MatiereA = MatiereDe((*Mats)[0]->AsString());
				I.MatiereB = MatiereDe((*Mats)[1]->AsString());
			}
			LitLignes(O, TEXT("polylignes"), I.Polylignes);
			if (I.Polylignes.Num() == 0)
			{
				Rapport.Ajoute(Nom, FString::Printf(
					TEXT("Interface %s|%s sans polyligne : elle ne designe aucun contact."),
					*I.A, *I.B));
			}
			if (I.bArbitrageDemande) { ++Out.ArbitragesN; }
			Out.Interfaces.Add(MoveTemp(I));
		}
	}

	// --- LE SEMIS : les instances retenues par le plan.
	{
		TSharedPtr<FJsonObject> Root;
		if (!ChargeSideCar(PlanVilleNoms::FamilleSemis, Cell, Root, Rapport)) { return false; }
		const FString Nom = NomSideCar(PlanVilleNoms::FamilleSemis, Cell);

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("instances"), Arr))
		{
			Rapport.Ajoute(Nom, TEXT("Champ `instances` absent."));
			return false;
		}
		Out.Semis.Reserve(Arr->Num());
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			if (!O.IsValid()) { continue; }
			FPlanInstance N;
			FString S;
			if (O->TryGetStringField(TEXT("kind"), S)) { N.Kind = FName(*S); }
			O->TryGetStringField(TEXT("mesh"), N.Mesh);
			O->TryGetNumberField(TEXT("x"), N.X);
			O->TryGetNumberField(TEXT("y"), N.Y);
			O->TryGetNumberField(TEXT("yaw"), N.YawDeg);
			O->TryGetNumberField(TEXT("scale"), N.Echelle);
			Out.Semis.Add(MoveTemp(N));
		}
	}

	// --- Les comptes de la cellule doivent etre EXACTEMENT ceux de l'index.
	if (Out.Parcelles.Num() != Attendus.Parcelles)
	{
		Rapport.Ajoute(NomSideCar(PlanVilleNoms::FamilleQui, Cell), FString::Printf(
			TEXT("COMPTE FAUX : %d parcelles lues, %d declarees par l'index."),
			Out.Parcelles.Num(), Attendus.Parcelles));
	}
	if (Out.Interfaces.Num() != Attendus.Interfaces)
	{
		Rapport.Ajoute(NomSideCar(PlanVilleNoms::FamilleInterfaces, Cell), FString::Printf(
			TEXT("COMPTE FAUX : %d interfaces lues, %d declarees par l'index."),
			Out.Interfaces.Num(), Attendus.Interfaces));
	}
	if (Out.Semis.Num() != Attendus.Instances)
	{
		Rapport.Ajoute(NomSideCar(PlanVilleNoms::FamilleSemis, Cell), FString::Printf(
			TEXT("COMPTE FAUX : %d instances lues, %d declarees par l'index."),
			Out.Semis.Num(), Attendus.Instances));
	}

	// --- SECTEUR NON CONSTRUCTIBLE. Une seule interface en attente d'arbitrage
	//     suffit : E2-1 n'y batit pas (S12, arbitrage differe par l'utilisateur).
	Out.bConstructible = (Out.ArbitragesN == 0);
	if (!Out.bConstructible)
	{
		Rapport.CellulesRefusees.AddUnique(Cell);
		UE_LOG(LogPlanVille, Warning,
			TEXT("PLAN : cellule (%d, %d) REFUSEE A LA CONSTRUCTION — %d interface(s) "
				 "`arbitrage_demande` en attente de l'arbitrage utilisateur."),
			Cell.X, Cell.Y, Out.ArbitragesN);
	}

	Rapport.ParcellesPieces += Out.Parcelles.Num();
	Rapport.InterfacesPieces += Out.Interfaces.Num();
	Rapport.Instances += Out.Semis.Num();
	Rapport.Secondes = FPlatformTime::Seconds() - T0;

	return Rapport.Defauts.Num() == DefautsAvant;
}

// =============================================================================
// ValiderTout() — LA GARDE DE BUILD.
// =============================================================================

bool FPlanVille::ValiderTout(FPlanRapport& Rapport)
{
	const double T0 = FPlatformTime::Seconds();

	if (!Idx.bCharge)
	{
		Rapport.Ajoute(TEXT(""), TEXT("ValiderTout() sans index charge : Ouvrir() d'abord."));
		return false;
	}

	// --- Les couches de tete du manifeste font partie de la completude du plan.
	{
		FString TexteManifeste;
		++NbFichiersOuverts;
		if (FFileHelper::LoadFileToString(TexteManifeste,
			*FPaths::Combine(Racine, PlanVilleNoms::Manifeste)))
		{
			TSharedPtr<FJsonObject> Man;
			const TSharedPtr<FJsonObject>* Couches = nullptr;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TexteManifeste), Man)
				&& Man.IsValid()
				&& Man->TryGetObjectField(TEXT("couches"), Couches) && Couches->IsValid())
			{
				for (const auto& KV : (*Couches)->Values)
				{
					const TSharedPtr<FJsonObject>* E = nullptr;
					if (!KV.Value.IsValid() || !KV.Value->TryGetObject(E) || !E->IsValid()) { continue; }
					FPlanIndex::FEmpreinte Emp;
					(*E)->TryGetStringField(TEXT("md5_octets"), Emp.Md5Octets);
					(*E)->TryGetStringField(TEXT("md5_logique"), Emp.Md5Logique);
					double O = 0.0;
					(*E)->TryGetNumberField(TEXT("octets"), O);
					Emp.Octets = (int64)O;
					FString Ignore;
					const FString NomCouche(*KV.Key);
					LireEtVerifier(FPaths::Combine(Racine, NomCouche), NomCouche, Emp, Ignore, Rapport);
				}
			}
		}
	}

	// --- Les 147 side-cars, cellule par cellule.
	//
	// RECONSTITUTION DES COMPTES DISTINCTS — la regle vient du contrat, pas
	// d'une hypothese :
	//   * parcelle : elle n'est comptee que dans sa `cellule_porteuse`
	//     (`convention_decoupe.champs` de l'index le dit mot pour mot) ; le
	//     compte des ids DISTINCTS sert de second temoin et doit tomber pareil ;
	//   * interface : le plan ne publie PAS de cellule porteuse pour les
	//     frontieres — l'identite est le COUPLE (a, b), et sa reconstitution est
	//     donc GLOBALE. C'est la raison pour laquelle elle n'a lieu qu'ici, et
	//     jamais dans le chemin district-first ;
	//   * instance : le semis n'est pas decoupe, la somme suffit.
	int32 PiecesParcelles = 0, PiecesInterfaces = 0, TotalInstances = 0;
	int32 ParcellesPortees = 0;
	TSet<FString> IdsParcelles;
	TSet<FString> CouplesInterfaces;
	IdsParcelles.Reserve(Idx.ParcellesDistinctes > 0 ? Idx.ParcellesDistinctes : 1024);
	CouplesInterfaces.Reserve(Idx.InterfacesDistinctes > 0 ? Idx.InterfacesDistinctes : 1024);

	TArray<FIntPoint> Cellules = Idx.Cellules;
	Cellules.Sort([](const FIntPoint& A, const FIntPoint& B)
	{
		return (A.X != B.X) ? (A.X < B.X) : (A.Y < B.Y);
	});

	for (const FIntPoint& Cell : Cellules)
	{
		const FPlanIndex::FComptesCellule* Attendus = Idx.ParCellule.Find(Cell);
		const FString TexteCell = TexteDeCellule(Cell);

		// QUI
		{
			TSharedPtr<FJsonObject> Root;
			if (ChargeSideCar(PlanVilleNoms::FamilleQui, Cell, Root, Rapport))
			{
				const FString Nom = NomSideCar(PlanVilleNoms::FamilleQui, Cell);
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				if (Root->TryGetArrayField(TEXT("parcelles"), Arr))
				{
					PiecesParcelles += Arr->Num();
					if (Attendus && Arr->Num() != Attendus->Parcelles)
					{
						Rapport.Ajoute(Nom, FString::Printf(
							TEXT("COMPTE FAUX : %d parcelles, %d declarees par l'index."),
							Arr->Num(), Attendus->Parcelles));
					}
					for (const TSharedPtr<FJsonValue>& V : *Arr)
					{
						const TSharedPtr<FJsonObject>& O = V->AsObject();
						if (!O.IsValid()) { continue; }
						FString Porteuse, Id;
						O->TryGetStringField(TEXT("id"), Id);
						if (O->TryGetStringField(TEXT("cellule_porteuse"), Porteuse)
							&& Porteuse == TexteCell)
						{
							++ParcellesPortees;
							IdsParcelles.Add(Id);
						}
					}
				}
				else
				{
					Rapport.Ajoute(Nom, TEXT("Champ `parcelles` absent."));
				}
			}
		}

		// COMMENT
		{
			TSharedPtr<FJsonObject> Root;
			if (ChargeSideCar(PlanVilleNoms::FamilleInterfaces, Cell, Root, Rapport))
			{
				const FString Nom = NomSideCar(PlanVilleNoms::FamilleInterfaces, Cell);
				TSet<FName> Catalogue;
				const TArray<TSharedPtr<FJsonValue>>* Cat = nullptr;
				if (Root->TryGetArrayField(TEXT("catalogue"), Cat))
				{
					for (const TSharedPtr<FJsonValue>& V : *Cat) { Catalogue.Add(FName(*V->AsString())); }
				}
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				if (Root->TryGetArrayField(TEXT("interfaces"), Arr))
				{
					PiecesInterfaces += Arr->Num();
					if (Attendus && Arr->Num() != Attendus->Interfaces)
					{
						Rapport.Ajoute(Nom, FString::Printf(
							TEXT("COMPTE FAUX : %d interfaces, %d declarees par l'index."),
							Arr->Num(), Attendus->Interfaces));
					}
					int32 Arbitrages = 0;
					for (const TSharedPtr<FJsonValue>& V : *Arr)
					{
						const TSharedPtr<FJsonObject>& O = V->AsObject();
						if (!O.IsValid()) { continue; }
						FString A, B, R;
						O->TryGetStringField(TEXT("a"), A);
						O->TryGetStringField(TEXT("b"), B);
						O->TryGetStringField(TEXT("resolution"), R);
						CouplesInterfaces.Add(A + TEXT("|") + B);
						const FName Res(*R);
						if (Catalogue.Num() > 0 && !Catalogue.Contains(Res))
						{
							Rapport.Ajoute(Nom, FString::Printf(
								TEXT("Interface %s|%s : resolution '%s' HORS catalogue."),
								*A, *B, *R));
						}
						if (Res == PlanVilleNoms::ArbitrageDemande) { ++Arbitrages; }
					}
					if (Arbitrages > 0)
					{
						Rapport.CellulesRefusees.AddUnique(Cell);
						UE_LOG(LogPlanVille, Warning,
							TEXT("PLAN : cellule (%d, %d) REFUSEE A LA CONSTRUCTION — %d "
								 "interface(s) `arbitrage_demande`."),
							Cell.X, Cell.Y, Arbitrages);
					}
				}
				else
				{
					Rapport.Ajoute(Nom, TEXT("Champ `interfaces` absent."));
				}
			}
		}

		// LE SEMIS
		{
			TSharedPtr<FJsonObject> Root;
			if (ChargeSideCar(PlanVilleNoms::FamilleSemis, Cell, Root, Rapport))
			{
				const FString Nom = NomSideCar(PlanVilleNoms::FamilleSemis, Cell);
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				if (Root->TryGetArrayField(TEXT("instances"), Arr))
				{
					TotalInstances += Arr->Num();
					if (Attendus && Arr->Num() != Attendus->Instances)
					{
						Rapport.Ajoute(Nom, FString::Printf(
							TEXT("COMPTE FAUX : %d instances, %d declarees par l'index."),
							Arr->Num(), Attendus->Instances));
					}
				}
				else
				{
					Rapport.Ajoute(Nom, TEXT("Champ `instances` absent."));
				}
			}
		}
	}

	Rapport.ParcellesPieces = PiecesParcelles;
	Rapport.InterfacesPieces = PiecesInterfaces;
	Rapport.Instances = TotalInstances;
	Rapport.ParcellesDistinctes = ParcellesPortees;
	Rapport.InterfacesDistinctes = CouplesInterfaces.Num();

	// --- La confrontation au manifeste. C'est ICI que le plan passe ou casse.
	auto Confronte = [&](const TCHAR* Quoi, int32 Reconstitue, int32 AuManifeste)
	{
		if (Reconstitue != AuManifeste)
		{
			Rapport.Ajoute(TEXT(""), FString::Printf(
				TEXT("COMPTE RECONSTITUE != MANIFESTE sur `%s` : reconstitue %d, manifeste %d."),
				Quoi, Reconstitue, AuManifeste));
		}
	};
	Confronte(TEXT("parcelles_pieces"), PiecesParcelles, Idx.ParcellesPieces);
	Confronte(TEXT("interfaces_pieces"), PiecesInterfaces, Idx.InterfacesPieces);
	Confronte(TEXT("instances"), TotalInstances, Idx.Instances);
	Confronte(TEXT("parcelles_distinctes"), ParcellesPortees, Idx.ParcellesDistinctes);
	Confronte(TEXT("interfaces_distinctes"), CouplesInterfaces.Num(), Idx.InterfacesDistinctes);

	// Le second temoin des parcelles : les ids DISTINCTS portes doivent etre
	// aussi nombreux que les pieces porteuses (sinon une cellule porte deux fois
	// la meme parcelle, et le compte « distinct » ne veut plus rien dire).
	if (IdsParcelles.Num() != ParcellesPortees)
	{
		Rapport.Ajoute(TEXT(""), FString::Printf(
			TEXT("RECONSTITUTION INCOHERENTE : %d pieces porteuses pour %d ids distincts "
				 "— une parcelle est portee deux fois."),
			ParcellesPortees, IdsParcelles.Num()));
	}

	Rapport.Secondes = FPlatformTime::Seconds() - T0;

	if (!Rapport.EstValide())
	{
		UE_LOG(LogPlanVille, Error, TEXT("%s"), *Rapport.Texte());
		return false;
	}

	FString Refusees;
	for (const FIntPoint& C : Rapport.CellulesRefusees)
	{
		Refusees += FString::Printf(TEXT("%s(%d, %d)"), Refusees.IsEmpty() ? TEXT("") : TEXT(", "), C.X, C.Y);
	}
	UE_LOG(LogPlanVille, Display, TEXT("%s Cellules refusees : %s"),
		*Rapport.Texte(), Refusees.IsEmpty() ? TEXT("aucune") : *Refusees);
	return true;
}
