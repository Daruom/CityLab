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

	// V4 — LE Z DU SOL RENDU. La dalle n'est pas le MNT : c'est une grille de quads
	// plans. Ce sont ces verrous qui garantissent qu'une pierre posee dessus ne
	// s'enterre pas (grief utilisateur : bordurettes ET bordures de chaussee a moitie
	// noyees sur le relief).
	Describe("RenderedQuadZ", [this]()
	{
		It("rend exactement les quatre coins", [this]()
		{
			TestEqual(TEXT("coin (0,0)"), RenderedQuadZ(10.f, 20.f, 40.f, 30.f, 0.f, 0.f), 10.f);
			TestEqual(TEXT("coin (1,0)"), RenderedQuadZ(10.f, 20.f, 40.f, 30.f, 1.f, 0.f), 20.f);
			TestEqual(TEXT("coin (1,1)"), RenderedQuadZ(10.f, 20.f, 40.f, 30.f, 1.f, 1.f), 40.f);
			TestEqual(TEXT("coin (0,1)"), RenderedQuadZ(10.f, 20.f, 40.f, 30.f, 0.f, 1.f), 30.f);
		});

		It("est le plan exact quand les quatre coins sont coplanaires", [this]()
		{
			// Z = 100 + 10u + 4v : les deux triangulations donnent le meme plan, donc
			// l'enveloppe superieure vaut ce plan a la virgule pres.
			const float Z00 = 100.f, Z10 = 110.f, Z11 = 114.f, Z01 = 104.f;
			const float UV[][2] = { { 0.25f, 0.5f }, { 0.5f, 0.25f }, { 0.9f, 0.1f }, { 0.5f, 0.5f } };
			for (const auto& P : UV)
			{
				const float Attendu = 100.f + 10.f * P[0] + 4.f * P[1];
				const float Obtenu = RenderedQuadZ(Z00, Z10, Z11, Z01, P[0], P[1]);
				TestTrue(FString::Printf(TEXT("plan en (%.2f, %.2f) : %.4f == %.4f"),
					P[0], P[1], Obtenu, Attendu), FMath::IsNearlyEqual(Obtenu, Attendu, 1e-3f));
			}
		});

		It("passe AU-DESSUS d'un creux : c'est la garantie anti-enterrement", [this]()
		{
			// Quad en selle : le MNT continu au centre serait la moyenne (5), la dalle
			// rendue y passe a 10 par l'une des deux diagonales. Une pierre posee a 5
			// avec 7 cm de relief disparaitrait sous la dalle.
			const float Centre = RenderedQuadZ(0.f, 10.f, 0.f, 10.f, 0.5f, 0.5f);
			TestTrue(FString::Printf(TEXT("centre de la selle = %.2f >= 5 (moyenne)"), Centre),
				Centre >= 5.f - 1e-3f);
			TestTrue(FString::Printf(TEXT("centre de la selle = %.2f <= 10 (le plus haut coin)"), Centre),
				Centre <= 10.f + 1e-3f);
		});

		It("n'est JAMAIS sous la plus basse des deux triangulations", [this]()
		{
			// Balayage : l'enveloppe superieure doit majorer les deux triangulations
			// en tout point. C'est la propriete qui rend l'erreur residuelle POSITIVE
			// (la pierre depasse d'un poil au lieu de disparaitre).
			const float Z00 = 0.f, Z10 = 37.f, Z11 = -12.f, Z01 = 25.f;
			for (int32 i = 0; i <= 10; ++i)
			{
				for (int32 j = 0; j <= 10; ++j)
				{
					const float u = i * 0.1f, v = j * 0.1f;
					const float Z = RenderedQuadZ(Z00, Z10, Z11, Z01, u, v);
					const float TA = (u >= v) ? Z00 + u * (Z10 - Z00) + v * (Z11 - Z10)
											  : Z00 + u * (Z11 - Z01) + v * (Z01 - Z00);
					const float TB = (u + v <= 1.f) ? Z00 + u * (Z10 - Z00) + v * (Z01 - Z00)
												    : Z11 + (1.f - v) * (Z10 - Z11) + (1.f - u) * (Z01 - Z11);
					if (Z < FMath::Max(TA, TB) - 1e-3f)
					{
						AddError(FString::Printf(
							TEXT("(%.1f, %.1f) : %.4f < max(%.4f, %.4f)"), u, v, Z, TA, TB));
					}
				}
			}
			TestTrue(TEXT("enveloppe superieure sur tout le quad"), true);
		});

		It("clampe hors du quad au lieu d'extrapoler", [this]()
		{
			TestEqual(TEXT("u = -1 clampe sur le bord"),
				RenderedQuadZ(10.f, 20.f, 40.f, 30.f, -1.f, 0.f), 10.f);
			TestEqual(TEXT("v = 3 clampe sur le bord"),
				RenderedQuadZ(10.f, 20.f, 40.f, 30.f, 0.f, 3.f), 30.f);
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
