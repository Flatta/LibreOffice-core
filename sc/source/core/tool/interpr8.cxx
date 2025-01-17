/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/*
 * This file is part of the LibreOffice project.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

#include <interpre.hxx>
#include <global.hxx>
#include <scmatrix.hxx>
#include <comphelper/random.hxx>
#include <formula/token.hxx>

#include <cmath>
#include <vector>

using namespace formula;

struct DataPoint
{
    double X, Y;

    DataPoint( double rX, double rY ) : X( rX ), Y( rY ) {};
};

static bool lcl_SortByX( const DataPoint &lhs, const DataPoint &rhs ) { return lhs.X < rhs.X; }

/*
 * ScETSForecastCalculation
 *
 * Class is set up to be used with Calc's FORECAST.ETS
 * functions and with chart extrapolations (not yet implemented).
 *
 * Triple Exponential Smoothing (Holt-Winters method)
 *
 * Forecasting of a linear change in data over time (y=a+b*x) with
 * superimposed absolute or relative seasonal deviations, using additive
 * respectively multiplicative Holt-Winters method.
 *
 * Initialisation and forecasting calculations are based on
 * Engineering Statistics Handbook, 6.4.3.5 Triple Exponential Smoothing
 * see "http://www.itl.nist.gov/div898/handbook/pmc/section4/pmc435.htm"
 * Further to the above is that initial calculation of Seasonal effect
 * is corrected for trend.
 *
 * Prediction Interval calculations are based on
 * Yar & Chatfield, Prediction Intervals for the Holt-Winters forecasting
 * procedure, International Journal of Forecasting, 1990, Vol.6, pp127-137
 * The calculation here is a simplified numerical approximation of the above,
 * using random distributions.
 *
 * Double Exponential Smoothing (Holt-Winters method)
 *
 * Forecasting of a linear change in data over time (y=a+b*x), using
 * the Holt-Winters method.
 *
 * Initialisation and forecasting calculations are based on
 * Engineering Statistics Handbook, 6.4.3.3 Double Exponential Smoothing
 * see "http://www.itl.nist.gov/div898/handbook/pmc/section4/pmc433.htm"
 *
 * Prediction Interval calculations are based on
 * Statistical Methods for Forecasting, Bovas & Ledolter, 2009, 3.8 Prediction
 * Intervals for Future Values
 *
 */
class ScETSForecastCalculation
{
private:
    SvNumberFormatter* mpFormatter;
    std::vector< DataPoint > maRange;   // data (X, Y)
    double* mpBase;                     // calculated base value array
    double* mpTrend;                    // calculated trend factor array
    double* mpPerIdx;                   // calculated periodical deviation array, not used with eds
    double* mpForecast;                 // forecasted value array
    SCSIZE mnSmplInPrd;                 // samples per period
    double mfStepSize;                  // increment of X in maRange
    double mfAlpha, mfBeta, mfGamma;    // constants to minimise the RMSE in the ES-equations
    SCSIZE mnCount;                     // No of data points
    bool mbInitialised;
    int mnMonthDay;                     // n-month X-interval, value is day of month
    // accuracy indicators
    double mfMAE;                       // mean absolute error
    double mfMASE;                      // mean absolute scaled error
    double mfMSE;                       // mean squared error (variation)
    double mfRMSE;                      // root mean squared error (standard deviation)
    double mfSMAPE;                     // symmetric mean absolute error
    sal_uInt16 mnErrorValue;
    bool bAdditive;                     // true: additive method, false: mulitplicative method
    bool bEDS;                          // true: EDS, false: ETS

    // constants used in determining best fit for alpha, beta, gamma
    const double cfMinABCResolution = 0.001;  // minimum change of alpha, beta, gamma
    const SCSIZE cnScenarios = 1000;          // No. of scenarios to calculate for PI calculations

    bool initData();
    bool prefillBaseData();
    bool prefillTrendData();
    bool prefillPerIdx();
    bool initCalc();
    void refill();
    SCSIZE CalcPeriodLen();
    void CalcAlphaBetaGamma();
    void CalcBetaGamma();
    void CalcGamma();
    void calcAccuracyIndicators();
    bool GetForecast( double fTarget, double& rForecast );
    double RandDev();
    double convertXtoMonths( double x );

public:
    ScETSForecastCalculation( SCSIZE nSize, SvNumberFormatter* pFormatter );
    ~ScETSForecastCalculation();

    bool PreprocessDataRange( ScMatrixRef rMatX, ScMatrixRef rMatY, int& rSmplInPrd,
                              bool bDataCompletion, int nAggregation, ScMatrixRef rTMat,
                              ScETSType eETSType );
    sal_uInt16 GetError() { return mnErrorValue; };
    bool GetForecastRange( ScMatrixRef rTMat, ScMatrixRef rFcMat );
    bool GetStatisticValue( ScMatrixRef rTypeMat, ScMatrixRef rStatMat );
    bool GetSamplesInPeriod( double& rVal );
    bool GetEDSPredictionIntervals( ScMatrixRef rTMat, ScMatrixRef rPIMat, double fPILevel );
    bool GetETSPredictionIntervals( ScMatrixRef rTMat, ScMatrixRef rPIMat, double fPILevel );
};

ScETSForecastCalculation::ScETSForecastCalculation( SCSIZE nSize, SvNumberFormatter* pFormatter )
{
    mpFormatter = pFormatter;
    mnCount = nSize;
    maRange.reserve( mnCount );
    mbInitialised = false;
    mnMonthDay = 0;
    mpBase     = nullptr;
    mpTrend    = nullptr;
    mpPerIdx   = nullptr;
    mpForecast = nullptr;
}

ScETSForecastCalculation::~ScETSForecastCalculation()
{
    delete mpBase;
    delete mpTrend;
    delete mpPerIdx;
    delete mpForecast;
}

