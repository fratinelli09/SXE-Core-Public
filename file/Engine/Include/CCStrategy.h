#pragma once
#include <string>
#include <vector>

#include "StrategyBase.h"
#include "sxe_strategy.h"

//=============================
// Strategy with the C contract (sxe_strategy.h): the library exports C functions instead of the
// CStrategyBase class, so it can be built with any compiler or written in another language.
// CCStrategy is the CStrategyBase the engine sees: it turns the contexts into the C structures
// and the orders of the strategy into calls to IOrders.
//
// Created by CStrategyLoader when the library exports sxe_strategy_api_version.
//=============================
class CCStrategy : public CStrategyBase
{
public:
	struct Functions
	{
		sxe_strategy_create_fn      pfnCreate      = nullptr;
		sxe_strategy_info_fn        pfnInfo        = nullptr;
		sxe_strategy_load_config_fn pfnLoadConfig  = nullptr;
		sxe_strategy_init_fn        pfnInit        = nullptr;
		sxe_strategy_step_fn        pfnStep        = nullptr;
		sxe_strategy_term_fn        pfnTerm        = nullptr;
		sxe_strategy_error_fn       pfnError       = nullptr;
		sxe_strategy_destroy_fn     pfnDestroy     = nullptr;
	};

	explicit CCStrategy(const Functions& functions);
	~CCStrategy() override;

	//creates the strategy of the library (sxe_strategy_create); false = not created (GetLastError)
	bool Create();

	StrategyInfo GetInfo() const override;
	bool LoadConfig(const std::string& strConfigFile) override;
	bool Init(const InitContext& ctx) override;
	RunResult Run(StepContext& ctx) override;
	void Term() override;

private:
	CCStrategy(const CCStrategy&) = delete;
	CCStrategy& operator=(const CCStrategy&) = delete;

	//the error of the library, or strDefault if it gives none
	void _TakeError(const std::string& strDefault);
	//account, positions and open orders of the current step in the C structure (after every order)
	void _Refresh();
	//the result of an order in the C structure (strOrderError keeps the text of the error)
	int _Result(const OrderResult& r, sxe_order_result* pOut);

	//functions of the host (sxe_host), engine = this
	static int  _MarketBuy(void* pEngine, double dUsdt, double dLeverage, sxe_order_result* pOut);
	static int  _MarketSell(void* pEngine, double dUsdt, double dLeverage, sxe_order_result* pOut);
	static int  _CloseAll(void* pEngine, sxe_order_result* pOut);
	static int  _ClosePosition(void* pEngine, int nSide, sxe_order_result* pOut);
	static int  _Reduce(void* pEngine, int nSide, double dQty, sxe_order_result* pOut);
	static int  _LimitBuy(void* pEngine, double dPrice, double dUsdt, double dLeverage, sxe_order_result* pOut);
	static int  _LimitSell(void* pEngine, double dPrice, double dUsdt, double dLeverage, sxe_order_result* pOut);
	static int  _LimitClose(void* pEngine, double dPrice, sxe_order_result* pOut);
	static int  _LimitReduce(void* pEngine, int nSide, double dPrice, double dQty, sxe_order_result* pOut);
	static int  _StopClose(void* pEngine, int nSide, double dTrigger, double dQty, sxe_order_result* pOut);
	static int  _Move(void* pEngine, int64_t nOrderId, double dPrice, sxe_order_result* pOut);
	static int  _Cancel(void* pEngine, int64_t nOrderId);
	static void _CancelAll(void* pEngine);
	static void _Log(void* pEngine, int nLevel, const char* pszMessage);
	static void _LogOnce(void* pEngine, int nLevel, const char* pszMessage);
	static void _ClearOnce(void* pEngine);

	//an order outside a step: rejected
	static int _NoStep(sxe_order_result* pOut);

private:
	Functions   fn;
	sxe_host    host;
	void*       pStrategy;		//the strategy of the library

	Logger*     pLog;			//from the InitContext, then from every StepContext
	StepContext* pCtx;			//the current step, nullptr outside Run()
	sxe_step    step;
	std::vector<sxe_open_order> vOpenOrders;
	std::vector<sxe_execution>  vExecutions;
	std::string strSymbol;
	std::string strLogDir;
	std::string strBackupDir;
	std::string strOrderError;
	mutable std::string strName;
	mutable std::string strVersion;
	mutable std::string strDescription;
};
