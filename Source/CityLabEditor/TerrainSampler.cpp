#include "TerrainSampler.h"

#include "Dom/JsonObject.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogTerrainSampler, Log, All);

float RenderedQuadZ(float Z00, float Z10, float Z11, float Z01, float u, float v)
{
	u = FMath::Clamp(u, 0.f, 1.f);
	v = FMath::Clamp(v, 0.f, 1.f);
	// diagonale 00-11 : triangles (00,10,11) puis (00,11,01)
	const float ZA = (u >= v)
		? Z00 + u * (Z10 - Z00) + v * (Z11 - Z10)
		: Z00 + u * (Z11 - Z01) + v * (Z01 - Z00);
	// diagonale 10-01 : triangles (00,10,01) puis (10,11,01)
	const float ZB = (u + v <= 1.f)
		? Z00 + u * (Z10 - Z00) + v * (Z01 - Z00)
		: Z11 + (1.f - v) * (Z10 - Z11) + (1.f - u) * (Z01 - Z11);
	return FMath::Max(ZA, ZB);
}

namespace
{
	// Point-dans-polygone pair-impair (ray casting horizontal), bords inclus "au mieux" :
	// suffisant pour un echantillonnage au pas du metre (les sommets sont testes a part).
	bool PointInPolygon(const FVector2D& P, const TArray<FVector2D>& Poly)
	{
		bool bInside = false;
		for (int32 i = 0, j = Poly.Num() - 1; i < Poly.Num(); j = i++)
		{
			if ((Poly[i].Y > P.Y) != (Poly[j].Y > P.Y) &&
				P.X < (Poly[j].X - Poly[i].X) * (P.Y - Poly[i].Y) / (Poly[j].Y - Poly[i].Y) + Poly[i].X)
			{
				bInside = !bInside;
			}
		}
		return bInside;
	}
}

bool FTerrainSampler::Load(const FString& PngPath, const FString& JsonPath)
{
	const double StartSeconds = FPlatformTime::Seconds();
	Heights.Empty();

	// --- JSON de georeferencement : rien n'est code en dur, tout vient d'ici. ---
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *JsonPath))
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("Cannot read MNT json file '%s'"), *JsonPath);
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(JsonText), Root) || !Root.IsValid())
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("Invalid MNT json '%s'"), *JsonPath);
		return false;
	}
	const TSharedPtr<FJsonObject>* Grid = nullptr;
	if (!Root->TryGetObjectField(TEXT("grid"), Grid))
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("MNT json '%s' has no 'grid' object"), *JsonPath);
		return false;
	}
	const TSharedPtr<FJsonObject>* CoinNw = nullptr;
	double WidthPx = 0.0, HeightPx = 0.0, PixelSizeM = 0.0, CoinX = 0.0, CoinY = 0.0;
	if (!(*Grid)->TryGetNumberField(TEXT("width_px"), WidthPx) ||
		!(*Grid)->TryGetNumberField(TEXT("height_px"), HeightPx) ||
		!(*Grid)->TryGetNumberField(TEXT("pixel_size_m"), PixelSizeM) ||
		!(*Grid)->TryGetObjectField(TEXT("coin_nw_unreal_cm"), CoinNw) ||
		!(*CoinNw)->TryGetNumberField(TEXT("x"), CoinX) ||
		!(*CoinNw)->TryGetNumberField(TEXT("y"), CoinY))
	{
		UE_LOG(LogTerrainSampler, Error,
			TEXT("MNT json '%s' misses grid fields (width_px, height_px, pixel_size_m, coin_nw_unreal_cm)"), *JsonPath);
		return false;
	}
	if (WidthPx < 2.0 || HeightPx < 2.0 || PixelSizeM <= 0.0)
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("MNT json '%s' has an invalid grid (%gx%g px, pixel %g m)"),
			*JsonPath, WidthPx, HeightPx, PixelSizeM);
		return false;
	}

	// --- PNG 16 bits gris via IImageWrapper (gere le byte-swap du PNG big-endian). ---
	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *PngPath))
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("Cannot read MNT png file '%s'"), *PngPath);
		return false;
	}
	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	const TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!Wrapper.IsValid() || !Wrapper->SetCompressed(FileData.GetData(), FileData.Num()))
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("MNT png '%s' is not a valid PNG"), *PngPath);
		return false;
	}
	if (Wrapper->GetWidth() != (int32)WidthPx || Wrapper->GetHeight() != (int32)HeightPx)
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("MNT png '%s' is %dx%d px but json says %gx%g"),
			*PngPath, Wrapper->GetWidth(), Wrapper->GetHeight(), WidthPx, HeightPx);
		return false;
	}
	TArray64<uint8> Raw;
	if (!Wrapper->GetRaw(ERGBFormat::Gray, 16, Raw) ||
		Raw.Num() != (int64)WidthPx * (int64)HeightPx * (int64)sizeof(uint16))
	{
		UE_LOG(LogTerrainSampler, Error, TEXT("Cannot decode MNT png '%s' as 16-bit gray"), *PngPath);
		return false;
	}

	Width = (int32)WidthPx;
	Height = (int32)HeightPx;
	PixelSizeCm = PixelSizeM * 100.0;
	OriginXcm = CoinX;
	OriginYcm = CoinY;
	Heights.SetNumUninitialized((int64)Width * (int64)Height);
	FMemory::Memcpy(Heights.GetData(), Raw.GetData(), Raw.Num());

	// Bornes de la dalle (une passe) + altitude de rebase au Capitole (0,0).
	uint16 MinV = MAX_uint16, MaxV = 0;
	for (const uint16 V : Heights)
	{
		MinV = FMath::Min(MinV, V);
		MaxV = FMath::Max(MaxV, V);
	}
	CachedMinAltCm = (float)MinV;
	CachedMaxAltCm = (float)MaxV;
	CachedAltCapitoleCm = AltCmAt(0.0, 0.0);

	UE_LOG(LogTerrainSampler, Log,
		TEXT("MNT charge : %dx%d px (pixel %.0f cm), coin NW (%.0f, %.0f) cm, alt [%.0f..%.0f] cm, Capitole %.1f cm, en %.2f s"),
		Width, Height, PixelSizeCm, OriginXcm, OriginYcm, CachedMinAltCm, CachedMaxAltCm,
		CachedAltCapitoleCm, FPlatformTime::Seconds() - StartSeconds);
	return true;
}

