#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "BuildingTools.h"

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
		}
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
		{
			UToolsetRegistry::UnregisterToolsetClass(UBuildingTools::StaticClass());
		}
	}
};

IMPLEMENT_MODULE(FCityLabEditorModule, CityLabEditor)
