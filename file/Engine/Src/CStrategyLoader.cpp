#include "CStrategyLoader.h"

#include "CCStrategy.h"

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
#ifdef _WIN32
	void* OpenLibrary(const std::string& strPath)
	{
		return reinterpret_cast<void*>(LoadLibraryW(std::filesystem::path(strPath).wstring().c_str()));
	}

	void* FindSymbol(void* pHandle, const char* pszName)
	{
		return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(pHandle), pszName));
	}

	void CloseLibrary(void* pHandle)
	{
		FreeLibrary(static_cast<HMODULE>(pHandle));
	}

	std::string LibraryError()
	{
		const DWORD nError = GetLastError();
		char buf[512] = {};
		const DWORD nLen = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, nError,
		                                  0, buf, sizeof(buf), nullptr);
		std::string strMsg(buf, nLen);
		while(!strMsg.empty() && (strMsg.back() == '\n' || strMsg.back() == '\r' || strMsg.back() == ' ' || strMsg.back() == '.'))
			strMsg.pop_back();
		return strMsg + " (error " + std::to_string(nError) + ")";
	}
#else
	void* OpenLibrary(const std::string& strPath)
	{
		return dlopen(strPath.c_str(), RTLD_NOW | RTLD_LOCAL);
	}

	void* FindSymbol(void* pHandle, const char* pszName)
	{
		return dlsym(pHandle, pszName);
	}

	void CloseLibrary(void* pHandle)
	{
		dlclose(pHandle);
	}

	std::string LibraryError()
	{
		const char* pszErr = dlerror();
		return pszErr ? pszErr : "unknown error";
	}
#endif
}

CStrategyLoader::CStrategyLoader():
	pHandle(nullptr),
	pfnDestroy(nullptr),
	pStrategy(nullptr),
	bOwned(false)
{
}

CStrategyLoader::~CStrategyLoader()
{
	Unload();
}

bool CStrategyLoader::Load(const std::string& strPath)
{
	Unload();

	std::error_code ec;
	if(!std::filesystem::is_regular_file(strPath, ec))
	{
		strLastError = "Strategy not found: " + strPath;
		return false;
	}

	pHandle = OpenLibrary(strPath);
	if(!pHandle)
	{
		strLastError = "Library cannot be loaded: " + strPath + ": " + LibraryError();
		return false;
	}

	//C contract (sxe_strategy.h)
	if(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_VERSION))
		return _LoadC(strPath);

	auto pfnVersion = reinterpret_cast<SXE_ApiVersionFn>(FindSymbol(pHandle, SXE_SYMBOL_API_VERSION));
	auto pfnCreate  = reinterpret_cast<SXE_CreateFn>(FindSymbol(pHandle, SXE_SYMBOL_CREATE));
	pfnDestroy      = reinterpret_cast<SXE_DestroyFn>(FindSymbol(pHandle, SXE_SYMBOL_DESTROY));

	std::string strMissing;
	if(!pfnVersion) strMissing += " " SXE_SYMBOL_API_VERSION;
	if(!pfnCreate)  strMissing += " " SXE_SYMBOL_CREATE;
	if(!pfnDestroy) strMissing += " " SXE_SYMBOL_DESTROY;
	if(!strMissing.empty())
	{
		strLastError = "Not an SXE strategy, missing functions:" + strMissing +
		               " (SXE_EXPORT_STRATEGY missing in the .cpp?, or " SXE_STRATEGY_SYMBOL_VERSION " for the C contract)";
		Unload();
		return false;
	}

	const int nVersion = pfnVersion();
	if(nVersion != SXE_API_VERSION)
	{
		strLastError = "Strategy built with interface version " + std::to_string(nVersion) +
		               ", the engine uses version " + std::to_string(SXE_API_VERSION) + ": rebuild it";
		Unload();
		return false;
	}

	pStrategy = pfnCreate();
	if(!pStrategy)
	{
		strLastError = "SXE_Create did not create the strategy";
		Unload();
		return false;
	}

	strLastError.clear();
	return true;
}

bool CStrategyLoader::_LoadC(const std::string& strPath)
{
	auto pfnVersion = reinterpret_cast<sxe_strategy_api_version_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_VERSION));

	CCStrategy::Functions fn;
	fn.pfnCreate     = reinterpret_cast<sxe_strategy_create_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_CREATE));
	fn.pfnInfo       = reinterpret_cast<sxe_strategy_info_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_INFO));
	fn.pfnLoadConfig = reinterpret_cast<sxe_strategy_load_config_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_LOAD_CONFIG));
	fn.pfnInit       = reinterpret_cast<sxe_strategy_init_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_INIT));
	fn.pfnStep       = reinterpret_cast<sxe_strategy_step_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_STEP));
	fn.pfnTerm       = reinterpret_cast<sxe_strategy_term_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_TERM));
	fn.pfnError      = reinterpret_cast<sxe_strategy_error_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_ERROR));
	fn.pfnDestroy    = reinterpret_cast<sxe_strategy_destroy_fn>(FindSymbol(pHandle, SXE_STRATEGY_SYMBOL_DESTROY));

	std::string strMissing;
	if(!fn.pfnCreate)     strMissing += " " SXE_STRATEGY_SYMBOL_CREATE;
	if(!fn.pfnInfo)       strMissing += " " SXE_STRATEGY_SYMBOL_INFO;
	if(!fn.pfnLoadConfig) strMissing += " " SXE_STRATEGY_SYMBOL_LOAD_CONFIG;
	if(!fn.pfnInit)       strMissing += " " SXE_STRATEGY_SYMBOL_INIT;
	if(!fn.pfnStep)       strMissing += " " SXE_STRATEGY_SYMBOL_STEP;
	if(!fn.pfnTerm)       strMissing += " " SXE_STRATEGY_SYMBOL_TERM;
	if(!fn.pfnError)      strMissing += " " SXE_STRATEGY_SYMBOL_ERROR;
	if(!fn.pfnDestroy)    strMissing += " " SXE_STRATEGY_SYMBOL_DESTROY;
	if(!strMissing.empty())
	{
		strLastError = "Incomplete strategy with the C contract: " + strPath + ", missing functions:" + strMissing;
		Unload();
		return false;
	}

	const int nVersion = pfnVersion();
	if(nVersion != SXE_STRATEGY_API_VERSION)
	{
		strLastError = "Strategy built for version " + std::to_string(nVersion) + " of the C contract, the engine uses version " +
		               std::to_string(SXE_STRATEGY_API_VERSION) + ": rebuild it";
		Unload();
		return false;
	}

	auto* pC = new CCStrategy(fn);
	pStrategy = pC;
	bOwned    = true;
	if(!pC->Create())
	{
		strLastError = pC->GetLastError();
		Unload();
		return false;
	}

	strLastError.clear();
	return true;
}

void CStrategyLoader::Unload()
{
	if(pStrategy && bOwned)
		delete pStrategy;
	else if(pStrategy && pfnDestroy)
		pfnDestroy(pStrategy);
	pStrategy  = nullptr;
	bOwned     = false;
	pfnDestroy = nullptr;

	if(pHandle)
		CloseLibrary(pHandle);
	pHandle = nullptr;
}