bool ScETSForecastCalculation::PreprocessDataRange( ScMatrixRef rMatX, ScMatrixRef rMatY, int& rSmplInPrd,
                                                    bool bDataCompletion, int nAggregation, ScMatrixRef rTMat,
                                                    ScETSType eETSType )
{
    bEDS = ( rSmplInPrd == 0 );
    bAdditive = ( eETSType == etsAdd || eETSType == etsPIAdd || eETSType == etsStatAdd );

    // maRange needs to be sorted by X
    for ( SCSIZE i = 0; i < mnCount; i++ )
        maRange.push_back( DataPoint( rMatX->GetDouble( i ), rMatY->GetDouble( i ) ) );
    sort( maRange.begin(), maRange.end(), lcl_SortByX );

    if ( rTMat )
    {
        if ( eETSType != etsPIAdd && eETSType != etsPIMult )
        {
            if ( rTMat->GetDouble( 0 ) < maRange[ 0 ].X )
            {
                // target cannot be less than start of X-range
                mnErrorValue = errIllegalFPOperation;
                return false;
            }
        }
        else
        {
            if ( rTMat->GetDouble( 0 ) < maRange[ mnCount - 1 ].X )
            {
                // target cannot be before end of X-range
                mnErrorValue = errIllegalFPOperation;
                return false;
            }
        }
    }

    if ( rSmplInPrd != 1 )
        mnSmplInPrd = rSmplInPrd;
    else
        mnSmplInPrd = CalcPeriodLen();

    // Month intervals don't have exact stepsize, so first
    // detect if month interval is used.
    // Method: assume there is an month interval and verify.
    // If month interval is used, replace maRange.X with month values
    // for ease of calculations.
    Date aNullDate = *( mpFormatter->GetNullDate() );
    Date aDate = aNullDate + static_cast< long >( maRange[ 0 ].X );
    mnMonthDay = aDate.GetDay();
    for ( SCSIZE i = 1; i < mnCount && mnMonthDay; i++ )
    {
        Date aDate1 = aNullDate + static_cast< long >( maRange[ i ].X );
        if ( aDate != aDate1 )
        {
            if ( aDate1.GetDay() != mnMonthDay )
                mnMonthDay = 0;
        }
    }

    mfStepSize = ::std::numeric_limits<double>::max();
    if ( mnMonthDay )
    {
        aDate = aNullDate + static_cast< long >( maRange[ 0 ].X );
        maRange[ 0 ].X = aDate.GetYear() * 12 + aDate.GetMonth();
    }
    for ( SCSIZE i = 1; i < mnCount; i++ )
    {
        if ( mnMonthDay )
        {
            aDate = aNullDate + static_cast< long >( maRange[ i ].X );
            maRange[ i ].X = aDate.GetYear() * 12 + aDate.GetMonth();
        }
        double fStep = maRange[ i ].X - maRange[ i - 1 ].X;
        if ( fStep == 0.0 )
        {
            if ( nAggregation == 0 )
            {
                // identical X-values are not allowed
                mnErrorValue = errNoValue;
                return false;
            }
            double fTmp = maRange[ i - 1 ].Y;
            SCSIZE nCounter = 1;
            switch ( nAggregation )
            {
                case 1 : // AVERAGE (default)
                case 7 : // SUM
                         while ( maRange[ i ].X == maRange[ i - 1 ].X  && i < mnCount )
                         {
                             fTmp += maRange[ i ].Y;
                             nCounter++;
                             maRange.erase( maRange.begin() + i );
                             --mnCount;
                         }
                         maRange[ i - 1 ].Y = ( nAggregation == 1 ? fTmp / nCounter : fTmp );
                         break;

                case 2 : // COUNT
                case 3 : // COUNTA (same as COUNT as there are no non-numeric Y-values)
                         while ( maRange[ i ].X == maRange[ i - 1 ].X  && i < mnCount )
                         {
                             nCounter++;
                             maRange.erase( maRange.begin() + i );
                             --mnCount;
                         }
                         maRange[ i - 1 ].Y = nCounter;
                         break;

                case 4 : // MAX
                         while ( maRange[ i ].X == maRange[ i - 1 ].X  && i < mnCount )
                         {
                             if ( maRange[ i ].Y > fTmp )
                                 fTmp = maRange[ i ].Y;
                             maRange.erase( maRange.begin() + i );
                             --mnCount;
                         }
                         maRange[ i - 1 ].Y = fTmp;
                         break;

                case 5 : // MEDIAN
                         {
                             std::vector< double > aTmp;
                             aTmp.push_back( maRange[ i - 1 ].Y );
                             while ( maRange[ i ].X == maRange[ i - 1 ].X  && i < mnCount )
                             {
                                 aTmp.push_back( maRange[ i ].Y );
                                 nCounter++;
                                 maRange.erase( maRange.begin() + i );
                                 --mnCount;
                             }
                             sort( aTmp.begin(), aTmp.end() );

                             if ( nCounter % 2 )
                                 maRange[ i - 1 ].Y = aTmp[ nCounter / 2 ];
                             else
                                 maRange[ i - 1 ].Y = ( aTmp[ nCounter / 2 ] + aTmp[ nCounter / 2 - 1 ] ) / 2.0;
                         }
                             break;

                case 6 : // MIN
                         while ( maRange[ i ].X == maRange[ i - 1 ].X  && i < mnCount )
                         {
                             if ( maRange[ i ].Y < fTmp )
                                 fTmp = maRange[ i ].Y;
                             maRange.erase( maRange.begin() + i );
                             --mnCount;
                         }
                         maRange[ i - 1 ].Y = fTmp;
                         break;
            }
            if ( i < mnCount - 1 )
            {
                i++;
                if ( mnMonthDay )
                {
                    Date aDate1 = aNullDate + static_cast< long >( maRange[ i ].X );
                    fStep = 12 * ( aDate1.GetYear() - aDate.GetYear() ) +
                            ( aDate1.GetMonth() - aDate.GetMonth() );
                    aDate = aDate1;
                }
                else
                    fStep = maRange[ i ].X - maRange[ i - 1 ].X;
            }
            else
               fStep = mfStepSize;
        }
        if ( fStep < mfStepSize )
            mfStepSize = fStep;
    }

    // step must be constant (or gap multiple of step)
    bool bHasGap = false;
    for ( SCSIZE i = 1; i < mnCount && !bHasGap; i++ )
    {
        double fStep = maRange[ i ].X - maRange[ i - 1 ].X;

        if ( fStep != mfStepSize )
        {
            if ( fmod( fStep, mfStepSize ) != 0.0 )
            {
                // step not constant nor multiple of mfStepSize in case of gaps
                mnErrorValue = errNoValue;
                return false;
            }
            bHasGap = true;
        }
    }

    // fill gaps with values depending on bDataCompletion
    if ( bHasGap )
    {
        SCSIZE nMissingXCount = 0;
        double fOriginalCount = static_cast< double >( mnCount );
        if ( mnMonthDay )
            aDate = aNullDate + static_cast< long >( maRange[ 0 ].X );
        for ( SCSIZE i = 1; i < mnCount; i++ )
        {
            double fDist;
            if ( mnMonthDay )
            {
                Date aDate1 = aNullDate + static_cast< long >( maRange[ i ].X );
                fDist = 12 * ( aDate1.GetYear() - aDate.GetYear() ) +
                         ( aDate1.GetMonth() - aDate.GetMonth() );
                aDate = aDate1;
            }
            else
                fDist = maRange[ i ].X - maRange[ i - 1 ].X;
            if ( fDist > mfStepSize )
            {
                // gap, insert missing data points
                double fYGap = ( maRange[ i ].Y + maRange[ i - 1 ].Y ) / 2.0;
                for ( double fXGap = maRange[ i - 1].X + mfStepSize;  fXGap < maRange[ i ].X; fXGap += mfStepSize )
                {
                    maRange.insert( maRange.begin() + i, DataPoint( fXGap, ( bDataCompletion ? fYGap : 0.0 ) ) );
                    i++;
                    mnCount++;
                    nMissingXCount++;
                    if ( static_cast< double >( nMissingXCount ) / fOriginalCount > 0.3 )
                    {
                        // maximum of 30% missing points exceeded
                        mnErrorValue = errNoValue;
                        return false;
                    }
                }
            }
        }
    }
    if ( !initData() )
        return false;  // note: mnErrorValue is set in called function(s)

    return true;
}

