#include "CityImportTools.h"

#include "Algo/Reverse.h"
#include "TerrainSampler.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EditorLevelUtils.h"
#include "Engine/LevelStreamingDynamic.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Components/TextRenderComponent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodySetup.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogCityImport, Log, All);

namespace
{
	void RaiseError(const FString& Message)
	{
		UE_LOG(LogCityImport, Error, TEXT("%s"), *Message);
		UKismetSystemLibrary::RaiseScriptError(Message);
	}

	// Sampler MNT partage : la dalle 10k x 10k pese ~200 Mo decompressee, elle se
	// charge UNE fois et reste en cache pour tout l'import (et les suivants).
	// Cache un emplacement : rechargee seulement si les chemins changent.
	FTerrainSampler* GetTerrainSampler(const FString& PngPath, const FString& JsonPath)
	{
		static FTerrainSampler Sampler;
		static FString LoadedKey;
		const FString Key = PngPath + TEXT("|") + JsonPath;
		if (LoadedKey != Key)
		{
			LoadedKey.Empty();
			if (!Sampler.Load(PngPath, JsonPath))
			{
				return nullptr;
			}
			LoadedKey = Key;
		}
		return &Sampler;
	}

	// Contexte de drapage resolu en debut d'import : sampler charge une fois +
	// altitude de rebase (Capitole -> z=0). Sampler nul = profil plat (mobile),
	// GroundZ rend alors exactement 0 (golden path bit-a-bit).
	struct FDrapeContext
	{
		const FTerrainSampler* Sampler = nullptr;
		float AltCapCm = 0.f;

		bool IsActive() const { return Sampler != nullptr; }

		float GroundZ(double Xcm, double Ycm) const
		{
			return Sampler ? Sampler->AltCmAt(Xcm, Ycm) - AltCapCm : 0.f;
		}
	};

	// Charge le MNT si le profil le demande. Rend false (apres RaiseError) si le
	// MNT est introuvable — un import desktop sans relief serait un faux resultat.
	bool MakeDrapeContext(const FCityGenProfile& Profile, FDrapeContext& Out)
	{
		if (!Profile.bDrapeToTerrain)
		{
			return true;
		}
		const FString Png = Profile.TerrainPngPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.png"))
			: Profile.TerrainPngPath;
		const FString Jsn = Profile.TerrainJsonPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.json"))
			: Profile.TerrainJsonPath;
		FTerrainSampler* Sampler = GetTerrainSampler(Png, Jsn);
		if (!Sampler)
		{
			RaiseError(FString::Printf(TEXT("Cannot load MNT ('%s' + '%s')."), *Png, *Jsn));
			return false;
		}
		Out.Sampler = Sampler;
		Out.AltCapCm = Sampler->AltCapitoleCm();
		return true;
	}

	// Re-echantillonne une polyligne a pas fixe : chaque segment plus long que
	// StepCm est subdivise, sommets d'origine conserves — sans cela les longs
	// segments droits OSM traversent les bosses du MNT (spec J2 §3.2).
	TArray<FVector2D> ResamplePolyline(const TArray<FVector2D>& Pts, float StepCm)
	{
		TArray<FVector2D> Out;
		Out.Reserve(Pts.Num());
		for (int32 i = 0; i < Pts.Num(); ++i)
		{
			if (i > 0)
			{
				const FVector2D& A = Pts[i - 1];
				const FVector2D& B = Pts[i];
				const int32 Sub = FMath::CeilToInt32((float)(B - A).Size() / StepCm);
				for (int32 s = 1; s < Sub; ++s)
				{
					Out.Add(FMath::Lerp(A, B, (double)s / (double)Sub));
				}
			}
			Out.Add(Pts[i]);
		}
		return Out;
	}

	// Z terrain par sommet d'une polyligne : drape sur le MNT, ou — pont — Z
	// interpole lineairement entre les deux culees par abscisse curviligne (le MNT
	// est un sol nu : une route drapee plongerait dans la Garonne, spec J2 Q5).
	void ComputePolylineZ(const TArray<FVector2D>& Pts, const FDrapeContext& Drape,
		bool bBridge, TArray<float>& OutZ)
	{
		OutZ.SetNum(Pts.Num());
		if (!bBridge)
		{
			for (int32 i = 0; i < Pts.Num(); ++i)
			{
				OutZ[i] = Drape.GroundZ(Pts[i].X, Pts[i].Y);
			}
			return;
		}
		const float Z0 = Drape.GroundZ(Pts[0].X, Pts[0].Y);
		const float Z1 = Drape.GroundZ(Pts.Last().X, Pts.Last().Y);
		float Total = 0.f;
		for (int32 i = 0; i + 1 < Pts.Num(); ++i)
		{
			Total += (float)(Pts[i + 1] - Pts[i]).Size();
		}
		float Arc = 0.f;
		for (int32 i = 0; i < Pts.Num(); ++i)
		{
			if (i > 0)
			{
				Arc += (float)(Pts[i] - Pts[i - 1]).Size();
			}
			OutZ[i] = Total > 1.f ? FMath::Lerp(Z0, Z1, Arc / Total) : Z0;
		}
	}

	// Copie assumee du builder de BuildingTools.cpp, version couleur RGB + triangles.
	// A consolider avec BuildingTools une fois le pipeline ville valide.
	struct FCityMeshBuilder
	{
		FMeshDescription MeshDesc;
		FStaticMeshAttributes Attributes;
		FPolygonGroupID WallGroup;
		FPolygonGroupID GlassGroup;
		int32 QuadCount = 0;

		// Lot B (desktop PBR) : vertex colors encodees LINEAIRE (le pow 2.2 est un
		// hack de lecture brute en unlit — faux pour un materiau lit, spec Q10).
		bool bLinearColors = false;

		// Lot B (desktop) : UV1 = (x,y) monde normalise sur la dalle 10 km
		// (coin NW a -5000 m -> [0,1]), prete pour l'ortho BD ORTHO (J3).
		bool bWorldUV1 = false;

		FCityMeshBuilder()
			: Attributes(MeshDesc)
		{
			Attributes.Register();
			WallGroup = MeshDesc.CreatePolygonGroup();
			GlassGroup = MeshDesc.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[WallGroup] = FName(TEXT("Wall"));
			Attributes.GetPolygonGroupMaterialSlotNames()[GlassGroup] = FName(TEXT("Glass"));
		}

		void EnableWorldUV1()
		{
			if (!bWorldUV1)
			{
				bWorldUV1 = true;
				MeshDesc.VertexInstanceAttributes().SetAttributeChannelCount(
					MeshAttribute::VertexInstance::TextureCoordinate, 2);
			}
		}

		static FVector2f WorldUV(const FVector3f& P)
		{
			return FVector2f((P.X + 500000.f) / 1000000.f, (P.Y + 500000.f) / 1000000.f);
		}

		// Compensation gamma : le build encode en sRGB, le shader lit brut (cf. BuildingTools).
		// bLinearColors (desktop PBR) : encodage lineaire, le shader lit VertexColor en lineaire.
		FVector4f Encode(const FVector3f& C) const
		{
			if (bLinearColors)
			{
				return FVector4f(FMath::Clamp(C.X, 0.f, 1.f), FMath::Clamp(C.Y, 0.f, 1.f),
					FMath::Clamp(C.Z, 0.f, 1.f), 1.f);
			}
			return FVector4f(FMath::Pow(FMath::Clamp(C.X, 0.f, 1.f), 2.2f),
				FMath::Pow(FMath::Clamp(C.Y, 0.f, 1.f), 2.2f),
				FMath::Pow(FMath::Clamp(C.Z, 0.f, 1.f), 2.2f), 1.f);
		}

		// Variante avec UNE couleur PAR SOMMET (grille de sol peinte).
		void AddPolyPerVertexColors(FPolygonGroupID Group, const FVector3f* Corners, int32 Num,
			const FVector3f& Outward, const FVector2f* UVs, const FVector3f* VertexColors)
		{
			const FVector3f RenderedNormal =
				FVector3f::CrossProduct(Corners[2] - Corners[0], Corners[1] - Corners[0]).GetSafeNormal();
			const bool bFlip = FVector3f::DotProduct(RenderedNormal, Outward) < 0.f;
			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector2f> UV0 = Attributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
			TArray<FVertexInstanceID> InstanceIDs;
			InstanceIDs.Reserve(Num);
			for (int32 i = 0; i < Num; ++i)
			{
				const int32 c = bFlip ? (Num - 1 - i) % Num : i;
				const FVertexID VertexID = MeshDesc.CreateVertex();
				Positions[VertexID] = Corners[c];
				const FVertexInstanceID InstanceID = MeshDesc.CreateVertexInstance(VertexID);
				Normals[InstanceID] = Outward;
				UV0.Set(InstanceID, 0, UVs[c]);
				if (bWorldUV1)
				{
					UV0.Set(InstanceID, 1, WorldUV(Corners[c]));
				}
				Colors[InstanceID] = Encode(VertexColors[c]);
				InstanceIDs.Add(InstanceID);
			}
			MeshDesc.CreatePolygon(Group, InstanceIDs);
			++QuadCount;
		}

		void AddPoly(FPolygonGroupID Group, const FVector3f* Corners, int32 Num, const FVector3f& Outward,
			const FVector2f* UVs, const FVector3f& Color)
		{
			const FVector3f RenderedNormal =
				FVector3f::CrossProduct(Corners[2] - Corners[0], Corners[1] - Corners[0]).GetSafeNormal();
			const bool bFlip = FVector3f::DotProduct(RenderedNormal, Outward) < 0.f;

			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector2f> UV0 = Attributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

			const FVector4f Encoded = Encode(Color);
			TArray<FVertexInstanceID> InstanceIDs;
			InstanceIDs.Reserve(Num);
			for (int32 i = 0; i < Num; ++i)
			{
				const int32 c = bFlip ? (Num - 1 - i) % Num : i;
				const FVertexID VertexID = MeshDesc.CreateVertex();
				Positions[VertexID] = Corners[c];
				const FVertexInstanceID InstanceID = MeshDesc.CreateVertexInstance(VertexID);
				Normals[InstanceID] = Outward;
				UV0.Set(InstanceID, 0, UVs[c]);
				if (bWorldUV1)
				{
					UV0.Set(InstanceID, 1, WorldUV(Corners[c]));
				}
				Colors[InstanceID] = Encoded;
				InstanceIDs.Add(InstanceID);
			}
			MeshDesc.CreatePolygon(Group, InstanceIDs);
			++QuadCount;
		}

		void AddQuad(FPolygonGroupID Group, const FVector3f& A, const FVector3f& B, const FVector3f& C,
			const FVector3f& D, const FVector3f& Outward, const FVector3f& Color, float UVScale = 0.0025f)
		{
			if ((A - B).IsNearlyZero(0.01f) || (B - C).IsNearlyZero(0.01f) || (C - D).IsNearlyZero(0.01f))
			{
				return;
			}
			const FVector3f P[4] = { A, B, C, D };
			FVector2f UV[4];
			for (int32 i = 0; i < 4; ++i)
			{
				// Projection planaire selon l'axe dominant de la normale.
				UV[i] = FMath::Abs(Outward.Z) > 0.5f
					? FVector2f(P[i].X * UVScale, P[i].Y * UVScale)
					: (FMath::Abs(Outward.X) > FMath::Abs(Outward.Y)
						? FVector2f(P[i].Y * UVScale, P[i].Z * UVScale)
						: FVector2f(P[i].X * UVScale, P[i].Z * UVScale));
			}
			AddPoly(Group, P, 4, Outward, UV, Color);
		}

		void AddTri(FPolygonGroupID Group, const FVector3f& A, const FVector3f& B, const FVector3f& C,
			const FVector3f& Outward, const FVector3f& Color)
		{
			if ((A - B).IsNearlyZero(0.01f) || (B - C).IsNearlyZero(0.01f) || (C - A).IsNearlyZero(0.01f))
			{
				return;
			}
			const FVector3f P[3] = { A, B, C };
			FVector2f UV[3];
			for (int32 i = 0; i < 3; ++i)
			{
				UV[i] = FVector2f(P[i].X * 0.0025f, P[i].Y * 0.0025f);
			}
			AddPoly(Group, P, 3, Outward, UV, Color);
		}
	};

	// Soleil fixe monde pour l'ombrage cuit (memes valeurs que le bench batiment heros).
	const FVector3f GSunDir = FVector3f(0.4f, -0.6f, 0.7f).GetSafeNormal();

	FVector3f Shade(const FVector3f& Base, const FVector3f& N, float Zcm)
	{
		const float L = 0.30f + 0.70f * FMath::Max(0.f, FVector3f::DotProduct(N, GSunDir));
		const float Ground = FMath::Lerp(0.85f, 1.f, FMath::Clamp(Zcm / 200.f, 0.f, 1.f));
		return Base * L * Ground;
	}

