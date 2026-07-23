#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "BuildingTools.h"
#include "CityImportTools.h"

class FCityLabEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Le registry MCP n'existe pas en mode commandlet : s'enregistrer quand meme
		// y logge une erreur qui fait echouer les commandlets (piege paye sur DroneFPV).
		if (!IsRunningCommandlet())
		{
			UToolsetRegistry::RegisterToolsetClass(UBuildingTools::StaticClass());
			UToolsetRegistry::RegisterToolsetClass(UCityImportTools::StaticClass());
		}
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
		{
			UToolsetRegistry::UnregisterToolsetClass(UBuildingTools::StaticClass());
			UToolsetRegistry::UnregisterToolsetClass(UCityImportTools::StaticClass());
		}
	}
};

IMPLEMENT_MODULE(FCityLabEditorModule, CityLabEditor)
