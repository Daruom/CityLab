#include "CityImportTools.h"

#include "Algo/Reverse.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextRenderActor.h"
#include "Components/TextRenderComponent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Misc/FileHelper.h"
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

	// Copie assumee du builder de BuildingTools.cpp, version couleur RGB + triangles.
	// A consolider avec BuildingTools une fois le pipeline ville valide.
	struct FCityMeshBuilder
	{
		FMeshDescription MeshDesc;
		FStaticMeshAttributes Attributes;
		FPolygonGroupID WallGroup;
		FPolygonGroupID GlassGroup;
		int32 QuadCount = 0;

		FCityMeshBuilder()
			: Attributes(MeshDesc)
		{
			Attributes.Register();
			WallGroup = MeshDesc.CreatePolygonGroup();
			GlassGroup = MeshDesc.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[WallGroup] = FName(TEXT("Wall"));
			Attributes.GetPolygonGroupMaterialSlotNames()[GlassGroup] = FName(TEXT("Glass"));
		}

		// Compensation gamma : le build encode en sRGB, le shader lit brut (cf. BuildingTools).
		static FVector4f Encode(const FVector3f& C)
		{
			return FVector4f(FMath::Pow(FMath::Clamp(C.X, 0.f, 1.f), 2.2f),
				FMath::Pow(FMath::Clamp(C.Y, 0.f, 1.f), 2.2f),
				FMath::Pow(FMath::Clamp(C.Z, 0.f, 1.f), 2.2f), 1.f);
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

	// Ruban de route : asphalte, trottoirs, marquage central.
	void BuildRoad(FCityMeshBuilder& QM, const TArray<FVector2D>& PtsCm, float WidthCm,
		const FString& Type, int32 RoadIndex)
	{
		const int32 N = PtsCm.Num();
		if (N < 2)
		{
			return;
		}
		// Leger decalage vertical par route pour eviter le z-fight aux croisements.
		const float ZRoad = 4.f + (RoadIndex % 7) * 0.6f;
		const float ZMark = ZRoad + 4.f;
		const float ZWalk = 20.f;
		const bool bWalkway = Type == TEXT("footway") || Type == TEXT("path") || Type == TEXT("cycleway");
		const bool bMarking = !bWalkway && WidthCm >= 550.f;
		const float WalkW = bWalkway ? 0.f : 170.f;
		const FVector3f Asphalt = bWalkway ? FVector3f(0.48f, 0.45f, 0.42f) : FVector3f(0.17f, 0.17f, 0.18f);
		const FVector3f Walk = FVector3f(0.50f, 0.48f, 0.46f);
		const FVector3f Mark = FVector3f(0.85f, 0.85f, 0.80f);
		const FVector3f Up = FVector3f(0, 0, 1);

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

		const float Half = WidthCm * 0.5f;
		float Arc = 0.f;
		for (int32 i = 0; i + 1 < N; ++i)
		{
			const FVector2D A = PtsCm[i], B = PtsCm[i + 1];
			const FVector2D NA = Nrm[i] * Half, NB = Nrm[i + 1] * Half;
			// Asphalte
			QM.AddQuad(QM.WallGroup,
				FVector3f(A.X - NA.X, A.Y - NA.Y, ZRoad), FVector3f(B.X - NB.X, B.Y - NB.Y, ZRoad),
				FVector3f(B.X + NB.X, B.Y + NB.Y, ZRoad), FVector3f(A.X + NA.X, A.Y + NA.Y, ZRoad),
				Up, Asphalt);
			// Trottoirs
			if (WalkW > 0.f)
			{
				const FVector2D WA = Nrm[i] * (Half + WalkW), WB = Nrm[i + 1] * (Half + WalkW);
				QM.AddQuad(QM.WallGroup,
					FVector3f(A.X + NA.X, A.Y + NA.Y, ZWalk), FVector3f(B.X + NB.X, B.Y + NB.Y, ZWalk),
					FVector3f(B.X + WB.X, B.Y + WB.Y, ZWalk), FVector3f(A.X + WA.X, A.Y + WA.Y, ZWalk),
					Up, Walk);
				QM.AddQuad(QM.WallGroup,
					FVector3f(A.X - WA.X, A.Y - WA.Y, ZWalk), FVector3f(B.X - WB.X, B.Y - WB.Y, ZWalk),
					FVector3f(B.X - NB.X, B.Y - NB.Y, ZWalk), FVector3f(A.X - NA.X, A.Y - NA.Y, ZWalk),
					Up, Walk);
			}
			// Marquage central : tirets 300 cm pleins / 300 cm vides (plein si voie majeure).
			if (bMarking)
			{
				const bool bSolid = Type == TEXT("primary") || Type == TEXT("secondary");
				const float SegLen = (B - A).Size();
				const FVector2D Dir = (B - A) / FMath::Max(SegLen, 1.f);
				const FVector2D Side(-Dir.Y * 8.f, Dir.X * 8.f);
				float S = 0.f;
				while (S < SegLen)
				{
					const float E = FMath::Min(S + 300.f, SegLen);
					const bool bDash = bSolid || FMath::Fmod(Arc + S, 600.f) < 300.f;
					if (bDash)
					{
						const FVector2D P0 = A + Dir * S, P1 = A + Dir * E;
						QM.AddQuad(QM.WallGroup,
							FVector3f(P0.X - Side.X, P0.Y - Side.Y, ZMark), FVector3f(P1.X - Side.X, P1.Y - Side.Y, ZMark),
							FVector3f(P1.X + Side.X, P1.Y + Side.Y, ZMark), FVector3f(P0.X + Side.X, P0.Y + Side.Y, ZMark),
							Up, Mark);
					}
					S = E;
				}
				Arc += SegLen;
			}
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

	UStaticMesh* CreateMeshAsset(const FString& AssetPath, FCityMeshBuilder& QM,
		UMaterialInterface* WallMat, UMaterialInterface* GlassMat)
	{
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
		// Collision : le maillage de rendu sert de collision (drone vs ville). Pas de
		// primitives simples generees ; les meshes sont statiques, cout memoire accepte.
		Mesh->CreateBodySetup();
		if (UBodySetup* Body = Mesh->GetBodySetup())
		{
			Body->CollisionTraceFlag = CTF_UseComplexAsSimple;
			Body->InvalidatePhysicsData();
		}
		Mesh->Build(false);
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
		return Mesh;
	}
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
	};

	const TMap<FString, FMarkerKind>& MarkerKinds()
	{
		static const TMap<FString, FMarkerKind> Kinds = {
			{ TEXT("metro"),    { 900.f,  FVector3f(0.90f, 0.10f, 0.10f), true } },
			{ TEXT("metro_e"),  { 400.f,  FVector3f(0.90f, 0.10f, 0.10f), false } },
			{ TEXT("church"),   { 1500.f, FVector3f(0.55f, 0.30f, 0.85f), true } },
			{ TEXT("townhall"), { 1900.f, FVector3f(0.95f, 0.72f, 0.15f), true } },
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
	const FString& WallMaterialPath, FVector Location)
{
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
		const FVector Pos = Location + FVector(O->GetNumberField(TEXT("x")) * 100.0,
			O->GetNumberField(TEXT("y")) * 100.0, 0.0);

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

		// Nom flottant : deux axes perpendiculaires EMPILES verticalement (le materiau
		// texte est double-face : superposes, les glyphes se melangent ; empiles, il y a
		// toujours un exemplaire lisible quel que soit l'angle).
		const FString Label = O->GetStringField(TEXT("n"));
		if (Kind->bLabel && !Label.IsEmpty())
		{
			for (int32 i = 0; i < 2; ++i)
			{
				const FRotator Rot(0.f, i * 90.f, 0.f);
				ATextRenderActor* Text = World->SpawnActor<ATextRenderActor>(
					Pos + FVector(0, 0, Kind->HeightCm + 180.0 + i * 360.0), Rot);
				UTextRenderComponent* Comp = Text->GetTextRender();
				Comp->SetText(FText::FromString(Label));
				Comp->SetWorldSize(320.f);
				Comp->SetHorizontalAlignment(EHTA_Center);
				Comp->SetTextRenderColor(FColor(
					FMath::RoundToInt(Kind->Color.X * 255.f),
					FMath::RoundToInt(Kind->Color.Y * 255.f),
					FMath::RoundToInt(Kind->Color.Z * 255.f)));
				Text->SetActorLabel(FString::Printf(TEXT("Label_%s_%d"), *Label, i));
			}
		}
		++Placed;
	}
	FStaticMeshCompilingManager::Get().FinishAllCompilation();
	UE_LOG(LogCityImport, Display, TEXT("%d marqueurs places."), Placed);
	return Placed;
}