	double SignedArea(const TArray<FVector2D>& P)
	{
		double A = 0;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FVector2D& U = P[i];
			const FVector2D& V = P[(i + 1) % P.Num()];
			A += U.X * V.Y - V.X * U.Y;
		}
		return A * 0.5;
	}

	// Ear clipping simple sur un anneau CCW. Fallback en eventail si degenere.
	void TriangulateRing(const TArray<FVector2D>& P, TArray<int32>& OutTris)
	{
		auto Cross = [](const FVector2D& a, const FVector2D& b, const FVector2D& c)
		{
			return (b.X - a.X) * (c.Y - a.Y) - (b.Y - a.Y) * (c.X - a.X);
		};
		auto InTri = [&](const FVector2D& p, const FVector2D& a, const FVector2D& b, const FVector2D& c)
		{
			return Cross(a, b, p) >= 0 && Cross(b, c, p) >= 0 && Cross(c, a, p) >= 0;
		};
		TArray<int32> V;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			V.Add(i);
		}
		int32 Guard = P.Num() * P.Num() * 2;
		while (V.Num() > 3 && Guard-- > 0)
		{
			bool bClipped = false;
			for (int32 i = 0; i < V.Num(); ++i)
			{
				const int32 i0 = V[(i + V.Num() - 1) % V.Num()];
				const int32 i1 = V[i];
				const int32 i2 = V[(i + 1) % V.Num()];
				if (Cross(P[i0], P[i1], P[i2]) <= 0)
				{
					continue;
				}
				bool bEar = true;
				for (int32 j = 0; j < V.Num(); ++j)
				{
					const int32 vj = V[j];
					if (vj == i0 || vj == i1 || vj == i2)
					{
						continue;
					}
					if (InTri(P[vj], P[i0], P[i1], P[i2]))
					{
						bEar = false;
						break;
					}
				}
				if (!bEar)
				{
					continue;
				}
				OutTris.Append({ i0, i1, i2 });
				V.RemoveAt(i);
				bClipped = true;
				break;
			}
			if (!bClipped)
			{
				break;
			}
		}
		if (V.Num() == 3)
		{
			OutTris.Append({ V[0], V[1], V[2] });
		}
		else if (V.Num() > 3)
		{
			for (int32 i = 1; i + 1 < V.Num(); ++i)
			{
				OutTris.Append({ V[0], V[i], V[i + 1] });
			}
		}
	}

	FVector3f UsageTint(const FString& Usage, int32 Seed)
	{
		FVector3f Base;
		if (Usage == TEXT("res")) { Base = FVector3f(0.80f, 0.55f, 0.40f); }        // brique toulousaine
		else if (Usage == TEXT("com")) { Base = FVector3f(0.82f, 0.75f, 0.62f); }   // enduit clair
		else if (Usage == TEXT("ind")) { Base = FVector3f(0.60f, 0.60f, 0.62f); }
		else { Base = FVector3f(0.74f, 0.66f, 0.55f); }
		const float Var = 0.90f + 0.20f * FMath::Frac(FMath::Sin(Seed * 12.9898f) * 43758.5453f);
		return Base * Var;
	}

	// Batiment a emprise polygonale : facades avec fenetres par travee + toit plat.
	void BuildPolygonBuilding(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float Hcm,
		const FVector3f& Tint)
	{
		const int32 Floors = FMath::Clamp(FMath::RoundToInt32(Hcm / 290.f), 1, 40);
		const float FloorH = Hcm / Floors;
		const FVector3f RoofColor = FVector3f(0.42f, 0.40f, 0.38f);

		const int32 N = PtsCm.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = PtsCm[e];
			const FVector2D B2 = PtsCm[(e + 1) % N];
			const FVector2D Dir2 = (B2 - A2);
			const float Len = Dir2.Size();
			if (Len < 30.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			// Polygone CCW : l'exterieur est a droite du parcours.
			const FVector3f Nout = FVector3f(T2.Y, -T2.X, 0.f);
			const FVector3f A = FVector3f(A2.X, A2.Y, 0.f);
			const FVector3f T = FVector3f(T2.X, T2.Y, 0.f);
			auto WallPoint = [&](float U, float Z) { return A + T * U + FVector3f(0, 0, Z); };
			auto Wall = [&](float U0, float U1, float Z0, float Z1)
			{
				if (U1 - U0 < 1.f || Z1 - Z0 < 1.f)
				{
					return;
				}
				QM.AddQuad(QM.WallGroup, WallPoint(U0, Z0), WallPoint(U1, Z0), WallPoint(U1, Z1),
					WallPoint(U0, Z1), Nout, Shade(Tint, Nout, (Z0 + Z1) * 0.5f));
			};

			const float Margin = 40.f;
			const float Usable = Len - 2.f * Margin;
			const int32 Bays = Usable > 200.f ? FMath::Max(1, FMath::RoundToInt32(Usable / 280.f)) : 0;

			float Z0 = 0.f;
			for (int32 F = 0; F < Floors; ++F)
			{
				const float Z1 = Z0 + FloorH;
				if (Bays == 0 || FloorH < 220.f)
				{
					Wall(0.f, Len, Z0, Z1);
					Z0 = Z1;
					continue;
				}
				Wall(0.f, Margin, Z0, Z1);
				Wall(Len - Margin, Len, Z0, Z1);
				const float BayW = Usable / Bays;
				for (int32 B = 0; B < Bays; ++B)
				{
					const float BU0 = Margin + B * BayW;
					const float BU1 = BU0 + BayW;
					const float WinW = BayW * 0.5f;
					const float WinH = FloorH * (F == 0 ? 0.60f : 0.45f);
					const float WU0 = (BU0 + BU1 - WinW) * 0.5f;
					const float WU1 = WU0 + WinW;
					const float WZ0 = F == 0 ? Z0 + 25.f : Z0 + (FloorH - WinH) * 0.5f;
					const float WZ1 = WZ0 + WinH;
					Wall(BU0, WU0, Z0, Z1);
					Wall(WU1, BU1, Z0, Z1);
					Wall(WU0, WU1, Z0, WZ0);
					Wall(WU0, WU1, WZ1, Z1);
					// Fenetre en creux simplifiee : vitre en retrait, sans tableaux (cout).
					const FVector3f In = -Nout * 15.f;
					const FVector3f G0 = WallPoint(WU0, WZ0) + In, G1 = WallPoint(WU1, WZ0) + In;
					const FVector3f G2 = WallPoint(WU1, WZ1) + In, G3 = WallPoint(WU0, WZ1) + In;
					const FVector3f P[4] = { G0, G1, G2, G3 };
					const FVector2f UV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
					QM.AddPoly(QM.GlassGroup, P, 4, Nout, UV,
						Shade(FVector3f(0.35f, 0.35f, 0.35f), Nout, (WZ0 + WZ1) * 0.5f));
				}
				Z0 = Z1;
			}
		}

		// Toit plat par triangulation de l'emprise.
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		const FVector3f RoofShaded = Shade(RoofColor, FVector3f(0, 0, 1), Hcm);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			QM.AddTri(QM.WallGroup,
				FVector3f(PtsCm[Tris[t]].X, PtsCm[Tris[t]].Y, Hcm),
				FVector3f(PtsCm[Tris[t + 1]].X, PtsCm[Tris[t + 1]].Y, Hcm),
				FVector3f(PtsCm[Tris[t + 2]].X, PtsCm[Tris[t + 2]].Y, Hcm),
				FVector3f(0, 0, 1), RoofShaded);
		}
	}

	// Ruban de route TEXTURE : UN SEUL quad par segment (chaussee + trottoirs + bordures
	// + marquage peints dans T_RoadStrip, slot Glass des cellules de sol). L'ancienne
	// version geometrique (asphalte + 2 trottoirs + tirets, ~500-700 k tris de tirets
	// sub-pixel au-dela de 300 m) plafonnait le panorama a 45 fps. En prime : plus de
	// z-fight de marquage (aucune geometrie superposee). Les sentiers pietons restent
	// un ruban uni sur le slot Wall (ils n'avaient deja ni trottoir ni marquage).
	// Empilement decimetrique (Adreno/GLES sans reversed-Z) : dalle 0 < parc < eau <
	// rail < route 55+. TerrainZ (desktop) : Z terrain par sommet, l'empilement
	// devient RELATIF au terrain ; nul = plat historique (mobile).
	// bBakedShade=false (desktop PBR) : teinte brute, Lumen eclaire (plus de Shade cuit).
	void BuildRoad(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float WidthCm,
		const FString& Type, int32 RoadIndex, const TArray<float>* TerrainZ = nullptr,
		bool bBakedShade = true)
	{
		const int32 N = PtsCm.Num();
		if (N < 2)
		{
			return;
		}
		// Jitter x4 : a 0,8 cm de pas, deux routes qui se croisent scintillaient au-dela
		// de ~1 km (precision depth GLES). 4 cm de pas repousse le seuil vers 2 km,
		// au-dela duquel les rubans sont de toute facon culles (sol peint derriere).
		const float ZRoad = 55.f + (RoadIndex % 7) * 4.f;
		const bool bWalkway = Type == TEXT("footway") || Type == TEXT("path") || Type == TEXT("cycleway");
		const bool bMarking = !bWalkway && WidthCm >= 550.f;
		const bool bSolid = Type == TEXT("primary") || Type == TEXT("secondary");
		const float WalkW = bWalkway ? 0.f : 170.f;
		const float Half = WidthCm * 0.5f + WalkW;
		const FVector3f Up(0, 0, 1);
		// Teinte de base CLAIRE : les bandes de la texture assombrissent (asphalte 0,2x,
		// trottoir 0,59x, marquage 1x) pour retrouver les couleurs historiques.
		const FVector3f Base = bWalkway ? FVector3f(0.48f, 0.45f, 0.42f) : FVector3f(0.85f, 0.85f, 0.80f);
		const FVector3f Shaded = bBakedShade ? Shade(Base, Up, ZRoad) : Base;

		// Normales par sommet (moyenne des segments adjacents).
		TArray<FVector2D> Nrm;
		Nrm.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FVector2D D(0, 0);
			if (i > 0) { D += (PtsCm[i] - PtsCm[i - 1]).GetSafeNormal(); }
			if (i < N - 1) { D += (PtsCm[i + 1] - PtsCm[i]).GetSafeNormal(); }
			D = D.GetSafeNormal();
			Nrm[i] = FVector2D(-D.Y, D.X);
		}

		float Arc = 0.f;
		for (int32 i = 0; i + 1 < N; ++i)
		{
			const FVector2D A = PtsCm[i], B = PtsCm[i + 1];
			const FVector2D NA = Nrm[i] * Half, NB = Nrm[i + 1] * Half;
			const float SegLen = (B - A).Size();
			const float ZA = ZRoad + (TerrainZ ? (*TerrainZ)[i] : 0.f);
			const float ZB = ZRoad + (TerrainZ ? (*TerrainZ)[i + 1] : 0.f);
			const FVector3f P[4] = {
				FVector3f(A.X - NA.X, A.Y - NA.Y, ZA), FVector3f(B.X - NB.X, B.Y - NB.Y, ZB),
				FVector3f(B.X + NB.X, B.Y + NB.Y, ZB), FVector3f(A.X + NA.X, A.Y + NA.Y, ZA) };
			if (bWalkway)
			{
				QM.AddQuad(QM.WallGroup, P[0], P[1], P[2], P[3], Up, Shaded);
			}
			else
			{
				// V : tirets qui defilent avec l'abscisse ; ligne continue = V fixe en
				// zone peinte ; pas de marquage = V fixe en zone vide de la texture.
				float V0, V1;
				if (!bMarking) { V0 = V1 = 0.75f; }
				else if (bSolid) { V0 = V1 = 0.25f; }
				else { V0 = Arc / 600.f; V1 = (Arc + SegLen) / 600.f; }
				const FVector2f UV[4] = {
					FVector2f(0, V0), FVector2f(0, V1), FVector2f(1, V1), FVector2f(1, V0) };
				QM.AddPoly(QM.GlassGroup, P, 4, Up, UV, Shaded);
			}
			Arc += SegLen;
		}
	}

	// Arbre low-poly : tronc 6 faces + 2 etages de feuillage coniques.
	void BuildTree(FCityMeshBuilder& QM)
	{
		const FVector3f Trunk(0.32f, 0.22f, 0.14f);
		const FVector3f Leaf(0.28f, 0.45f, 0.22f);
		const int32 Sides = 6;
		auto Ring = [&](float R, float Z, int32 i) {
			const float Ang = 2.f * PI * i / Sides;
			return FVector3f(R * FMath::Cos(Ang), R * FMath::Sin(Ang), Z);
		};
		for (int32 i = 0; i < Sides; ++i)
		{
			const FVector3f N = (Ring(1.f, 0.f, i) + Ring(1.f, 0.f, i + 1)).GetSafeNormal();
			// Tronc
			QM.AddQuad(QM.WallGroup, Ring(14.f, 0.f, i), Ring(14.f, 0.f, i + 1), Ring(11.f, 260.f, i + 1),
				Ring(11.f, 260.f, i), N, Trunk);
			// Feuillage : cone bas + cone haut
			QM.AddTri(QM.WallGroup, Ring(170.f, 220.f, i), Ring(170.f, 220.f, i + 1), FVector3f(0, 0, 460.f),
				(N + FVector3f(0, 0, 0.5f)).GetSafeNormal(), Shade(Leaf, (N + FVector3f(0, 0, 0.5f)).GetSafeNormal(), 300.f));
			QM.AddTri(QM.WallGroup, Ring(170.f, 220.f, i + 1), Ring(170.f, 220.f, i), FVector3f(0, 0, 160.f),
				FVector3f(0, 0, -1), Shade(Leaf * 0.6f, FVector3f(0, 0, -1), 200.f));
		}
	}

	UMaterialInterface* LoadMaterialOrDefault(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return UMaterial::GetDefaultMaterial(MD_Surface);
		}
		UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Path);
		if (!M)
		{
			RaiseError(FString::Printf(TEXT("Material '%s' not found."), *Path));
		}
		return M;
	}

	// bNanite (Lot B, desktop) : NaniteSettings.bEnabled sur les meshes generes —
	// murs, sol, routes, proxys ET vitres (J2e, 25/07 : le verre est OPAQUE ;
	// vitres non-Nanite devant murs Nanite = fenetres qui « flottent » aux
	// transitions LOD). Jamais mobile. Le flag est REPOSE a chaque generation :
	// une regeneration mobile par-dessus des assets desktop les remet a false
	// (golden path).
	UStaticMesh* CreateMeshAsset(const FString& AssetPath, FCityMeshBuilder& QM,
		UMaterialInterface* WallMat, UMaterialInterface* GlassMat, bool bWithCollision = true,
		bool bBoxCollision = false, float BoxTopCm = 0.f, UStaticMesh* ComplexCollisionMesh = nullptr,
		bool bNanite = false)
	{
		// bBoxCollision : une simple boite englobante remplace le trimesh — reserve aux
		// cellules de sol PLATES (dalles+routes), ou le trimesh coutait ~90 Mo de RAM
		// device pour un gain de precision negligeable (< 20 cm en Z).
		FBox SimpleBox(ForceInit);
		if (bWithCollision && bBoxCollision)
		{
			TVertexAttributesRef<FVector3f> Pos = FStaticMeshAttributes(QM.MeshDesc).GetVertexPositions();
			for (const FVertexID V : QM.MeshDesc.Vertices().GetElementIDs())
			{
				SimpleBox += FVector(Pos[V]);
			}
		}
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *(AssetPath + TEXT(".") + ObjectName), nullptr,
			LOAD_NoWarn | LOAD_Quiet);
		if (!Mesh)
		{
			UPackage* Package = CreatePackage(*AssetPath);
			Mesh = NewObject<UStaticMesh>(Package, *ObjectName, RF_Public | RF_Standalone);
			FAssetRegistryModule::AssetCreated(Mesh);
		}
		Mesh->Modify();
		Mesh->GetStaticMaterials().Empty();
		Mesh->GetStaticMaterials().Add(FStaticMaterial(WallMat, FName(TEXT("Wall"))));
		Mesh->GetStaticMaterials().Add(FStaticMaterial(GlassMat, FName(TEXT("Glass"))));
		Mesh->SetNumSourceModels(1);
		FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(0);
		SourceModel.BuildSettings.bRecomputeNormals = true;
		SourceModel.BuildSettings.bRecomputeTangents = true;
		SourceModel.BuildSettings.bGenerateLightmapUVs = false;
		SourceModel.BuildSettings.bRemoveDegenerates = true;
		Mesh->CreateMeshDescription(0, MoveTemp(QM.MeshDesc));
		Mesh->CommitMeshDescription(0);
		Mesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);
		Mesh->GetNaniteSettings().bEnabled = bNanite;
		// Collision : le maillage de rendu sert de collision (drone vs ville). Pas de
		// primitives simples generees ; les meshes sont statiques, cout memoire accepte.
		// bWithCollision=false (proxy lointain) : UseSimpleAsComplex sans primitive
		// simple = aucun trimesh cuit, zero data de collision dans l'APK.
		// ComplexCollisionMesh (sol desktop) : le trimesh cuit vient d'un mesh basse
		// resolution dedie (grille 16x16), le rendu garde sa pleine densite 64x64.
		// Attendre la fin de sa compilation async AVANT le build de ce mesh : le cook
		// physique lit sa SectionInfoMap depuis un worker (ensure StaticMesh.cpp:4790).
		Mesh->ComplexCollisionMesh = ComplexCollisionMesh;
		if (ComplexCollisionMesh)
		{
			FStaticMeshCompilingManager::Get().FinishCompilation(
				TArrayView<UStaticMesh* const>(&ComplexCollisionMesh, 1));
		}
		Mesh->CreateBodySetup();
		if (UBodySetup* Body = Mesh->GetBodySetup())
		{
			Body->CollisionTraceFlag = (bWithCollision && !bBoxCollision)
				? CTF_UseComplexAsSimple : CTF_UseSimpleAsComplex;
			// Regeneration en place : purger les boites de la generation precedente
			// (elles s'ACCUMULAIENT a chaque re-import — bug latent expose par le
			// test de non-regression du profil mobile).
			Body->AggGeom.BoxElems.Reset();
			if (bWithCollision && bBoxCollision && SimpleBox.IsValid)
			{
				// BoxTopCm > 0 : plafond de boite force (ex. dalles de sol dont la boite
				// doit englober les rubans de route poses au-dessus, ~55-80 cm).
				const float ZSize = BoxTopCm > 0.f
					? BoxTopCm : FMath::Max<float>(SimpleBox.GetSize().Z, 8.f);
				FKBoxElem BoxElem(SimpleBox.GetSize().X, SimpleBox.GetSize().Y, ZSize);
				BoxElem.Center = BoxTopCm > 0.f
					? FVector(SimpleBox.GetCenter().X, SimpleBox.GetCenter().Y, ZSize * 0.5)
					: SimpleBox.GetCenter();
				Body->AggGeom.BoxElems.Add(BoxElem);
			}
			Body->InvalidatePhysicsData();
		}
		Mesh->Build(false);
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
		return Mesh;
	}
}

FCityGenProfile FCityGenProfile::Desktop()
{
	FCityGenProfile Profile;
	Profile.bDesktop = true;
	Profile.GroundGridN = 64;
	Profile.GroundCollisionGridN = 16;
	Profile.RoadResampleStepCm = 1500.f;
	Profile.bDrapeToTerrain = true;
	Profile.bWindowReveals = true;
	Profile.bSplitWallGlass = true;
	Profile.bNanite = true;
	Profile.bPBRMaterials = true;
	return Profile;
}

FCityGenProfile FCityGenProfile::Resolved() const
{
	if (!bDesktop)
	{
		return *this;
	}
	// bDesktop seul (appel MCP minimal) : tout champ laisse a sa valeur mobile
	// bascule vers le prereglage desktop ; un champ renseigne est conserve.
	const FCityGenProfile Mobile;
	FCityGenProfile Out = *this;
	if (Out.GroundGridN == Mobile.GroundGridN) { Out.GroundGridN = 64; }
	if (Out.GroundCollisionGridN == Mobile.GroundCollisionGridN) { Out.GroundCollisionGridN = 16; }
	if (Out.RoadResampleStepCm == Mobile.RoadResampleStepCm) { Out.RoadResampleStepCm = 1500.f; }
	Out.bDrapeToTerrain = true;
	Out.bWindowReveals = true;
	Out.bSplitWallGlass = true;
	Out.bNanite = true;
	Out.bPBRMaterials = true;
	return Out;
}

