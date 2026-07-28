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

		// J3c point 2 : groupes de polygones SUPPLEMENTAIRES nommes (un par classe de
		// revetement). Wall = slot 0, Glass = slot 1, puis les extras dans leur ordre
		// de creation — CreateMeshAsset ajoute un FStaticMaterial par extra, de meme
		// nom de slot, ce qui donne une section de mesh par classe. Un builder sans
		// extra produit exactement les deux slots historiques (golden path mobile).
		TArray<FName> ExtraSlotNames;
		TArray<UMaterialInterface*> ExtraSlotMaterials;
		TMap<FName, FPolygonGroupID> ExtraGroups;

		// Groupe d'une classe de revetement, cree a la PREMIERE utilisation (jamais de
		// slot vide). MatOrNull = materiau du pack ; nul -> CreateMeshAsset retombe sur
		// le materiau de repli du mesh.
		FPolygonGroupID GetOrCreateGroup(FName SlotName, UMaterialInterface* MatOrNull)
		{
			if (const FPolygonGroupID* Found = ExtraGroups.Find(SlotName))
			{
				return *Found;
			}
			const FPolygonGroupID Group = MeshDesc.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[Group] = SlotName;
			ExtraGroups.Add(SlotName, Group);
			ExtraSlotNames.Add(SlotName);
			ExtraSlotMaterials.Add(MatOrNull);
			return Group;
		}

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

	// -----------------------------------------------------------------------------
	// J3c point 2 « builder sols » : classes de revetement Megascans.
	//
	// Une classe = un pack scanne importe sous <SurfacesFolder>/<Slug>/M_Surf_<Slug>
	// (Tools/import_surfaces.py). Le materiau divise l'UV0 par la taille PHYSIQUE du
	// scan (lue au JSON du pack) : le generateur ecrit donc des UV0 EN METRES, et un
	// meme UV metrique donne la bonne echelle reelle quel que soit le pack.
	//   AcrossM    taille physique du scan EN TRAVERS de la route (m).
	//   bFullWidth la largeur ENTIERE du ruban est mappee sur AcrossM — reserve aux
	//              scans qui portent deja leur ligne axiale (fine_road_*, marked_*) :
	//              la ligne tombe alors exactement au milieu du ruban. Sinon la
	//              texture tuile aussi en travers (UV0.V = metres reels).
	//   bSwapUV    le scan est tourne de 90 deg (son axe « le long de la route » est
	//              V et non U) — verifie image par image, cf. fine_road_viciaalew.
	//   ZClassCm   v2 : offset d'empilement DETERMINISTE PAR CLASSE au-dessus du
	//              plancher des rubans (55 cm). L'ancien offset par ordre d'arrivee
	//              (RoadIndex % 7) faisait passer un trottoir SOUS une chaussee ici
	//              et AU-DESSUS la ou elle la recroisait : frontieres instables,
	//              « effet bacle » (verdict utilisateur v1). Ordre impose : gravier
	//              le plus bas, pieton le plus haut.
	//   bAuto      v3 : la classe est une CHAUSSEE AUTOMOBILE. Sert au filtre des
	//              patchs de carrefour (voir FJunctionNode) : un carrefour n'existe
	//              qu'entre voitures.
	// -----------------------------------------------------------------------------
	struct FSurfaceClass
	{
		const TCHAR* Slug;
		float AcrossM;
		bool bFullWidth;
		bool bSwapUV;
		float ZClassCm;
		bool bAuto;
	};

	const FSurfaceClass GSurfGravel{ TEXT("gravel_on_soil_okosdmp0"), 0.89f, false, false, 0.f, false };
	const FSurfaceClass GSurfGrassCut{ TEXT("grass_cut_pjxmz0"), 1.f, false, false, 2.f, false };
	const FSurfaceClass GSurfAsphalt{ TEXT("asphalt_road_tiggcjdo"), 2.f, false, false, 4.f, true };
	const FSurfaceClass GSurfRoadMedium{ TEXT("fine_road_viciaalew"), 4.f, true, true, 10.f, true };
	const FSurfaceClass GSurfRoadWide{ TEXT("fine_road_vgdlejpew"), 8.f, true, false, 13.f, true };
	const FSurfaceClass GSurfGrassUncut{ TEXT("uncut_grass_oilpt20"), 2.f, false, false, 2.f, false };
	const FSurfaceClass GSurfGrassWild{ TEXT("wild_grass_sfknaeoa"), 2.f, false, false, 2.f, false };
	// v4 — LA DALLE. Ce n'est pas un ruban : c'est le sol porteur de toute la ville,
	// pose au Z du terrain sous tout le reste. Verdict DA v3 : « grand puzzle » — la
	// dalle etait restee a la teinte unie blanc-bleu de J2, si bien que chaque ruban
	// texture ressemblait a un autocollant sur du papier et que les interstices entre
	// rubans laissaient voir le vide. Aucun reglage de palette ne rattrape un fond nu.
	// dirty_sidewalk_tiles : le scan mineral le plus NEUTRE et le plus CLAIR de la
	// bibliotheque une fois harmonise (0,0991) — il ne raconte rien, c'est ce qu'on
	// demande a un fond. ZClassCm ne sert pas (la dalle n'entre pas dans l'empilement
	// des rubans).
	const FSurfaceClass GSurfSlab{ TEXT("dirty_sidewalk_tiles_ugxjcdpn"), 2.f, false, false, 0.f, false };

	// -----------------------------------------------------------------------------
	// v5 « VOIRIE » (J3c point 3). Verdict utilisateur sur la v4b : « il manque la
	// structure des rues (rives) » — la ville lisait comme une esplanade continue
	// ou personne ne sait ou finit la chaussee. Le ruban de chaussee, qui couvrait
	// historiquement chaussee + 2 x 1,70 m d'un seul tenant, est RE-PARTITIONNE :
	//   bande centrale = classe chaussee ;
	//   bordure       = face verticale de 12 cm + chant horizontal de 15 cm ;
	//   bandes rives  = 1,70 m de classe DALLE (meme scan que le fond de ville :
	//                   le trottoir n'est PAS un revetement de plus, c'est le sol
	//                   de la ville que la bordure vient decoller de 12 cm).
	// C'est le relief de 12 cm qui donne la lecture de la rue : deux aretes eclairees
	// differemment de part et d'autre de la chaussee.
	// -----------------------------------------------------------------------------
	constexpr float GSidewalkWidthCm = 170.f; // rive, largeur historique du « trottoir »
	constexpr float GCurbHeightCm = 12.f;     // relief de la bordure
	constexpr float GCurbTopWidthCm = 15.f;   // chant horizontal, entre face et rive
	// Pas de sous-decoupe des quads lateraux, sauf au voisinage d'un patch de
	// carrefour ou la bordure doit s'interrompre au plus pres du disque.
	constexpr float GCurbClipStepCm = 200.f;

	// La BORDURE : meme matiere que la dalle, assombrie x0,92 (materiau derive
	// M_Surf_curb fabrique par Tools/import_surfaces.py a partir des textures du
	// pack de dalle). Un materiau dedie plutot qu'une teinte de sommet : les
	// M_Surf_* ne lisent PAS la VertexColor (BaseColor = scan x constante).
	const FSurfaceClass GSurfCurb{ TEXT("curb"), 2.f, false, false, 0.f, false };
	// PASSAGE PIETON. Le scan fait 4 x 2 m : son axe de 4 m porte la REPETITION des
	// bandes (8 bandes, pas de 50 cm) et son axe de 2 m leur LONGUEUR, avec un trait
	// blanc en travers a mi-hauteur. L'axe de 4 m part donc EN TRAVERS de la rue
	// (bandes de 50 cm paralleles a l'axe de la chaussee, norme francaise) et l'axe
	// de 2 m le long de la rue, cale pour que le trait blanc tombe exactement sur les
	// deux bords du passage. AcrossM/bFullWidth/ZClassCm ne servent pas : les UV du
	// passage sont calculees a la main dans BuildCrossing.
	const FSurfaceClass GSurfCrossing{ TEXT("pedestrian_crossing_lines_veggecd"), 4.f, false, false, 0.f, false };
	constexpr float GCrossingHalfLenCm = 200.f; // 4 m dans l'axe de la rue
	constexpr float GCrossingLiftCm = 9.f;      // au-dessus de la chaussee et du patch, sous le chant (12)

	// -----------------------------------------------------------------------------
	// J3c « MAQUETTE DU SOL » — LE SOL EST PEINT, LE RELIEF EST MAILLE.
	//
	// Le corridor cadastral etant desormais cuit en masques par cellule
	// (Tools/j3c_sols_masks.py), la chaussee n'est plus un ruban pose SUR la dalle :
	// elle EST la dalle, peinte par MI_CityGround_<x>_<y>. Ne reste en geometrie que
	// ce qu'un masque ne peut pas rendre :
	//   - la BORDURE, seule chose qui donne du relief a la rue (c'est elle qui prend
	//     la lumiere autrement que le sol) ;
	//   - le PASSAGE PIETON et les TIRETS axiaux, dont on veut le trait franc a
	//     n'importe quelle distance ;
	//   - les PONTS, qui restent les rubans/tabliers existants (un tablier ne se
	//     peint pas sur le terrain qu'il survole).
	// -----------------------------------------------------------------------------
	// Le materiau de la dalle masquee. Pas de scan : le master melange lui-meme les
	// quatre revetements d'apres le masque. AcrossM/bFullWidth/ZClassCm ne servent
	// pas — l'UV0 metrique monde de la dalle est deja exactement ce qu'il attend.
	const FSurfaceClass GSurfMaskedGround{ TEXT("ground_masked"), 1.f, false, false, 0.f, false };
	// La peinture blanche des tirets (Tools/import_ground_masks.py).
	const FSurfaceClass GSurfMarking{ TEXT("marking"), 1.f, false, false, 0.f, false };

	constexpr float GAxialWidthCm = 15.f;      // largeur d'un tiret de ligne axiale
	// Pied de bordure ENTERRE : la dalle est drapee sur le MNT par une grille de
	// 7,8 m de pas, la bordure suit le MNT continu — entre deux sommets, les deux
	// surfaces divergent de quelques centimetres. Enterrer le pied coute zero
	// triangle et supprime tout risque de jour sous la bordure.
	constexpr float GMaskCurbSinkCm = 10.f;
	constexpr float GMaskCrossLiftCm = 4.f;    // passage pieton au-dessus de la peinture
	constexpr float GMaskDashLiftCm = 6.f;     // tiret au-dessus du passage, sous le chant (12)

	// Le RELIEF d'une cellule, lu dans sols_<x>_<y>.json. Tout est deja decoupe au
	// prep (bordures orientees chaussee a gauche, tirets debites, passages
	// dedoublonnes) : ici on ne fait que poser des quads.
	struct FMaskCrossing
	{
		FVector2D PosCm = FVector2D::ZeroVector;
		FVector2D DirCm = FVector2D::ZeroVector;
		float HalfWCm = 0.f;
	};

	struct FGroundMaskCell
	{
		TArray<TArray<FVector2D>> Curbs;  // polylignes cm, chaussee A GAUCHE
		TArray<FMaskCrossing> Crossings;
		TArray<FVector4> Axial;           // (ax, ay, bx, by) en cm
	};

	FString GroundMasksDir(const FCityGenProfile& Gen)
	{
		return Gen.GroundMasksPath.IsEmpty()
			? FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/Sols"))
			: Gen.GroundMasksPath;
	}

	FString GroundMasksAssetDir(const FCityGenProfile& Gen)
	{
		return Gen.GroundMasksAssetFolder.IsEmpty()
			? FString(TEXT("/Game/City/Ground")) : Gen.GroundMasksAssetFolder;
	}

	// Rend false SANS erreur si la cellule n'a pas de masque : une cellule sans
	// masque garde le comportement actuel, c'est un mode de fonctionnement normal
	// (cuisson partielle, zone proto).
	bool LoadGroundMaskCell(const FString& Dir, int32 CellX, int32 CellY, float CellSizeM,
		FGroundMaskCell& Out)
	{
		const FString Path = FPaths::Combine(Dir, FString::Printf(TEXT("sols_%d_%d.json"), CellX, CellY));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			RaiseError(FString::Printf(TEXT("Ground mask file '%s' is not valid JSON."), *Path));
			return false;
		}
		// Le masque est cuit POUR une taille de cellule : le cuire a 500 m puis
		// generer a 250 m decalerait chaque masque d'une demi-cellule sans que rien
		// ne proteste. On refuse plutot que de peindre a cote.
		double BakedCellM = 0.0;
		if (Root->TryGetNumberField(TEXT("cellSizeM"), BakedCellM) &&
			!FMath::IsNearlyEqual((float)BakedCellM, CellSizeM, 0.01f))
		{
			RaiseError(FString::Printf(
				TEXT("Ground mask '%s' was baked for %.0f m cells but the import uses %.0f m."),
				*Path, BakedCellM, CellSizeM));
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Root->TryGetArrayField(TEXT("curbs"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				TArray<FVector2D> Line;
				for (const TSharedPtr<FJsonValue>& PV : V->AsArray())
				{
					const TArray<TSharedPtr<FJsonValue>>& Comp = PV->AsArray();
					if (Comp.Num() >= 2)
					{
						Line.Add(FVector2D(Comp[0]->AsNumber() * 100.0, Comp[1]->AsNumber() * 100.0));
					}
				}
				if (Line.Num() >= 2)
				{
					Out.Curbs.Add(MoveTemp(Line));
				}
			}
		}
		if (Root->TryGetArrayField(TEXT("crossings"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject>& O = V->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* P = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* D = nullptr;
				if (!O.IsValid() || !O->TryGetArrayField(TEXT("p"), P) ||
					!O->TryGetArrayField(TEXT("d"), D) || P->Num() < 2 || D->Num() < 2)
				{
					continue;
				}
				FMaskCrossing Site;
				Site.PosCm = FVector2D((*P)[0]->AsNumber() * 100.0, (*P)[1]->AsNumber() * 100.0);
				Site.DirCm = FVector2D((*D)[0]->AsNumber(), (*D)[1]->AsNumber());
				Site.HalfWCm = (float)(O->GetNumberField(TEXT("halfW")) * 100.0);
				if (Site.HalfWCm > 0.f && !Site.DirCm.IsNearlyZero())
				{
					Out.Crossings.Add(Site);
				}
			}
		}
		if (Root->TryGetArrayField(TEXT("axial"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TArray<TSharedPtr<FJsonValue>>& S = V->AsArray();
				if (S.Num() >= 4)
				{
					Out.Axial.Add(FVector4(S[0]->AsNumber() * 100.0, S[1]->AsNumber() * 100.0,
						S[2]->AsNumber() * 100.0, S[3]->AsNumber() * 100.0));
				}
			}
		}
		return true;
	}

	// -----------------------------------------------------------------------------
	// v4 — LE PIETON EST LA DALLE. Verdict DA v3 : « grand puzzle ». Le coupable
	// n'etait pas la palette (deja reduite a 3 classes en v3) mais LE FOND : la dalle
	// urbaine restait a la teinte unie de J2, et les centaines de petits rubans
	// pietons du centre-ville s'y collaient comme des autocollants sur du papier, en
	// laissant voir le vide dans leurs interstices. La v4 renverse le probleme :
	//   - la DALLE recoit la matiere minerale (GSurfSlab), donc toute la ville repose
	//     sur un sol credible ;
	//   - les voies PIETONNES ne produisent plus AUCUN ruban (IsPedestrianRibbon,
	//     early-continue cote generation) : marcher, c'est marcher sur la dalle. Plus
	//     un seul interstice a boucher, plus un seul lacis a harmoniser.
	// Il ne reste donc en rubans que ce qui se DISTINGUE vraiment du sol de la ville :
	//   (a) CHAUSSEE AUTO — asphalt_road partout ; les fine_road_* (ligne axiale
	//       peinte dans le scan) restent reserves aux voies annoncees lanes >= 2 ;
	//   (b) ALLEE NATURELLE — gravel_on_soil sur tag surface non revetu (surtout les
	//       allees de parc, qui doivent se lire sur l'herbe).
	// Une rue pavee (sett, paving_stones...) NON pietonne est une chaussee : elle part
	// desormais en asphalte, plus en revetement pave — cobblestone, marked_rough_road
	// et herringbone_brick_pavement ne sont plus references nulle part.
	// WidthCm = largeur de chaussee du JSON (hors trottoirs).
	// -----------------------------------------------------------------------------

	// Voies dont le ruban est SUPPRIME en profil revetements : leur sol, c'est la
	// dalle. Un seul endroit de verite — la passe de releve des carrefours et la passe
	// de generation doivent skipper exactement les memes voies (sinon un noeud
	// purement pieton continuerait de peser sur la dominante d'un carrefour).
	bool IsPedestrianRibbon(const FString& Type)
	{
		return Type == TEXT("pedestrian") || Type == TEXT("footway") || Type == TEXT("path") ||
			Type == TEXT("sidewalk") || Type == TEXT("steps") || Type == TEXT("platform") ||
			Type == TEXT("track");
	}

	const FSurfaceClass* SurfaceClassForRoad(const FString& Surface, const FString& Type,
		int32 Lanes, float WidthCm)
	{
		// 1) Sol NATUREL annonce par la donnee : prime sur le type.
		if (Surface == TEXT("gravel") || Surface == TEXT("fine_gravel") ||
			Surface == TEXT("compacted") || Surface == TEXT("dirt") || Surface == TEXT("ground") ||
			Surface == TEXT("earth") || Surface == TEXT("unpaved") || Surface == TEXT("sand"))
		{
			return &GSurfGravel;
		}
		// 2) CHAUSSEE AUTO. Les scans a ligne axiale sont reserves aux voies dont la
		//    donnee annonce au moins 2 files ; le reste est de l'asphalte nu.
		if (Lanes >= 3 || (Lanes >= 2 && WidthCm >= 900.f))
		{
			return &GSurfRoadWide;
		}
		if (Lanes == 2)
		{
			return &GSurfRoadMedium;
		}
		return &GSurfAsphalt;
	}

	// Classe resolue : le pack + le materiau charge (nul = repli sur l'historique).
	struct FResolvedSurface
	{
		const FSurfaceClass* Class = nullptr;
		UMaterialInterface* Material = nullptr;

		FName SlotName() const { return FName(Class->Slug); }
	};

	// Cache de chargement des materiaux de revetement. L'ABSENCE d'un materiau n'est
	// PAS une erreur : le groupe est quand meme cree (geometrie et UV metriques
	// identiques) et CreateMeshAsset lui donne le materiau de repli du mesh.
	struct FSurfaceLibrary
	{
		void Init(bool bOn, const FString& InFolder)
		{
			bEnabled = bOn;
			Folder = InFolder.IsEmpty() ? TEXT("/Game/City/Surfaces") : InFolder;
		}

		// Rend nullptr si les revetements sont desactives (profil mobile) : tout
		// appelant retombe alors sur le chemin historique, a l'octet pres.
		// Entrees en TUniquePtr : les pointeurs rendus restent valides quand le cache
		// grandit (le rehash d'une TMap de valeurs les invaliderait).
		const FResolvedSurface* Resolve(const FSurfaceClass* Class)
		{
			if (!bEnabled || !Class)
			{
				return nullptr;
			}
			const FString Key(Class->Slug);
			if (TUniquePtr<FResolvedSurface>* Found = Resolved.Find(Key))
			{
				return Found->Get();
			}
			const FString Path = FString::Printf(TEXT("%s/%s/M_Surf_%s.M_Surf_%s"),
				*Folder, Class->Slug, Class->Slug, Class->Slug);
			TUniquePtr<FResolvedSurface> Entry = MakeUnique<FResolvedSurface>();
			Entry->Class = Class;
			Entry->Material = LoadObject<UMaterialInterface>(nullptr, *Path, nullptr,
				LOAD_NoWarn | LOAD_Quiet);
			if (!Entry->Material)
			{
				// Display et NON Warning : le repli est un mode de fonctionnement
				// normal (tests sans assets Megascans, generation avant import) —
				// et l'automation eleve les warnings en erreurs de test.
				UE_LOG(LogCityImport, Display,
					TEXT("Revetement '%s' absent (%s) : repli sur le materiau historique."),
					Class->Slug, *Path);
			}
			const FResolvedSurface* Out = Entry.Get();
			Resolved.Add(Key, MoveTemp(Entry));
			return Out;
		}

	private:
		FString Folder;
		bool bEnabled = false;
		TMap<FString, TUniquePtr<FResolvedSurface>> Resolved;
	};

	// -----------------------------------------------------------------------------
	// v2 — CARREFOURS. Verdict utilisateur sur le proto v1 : « les revetements se
	// rencontrent sans harmonie, coupes franches, superpositions ». La cause est aux
	// noeuds : N rubans de classes differentes s'y empilent et les tirets axiaux
	// traversent le croisement. Parade en deux temps :
	//   1. un PATCH polygonal du revetement dominant recouvre le disque de rencontre ;
	//   2. les segments de ruban a moins de GJunctionPlainCm d'un noeud passent en
	//      asphalte NU (plus de tiret qui traverse le carrefour).
	// Un noeud est un carrefour s'il est partage par >= 3 routes, OU s'il est un point
	// INTERIEUR d'au moins une route (une route qui passe au travers). Deux routes qui
	// s'y terminent seulement = simple decoupage OSM d'une meme rue : ce n'est PAS un
	// carrefour, et y effacer les tirets creverait le marquage de tout un boulevard.
	// -----------------------------------------------------------------------------
	constexpr float GJunctionPlainCm = 800.f;  // rayon d'effacement des tirets (8 m)
	constexpr float GJunctionGridCm = 800.f;   // pas de la grille de recherche
	constexpr float GJunctionPatchMarginCm = 100.f; // rayon = max demi-largeur + 1 m
	constexpr float GJunctionPatchLiftCm = 5.f;     // patch pose au-dessus du ruban le plus haut
	// v5 point 4 — FRAGMENTS ORPHELINS. Verdict utilisateur sur la v4b : « morceaux
	// perdus » — des bouts de voie de quelques metres, sans aucun noeud commun avec le
	// reseau, poses seuls au milieu de la dalle uniforme (troncons OSM coupes par la
	// fenetre d'extraction, contre-allees, acces de parking). Un ruban court ET
	// deconnecte n'apporte rien : il ne raconte pas une rue, il salit le fond.
	constexpr float GOrphanMaxLenCm = 2500.f;
	// Grille de l'index des DISQUES DE PATCH (plus large que celle des noeuds : un
	// disque deborde de sa cellule). Chaque disque s'inscrit dans toutes les cellules
	// que touche sa boite englobante ELARGIE de GPatchSlackMaxCm, si bien qu'une
	// requete ne consulte qu'une seule cellule.
	constexpr float GPatchGridCm = 3200.f;
	constexpr float GPatchSlackMaxCm = 2000.f;

	struct FJunctionNode
	{
		FVector2D PosCm = FVector2D::ZeroVector;
		int32 FirstRoad = INDEX_NONE;
		int32 LastRoad = INDEX_NONE;
		int32 NumRoads = 0;
		int32 NumAutoRoads = 0;
		int32 NumInterior = 0;
		float MaxHalfCm = 0.f;
		float MaxZClassCm = 0.f;
		const FSurfaceClass* Dominant = nullptr;
		float DominantHalfCm = -1.f;

		// NumRoads >= 2 est une PRECONDITION : sans elle, chaque sommet interieur
		// d'une route SEULE passait pour un carrefour — mesure sur le proto v2 :
		// 3 042 « carrefours » sur 3 920 noeuds releves, soit un patch tous les
		// quelques metres et plus un seul tiret axial nulle part.
		bool IsJunction() const { return NumRoads >= 2 && (NumRoads >= 3 || NumInterior >= 1); }

		// v3 — un patch de carrefour n'a de sens qu'entre VOITURES. Verdict DA sur le
		// proto v2 : dans le lacis pieton du centre, chaque croisement de sentiers
		// posait son disque d'un autre revetement — « peau de leopard ». Condition :
		// la voie DOMINANTE est une chaussee auto ET au moins une AUTRE voie du noeud
		// l'est aussi (le disque recouvre alors une vraie zone de roulement).
		bool WantsPatch() const
		{
			return IsJunction() && Dominant && Dominant->bAuto && NumAutoRoads >= 2;
		}
	};

	struct FJunctionMap
	{
		// Quantification au decimetre : les noeuds partages viennent du MEME noeud OSM
		// et traversent la conversion a l'identique — le decimetre absorbe le bruit
		// d'arrondi du JSON (2 decimales de metre) sans fusionner deux vrais noeuds.
		static FIntPoint Key(const FVector2D& P)
		{
			return FIntPoint(FMath::RoundToInt(P.X / 10.f), FMath::RoundToInt(P.Y / 10.f));
		}

		void Add(int32 RoadIndex, const TArray<FVector2D>& PtsCm, float HalfCm,
			const FSurfaceClass* Class)
		{
			for (int32 i = 0; i < PtsCm.Num(); ++i)
			{
				FJunctionNode& Node = Nodes.FindOrAdd(Key(PtsCm[i]));
				bool bNewRoadHere = false;
				if (Node.NumRoads == 0)
				{
					Node.PosCm = PtsCm[i];
					Node.FirstRoad = RoadIndex;
					Node.NumRoads = 1;
					bNewRoadHere = true;
				}
				else if (Node.FirstRoad != RoadIndex && Node.LastRoad != RoadIndex)
				{
					++Node.NumRoads;
					bNewRoadHere = true;
				}
				// v3 : compte des CHAUSSEES AUTO distinctes au noeud (meme regle de
				// dedoublonnage que NumRoads) — filtre des patchs de carrefour.
				if (bNewRoadHere && Class && Class->bAuto)
				{
					++Node.NumAutoRoads;
				}
				Node.LastRoad = RoadIndex;
				if (i > 0 && i + 1 < PtsCm.Num())
				{
					++Node.NumInterior;
				}
				Node.MaxHalfCm = FMath::Max(Node.MaxHalfCm, HalfCm);
				if (Class)
				{
					Node.MaxZClassCm = FMath::Max(Node.MaxZClassCm, Class->ZClassCm);
					// Dominante = la voie la plus LARGE (donc la plus prioritaire) ;
					// a egalite, la premiere rencontree (deterministe : l'ordre du JSON).
					if (HalfCm > Node.DominantHalfCm)
					{
						Node.DominantHalfCm = HalfCm;
						Node.Dominant = Class;
					}
				}
			}
		}

		// Index spatial des SEULS vrais carrefours, construit une fois la collecte finie.
		// v5 : le meme passage remplit l'index des DISQUES DE PATCH — l'emprise ou la
		// bordure s'interrompt et ou un passage pieton est reporte. Meme condition
		// EXACTE que la passe de generation des patchs (WantsPatch + demi-largeur
		// mini) : un seul endroit de verite, sinon la bordure se couperait la ou aucun
		// disque n'est pose.
		void Build()
		{
			for (TPair<FIntPoint, FJunctionNode>& Pair : Nodes)
			{
				if (Pair.Value.IsJunction())
				{
					Grid.FindOrAdd(FIntPoint(FMath::FloorToInt(Pair.Value.PosCm.X / GJunctionGridCm),
						FMath::FloorToInt(Pair.Value.PosCm.Y / GJunctionGridCm))).Add(Pair.Value.PosCm);
					++NumJunctions;
					if (Pair.Value.WantsPatch())
					{
						++NumAutoJunctions;
						if (Pair.Value.MaxHalfCm >= 150.f)
						{
							AddPatchDisc(Pair.Value.PosCm, Pair.Value.MaxHalfCm + GJunctionPatchMarginCm);
						}
					}
				}
			}
		}

		// Disque de patch reellement pose (centre + rayon), pour le decoupage de la
		// bordure et le report des passages pietons.
		struct FPatchDisc
		{
			FVector2D PosCm = FVector2D::ZeroVector;
			float RadiusCm = 0.f;
		};

		void AddPatchDisc(const FVector2D& P, float RadiusCm)
		{
			const float Reach = RadiusCm + GPatchSlackMaxCm;
			const int32 X0 = FMath::FloorToInt((P.X - Reach) / GPatchGridCm);
			const int32 X1 = FMath::FloorToInt((P.X + Reach) / GPatchGridCm);
			const int32 Y0 = FMath::FloorToInt((P.Y - Reach) / GPatchGridCm);
			const int32 Y1 = FMath::FloorToInt((P.Y + Reach) / GPatchGridCm);
			const FPatchDisc Disc{ P, RadiusCm };
			for (int32 Y = Y0; Y <= Y1; ++Y)
			{
				for (int32 X = X0; X <= X1; ++X)
				{
					PatchGrid.FindOrAdd(FIntPoint(X, Y)).Add(Disc);
				}
			}
			++NumPatchDiscs;
		}

		// Le point est-il couvert par un disque de patch (SlackCm = marge d'approche) ?
		// Les disques etant inscrits dans toutes les cellules de leur boite elargie de
		// GPatchSlackMaxCm, une SEULE cellule suffit tant que SlackCm reste sous cette
		// borne — le clamp evite un faux negatif silencieux si un appelant la depasse.
		bool IsInPatch(const FVector2D& P, float SlackCm = 0.f) const
		{
			const float Slack = FMath::Min(SlackCm, GPatchSlackMaxCm);
			const TArray<FPatchDisc>* Cell = PatchGrid.Find(
				FIntPoint(FMath::FloorToInt(P.X / GPatchGridCm), FMath::FloorToInt(P.Y / GPatchGridCm)));
			if (!Cell)
			{
				return false;
			}
			for (const FPatchDisc& D : *Cell)
			{
				const float R = D.RadiusCm + Slack;
				if (FVector2D::DistSquared(P, D.PosCm) <= R * R)
				{
					return true;
				}
			}
			return false;
		}

		bool IsNear(const FVector2D& P, float RadiusCm) const
		{
			const int32 GX = FMath::FloorToInt(P.X / GJunctionGridCm);
			const int32 GY = FMath::FloorToInt(P.Y / GJunctionGridCm);
			const float R2 = RadiusCm * RadiusCm;
			for (int32 dy = -1; dy <= 1; ++dy)
			{
				for (int32 dx = -1; dx <= 1; ++dx)
				{
					if (const TArray<FVector2D>* Cell = Grid.Find(FIntPoint(GX + dx, GY + dy)))
					{
						for (const FVector2D& Q : *Cell)
						{
							if (FVector2D::DistSquared(P, Q) <= R2)
							{
								return true;
							}
						}
					}
				}
			}
			return false;
		}

		TMap<FIntPoint, FJunctionNode> Nodes;
		TMap<FIntPoint, TArray<FVector2D>> Grid;
		TMap<FIntPoint, TArray<FPatchDisc>> PatchGrid;
		int32 NumJunctions = 0;
		int32 NumAutoJunctions = 0; // v3 : ceux qui recoivent reellement un patch
		int32 NumPatchDiscs = 0;    // v5 : disques indexes (== patchs poses)
	};

	// Patch de carrefour : disque du revetement dominant, pose AU-DESSUS de tous les
	// rubans du noeud, UV0 monde en metres (jamais de ligne axiale au milieu d'un
	// croisement). Le triangle-fan part du centre : 16 secteurs suffisent a un disque
	// de 5-15 m vu depuis un drone.
	void BuildJunctionPatch(FCityMeshBuilder& QM, const FVector2D& CenterCm, float RadiusCm,
		float Zcm, const FResolvedSurface* Surf, const FVector3f& Tint)
	{
		constexpr int32 Sides = 16;
		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID Group = QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material);
		auto At = [&](int32 i)
		{
			const float Ang = 2.f * PI * i / Sides;
			return FVector3f((float)CenterCm.X + RadiusCm * FMath::Cos(Ang),
				(float)CenterCm.Y + RadiusCm * FMath::Sin(Ang), Zcm);
		};
		const FVector3f C((float)CenterCm.X, (float)CenterCm.Y, Zcm);
		for (int32 i = 0; i < Sides; ++i)
		{
			const FVector3f P[3] = { C, At(i), At(i + 1) };
			const FVector2f UV[3] = {
				FVector2f(P[0].X * 0.01f, P[0].Y * 0.01f),
				FVector2f(P[1].X * 0.01f, P[1].Y * 0.01f),
				FVector2f(P[2].X * 0.01f, P[2].Y * 0.01f) };
			QM.AddPoly(Group, P, 3, Up, UV, Tint);
		}
	}

	// -----------------------------------------------------------------------------
	// v5 « VOIRIE » — RIVES ET BORDURES d'un segment de chaussee. Pour chaque cote :
	//   1. la FACE de bordure, quad VERTICAL de 12 cm tourne vers la chaussee (c'est
	//      elle qui prend la lumiere autrement que le sol : la rue se lit) ;
	//   2. le CHANT, bande horizontale de 15 cm au sommet de la bordure ;
	//   3. la RIVE, 1,70 m de classe DALLE au meme niveau que le chant.
	// Chant et rive portent une UV0 MONDE en metres, exactement comme la dalle qui les
	// porte : le scan y est EN PHASE avec le fond de ville, la rive prolonge le sol au
	// lieu d'y coller un rectangle.
	// Junctions : sur l'emprise d'un patch de carrefour, la structure laterale
	// s'efface (le disque du patch EST la zone de roulement — une bordure la
	// traverserait de part en part). Le decoupage ne se sous-divise qu'au VOISINAGE
	// d'un patch : ailleurs, un quad par segment et par bande, comme avant.
	// -----------------------------------------------------------------------------
	void BuildStreetSides(FCityMeshBuilder& QM, const FVector2D& A, const FVector2D& B,
		const FVector2D& NrmA, const FVector2D& NrmB, float ZA, float ZB, float Arc, float SegLen,
		float RoadHalfCm, const FResolvedSurface* SurfCurb, const FResolvedSurface* SurfSlab,
		const FVector3f& Tint, const FJunctionMap* Junctions, int32* OutCurbQuads)
	{
		if (SegLen < 1.f)
		{
			return;
		}
		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID CurbGroup = QM.GetOrCreateGroup(SurfCurb->SlotName(), SurfCurb->Material);
		const FPolygonGroupID SlabGroup = QM.GetOrCreateGroup(SurfSlab->SlotName(), SurfSlab->Material);

		// Sous-decoupe UNIQUEMENT si un disque de patch est a portee du segment.
		int32 Sub = 1;
		const bool bClip = Junctions && Junctions->NumPatchDiscs > 0 &&
			(Junctions->IsInPatch(A, SegLen) || Junctions->IsInPatch(B, SegLen));
		if (bClip)
		{
			Sub = FMath::Clamp(FMath::CeilToInt(SegLen / GCurbClipStepCm), 1, 32);
		}
		for (int32 s = 0; s < Sub; ++s)
		{
			const float T0 = (float)s / Sub;
			const float T1 = (float)(s + 1) / Sub;
			const FVector2D S0 = FMath::Lerp(A, B, T0);
			const FVector2D S1 = FMath::Lerp(A, B, T1);
			if (bClip && Junctions->IsInPatch(FMath::Lerp(S0, S1, 0.5f)))
			{
				continue;
			}
			const FVector2D N0 = FMath::Lerp(NrmA, NrmB, T0);
			const FVector2D N1 = FMath::Lerp(NrmA, NrmB, T1);
			const float Z0 = FMath::Lerp(ZA, ZB, T0);
			const float Z1 = FMath::Lerp(ZA, ZB, T1);
			const float U0 = (Arc + SegLen * T0) * 0.01f;
			const float U1 = (Arc + SegLen * T1) * 0.01f;
			// Side = +1 / -1 : les deux rives, symetriques par rapport a l'axe.
			for (int32 Side = -1; Side <= 1; Side += 2)
			{
				auto At = [&](const FVector2D& S, const FVector2D& Nl, float Z, float Lateral, float Lift)
				{
					const FVector2D P = S + Nl * (Side * Lateral);
					return FVector3f((float)P.X, (float)P.Y, Z + Lift);
				};
				auto WorldUV = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };

				// 1. FACE de bordure : quad vertical, normale vers l'axe de la rue.
				const FVector3f F[4] = {
					At(S0, N0, Z0, RoadHalfCm, 0.f),
					At(S1, N1, Z1, RoadHalfCm, 0.f),
					At(S1, N1, Z1, RoadHalfCm, GCurbHeightCm),
					At(S0, N0, Z0, RoadHalfCm, GCurbHeightCm) };
				const FVector2f FUV[4] = {
					FVector2f(U0, 0.f), FVector2f(U1, 0.f),
					FVector2f(U1, GCurbHeightCm * 0.01f), FVector2f(U0, GCurbHeightCm * 0.01f) };
				const FVector2D Inward = -N0 * (float)Side;
				QM.AddPoly(CurbGroup, F, 4,
					FVector3f((float)Inward.X, (float)Inward.Y, 0.f).GetSafeNormal(), FUV, Tint);

				// 2. CHANT : bande horizontale de 15 cm au sommet de la bordure.
				const FVector3f C[4] = {
					At(S0, N0, Z0, RoadHalfCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm),
					At(S0, N0, Z0, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm) };
				const FVector2f CUV[4] = { WorldUV(C[0]), WorldUV(C[1]), WorldUV(C[2]), WorldUV(C[3]) };
				QM.AddPoly(CurbGroup, C, 4, Up, CUV, Tint);

				// 3. RIVE : 1,70 m de dalle, de plain-pied avec le chant.
				const FVector3f W[4] = {
					At(S0, N0, Z0, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm + GCurbTopWidthCm, GCurbHeightCm),
					At(S1, N1, Z1, RoadHalfCm + GCurbTopWidthCm + GSidewalkWidthCm, GCurbHeightCm),
					At(S0, N0, Z0, RoadHalfCm + GCurbTopWidthCm + GSidewalkWidthCm, GCurbHeightCm) };
				const FVector2f WUV[4] = { WorldUV(W[0]), WorldUV(W[1]), WorldUV(W[2]), WorldUV(W[3]) };
				QM.AddPoly(SlabGroup, W, 4, Up, WUV, Tint);

				if (OutCurbQuads)
				{
					*OutCurbQuads += 2; // face + chant
				}
			}
		}
	}

	// PASSAGE PIETON (v5 point 2). Les voies pietonnes vivent dans la DONNEE meme si
	// elles ne produisent plus de ruban : la ou l'une d'elles partage un noeud avec une
	// chaussee auto, il y avait un passage dans la vraie ville. On y pose un quad du
	// scan pedestrian_crossing_lines, EN TRAVERS de la chaussee seule (jamais sur les
	// rives : un passage ne monte pas sur le trottoir), aligne sur l'axe de la rue.
	// UV : U = travers de la rue en metres (le scan repete ses bandes de 50 cm tous les
	// 4 m : bandes PARALLELES a l'axe de la chaussee, norme francaise) ; V = 1 -> 3 m,
	// soit UNE tuile du scan calee pour que son trait blanc tombe exactement sur les
	// deux bords du passage plutot qu'en son milieu.
	void BuildCrossing(FCityMeshBuilder& QM, const FVector2D& CenterCm, const FVector2D& DirCm,
		float RoadHalfCm, float Zcm, const FResolvedSurface* Surf, const FVector3f& Tint)
	{
		const FVector2D D = DirCm.GetSafeNormal();
		if (D.IsNearlyZero())
		{
			return;
		}
		const FVector2D Lat(-D.Y, D.X);
		const FVector2D Along = D * GCrossingHalfLenCm;
		const FVector2D Across = Lat * RoadHalfCm;
		auto At = [&](float SAlong, float SAcross)
		{
			const FVector2D P = CenterCm + Along * SAlong + Across * SAcross;
			return FVector3f((float)P.X, (float)P.Y, Zcm);
		};
		const FVector3f P[4] = { At(-1.f, -1.f), At(1.f, -1.f), At(1.f, 1.f), At(-1.f, 1.f) };
		const float AcrossM = RoadHalfCm * 0.01f;
		const FVector2f UV[4] = {
			FVector2f(-AcrossM, 1.f), FVector2f(-AcrossM, 3.f),
			FVector2f(AcrossM, 3.f), FVector2f(AcrossM, 1.f) };
		QM.AddPoly(QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material), P, 4,
			FVector3f(0, 0, 1), UV, Tint);
	}

	// -----------------------------------------------------------------------------
	// J3c maquette — BORDURE le long d'une polyligne de masque. Meme vocabulaire
	// geometrique que BuildStreetSides (face verticale de 12 cm + chant de 15 cm),
	// mais la polyligne n'est plus un AXE de route : c'est la FRONTIERE elle-meme,
	// deja orientee au prep chaussee A GAUCHE. Un troisieme quad ferme la bordure
	// cote trottoir.
	//
	// ASSUME : le sol etant PEINT, il est plan des deux cotes de la bordure — le
	// trottoir n'est pas surhausse. La bordure est donc une PIERRE POSEE (12 cm de
	// relief, 15 cm de chant) et non une marche. C'est ce qui donne la lecture des
	// rives depuis le ciel (deux aretes eclairees differemment) au prix d'un
	// trottoir qui, au ras du sol, est de plain-pied avec la chaussee. Surhausser
	// le trottoir demanderait de deformer la dalle : hors perimetre de la maquette.
	void BuildMaskCurb(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm,
		const FDrapeContext& Drape, const FResolvedSurface* Surf, const FVector3f& Tint,
		int32* OutQuads)
	{
		if (!Surf || PtsCm.Num() < 2)
		{
			return;
		}
		const FVector3f Up(0, 0, 1);
		const FPolygonGroupID Group = QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material);
		auto WorldUV = [](const FVector3f& P) { return FVector2f(P.X * 0.01f, P.Y * 0.01f); };
		float Arc = 0.f;
		for (int32 i = 0; i + 1 < PtsCm.Num(); ++i)
		{
			const FVector2D A = PtsCm[i];
			const FVector2D B = PtsCm[i + 1];
			const FVector2D D = B - A;
			const float SegLen = (float)D.Size();
			if (SegLen < 1.f)
			{
				continue;
			}
			const FVector2D Dir = D / SegLen;
			// Chaussee A GAUCHE : la normale qui pointe VERS la chaussee est
			// (-Dir.Y, Dir.X), le trottoir est du cote oppose.
			const FVector2D ToRoad(-Dir.Y, Dir.X);
			const FVector2D ToWalk = -ToRoad;
			const float ZA = Drape.GroundZ(A.X, A.Y);
			const float ZB = Drape.GroundZ(B.X, B.Y);
			auto At = [&](const FVector2D& P, float Z, const FVector2D& Lat, float Off, float Lift)
			{
				return FVector3f((float)(P.X + Lat.X * Off), (float)(P.Y + Lat.Y * Off), Z + Lift);
			};
			const float U0 = Arc * 0.01f;
			const float U1 = (Arc + SegLen) * 0.01f;
			Arc += SegLen;

			// 1. FACE cote chaussee (celle qui prend la lumiere rasante).
			const FVector3f F[4] = {
				At(A, ZA, ToRoad, 0.f, -GMaskCurbSinkCm),
				At(B, ZB, ToRoad, 0.f, -GMaskCurbSinkCm),
				At(B, ZB, ToRoad, 0.f, GCurbHeightCm),
				At(A, ZA, ToRoad, 0.f, GCurbHeightCm) };
			const FVector2f FUV[4] = {
				FVector2f(U0, 0.f), FVector2f(U1, 0.f),
				FVector2f(U1, (GCurbHeightCm + GMaskCurbSinkCm) * 0.01f),
				FVector2f(U0, (GCurbHeightCm + GMaskCurbSinkCm) * 0.01f) };
			QM.AddPoly(Group, F, 4,
				FVector3f((float)ToRoad.X, (float)ToRoad.Y, 0.f).GetSafeNormal(), FUV, Tint);

			// 2. CHANT horizontal, 15 cm vers le trottoir. UV MONDE : le motif reste
			//    en phase avec la dalle qui le porte (la bordure est de la meme
			//    matiere, juste assombrie).
			const FVector3f C[4] = {
				At(A, ZA, ToRoad, 0.f, GCurbHeightCm),
				At(B, ZB, ToRoad, 0.f, GCurbHeightCm),
				At(B, ZB, ToWalk, GCurbTopWidthCm, GCurbHeightCm),
				At(A, ZA, ToWalk, GCurbTopWidthCm, GCurbHeightCm) };
			const FVector2f CUV[4] = { WorldUV(C[0]), WorldUV(C[1]), WorldUV(C[2]), WorldUV(C[3]) };
			QM.AddPoly(Group, C, 4, Up, CUV, Tint);

			// 3. FACE cote trottoir : sans elle la bordure serait un plan sans dos,
			//    invisible depuis le trottoir.
			const FVector3f W[4] = {
				At(B, ZB, ToWalk, GCurbTopWidthCm, GCurbHeightCm),
				At(A, ZA, ToWalk, GCurbTopWidthCm, GCurbHeightCm),
				At(A, ZA, ToWalk, GCurbTopWidthCm, -GMaskCurbSinkCm),
				At(B, ZB, ToWalk, GCurbTopWidthCm, -GMaskCurbSinkCm) };
			const FVector2f WUV[4] = {
				FVector2f(U1, 0.f), FVector2f(U0, 0.f),
				FVector2f(U0, (GCurbHeightCm + GMaskCurbSinkCm) * 0.01f),
				FVector2f(U1, (GCurbHeightCm + GMaskCurbSinkCm) * 0.01f) };
			QM.AddPoly(Group, W, 4,
				FVector3f((float)ToWalk.X, (float)ToWalk.Y, 0.f).GetSafeNormal(), WUV, Tint);

			if (OutQuads)
			{
				*OutQuads += 3;
			}
		}
	}

	// TIRET de ligne axiale : un quad de 15 cm de large. Le debitage (3 m plein /
	// 1,5 m vide, 8 m d'ecart aux carrefours, dans la chaussee seulement) est fait
	// au prep — le C++ ne decide de rien ici.
	void BuildAxialDash(FCityMeshBuilder& QM, const FVector2D& ACm, const FVector2D& BCm,
		const FDrapeContext& Drape, const FResolvedSurface* Surf, const FVector3f& Tint)
	{
		const FVector2D D = BCm - ACm;
		const float Len = (float)D.Size();
		if (!Surf || Len < 1.f)
		{
			return;
		}
		const FVector2D Dir = D / Len;
		const FVector2D Lat(-Dir.Y, Dir.X);
		const float H = GAxialWidthCm * 0.5f;
		auto At = [&](const FVector2D& P, float Side)
		{
			const FVector2D Q = P + Lat * (Side * H);
			return FVector3f((float)Q.X, (float)Q.Y,
				Drape.GroundZ(Q.X, Q.Y) + GMaskDashLiftCm);
		};
		const FVector3f P[4] = { At(ACm, -1.f), At(BCm, -1.f), At(BCm, 1.f), At(ACm, 1.f) };
		const FVector2f UV[4] = {
			FVector2f(0.f, 0.f), FVector2f(Len * 0.01f, 0.f),
			FVector2f(Len * 0.01f, GAxialWidthCm * 0.01f), FVector2f(0.f, GAxialWidthCm * 0.01f) };
		QM.AddPoly(QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material), P, 4,
			FVector3f(0, 0, 1), UV, Tint);
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
	// Surf (J3c point 2) : classe de revetement resolue — le ruban part dans un groupe
	// de polygones dedie avec une UV0 EN METRES (U = abscisse curviligne, V = position
	// transversale) au lieu du slot Glass + T_RoadStrip. Nul = chemin historique.
	// SurfPlain + NearJunction (v2) : sur les segments dont un sommet touche un
	// carrefour, la classe bascule vers l'asphalte NU — les tirets axiaux ne
	// traversent plus les croisements. Le Z reste celui de la classe D'ORIGINE sur
	// toute la longueur : pas de marche au raccord.
	// SurfCurb + SurfSlab + Junctions (v5 « voirie ») : la CHAUSSEE AUTO n'est plus un
	// quad unique — bande centrale de classe chaussee, deux bordures en relief de
	// 12 cm et deux rives de 1,70 m en classe DALLE (cf. GSidewalkWidthCm). Le tout
	// nul = chemin d'avant, a l'octet pres (mobile, gravier, sentiers).
	void BuildRoad(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float WidthCm,
		const FString& Type, int32 RoadIndex, const TArray<float>* TerrainZ = nullptr,
		bool bBakedShade = true, const FResolvedSurface* Surf = nullptr,
		const FResolvedSurface* SurfPlain = nullptr, const TArray<uint8>* NearJunction = nullptr,
		const FResolvedSurface* SurfCurb = nullptr, const FResolvedSurface* SurfSlab = nullptr,
		const FJunctionMap* Junctions = nullptr, int32* OutCurbQuads = nullptr)
	{
		const int32 N = PtsCm.Num();
		if (N < 2)
		{
			return;
		}
		// v1 (mobile) : jitter par ordre d'arrivee — a 0,8 cm de pas, deux routes qui se
		// croisent scintillaient au-dela de ~1 km (precision depth GLES).
		// v2 (revetements) : offset DETERMINISTE PAR CLASSE (gravier bas ... dalles
		// haut) + un micro-jitter de 0 a 1,2 cm pour departager deux rubans COPLANAIRES
		// de la meme classe, dix fois plus petit que le pas entre classes : l'ordre
		// entre classes ne s'inverse jamais.
		const float ZRoad = Surf
			? 55.f + Surf->Class->ZClassCm + (RoadIndex % 4) * 0.4f
			: 55.f + (RoadIndex % 7) * 4.f;
		const bool bWalkway = Type == TEXT("footway") || Type == TEXT("path") || Type == TEXT("cycleway");
		const bool bMarking = !bWalkway && WidthCm >= 550.f;
		const bool bSolid = Type == TEXT("primary") || Type == TEXT("secondary");
		const float WalkW = bWalkway ? 0.f : 170.f;
		// v5 — RUE COMPLETE : reserve aux CHAUSSEES AUTO. Une allee de gravier ou une
		// piste cyclable n'a pas de bordure ; elle garde son ruban d'un seul tenant.
		const bool bStreet = Surf && SurfCurb && SurfSlab && Surf->Class->bAuto;
		const float RoadHalf = WidthCm * 0.5f;
		// La demi-largeur TOTALE du ruban : chaussee + chant + rive en rue complete
		// (RibbonHalfCm cote appelant applique la MEME regle — c'est elle qui fixe le
		// rayon des patchs de carrefour).
		const float Half = bStreet ? RoadHalf + GCurbTopWidthCm + GSidewalkWidthCm
			: WidthCm * 0.5f + WalkW;
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
			// v5 : en rue complete, le quad de CLASSE ne couvre plus que la chaussee —
			// les rives partent dans le groupe de la dalle et la bordure les separe.
			const float QuadHalf = bStreet ? RoadHalf : Half;
			const FVector2D NA = Nrm[i] * QuadHalf, NB = Nrm[i + 1] * QuadHalf;
			const float SegLen = (B - A).Size();
			const float ZA = ZRoad + (TerrainZ ? (*TerrainZ)[i] : 0.f);
			const float ZB = ZRoad + (TerrainZ ? (*TerrainZ)[i + 1] : 0.f);
			const FVector3f P[4] = {
				FVector3f(A.X - NA.X, A.Y - NA.Y, ZA), FVector3f(B.X - NB.X, B.Y - NB.Y, ZB),
				FVector3f(B.X + NB.X, B.Y + NB.Y, ZB), FVector3f(A.X + NA.X, A.Y + NA.Y, ZA) };
			if (bStreet)
			{
				BuildStreetSides(QM, A, B, Nrm[i], Nrm[i + 1], ZA, ZB, Arc, SegLen, RoadHalf,
					SurfCurb, SurfSlab, Shaded, Junctions, OutCurbQuads);
			}
			if (Surf)
			{
				// Segment au contact d'un carrefour : classe de remplacement NUE
				// (asphalte tuile) pour ne pas tirer un tiret axial en travers du
				// croisement. Seules les classes MARQUEES (bFullWidth) sont concernees.
				const FResolvedSurface* SegSurf = Surf;
				if (SurfPlain && NearJunction && Surf->Class->bFullWidth &&
					((*NearJunction)[i] || (*NearJunction)[i + 1]))
				{
					SegSurf = SurfPlain;
				}
				// UV0 en METRES : U = abscisse curviligne, V = position transversale.
				// bFullWidth : V couvre exactement AcrossM sur TOUTE la largeur du
				// quad (la ligne axiale integree au scan tombe au milieu) ; sinon V
				// est la distance transversale reelle et la texture tuile aussi.
				// v5 : en rue complete le quad EST la chaussee — la ligne axiale peinte
				// tombe donc au milieu de la CHAUSSEE et non plus au milieu de
				// chaussee + trottoirs (elle etait decalee de toute la largeur d'un
				// trottoir sur les rues etroites).
				const float Along0 = Arc * 0.01f;
				const float Along1 = (Arc + SegLen) * 0.01f;
				const float AcrossMax = SegSurf->Class->bFullWidth
					? SegSurf->Class->AcrossM : (2.f * QuadHalf * 0.01f);
				const float AlongAt[4] = { Along0, Along1, Along1, Along0 };
				const float AcrossAt[4] = { 0.f, 0.f, AcrossMax, AcrossMax };
				FVector2f UV[4];
				for (int32 c = 0; c < 4; ++c)
				{
					UV[c] = SegSurf->Class->bSwapUV
						? FVector2f(AcrossAt[c], AlongAt[c])
						: FVector2f(AlongAt[c], AcrossAt[c]);
				}
				QM.AddPoly(QM.GetOrCreateGroup(SegSurf->SlotName(), SegSurf->Material), P, 4, Up, UV, Shaded);
			}
			else if (bWalkway)
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
		// J3c point 2 : un slot par classe de revetement, MEME ORDRE que les groupes
		// de polygones du builder (le nom de slot est la cle de correspondance a la
		// construction). Materiau du pack absent -> repli sur GlassMat, qui est le
		// materiau HISTORIQUE des rubans sur les cellules de sol (M_CityRoad_PBR) et
		// vaut WallMat sur les cellules de surfaces : un import sans les assets
		// Megascans rend donc exactement comme avant.
		for (int32 e = 0; e < QM.ExtraSlotNames.Num(); ++e)
		{
			UMaterialInterface* ExtraMat = QM.ExtraSlotMaterials[e] ? QM.ExtraSlotMaterials[e] : GlassMat;
			Mesh->GetStaticMaterials().Add(FStaticMaterial(ExtraMat, QM.ExtraSlotNames[e]));
		}
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
	Profile.bSurfaceMaterials = true;
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
	Out.bSurfaceMaterials = true;
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
	// Surf (J3c point 2) : classe de revetement resolue — le polygone part dans son
	// groupe dedie avec une UV0 MONDE EN METRES (pelouses, bois). Nul = historique.
	void BuildFlatPolygon(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float Zcm,
		const FVector3f& Tint, const TArray<float>* TerrainZ = nullptr,
		const FResolvedSurface* Surf = nullptr)
	{
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		const FVector3f Shaded = Shade(Tint, FVector3f(0, 0, 1), Zcm);
		auto VertexZ = [&](int32 Index)
		{
			return Zcm + (TerrainZ ? (*TerrainZ)[Index] : 0.f);
		};
		const FPolygonGroupID Group = Surf
			? QM.GetOrCreateGroup(Surf->SlotName(), Surf->Material) : QM.WallGroup;
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			const FVector3f P[3] = {
				FVector3f(PtsCm[Tris[t]].X, PtsCm[Tris[t]].Y, VertexZ(Tris[t])),
				FVector3f(PtsCm[Tris[t + 1]].X, PtsCm[Tris[t + 1]].Y, VertexZ(Tris[t + 1])),
				FVector3f(PtsCm[Tris[t + 2]].X, PtsCm[Tris[t + 2]].Y, VertexZ(Tris[t + 2])) };
			if (!Surf)
			{
				QM.AddTri(QM.WallGroup, P[0], P[1], P[2], FVector3f(0, 0, 1), Shaded);
				continue;
			}
			if ((P[0] - P[1]).IsNearlyZero(0.01f) || (P[1] - P[2]).IsNearlyZero(0.01f) ||
				(P[2] - P[0]).IsNearlyZero(0.01f))
			{
				continue;
			}
			const FVector2f UV[3] = {
				FVector2f(P[0].X * 0.01f, P[0].Y * 0.01f),
				FVector2f(P[1].X * 0.01f, P[1].Y * 0.01f),
				FVector2f(P[2].X * 0.01f, P[2].Y * 0.01f) };
			QM.AddPoly(Group, P, 3, FVector3f(0, 0, 1), UV, Shaded);
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

	// J3c point 2 : revetements Megascans des surfaces vertes (l'eau et les rails
	// gardent leur film teinte en v1). Desactive = surfaces historiques a l'octet pres.
	FSurfaceLibrary Surfaces;
	Surfaces.Init(Gen.bSurfaceMaterials, Gen.SurfacesFolder);

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
			// v5 point 3 — ASSAINISSEMENT DES ESPACES VERTS. Verdict utilisateur sur la
			// v4b : « spaghetti des espaces verts ». Les polygones verts d'OSM se
			// CHEVAUCHENT largement (un parc porte souvent 3 ou 4 anneaux empiles :
			// leisure=park, landuse=grass, natural=wood...) ; y faire alterner trois
			// herbes differentes transformait chaque chevauchement en frontiere visible.
			// Une seule herbe (grass_cut) pour TOUS les verts : les chevauchements
			// deviennent invisibles, il n'y a plus rien a harmoniser.
			// bVariedGrass (defaut false) : l'alternance historique reste en code pour
			// un usage futur — berges, friches — la ou la variete se justifiera.
			const FSurfaceClass* GreenClass = &GSurfGrassCut;
			if (Gen.bVariedGrass && bForest)
			{
				GreenClass = (((uint32)GreenIndex * 2654435761u) >> 16) % 2u == 0u
					? &GSurfGrassUncut : &GSurfGrassWild;
			}
			BuildFlatPolygon(GetCell(Centroid(Pts)), Pts, (bForest ? 20.f : 12.f) + ZJitter,
				bForest ? ForestTint : ParkTint, GreenTerrainZPtr, Surfaces.Resolve(GreenClass));
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

	// -----------------------------------------------------------------------------
	// J3b — toits en pente ANCRES. Le squelette droit est PRECALCULE par
	// Tools/j3b_prep_toits.py (bpypolyskel) et livre dans le JSON batiments
	// (bloc "roof") : le C++ ne fait que mailler, z = egout + delta * d / maxd
	// (la pente est lineaire en d par construction du squelette). Tout bloc
	// incoherent -> toit plat historique, jamais d'erreur fatale.
	struct FRoofData
	{
		float EaveCm = 0.f;          // egout - sol (IGN alt_min_toit - alt_min_sol)
		float DeltaCm = 0.f;         // faitage - egout (borne [0,3 ; 30] m au prep)
		int32 Tile = 10;             // sous-tuile atlas (10 tuile, 11 ardoise/zinc, 13 beton)
		TArray<FVector3f> Skel;      // noeuds du squelette : x,y en cm, Z = retrait d en cm
		TArray<TArray<int32>> Faces; // versants ; idx < N = anneau, sinon Skel[idx - N]
		float MaxDcm = 0.f;
		// J3c — teinte de vertex du toit : couleur REELLE echantillonnee dans la BD
		// ORTHO par Tools/j3c_tint_toits.py (mediane robuste / gris de reference),
		// deja multipliee par le quasi-blanc historique. Defaut = ancien RoofTint.
		FVector3f Tint = FVector3f(0.95f, 0.95f, 0.95f);
	};

	int32 RoofTileFromMat(const FString& Mat)
	{
		if (Mat == TEXT("ardoise") || Mat == TEXT("zinc")) { return 11; }
		if (Mat == TEXT("beton")) { return 13; }
		return 10; // tuile terre cuite — defaut toulousain (verrou 1 : ~79 % tuile)
	}

	// Lit et VALIDE le bloc "roof" d'un batiment (N = nombre de points du contour).
	// Holes = anneaux de cour (peut etre vide) : l'espace d'indices des faces est alors
	// contour(N) ++ trous(HTotal) ++ squelette(Skel), et la 1re arete d'un versant peut
	// etre une arete du contour OU d'un trou (avant-toit de cour). Vide = comportement
	// historique bit-a-bit. false = pas de toit en pente (l'appelant retombe sur plat).
	bool ParseRoof(const TSharedPtr<FJsonObject>& Bldg, int32 N,
		const TArray<TArray<FVector2D>>& Holes, FRoofData& Out)
	{
		const TSharedPtr<FJsonObject>* RoofObj = nullptr;
		if (!Bldg->TryGetObjectField(TEXT("roof"), RoofObj))
		{
			return false;
		}
		double Eave = 0.0, Delta = 0.0;
		if (!(*RoofObj)->TryGetNumberField(TEXT("eave"), Eave) ||
			!(*RoofObj)->TryGetNumberField(TEXT("delta"), Delta) ||
			Eave < 2.0 || Delta < 0.3 || Delta > 30.0)
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Sv = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Fa = nullptr;
		if (!(*RoofObj)->TryGetArrayField(TEXT("sv"), Sv) || Sv->Num() == 0 ||
			!(*RoofObj)->TryGetArrayField(TEXT("f"), Fa) || Fa->Num() < 3)
		{
			return false;
		}
		Out.EaveCm = (float)(Eave * 100.0);
		Out.DeltaCm = (float)(Delta * 100.0);
		FString Mat;
		(*RoofObj)->TryGetStringField(TEXT("mat"), Mat);
		Out.Tile = RoofTileFromMat(Mat);
		Out.Skel.Reset(Sv->Num());
		Out.MaxDcm = 0.f;
		for (const TSharedPtr<FJsonValue>& V : *Sv)
		{
			const TArray<TSharedPtr<FJsonValue>>& C = V->AsArray();
			if (C.Num() < 3)
			{
				return false;
			}
			const FVector3f P((float)(C[0]->AsNumber() * 100.0),
				(float)(C[1]->AsNumber() * 100.0), (float)(C[2]->AsNumber() * 100.0));
			if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || P.Z < 0.f)
			{
				return false;
			}
			Out.MaxDcm = FMath::Max(Out.MaxDcm, P.Z);
			Out.Skel.Add(P);
		}
		if (Out.MaxDcm < 1.f)
		{
			return false;
		}
		// Sommets de BORD (contour + trous), tous a d = 0 ; le squelette vient apres.
		int32 HTotal = 0;
		for (const TArray<FVector2D>& H : Holes) { HTotal += H.Num(); }
		const int32 NB = N + HTotal;
		// Aretes de BORD admissibles comme 1re arete d'un versant (egout exterieur OU
		// avant-toit de cour) — contrat du prep (skeleton_faces valide l'identique).
		// Sans trou : Bnd = les seules aretes (i, i+1) du contour, donc ce test est
		// STRICTEMENT equivalent a l'ancien Face[0]<N && Face[1]<N && voisines.
		TSet<TPair<int32, int32>> Bnd;
		for (int32 i = 0; i < N; ++i) { Bnd.Add(TPair<int32, int32>(i, (i + 1) % N)); }
		{
			int32 Base = N;
			for (const TArray<FVector2D>& H : Holes)
			{
				const int32 M = H.Num();
				for (int32 i = 0; i < M; ++i) { Bnd.Add(TPair<int32, int32>(Base + i, Base + (i + 1) % M)); }
				Base += M;
			}
		}
		Out.Faces.Reset(Fa->Num());
		for (const TSharedPtr<FJsonValue>& FV : *Fa)
		{
			TArray<int32> Face;
			for (const TSharedPtr<FJsonValue>& IV : FV->AsArray())
			{
				const int32 Idx = (int32)IV->AsNumber();
				if (Idx < 0 || Idx >= NB + Out.Skel.Num())
				{
					return false;
				}
				Face.Add(Idx);
			}
			// Contrat du prep : la 1re arete de chaque versant = une arete de bord.
			if (Face.Num() < 3 || !Bnd.Contains(TPair<int32, int32>(Face[0], Face[1])))
			{
				return false;
			}
			Out.Faces.Add(MoveTemp(Face));
		}
		// J3c — "tint":[r,g,b] OPTIONNEL au niveau du batiment (l'ortho couvre aussi
		// les toits plats, qui n'ont pas de bloc roof). Absent ou aberrant -> quasi
		// blanc historique : aucune regression si le champ n'est pas la.
		const TArray<TSharedPtr<FJsonValue>>* TintArr = nullptr;
		if (Bldg->TryGetArrayField(TEXT("tint"), TintArr) && TintArr->Num() >= 3)
		{
			FVector3f T(0.f, 0.f, 0.f);
			bool bOk = true;
			for (int32 i = 0; i < 3; ++i)
			{
				const float V = (float)(*TintArr)[i]->AsNumber();
				if (!FMath::IsFinite(V))
				{
					bOk = false;
					break;
				}
				T[i] = FMath::Clamp(V, 0.3f, 2.0f) * 0.95f;
			}
			if (bOk)
			{
				Out.Tint = T;
			}
		}
		return true;
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
		float ZBaseCm, float SocleDepthCm, bool bReveals, bool bBakedShade,
		const TArray<TArray<FVector2D>>& Holes, const FRoofData* Roof = nullptr)
	{
		const int32 Floors = FMath::Clamp(FMath::RoundToInt32(Hcm / 290.f), 1, 40);
		const float FloorH = Hcm / Floors;
		const FVector3f StoneTint(0.82f, 0.79f, 0.72f);
		auto Col = [&](const FVector3f& C, const FVector3f& Nrm, float Zrel)
		{
			return bBakedShade ? Shade(C, Nrm, Zrel) : C;
		};
		// Un anneau de murs. Appele sur le CONTOUR (exterieur, CCW -> normale sortante)
		// puis, si cour, sur chaque TROU (CW -> la meme formule Nout(dy,-dx) pointe vers
		// l'interieur de la cour = la bonne face). Le corps est l'ancienne boucle d'aretes,
		// inchangee : sans cour, seul le contour est monte -> geometrie bit-a-bit identique.
		auto BuildWallRing = [&](const TArray<FVector2D>& Ring)
		{
		const int32 N = Ring.Num();
		for (int32 e = 0; e < N; ++e)
		{
			const FVector2D A2 = Ring[e];
			const FVector2D B2 = Ring[(e + 1) % N];
			const FVector2D Dir2 = B2 - A2;
			const float Len = Dir2.Size();
			// Seuil a 1 cm (pas 30) : au-dela de ~1 cm on MONTE le mur de l'arete,
			// meme courte. L'ancien saut a 30 cm laissait une FENTE verticale pleine
			// hauteur partout ou le contour a un petit redan / pan coupe de coin
			// (mesure : 46 778 aretes, 8 644 batiments = 6,58 %, dont le Capitole).
			// Les murs sont bout a bout aux sommets partages : monter chaque arete
			// rend l'anneau etanche. La garde U1-U0<1 de WallQuad couvre deja les
			// quads degeneres ; 1 cm evite juste le NaN de Dir2/Len sur un doublon.
			if (Len < 1.f)
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

		};
		BuildWallRing(PtsCm);
		// Cours : murs interieurs (face tournee vers la cour) montes UNIQUEMENT quand le
		// toit en pente a trous existe ; sinon le toit plat couvre la cour et on garde le
		// batiment plein (repli documente). Sans cour, seul le contour est monte : la
		// geometrie est bit-a-bit identique a l'historique.
		const bool bUseHoles = (Roof != nullptr) && (Holes.Num() > 0);
		if (bUseHoles)
		{
			for (const TArray<FVector2D>& H : Holes)
			{
				BuildWallRing(H);
			}
		}

		// N = nombre de sommets du CONTOUR (base de l'espace d'indices du toit).
		const int32 N = PtsCm.Num();
		// Toit : versants du squelette droit si fournis (J3b), sinon plat historique.
		// Quasi blanc en vertex color (la sous-tuile porte la couleur du materiau).
		// J3c : les VERSANTS prennent Roof->Tint (couleur ortho reelle du toit) ; le
		// toit plat garde ce quasi blanc — non-regression mobile intouchable.
		const FVector3f RoofTint(0.95f, 0.95f, 0.95f);
		if (Roof)
		{
			// Ici Hcm = hauteur d'EGOUT (les murs s'arretent a l'egout), le versant
			// monte de delta * d / maxd. UV0 : U le long de l'arete d'egout (1re
			// arete du versant, contrat du prep), V le long du rampant (longueur
			// reelle) — les rangees de tuiles restent paralleles a l'egout.
			const float SlopeLen = FMath::Sqrt(1.f + FMath::Square(Roof->DeltaCm / Roof->MaxDcm));
			// Espace de sommets du toit = contour(N) ++ trous(Holes, a plat) ++ squelette.
			// C'est le contrat EXACT du prep (j3b_prep_toits.py) : le C++ reconstruit ici
			// le meme espace pour poser les faces. Sans cour (Holes vide), Idx>=N tombe
			// directement sur le squelette -> mapping identique a l'historique.
			auto RoofVert = [&](int32 Idx, FVector2D& OutXY, float& OutD)
			{
				if (Idx < N) { OutXY = PtsCm[Idx]; OutD = 0.f; return; }
				int32 j = Idx - N;
				for (const TArray<FVector2D>& H : Holes)
				{
					if (j < H.Num()) { OutXY = H[j]; OutD = 0.f; return; }
					j -= H.Num();
				}
				OutXY = FVector2D(Roof->Skel[j].X, Roof->Skel[j].Y);
				OutD = Roof->Skel[j].Z;
			};
			for (const TArray<int32>& Face : Roof->Faces)
			{
				const int32 Nf = Face.Num();
				TArray<FVector3f> C;
				TArray<FVector2D> C2;
				TArray<float> D;
				C.Reserve(Nf);
				C2.Reserve(Nf);
				D.Reserve(Nf);
				for (const int32 Idx : Face)
				{
					FVector2D XY;
					float d;
					RoofVert(Idx, XY, d);
					C.Add(FVector3f((float)XY.X, (float)XY.Y,
						ZBaseCm + Hcm + Roof->DeltaCm * d / Roof->MaxDcm));
					C2.Add(XY);
					D.Add(d);
				}
				const FVector2D EaveDir = (C2[1] - C2[0]).GetSafeNormal();
				float U0 = FLT_MAX, U1 = -FLT_MAX, VMax = 1.f;
				TArray<FVector2f> UV;
				UV.Reserve(Nf);
				for (int32 i = 0; i < Nf; ++i)
				{
					const float U = (float)FVector2D::DotProduct(C2[i] - C2[0], EaveDir);
					U0 = FMath::Min(U0, U);
					U1 = FMath::Max(U1, U);
					VMax = FMath::Max(VMax, D[i] * SlopeLen);
					UV.Add(FVector2f(U, D[i] * SlopeLen));
				}
				const float USpan = FMath::Max(U1 - U0, 1.f);
				for (FVector2f& T : UV)
				{
					T = AtlasUV(Roof->Tile, (T.X - U0) / USpan, T.Y / VMax);
				}
				// Normale vraie du versant (Z force vers le haut). Tris par
				// ear-clipping de la projection XY : un toit est un champ de hauteur,
				// la projection d'un versant est donc un polygone simple (et le fan
				// des n-gons ne gere pas les versants non convexes).
				FVector3f Nrm = FVector3f::CrossProduct(C[1] - C[0], C[2] - C[0]).GetSafeNormal();
				if (Nrm.Z < 0.f)
				{
					Nrm = -Nrm;
				}
				if (Nrm.IsNearlyZero())
				{
					Nrm = FVector3f(0, 0, 1);
				}
				TArray<int32> Tris;
				TriangulateRing(C2, Tris);
				for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
				{
					const FVector3f P[3] = { C[Tris[t]], C[Tris[t + 1]], C[Tris[t + 2]] };
					const FVector2f TUV[3] = { UV[Tris[t]], UV[Tris[t + 1]], UV[Tris[t + 2]] };
					// J3c : teinte ortho du toit (Roof->Tint vaut RoofTint si absente).
					Wall.AddPoly(Wall.WallGroup, P, 3, Nrm, TUV, Col(Roof->Tint, Nrm, Hcm));
				}
			}
			return;
		}
		// Toit plat : sous-tuile toit terre cuite (10), UV0 = emprise normalisee.
		TArray<int32> Tris;
		TriangulateRing(PtsCm, Tris);
		FBox2D RoofBox(ForceInit);
		for (const FVector2D& P : PtsCm)
		{
			RoofBox += P;
		}
		const FVector2D RoofSize = RoofBox.GetSize();
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
	// Holes / Roof (cours J3b) : quand un batiment a un toit en pente a trous, la
	// collision devient un PUITS — murs interieurs de cour en plus, et planchers/toits
	// AJOURES (tessellation du toit = contour ++ trous ++ squelette, mise a plat) au lieu
	// de l'emprise pleine, pour que le sol de la cour reste ouvert (pas de "beton"). Sans
	// cour (Holes vide OU Roof nul), on garde EXACTEMENT le prisme plein historique.
	void BuildCollisionPrism(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm,
		float TopZCm, float BottomZCm,
		const TArray<TArray<FVector2D>>& Holes = TArray<TArray<FVector2D>>(),
		const FRoofData* Roof = nullptr)
	{
		const FVector3f White(1.f, 1.f, 1.f);
		// Un anneau de murs lateraux (contour ou cour). Meme quad des deux cotes de la
		// convention : la collision est double-face, l'orientation de la normale n'importe pas.
		auto SideWalls = [&](const TArray<FVector2D>& Ring)
		{
			const int32 M = Ring.Num();
			for (int32 e = 0; e < M; ++e)
			{
				const FVector2D A2 = Ring[e];
				const FVector2D B2 = Ring[(e + 1) % M];
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
		};
		SideWalls(PtsCm);
		const bool bUseHoles = (Roof != nullptr) && (Holes.Num() > 0);
		if (bUseHoles)
		{
			const int32 N = PtsCm.Num();
			for (const TArray<FVector2D>& H : Holes)
			{
				SideWalls(H);
			}
			// Espace de sommets = contour ++ trous ++ squelette (identique au toit).
			auto CapVert = [&](int32 Idx) -> FVector2D
			{
				if (Idx < N) { return PtsCm[Idx]; }
				int32 j = Idx - N;
				for (const TArray<FVector2D>& H : Holes)
				{
					if (j < H.Num()) { return H[j]; }
					j -= H.Num();
				}
				return FVector2D(Roof->Skel[j].X, Roof->Skel[j].Y);
			};
			// Planchers ajoures : chaque versant (polygone simple) triangule par ear-clip
			// (comme le rendu du toit) puis pose a plat, en haut ET en bas.
			for (const TArray<int32>& Face : Roof->Faces)
			{
				TArray<FVector2D> C2;
				C2.Reserve(Face.Num());
				for (const int32 Idx : Face)
				{
					C2.Add(CapVert(Idx));
				}
				TArray<int32> Tris;
				TriangulateRing(C2, Tris);
				for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
				{
					const FVector2D& P0 = C2[Tris[t]];
					const FVector2D& P1 = C2[Tris[t + 1]];
					const FVector2D& P2 = C2[Tris[t + 2]];
					QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, TopZCm), FVector3f(P1.X, P1.Y, TopZCm),
						FVector3f(P2.X, P2.Y, TopZCm), FVector3f(0, 0, 1), White);
					QM.AddTri(QM.WallGroup, FVector3f(P0.X, P0.Y, BottomZCm), FVector3f(P1.X, P1.Y, BottomZCm),
						FVector3f(P2.X, P2.Y, BottomZCm), FVector3f(0, 0, -1), White);
				}
			}
			return;
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
	// J3b : source batiments dediee (anneaux nettoyes CCW + toits precalcules du
	// prep). Routes, arbres et surfaces restent dans le JSON principal.
	TSharedPtr<FJsonObject> BldRoot = Root;
	if (!Gen.BuildingsJsonPath.IsEmpty())
	{
		FString BldJson;
		if (!FFileHelper::LoadFileToString(BldJson, *Gen.BuildingsJsonPath))
		{
			RaiseError(FString::Printf(TEXT("Cannot read buildings file '%s'."), *Gen.BuildingsJsonPath));
			return Summary;
		}
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(BldJson), BldRoot) || !BldRoot.IsValid())
		{
			RaiseError(TEXT("Buildings file is not valid JSON."));
			return Summary;
		}
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

	// J3b : sous-niveaux de blocs VISIBLES en editeur AVANT la purge — DestroyActor
	// echoue en silence sur un niveau invisible (constate sur le proto : generations
	// empilees 8 -> 16), et un niveau sauve invisible rend la ville « proxys seuls »
	// dans l'editeur CityLab (3 sessions de diagnostic payees le 25/07).
	for (ULevelStreaming* S : World->GetStreamingLevels())
	{
		if (S && S->GetWorldAssetPackageName().StartsWith(BlocksFolder))
		{
			S->SetShouldBeVisibleInEditor(true);
			if (ULevel* Lvl = S->GetLoadedLevel())
			{
				UEditorLevelUtils::SetLevelVisibility(Lvl, true, false);
			}
		}
	}

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
	if (BldRoot->TryGetArrayField(TEXT("buildings"), BuildingsJson))
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
			bool bReversed = false;
			if (SignedArea(Pts) < 0)
			{
				// Un JSON du prep J3b est deja CCW ; un anneau legacy peut ne pas
				// l'etre — dans ce cas les indices de toit seraient invalides.
				Algo::Reverse(Pts);
				bReversed = true;
			}
			FVector2D Centroid(0, 0);
			for (const FVector2D& P : Pts) { Centroid += P; }
			Centroid /= Pts.Num();
			// Cours interieures (J3b cours) : anneaux CW (metres) livres par
			// j3b_ajoute_cours.py. Chaque trou -> mur de cour + toit qui retombe vers
			// l'avant-toit interieur + collision ajouree. Ignore si le contour a du etre
			// reoriente (bReversed) : l'alignement des indices toit/trous exige le meme
			// contour CCW que le prep. Absent = batiment plein (compat totale).
			TArray<TArray<FVector2D>> Holes;
			const TArray<TSharedPtr<FJsonValue>>* HolesJson = nullptr;
			if (!bReversed && O->TryGetArrayField(TEXT("holes"), HolesJson))
			{
				for (const TSharedPtr<FJsonValue>& HV : *HolesJson)
				{
					TArray<FVector2D> Hole;
					ReadPts(HV->AsArray(), Hole);
					if (Hole.Num() >= 3)
					{
						Holes.Add(MoveTemp(Hole));
					}
				}
			}
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
				// J3b : toit en pente si le bloc "roof" est present et coherent — les
				// murs s'arretent alors a l'EGOUT et le versant monte au faitage
				// (coherent avec h = faitage - sol de la BD TOPO). Le prisme de
				// collision reste a ZBase + h (plan du faitage).
				FRoofData Roof;
				const bool bPitched = !bReversed && ParseRoof(O, Pts.Num(), Holes, Roof)
					&& Roof.EaveCm <= Hcm + 50.f;
				const float WallHcm = bPitched ? FMath::Min(Roof.EaveCm, Hcm) : Hcm;
				// Lot B : fenetres geometriques (en creux si bWindowReveals) ; les
				// vitres partent dans un builder SEPARE si bSplitWallGlass (Q3).
				FCityMeshBuilder& WallB = GetIn(BldgCells, Centroid, Cell, bLinearColors, bWorldUVs);
				FCityMeshBuilder& GlassB = Gen.bSplitWallGlass
					? GetIn(BldgGlassCells, Centroid, Cell, bLinearColors, false) : WallB;
				BuildPolygonBuildingDesktop(WallB, GlassB, Pts, WallHcm, Tint, UsageTile(Usage, Index),
					ZBase, SocleDepth, Gen.bWindowReveals, bBakedShade, Holes, bPitched ? &Roof : nullptr);
				if (bPitched)
				{
					++Summary.RoofsPitched;
				}
				// Verrou 2 : prisme de collision dedie, meme pose — les murs Nanite ne
				// servent JAMAIS de collision (fallback decime = facades traversables).
				BuildCollisionPrism(GetIn(BldgColCells, Centroid, Cell), Pts,
					ZBase + Hcm, ZBase - SocleDepth, Holes, bPitched ? &Roof : nullptr);
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
	// J3c point 2 : classe de revetement par DONNEE (tag OSM "surface"), a defaut par
	// type + nombre de voies. Desactive (mobile) = ruban T_RoadStrip historique.
	FSurfaceLibrary Surfaces;
	Surfaces.Init(Gen.bSurfaceMaterials, Gen.SurfacesFolder);

	// --- J3c maquette du sol : releve des cellules qui ont un masque cuit.
	// La liste vient du DISQUE et non des routes : avec la chaussee peinte, une
	// cellule peut n'avoir plus aucun ruban et devoir quand meme sa dalle.
	// bMaskedGround exige bSurfaceMaterials (le melange se fait avec les memes
	// scans) — sans lui, on ne bascule pas et le comportement actuel tient.
	const bool bMaskedGround = Gen.bMaskedGround && Gen.bSurfaceMaterials;
	TMap<FIntPoint, FGroundMaskCell> MaskCells;
	if (bMaskedGround)
	{
		const FString Dir = GroundMasksDir(Gen);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Dir / TEXT("sols_*.json")), true, false);
		for (const FString& File : Files)
		{
			FString Rest = FPaths::GetBaseFilename(File);
			Rest.RemoveFromStart(TEXT("sols_"));
			FString Sx, Sy;
			if (!Rest.Split(TEXT("_"), &Sx, &Sy, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
			{
				continue;
			}
			const FIntPoint Key(FCString::Atoi(*Sx), FCString::Atoi(*Sy));
			FGroundMaskCell Data;
			if (LoadGroundMaskCell(Dir, Key.X, Key.Y, CellSizeM, Data))
			{
				MaskCells.Add(Key, MoveTemp(Data));
				SlabKeys.Add(Key);
			}
		}
		if (MaskCells.Num() == 0)
		{
			UE_LOG(LogCityImport, Warning,
				TEXT("Maquette du sol demandee mais aucun masque dans '%s' : les rubans de chaussee restent generes."),
				*Dir);
		}
	}
	// Une cellule N'A de dalle masquee que si elle a A LA FOIS son JSON et son
	// instance de materiau : sans le materiau, la peindre reviendrait a effacer la
	// chaussee sans rien mettre a la place.
	const FString MaskAssetDir = GroundMasksAssetDir(Gen);
	auto LoadCellMaskMaterial = [&MaskAssetDir](const FIntPoint& Key) -> UMaterialInterface*
	{
		const FString Path = FString::Printf(TEXT("%s/MI_CityGround_%d_%d.MI_CityGround_%d_%d"),
			*MaskAssetDir, Key.X, Key.Y, Key.X, Key.Y);
		return LoadObject<UMaterialInterface>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	};
	// Le basculement est GLOBAL et non par cellule : supprimer les rubans sur une
	// cellule peinte et les garder sur sa voisine ferait une couture visible en
	// plein milieu d'une rue. Si un seul materiau de cellule manque, on ne bascule
	// pas du tout et on le dit.
	bool bMaskedActive = bMaskedGround && MaskCells.Num() > 0;
	if (bMaskedActive)
	{
		int32 Missing = 0;
		for (const TPair<FIntPoint, FGroundMaskCell>& Pair : MaskCells)
		{
			if (!LoadCellMaskMaterial(Pair.Key))
			{
				++Missing;
			}
		}
		if (Missing > 0)
		{
			bMaskedActive = false;
			UE_LOG(LogCityImport, Warning,
				TEXT("Maquette du sol : %d/%d instances MI_CityGround_* absentes sous '%s' — bascule ANNULEE (lancer Tools/import_ground_masks.py)."),
				Missing, MaskCells.Num(), *MaskAssetDir);
		}
	}

	// Largeur de ruban : la MEME regle que BuildRoad — elle sert au rayon des patchs
	// de carrefour, donc a l'emprise ou la bordure s'interrompt. v5 : une chaussee
	// auto emporte desormais chant + rive (chaussee/2 + 15 + 170) ; le reste garde la
	// regle historique (chaussee/2 + 170, ou + 0 pour une voie pietonne).
	auto RibbonHalfCm = [](const FString& Type, float WidthCm, const FSurfaceClass* Class)
	{
		if (Class && Class->bAuto)
		{
			return WidthCm * 0.5f + GCurbTopWidthCm + GSidewalkWidthCm;
		}
		const bool bWalkway = Type == TEXT("footway") || Type == TEXT("path") || Type == TEXT("cycleway");
		return WidthCm * 0.5f + (bWalkway ? 0.f : 170.f);
	};
	auto ReadRoadTags = [](const TSharedPtr<FJsonObject>& O, FString& OutSurface, int32& OutLanes)
	{
		OutSurface.Empty();
		OutLanes = 0;
		O->TryGetStringField(TEXT("surface"), OutSurface);
		O->TryGetNumberField(TEXT("lanes"), OutLanes);
	};
	// v5 point 2 — SITE DE PASSAGE PIETON. Un noeud partage entre une CHAUSSEE AUTO et
	// une voie PIETONNE : la voie pietonne ne produit plus de ruban depuis la v4, mais
	// elle existe toujours dans la donnee — c'est elle qui dit ou la vraie ville avait
	// un passage. On retient, par noeud, la chaussee la plus LARGE (deterministe) avec
	// sa tangente au noeud : le quad se posera dans l'axe de cette rue.
	struct FCrossingSite
	{
		FVector2D PosCm = FVector2D::ZeroVector;
		FVector2D DirCm = FVector2D::ZeroVector;
		float RoadHalfCm = -1.f;
		float ZClassCm = 0.f;
	};
	TMap<FIntPoint, FCrossingSite> CrossingSites;
	TSet<FIntPoint> PedestrianNodes;

	// v2 — PASSE 1 : releve des noeuds partages (sur les points D'ORIGINE du JSON,
	// pas les points re-echantillonnes : seuls les noeuds OSM sont partages).
	FJunctionMap Junctions;
	const TArray<TSharedPtr<FJsonValue>>* RoadsJson = nullptr;
	if (Gen.bSurfaceMaterials && Root->TryGetArrayField(TEXT("roads"), RoadsJson))
	{
		int32 Index = 0;
		for (const TSharedPtr<FJsonValue>& V : *RoadsJson)
		{
			const TSharedPtr<FJsonObject>& O = V->AsObject();
			TArray<FVector2D> Pts;
			ReadPts(O->GetArrayField(TEXT("pts")), Pts);
			const FString Type = O->GetStringField(TEXT("t"));
			// v4 : une voie pietonne ne produit plus de ruban — elle ne doit donc plus
			// peser sur les carrefours, ni par son compte de voies, ni comme dominante
			// (une place pietonne large aurait sinon interdit le patch d'un vrai
			// croisement de voitures qu'elle traverse).
			if (Pts.Num() >= 2 && IsPedestrianRibbon(Type))
			{
				for (const FVector2D& P : Pts)
				{
					PedestrianNodes.Add(FJunctionMap::Key(P));
				}
			}
			else if (Pts.Num() >= 2)
			{
				const float WidthCm = O->GetNumberField(TEXT("w")) * 100.f;
				FString SurfaceTag;
				int32 Lanes = 0;
				ReadRoadTags(O, SurfaceTag, Lanes);
				const FSurfaceClass* Class = SurfaceClassForRoad(SurfaceTag, Type, Lanes, WidthCm);
				Junctions.Add(Index, Pts, RibbonHalfCm(Type, WidthCm, Class), Class);
				if (Class && Class->bAuto)
				{
					for (int32 i = 0; i < Pts.Num(); ++i)
					{
						FCrossingSite& Site = CrossingSites.FindOrAdd(FJunctionMap::Key(Pts[i]));
						if (WidthCm * 0.5f <= Site.RoadHalfCm)
						{
							continue;
						}
						// Tangente au noeud : moyenne des segments adjacents, comme les
						// normales de BuildRoad — le passage reste dans l'axe meme sur un
						// sommet de virage.
						FVector2D D(0, 0);
						if (i > 0) { D += (Pts[i] - Pts[i - 1]).GetSafeNormal(); }
						if (i + 1 < Pts.Num()) { D += (Pts[i + 1] - Pts[i]).GetSafeNormal(); }
						if (D.IsNearlyZero())
						{
							continue;
						}
						Site.PosCm = Pts[i];
						Site.DirCm = D.GetSafeNormal();
						Site.RoadHalfCm = WidthCm * 0.5f;
						Site.ZClassCm = Class->ZClassCm;
					}
				}
			}
			++Index;
		}
		Junctions.Build();
		UE_LOG(LogCityImport, Display,
			TEXT("Carrefours : %d noeuds partages sur %d releves, dont %d a patcher (>= 2 chaussees auto)."),
			Junctions.NumJunctions, Junctions.Nodes.Num(), Junctions.NumAutoJunctions);
	}
	RoadsJson = nullptr;
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
			// v4 — LE PIETON EST LA DALLE : en profil revetements, aucune voie pietonne
			// ne produit de ruban. C'est LA simplification structurelle de la v4 (le
			// lacis pieton du centre faisait a lui seul l'effet « grand puzzle »). Le
			// golden path mobile n'est pas concerne : sans revetements, les sentiers
			// gardent leur ruban uni historique, a l'octet pres.
			if (Gen.bSurfaceMaterials && IsPedestrianRibbon(O->GetStringField(TEXT("t"))))
			{
				++Index;
				continue;
			}
			// J3c maquette — LA CHAUSSEE N'EST PLUS UN FILM POSE SUR LA DALLE. Une
			// fois le sol peint, un ruban au niveau du sol ne ferait que doubler la
			// peinture (et se battre avec elle en profondeur). Seuls survivent les
			// PONTS : leur tablier est au-dessus du terrain, aucun masque de sol ne
			// peut le rendre.
			if (bMaskedActive)
			{
				bool bBridgeRibbon = false;
				O->TryGetBoolField(TEXT("bridge"), bBridgeRibbon);
				if (!bBridgeRibbon)
				{
					++Summary.GroundRibbonsSkipped;
					++Index;
					continue;
				}
				++Summary.BridgeRibbons;
			}
			// v5 point 4 — FRAGMENT ORPHELIN. Court (< 25 m) ET sans le moindre noeud
			// partage avec le reste du reseau : ce n'est pas une rue, c'est un bout de
			// voie coupe par la fenetre d'extraction, pose seul sur la dalle (« morceaux
			// perdus », verdict v4b). La connexite se lit dans la carte des noeuds deja
			// relevee en passe 1, sur les points D'ORIGINE (les seuls partages).
			if (Gen.bSurfaceMaterials && Junctions.Nodes.Num() > 0)
			{
				float LenCm = 0.f;
				for (int32 i = 0; i + 1 < Pts.Num(); ++i)
				{
					LenCm += (float)(Pts[i + 1] - Pts[i]).Size();
				}
				if (LenCm < GOrphanMaxLenCm)
				{
					bool bConnected = false;
					for (const FVector2D& P : Pts)
					{
						const FJunctionNode* Node = Junctions.Nodes.Find(FJunctionMap::Key(P));
						if (Node && Node->NumRoads >= 2)
						{
							bConnected = true;
							break;
						}
					}
					if (!bConnected)
					{
						++Summary.OrphanRibbons;
						++Index;
						continue;
					}
				}
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
			// Champs OSM optionnels, lus TOLERANT (absents sur les JSON historiques).
			const float RoadWidthCm = O->GetNumberField(TEXT("w")) * 100.f;
			const FString RoadType = O->GetStringField(TEXT("t"));
			FString RoadSurface;
			int32 RoadLanes = 0;
			ReadRoadTags(O, RoadSurface, RoadLanes);
			const FResolvedSurface* RoadSurf =
				Surfaces.Resolve(SurfaceClassForRoad(RoadSurface, RoadType, RoadLanes, RoadWidthCm));
			// v2 : drapeau « sommet au contact d'un carrefour » (les tirets axiaux y
			// cedent la place a l'asphalte nu). Calcule seulement pour les classes
			// marquees — inutile de sonder la grille pour un trottoir.
			const FResolvedSurface* PlainSurf = nullptr;
			TArray<uint8> NearJunction;
			if (RoadSurf && RoadSurf->Class->bFullWidth && Junctions.NumJunctions > 0)
			{
				PlainSurf = Surfaces.Resolve(&GSurfAsphalt);
				NearJunction.SetNumUninitialized(RoadPts->Num());
				for (int32 i = 0; i < RoadPts->Num(); ++i)
				{
					NearJunction[i] = Junctions.IsNear((*RoadPts)[i], GJunctionPlainCm) ? 1 : 0;
				}
			}
			// v5 : rives + bordures (chaussees auto uniquement). CurbSurf/SlabSurf nuls
			// en profil mobile -> BuildRoad reprend son chemin d'un seul quad.
			const FResolvedSurface* CurbSurf = Surfaces.Resolve(&GSurfCurb);
			const FResolvedSurface* RibbonSlabSurf = Surfaces.Resolve(&GSurfSlab);
			BuildRoad(GetIn(GroundCells, Pts[0], Cell, bLinearColors, bWorldUVs), *RoadPts,
				RoadWidthCm, RoadType, Index, TerrainZPtr, bBakedShade, RoadSurf,
				PlainSurf, PlainSurf ? &NearJunction : nullptr,
				CurbSurf, RibbonSlabSurf, &Junctions, &Summary.CurbQuads);
			SlabKeys.Add(FIntPoint(FMath::FloorToInt(Pts[0].X / Cell), FMath::FloorToInt(Pts[0].Y / Cell)));
			++Summary.Roads;
			++Index;
		}
		// v2 — PASSE 3 : patchs de carrefour. Un disque du revetement DOMINANT
		// (classe de la voie la plus large) recouvre le disque de rencontre, pose
		// au-dessus du ruban le plus haut du noeud. Si la dominante est une classe
		// MARQUEE, le patch prend son equivalent NU : un croisement n'a jamais de
		// ligne axiale en son milieu.
		// v3 : WantsPatch() — reserve aux noeuds ou au moins DEUX chaussees auto se
		// rencontrent (zero patch dans le lacis pieton, cf. « peau de leopard »).
		for (const TPair<FIntPoint, FJunctionNode>& Pair : Junctions.Nodes)
		{
			const FJunctionNode& Node = Pair.Value;
			// J3c maquette : un carrefour PEINT n'a pas besoin d'un disque de
			// rattrapage — il n'y a plus de rubans a raccorder.
			if (bMaskedActive || !Node.WantsPatch() || Node.MaxHalfCm < 150.f)
			{
				continue;
			}
			const FSurfaceClass* PatchClass = Node.Dominant->bFullWidth ? &GSurfAsphalt : Node.Dominant;
			const FResolvedSurface* PatchSurf = Surfaces.Resolve(PatchClass);
			if (!PatchSurf)
			{
				continue;
			}
			const float Zcm = 55.f + Node.MaxZClassCm + GJunctionPatchLiftCm
				+ Drape.GroundZ(Node.PosCm.X, Node.PosCm.Y);
			BuildJunctionPatch(GetIn(GroundCells, Node.PosCm, Cell, bLinearColors, bWorldUVs),
				Node.PosCm, Node.MaxHalfCm + GJunctionPatchMarginCm, Zcm, PatchSurf,
				FVector3f(0.85f, 0.85f, 0.80f));
			SlabKeys.Add(FIntPoint(FMath::FloorToInt(Node.PosCm.X / Cell),
				FMath::FloorToInt(Node.PosCm.Y / Cell)));
			++Summary.JunctionPatches;
		}

		// v5 — PASSE 4 : PASSAGES PIETONS. Un quad par noeud partage entre une chaussee
		// auto et une voie pietonne, en travers de la CHAUSSEE SEULE, au-dessus du
		// ruban (+9 cm) et donc au-dessus du patch de ce noeud (+5 cm), mais SOUS le
		// chant des bordures (+12 cm) : le passage s'arrete au pied du trottoir.
		// Noeud deja couvert par un disque de patch : passage REPORTE (compte a part).
		// Le raccord propre bord-de-patch (passage pose en amont de l'entree du
		// carrefour) est au backlog — ici, un carrefour reste une zone de roulement nue.
		if (Gen.bSurfaceMaterials && !bMaskedActive && CrossingSites.Num() > 0)
		{
			const FResolvedSurface* CrossSurf = Surfaces.Resolve(&GSurfCrossing);
			for (const TPair<FIntPoint, FCrossingSite>& Pair : CrossingSites)
			{
				const FCrossingSite& Site = Pair.Value;
				if (Site.RoadHalfCm <= 0.f || !PedestrianNodes.Contains(Pair.Key))
				{
					continue;
				}
				if (Junctions.IsInPatch(Site.PosCm))
				{
					++Summary.CrossingsDeferred;
					continue;
				}
				if (!CrossSurf)
				{
					continue;
				}
				const float Zcm = 55.f + Site.ZClassCm + GCrossingLiftCm
					+ Drape.GroundZ(Site.PosCm.X, Site.PosCm.Y);
				BuildCrossing(GetIn(GroundCells, Site.PosCm, Cell, bLinearColors, bWorldUVs),
					Site.PosCm, Site.DirCm, Site.RoadHalfCm, Zcm, CrossSurf,
					FVector3f(0.85f, 0.85f, 0.80f));
				SlabKeys.Add(FIntPoint(FMath::FloorToInt(Site.PosCm.X / Cell),
					FMath::FloorToInt(Site.PosCm.Y / Cell)));
				++Summary.Crossings;
			}
		}
		if (Gen.bSurfaceMaterials)
		{
			UE_LOG(LogCityImport, Display,
				TEXT("Voirie : %d quads de bordure, %d passages pietons poses, %d reportes (patch), %d rubans orphelins ecartes."),
				Summary.CurbQuads, Summary.Crossings, Summary.CrossingsDeferred, Summary.OrphanRibbons);
		}
	}

	// --- J3c maquette : LE RELIEF, depuis le masque. Trois passes tres courtes —
	// tout le decoupage a ete fait au prep, il ne reste qu'a poser des quads. Elles
	// vivent dans les cellules de RUBANS (SM_Ground_*, sans collision, cullables) :
	// une bordure ou un tiret n'a rien a faire dans la dalle porteuse.
	if (bMaskedActive)
	{
		const FResolvedSurface* CurbSurf = Surfaces.Resolve(&GSurfCurb);
		const FResolvedSurface* CrossSurf = Surfaces.Resolve(&GSurfCrossing);
		const FResolvedSurface* MarkSurf = Surfaces.Resolve(&GSurfMarking);
		const FVector3f Tint(0.85f, 0.85f, 0.80f);
		for (const TPair<FIntPoint, FGroundMaskCell>& Pair : MaskCells)
		{
			const FGroundMaskCell& Data = Pair.Value;
			for (const TArray<FVector2D>& Line : Data.Curbs)
			{
				BuildMaskCurb(GetIn(GroundCells, Line[0], Cell, bLinearColors, bWorldUVs),
					Line, Drape, CurbSurf, Tint, &Summary.CurbQuads);
			}
			for (const FMaskCrossing& Site : Data.Crossings)
			{
				const float Zcm = Drape.GroundZ(Site.PosCm.X, Site.PosCm.Y) + GMaskCrossLiftCm;
				BuildCrossing(GetIn(GroundCells, Site.PosCm, Cell, bLinearColors, bWorldUVs),
					Site.PosCm, Site.DirCm, Site.HalfWCm, Zcm, CrossSurf, Tint);
				++Summary.Crossings;
			}
			for (const FVector4& Seg : Data.Axial)
			{
				const FVector2D A(Seg.X, Seg.Y);
				BuildAxialDash(GetIn(GroundCells, A, Cell, bLinearColors, bWorldUVs),
					A, FVector2D(Seg.Z, Seg.W), Drape, MarkSurf, Tint);
				++Summary.AxialDashes;
			}
		}
		UE_LOG(LogCityImport, Display,
			TEXT("Maquette du sol : %d cellules masquees, %d rubans de chaussee supprimes, %d ponts conserves, %d quads de bordure, %d passages, %d tirets axiaux."),
			MaskCells.Num(), Summary.GroundRibbonsSkipped, Summary.BridgeRibbons,
			Summary.CurbQuads, Summary.Crossings, Summary.AxialDashes);
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
	// v4 — LA DALLE PORTE LA MATIERE. Resolue une fois : nulle en profil mobile (la
	// dalle reste la grille peinte historique, a l'octet pres) et nulle pour le mesh
	// de COLLISION (jamais rendu — lui donner un slot de revetement serait du gachis).
	const FResolvedSurface* SlabSurf = Surfaces.Resolve(&GSurfSlab);
	auto BuildGroundGrid = [&](FCityMeshBuilder& Builder, const FIntPoint& Key, int32 GridN, bool bPaint,
		const FResolvedSurface* Surf)
	{
		const float Step = Cell / GridN;
		// Groupe de revetement cree UNE fois pour toute la dalle (pas par quad).
		const FPolygonGroupID Group = Surf
			? Builder.GetOrCreateGroup(Surf->SlotName(), Surf->Material) : Builder.WallGroup;
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
				// UV0 : historiquement [0,1] PAR QUAD (le materiau de dalle ne lisait
				// que la VertexColor). v4 : UV0 EN METRES MONDE, comme les rubans —
				// c'est ce qui donne au scan sa vraie echelle et, surtout, ce qui fait
				// que la dalle et le ruban qui la recouvre parlent la meme langue.
				FVector2f UV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
				if (Surf)
				{
					for (int32 c = 0; c < 4; ++c)
					{
						UV[c] = FVector2f(C[c].X * 0.01f, C[c].Y * 0.01f);
					}
				}
				FVector3f Cols[4];
				for (int32 c = 0; c < 4; ++c)
				{
					const FVector3f Base = bPaint ? SampleGround(FVector2D(C[c].X, C[c].Y)) : SlabBase;
					Cols[c] = bBakedShade ? Shade(Base, FVector3f(0, 0, 1), 0.f) : Base;
				}
				Builder.AddPolyPerVertexColors(Group, C, 4, FVector3f(0, 0, 1), UV, Cols);
			}
		}
	};
	// J3c maquette : l'instance de la CELLULE, resolue comme une classe de
	// revetement de plus (meme mecanique de slot, meme UV0 metrique monde — c'est
	// exactement ce que le master attend). Une seule difference : son materiau
	// change d'une cellule a l'autre, donc elle se resout dans la boucle.
	FResolvedSurface MaskedSurf;
	MaskedSurf.Class = &GSurfMaskedGround;
	for (const FIntPoint& Key : SlabKeys)
	{
		FCityMeshBuilder SlabBuilder;
		SlabBuilder.bLinearColors = bLinearColors;
		if (bWorldUVs)
		{
			SlabBuilder.EnableWorldUV1();
		}
		const FResolvedSurface* CellSurf = SlabSurf;
		if (bMaskedActive && MaskCells.Contains(Key))
		{
			MaskedSurf.Material = LoadCellMaskMaterial(Key);
			if (MaskedSurf.Material)
			{
				CellSurf = &MaskedSurf;
				++Summary.MaskedCells;
			}
		}
		BuildGroundGrid(SlabBuilder, Key, SlabGrid, /*bPaint=*/true, CellSurf);
		const FString SlabName = FString::Printf(TEXT("SM_Slab_%d_%d"), Key.X, Key.Y);
		UStaticMesh* SlabMesh = nullptr;
		if (Drape.IsActive() && Gen.GroundCollisionGridN > 0)
		{
			// Collision desktop : la boite plate est fausse des que le terrain ondule
			// — trimesh BASSE RESOLUTION dedie (16x16), cuit a la place du rendu 64x64
			// via ComplexCollisionMesh (le trimesh plein serait du gachis memoire).
			// Le mesh de collision n'est jamais rendu : ni Nanite ni materiau PBR.
			FCityMeshBuilder ColBuilder;
			BuildGroundGrid(ColBuilder, Key, FMath::Clamp(Gen.GroundCollisionGridN, 1, 64), /*bPaint=*/false,
				/*Surf=*/nullptr);
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
		// J3b : visibilite EDITEUR distincte des flags runtime ci-dessus — elle se
		// sauve avec la map ; invisible, la ville n'affiche que ses proxys.
		Streaming->SetShouldBeVisibleInEditor(true);
		ULevel* BlockLevel = Streaming->GetLoadedLevel();
		if (!BlockLevel)
		{
			World->FlushLevelStreaming(EFlushLevelStreamingType::Full);
			BlockLevel = Streaming->GetLoadedLevel();
		}
		if (BlockLevel)
		{
			UEditorLevelUtils::SetLevelVisibility(BlockLevel, true, false);
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
		TEXT("Ville streamee : %d batiments, %d routes, %d arbres — %d sols, %d proxys, %d meshes detail, %d collisions batiments, %d blocs, %d patchs de carrefour, %d quads de bordure, %d passages pietons (%d reportes), %d orphelins ecartes. Tout est sauve."),
		Summary.Buildings, Summary.Roads, Summary.Trees, Summary.GroundMeshes, Summary.ProxyMeshes,
		Summary.BuildingMeshes, Summary.BuildingColMeshes, Summary.StreamingBlocks,
		Summary.JunctionPatches, Summary.CurbQuads, Summary.Crossings, Summary.CrossingsDeferred,
		Summary.OrphanRibbons);
	if (Summary.MaskedCells > 0)
	{
		UE_LOG(LogCityImport, Display,
			TEXT("Maquette du sol : %d dalles peintes, %d rubans de chaussee supprimes, %d ponts, %d tirets axiaux."),
			Summary.MaskedCells, Summary.GroundRibbonsSkipped, Summary.BridgeRibbons,
			Summary.AxialDashes);
	}
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
