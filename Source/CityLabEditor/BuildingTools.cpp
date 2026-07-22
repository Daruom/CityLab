#include "BuildingTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "FileHelpers.h"
#include "Kismet/KismetSystemLibrary.h"
#include "LevelEditorSubsystem.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogBuildingTools, Log, All);

namespace
{
	void RaiseError(const FString& Message)
	{
		// Log en plus de l'exception script : hors contexte script (tests, appels C++
		// directs), RaiseScriptError seul est silencieux.
		UE_LOG(LogBuildingTools, Error, TEXT("%s"), *Message);
		UKismetSystemLibrary::RaiseScriptError(Message);
	}

	// Accumule des quads (2 triangles) dans une FMeshDescription, avec luminance cuite
	// par sommet et UV planaires. Chaque quad a ses propres sommets : aretes dures
	// partout, et couleurs discontinues possibles entre surfaces adjacentes.
	struct FQuadMesh
	{
		FMeshDescription MeshDesc;
		FStaticMeshAttributes Attributes;
		FPolygonGroupID WallGroup;
		FPolygonGroupID GlassGroup;

		FQuadMesh()
			: Attributes(MeshDesc)
		{
			Attributes.Register();
			WallGroup = MeshDesc.CreatePolygonGroup();
			GlassGroup = MeshDesc.CreatePolygonGroup();
			Attributes.GetPolygonGroupMaterialSlotNames()[WallGroup] = FName(TEXT("Wall"));
			Attributes.GetPolygonGroupMaterialSlotNames()[GlassGroup] = FName(TEXT("Glass"));
		}

		// Coins dans l'ordre du contour ; Outward = cote visible. Le winding est
		// choisi ici pour que la face rendue pointe vers Outward : si un test visuel
		// montrait des batiments retournes, c'est LA seule ligne a inverser.
		void AddQuad(FPolygonGroupID Group, const FVector3f Corners[4], const FVector3f& Outward,
			const FVector2f UVs[4], const float Lums[4])
		{
			if ((Corners[0] - Corners[1]).IsNearlyZero(0.01f) ||
				(Corners[1] - Corners[2]).IsNearlyZero(0.01f) ||
				(Corners[2] - Corners[3]).IsNearlyZero(0.01f) ||
				(Corners[3] - Corners[0]).IsNearlyZero(0.01f))
			{
				return;
			}

			const FVector3f RenderedNormal =
				FVector3f::CrossProduct(Corners[2] - Corners[0], Corners[1] - Corners[0]).GetSafeNormal();
			const bool bFlip = FVector3f::DotProduct(RenderedNormal, Outward) < 0.f;
			static const int32 Fwd[4] = { 0, 1, 2, 3 };
			static const int32 Rev[4] = { 0, 3, 2, 1 };
			const int32* Order = bFlip ? Rev : Fwd;

			TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
			TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			TVertexInstanceAttributesRef<FVector2f> UV0 = Attributes.GetVertexInstanceUVs();
			TVertexInstanceAttributesRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();

			TArray<FVertexInstanceID> InstanceIDs;
			InstanceIDs.Reserve(4);
			for (int32 i = 0; i < 4; ++i)
			{
				const int32 c = Order[i];
				const FVertexID VertexID = MeshDesc.CreateVertex();
				Positions[VertexID] = Corners[c];
				const FVertexInstanceID InstanceID = MeshDesc.CreateVertexInstance(VertexID);
				Normals[InstanceID] = Outward;
				UV0.Set(InstanceID, 0, UVs[c]);
				const float L = FMath::Clamp(Lums[c], 0.f, 1.f);
				Colors[InstanceID] = FVector4f(L, L, L, 1.f);
				InstanceIDs.Add(InstanceID);
			}
			MeshDesc.CreatePolygon(Group, InstanceIDs);
		}
	};