FCityImportSummary UCityImportTools::ImportCityDistrict(const FString& JsonFilePath, const FString& AssetFolder,
	const FString& WallMaterialPath, const FString& GlassMaterialPath, float CellSizeM, FVector Location)
{
	FCityImportSummary Summary;

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read district file '%s'."), *JsonFilePath));
		return Summary;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("District file is not valid JSON."));
		return Summary;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder must be a package path such as /Game/City/Capitole."));
		return Summary;
	}
	if (CellSizeM < 20.f)
	{
		RaiseError(TEXT("CellSizeM must be at least 20."));
		return Summary;
	}

	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UMaterialInterface* GlassMat = LoadMaterialOrDefault(GlassMaterialPath);
	if (!WallMat || !GlassMat)
	{
		return Summary;
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		RaiseError(TEXT("No editor world is loaded."));
		return Summary;
	}

	// Idempotence : un re-import remplace la ville existante (memes labels d'acteurs).
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L.StartsWith(TEXT("SM_City_")) || L == TEXT("CityTrees"))
		{
			ToDestroy.Add(*It);
		}
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	const float Cell = CellSizeM * 100.f;
	TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> Cells;
	TMap<FIntPoint, FBox2D> CellBounds;
	auto GetCell = [&](const FVector2D& P) -> FCityMeshBuilder&
	{
		const FIntPoint Key(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell));
		TUniquePtr<FCityMeshBuilder>& B = Cells.FindOrAdd(Key);
		if (!B)
		{
			B = MakeUnique<FCityMeshBuilder>();
			CellBounds.Add(Key, FBox2D(FVector2D(Key.X * Cell, Key.Y * Cell),
				FVector2D((Key.X + 1) * Cell, (Key.Y + 1) * Cell)));
		}
		return *B;
	};
	auto ReadPts = [](const TArray<TSharedPtr<FJsonValue>>& In, TArray<FVector2D>& Out)
	{
		for (const TSharedPtr<FJsonValue>& V : In)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() >= 2)
			{
				Out.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
			}
		}
	};

	// --- Batiments ---
	const TArray<TSharedPtr<FJsonValue>>* BuildingsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("buildings"), BuildingsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *BuildingsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			FVector2D Centroid(0, 0);
			for (const FVector2D& P : Pts) { Centroid += P; }
			Centroid /= Pts.Num();
			const float Hcm = O->GetNumberField(TEXT("h")) * 100.f;
			BuildPolygonBuilding(GetCell(Centroid), Pts, Hcm,
				UsageTint(O->GetStringField(TEXT("u")), Index));
			++Summary.Buildings;
			++Index;
		}
	}

	// --- Routes ---
	const TArray<TSharedPtr<FJsonValue>>* RoadsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("roads"), RoadsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RoadsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 2)
			{
				continue;
			}
			BuildRoad(GetCell(Pts[0]), Pts, O->GetNumberField(TEXT("w")) * 100.f,
				O->GetStringField(TEXT("t")), Index);
			++Summary.Roads;
			++Index;
		}
	}

	// --- Sols par cellule ---
	for (auto& Pair : CellBounds)
	{
		FCityMeshBuilder& B = *Cells[Pair.Key];
		const FBox2D& Bx = Pair.Value;
		B.AddQuad(B.WallGroup,
			FVector3f(Bx.Min.X, Bx.Min.Y, 0), FVector3f(Bx.Max.X, Bx.Min.Y, 0),
			FVector3f(Bx.Max.X, Bx.Max.Y, 0), FVector3f(Bx.Min.X, Bx.Max.Y, 0),
			FVector3f(0, 0, 1), FVector3f(0.33f, 0.31f, 0.28f));
	}

	// --- Assets + acteurs par cellule ---
	for (auto& Pair : Cells)
	{
		const FString Name = FString::Printf(TEXT("SM_City_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, WallMat, GlassMat);
		++Summary.Meshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->SetActorLabel(Name);
	}

	// --- Arbres : un mesh + un HISM ---
	const TArray<TSharedPtr<FJsonValue>>* TreesJson = nullptr;
	if (Root->TryGetArrayField(TEXT("trees"), TreesJson) && TreesJson->Num() > 0)
	{
		FCityMeshBuilder TreeBuilder;
		BuildTree(TreeBuilder);
		UStaticMesh* TreeMesh = CreateMeshAsset(AssetFolder / TEXT("SM_CityTree"), TreeBuilder, WallMat, GlassMat);
		++Summary.Meshes;
		// Piege F.39 : usage InstancedStaticMeshes persiste sur le materiau de base.
		WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		GlassMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

		AActor* TreeActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		USceneComponent* Root2 = NewObject<USceneComponent>(TreeActor, TEXT("Root"), RF_Transactional);
		TreeActor->SetRootComponent(Root2);
		TreeActor->AddInstanceComponent(Root2);
		Root2->RegisterComponent();
		Root2->SetWorldLocation(Location);
		UHierarchicalInstancedStaticMeshComponent* Hism =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(TreeActor, TEXT("Trees"), RF_Transactional);
		Hism->SetStaticMesh(TreeMesh);
		Hism->SetupAttachment(Root2);
		TreeActor->AddInstanceComponent(Hism);
		Hism->RegisterComponent();
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *TreesJson)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() < 2)
			{
				continue;
			}
			const float Yaw = FMath::Frac(FMath::Sin(Index * 78.233f) * 12543.21f) * 360.f;
			const float Scale = 0.8f + 0.5f * FMath::Frac(FMath::Sin(Index * 39.11f) * 6543.87f);
			FTransform Xf(FRotator(0, Yaw, 0),
				FVector(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0, 0),
				FVector(Scale));
			Hism->AddInstance(Xf);
			++Summary.Trees;
			++Index;
		}
		TreeActor->SetActorLabel(TEXT("CityTrees"));
	}

	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display, TEXT("District importe : %d batiments, %d routes, %d arbres, %d meshes."),
		Summary.Buildings, Summary.Roads, Summary.Trees, Summary.Meshes);
	return Summary;
}

namespace
{
	struct FMarkerKind
	{
		float HeightCm;
		FVector3f Color;
		bool bLabel;
		float LabelSizeCm = 320.f;
	};

	const TMap<FString, FMarkerKind>& MarkerKinds()
	{
		static const TMap<FString, FMarkerKind> Kinds = {
			{ TEXT("metro"),    { 900.f,  FVector3f(0.90f, 0.10f, 0.10f), true } },
			{ TEXT("metro_e"),  { 400.f,  FVector3f(0.90f, 0.10f, 0.10f), false } },
			{ TEXT("church"),   { 1500.f, FVector3f(0.55f, 0.30f, 0.85f), true } },
			{ TEXT("townhall"), { 1900.f, FVector3f(0.95f, 0.72f, 0.15f), true } },
			// Indications de zones (place=suburb) et quartiers (place=quarter|neighbourhood) :
			// totems hauts et noms geants, lisibles en vol a l'echelle de la ville.
			{ TEXT("district"), { 3500.f, FVector3f(0.10f, 0.80f, 0.75f), true, 1100.f } },
			{ TEXT("quarter"),  { 2200.f, FVector3f(0.25f, 0.50f, 0.95f), true, 650.f } },
		};
		return Kinds;
	}

	// Totem : socle + mat sombre + tete cubique de la couleur du type.
	void BuildTotem(FCityMeshBuilder& QM, const FMarkerKind& Kind)
	{
		auto Box = [&](float HalfX, float HalfY, float Z0, float Z1, const FVector3f& C)
		{
			const FVector3f Mn(-HalfX, -HalfY, Z0), Mx(HalfX, HalfY, Z1);
			const FVector3f v000(Mn.X, Mn.Y, Mn.Z), v100(Mx.X, Mn.Y, Mn.Z), v010(Mn.X, Mx.Y, Mn.Z),
				v110(Mx.X, Mx.Y, Mn.Z), v001(Mn.X, Mn.Y, Mx.Z), v101(Mx.X, Mn.Y, Mx.Z),
				v011(Mn.X, Mx.Y, Mx.Z), v111(Mx.X, Mx.Y, Mx.Z);
			QM.AddQuad(QM.WallGroup, v000, v100, v101, v001, FVector3f(0, -1, 0), Shade(C, FVector3f(0, -1, 0), Z1));
			QM.AddQuad(QM.WallGroup, v110, v010, v011, v111, FVector3f(0, 1, 0), Shade(C, FVector3f(0, 1, 0), Z1));
			QM.AddQuad(QM.WallGroup, v010, v000, v001, v011, FVector3f(-1, 0, 0), Shade(C, FVector3f(-1, 0, 0), Z1));
			QM.AddQuad(QM.WallGroup, v100, v110, v111, v101, FVector3f(1, 0, 0), Shade(C, FVector3f(1, 0, 0), Z1));
			QM.AddQuad(QM.WallGroup, v001, v101, v111, v011, FVector3f(0, 0, 1), Shade(C, FVector3f(0, 0, 1), Z1));
			QM.AddQuad(QM.WallGroup, v010, v110, v100, v000, FVector3f(0, 0, -1), C * 0.4f);
		};
		const float HeadHalf = FMath::Clamp(Kind.HeightCm * 0.12f, 80.f, 180.f);
		Box(70.f, 70.f, 0.f, 25.f, FVector3f(0.22f, 0.22f, 0.24f));
		Box(20.f, 20.f, 25.f, Kind.HeightCm - HeadHalf * 2.f, Kind.Color * 0.55f);
		Box(HeadHalf, HeadHalf, Kind.HeightCm - HeadHalf * 2.f, Kind.HeightCm, Kind.Color);
	}
}

int32 UCityImportTools::ImportCityMarkers(const FString& JsonFilePath, const FString& AssetFolder,
	const FString& WallMaterialPath, FVector Location, const FCityGenProfile& Profile)
{
	// Profil effectif + MNT charge UNE fois pour tout l'import (jalon J2).
	const FCityGenProfile Gen = Profile.Resolved();
	FDrapeContext Drape;
	if (!MakeDrapeContext(Gen, Drape))
	{
		return 0;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read markers file '%s'."), *JsonFilePath));
		return 0;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("Markers file is not valid JSON."));
		return 0;
	}
	const TArray<TSharedPtr<FJsonValue>>* MarkersJson = nullptr;
	if (!Root->TryGetArrayField(TEXT("markers"), MarkersJson) || MarkersJson->Num() == 0)
	{
		RaiseError(TEXT("Markers file has no 'markers' array."));
		return 0;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder must be a package path such as /Game/City/Capitole."));
		return 0;
	}
	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!WallMat || !World)
	{
		if (!World)
		{
			RaiseError(TEXT("No editor world is loaded."));
		}
		return 0;
	}
	WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

	// Idempotence : un re-import remplace les marqueurs existants.
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L == TEXT("CityMarkers") || L.StartsWith(TEXT("Label_")))
		{
			ToDestroy.Add(*It);
		}
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	// Un mesh + un HISM par type present dans le fichier.
	TMap<FString, UHierarchicalInstancedStaticMeshComponent*> HismByKind;
	AActor* MarkerActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
	USceneComponent* Root2 = NewObject<USceneComponent>(MarkerActor, TEXT("Root"), RF_Transactional);
	MarkerActor->SetRootComponent(Root2);
	MarkerActor->AddInstanceComponent(Root2);
	Root2->RegisterComponent();
	Root2->SetWorldLocation(Location);
	MarkerActor->SetActorLabel(TEXT("CityMarkers"));

	int32 Placed = 0;
	for (const TSharedPtr<FJsonValue>& V : *MarkersJson)
	{
		const TSharedPtr<FJsonObject>& O = V->AsObject();
		const FString KindName = O->GetStringField(TEXT("k"));
		const FMarkerKind* Kind = MarkerKinds().Find(KindName);
		if (!Kind)
		{
			continue;
		}
		// Desktop : totem et labels poses a l'altitude du terrain (rebase Capitole).
		const double Mx = O->GetNumberField(TEXT("x")) * 100.0;
		const double My = O->GetNumberField(TEXT("y")) * 100.0;
		const FVector Pos = Location + FVector(Mx, My, Drape.GroundZ(Mx, My));

		UHierarchicalInstancedStaticMeshComponent*& Hism = HismByKind.FindOrAdd(KindName);
		if (!Hism)
		{
			FCityMeshBuilder Builder;
			BuildTotem(Builder, *Kind);
			UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / (TEXT("SM_Marker_") + KindName), Builder,
				WallMat, WallMat);
			Hism = NewObject<UHierarchicalInstancedStaticMeshComponent>(MarkerActor,
				*(TEXT("Markers_") + KindName), RF_Transactional);
			Hism->SetStaticMesh(Mesh);
			Hism->SetupAttachment(Root2);
			MarkerActor->AddInstanceComponent(Hism);
			Hism->RegisterComponent();
		}
		Hism->AddInstance(FTransform(Pos - Location));

		// Nom flottant : QUATRE orientations empilees (0/90/180/270 — le materiau texte
		// est double-face avec revers en miroir : superposes les glyphes se melangent,
		// empiles il y a toujours un exemplaire lisible NON-miroir de n'importe quelle
		// direction au sol) + UN exemplaire A PLAT au sommet pour la vue aerienne.
		const FString Label = O->GetStringField(TEXT("n"));
		if (Kind->bLabel && !Label.IsEmpty())
		{
			const FColor LabelColor(
				FMath::RoundToInt(Kind->Color.X * 255.f),
				FMath::RoundToInt(Kind->Color.Y * 255.f),
				FMath::RoundToInt(Kind->Color.Z * 255.f));
			auto MakeText = [&](const FVector& P, const FRotator& Rot, int32 Idx)
			{
				ATextRenderActor* Text = World->SpawnActor<ATextRenderActor>(P, Rot);
				UTextRenderComponent* Comp = Text->GetTextRender();
				Comp->SetText(FText::FromString(Label));
				Comp->SetWorldSize(Kind->LabelSizeCm);
				Comp->SetHorizontalAlignment(EHTA_Center);
				Comp->SetTextRenderColor(LabelColor);
				Text->SetActorLabel(FString::Printf(TEXT("Label_%s_%d"), *Label, Idx));
			};
			for (int32 i = 0; i < 4; ++i)
			{
				MakeText(Pos + FVector(0, 0, Kind->HeightCm + Kind->LabelSizeCm * (0.5625 + i * 1.125)),
					FRotator(0.f, i * 90.f, 0.f), i);
			}
			// Paire A PLAT dos-a-dos (30 cm d'ecart) : le materiau texte double-face a un
			// revers en miroir — une copie seule se lisait a l'envers vue du ciel.
			MakeText(Pos + FVector(0, 0, Kind->HeightCm + Kind->LabelSizeCm * 5.2),
				FRotator(-90.f, 0.f, 0.f), 4);
			MakeText(Pos + FVector(0, 0, Kind->HeightCm + Kind->LabelSizeCm * 5.2 + 30.0),
				FRotator(90.f, 180.f, 0.f), 5);
		}
		++Placed;
	}
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display, TEXT("%d marqueurs places."), Placed);
	return Placed;
}

namespace
{
	// Polygone plat de reperage (eau, parc, bois) : triangulation de l'anneau, teinte
	// cuite. TerrainZ (desktop) : Z terrain PAR SOMMET ajoute a l'offset d'empilement
	// Zcm (verts drapes) ; nul = film plat historique (mobile, et l'eau desktop qui
	// reste plane — son niveau est alors porte par Zcm).
	void BuildFlatPolygon(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float Zcm,
		const FVector3f& Tint, const TArray<float>* TerrainZ = nullptr)
	{
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		const FVector3f Shaded = Shade(Tint, FVector3f(0, 0, 1), Zcm);
		auto VertexZ = [&](int32 Index)
		{
			return Zcm + (TerrainZ ? (*TerrainZ)[Index] : 0.f);
		};
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			QM.AddTri(QM.WallGroup,
				FVector3f(PtsCm[Tris[t]].X, PtsCm[Tris[t]].Y, VertexZ(Tris[t])),
				FVector3f(PtsCm[Tris[t + 1]].X, PtsCm[Tris[t + 1]].Y, VertexZ(Tris[t + 1])),
				FVector3f(PtsCm[Tris[t + 2]].X, PtsCm[Tris[t + 2]].Y, VertexZ(Tris[t + 2])),
				FVector3f(0, 0, 1), Shaded);
		}
	}

	// Ruban de voie ferree : ballast sombre, sans trottoir ni marquage.
	// TerrainZ : cf. BuildRoad (drapage desktop, nul = plat mobile).
	void BuildRail(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float WidthCm, int32 RailIndex,
		const TArray<float>* TerrainZ = nullptr)
	{
		const int32 N = PtsCm.Num();
		if (N < 2)
		{
			return;
		}
		// Empilement decimetrique (cf. BuildRoad) + decalage par voie aux croisements.
		const float Z = 45.f + (RailIndex % 4) * 2.f;
		const FVector3f Ballast(0.24f, 0.22f, 0.21f);
		const FVector3f Up(0, 0, 1);
		TArray<FVector2D> Nrm;
		Nrm.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FVector2D D(0, 0);
			if (i > 0) { D += (PtsCm[i] - PtsCm[i - 1]).GetSafeNormal(); }
			if (i < N - 1) { D += (PtsCm[i + 1] - PtsCm[i]).GetSafeNormal(); }
			D = D.GetSafeNormal();
			Nrm[i] = FVector2D(-D.Y, D.X);
		}
		const float Half = WidthCm * 0.5f;
		for (int32 i = 0; i + 1 < N; ++i)
		{
			const FVector2D A = PtsCm[i], B = PtsCm[i + 1];
			const FVector2D NA = Nrm[i] * Half, NB = Nrm[i + 1] * Half;
			const float ZA = Z + (TerrainZ ? (*TerrainZ)[i] : 0.f);
			const float ZB = Z + (TerrainZ ? (*TerrainZ)[i + 1] : 0.f);
			QM.AddQuad(QM.WallGroup,
				FVector3f(A.X - NA.X, A.Y - NA.Y, ZA), FVector3f(B.X - NB.X, B.Y - NB.Y, ZB),
				FVector3f(B.X + NB.X, B.Y + NB.Y, ZB), FVector3f(A.X + NA.X, A.Y + NA.Y, ZA),
				Up, Ballast);
		}
	}

	// Polygone de peinture du sol (echantillonnage des dalles).
	struct FPaintPoly
	{
		FBox2D Bounds;
		TArray<FVector2D> Pts;
		FVector3f Tint;
		int32 Priority = 0;
	};

	bool PointInRing(const TArray<FVector2D>& P, const FVector2D& Q)
	{
		bool bIn = false;
		for (int32 i = 0, j = P.Num() - 1; i < P.Num(); j = i++)
		{
			if (((P[i].Y > Q.Y) != (P[j].Y > Q.Y)) &&
				(Q.X < (P[j].X - P[i].X) * (Q.Y - P[i].Y) / (P[j].Y - P[i].Y) + P[i].X))
			{
				bIn = !bIn;
			}
		}
		return bIn;
	}
}

