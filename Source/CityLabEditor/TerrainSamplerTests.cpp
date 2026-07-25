#include "TerrainSampler.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

BEGIN_DEFINE_SPEC(
	FTerrainSamplerSpec,
	"CityLab.TerrainSampler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
	// Le sampler est charge une seule fois pour toute la spec (la dalle 10k x 10k
	// pese ~200 Mo decompressee, inutile de la recharger a chaque It).
	FTerrainSampler Sampler;
	bool EnsureLoaded();
END_DEFINE_SPEC(FTerrainSamplerSpec)

bool FTerrainSamplerSpec::EnsureLoaded()
{
	if (!Sampler.IsLoaded())
	{
		Sampler.Load(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.png")),
			FPaths::Combine(FPaths::ProjectDir(), TEXT("SourceData/toulouse10_mnt.json")));
	}
	return Sampler.IsLoaded();
}

void FTerrainSamplerSpec::Define()
{
	Describe("Load", [this]()
	{
		It("charge la dalle et lit l'altitude du Capitole a l'origine", [this]()
		{
			TestTrue(TEXT("Load OK"), EnsureLoaded());
			const float Alt0 = Sampler.AltCmAt(0.0, 0.0);
			TestTrue(FString::Printf(TEXT("alt(0,0) = %.1f cm, attendu 14239 +/- 30"), Alt0),
				FMath::Abs(Alt0 - 14239.f) <= 30.f);
			TestEqual(TEXT("AltCapitoleCm() = alt(0,0)"), Sampler.AltCapitoleCm(), Alt0);
		});

		It("borne la dalle dans des altitudes toulousaines plausibles", [this]()
		{
			TestTrue(TEXT("Load OK"), EnsureLoaded());
			const float MinCm = Sampler.MinAltCm();
			const float MaxCm = Sampler.MaxAltCm();
			TestTrue(FString::Printf(TEXT("min dalle = %.0f cm dans [12000, 26000]"), MinCm),
				MinCm >= 12000.f && MinCm <= 26000.f);
			TestTrue(FString::Printf(TEXT("max dalle = %.0f cm dans [12000, 26000]"), MaxCm),
				MaxCm >= 12000.f && MaxCm <= 26000.f);
			TestTrue(TEXT("min <= max"), MinCm <= MaxCm);
		});
	});

	Describe("AltCmAt", [this]()
	{
		It("est continue : deux points a 0,5 m d'ecart different de moins de 2 m", [this]()
		{
			TestTrue(TEXT("Load OK"), EnsureLoaded());
			// Quelques paires a 50 cm d'ecart, dont une a cheval sur un centre de pixel.
			const double Points[][2] = { { 0.0, 0.0 }, { 12345.0, -6789.0 }, { -25.0, 40.0 } };
			for (const auto& P : Points)
			{
				const float A = Sampler.AltCmAt(P[0], P[1]);
				const float Bx = Sampler.AltCmAt(P[0] + 50.0, P[1]);
				const float By = Sampler.AltCmAt(P[0], P[1] + 50.0);
				TestTrue(FString::Printf(TEXT("delta X en (%.0f, %.0f) : |%.1f - %.1f| < 200 cm"), P[0], P[1], A, Bx),
					FMath::Abs(A - Bx) < 200.f);
				TestTrue(FString::Printf(TEXT("delta Y en (%.0f, %.0f) : |%.1f - %.1f| < 200 cm"), P[0], P[1], A, By),
					FMath::Abs(A - By) < 200.f);
			}
		});

		It("clampe aux bords de la dalle sans sortir des bornes", [this]()
		{
			TestTrue(TEXT("Load OK"), EnsureLoaded());
			// Tres au-dela des coins : doit rendre une altitude valide (celle du bord).
			const float FarNW = Sampler.AltCmAt(-9e6, -9e6);
			const float FarSE = Sampler.AltCmAt(9e6, 9e6);
			TestTrue(FString::Printf(TEXT("coin NW clampe = %.0f cm dans la dalle"), FarNW),
				FarNW >= Sampler.MinAltCm() && FarNW <= Sampler.MaxAltCm());
			TestTrue(FString::Printf(TEXT("coin SE clampe = %.0f cm dans la dalle"), FarSE),
				FarSE >= Sampler.MinAltCm() && FarSE <= Sampler.MaxAltCm());
		});
	});

	Describe("MinMaxAltCmInPolygon", [this]()
	{
		It("encadre l'altitude de l'origine dans un carre de 100 m", [this]()
		{
			TestTrue(TEXT("Load OK"), EnsureLoaded());
			const TArray<FVector2D> Carre = {
				FVector2D(-5000.0, -5000.0), FVector2D(5000.0, -5000.0),
				FVector2D(5000.0, 5000.0), FVector2D(-5000.0, 5000.0) };
			const float MinCm = Sampler.MinAltCmInPolygon(Carre);
			const float MaxCm = Sampler.MaxAltCmInPolygon(Carre);
			const float Alt0 = Sampler.AltCmAt(0.0, 0.0);
			TestTrue(FString::Printf(TEXT("min %.1f <= alt(0,0) %.1f"), MinCm, Alt0), MinCm <= Alt0);
			TestTrue(FString::Printf(TEXT("alt(0,0) %.1f <= max %.1f"), Alt0, MaxCm), Alt0 <= MaxCm);
			TestTrue(TEXT("bornes du polygone dans les bornes de la dalle"),
				MinCm >= Sampler.MinAltCm() && MaxCm <= Sampler.MaxAltCm());
		});
	});
}