bool ScETSForecastCalculation::initData( )
{
    // give variuous vectors size and initial value
    mpBase     = new double[ mnCount ];
    mpTrend    = new double[ mnCount ];
    if ( !bEDS )
        mpPerIdx   = new double[ mnCount ];
    mpForecast = new double[ mnCount ];
    mpForecast[ 0 ] = maRange[ 0 ].Y;

    if ( prefillTrendData() )
    {
        if ( prefillPerIdx() )
        {
            if ( prefillBaseData() )
                return true;
        }
    }
    return false;
}

bool ScETSForecastCalculation::prefillTrendData()
{
    if ( bEDS )
        mpTrend[ 0 ] = ( maRange[ mnCount - 1 ].Y - maRange[ 0 ].Y ) / static_cast< double >( mnCount - 1 );
    else
    {
        // we need at least 2 periods in the data range
        if ( mnCount < 2 * mnSmplInPrd )
        {
            mnErrorValue = errNoValue;
            return false;
        }

        double fSum = 0.0;
        for ( SCSIZE i = 0; i < mnSmplInPrd; i++ )
            fSum += maRange[ i + mnSmplInPrd ].Y - maRange[ i ].Y;
        double fTrend = fSum / static_cast< double >( mnSmplInPrd * mnSmplInPrd );

        mpTrend[ 0 ] = fTrend;
    }

    return true;
}

bool ScETSForecastCalculation::prefillPerIdx()
{
    if ( !bEDS )
    {
        // use as many complete periods as available
        if ( mnSmplInPrd == 0 )
        {
            // should never happen; if mnSmplInPrd equals 0, bEDS is true
            mnErrorValue = errUnknownState;
            return false;
        }
        SCSIZE nPeriods = mnCount / mnSmplInPrd;
        std::vector< double > aPeriodAverage( nPeriods, 0.0 );
        for ( SCSIZE i = 0; i < nPeriods ; i++ )
        {
            for ( SCSIZE j = 0; j < mnSmplInPrd; j++ )
                aPeriodAverage[ i ] += maRange[ i * mnSmplInPrd + j ].Y;
            aPeriodAverage[ i ] /= static_cast< double >( mnSmplInPrd );
            if ( aPeriodAverage[ i ] == 0.0 )
            {
                SAL_WARN( "sc.core", "prefillPerIdx(), average of 0 will cause divide by zero error, quitting calculation" );
                mnErrorValue = errDivisionByZero;
                return false;
            }
        }

        for ( SCSIZE j = 0; j < mnSmplInPrd; j++ )
        {
            double fI = 0.0;
            for ( SCSIZE i = 0; i < nPeriods ; i++ )
            {
                // adjust average value for position within period
                if ( bAdditive )
                    fI += ( maRange[ i * mnSmplInPrd + j ].Y -
                            ( aPeriodAverage[ i ] + ( static_cast< double >( j ) - 0.5 * ( mnSmplInPrd - 1 ) ) *
                              mpTrend[ 0 ] ) );
                else
                    fI += ( maRange[ i * mnSmplInPrd + j ].Y /
                            ( aPeriodAverage[ i ] + ( static_cast< double >( j ) - 0.5 * ( mnSmplInPrd - 1 ) ) *
                              mpTrend[ 0 ] ) );
            }
            mpPerIdx[ j ] = fI / nPeriods;
        }
    }
    return true;
}

bool ScETSForecastCalculation::prefillBaseData()
{
    if ( bEDS )
        mpBase[ 0 ] = maRange[ 0 ].Y;
    else
        mpBase[ 0 ] = maRange[ 0 ].Y / mpPerIdx[ 0 ];
    return true;
}

bool ScETSForecastCalculation::initCalc()
{
    if ( !mbInitialised )
    {
        CalcAlphaBetaGamma();

        mbInitialised = true;
        calcAccuracyIndicators();
    }
    return true;
}

void ScETSForecastCalculation::calcAccuracyIndicators()
{
    double fSumAbsErr     = 0.0;
    double fSumDivisor    = 0.0;
    double fSumErrSq      = 0.0;
    double fSumAbsPercErr = 0.0;

    for ( SCSIZE i = 1; i < mnCount; i++ )
    {
        double fError = mpForecast[ i ] - maRange[ i ].Y;
        fSumAbsErr     += fabs( fError );
        fSumErrSq      += fError * fError;
        fSumAbsPercErr += fabs( fError ) / ( fabs( mpForecast[ i ] ) + fabs( maRange[ i ].Y ) );
    }

    for ( SCSIZE i = 2; i < mnCount; i++ )
        fSumDivisor += fabs( maRange[ i ].Y - maRange[ i - 1 ].Y );

    int nCalcCount = mnCount - 1;
    mfMAE   = fSumAbsErr / nCalcCount;
    mfMASE  = fSumAbsErr / ( nCalcCount * fSumDivisor / ( nCalcCount - 1 ) );
    mfMSE   = fSumErrSq / nCalcCount;
    mfRMSE  = sqrt( mfMSE );
    mfSMAPE = fSumAbsPercErr * 2.0 / nCalcCount;
}