FCitySurfacesSummary UCityImportTools::ImportCitySurfaces(const FString& JsonFilePath,
	const FString& AssetFolder, const FString& WallMaterialPath, float CellSizeM, FVector Location,
	const FCityGenProfile& Profile)
{
	FCitySurfacesSummary Summary;

	// Profil effectif + MNT charge UNE fois pour tout l'import (jalon J2).
	const FCityGenProfile Gen = Profile.Resolved();
	FDrapeContext Drape;
	if (!MakeDrapeContext(Gen, Drape))
	{
		return Summary;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read surfaces file '%s'."), *JsonFilePath));
		return Summary;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("Surfaces file is not valid JSON."));
		return Summary;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder must be a package path such as /Game/City/Capitole."));
		return Summary;
	}
	if (CellSizeM < 20.f)
	{
		RaiseError(TEXT("CellSizeM must be at least 20."));
		return Summary;
	}
	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!WallMat || !World)
	{
		if (!World)
		{
			RaiseError(TEXT("No editor world is loaded."));
		}
		return Summary;
	}

	// Idempotence : un re-import remplace les surfaces existantes.
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L.StartsWith(TEXT("SM_Surface_")) || L == TEXT("CitySurfaceTrees"))
		{
			ToDestroy.Add(*It);
		}
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	const float Cell = CellSizeM * 100.f;
	TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> Cells;
	auto GetCell = [&](const FVector2D& P) -> FCityMeshBuilder&
	{
		const FIntPoint Key(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell));
		TUniquePtr<FCityMeshBuilder>& B = Cells.FindOrAdd(Key);
		if (!B)
		{
			B = MakeUnique<FCityMeshBuilder>();
		}
		return *B;
	};
	auto ReadPts = [](const TArray<TSharedPtr<FJsonValue>>& In, TArray<FVector2D>& Out)
	{
		for (const TSharedPtr<FJsonValue>& V : In)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() >= 2)
			{
				Out.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
			}
		}
	};
	auto Centroid = [](const TArray<FVector2D>& Pts)
	{
		FVector2D C(0, 0);
		for (const FVector2D& P : Pts) { C += P; }
		return C / Pts.Num();
	};

	// Empilement decimetrique sous les routes (55+) : parc 15 < bois 20 < eau 30 <
	// rail 45 (cf. BuildRoad — ecarts centimetriques = z-fight garanti a 2 km en GLES).
	const FVector3f WaterTint(0.16f, 0.30f, 0.38f);
	const FVector3f ForestTint(0.20f, 0.34f, 0.16f);
	const FVector3f ParkTint(0.35f, 0.48f, 0.22f);
	TArray<FTransform> ScatterXf;

	const TArray<TSharedPtr<FJsonValue>>* WaterJson = nullptr;
	if (Root->TryGetArrayField(TEXT("water"), WaterJson))
	{
		for (const TSharedPtr<FJsonValue>& V : *WaterJson)
		{
			TArray<FVector2D> Pts;
			ReadPts(V->AsObject()->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			// Desktop (J2 §3.4) : l'eau n'est PAS drapee — plan horizontal au
			// percentile bas (p10) du MNT sous le polygone (le MNT sol nu descend
			// dans le lit ; la Garonne reste plane par troncon), offset conserve.
			float ZWater = 30.f;
			if (Drape.IsActive())
			{
				ZWater += Drape.Sampler->PercentileAltCmInPolygon(Pts, 0.10f) - Drape.AltCapCm;
			}
			BuildFlatPolygon(GetCell(Centroid(Pts)), Pts, ZWater, WaterTint);
			++Summary.Water;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* GreenJson = nullptr;
	if (Root->TryGetArrayField(TEXT("green"), GreenJson))
	{
		int32 GreenIndex = 0;
		for (const TSharedPtr<FJsonValue>& V : *GreenJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			const bool bForest = O->GetStringField(TEXT("k")) == TEXT("forest");
			// Etagement PAR POLYGONE : les verts se chevauchent entre eux (parc sur
			// pelouse, bois sur parc) — a hauteur egale ils z-fightaient encore.
			// Desktop : drapes sur le MNT par sommet, l'etagement reste RELATIF.
			const float ZJitter = (GreenIndex % 5) * 1.5f;
			TArray<float> GreenTerrainZ;
			const TArray<float>* GreenTerrainZPtr = nullptr;
			if (Drape.IsActive())
			{
				ComputePolylineZ(Pts, Drape, /*bBridge=*/false, GreenTerrainZ);
				GreenTerrainZPtr = &GreenTerrainZ;
			}
			BuildFlatPolygon(GetCell(Centroid(Pts)), Pts, (bForest ? 20.f : 12.f) + ZJitter,
				bForest ? ForestTint : ParkTint, GreenTerrainZPtr);
			++GreenIndex;
			++Summary.Green;

			// Bois : arbres proceduraux disperses sur une grille de 28 m avec jitter,
			// plafond global — reperage leger, pas une foret dense.
			if (bForest && ScatterXf.Num() < 80000)
			{
				FBox2D Box(ForceInit);
				for (const FVector2D& P : Pts) { Box += P; }
				constexpr float Step = 2800.f;
				for (float Y = Box.Min.Y; Y <= Box.Max.Y; Y += Step)
				{
					for (float X = Box.Min.X; X <= Box.Max.X; X += Step)
					{
						const int32 Seed = FMath::RoundToInt32(X * 0.001f + Y * 0.017f);
						const float Jx = (FMath::Frac(FMath::Sin(Seed * 12.9898f) * 43758.5453f) - 0.5f) * Step * 0.6f;
						const float Jy = (FMath::Frac(FMath::Sin(Seed * 78.233f) * 12543.21f) - 0.5f) * Step * 0.6f;
						const FVector2D Q(X + Jx, Y + Jy);
						if (!PointInRing(Pts, Q))
						{
							continue;
						}
						const float Yaw = FMath::Frac(FMath::Sin(Seed * 39.11f) * 6543.87f) * 360.f;
						const float Scale = 0.9f + 0.6f * FMath::Frac(FMath::Sin(Seed * 3.7f) * 971.3f);
						ScatterXf.Emplace(FRotator(0, Yaw, 0),
							FVector(Q.X, Q.Y, Drape.GroundZ(Q.X, Q.Y)), FVector(Scale));
						++Summary.ScatterTrees;
						if (ScatterXf.Num() >= 80000)
						{
							break;
						}
					}
					if (ScatterXf.Num() >= 80000)
					{
						break;
					}
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RailsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("rails"), RailsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RailsJson)
		{
			TArray<FVector2D> Pts;
			ReadPts(V->AsObject()->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 2)
			{
				continue;
			}
			// Desktop : meme traitement que les routes (re-echantillonnage + drapage).
			const TArray<FVector2D>* RailPts = &Pts;
			TArray<FVector2D> Resampled;
			TArray<float> TerrainZ;
			const TArray<float>* TerrainZPtr = nullptr;
			if (Drape.IsActive())
			{
				if (Gen.RoadResampleStepCm > 0.f)
				{
					Resampled = ResamplePolyline(Pts, Gen.RoadResampleStepCm);
					RailPts = &Resampled;
				}
				ComputePolylineZ(*RailPts, Drape, /*bBridge=*/false, TerrainZ);
				TerrainZPtr = &TerrainZ;
			}
			BuildRail(GetCell(Pts[0]), *RailPts, 400.f, Index, TerrainZPtr);
			++Summary.Rails;
			++Index;
		}
	}

	// --- Assets + acteurs par cellule ---
	// SANS collision : ces surfaces sont des films de 1-3 cm poses sur la dalle de sol
	// qui porte deja la collision — le trimesh cuit etait ~200 Mo de RAM device pour rien.
	for (auto& Pair : Cells)
	{
		const FString Name = FString::Printf(TEXT("SM_Surface_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, WallMat, WallMat, false);
		++Summary.Meshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Actor->SetActorLabel(Name);
	}

	// --- Arbres disperses : HISM dedie, mesh SM_CityTree reutilise s'il existe ---
	if (ScatterXf.Num() > 0)
	{
		const FString TreePath = AssetFolder / TEXT("SM_CityTree");
		UStaticMesh* TreeMesh = LoadObject<UStaticMesh>(nullptr,
			*(TreePath + TEXT(".SM_CityTree")), nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (!TreeMesh)
		{
			FCityMeshBuilder TreeBuilder;
			BuildTree(TreeBuilder);
			TreeMesh = CreateMeshAsset(TreePath, TreeBuilder, WallMat, WallMat);
			++Summary.Meshes;
		}
		WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

		AActor* TreeActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		USceneComponent* Root2 = NewObject<USceneComponent>(TreeActor, TEXT("Root"), RF_Transactional);
		TreeActor->SetRootComponent(Root2);
		TreeActor->AddInstanceComponent(Root2);
		Root2->RegisterComponent();
		Root2->SetWorldLocation(Location);
		UHierarchicalInstancedStaticMeshComponent* Hism =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(TreeActor, TEXT("Trees"), RF_Transactional);
		Hism->SetStaticMesh(TreeMesh);
		Hism->SetupAttachment(Root2);
		TreeActor->AddInstanceComponent(Hism);
		Hism->RegisterComponent();
		for (const FTransform& Xf : ScatterXf)
		{
			Hism->AddInstance(Xf);
		}
		TreeActor->SetActorLabel(TEXT("CitySurfaceTrees"));
	}

	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display,
		TEXT("Surfaces importees : %d eau, %d vert, %d rails, %d arbres disperses, %d meshes."),
		Summary.Water, Summary.Green, Summary.Rails, Summary.ScatterTrees, Summary.Meshes);
	return Summary;
}

namespace
{
	// Texture de fenetres tuilee (1 tuile = 1 fenetre centree sur fond mur blanc, le
	// blanc etant teinte par les vertex colors de la facade dans le materiau).
	UTexture2D* GetOrCreateFacadeTexture(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("T_FacadeWindow");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		constexpr int32 Size = 128;
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		// Fenetre : 50 % de largeur, 45 % de hauteur, centree (52 % vertical — appui bas).
		constexpr int32 X0 = 32, X1 = 96, Y0 = 33, Y1 = 91;
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const bool bIn = X >= X0 && X < X1 && Y >= Y0 && Y < Y1;
				const bool bFrame = bIn && (X < X0 + 3 || X >= X1 - 3 || Y < Y0 + 3 || Y >= Y1 - 3);
				FColor C(255, 255, 255);                       // mur : blanc -> teinte facade
				if (bFrame) { C = FColor(210, 208, 200); }     // cadre clair
				else if (bIn) { C = FColor(58, 64, 74); }      // vitre sombre bleutee
				// Croisillon central vertical (2 px) pour casser l'aplat.
				if (bIn && !bFrame && FMath::Abs(X - (X0 + X1) / 2) < 1) { C = FColor(96, 100, 106); }
				Pixels[Y * Size + X] = C;
			}
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UTexture2D* Tex = NewObject<UTexture2D>(Package, *ObjectName, RF_Public | RF_Standalone);
		Tex->Source.Init(Size, Size, 1, 1, TSF_BGRA8, (const uint8*)Pixels.GetData());
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->AddressX = TA_Wrap;
		Tex->AddressY = TA_Wrap;
		Tex->UpdateResource();
		Tex->PostEditChange();
		Tex->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Tex);
		return Tex;
	}

	// Materiau facade : unlit, Emissive = texture fenetres x vertex color (meme
	// convention que M_BldgWall : les vertex colors sont deja compensees pow 2.2).
	UMaterialInterface* GetOrCreateFacadeMaterial(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("M_BldgFacade");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		UTexture2D* Tex = GetOrCreateFacadeTexture(AssetFolder);
		UPackage* Package = CreatePackage(*AssetPath);
		UMaterial* M = NewObject<UMaterial>(Package, *ObjectName, RF_Public | RF_Standalone);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_Unlit);
		UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>(M);
		Sample->Texture = Tex;
		Sample->SamplerType = SAMPLERTYPE_Color;
		M->GetExpressionCollection().AddExpression(Sample);
		UMaterialExpressionVertexColor* VColor = NewObject<UMaterialExpressionVertexColor>(M);
		M->GetExpressionCollection().AddExpression(VColor);
		UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
		Mul->A.Connect(0, Sample);
		Mul->B.Connect(0, VColor);
		M->GetExpressionCollection().AddExpression(Mul);
		M->GetEditorOnlyData()->EmissiveColor.Connect(0, Mul);
		M->PostEditChange();
		M->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(M);
		return M;
	}

	// Texture de ruban routier : U = travers (trottoir|bordure|asphalte|marquage|...),
	// V = longueur (tirets 3 m peints sur V<0,5, periode 6 m). Multipliee par la
	// teinte claire du vertex color : asphalte 0.2x, trottoir 0.59x, marquage 1x.
	UTexture2D* GetOrCreateRoadTexture(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("T_RoadStrip");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		constexpr int32 Size = 128;
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				uint8 V = 50;                                    // asphalte
				if (X < 15 || X >= 113) { V = 150; }             // trottoirs
				else if (X < 17 || X >= 111) { V = 35; }         // bordures (lisibilite)
				else if (X >= 58 && X < 70 && Y < 64) { V = 255; } // marquage central (tiret)
				Pixels[Y * Size + X] = FColor(V, V, V);
			}
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UTexture2D* Tex = NewObject<UTexture2D>(Package, *ObjectName, RF_Public | RF_Standalone);
		Tex->Source.Init(Size, Size, 1, 1, TSF_BGRA8, (const uint8*)Pixels.GetData());
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Wrap;
		Tex->UpdateResource();
		Tex->PostEditChange();
		Tex->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Tex);
		return Tex;
	}

	// Materiau ruban routier : unlit, texture x vertex color (meme patron que la facade).
	UMaterialInterface* GetOrCreateRoadMaterial(const FString& AssetFolder)
	{
		const FString AssetPath = AssetFolder / TEXT("M_RoadStrip");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		UTexture2D* Tex = GetOrCreateRoadTexture(AssetFolder);
		UPackage* Package = CreatePackage(*AssetPath);
		UMaterial* M = NewObject<UMaterial>(Package, *ObjectName, RF_Public | RF_Standalone);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_Unlit);
		UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>(M);
		Sample->Texture = Tex;
		Sample->SamplerType = SAMPLERTYPE_Color;
		M->GetExpressionCollection().AddExpression(Sample);
		UMaterialExpressionVertexColor* VColor = NewObject<UMaterialExpressionVertexColor>(M);
		M->GetExpressionCollection().AddExpression(VColor);
		UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
		Mul->A.Connect(0, Sample);
		Mul->B.Connect(0, VColor);
		M->GetExpressionCollection().AddExpression(Mul);
		M->GetEditorOnlyData()->EmissiveColor.Connect(0, Mul);
		M->PostEditChange();
		M->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(M);
		return M;
	}

	// ---- Lot B (J2 §3.3-3.4) : matiere desktop — atlas de facades + materiaux PBR ----

	// Hash 2D stable (meme famille que les jitters d'arbres).
	float TileHash(int32 A, int32 B)
	{
		return FMath::Frac(FMath::Sin(A * 12.9898f + B * 78.233f) * 43758.5453f);
	}

	// Atlas de facades 2048² (parametrable) en grille 4x4 de sous-tuiles generees
	// procedureralement : 0-3 brique toulousaine (4 variantes), 4-7 enduit (4),
	// 8 moderne, 9 industriel, 10 toit tuiles terre cuite, 11 toit ardoise,
	// 12 pierre claire (modenature), 13 beton, 14 enduit blanc, 15 neutre.
	// Tuiles quasi neutres (valeur/motif) : la COULEUR vient du VertexColor
	// (UsageTint) dans M_CityWall_PBR — sauf les toits qui portent leur teinte.
	UTexture2D* GetOrCreateCityAtlasTexture(const FString& AssetFolder, int32 SizePx)
	{
		const FString AssetPath = AssetFolder / TEXT("T_CityAtlas");
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UTexture2D* Existing = LoadObject<UTexture2D>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		const int32 Size = FMath::Clamp(SizePx, 512, 4096);
		const int32 T = Size / 4;
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Size * Size);
		for (int32 Y = 0; Y < Size; ++Y)
		{
			for (int32 X = 0; X < Size; ++X)
			{
				const int32 Tile = (Y / T) * 4 + (X / T);
				const int32 LX = X % T, LY = Y % T;
				FColor C(200, 200, 200);
				if (Tile <= 3)
				{
					// Brique : rangees decalees d'une demi-brique, joints de mortier.
					const int32 BH = FMath::Max(T / 18, 4), BW = FMath::Max(T / 8, 8);
					const int32 Row = LY / BH;
					const int32 LXo = LX + ((Row % 2) ? BW / 2 : 0);
					const int32 Col2 = LXo / BW;
					if (LY % BH < 2 || LXo % BW < 2)
					{
						C = FColor(168, 165, 160);                    // mortier
					}
					else
					{
						const uint8 V = (uint8)(196 + 28.f * (TileHash(Col2, Row * 7 + Tile) - 0.5f));
						C = FColor(V, (uint8)(V * 0.95f), (uint8)(V * 0.90f));
					}
				}
				else if (Tile <= 7)
				{
					// Enduit : grain leger, 4 graines de variation.
					const float N = TileHash(LX / 6 + Tile * 31, LY / 6)
						+ 0.5f * TileHash(LX, LY + Tile);
					const uint8 V = (uint8)FMath::Clamp(206.f + 14.f * (N - 0.75f), 0.f, 255.f);
					C = FColor(V, V, (uint8)(V * 0.97f));
				}
				else if (Tile == 8)
				{
					// Moderne : panneaux avec joints creux.
					const int32 P = T / 4;
					C = (LX % P < 2 || LY % P < 2) ? FColor(140, 140, 142) : FColor(212, 212, 214);
				}
				else if (Tile == 9)
				{
					// Industriel : bardage ondule vertical.
					C = ((LX / FMath::Max(T / 64, 2)) % 2) ? FColor(180, 181, 184) : FColor(204, 205, 208);
				}
				else if (Tile == 10 || Tile == 11)
				{
					// Toits : rangees de tuiles ombrees (terre cuite / ardoise).
					const int32 RH = FMath::Max(T / 12, 6);
					const float G = (float)(LY % RH) / RH;
					const int32 Row = LY / RH;
					const int32 Sep = FMath::Max(T / 16, 4);
					float V01 = 0.55f + 0.45f * G;
					if ((LX + Row * Sep / 2) % Sep < 1) { V01 *= 0.8f; }
					if (Tile == 10)
					{
						C = FColor((uint8)(212 * V01), (uint8)(124 * V01), (uint8)(88 * V01));
					}
					else
					{
						const uint8 V = (uint8)(150 * V01 + 20.f * TileHash(LX / Sep, Row));
						C = FColor(V, V, (uint8)(V * 1.05f));
					}
				}
				else if (Tile == 12)
				{
					// Pierre claire : modenature (tableaux, appuis, linteaux).
					const uint8 V = (uint8)(220 + 8.f * (TileHash(LX / 8, LY / 8) - 0.5f));
					C = FColor(V, (uint8)(V * 0.98f), (uint8)(V * 0.93f));
				}
				else if (Tile == 13)
				{
					const uint8 V = (uint8)(184 + 20.f * (TileHash(LX / 12 + 5, LY / 12) - 0.5f));
					C = FColor(V, V, V);
				}
				else if (Tile == 14)
				{
					const uint8 V = (uint8)(233 + 6.f * (TileHash(LX, LY + 9) - 0.5f));
					C = FColor(V, V, V);
				}
				Pixels[Y * Size + X] = C;
			}
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UTexture2D* Tex = NewObject<UTexture2D>(Package, *ObjectName, RF_Public | RF_Standalone);
		Tex->Source.Init(Size, Size, 1, 1, TSF_BGRA8, (const uint8*)Pixels.GetData());
		Tex->SRGB = true;
		Tex->LODGroup = TEXTUREGROUP_World;
		Tex->AddressX = TA_Clamp;
		Tex->AddressY = TA_Clamp;
		Tex->UpdateResource();
		Tex->PostEditChange();
		Tex->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Tex);
		return Tex;
	}

	// UV dans une sous-tuile de l'atlas (grille 4x4), marge anti-bleed 3 %.
	FVector2f AtlasUV(int32 Tile, float U01, float V01)
	{
		const int32 TX = Tile % 4, TY = Tile / 4;
		const float M = 0.03f;
		return FVector2f((TX + M + FMath::Clamp(U01, 0.f, 1.f) * (1.f - 2.f * M)) * 0.25f,
			(TY + M + FMath::Clamp(V01, 0.f, 1.f) * (1.f - 2.f * M)) * 0.25f);
	}

	// Sous-tuile de facade par usage/graine : meme logique de variation qu'UsageTint.
	int32 UsageTile(const FString& Usage, int32 Seed)
	{
		if (Usage == TEXT("res")) { return Seed % 4; }         // brique toulousaine x4
		if (Usage == TEXT("com")) { return 4 + Seed % 4; }     // enduit x4
		if (Usage == TEXT("ind")) { return 9; }                // industriel
		return 8;                                              // moderne
	}

	// Fabrique commune des materiaux PBR Lot B : DefaultLit (PAS unlit — Lumen
	// eclaire, spec §3.3), BaseColor = [Texture x] VertexColor ou constante,
	// Roughness/Metallic constants. Meme patron code que GetOrCreateFacadeMaterial.
	UMaterialInterface* GetOrCreatePBRMaterial(const FString& AssetFolder, const TCHAR* Name,
		UTexture2D* TexOrNull, bool bVertexColor, const FLinearColor& ConstantBase,
		float Roughness, float Metallic)
	{
		const FString AssetPath = AssetFolder / Name;
		const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (UMaterialInterface* Existing = LoadObject<UMaterialInterface>(nullptr,
			*(AssetPath + TEXT(".") + ObjectName), nullptr, LOAD_NoWarn | LOAD_Quiet))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*AssetPath);
		UMaterial* M = NewObject<UMaterial>(Package, *ObjectName, RF_Public | RF_Standalone);
		M->MaterialDomain = MD_Surface;
		M->SetShadingModel(MSM_DefaultLit);
		// Flag d'usage Nanite : les meshes Lot B opaques (et le verre opaque J2e)
		// sont Nanite ; sans ce flag, -game refuse le materiau (« missing usage
		// flag Nanite ») et rend le fallback decime 0,1 % en Default Material
		// (toits fondus). L'acces direct a bUsedWithNanite est deprecie en 5.8 :
		// SetUsageByFlag pose le flag sans recompiler, le PostEditChange final
		// compile avec l'usage inclus.
		M->SetUsageByFlag(MATUSAGE_Nanite, true);
		UMaterialExpression* Base = nullptr;
		if (TexOrNull)
		{
			UMaterialExpressionTextureSample* Sample = NewObject<UMaterialExpressionTextureSample>(M);
			Sample->Texture = TexOrNull;
			Sample->SamplerType = SAMPLERTYPE_Color;
			M->GetExpressionCollection().AddExpression(Sample);
			Base = Sample;
		}
		if (bVertexColor)
		{
			UMaterialExpressionVertexColor* VColor = NewObject<UMaterialExpressionVertexColor>(M);
			M->GetExpressionCollection().AddExpression(VColor);
			if (Base)
			{
				UMaterialExpressionMultiply* Mul = NewObject<UMaterialExpressionMultiply>(M);
				Mul->A.Connect(0, Base);
				Mul->B.Connect(0, VColor);
				M->GetExpressionCollection().AddExpression(Mul);
				Base = Mul;
			}
			else
			{
				Base = VColor;
			}
		}
		if (!Base)
		{
			UMaterialExpressionConstant3Vector* CB = NewObject<UMaterialExpressionConstant3Vector>(M);
			CB->Constant = ConstantBase;
			M->GetExpressionCollection().AddExpression(CB);
			Base = CB;
		}
		M->GetEditorOnlyData()->BaseColor.Connect(0, Base);
		UMaterialExpressionConstant* R = NewObject<UMaterialExpressionConstant>(M);
		R->R = Roughness;
		M->GetExpressionCollection().AddExpression(R);
		M->GetEditorOnlyData()->Roughness.Connect(0, R);
		if (Metallic > 0.f)
		{
			UMaterialExpressionConstant* Met = NewObject<UMaterialExpressionConstant>(M);
			Met->R = Metallic;
			M->GetExpressionCollection().AddExpression(Met);
			M->GetEditorOnlyData()->Metallic.Connect(0, Met);
		}
		M->PostEditChange();
		M->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(M);
		return M;
	}

	// Murs : atlas facades x VertexColor (teinte UsageTint LINEAIRE = variation).
	UMaterialInterface* GetOrCreateWallPBRMaterial(const FString& AssetFolder, int32 AtlasSizePx)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityWall_PBR"),
			GetOrCreateCityAtlasTexture(AssetFolder, AtlasSizePx), true,
			FLinearColor::White, 0.8f, 0.f);
	}

	// Vitres : OPAQUE tres lisse (choix Q3, le plus robuste Lumen : le translucide
	// n'a ni reflets Lumen fiables ni depth propre, l'opaque roughness 0,05 recoit
	// reflexions ecran/Lumen sans cout ni tri). Sombre bleute, leger metallic.
	UMaterialInterface* GetOrCreateGlassPBRMaterial(const FString& AssetFolder)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityGlass_PBR"),
			nullptr, false, FLinearColor(0.02f, 0.035f, 0.05f), 0.05f, 0.4f);
	}

	// Sol : version lit du vertex-color unlit historique (dalles peintes, sentiers).
	UMaterialInterface* GetOrCreateGroundPBRMaterial(const FString& AssetFolder)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityGround_PBR"),
			nullptr, true, FLinearColor::White, 0.9f, 0.f);
	}

	// Routes : version lit du ruban texture (meme T_RoadStrip x VertexColor).
	UMaterialInterface* GetOrCreateRoadPBRMaterial(const FString& AssetFolder)
	{
		return GetOrCreatePBRMaterial(AssetFolder, TEXT("M_CityRoad_PBR"),
			GetOrCreateRoadTexture(AssetFolder), true, FLinearColor::White, 0.85f, 0.f);
	}

	// Batiment desktop Lot B : fenetres GEOMETRIQUES en creux. Wall recoit murs,
	// socle, toit et modenature (opaque -> atlas PBR, Nanite) ; Glass les vitres
	// (Nanite AUSSI depuis J2e — verre opaque). En mode non-split, passer le
	// MEME builder aux deux.
	// bReveals : par fenetre, +9 quads (+18 tris) EXACTEMENT vs la vitre en simple
	// retrait — 4 tableaux (retours 18 cm) + appui saillant 3 quads (dessus/face/
	// dessous, saillie 8 cm) + linteau saillant 2 quads (face avant/sous-face,
	// saillie 6 cm). C'est la base du test « delta de tris par fenetre = +18 ».
	// bBakedShade=false (PBR) : teintes brutes, Lumen eclaire (plus de Shade cuit).
	void BuildPolygonBuildingDesktop(FCityMeshBuilder& Wall, FCityMeshBuilder& Glass,
		const TArray<FVector2D>& PtsCm, float Hcm, const FVector3f& Tint, int32 WallTile,
		float ZBaseCm, float SocleDepthCm, bool bReveals, bool bBakedShade)
	{
		const int32 Floors = FMath::Clamp(FMath::RoundToInt32(Hcm / 290.f), 1, 40);
		const float FloorH = Hcm / Floors;
		const FVector3f StoneTint(0.82f, 0.79f, 0.72f);
		auto Col = [&](const FVector3f& C, const FVector3f& Nrm, float Zrel)
		{
			return bBakedShade ? Shade(C, Nrm, Zrel) : C;
		};
		const int32 N = PtsCm.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = PtsCm[e];
			const FVector2D B2 = PtsCm[(e + 1) % N];
			const FVector2D Dir2 = B2 - A2;
			const float Len = Dir2.Size();
			if (Len < 30.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			const FVector3f Nout(T2.Y, -T2.X, 0.f);
			const FVector3f A(A2.X, A2.Y, 0.f);
			const FVector3f T(T2.X, T2.Y, 0.f);
			// Point de facade : U le long du mur, Z relatif au rez-de-chaussee,
			// D profondeur vers l'interieur (creux) — negatif = saillie.
			auto Pt = [&](float U, float Z, float D)
			{
				return A + T * U + FVector3f(0, 0, ZBaseCm + Z) - Nout * D;
			};
			// Quad de mur : UV0 dans la sous-tuile atlas, echelle ~5,12 m / tuile
			// (clampee : un mur aveugle geant etire sa tuile, assume Lot B).
			auto WallQuad = [&](float U0, float U1, float Z0, float Z1)
			{
				if (U1 - U0 < 1.f || Z1 - Z0 < 1.f)
				{
					return;
				}
				const FVector3f P[4] = { Pt(U0, Z0, 0), Pt(U1, Z0, 0), Pt(U1, Z1, 0), Pt(U0, Z1, 0) };
				const float SU = (U1 - U0) / 512.f;
				const float SV = (Z1 - Z0) / 512.f;
				const FVector2f UV[4] = { AtlasUV(WallTile, 0, 0), AtlasUV(WallTile, SU, 0),
					AtlasUV(WallTile, SU, SV), AtlasUV(WallTile, 0, SV) };
				Wall.AddPoly(Wall.WallGroup, P, 4, Nout, UV, Col(Tint, Nout, (Z0 + Z1) * 0.5f));
			};
			// Quad de modenature en pierre claire (sous-tuile 12).
			auto StoneQuad = [&](const FVector3f& P0, const FVector3f& P1, const FVector3f& P2,
				const FVector3f& P3, const FVector3f& Nrm)
			{
				const FVector3f P[4] = { P0, P1, P2, P3 };
				const FVector2f UV[4] = { AtlasUV(12, 0.1f, 0.1f), AtlasUV(12, 0.4f, 0.1f),
					AtlasUV(12, 0.4f, 0.4f), AtlasUV(12, 0.1f, 0.4f) };
				Wall.AddPoly(Wall.WallGroup, P, 4, Nrm, UV,
					Col(StoneTint, Nrm, P0.Z - ZBaseCm));
			};
			// Fenetre : vitre en retrait D, puis modenature (+9 quads si bReveals).
			auto Window = [&](float WU0, float WU1, float WZ0, float WZ1)
			{
				const float D = 18.f;
				const FVector3f G[4] = { Pt(WU0, WZ0, D), Pt(WU1, WZ0, D),
					Pt(WU1, WZ1, D), Pt(WU0, WZ1, D) };
				const FVector2f GUV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
				Glass.AddPoly(Glass.GlassGroup, G, 4, Nout, GUV,
					bBakedShade ? Shade(FVector3f(0.35f, 0.35f, 0.35f), Nout, (WZ0 + WZ1) * 0.5f)
						: FVector3f(1, 1, 1));
				if (!bReveals)
				{
					return;
				}
				// 4 tableaux (retours d'embrasure de la facade vers la vitre).
				StoneQuad(Pt(WU0, WZ0, 0), Pt(WU0, WZ1, 0), Pt(WU0, WZ1, D), Pt(WU0, WZ0, D), T);
				StoneQuad(Pt(WU1, WZ0, 0), Pt(WU1, WZ1, 0), Pt(WU1, WZ1, D), Pt(WU1, WZ0, D), -T);
				StoneQuad(Pt(WU0, WZ1, 0), Pt(WU1, WZ1, 0), Pt(WU1, WZ1, D), Pt(WU0, WZ1, D),
					FVector3f(0, 0, -1));
				StoneQuad(Pt(WU0, WZ0, 0), Pt(WU1, WZ0, 0), Pt(WU1, WZ0, D), Pt(WU0, WZ0, D),
					FVector3f(0, 0, 1));
				// Appui saillant : dessus, face avant, dessous (visible en vol drone).
				const float S = 8.f, E = 6.f;
				StoneQuad(Pt(WU0, WZ0, -S), Pt(WU1, WZ0, -S), Pt(WU1, WZ0, 0), Pt(WU0, WZ0, 0),
					FVector3f(0, 0, 1));
				StoneQuad(Pt(WU0, WZ0 - E, -S), Pt(WU1, WZ0 - E, -S), Pt(WU1, WZ0, -S), Pt(WU0, WZ0, -S),
					Nout);
				StoneQuad(Pt(WU0, WZ0 - E, -S), Pt(WU1, WZ0 - E, -S), Pt(WU1, WZ0 - E, 0), Pt(WU0, WZ0 - E, 0),
					FVector3f(0, 0, -1));
				// Linteau saillant : face avant + sous-face.
				const float L = 6.f, LH = 12.f;
				StoneQuad(Pt(WU0, WZ1, -L), Pt(WU1, WZ1, -L), Pt(WU1, WZ1 + LH, -L), Pt(WU0, WZ1 + LH, -L),
					Nout);
				StoneQuad(Pt(WU0, WZ1, -L), Pt(WU1, WZ1, -L), Pt(WU1, WZ1, 0), Pt(WU0, WZ1, 0),
					FVector3f(0, 0, -1));
			};

			const float Margin = 40.f;
			const float Usable = Len - 2.f * Margin;
			const int32 Bays = Usable > 200.f ? FMath::Max(1, FMath::RoundToInt32(Usable / 280.f)) : 0;
			// Socle enterre (desktop drape) : mur aveugle sous le rez-de-chaussee.
			if (SocleDepthCm >= 1.f)
			{
				WallQuad(0.f, Len, -SocleDepthCm, 0.f);
			}
			float Z0 = 0.f;
			for (int32 F = 0; F < Floors; ++F)
			{
				const float Z1 = Z0 + FloorH;
				if (Bays == 0 || FloorH < 220.f)
				{
					WallQuad(0.f, Len, Z0, Z1);
					Z0 = Z1;
					continue;
				}
				WallQuad(0.f, Margin, Z0, Z1);
				WallQuad(Len - Margin, Len, Z0, Z1);
				const float BayW = Usable / Bays;
				for (int32 B = 0; B < Bays; ++B)
				{
					const float BU0 = Margin + B * BayW;
					const float BU1 = BU0 + BayW;
					const float WinW = BayW * 0.5f;
					const float WinH = FloorH * (F == 0 ? 0.60f : 0.45f);
					const float WU0 = (BU0 + BU1 - WinW) * 0.5f;
					const float WU1 = WU0 + WinW;
					const float WZ0 = F == 0 ? Z0 + 25.f : Z0 + (FloorH - WinH) * 0.5f;
					const float WZ1 = WZ0 + WinH;
					WallQuad(BU0, WU0, Z0, Z1);
					WallQuad(WU1, BU1, Z0, Z1);
					WallQuad(WU0, WU1, Z0, WZ0);
					WallQuad(WU0, WU1, WZ1, Z1);
					Window(WU0, WU1, WZ0, WZ1);
				}
				Z0 = Z1;
			}
		}

		// Toit plat : sous-tuile toit terre cuite (10), quasi blanc en vertex color
		// (la tuile porte la couleur), UV0 = emprise normalisee dans la sous-tuile.
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		FBox2D RoofBox(ForceInit);
		for (const FVector2D& P : PtsCm)
		{
			RoofBox += P;
		}
		const FVector2D RoofSize = RoofBox.GetSize();
		const FVector3f RoofTint(0.95f, 0.95f, 0.95f);
		const FVector3f Up(0, 0, 1);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			FVector3f R[3];
			FVector2f RUV[3];
			for (int32 i = 0; i < 3; ++i)
			{
				const FVector2D& P2 = PtsCm[Tris[t + i]];
				R[i] = FVector3f(P2.X, P2.Y, ZBaseCm + Hcm);
				RUV[i] = AtlasUV(10,
					(float)((P2.X - RoofBox.Min.X) / FMath::Max(RoofSize.X, 1.0)),
					(float)((P2.Y - RoofBox.Min.Y) / FMath::Max(RoofSize.Y, 1.0)));
			}
			Wall.AddPoly(Wall.WallGroup, R, 3, Up, RUV, Col(RoofTint, Up, Hcm));
		}
	}

	// Batiment a facades TEXTUREES : un seul quad par mur, la grille de fenetres est
	// dans la texture (UV = travees x etages). ~x8-10 moins de triangles que la
	// version geometrique — budget Adreno 512 ~1,5 M tris/image. Les toits vont sur
	// le slot Glass (materiau uni) pour ne pas recevoir la texture de fenetres.
	// Desktop (J2 §3.3) : ZBaseCm pose le rez-de-chaussee a MinAlt du terrain sous
	// l'emprise (rebase Capitole) et SocleDepthCm prolonge le mur en socle aveugle
	// enterre jusqu'a ZBase - (MaxAlt - MinAlt) - SocleCm : aucun coin ne flotte en
	// pente. Mobile : 0/0, geometrie bit-a-bit identique a l'historique.
	void BuildPolygonBuildingTextured(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm,
		float Hcm, const FVector3f& Tint, float ZBaseCm = 0.f, float SocleDepthCm = 0.f)
	{
		const int32 Floors = FMath::Clamp(FMath::RoundToInt32(Hcm / 290.f), 1, 40);
		const float FloorH = Hcm / Floors;
		const int32 N = PtsCm.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = PtsCm[e];
			const FVector2D B2 = PtsCm[(e + 1) % N];
			const FVector2D Dir2 = B2 - A2;
			const float Len = Dir2.Size();
			if (Len < 30.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			const FVector3f Nout(T2.Y, -T2.X, 0.f);
			const float Usable = Len - 80.f;
			const int32 Bays = (Usable > 200.f && FloorH >= 220.f)
				? FMath::Max(1, FMath::RoundToInt32(Usable / 280.f)) : 0;
			const FVector3f P[4] = {
				FVector3f(A2.X, A2.Y, ZBaseCm), FVector3f(B2.X, B2.Y, ZBaseCm),
				FVector3f(B2.X, B2.Y, ZBaseCm + Hcm), FVector3f(A2.X, A2.Y, ZBaseCm + Hcm) };
			// Mur aveugle : UV constante dans un coin 100 % mur de la tuile.
			FVector2f UV[4] = { FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f),
				FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f) };
			if (Bays > 0)
			{
				UV[0] = FVector2f(0, 0);
				UV[1] = FVector2f(Bays, 0);
				UV[2] = FVector2f(Bays, Floors);
				UV[3] = FVector2f(0, Floors);
			}
			QM.AddPoly(QM.WallGroup, P, 4, Nout, UV, Shade(Tint, Nout, Hcm * 0.5f));
			// Socle enterre (desktop) : mur aveugle sous le rez-de-chaussee.
			if (SocleDepthCm >= 1.f)
			{
				const FVector3f S[4] = {
					FVector3f(A2.X, A2.Y, ZBaseCm - SocleDepthCm), FVector3f(B2.X, B2.Y, ZBaseCm - SocleDepthCm),
					FVector3f(B2.X, B2.Y, ZBaseCm), FVector3f(A2.X, A2.Y, ZBaseCm) };
				const FVector2f SocleUV[4] = { FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f),
					FVector2f(0.03f, 0.03f), FVector2f(0.03f, 0.03f) };
				QM.AddPoly(QM.WallGroup, S, 4, Nout, SocleUV, Shade(Tint * 0.85f, Nout, 0.f));
			}
		}
		// Toit plat : slot Glass = materiau uni (M_BldgWall), pas de texture fenetres.
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		const FVector3f RoofShaded = Shade(FVector3f(0.42f, 0.40f, 0.38f), FVector3f(0, 0, 1), Hcm);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			const FVector3f R[3] = {
				FVector3f(PtsCm[Tris[t]].X, PtsCm[Tris[t]].Y, ZBaseCm + Hcm),
				FVector3f(PtsCm[Tris[t + 1]].X, PtsCm[Tris[t + 1]].Y, ZBaseCm + Hcm),
				FVector3f(PtsCm[Tris[t + 2]].X, PtsCm[Tris[t + 2]].Y, ZBaseCm + Hcm) };
			const FVector2f RUV[3] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1) };
			QM.AddPoly(QM.GlassGroup, R, 3, FVector3f(0, 0, 1), RUV, RoofShaded);
		}
	}

	// Boite proxy : contour simplifie (>= 3 m entre points, plafond 12) et retracte de
	// 2 m vers le centroide, murs pleins + toit plat abaisse de 2,5 m. La version
	// detaillee du batiment recouvre le proxy -> il disparait dedans quand le bloc de
	// detail est charge. Retrait 30 cm au depart : z-fight VISIBLE sur device (toits
	// qui clignotent vus d'altitude a 1-2 km, la precision depth ne separe plus 30 cm).
	// ZBaseCm / SocleDepthCm : meme pose desktop que le batiment detaille (le proxy
	// doit rester CONTENU dans le detail pour disparaitre dedans) ; 0/0 en mobile.
	// bBakedShade=false (desktop PBR) : teinte brute, Lumen eclaire (cf. BuildRoad).
	void BuildProxyBuilding(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float Hcm,
		const FVector3f& Tint, float ZBaseCm = 0.f, float SocleDepthCm = 0.f,
		bool bBakedShade = true)
	{
		// RECTANGLE ORIENTE (10 tris par batiment contre ~28 pour le contour simplifie,
		// « tout garder, moins cher ») : axe = plus longue arete de l'emprise,
		// projection min/max sur (axe, perpendiculaire), puis retrait de 2 m.
		if (PtsCm.Num() < 3)
		{
			return;
		}
		FVector2D Axis(1, 0);
		float BestLen = 0.f;
		for (int32 i = 0; i < PtsCm.Num(); ++i)
		{
			const FVector2D E = PtsCm[(i + 1) % PtsCm.Num()] - PtsCm[i];
			const float L = E.SquaredLength();
			if (L > BestLen)
			{
				BestLen = L;
				Axis = E.GetSafeNormal();
			}
		}
		const FVector2D Perp(-Axis.Y, Axis.X);
		float MinU = FLT_MAX, MaxU = -FLT_MAX, MinV = FLT_MAX, MaxV = -FLT_MAX;
		for (const FVector2D& Q : PtsCm)
		{
			const float U = FVector2D::DotProduct(Q, Axis);
			const float V = FVector2D::DotProduct(Q, Perp);
			MinU = FMath::Min(MinU, U); MaxU = FMath::Max(MaxU, U);
			MinV = FMath::Min(MinV, V); MaxV = FMath::Max(MaxV, V);
		}
		TArray<FVector2D> P;
		const float BoxArea = (MaxU - MinU) * (MaxV - MinV);
		const float FootprintArea = FMath::Abs(float(SignedArea(PtsCm)));
		if (BoxArea < 1.f || FootprintArea / BoxArea >= 0.68f)
		{
			// Emprise compacte : rectangle oriente, retrait 2 m par cote (plafonne au
			// tiers de la dimension pour les petites emprises).
			const float InsetU = FMath::Min(200.f, (MaxU - MinU) / 3.f);
			const float InsetV = FMath::Min(200.f, (MaxV - MinV) / 3.f);
			P.Add(Axis * (MinU + InsetU) + Perp * (MinV + InsetV));
			P.Add(Axis * (MaxU - InsetU) + Perp * (MinV + InsetV));
			P.Add(Axis * (MaxU - InsetU) + Perp * (MaxV - InsetV));
			P.Add(Axis * (MinU + InsetU) + Perp * (MaxV - InsetV));
		}
		else
		{
			// Emprise concave/composite (< 68 % de sa boite) : le rectangle creait des
			// DALLES GEANTES qui debordaient et clignotaient contre le detail (vu en
			// v12) -> contour simplifie (>= 3 m, plafond 12 pts) retracte de 2 m.
			for (const FVector2D& Pt : PtsCm)
			{
				if (P.Num() == 0 || FVector2D::Distance(P.Last(), Pt) >= 300.f)
				{
					P.Add(Pt);
				}
			}
			if (P.Num() > 12)
			{
				TArray<FVector2D> Thin;
				const int32 Step = FMath::DivideAndRoundUp(P.Num(), 12);
				for (int32 i = 0; i < P.Num(); i += Step)
				{
					Thin.Add(P[i]);
				}
				P = MoveTemp(Thin);
			}
			if (P.Num() < 3)
			{
				P = PtsCm;
			}
			FVector2D Ctr(0, 0);
			for (const FVector2D& Q : P) { Ctr += Q; }
			Ctr /= P.Num();
			for (FVector2D& Q : P)
			{
				const FVector2D D = Ctr - Q;
				const float L = D.Size();
				if (L > 60.f)
				{
					Q += D / L * FMath::Min(200.f, L * 0.5f);
				}
			}
		}
		if (SignedArea(P) < 0)
		{
			Algo::Reverse(P);
		}
		const float H = FMath::Max(Hcm - 250.f, 100.f);
		const float Z0 = ZBaseCm - SocleDepthCm;
		const float Z1 = ZBaseCm + H;
		const int32 N = P.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = P[e], B2 = P[(e + 1) % N];
			const FVector2D Dir2 = B2 - A2;
			const float Len = Dir2.Size();
			if (Len < 30.f)
			{
				continue;
			}
			const FVector2D T2 = Dir2 / Len;
			const FVector3f Nout(T2.Y, -T2.X, 0.f);
			QM.AddQuad(QM.WallGroup, FVector3f(A2.X, A2.Y, Z0), FVector3f(B2.X, B2.Y, Z0),
				FVector3f(B2.X, B2.Y, Z1), FVector3f(A2.X, A2.Y, Z1), Nout,
				bBakedShade ? Shade(Tint, Nout, H * 0.5f) : Tint);
		}
		TArray<int32> Tris;
		TriangulateRing(P, Tris);
		const FVector3f RoofBase(0.42f, 0.40f, 0.38f);
		const FVector3f RoofShaded = bBakedShade ? Shade(RoofBase, FVector3f(0, 0, 1), H) : RoofBase;
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			QM.AddTri(QM.WallGroup,
				FVector3f(P[Tris[t]].X, P[Tris[t]].Y, Z1),
				FVector3f(P[Tris[t + 1]].X, P[Tris[t + 1]].Y, Z1),
				FVector3f(P[Tris[t + 2]].X, P[Tris[t + 2]].Y, Z1),
				FVector3f(0, 0, 1), RoofShaded);
		}
	}

	// Prisme de collision FERME d'un batiment (Verrou 2) : emprise triangulee en
	// plancher ET toit + un quad lateral par arete, aux Z EXACTS de la pose Lot A
	// (toit = ZBase + h, pied = ZBase - socle). ~30 tris par batiment. Raison
	// d'etre : les murs Nanite ne doivent JAMAIS servir de collision — leur
	// fallback decime (~0,1 %) laisse les facades traversables (sonde 2026-07-25).
	// Collision pure : ni teinte utile, ni UV, jamais rendu (pattern _Col du sol).
	void BuildCollisionPrism(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm,
		float TopZCm, float BottomZCm)
	{
		const FVector3f White(1.f, 1.f, 1.f);
		const int32 N = PtsCm.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = PtsCm[e];
			const FVector2D B2 = PtsCm[(e + 1) % N];
			if ((B2 - A2).Size() < 1.0)
			{
				continue;
			}
			const FVector2D T2 = (B2 - A2).GetSafeNormal();
			const FVector3f Nout(T2.Y, -T2.X, 0.f);
			QM.AddQuad(QM.WallGroup,
				FVector3f(A2.X, A2.Y, BottomZCm), FVector3f(B2.X, B2.Y, BottomZCm),
				FVector3f(B2.X, B2.Y, TopZCm), FVector3f(A2.X, A2.Y, TopZCm), Nout, White);
		}
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			const FVector2D& P0 = PtsCm[Tris[t]];
			const FVector2D& P1 = PtsCm[Tris[t + 1]];
			const FVector2D& P2 = PtsCm[Tris[t + 2]];
			QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, TopZCm), FVector3f(P1.X, P1.Y, TopZCm),
				FVector3f(P2.X, P2.Y, TopZCm), FVector3f(0, 0, 1), White);
			QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, BottomZCm), FVector3f(P1.X, P1.Y, BottomZCm),
				FVector3f(P2.X, P2.Y, BottomZCm), FVector3f(0, 0, -1), White);
		}
	}

	// Coeur commun des deux outils de collision batiments : parse le JSON UNE fois,
	// construit les prismes par cellule (memes conventions que ImportCityStreamed :
	// pts x100, CCW, cellule du centroide, ZBase/socle drapes), cree chaque
	// SM_Bldg_<x>_<y>_Col (complex-as-simple, sans Nanite, materiau par defaut),
	// le cable en ComplexCollisionMesh du SM_Bldg_<x>_<y>_Wall s'il existe, et
	// sauvegarde les packages AU FIL DE L'EAU (un kill ne perd que la cellule en
	// cours). OnlyCell non nul = une seule cellule.
	bool GenerateBuildingCollisionForCells(const FString& JsonFilePath, const FString& AssetFolder,
		float CellSizeM, const FCityGenProfile& Profile, const FIntPoint* OnlyCell,
		TMap<FIntPoint, FCityBldgColSummary>& OutCells)
	{
		if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")))
		{
			RaiseError(TEXT("AssetFolder must be a package path such as /Game/City."));
			return false;
		}
		if (CellSizeM < 20.f)
		{
			RaiseError(TEXT("CellSizeM must be >= 20."));
			return false;
		}
		const FCityGenProfile Gen = Profile.Resolved();
		FDrapeContext Drape;
		if (!MakeDrapeContext(Gen, Drape))
		{
			return false;
		}
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
		{
			RaiseError(FString::Printf(TEXT("Cannot read district file '%s'."), *JsonFilePath));
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
		{
			RaiseError(TEXT("District file is not valid JSON."));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* BuildingsJson = nullptr;
		if (!Root->TryGetArrayField(TEXT("buildings"), BuildingsJson) || BuildingsJson->Num() == 0)
		{
			RaiseError(TEXT("District file has no 'buildings' array."));
			return false;
		}

		const float Cell = CellSizeM * 100.f;
		TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> ColCells;
		TMap<FIntPoint, int32> CellBuildings;
		for (const TSharedPtr<FJsonValue>& V : *BuildingsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			for (const TSharedPtr<FJsonValue>& PV : O->GetArrayField(TEXT("pts")))
			{
				const TArray<TSharedPtr<FJsonValue>>& C = PV->AsArray();
				if (C.Num() >= 2)
				{
					Pts.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
				}
			}
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			FVector2D Centroid(0, 0);
			for (const FVector2D& P : Pts) { Centroid += P; }
			Centroid /= Pts.Num();
			const FIntPoint Key(FMath::FloorToInt(Centroid.X / Cell), FMath::FloorToInt(Centroid.Y / Cell));
			if (OnlyCell && Key != *OnlyCell)
			{
				continue;
			}
			const float Hcm = O->GetNumberField(TEXT("h")) * 100.f;
			float ZBase = 0.f, SocleDepth = 0.f;
			if (Drape.IsActive())
			{
				const float MinAlt = Drape.Sampler->MinAltCmInPolygon(Pts);
				const float MaxAlt = Drape.Sampler->MaxAltCmInPolygon(Pts);
				ZBase = MinAlt - Drape.AltCapCm;
				SocleDepth = (MaxAlt - MinAlt) + Gen.SocleCm;
			}
			TUniquePtr<FCityMeshBuilder>& Builder = ColCells.FindOrAdd(Key);
			if (!Builder)
			{
				Builder = MakeUnique<FCityMeshBuilder>();
			}
			BuildCollisionPrism(*Builder, Pts, ZBase + Hcm, ZBase - SocleDepth);
			++CellBuildings.FindOrAdd(Key);
		}
		if (ColCells.Num() == 0)
		{
			RaiseError(OnlyCell
				? FString::Printf(TEXT("No building centroid in cell (%d, %d)."), OnlyCell->X, OnlyCell->Y)
				: TEXT("No usable building footprint in the district file."));
			return false;
		}

		UMaterialInterface* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
		int32 Done = 0;
		for (auto& Pair : ColCells)
		{
			const FString Name = FString::Printf(TEXT("SM_Bldg_%d_%d"), Pair.Key.X, Pair.Key.Y);
			UStaticMesh* ColMesh = CreateMeshAsset(AssetFolder / (Name + TEXT("_Col")), *Pair.Value,
				DefaultMat, DefaultMat, true);
			// Attendre la compilation du _Col avant cablage/sauvegarde (meme piege que
			// le sol : le cook physique lit sa SectionInfoMap depuis un worker).
			FStaticMeshCompilingManager::Get().FinishCompilation(
				TArrayView<UStaticMesh* const>(&ColMesh, 1));
			FCityBldgColSummary& Out = OutCells.FindOrAdd(Pair.Key);
			Out.Buildings = CellBuildings[Pair.Key];
			const FMeshDescription* ColDesc = ColMesh->GetMeshDescription(0);
			Out.Triangles = ColDesc ? ColDesc->Triangles().Num() : 0;

			TArray<UPackage*> ToSave = { ColMesh->GetPackage() };
			const FString WallName = Name + TEXT("_Wall");
			UStaticMesh* Wall = LoadObject<UStaticMesh>(nullptr,
				*(AssetFolder / WallName + TEXT(".") + WallName), nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (Wall)
			{
				Wall->Modify();
				Wall->ComplexCollisionMesh = ColMesh;
				if (!Wall->GetBodySetup())
				{
					Wall->CreateBodySetup();
				}
				Wall->GetBodySetup()->CollisionTraceFlag = CTF_UseComplexAsSimple;
				// GUID physique regenere : le trimesh cuit (DDC) doit repartir du _Col,
				// pas du fallback Nanite decime. Pas de Build() complet : la geometrie de
				// rendu du Wall ne change pas, seule la source de collision change.
				Wall->GetBodySetup()->InvalidatePhysicsData();
				Wall->MarkPackageDirty();
				ToSave.Add(Wall->GetPackage());
				Out.bWallWired = true;
			}
			// Sauvegarde FORCEE (bOnlyDirty=false) au fil de l'eau.
			Out.bSaved = UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty=*/false);
			++Done;
			UE_LOG(LogCityImport, Display,
				TEXT("Collision batiments %d/%d : %s_Col — %d batiments, %d tris, wall %s, save %s"),
				Done, ColCells.Num(), *Name, Out.Buildings, Out.Triangles,
				Out.bWallWired ? TEXT("cable") : TEXT("ABSENT"), Out.bSaved ? TEXT("OK") : TEXT("ECHEC"));
		}
		return true;
	}
}

FCityStreamedSummary UCityImportTools::ImportCityStreamed(const FString& JsonFilePath,
	const FString& SurfacesJsonFilePath, const FString& AssetFolder, const FString& BlocksFolder,
	const FString& WallMaterialPath, const FString& GlassMaterialPath, float CellSizeM,
	float BlockSizeM, float ProxyCellSizeM, FVector Location, const FCityGenProfile& Profile)
{
	FCityStreamedSummary Summary;

	// Profil effectif + MNT charge UNE fois pour tout l'import (jalon J2).
	const FCityGenProfile Gen = Profile.Resolved();
	FDrapeContext Drape;
	if (!MakeDrapeContext(Gen, Drape))
	{
		return Summary;
	}

	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *JsonFilePath))
	{
		RaiseError(FString::Printf(TEXT("Cannot read district file '%s'."), *JsonFilePath));
		return Summary;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		RaiseError(TEXT("District file is not valid JSON."));
		return Summary;
	}
	if (AssetFolder.IsEmpty() || !AssetFolder.StartsWith(TEXT("/")) ||
		BlocksFolder.IsEmpty() || !BlocksFolder.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetFolder and BlocksFolder must be package paths such as /Game/City."));
		return Summary;
	}
	if (CellSizeM < 20.f || ProxyCellSizeM < 20.f || BlockSizeM < CellSizeM)
	{
		RaiseError(TEXT("Cell sizes must be >= 20 and BlockSizeM >= CellSizeM."));
		return Summary;
	}
	UMaterialInterface* WallMat = LoadMaterialOrDefault(WallMaterialPath);
	UMaterialInterface* GlassMat = LoadMaterialOrDefault(GlassMaterialPath);
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!WallMat || !GlassMat || !World)
	{
		if (!World)
		{
			RaiseError(TEXT("No editor world is loaded."));
		}
		return Summary;
	}
	UEditorLevelUtils::MakeLevelCurrent(World->PersistentLevel, true);

	// Lot B : bascule matiere desktop. Le chemin batiments GEOMETRIQUES est pris des
	// qu'un flag Lot B batiments est actif ; bPBRMaterials commande les materiaux
	// DefaultLit, l'encodage LINEAIRE des vertex colors, la fin de l'ombrage cuit
	// Shade() (Lumen eclaire) et l'UV1 monde (sol, routes, toits — ortho-ready J3).
	const bool bDesktopBldg = Gen.bWindowReveals || Gen.bSplitWallGlass || Gen.bPBRMaterials;
	const bool bLinearColors = Gen.bPBRMaterials;
	const bool bBakedShade = !Gen.bPBRMaterials;
	const bool bWorldUVs = Gen.bPBRMaterials;
	const bool bNanite = Gen.bNanite;

	// Idempotence : couches precedentes + heritage monolithique SM_City_*.
	TArray<AActor*> ToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const FString L = It->GetActorLabel();
		if (L.StartsWith(TEXT("SM_Ground_")) || L.StartsWith(TEXT("SM_Slab_")) ||
			L.StartsWith(TEXT("SM_Proxy_")) ||
			L.StartsWith(TEXT("SM_City_")) || L.StartsWith(TEXT("SM_Bldg_")) || L == TEXT("CityTrees"))
		{
			ToDestroy.Add(*It);
		}
	}
	for (AActor* A : ToDestroy)
	{
		World->DestroyActor(A);
	}

	// Sous-niveaux existants du dossier : reutilises (vides par la purge ci-dessus si
	// charges — les acteurs SM_Bldg_* d'un niveau charge sont vus par TActorIterator).
	TMap<FIntPoint, ULevelStreaming*> Blocks;
	for (ULevelStreaming* S : World->GetStreamingLevels())
	{
		const FString Pkg = S->GetWorldAssetPackageName();
		if (!Pkg.StartsWith(BlocksFolder))
		{
			continue;
		}
		FString Tail = FPackageName::GetShortName(Pkg);
		FString Sx, Sy;
		if (Tail.RemoveFromStart(TEXT("L_T10_B_")) && Tail.Split(TEXT("_"), &Sx, &Sy))
		{
			Blocks.Add(FIntPoint(FCString::Atoi(*Sx), FCString::Atoi(*Sy)), S);
		}
	}

	const float Cell = CellSizeM * 100.f;
	const float ProxyCell = ProxyCellSizeM * 100.f;
	const float Block = BlockSizeM * 100.f;
	TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>> BldgCells, BldgGlassCells, BldgColCells, GroundCells, ProxyCells;
	TSet<FIntPoint> SlabKeys;
	// bLinear / bUV1 (Lot B) : appliques a la CREATION du builder de cellule —
	// encodage lineaire des couleurs et canal UV1 monde. Defauts = mobile inchange.
	auto GetIn = [](TMap<FIntPoint, TUniquePtr<FCityMeshBuilder>>& Map, const FVector2D& P,
		float Size, bool bLinear = false, bool bUV1 = false) -> FCityMeshBuilder&
	{
		const FIntPoint Key(FMath::FloorToInt(P.X / Size), FMath::FloorToInt(P.Y / Size));
		TUniquePtr<FCityMeshBuilder>& B = Map.FindOrAdd(Key);
		if (!B)
		{
			B = MakeUnique<FCityMeshBuilder>();
			B->bLinearColors = bLinear;
			if (bUV1)
			{
				B->EnableWorldUV1();
			}
		}
		return *B;
	};
	auto ReadPts = [](const TArray<TSharedPtr<FJsonValue>>& In, TArray<FVector2D>& Out)
	{
		for (const TSharedPtr<FJsonValue>& V : In)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() >= 2)
			{
				Out.Add(FVector2D(C[0]->AsNumber() * 100.0, C[1]->AsNumber() * 100.0));
			}
		}
	};

	// --- Batiments : detail (cellules) + proxy (grandes cellules), meme teinte ---
	const TArray<TSharedPtr<FJsonValue>>* BuildingsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("buildings"), BuildingsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *BuildingsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 3)
			{
				continue;
			}
			if (SignedArea(Pts) < 0)
			{
				Algo::Reverse(Pts);
			}
			FVector2D Centroid(0, 0);
			for (const FVector2D& P : Pts) { Centroid += P; }
			Centroid /= Pts.Num();
			const float Hcm = O->GetNumberField(TEXT("h")) * 100.f;
			const FString Usage = O->GetStringField(TEXT("u"));
			const FVector3f Tint = UsageTint(Usage, Index);
			// Pose desktop (J2 §3.3) : rez-de-chaussee a MinAlt sous l'emprise
			// (rebase Capitole), socle aveugle jusqu'a ZBase - (Max - Min) - SocleCm.
			float ZBase = 0.f, SocleDepth = 0.f;
			if (Drape.IsActive())
			{
				const float MinAlt = Drape.Sampler->MinAltCmInPolygon(Pts);
				const float MaxAlt = Drape.Sampler->MaxAltCmInPolygon(Pts);
				ZBase = MinAlt - Drape.AltCapCm;
				SocleDepth = (MaxAlt - MinAlt) + Gen.SocleCm;
			}
			if (bDesktopBldg)
			{
				// Lot B : fenetres geometriques (en creux si bWindowReveals) ; les
				// vitres partent dans un builder SEPARE si bSplitWallGlass (Q3).
				FCityMeshBuilder& WallB = GetIn(BldgCells, Centroid, Cell, bLinearColors, bWorldUVs);
				FCityMeshBuilder& GlassB = Gen.bSplitWallGlass
					? GetIn(BldgGlassCells, Centroid, Cell, bLinearColors, false) : WallB;
				BuildPolygonBuildingDesktop(WallB, GlassB, Pts, Hcm, Tint, UsageTile(Usage, Index),
					ZBase, SocleDepth, Gen.bWindowReveals, bBakedShade);
				// Verrou 2 : prisme de collision dedie, meme pose — les murs Nanite ne
				// servent JAMAIS de collision (fallback decime = facades traversables).
				BuildCollisionPrism(GetIn(BldgColCells, Centroid, Cell), Pts,
					ZBase + Hcm, ZBase - SocleDepth);
			}
			else
			{
				BuildPolygonBuildingTextured(GetIn(BldgCells, Centroid, Cell), Pts, Hcm, Tint,
					ZBase, SocleDepth);
			}
			BuildProxyBuilding(GetIn(ProxyCells, Centroid, ProxyCell, bLinearColors), Pts, Hcm, Tint,
				ZBase, SocleDepth, bBakedShade);
			SlabKeys.Add(FIntPoint(FMath::FloorToInt(Centroid.X / Cell), FMath::FloorToInt(Centroid.Y / Cell)));
			++Summary.Buildings;
			++Index;
		}
	}

	// --- Routes : couche sol residente ---
	const TArray<TSharedPtr<FJsonValue>>* RoadsJson = nullptr;
	if (Root->TryGetArrayField(TEXT("roads"), RoadsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RoadsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			if (Pts.Num() < 2)
			{
				continue;
			}
			// Desktop (J2 §3.2) : re-echantillonnage a pas fixe puis drapage MNT,
			// l'empilement 55+ devient relatif au terrain. Champ optionnel bridge
			// (lu TOLERANT, absent = false) : tablier interpole entre les culees.
			const TArray<FVector2D>* RoadPts = &Pts;
			TArray<FVector2D> Resampled;
			TArray<float> TerrainZ;
			const TArray<float>* TerrainZPtr = nullptr;
			if (Drape.IsActive())
			{
				if (Gen.RoadResampleStepCm > 0.f)
				{
					Resampled = ResamplePolyline(Pts, Gen.RoadResampleStepCm);
					RoadPts = &Resampled;
				}
				bool bBridge = false;
				O->TryGetBoolField(TEXT("bridge"), bBridge);
				ComputePolylineZ(*RoadPts, Drape, bBridge, TerrainZ);
				TerrainZPtr = &TerrainZ;
			}
			BuildRoad(GetIn(GroundCells, Pts[0], Cell, bLinearColors, bWorldUVs), *RoadPts,
				O->GetNumberField(TEXT("w")) * 100.f,
				O->GetStringField(TEXT("t")), Index, TerrainZPtr, bBakedShade);
			SlabKeys.Add(FIntPoint(FMath::FloorToInt(Pts[0].X / Cell), FMath::FloorToInt(Pts[0].Y / Cell)));
			++Summary.Roads;
			++Index;
		}
	}

	// --- Dalles de sol PEINTES : grilles 12x12 par cellule, sommets teintes par
	// echantillonnage des surfaces (eau > bois > parc). Toujours residentes, elles
	// portent l'apparence de la map au-dela des distances de cull des films 3D —
	// une seule couche au loin = zero z-fight possible (precision depth GLES).
	TArray<FPaintPoly> PaintPolys;
	if (!SurfacesJsonFilePath.IsEmpty())
	{
		FString SurfJson;
		if (FFileHelper::LoadFileToString(SurfJson, *SurfacesJsonFilePath))
		{
			TSharedPtr<FJsonObject> SurfRoot;
			if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SurfJson), SurfRoot) &&
				SurfRoot.IsValid())
			{
				auto LoadPolys = [&](const TCHAR* Field, auto TintForKind)
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
					if (!SurfRoot->TryGetArrayField(Field, Arr))
					{
						return;
					}
					for (const TSharedPtr<FJsonValue>& V : *Arr)
					{
						const TSharedPtr<FJsonObject>& O = V->AsObject();
						FPaintPoly Poly;
						ReadPts(O->GetArrayField(TEXT("pts")), Poly.Pts);
						if (Poly.Pts.Num() < 3)
						{
							continue;
						}
						Poly.Bounds = FBox2D(ForceInit);
						for (const FVector2D& P : Poly.Pts) { Poly.Bounds += P; }
						TintForKind(O, Poly);
						PaintPolys.Add(MoveTemp(Poly));
					}
				};
				LoadPolys(TEXT("water"), [](const TSharedPtr<FJsonObject>&, FPaintPoly& P)
					{ P.Tint = FVector3f(0.16f, 0.30f, 0.38f); P.Priority = 3; });
				LoadPolys(TEXT("green"), [](const TSharedPtr<FJsonObject>& O, FPaintPoly& P)
					{
						const bool bForest = O->GetStringField(TEXT("k")) == TEXT("forest");
						P.Tint = bForest ? FVector3f(0.20f, 0.34f, 0.16f) : FVector3f(0.35f, 0.48f, 0.22f);
						P.Priority = bForest ? 2 : 1;
					});
			}
		}
	}
	const FVector3f SlabBase(0.33f, 0.31f, 0.28f);
	auto SampleGround = [&](const FVector2D& P) -> FVector3f
	{
		int32 BestPrio = 0;
		FVector3f Tint = SlabBase;
		for (const FPaintPoly& Poly : PaintPolys)
		{
			if (Poly.Priority > BestPrio && Poly.Bounds.IsInside(P) && PointInRing(Poly.Pts, P))
			{
				BestPrio = Poly.Priority;
				Tint = Poly.Tint;
			}
		}
		return Tint;
	};
	// Lot B : materiaux PBR DefaultLit en desktop — sol/sentiers/proxys en
	// M_CityGround_PBR (VertexColor lit), rubans en M_CityRoad_PBR (meme
	// T_RoadStrip). Mobile : materiaux unlit historiques, inchanges.
	UMaterialInterface* RoadMat = Gen.bPBRMaterials
		? GetOrCreateRoadPBRMaterial(AssetFolder) : GetOrCreateRoadMaterial(AssetFolder);
	UMaterialInterface* SlabMat = Gen.bPBRMaterials
		? GetOrCreateGroundPBRMaterial(AssetFolder) : WallMat;
	// Grille de sol parametree par le profil : 12x12 plat (mobile) ou 64x64 drape
	// MNT (desktop, sommets Z = AltCmAt - AltCapitole). Fabrique commune : la teinte
	// peinte reste echantillonnee en (X, Y), le Z ne change pas les couleurs.
	const int32 SlabGrid = FMath::Clamp(Gen.GroundGridN, 1, 256);
	auto BuildGroundGrid = [&](FCityMeshBuilder& Builder, const FIntPoint& Key, int32 GridN, bool bPaint)
	{
		const float Step = Cell / GridN;
		for (int32 GY = 0; GY < GridN; ++GY)
		{
			for (int32 GX = 0; GX < GridN; ++GX)
			{
				const float X0 = Key.X * Cell + GX * Step, Y0 = Key.Y * Cell + GY * Step;
				const FVector3f C[4] = {
					FVector3f(X0, Y0, Drape.GroundZ(X0, Y0)),
					FVector3f(X0 + Step, Y0, Drape.GroundZ(X0 + Step, Y0)),
					FVector3f(X0 + Step, Y0 + Step, Drape.GroundZ(X0 + Step, Y0 + Step)),
					FVector3f(X0, Y0 + Step, Drape.GroundZ(X0, Y0 + Step)) };
				const FVector2f UV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
				FVector3f Cols[4];
				for (int32 c = 0; c < 4; ++c)
				{
					const FVector3f Base = bPaint ? SampleGround(FVector2D(C[c].X, C[c].Y)) : SlabBase;
					Cols[c] = bBakedShade ? Shade(Base, FVector3f(0, 0, 1), 0.f) : Base;
				}
				Builder.AddPolyPerVertexColors(Builder.WallGroup, C, 4,
					FVector3f(0, 0, 1), UV, Cols);
			}
		}
	};
	for (const FIntPoint& Key : SlabKeys)
	{
		FCityMeshBuilder SlabBuilder;
		SlabBuilder.bLinearColors = bLinearColors;
		if (bWorldUVs)
		{
			SlabBuilder.EnableWorldUV1();
		}
		BuildGroundGrid(SlabBuilder, Key, SlabGrid, /*bPaint=*/true);
		const FString SlabName = FString::Printf(TEXT("SM_Slab_%d_%d"), Key.X, Key.Y);
		UStaticMesh* SlabMesh = nullptr;
		if (Drape.IsActive() && Gen.GroundCollisionGridN > 0)
		{
			// Collision desktop : la boite plate est fausse des que le terrain ondule
			// — trimesh BASSE RESOLUTION dedie (16x16), cuit a la place du rendu 64x64
			// via ComplexCollisionMesh (le trimesh plein serait du gachis memoire).
			// Le mesh de collision n'est jamais rendu : ni Nanite ni materiau PBR.
			FCityMeshBuilder ColBuilder;
			BuildGroundGrid(ColBuilder, Key, FMath::Clamp(Gen.GroundCollisionGridN, 1, 64), /*bPaint=*/false);
			UStaticMesh* ColMesh = CreateMeshAsset(AssetFolder / (SlabName + TEXT("_Col")), ColBuilder,
				WallMat, WallMat, true);
			SlabMesh = CreateMeshAsset(AssetFolder / SlabName, SlabBuilder, SlabMat, SlabMat,
				true, false, 0.f, ColMesh, bNanite);
		}
		else
		{
			SlabMesh = CreateMeshAsset(AssetFolder / SlabName, SlabBuilder, SlabMat, SlabMat,
				true, true, 60.f, nullptr, bNanite);
		}
		++Summary.GroundMeshes;
		AStaticMeshActor* SlabActor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		SlabActor->GetStaticMeshComponent()->SetStaticMesh(SlabMesh);
		SlabActor->SetActorLabel(SlabName);
	}

	// --- Rubans routiers : SANS collision (films visuels 55-80 cm au-dessus de la
	// dalle porteuse, dont la boite monte a 60 cm) et cullables a ~2 km cote runtime.
	// Slot Wall = sentiers (vertex color) ; slot Glass = rubans textures.
	for (auto& Pair : GroundCells)
	{
		const FString Name = FString::Printf(TEXT("SM_Ground_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, SlabMat, RoadMat, false,
			false, 0.f, nullptr, bNanite);
		++Summary.GroundMeshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Actor->SetActorLabel(Name);
	}
	for (auto& Pair : ProxyCells)
	{
		const FString Name = FString::Printf(TEXT("SM_Proxy_%d_%d"), Pair.Key.X, Pair.Key.Y);
		UStaticMesh* Mesh = CreateMeshAsset(AssetFolder / Name, *Pair.Value, SlabMat,
			Gen.bPBRMaterials ? SlabMat : GlassMat, false, false, 0.f, nullptr, bNanite);
		++Summary.ProxyMeshes;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->GetStaticMeshComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Actor->SetActorLabel(Name);
	}

	// --- Blocs de streaming : un sous-niveau par BlockSizeM, batiments detailles ---
	TMap<FIntPoint, TArray<FIntPoint>> CellsByBlock;
	for (auto& Pair : BldgCells)
	{
		const FIntPoint BlockKey(
			FMath::FloorToInt(Pair.Key.X * Cell / Block),
			FMath::FloorToInt(Pair.Key.Y * Cell / Block));
		CellsByBlock.FindOrAdd(BlockKey).Add(Pair.Key);
	}
	for (auto& Pair : CellsByBlock)
	{
		ULevelStreaming* Streaming = Blocks.FindRef(Pair.Key);
		if (!Streaming)
		{
			// CreateNewStreamingLevel attend un chemin de PACKAGE (il convertit lui-meme
			// en nom de fichier) — lui passer un fichier deja converti echoue en silence.
			const FString Pkg = FString::Printf(TEXT("%s/L_T10_B_%d_%d"), *BlocksFolder, Pair.Key.X, Pair.Key.Y);
			Streaming = UEditorLevelUtils::CreateNewStreamingLevel(
				ULevelStreamingDynamic::StaticClass(), Pkg, false);
			if (!Streaming)
			{
				RaiseError(FString::Printf(TEXT("Cannot create streaming level '%s'."), *Pkg));
				return Summary;
			}
			Blocks.Add(Pair.Key, Streaming);
		}
		if (ULevelStreamingDynamic* Dyn = Cast<ULevelStreamingDynamic>(Streaming))
		{
			Dyn->bInitiallyLoaded = false;
			Dyn->bInitiallyVisible = false;
		}
		ULevel* BlockLevel = Streaming->GetLoadedLevel();
		if (!BlockLevel)
		{
			World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
			BlockLevel = Streaming->GetLoadedLevel();
		}
		if (!BlockLevel)
		{
			RaiseError(FString::Printf(TEXT("Streaming level '%s' is not loaded in editor — open the map with all sublevels loaded and retry."),
				*Streaming->GetWorldAssetPackageName()));
			return Summary;
		}
		for (const FIntPoint& CellKey : Pair.Value)
		{
			const FString Name = FString::Printf(TEXT("SM_Bldg_%d_%d"), CellKey.X, CellKey.Y);
			auto SpawnBldg = [&](const FString& ActorName, UStaticMesh* Mesh)
			{
				++Summary.BuildingMeshes;
				FActorSpawnParameters SpawnParams;
				SpawnParams.OverrideLevel = BlockLevel;
				SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(
					AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, SpawnParams);
				Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
				Actor->SetActorLabel(ActorName);
			};
			if (bDesktopBldg)
			{
				// Lot B : murs geometriques (atlas PBR, Nanite si demande) ; vitres
				// dans un mesh SEPARE si bSplitWallGlass — Nanite AUSSI (J2e,
				// retours utilisateur du 25/07 : le verre est opaque, et des vitres
				// non-Nanite devant des murs Nanite « flottent » aux transitions
				// LOD), et AVEC collision (l'embrasure est un vrai trou, la vitre
				// doit arreter le drone). Cellule sans fenetre : pas de mesh Glass.
				UMaterialInterface* BldgWallMat = Gen.bPBRMaterials
					? GetOrCreateWallPBRMaterial(AssetFolder, Gen.AtlasSizePx) : WallMat;
				UMaterialInterface* BldgGlassMat = Gen.bPBRMaterials
					? GetOrCreateGlassPBRMaterial(AssetFolder) : GlassMat;
				// Verrou 2 : le mesh de collision dedie (prismes) est cree d'abord puis
				// cable en ComplexCollisionMesh du mesh opaque — pattern _Col du sol.
				// Sans lui, la collision du Wall Nanite viendrait du fallback decime.
				UStaticMesh* ColMesh = nullptr;
				if (TUniquePtr<FCityMeshBuilder>* ColB = BldgColCells.Find(CellKey))
				{
					UMaterialInterface* DefaultMat = UMaterial::GetDefaultMaterial(MD_Surface);
					ColMesh = CreateMeshAsset(AssetFolder / (Name + TEXT("_Col")), **ColB,
						DefaultMat, DefaultMat, true);
					// Finir sa compilation TOUT DE SUITE : a la regeneration en place, le
					// chargement du Wall deja cable sur disque peut survenir pendant un
					// flush de chargement — finir la compilation du _Col DEPUIS ce contexte
					// declenche l'ensure « Overriding GIsEditorLoadingPackage » (attrape
					// par le test sentinelle au 2e run).
					FStaticMeshCompilingManager::Get().FinishCompilation(
						TArrayView<UStaticMesh* const>(&ColMesh, 1));
					++Summary.BuildingColMeshes;
				}
				if (Gen.bSplitWallGlass)
				{
					SpawnBldg(Name + TEXT("_Wall"), CreateMeshAsset(AssetFolder / (Name + TEXT("_Wall")),
						*BldgCells[CellKey], BldgWallMat, BldgWallMat, true, false, 0.f, ColMesh, bNanite));
					TUniquePtr<FCityMeshBuilder>* GlassB = BldgGlassCells.Find(CellKey);
					if (GlassB && (*GlassB)->QuadCount > 0)
					{
						SpawnBldg(Name + TEXT("_Glass"), CreateMeshAsset(AssetFolder / (Name + TEXT("_Glass")),
							**GlassB, BldgGlassMat, BldgGlassMat, true, false, 0.f, nullptr, bNanite));
					}
				}
				else
				{
					SpawnBldg(Name, CreateMeshAsset(AssetFolder / Name, *BldgCells[CellKey],
						BldgWallMat, BldgGlassMat, true, false, 0.f, ColMesh, bNanite));
				}
			}
			else
			{
				// Facades texturees (slot Wall) + toits unis en vertex color (slot Glass).
				SpawnBldg(Name, CreateMeshAsset(AssetFolder / Name, *BldgCells[CellKey],
					GetOrCreateFacadeMaterial(AssetFolder), WallMat, true));
			}
		}
		BlockLevel->MarkPackageDirty();
		// Sauver le bloc TOUT DE SUITE puis le masquer : l'editeur ne cumule jamais la
		// ville entiere en VRAM (2 crashs GPU TerminateOnGPUCrash payes juste apres
		// l'import, fenetre minimisee comprise — l'upload des ressources + le premier
		// tick rendu suffisaient a tuer le device D3D12).
		FEditorFileUtils::SaveLevel(BlockLevel);
		UEditorLevelUtils::SetLevelVisibility(BlockLevel, false, false, ELevelVisibilityDirtyMode::DontModify);
		++Summary.StreamingBlocks;
	}
	UEditorLevelUtils::MakeLevelCurrent(World->PersistentLevel, true);

	// --- Arbres : residents, un mesh + un HISM (identique a ImportCityDistrict) ---
	const TArray<TSharedPtr<FJsonValue>>* TreesJson = nullptr;
	if (Root->TryGetArrayField(TEXT("trees"), TreesJson) && TreesJson->Num() > 0)
	{
		FCityMeshBuilder TreeBuilder;
		BuildTree(TreeBuilder);
		UStaticMesh* TreeMesh = CreateMeshAsset(AssetFolder / TEXT("SM_CityTree"), TreeBuilder, WallMat, GlassMat);
		WallMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);
		GlassMat->CheckMaterialUsage(MATUSAGE_InstancedStaticMeshes);

		AActor* TreeActor = World->SpawnActor<AActor>(Location, FRotator::ZeroRotator);
		USceneComponent* Root2 = NewObject<USceneComponent>(TreeActor, TEXT("Root"), RF_Transactional);
		TreeActor->SetRootComponent(Root2);
		TreeActor->AddInstanceComponent(Root2);
		Root2->RegisterComponent();
		Root2->SetWorldLocation(Location);
		UHierarchicalInstancedStaticMeshComponent* Hism =
			NewObject<UHierarchicalInstancedStaticMeshComponent>(TreeActor, TEXT("Trees"), RF_Transactional);
		Hism->SetStaticMesh(TreeMesh);
		Hism->SetupAttachment(Root2);
		TreeActor->AddInstanceComponent(Hism);
		Hism->RegisterComponent();
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *TreesJson)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() < 2)
			{
				continue;
			}
			const float Yaw = FMath::Frac(FMath::Sin(Index * 78.233f) * 12543.21f) * 360.f;
			const float Scale = 0.8f + 0.5f * FMath::Frac(FMath::Sin(Index * 39.11f) * 6543.87f);
			const double Tx = C[0]->AsNumber() * 100.0, Ty = C[1]->AsNumber() * 100.0;
			Hism->AddInstance(FTransform(FRotator(0, Yaw, 0),
				FVector(Tx, Ty, Drape.GroundZ(Tx, Ty)), FVector(Scale)));
			++Summary.Trees;
			++Index;
		}
		TreeActor->SetActorLabel(TEXT("CityTrees"));
	}

	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	// Sauvegarde generale AVANT de rendre la main : si l'editeur crashe au premier
	// tick rendu apres l'outil, tout est deja sur disque.
	FEditorFileUtils::SaveDirtyPackages(/*bPromptUserToSave=*/false, /*bSaveMapPackages=*/true,
		/*bSaveContentPackages=*/true);
	UE_LOG(LogCityImport, Display,
		TEXT("Ville streamee : %d batiments, %d routes, %d arbres — %d sols, %d proxys, %d meshes detail, %d collisions batiments, %d blocs. Tout est sauve."),
		Summary.Buildings, Summary.Roads, Summary.Trees, Summary.GroundMeshes, Summary.ProxyMeshes,
		Summary.BuildingMeshes, Summary.BuildingColMeshes, Summary.StreamingBlocks);
	return Summary;
}

