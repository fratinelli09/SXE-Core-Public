#pragma once
#include <memory>
#include <string>
#include <vector>

#include "IDataFeed.h"

//=============================
// Feed of the simulation: candles from a CSV
//
// Format: timestamp,open,high,low,close[,volume] or date;time;open;high;low;close[;volume]
// (separator ',' or ';'), with an optional header. Times in UTC.
// The whole file is read at start (Load), then Next() neither allocates nor reads the disk.
//
// Every candle is walked in ticks (one step per tick), like the price read from the
// exchange every few seconds:
//   intrabar 0: the close only
//   intrabar 1: rising open -> low -> high -> close, falling open -> high -> low -> close
//   intrabar 2: always open -> low -> high -> close (low first)
//   intrabar 3: always open -> high -> low -> close (high first)
// between two distant points intermediate ticks are added every tick_step (fraction of the
// price), starting from the close of the previous candle.
// The ticks are spread over the duration of the candle; the last one (the close) falls at
// the end of the candle and is the step in which lastCandle becomes the candle itself.
//
// The candles read (CandleSeries) are read only and can be shared by several feeds:
// a caller with many runs reads the CSV once and uses it in all of them.
//=============================

//candles of a CSV in the period asked
struct CandleSeries
{
	std::vector<Candle> vCandles;
	long long nInterval    = 0;		//duration of a candle in seconds (from the first two rows)
	size_t    nSkippedRows = 0;
	size_t    nGaps        = 0;		//intervals between two candles longer than nInterval (missing data)
};

class CCsvFeed : public IDataFeed
{
public:
	CCsvFeed();

	//strStart/strEnd "YYYY-MM-DD" or empty; strEnd is included (up to 23:59 of that day)
	//nullptr if the file cannot be read or has no candles in the period (reason in strError)
	static std::shared_ptr<const CandleSeries> Read(const std::string& strFile, const std::string& strStart,
	                                                const std::string& strEnd, std::string& strError);

	//Read() + Use()
	bool Load(const std::string& strFile, const std::string& strStart, const std::string& strEnd);
	//candles already read (also used by other feeds); the feed starts again from the first
	void Use(std::shared_ptr<const CandleSeries> pCandles);
	void SetTicks(int nIntrabarMode, double dTickStep);
	std::string GetLastError() const { return strLastError; }

	size_t Count() const { return pSeries->vCandles.size(); }
	long long FirstTime() const { return pSeries->vCandles.empty() ? 0 : pSeries->vCandles.front().nTime; }
	long long LastTime() const  { return pSeries->vCandles.empty() ? 0 : pSeries->vCandles.back().nTime; }
	size_t SkippedRows() const  { return pSeries->nSkippedRows; }
	//intervals between two candles longer than the duration of a candle (missing data)
	size_t Gaps() const         { return pSeries->nGaps; }
	long long Interval() const  { return pSeries->nInterval; }
	//candles completed so far
	size_t CandlesDone() const  { return !bStarted ? 0 : (nTick < vTicks.size() ? nCandle : nCandle + 1); }

	bool Next(MarketStep& step) override;
	double Progress() const override;

	//--- resume ---
	//current candle (open time) and index of the next tick; -1 if no candle has started
	long long CurrentCandleTime() const;
	size_t NextTick() const { return nTick; }
	//starts again from the candle opening at nCandleTime, at tick nNextTick; false if the candle is not there
	bool Seek(long long nCandleTime, size_t nNextTick);

private:
	void _BuildTicks(size_t nIndex);

private:
	std::shared_ptr<const CandleSeries> pSeries;	//never null: empty until Load()/Use() is called
	std::vector<double> vTicks;		//ticks of the candle nCandle
	size_t      nCandle;			//current candle
	size_t      nTick;				//next tick of nCandle (== vTicks.size(): candle over)
	bool        bStarted;
	int         nIntrabar;
	double      dTickStep;
	std::string strLastError;
};
