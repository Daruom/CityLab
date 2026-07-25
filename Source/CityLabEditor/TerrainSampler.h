#pragma once

#include "CoreMinimal.h"

/**
 * Echantillonneur du MNT (RGE ALTI 1 m) charge depuis un PNG 16 bits gris dont la
 * valeur est l'altitude NGF en CENTIMETRES, plus un JSON de georeferencement
 * (SourceData/toulouse10_mnt.json) : taille de grille, taille de pixel et coin NW
 * en cm Unreal. Ligne 0 = nord, les lignes croissent vers le sud (+Y), centres de
 * pixels a (col+0,5 ; row+0,5). Brique commune du profil desktop (J2) : sol, routes,
 * surfaces, batiments et arbres echantillonnent tous le meme sampler.
 */
class FTerrainSampler
{
public:
	/**
	 * Charge le PNG 16 bits via IImageWrapper et le JSON de georeferencement.
	 * Toutes les constantes (dimensions, taille pixel, coin NW) sont lues du JSON ;
	 * les dimensions du PNG doivent les recouper. Logge les erreurs et rend false.
	 */
	bool Load(const FString& PngPath, const FString& JsonPath);

	/** true une fois Load() reussi. */
	bool IsLoaded() const { return Heights.Num() > 0; }

	/**
	 * Altitude NGF en cm a la position Unreal (Xcm, Ycm), interpolation bilineaire
	 * entre les 4 centres de pixels voisins, clampee aux bords de la dalle.
	 */
	float AltCmAt(double Xcm, double Ycm) const;

	/**
	 * Altitude minimale (cm) sous un polygone en cm Unreal : echantillonne les
	 * sommets ET l'interieur (grille au pas du MNT ~1 m, bornee a la bbox, points
	 * retenus par point-dans-polygone). Sert a poser les batiments (ZBase).
	 */
	float MinAltCmInPolygon(const TArray<FVector2D>& PolyCm) const;

	/** Altitude maximale (cm) sous un polygone, meme echantillonnage que le min. */
	float MaxAltCmInPolygon(const TArray<FVector2D>& PolyCm) const;

	/**
	 * Altitude (cm) au percentile donne (0..1) des echantillons sous un polygone,
	 * memes echantillons que Min/Max (sommets + grille interieure au pas du MNT).
	 * Sert a poser l'eau plane au quantile bas (p10) de son emprise (J2 §3.4).
	 */
	float PercentileAltCmInPolygon(const TArray<FVector2D>& PolyCm, float Percentile) const;

	/** Altitude (cm) au point Unreal (0,0) — le Capitole — cachee au Load. Sert de rebase Z=0. */
	float AltCapitoleCm() const { return CachedAltCapitoleCm; }

	/** Altitude minimale (cm) de toute la dalle, calculee au Load. */
	float MinAltCm() const { return CachedMinAltCm; }

	/** Altitude maximale (cm) de toute la dalle, calculee au Load. */
	float MaxAltCm() const { return CachedMaxAltCm; }

private:
	/** Parcours commun min/max : sommets + grille interieure du polygone. */
	float MinMaxAltCmInPolygon(const TArray<FVector2D>& PolyCm, bool bWantMax) const;

	/** Echantillonnage commun min/max/percentile : sommets + grille interieure. */
	void CollectAltCmInPolygon(const TArray<FVector2D>& PolyCm, TArray<float>& OutAltCm) const;

	/** Altitudes NGF en cm, row-major, ligne 0 = nord (une valeur PNG = 1 cm). */
	TArray64<uint16> Heights;

	/** Dimensions de la grille, lues du JSON et recoupees avec le PNG. */
	int32 Width = 0;
	int32 Height = 0;

	/** Taille d'un pixel en cm Unreal (pixel_size_m * 100). */
	double PixelSizeCm = 100.0;

	/** Coin NW du pixel (0,0) en cm Unreal (coin_nw_unreal_cm). */
	double OriginXcm = 0.0;
	double OriginYcm = 0.0;

	float CachedAltCapitoleCm = 0.f;
	float CachedMinAltCm = 0.f;
	float CachedMaxAltCm = 0.f;
};