/*
 * CalcPeriodLen() calculates the most likely length of a period.
 *
 * Method used: for all possible values (between mnCount/2 and 2) compare for
 * each (sample-previous sample) with next period and calculate mean error.
 * Use as much samples as possible for each period length and the most recent samples
 * Return the period length with the lowest mean error.
 */
SCSIZE ScETSForecastCalculation::CalcPeriodLen()
{
    SCSIZE nBestVal = mnCount;
    double fBestME = ::std::numeric_limits<double>::max();

    for ( SCSIZE nPeriodLen = mnCount / 2; nPeriodLen > 1; nPeriodLen-- )
    {
        double fMeanError = 0.0;
        SCSIZE nPeriods = mnCount / nPeriodLen;
        SCSIZE nStart = mnCount - ( nPeriods * nPeriodLen ) + 1;
        for ( SCSIZE i = nStart; i < ( mnCount - nPeriodLen ); i++ )
        {
            fMeanError += fabs( ( maRange[ i ].Y - maRange[ i - 1 ].Y ) -
                                ( maRange[ nPeriodLen + i ].Y - maRange[ nPeriodLen + i - 1 ].Y ) );
        }
        fMeanError /= static_cast< double >( ( nPeriods - 1 ) * nPeriodLen - 1 );

        if ( fMeanError < fBestME || fMeanError == 0.0 )
        {
            nBestVal = nPeriodLen;
            fBestME = fMeanError;
        }
    }
    return nBestVal;
}

void ScETSForecastCalculation::CalcAlphaBetaGamma()
{
    double f0 = 0.0;
    mfAlpha = f0;
    if ( bEDS )
    {
        mfBeta = 0.0; // beta is not used with EDS
        CalcGamma();
    }
    else
        CalcBetaGamma();
    refill();
    double fE0 = mfMSE;

    double f2 = 1.0;
    mfAlpha = f2;
    if ( bEDS )
        CalcGamma();
    else
        CalcBetaGamma();
    refill();
    double fE2 = mfMSE;

    double f1 = 0.5;
    mfAlpha = f1;
    if ( bEDS )
        CalcGamma();
    else
        CalcBetaGamma();
    refill();

    if ( fE0 == mfMSE && mfMSE == fE2 )
    {
        mfAlpha = 0;
        if ( bEDS )
            CalcGamma();
        else
            CalcBetaGamma();
        refill();
        return;
    }
    while ( ( f2 - f1 ) > cfMinABCResolution )
    {
        if ( fE2 > fE0 )
        {
            f2 = f1;
            fE2 = mfMSE;
            f1 = ( f0 + f1 ) / 2;
        }
        else
        {
            f0 = f1;
            fE0 = mfMSE;
            f1 = ( f1 + f2 ) / 2;
        }
        mfAlpha = f1;
        if ( bEDS )
            CalcGamma();
        else
            CalcBetaGamma();
        refill();
    }
    if ( fE2 > fE0 )
    {
        if ( fE0 < mfMSE )
        {
            mfAlpha = f0;
            if ( bEDS )
                CalcGamma();
            else
                CalcBetaGamma();
            refill();
        }
    }
    else
    {
        if ( fE2 < mfMSE )
        {
            mfAlpha = f2;
            if ( bEDS )
                CalcGamma();
            else
                CalcBetaGamma();
            refill();
        }
    }
    calcAccuracyIndicators();

    return;
}

void ScETSForecastCalculation::CalcBetaGamma()
{
    double f0 = 0.0;
    mfBeta = f0;
    CalcGamma();
    refill();
    double fE0 = mfMSE;

    double f2 = 1.0;
    mfBeta = f2;
    CalcGamma();
    refill();
    double fE2 = mfMSE;

    double f1 = 0.5;
    mfBeta = f1;
    CalcGamma();
    refill();

    if ( fE0 == mfMSE && mfMSE == fE2 )
    {
        mfBeta = 0;
        CalcGamma();
        refill();
        return;
    }
    while ( ( f2 - f1 ) > cfMinABCResolution )
    {
        if ( fE2 > fE0 )
        {
            f2 = f1;
            fE2 = mfMSE;
            f1 = ( f0 + f1 ) / 2;
        }
        else
        {
            f0 = f1;
            fE0 = mfMSE;
            f1 = ( f1 + f2 ) / 2;
        }
        mfBeta = f1;
        CalcGamma();
        refill();
    }
    if ( fE2 > fE0 )
    {
        if ( fE0 < mfMSE )
        {
            mfBeta = f0;
            CalcGamma();
            refill();
        }
    }
    else
    {
        if ( fE2 < mfMSE )
        {
            mfBeta = f2;
            CalcGamma();
            refill();
        }
    }
}

void ScETSForecastCalculation::CalcGamma()
{
    double f0 = 0.0;
    mfGamma = f0;
    refill();
    double fE0 = mfMSE;

    double f2 = 1.0;
    mfGamma = f2;
    refill();
    double fE2 = mfMSE;

    double f1 = 0.5;
    mfGamma = f1;
    refill();

    if ( fE0 == mfMSE && mfMSE == fE2 )
    {
        mfGamma = 0;
        refill();
        return;
    }
    while ( ( f2 - f1 ) > cfMinABCResolution )
    {
        if ( fE2 > fE0 )
        {
            f2 = f1;
            fE2 = mfMSE;
            f1 = ( f0 + f1 ) / 2;
        }
        else
        {
            f0 = f1;
            fE0 = mfMSE;
            f1 = ( f1 + f2 ) / 2;
        }
        mfGamma = f1;
        refill();
    }
    if ( fE2 > fE0 )
    {
        if ( fE0 < mfMSE )
        {
            mfGamma = f0;
            refill();
        }
    }
    else
    {
        if ( fE2 < mfMSE )
        {
            mfGamma = f2;
            refill();
        }
    }
}