float FTerrainSampler::AltCmAt(double Xcm, double Ycm) const
{
	if (Heights.Num() == 0)
	{
		return 0.f;
	}
	// Coordonnee pixel continue : les centres de pixels sont a (col+0,5 ; row+0,5),
	// donc la valeur du pixel (0,0) vit a un demi-pixel du coin NW. Clamp aux bords.
	const double Fx = FMath::Clamp((Xcm - OriginXcm) / PixelSizeCm - 0.5, 0.0, (double)(Width - 1));
	const double Fy = FMath::Clamp((Ycm - OriginYcm) / PixelSizeCm - 0.5, 0.0, (double)(Height - 1));
	const int32 X0 = FMath::Min((int32)Fx, Width - 2);
	const int32 Y0 = FMath::Min((int32)Fy, Height - 2);
	const float Tx = (float)(Fx - X0);
	const float Ty = (float)(Fy - Y0);
	const int64 Row0 = (int64)Y0 * Width + X0;
	const int64 Row1 = Row0 + Width;
	const float Top = FMath::Lerp((float)Heights[Row0], (float)Heights[Row0 + 1], Tx);
	const float Bottom = FMath::Lerp((float)Heights[Row1], (float)Heights[Row1 + 1], Tx);
	return FMath::Lerp(Top, Bottom, Ty);
}

float FTerrainSampler::MinAltCmInPolygon(const TArray<FVector2D>& PolyCm) const
{
	return MinMaxAltCmInPolygon(PolyCm, false);
}

float FTerrainSampler::MaxAltCmInPolygon(const TArray<FVector2D>& PolyCm) const
{
	return MinMaxAltCmInPolygon(PolyCm, true);
}

float FTerrainSampler::PercentileAltCmInPolygon(const TArray<FVector2D>& PolyCm, float Percentile) const
{
	if (Heights.Num() == 0 || PolyCm.Num() == 0)
	{
		return 0.f;
	}
	TArray<float> Samples;
	CollectAltCmInPolygon(PolyCm, Samples);
	Samples.Sort();
	const int32 Index = FMath::Clamp(
		FMath::RoundToInt32(FMath::Clamp(Percentile, 0.f, 1.f) * (Samples.Num() - 1)),
		0, Samples.Num() - 1);
	return Samples[Index];
}

float FTerrainSampler::MinMaxAltCmInPolygon(const TArray<FVector2D>& PolyCm, bool bWantMax) const
{
	if (Heights.Num() == 0 || PolyCm.Num() == 0)
	{
		return 0.f;
	}
	TArray<float> Samples;
	CollectAltCmInPolygon(PolyCm, Samples);
	float Best = Samples[0];
	for (const float Alt : Samples)
	{
		Best = bWantMax ? FMath::Max(Best, Alt) : FMath::Min(Best, Alt);
	}
	return Best;
}

void FTerrainSampler::CollectAltCmInPolygon(const TArray<FVector2D>& PolyCm, TArray<float>& OutAltCm) const
{
	// Les sommets d'abord : garantit un resultat meme pour un polygone fin ou
	// plus petit que le pas de la grille.
	FVector2D BboxMin = PolyCm[0], BboxMax = PolyCm[0];
	for (int32 i = 0; i < PolyCm.Num(); ++i)
	{
		OutAltCm.Add(AltCmAt(PolyCm[i].X, PolyCm[i].Y));
		BboxMin = FVector2D::Min(BboxMin, PolyCm[i]);
		BboxMax = FVector2D::Max(BboxMax, PolyCm[i]);
	}

	// Interieur : grille au pas du MNT (~1 m) bornee a la bbox, points retenus
	// par point-dans-polygone. Demi-pas de depart pour echantillonner les centres.
	for (double Y = BboxMin.Y + PixelSizeCm * 0.5; Y < BboxMax.Y; Y += PixelSizeCm)
	{
		for (double X = BboxMin.X + PixelSizeCm * 0.5; X < BboxMax.X; X += PixelSizeCm)
		{
			if (PointInPolygon(FVector2D(X, Y), PolyCm))
			{
				OutAltCm.Add(AltCmAt(X, Y));
			}
		}
	}
}