	// Construit toute la geometrie du batiment dans QM. Unites internes : centimetres.
	void BuildBuildingGeometry(FQuadMesh& QM, const FBuildingSpec& S)
	{
		const float W = S.WidthM * 100.f;
		const float D = S.DepthM * 100.f;
		const float FloorH = S.FloorHeightM * 100.f;
		const float GroundH = S.GroundFloorHeightM * 100.f;
		const float WallTop = GroundH + (S.Floors - 1) * FloorH;
		const float CorniceD = S.CorniceDepthM * 100.f;
		const float CorniceH = S.CorniceHeightM * 100.f;
		const float ParapetH = S.ParapetHeightM * 100.f;
		const float ParapetT = S.ParapetThicknessM * 100.f;
		const float Corner = S.CornerPierM * 100.f;
		const float Inset = S.WindowInsetM * 100.f;
		const float Tex = S.WallTexWorldSizeM * 100.f;
		const FVector3f Sun = FVector3f(S.SunDirection.GetSafeNormal());
		const float RoofZ = WallTop + (CorniceD > 1.f ? CorniceH : 0.f);

		// Luminance unlit cuite : ambiant + soleil directionnel, assombrissement au
		// contact du sol (0-2 m) et sous la corniche (ombre portee approximee).
		auto Lum = [&](const FVector3f& N, float Z) -> float
		{
			float L = S.AmbientLuminance + S.SunLuminance * FMath::Max(0.f, FVector3f::DotProduct(N, Sun));
			L *= FMath::Lerp(0.82f, 1.f, FMath::Clamp(Z / 200.f, 0.f, 1.f));
			L *= FMath::Lerp(1.f, 0.85f, FMath::Clamp((Z - (WallTop - 150.f)) / 150.f, 0.f, 1.f));
			return L;
		};

		// Boite alignee sur les axes, 6 faces, UV planaires, luminance par sommet.
		auto EmitBox = [&](const FVector3f& Mn, const FVector3f& Mx)
		{
			auto Face = [&](const FVector3f& A, const FVector3f& B, const FVector3f& C,
				const FVector3f& Dd, const FVector3f& N)
			{
				const FVector3f P[4] = { A, B, C, Dd };
				FVector2f UV[4];
				float Ls[4];
				for (int32 i = 0; i < 4; ++i)
				{
					// Projection planaire selon l'axe dominant de la normale.
					UV[i] = FMath::Abs(N.Z) > 0.5f
						? FVector2f(P[i].X / Tex, P[i].Y / Tex)
						: (FMath::Abs(N.X) > 0.5f ? FVector2f(P[i].Y / Tex, P[i].Z / Tex)
							: FVector2f(P[i].X / Tex, P[i].Z / Tex));
					Ls[i] = Lum(N, P[i].Z);
				}
				QM.AddQuad(QM.WallGroup, P, N, UV, Ls);
			};
			const FVector3f v000(Mn.X, Mn.Y, Mn.Z), v100(Mx.X, Mn.Y, Mn.Z), v010(Mn.X, Mx.Y, Mn.Z),
				v110(Mx.X, Mx.Y, Mn.Z), v001(Mn.X, Mn.Y, Mx.Z), v101(Mx.X, Mn.Y, Mx.Z),
				v011(Mn.X, Mx.Y, Mx.Z), v111(Mx.X, Mx.Y, Mx.Z);
			Face(v000, v100, v101, v001, FVector3f(0, -1, 0));
			Face(v110, v010, v011, v111, FVector3f(0, 1, 0));
			Face(v010, v000, v001, v011, FVector3f(-1, 0, 0));
			Face(v100, v110, v111, v101, FVector3f(1, 0, 0));
			Face(v001, v101, v111, v011, FVector3f(0, 0, 1));
			Face(v010, v110, v100, v000, FVector3f(0, 0, -1));
		};

		// Les 4 facades : origine au coin gauche (vue de l'exterieur), tangente vers
		// la droite, normale sortante. UOffset enchaine les UV le long du perimetre.
		struct FFace { FVector3f O; FVector3f T; FVector3f N; float Len; float UOffset; };
		const FFace FacesArr[4] = {
			{ FVector3f(-W / 2, -D / 2, 0), FVector3f(1, 0, 0), FVector3f(0, -1, 0), W, 0.f },
			{ FVector3f(W / 2, -D / 2, 0), FVector3f(0, 1, 0), FVector3f(1, 0, 0), D, W },
			{ FVector3f(W / 2, D / 2, 0), FVector3f(-1, 0, 0), FVector3f(0, 1, 0), W, W + D },
			{ FVector3f(-W / 2, D / 2, 0), FVector3f(0, -1, 0), FVector3f(-1, 0, 0), D, 2 * W + D },
		};

		for (const FFace& F : FacesArr)
		{
			auto WallPoint = [&](float U, float Z) { return F.O + F.T * U + FVector3f(0, 0, Z); };

			// Rectangle de mur au plan de facade.
			auto EmitWall = [&](float U0, float U1, float Z0, float Z1)
			{
				if (U1 - U0 < 0.5f || Z1 - Z0 < 0.5f)
				{
					return;
				}
				const FVector3f P[4] = { WallPoint(U0, Z0), WallPoint(U1, Z0), WallPoint(U1, Z1), WallPoint(U0, Z1) };
				const FVector2f UV[4] = {
					FVector2f((F.UOffset + U0) / Tex, Z0 / Tex), FVector2f((F.UOffset + U1) / Tex, Z0 / Tex),
					FVector2f((F.UOffset + U1) / Tex, Z1 / Tex), FVector2f((F.UOffset + U0) / Tex, Z1 / Tex) };
				const float Ls[4] = { Lum(F.N, Z0), Lum(F.N, Z0), Lum(F.N, Z1), Lum(F.N, Z1) };
				QM.AddQuad(QM.WallGroup, P, F.N, UV, Ls);
			};

			// Fenetre en creux : 4 tableaux (jambages, linteau, appui) + vitre au fond.
			auto EmitWindow = [&](float U0, float U1, float Z0, float Z1)
			{
				const FVector3f In = -F.N * Inset;
				auto Reveal = [&](const FVector3f& A, const FVector3f& B, const FVector3f& N)
				{
					const FVector3f P[4] = { A, B, B + In, A + In };
					const FVector2f UV[4] = { FVector2f(0, 0), FVector2f((B - A).Size() / Tex, 0),
						FVector2f((B - A).Size() / Tex, Inset / Tex), FVector2f(0, Inset / Tex) };
					const float Ls[4] = { Lum(N, A.Z) * S.RevealShade, Lum(N, B.Z) * S.RevealShade,
						Lum(N, B.Z) * S.RevealShade, Lum(N, A.Z) * S.RevealShade };
					QM.AddQuad(QM.WallGroup, P, N, UV, Ls);
				};
				if (Inset > 0.5f)
				{
					Reveal(WallPoint(U0, Z0), WallPoint(U0, Z1), F.T);           // jambage gauche
					Reveal(WallPoint(U1, Z1), WallPoint(U1, Z0), -F.T);          // jambage droit
					Reveal(WallPoint(U1, Z0), WallPoint(U0, Z0), FVector3f(0, 0, 1));  // appui
					Reveal(WallPoint(U0, Z1), WallPoint(U1, Z1), FVector3f(0, 0, -1)); // linteau
				}
				const FVector3f G0 = WallPoint(U0, Z0) + In, G1 = WallPoint(U1, Z0) + In,
					G2 = WallPoint(U1, Z1) + In, G3 = WallPoint(U0, Z1) + In;
				const FVector3f P[4] = { G0, G1, G2, G3 };
				const FVector2f UV[4] = { FVector2f(0, 0), FVector2f(1, 0), FVector2f(1, 1), FVector2f(0, 1) };
				const float GlassL = Lum(F.N, (Z0 + Z1) * 0.5f) * 0.35f;
				const float Ls[4] = { GlassL, GlassL, GlassL, GlassL };
				QM.AddQuad(QM.GlassGroup, P, F.N, UV, Ls);
			};

			const float Usable = F.Len - 2.f * Corner;
			const int32 NumBays = Usable > S.BayWidthM * 100.f * 0.6f
				? FMath::Max(1, FMath::RoundToInt32(Usable / (S.BayWidthM * 100.f))) : 0;
			const float BayW = NumBays > 0 ? Usable / NumBays : 0.f;

			float Z0 = 0.f;
			for (int32 Floor = 0; Floor < S.Floors; ++Floor)
			{
				const float FH = Floor == 0 ? GroundH : FloorH;
				const float Z1 = Z0 + FH;

				if (NumBays == 0)
				{
					EmitWall(0.f, F.Len, Z0, Z1);
					Z0 = Z1;
					continue;
				}

				// Trumeaux d'angle.
				EmitWall(0.f, Corner, Z0, Z1);
				EmitWall(F.Len - Corner, F.Len, Z0, Z1);

				for (int32 Bay = 0; Bay < NumBays; ++Bay)
				{
					const float BU0 = Corner + Bay * BayW;
					const float BU1 = BU0 + BayW;
					const float WinW = BayW * S.WindowWidthRatio;
					// Rez-de-chaussee : ouvertures plus hautes (vitrines).
					const float WinH = Floor == 0
						? FH * FMath::Min(0.85f, S.WindowHeightRatio * 1.35f)
						: FH * S.WindowHeightRatio;
					const float WU0 = (BU0 + BU1 - WinW) * 0.5f;
					const float WU1 = WU0 + WinW;
					const float WZ0 = Floor == 0 ? Z0 + 20.f : Z0 + (FH - WinH) * 0.45f;
					const float WZ1 = WZ0 + WinH;

					EmitWall(BU0, WU0, Z0, Z1);          // trumeau gauche
					EmitWall(WU1, BU1, Z0, Z1);          // trumeau droit
					EmitWall(WU0, WU1, Z0, WZ0);         // allege
					EmitWall(WU0, WU1, WZ1, Z1);         // linteau
					EmitWindow(WU0, WU1, WZ0, WZ1);
				}
				Z0 = Z1;
			}
		}

		// Corniche : anneau de 4 boites jointives (pavage exact, zero recouvrement).
		if (CorniceD > 1.f)
		{
			EmitBox(FVector3f(-W / 2 - CorniceD, -D / 2 - CorniceD, WallTop), FVector3f(W / 2 + CorniceD, -D / 2, WallTop + CorniceH));
			EmitBox(FVector3f(-W / 2 - CorniceD, D / 2, WallTop), FVector3f(W / 2 + CorniceD, D / 2 + CorniceD, WallTop + CorniceH));
			EmitBox(FVector3f(-W / 2 - CorniceD, -D / 2, WallTop), FVector3f(-W / 2, D / 2, WallTop + CorniceH));
			EmitBox(FVector3f(W / 2, -D / 2, WallTop), FVector3f(W / 2 + CorniceD, D / 2, WallTop + CorniceH));
		}

		// Parapet : meme pavage, en retrait vers l'interieur depuis le bord du toit.
		const float InnerInset = ParapetH > 1.f ? ParapetT : 0.f;
		if (ParapetH > 1.f)
		{
			EmitBox(FVector3f(-W / 2, -D / 2, RoofZ), FVector3f(W / 2, -D / 2 + ParapetT, RoofZ + ParapetH));
			EmitBox(FVector3f(-W / 2, D / 2 - ParapetT, RoofZ), FVector3f(W / 2, D / 2, RoofZ + ParapetH));
			EmitBox(FVector3f(-W / 2, -D / 2 + ParapetT, RoofZ), FVector3f(-W / 2 + ParapetT, D / 2 - ParapetT, RoofZ + ParapetH));
			EmitBox(FVector3f(W / 2 - ParapetT, -D / 2 + ParapetT, RoofZ), FVector3f(W / 2, D / 2 - ParapetT, RoofZ + ParapetH));
		}

		// Dalle de toit.
		{
			const FVector3f P[4] = {
				FVector3f(-W / 2 + InnerInset, -D / 2 + InnerInset, RoofZ),
				FVector3f(W / 2 - InnerInset, -D / 2 + InnerInset, RoofZ),
				FVector3f(W / 2 - InnerInset, D / 2 - InnerInset, RoofZ),
				FVector3f(-W / 2 + InnerInset, D / 2 - InnerInset, RoofZ) };
			FVector2f UV[4];
			float Ls[4];
			for (int32 i = 0; i < 4; ++i)
			{
				UV[i] = FVector2f(P[i].X / Tex, P[i].Y / Tex);
				Ls[i] = Lum(FVector3f(0, 0, 1), RoofZ) * 0.9f;
			}
			QM.AddQuad(QM.WallGroup, P, FVector3f(0, 0, 1), UV, Ls);
		}
	}