void ScETSForecastCalculation::refill()
{
    // refill mpBase, mpTrend, mpPerIdx and mpForecast with values
    // using the calculated mfAlpha, (mfBeta), mfGamma
    // forecast 1 step ahead
    for ( SCSIZE i = 1; i < mnCount; i++ )
    {
        if ( bEDS )
        {
            mpBase[ i ] = mfAlpha * maRange[ i ].Y +
                          ( 1 - mfAlpha ) * ( mpBase[ i - 1 ] + mpTrend[ i - 1 ] );
            mpTrend[ i ] = mfGamma * ( mpBase[ i ] - mpBase[ i - 1 ] ) +
                          ( 1 - mfGamma ) * mpTrend[ i - 1 ];
            mpForecast[ i ] = mpBase[ i - 1 ] + mpTrend[ i - 1 ];
        }
        else
        {
            SCSIZE nIdx;
            if ( bAdditive )
            {
                nIdx = ( i > mnSmplInPrd ? i - mnSmplInPrd : i );
                mpBase[ i ] = mfAlpha * ( maRange[ i ].Y - mpPerIdx[ nIdx ] ) +
                              ( 1 - mfAlpha ) * ( mpBase[ i - 1 ] + mpTrend[ i - 1 ] );
                mpPerIdx[ i ] = mfBeta * ( maRange[ i ].Y - mpBase[ i ] ) +
                                      ( 1 - mfBeta ) * mpPerIdx[ nIdx ];
            }
            else
            {
                nIdx = ( i >= mnSmplInPrd ? i - mnSmplInPrd : i );
                mpBase[ i ] = mfAlpha * ( maRange[ i ].Y / mpPerIdx[ nIdx ] ) +
                              ( 1 - mfAlpha ) * ( mpBase[ i - 1 ] + mpTrend[ i - 1 ] );
                mpPerIdx[ i ] = mfBeta * ( maRange[ i ].Y / mpBase[ i ] ) +
                                      ( 1 - mfBeta ) * mpPerIdx[ nIdx ];
            }
            mpTrend[ i ] = mfGamma * ( mpBase[ i ] - mpBase[ i - 1 ] ) +
                          ( 1 - mfGamma ) * mpTrend[ i - 1 ];

            if ( bAdditive )
                mpForecast[ i ] = mpBase[ i - 1 ] + mpTrend[ i - 1 ] + mpPerIdx[ nIdx ];
            else
                mpForecast[ i ] = ( mpBase[ i - 1 ] + mpTrend[ i - 1 ] ) * mpPerIdx[ nIdx ];
        }
    }
    calcAccuracyIndicators();
}

double ScETSForecastCalculation::convertXtoMonths( double x )
{
    Date aNullDate = *( mpFormatter->GetNullDate() );
    Date aDate = aNullDate + static_cast< long >( x );
    int nYear = aDate.GetYear();
    int nMonth = aDate.GetMonth();
    double fMonthLength;
    switch ( nMonth )
    {
        case  1 :
        case  3 :
        case  5 :
        case  7 :
        case  8 :
        case 10 :
        case 12 :
            fMonthLength = 31.0;
            break;
        case  2 :
            fMonthLength = ( aDate.IsLeapYear() ? 29.0 : 28.0 );
            break;
        default :
            fMonthLength = 30.0;
    }
    return ( 12.0 * nYear + nMonth + ( aDate.GetDay() - mnMonthDay ) / fMonthLength );
}

bool ScETSForecastCalculation::GetForecast( double fTarget, double& rForecast )
{
    if ( !initCalc() )
        return false;

    if ( fTarget <= maRange[ mnCount - 1 ].X )
    {
        SCSIZE n = ( fTarget - maRange[ 0 ].X ) / mfStepSize;
        double fInterpolate = fmod( fTarget - maRange[ 0 ].X, mfStepSize );
        rForecast = maRange[ n ].Y;

        if ( fInterpolate >= cfMinABCResolution )
        {
            double fInterpolateFactor = fInterpolate / mfStepSize;
            double fFc_1 = mpForecast[ n + 1 ];
            rForecast = rForecast + fInterpolateFactor * ( fFc_1 - rForecast );
        }
    }
    else
    {
        SCSIZE n = ( fTarget - maRange[ mnCount - 1 ].X ) / mfStepSize;
        double fInterpolate = fmod( fTarget - maRange[ mnCount - 1 ].X, mfStepSize );

        if ( bEDS )
            rForecast = mpBase[ mnCount - 1 ] + n * mpTrend[ mnCount - 1 ];
        else if ( bAdditive )
            rForecast = mpBase[ mnCount - 1 ] + n * mpTrend[ mnCount - 1 ] +
                        mpPerIdx[ mnCount - 1 - mnSmplInPrd + ( n % mnSmplInPrd ) ];
        else
            rForecast = ( mpBase[ mnCount - 1 ] + n * mpTrend[ mnCount - 1 ] ) *
                        mpPerIdx[ mnCount - 1 - mnSmplInPrd + ( n % mnSmplInPrd ) ];

        if ( fInterpolate >= cfMinABCResolution )
        {
            double fInterpolateFactor = fInterpolate / mfStepSize;
            double fFc_1;
            if ( bEDS )
                fFc_1 = mpBase[ mnCount - 1 ] + ( n + 1 ) * mpTrend[ mnCount - 1 ];
            else if ( bAdditive )
                fFc_1 = mpBase[ mnCount - 1 ] + ( n + 1 ) * mpTrend[ mnCount - 1 ] +
                        mpPerIdx[ mnCount - 1 - mnSmplInPrd + ( ( n + 1 ) % mnSmplInPrd ) ];
            else
                fFc_1 = ( mpBase[ mnCount - 1 ] + ( n + 1 ) * mpTrend[ mnCount - 1 ] ) *
                        mpPerIdx[ mnCount - 1 - mnSmplInPrd + ( ( n + 1 ) % mnSmplInPrd ) ];
            rForecast = rForecast + fInterpolateFactor * ( fFc_1 - rForecast );
        }
    }
    return true;
}

bool ScETSForecastCalculation::GetForecastRange( ScMatrixRef rTMat, ScMatrixRef rFcMat )
{
    SCSIZE nC, nR;
    rTMat->GetDimensions( nC, nR );

    for ( SCSIZE i = 0; i < nR; i++ )
    {
        for ( SCSIZE j = 0; j < nC; j++ )
        {
            double fTarget;
            if ( mnMonthDay )
                fTarget = convertXtoMonths( rTMat->GetDouble( j, i ) );
            else
                fTarget = rTMat->GetDouble( j, i );
            double fForecast;
            if ( GetForecast( fTarget, fForecast ) )
                rFcMat->PutDouble( fForecast, j, i );
            else
                return false;
        }
    }
    return true;
}

