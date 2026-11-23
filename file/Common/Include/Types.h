#pragma once
#include <string>

//=============================
// Types shared by the engine and the strategies
// Times are UTC seconds since 1970 (epoch): in simulation the time of the
// candle, live the current time. The strategy never reads the clock.
//=============================

enum class Mode
{
	Simulated,
	Real
};

enum class MarginMode
{
	Crossed,		//the whole account backs the positions
	Isolated		//each position is backed only by its own margin
};

//how the exchange holds the positions of a symbol
enum class PositionMode
{
	OneWay,			//one direction at a time: an opposite order reduces the position and, if larger, reverses it
	Hedge			//long and short together, each with its own margin, average price and liquidation
};

//side of a position
enum class PositionSide
{
	Long,
	Short
};

struct Candle
{
	long long nTime   = 0;		//open time of the candle
	double    dOpen   = 0;
	double    dHigh   = 0;
	double    dLow    = 0;
	double    dClose  = 0;
	double    dVolume = 0;
};

struct Account
{
	double dWallet    = 0;		//balance: fees and funding already taken off
	double dEquity    = 0;		//balance + unrealized PnL
	double dAvailable = 0;		//margin available for a new order
};

struct Position
{
	bool   bOpen             = false;
	PositionSide eSide       = PositionSide::Long;
	double dQty              = 0;		//quantity in the base currency (e.g. BTC)
	double dAvgPrice         = 0;		//average price weighted by quantity
	double dMargin           = 0;		//initial margin (USDT)
	double dLeverage         = 0;
	double dUnrealizedPnL    = 0;
	double dLiquidationPrice = 0;		//0 = cannot be computed
	double dFeesPaid         = 0;		//fees paid by the position
	double dFundingPaid      = 0;		//funding paid (negative if received)
	double dRealizedPnL      = 0;		//gross PnL of the partial reductions already executed
};

//order type: market (taker fee and slippage) or limit (maker fee, at its price)
enum class OrderType
{
	Market,
	Limit,
	Stop			//on a trigger: when the price is reached, it closes at market (stop loss / take profit)
};

enum class OrderSide
{
	Buy,			//opens or increases the long position (one-way: first reduces an open short)
	Close,			//reduces or closes the position ePositionSide (reduce-only)
	Sell			//opens or increases the short position (one-way: first reduces an open long)
};

struct OrderResult
{
	bool        bOk        = false;
	double      dFillPrice = 0;			//0 for a limit order just placed
	double      dQty       = 0;
	double      dFee       = 0;
	std::string strError;			//why it was rejected if bOk = false
	OrderType   eType      = OrderType::Market;
	double      dFeeRate   = 0;			//fee applied (fraction): fee_taker or fee_maker
	long long   nOrderId   = 0;			//limit orders: id for Move and Cancel
};

//open order: limit (eType Limit) or on a trigger (eType Stop)
struct LimitOrder
{
	long long nId       = 0;
	OrderSide eSide     = OrderSide::Buy;
	double    dPrice    = 0;			//limit price, or trigger price of a stop
	double    dUsdt     = 0;			//opening: margin asked
	double    dLeverage = 0;
	double    dQty      = 0;			//opening: quantity; close: how much to close (the position if placed with 0)
	double    dReserved = 0;			//opening: margin and fee reserved
	long long nTime     = 0;			//when it was placed
	PositionSide ePositionSide = PositionSide::Long;	//position it opens or closes
	OrderType eType     = OrderType::Limit;
	bool      bAll      = true;			//close: the whole position at the time of the fill
	bool      bAbove    = false;		//stop: triggers when the price rises to dPrice (false: when it falls)
};

//open order filled (limit or stop)
struct Fill
{
	long long nOrderId = 0;
	OrderSide eSide    = OrderSide::Buy;
	OrderType eType    = OrderType::Limit;
	long long nTime    = 0;
	double    dPrice   = 0;
	double    dQty     = 0;
	double    dFee     = 0;
	double    dFeeRate = 0;
	double    dWalletBefore = 0;		//balance before the fill
	PositionSide ePositionSide = PositionSide::Long;
};

struct ExchangeRules
{
	MarginMode   eMarginMode      = MarginMode::Isolated;
	PositionMode ePositionMode    = PositionMode::OneWay;
	double     dFeeTaker          = 0;	//fee per side of market orders (fraction)
	double     dFeeMaker          = 0;	//fee per side of filled limit orders (fraction)
	double     dSlippage          = 0;	//expected deviation of the fill price (fraction)
	double     dMaintenanceMargin = 0;
	double     dQtyStep           = 0;	//quantity step
	double     dMinQty            = 0;	//minimum quantity
	double     dMinNotional       = 0;	//minimum order value (USDT)
};