	UMaterialInterface* LoadMaterialChecked(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return UMaterial::GetDefaultMaterial(MD_Surface);
		}
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *Path);
		if (!Material)
		{
			RaiseError(FString::Printf(TEXT("Material '%s' not found."), *Path));
		}
		return Material;
	}
}

UWorld* UBuildingTools::GetEditorWorldChecked()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		RaiseError(TEXT("No editor world is loaded."));
	}
	return World;
}

void UBuildingTools::NewLevel(const FString& AssetPath)
{
	if (AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetPath must be a package path such as /Game/Maps/L_BuildingTest."));
		return;
	}
	ULevelEditorSubsystem* LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEditor || !LevelEditor->NewLevel(AssetPath))
	{
		RaiseError(FString::Printf(TEXT("Failed to create level '%s'."), *AssetPath));
	}
}

void UBuildingTools::SaveLevel()
{
	ULevelEditorSubsystem* LevelEditor = GEditor->GetEditorSubsystem<ULevelEditorSubsystem>();
	if (!LevelEditor || !LevelEditor->SaveCurrentLevel())
	{
		RaiseError(TEXT("Failed to save the current level."));
		return;
	}
	const bool bPromptUserToSave = false;
	const bool bSaveMapPackages = true;
	const bool bSaveContentPackages = true;
	FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages);
}