bool ScETSForecastCalculation::GetStatisticValue( ScMatrixRef rTypeMat, ScMatrixRef rStatMat )
{
    if ( !initCalc() )
        return false;

    SCSIZE nC, nR;
    rTypeMat->GetDimensions( nC, nR );
    for ( SCSIZE i = 0; i < nR; i++ )
    {
        for ( SCSIZE j = 0; j < nC; j++ )
        {
            switch ( static_cast< int >( rTypeMat->GetDouble( j, i ) ) )
            {
                case 1 : // alpha
                    rStatMat->PutDouble( mfAlpha, j, i );
                    break;
                case 2 : // gamma
                    rStatMat->PutDouble( mfGamma, j, i );
                    break;
                case 3 : // beta
                    rStatMat->PutDouble( mfBeta, j, i );
                    break;
                case 4 : // MASE
                    rStatMat->PutDouble( mfMASE, j, i );
                    break;
                case 5 : // SMAPE
                    rStatMat->PutDouble( mfSMAPE, j, i );
                    break;
                case 6 : // MAE
                    rStatMat->PutDouble( mfMAE, j, i );
                    break;
                case 7 : // RMSE
                    rStatMat->PutDouble( mfRMSE, j, i );
                    break;
                case 8 : // step size
                    rStatMat->PutDouble( mfStepSize, j, i );
                    break;
                case 9 : // samples in period
                    rStatMat->PutDouble( mnSmplInPrd, j, i );
                    break;
            }
        }
    }
    return true;
}

bool ScETSForecastCalculation::GetSamplesInPeriod( double& rVal )
{
    if ( !initCalc() )
        return false;

    rVal = mnSmplInPrd;
    return true;
}

double ScETSForecastCalculation::RandDev()
{
    // return a random deviation given the standard deviation
    return ( mfRMSE * ScInterpreter::gaussinv(
             ::comphelper::rng::uniform_real_distribution( 0.5, 1.0 ) ) );
}

bool ScETSForecastCalculation::GetETSPredictionIntervals( ScMatrixRef rTMat, ScMatrixRef rPIMat, double fPILevel )
{
    if ( !initCalc() )
        return false;

    SCSIZE nC, nR;
    rTMat->GetDimensions( nC, nR );

    // find maximum target value and calculate size of scenario-arrays
    double fMaxTarget = rTMat->GetDouble( 0, 0 );
    for ( SCSIZE i = 0; i < nR; i++ )
    {
        for ( SCSIZE j = 0; j < nC; j++ )
        {
            if ( fMaxTarget < rTMat->GetDouble( j, i ) )
                fMaxTarget = rTMat->GetDouble( j, i );
        }
    }
    if ( mnMonthDay )
        fMaxTarget = convertXtoMonths( fMaxTarget ) - maRange[ mnCount - 1 ].X;
    else
        fMaxTarget -= maRange[ mnCount - 1 ].X;
    SCSIZE nSize = ( fMaxTarget / mfStepSize );
    if ( fmod( fMaxTarget, mfStepSize ) != 0.0 )
        nSize++;

    std::unique_ptr< double[] > xScenRange( new double[nSize]);
    std::unique_ptr< double[] > xScenBase( new double[nSize]);
    std::unique_ptr< double[] > xScenTrend( new double[nSize]);
    std::unique_ptr< double[] > xScenPerIdx( new double[nSize]);
    std::vector< std::vector< double > >  aPredictions( nSize, std::vector< double >( cnScenarios ) );

    // fill scenarios
    for ( SCSIZE k = 0; k < cnScenarios; k++ )
    {
        // fill array with forecasts, with RandDev() added to xScenRange
        if ( bAdditive )
        {
            // calculation based on additive model
            xScenRange[ 0 ] = mpBase[ mnCount - 1 ] + mpTrend[ mnCount - 1 ] +
                              mpPerIdx[ mnCount - mnSmplInPrd ] +
                              RandDev();
            aPredictions[ 0 ][ k ] = xScenRange[ 0 ];
            xScenBase[ 0 ] = mfAlpha * ( xScenRange[ 0 ] - mpPerIdx[ mnCount - mnSmplInPrd ] ) +
                             ( 1 - mfAlpha ) * ( mpBase[ mnCount - 1 ] + mpTrend[ mnCount - 1 ] );
            xScenTrend[ 0 ] = mfGamma * ( xScenBase[ 0 ] - mpBase[ mnCount - 1 ] ) +
                              ( 1 - mfGamma ) * mpTrend[ mnCount - 1 ];
            xScenPerIdx[ 0 ] = mfBeta * ( xScenRange[ 0 ] - xScenBase[ 0 ] ) +
                               ( 1 - mfBeta ) * mpPerIdx[ mnCount - mnSmplInPrd ];
            for ( SCSIZE i = 1; i < nSize; i++ )
            {
                double fPerIdx;
                if ( i < mnSmplInPrd )
                    fPerIdx = mpPerIdx[ mnCount + i - mnSmplInPrd ];
                else
                    fPerIdx = xScenPerIdx[ i - mnSmplInPrd ];
                xScenRange[ i ] = xScenBase[ i - 1 ] + xScenTrend[ i - 1 ] + fPerIdx + RandDev();
                aPredictions[ i ][ k ] = xScenRange[ i ];
                xScenBase[ i ] = mfAlpha * ( xScenRange[ i ] - fPerIdx ) +
                                 ( 1 - mfAlpha ) * ( xScenBase[ i - 1 ] + xScenTrend[ i - 1 ] );
                xScenTrend[ i ] = mfGamma * ( xScenBase[ i ] - xScenBase[ i - 1 ] ) +
                                  ( 1 - mfGamma ) * xScenTrend[ i - 1 ];
                xScenPerIdx[ i ] = mfBeta * ( xScenRange[ i ] - xScenBase[ i ] ) +
                                   ( 1 - mfBeta ) * fPerIdx;
            }
        }
        else
        {
            // calculation based on multiplicative model
            xScenRange[ 0 ] = ( mpBase[ mnCount - 1 ] + mpTrend[ mnCount - 1 ] ) *
                              mpPerIdx[ mnCount - mnSmplInPrd ] +
                              RandDev();
            aPredictions[ 0 ][ k ] = xScenRange[ 0 ];
            xScenBase[ 0 ] = mfAlpha * ( xScenRange[ 0 ] / mpPerIdx[ mnCount - mnSmplInPrd ] ) +
                             ( 1 - mfAlpha ) * ( mpBase[ mnCount - 1 ] + mpTrend[ mnCount - 1 ] );
            xScenTrend[ 0 ] = mfGamma * ( xScenBase[ 0 ] - mpBase[ mnCount - 1 ] ) +
                              ( 1 - mfGamma ) * mpTrend[ mnCount - 1 ];
            xScenPerIdx[ 0 ] = mfBeta * ( xScenRange[ 0 ] / xScenBase[ 0 ] ) +
                               ( 1 - mfBeta ) * mpPerIdx[ mnCount - mnSmplInPrd ];
            for ( SCSIZE i = 1; i < nSize; i++ )
            {
                double fPerIdx;
                if ( i < mnSmplInPrd )
                    fPerIdx = mpPerIdx[ mnCount + i - mnSmplInPrd ];
                else
                    fPerIdx = xScenPerIdx[ i - mnSmplInPrd ];
                xScenRange[ i ] = ( xScenBase[ i - 1 ] + xScenTrend[ i - 1 ] ) * fPerIdx + RandDev();
                aPredictions[ i ][ k ] = xScenRange[ i ];
                xScenBase[ i ] = mfAlpha * ( xScenRange[ i ] / fPerIdx ) +
                                 ( 1 - mfAlpha ) * ( xScenBase[ i - 1 ] + xScenTrend[ i - 1 ] );
                xScenTrend[ i ] = mfGamma * ( xScenBase[ i ] - xScenBase[ i - 1 ] ) +
                                  ( 1 - mfGamma ) * xScenTrend[ i - 1 ];
                xScenPerIdx[ i ] = mfBeta * ( xScenRange[ i ] / xScenBase[ i ] ) +
                                   ( 1 - mfBeta ) * fPerIdx;
            }
        }
    }

    // create array of Percentile values;
    std::unique_ptr< double[] > xPercentile( new double[nSize]);
    for ( SCSIZE i = 0; i < nSize; i++ )
    {
        xPercentile[ i ] = ScInterpreter::GetPercentile( aPredictions[ i ], ( 1 + fPILevel ) / 2 ) -
                           ScInterpreter::GetPercentile( aPredictions[ i ], 0.5 );
    }

    for ( SCSIZE i = 0; i < nR; i++ )
    {
        for ( SCSIZE j = 0; j < nC; j++ )
        {
            double fTarget;
            if ( mnMonthDay )
                fTarget = convertXtoMonths( rTMat->GetDouble( j, i ) ) - maRange[ mnCount - 1 ].X;
            else
                fTarget = rTMat->GetDouble( j, i ) - maRange[ mnCount - 1 ].X;
            SCSIZE nSteps = ( fTarget / mfStepSize ) - 1;
            double fFactor = fmod( fTarget, mfStepSize );
            double fPI = xPercentile[ nSteps ];
            if ( fFactor != 0.0 )
            {
                // interpolate
                double fPI1 = xPercentile[ nSteps + 1 ];
                fPI = fPI + fFactor * ( fPI1 - fPI );
            }
            rPIMat->PutDouble( fPI, j, i );
        }
    }
    return true;
}


