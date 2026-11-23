#pragma once
#include <string>

#include "StrategyExport.h"

class CStrategyLoader
{
public:
	CStrategyLoader();
	~CStrategyLoader();

	bool Load(const std::string& strPath);
	void Unload();

	CStrategyBase* Get() const { return pStrategy; }
	std::string GetLastError() const { return strLastError; }

private:
	//library with the C contract (sxe_strategy.h)
	bool _LoadC(const std::string& strPath);

private:
	void*          pHandle;
	SXE_DestroyFn  pfnDestroy;
	CStrategyBase* pStrategy;
	bool           bOwned;			//pStrategy is a CCStrategy of the engine: the loader deletes it
	std::string    strLastError;
};