void UBuildingTools::ExecConsoleCommand(const FString& Command)
{
	if (Command.TrimStartAndEnd().IsEmpty())
	{
		RaiseError(TEXT("Command must not be empty."));
		return;
	}
	UWorld* World = GetEditorWorldChecked();
	if (World)
	{
		GEngine->Exec(World, *Command);
	}
}

UStaticMesh* UBuildingTools::GenerateBuilding(const FString& AssetPath, const FBuildingSpec& Spec,
	bool bSpawnActor, FVector Location, float YawDegrees)
{
	if (AssetPath.IsEmpty() || !AssetPath.StartsWith(TEXT("/")))
	{
		RaiseError(TEXT("AssetPath must be a package path such as /Game/Dev/SM_Bldg01."));
		return nullptr;
	}
	if (Spec.Floors < 1)
	{
		RaiseError(TEXT("Floors must be at least 1."));
		return nullptr;
	}
	if (Spec.WidthM < 2.f * Spec.CornerPierM + 1.f || Spec.DepthM < 2.f * Spec.CornerPierM + 1.f)
	{
		RaiseError(TEXT("Footprint too small for the requested corner piers."));
		return nullptr;
	}

	UMaterialInterface* WallMaterial = LoadMaterialChecked(Spec.WallMaterialPath);
	UMaterialInterface* GlassMaterial = LoadMaterialChecked(Spec.GlassMaterialPath);
	if (!WallMaterial || !GlassMaterial)
	{
		return nullptr;
	}

	FQuadMesh QM;
	BuildBuildingGeometry(QM, Spec);

	const FString ObjectName = FPackageName::GetLongPackageAssetName(AssetPath);
	const FString ObjectPath = AssetPath + TEXT(".") + ObjectName;

	// Regeneration en place : un asset existant est reconstruit, et tous les acteurs
	// deja places dans le level se mettent a jour (boucle d'iteration MCP sans respawn).
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Mesh)
	{
		UPackage* Package = CreatePackage(*AssetPath);
		Mesh = NewObject<UStaticMesh>(Package, *ObjectName, RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Mesh);
	}
	Mesh->Modify();

	Mesh->GetStaticMaterials().Empty();
	Mesh->GetStaticMaterials().Add(FStaticMaterial(WallMaterial, FName(TEXT("Wall"))));
	Mesh->GetStaticMaterials().Add(FStaticMaterial(GlassMaterial, FName(TEXT("Glass"))));

	Mesh->SetNumSourceModels(1);
	FStaticMeshSourceModel& SourceModel = Mesh->GetSourceModel(0);
	SourceModel.BuildSettings.bRecomputeNormals = true;
	SourceModel.BuildSettings.bRecomputeTangents = true;
	// Regle F.38 : zero eclairage precalcule, donc jamais d'UV de lightmap.
	SourceModel.BuildSettings.bGenerateLightmapUVs = false;
	SourceModel.BuildSettings.bRemoveDegenerates = true;

	Mesh->CreateMeshDescription(0, MoveTemp(QM.MeshDesc));
	Mesh->CommitMeshDescription(0);
	Mesh->SetImportVersion(EImportStaticMeshVersion::LastVersion);
	Mesh->Build(false);
	Mesh->PostEditChange();
	Mesh->MarkPackageDirty();
	// Piege F.39 : sans attente de compilation, crash intermittent sur les petits meshes.
	FStaticMeshCompilingManager::Get().FinishAllCompilation();

	if (bSpawnActor)
	{
		UWorld* World = GetEditorWorldChecked();
		if (!World)
		{
			return Mesh;
		}
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator(0.f, YawDegrees, 0.f));
		if (!Actor)
		{
			RaiseError(TEXT("Failed to spawn the building actor."));
			return Mesh;
		}
		Actor->GetStaticMeshComponent()->SetStaticMesh(Mesh);
		Actor->SetActorLabel(ObjectName);
	}
	return Mesh;
}