bool ScETSForecastCalculation::GetEDSPredictionIntervals( ScMatrixRef rTMat, ScMatrixRef rPIMat, double fPILevel )
{
    if ( !initCalc() )
        return false;

    SCSIZE nC, nR;
    rTMat->GetDimensions( nC, nR );

    // find maximum target value and calculate size of coefficient- array c
    double fMaxTarget = rTMat->GetDouble( 0, 0 );
    for ( SCSIZE i = 0; i < nR; i++ )
    {
        for ( SCSIZE j = 0; j < nC; j++ )
        {
            if ( fMaxTarget < rTMat->GetDouble( j, i ) )
                fMaxTarget = rTMat->GetDouble( j, i );
        }
    }
    if ( mnMonthDay )
        fMaxTarget = convertXtoMonths( fMaxTarget ) - maRange[ mnCount - 1 ].X;
    else
        fMaxTarget -= maRange[ mnCount - 1 ].X;
    SCSIZE nSize = ( fMaxTarget / mfStepSize );
    if ( fmod( fMaxTarget, mfStepSize ) != 0.0 )
        nSize++;

    double z = ScInterpreter::gaussinv( ( 1.0 + fPILevel ) / 2.0 );
    double o = 1 - fPILevel;
    std::vector< double > c( nSize );
    for ( SCSIZE i = 0; i < nSize; i++ )
    {
        c[ i ] = sqrt( 1 + ( fPILevel / pow( 1 + o, 3.0 ) ) *
                 ( ( 1 + 4 * o + 5 * o * o ) +
                   2 * static_cast< double >( i ) * fPILevel * ( 1 + 3 * o ) +
                   2 * static_cast< double >( i * i ) * fPILevel * fPILevel ) );
    }


    for ( SCSIZE i = 0; i < nR; i++ )
    {
        for ( SCSIZE j = 0; j < nC; j++ )
        {
            double fTarget;
            if ( mnMonthDay )
                fTarget = convertXtoMonths( rTMat->GetDouble( j, i ) ) - maRange[ mnCount - 1 ].X;
            else
                fTarget = rTMat->GetDouble( j, i ) - maRange[ mnCount - 1 ].X;
            SCSIZE nSteps = ( fTarget / mfStepSize ) - 1;
            double fFactor = fmod( fTarget, mfStepSize );
            double fPI = z * mfRMSE * c[ nSteps ] / c[ 0 ];
            if ( fFactor != 0.0 )
            {
                // interpolate
                double fPI1 = z * mfRMSE * c[ nSteps + 1 ] / c[ 0 ];
                fPI = fPI + fFactor * ( fPI1 - fPI );
            }
            rPIMat->PutDouble( fPI, j, i );
        }
    }
    return true;
}