FCityBldgColSummary UCityImportTools::GenerateBuildingCollisionCell(const FString& JsonFilePath,
	const FString& AssetFolder, float CellSizeM, int32 CellX, int32 CellY,
	const FCityGenProfile& Profile)
{
	FCityBldgColSummary Summary;
	const FIntPoint Cell(CellX, CellY);
	TMap<FIntPoint, FCityBldgColSummary> Cells;
	if (GenerateBuildingCollisionForCells(JsonFilePath, AssetFolder, CellSizeM, Profile, &Cell, Cells))
	{
		Summary = Cells.FindRef(Cell);
	}
	return Summary;
}

FCityBldgColBatchSummary UCityImportTools::GenerateBuildingCollisionAll(const FString& JsonFilePath,
	const FString& AssetFolder, float CellSizeM, const FCityGenProfile& Profile)
{
	FCityBldgColBatchSummary Summary;
	TMap<FIntPoint, FCityBldgColSummary> Cells;
	if (!GenerateBuildingCollisionForCells(JsonFilePath, AssetFolder, CellSizeM, Profile, nullptr, Cells))
	{
		return Summary;
	}
	for (const auto& Pair : Cells)
	{
		++Summary.Cells;
		Summary.Buildings += Pair.Value.Buildings;
		Summary.WiredWalls += Pair.Value.bWallWired ? 1 : 0;
		Summary.MissingWalls += Pair.Value.bWallWired ? 0 : 1;
	}
	UE_LOG(LogCityImport, Display,
		TEXT("Collision batiments : %d cellules, %d batiments, %d walls cables, %d walls absents."),
		Summary.Cells, Summary.Buildings, Summary.WiredWalls, Summary.MissingWalls);
	return Summary;
}