void ScInterpreter::ScForecast_Ets( ScETSType eETSType )
{
    sal_uInt8 nParamCount = GetByte();
    switch ( eETSType )
    {
        case etsAdd :
        case etsMult :
        case etsStatAdd :
        case etsStatMult :
            if ( !MustHaveParamCount( nParamCount, 3, 6 ) )
                return;
            break;
        case etsPIAdd :
        case etsPIMult :
            if ( !MustHaveParamCount( nParamCount, 3, 7 ) )
            {
                return;
            }
            break;
        case etsSeason :
            if ( !MustHaveParamCount( nParamCount, 2, 4 ) )
                return;
            break;
    }

    int nAggregation;
    if ( ( nParamCount == 6 && eETSType != etsPIAdd && eETSType != etsPIMult ) ||
         ( nParamCount == 4 && eETSType == etsSeason ) ||
         nParamCount == 7 )
        nAggregation = static_cast< int >( GetDoubleWithDefault( 1.0 ) );
    else
        nAggregation = 1;
    if ( nAggregation < 1 || nAggregation > 7 )
    {
        PushIllegalParameter();
        return;
    }

    bool bDataCompletion;
    if ( ( nParamCount >= 5 && eETSType != etsPIAdd && eETSType != etsPIMult ) ||
         ( nParamCount >= 3 && eETSType == etsSeason ) ||
         ( nParamCount >= 6  && ( eETSType == etsPIAdd || eETSType == etsPIMult ) ) )
    {
        int nTemp = static_cast< int >( GetDoubleWithDefault( 1.0 ) );
        if ( nTemp == 0 || nTemp == 1 )
            bDataCompletion = nTemp;
        else
        {
            PushIllegalParameter();
            return;
        }
    }
    else
        bDataCompletion = true;

    int nSmplInPrd;
    if ( ( ( nParamCount >= 4 && eETSType != etsPIAdd && eETSType != etsPIMult ) ||
           ( nParamCount >= 5  && ( eETSType == etsPIAdd || eETSType == etsPIMult ) ) ) &&
         eETSType != etsSeason )
    {
        double fVal = GetDoubleWithDefault( 1.0 );
        if ( fmod( fVal, 1.0 ) != 0 || fVal < 0.0 )
        {
            PushError( errIllegalFPOperation );
            return;
        }
        nSmplInPrd = static_cast< int >( fVal );
    }
    else
        nSmplInPrd = 1;

    // required arguments
    double fPILevel = 0.0;
    if ( nParamCount < 3 && !( nParamCount == 2 && eETSType == etsSeason ) )
    {
        PushIllegalArgument();
        return;
    }

    if ( eETSType == etsPIAdd || eETSType == etsPIMult )
    {
        fPILevel = GetDoubleWithDefault( 0.95 );
        if ( fPILevel < 0 || fPILevel > 1 )
        {
            PushIllegalParameter();
            return;
        }
    }

    ScMatrixRef pTypeMat;
    if ( eETSType == etsStatAdd || eETSType == etsStatMult )
    {
        pTypeMat = GetMatrix();
        SCSIZE nC, nR;
        pTypeMat->GetDimensions( nC, nR );
        for ( SCSIZE i = 0; i < nR; i++ )
        {
            for ( SCSIZE j = 0; j < nC; j++ )
            {
                if ( static_cast< int >( pTypeMat->GetDouble( j, i ) ) < 1 ||
                     static_cast< int >( pTypeMat->GetDouble( j, i ) ) > 9 )
                {
                    PushIllegalParameter();
                    return;
                }
            }
        }
    }

    ScMatrixRef pMatX = GetMatrix();
    ScMatrixRef pMatY = GetMatrix();
    if ( !pMatX || !pMatY )
    {
        PushIllegalParameter();
        return;
    }
    SCSIZE nCX, nCY;
    SCSIZE nRX, nRY;
    pMatX->GetDimensions( nCX, nRX );
    pMatY->GetDimensions( nCY, nRY );
    if ( nRX != nRY || nCX != nCY ||
         !pMatX->IsNumeric() || !pMatY->IsNumeric() )
    {
        PushIllegalArgument();
        return;
    }

    ScMatrixRef pTMat;
    if ( eETSType != etsStatAdd && eETSType != etsStatMult && eETSType != etsSeason )
    {
        pTMat = GetMatrix();
        if ( !pTMat )
        {
            PushIllegalArgument();
            return;
        }
    }

    ScETSForecastCalculation aETSCalc( pMatX->GetElementCount(), pFormatter );
    if ( !aETSCalc.PreprocessDataRange( pMatX, pMatY, nSmplInPrd, bDataCompletion,
                                       nAggregation,
                                       ( eETSType != etsStatAdd && eETSType != etsStatMult ? pTMat : nullptr ),
                                       eETSType ) )
    {
        PushError( aETSCalc.GetError() );
        return;
    }

    switch ( eETSType )
    {
        case etsAdd    :
        case etsMult   :
            {
                SCSIZE nC, nR;
                pTMat->GetDimensions( nC, nR );
                ScMatrixRef pFcMat = GetNewMat( nC, nR );
                if ( aETSCalc.GetForecastRange( pTMat, pFcMat ) )
                    PushMatrix( pFcMat );
                else
                    PushError( aETSCalc.GetError() );
            }
            break;
        case etsPIAdd :
        case etsPIMult :
            {
                SCSIZE nC, nR;
                pTMat->GetDimensions( nC, nR );
                ScMatrixRef pPIMat = GetNewMat( nC, nR );
                if ( nSmplInPrd == 0 )
                {
                    if ( aETSCalc.GetEDSPredictionIntervals( pTMat, pPIMat, fPILevel ) )
                        PushMatrix( pPIMat );
                    else
                        PushError( aETSCalc.GetError() );
                }
                else
                {
                    if ( aETSCalc.GetETSPredictionIntervals( pTMat, pPIMat, fPILevel ) )
                        PushMatrix( pPIMat );
                    else
                        PushError( aETSCalc.GetError() );
                }
            }
            break;
            break;
        case etsStatAdd  :
        case etsStatMult :
            {
                SCSIZE nC, nR;
                pTypeMat->GetDimensions( nC, nR );
                ScMatrixRef pStatMat = GetNewMat( nC, nR );
                if ( aETSCalc.GetStatisticValue( pTypeMat, pStatMat ) )
                    PushMatrix( pStatMat );
                else
                    PushError( aETSCalc.GetError() );
            }
            break;
        case etsSeason :
            {
                double rVal;
                if ( aETSCalc.GetSamplesInPeriod( rVal ) )
                    PushDouble( rVal );
                else
                    PushError( aETSCalc.GetError() );
            }
            break;
    }

    return;
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
