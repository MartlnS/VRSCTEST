/********************************************************************
 * (C) 2019 Michael Toutonghi
 * 
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 * 
 * This provides reserve currency functions, leveraging the multi-precision boost libraries to calculate reserve currency conversions.
 * 
 */

#include "main.h"
#include "pbaas/pbaas.h"
#include "pbaas/reserves.h"
#include "pbaas/notarization.h"
#include "rpc/server.h"
#include "key_io.h"
#include <random>

CTokenOutput::CTokenOutput(const UniValue &obj)
{
    nVersion = (uint32_t)uni_get_int(find_value(obj, "version"), VERSION_CURRENT);
    currencyID = GetDestinationID(DecodeDestination(uni_get_str(find_value(obj, "currencyid"))));

    try
    {
        nValue = AmountFromValue(find_value(obj, "value"));
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        nVersion = VERSION_INVALID;
    }
}

CAmount CReserveTransfer::CalculateTransferFee(const CTransferDestination &destination, uint32_t flags)
{
    if ((flags & FEE_OUTPUT) || (!(flags & PRECONVERT) && (flags & CONVERT)))
    {
        return 0;
    }
    return CReserveTransfer::DEFAULT_PER_STEP_FEE << 1 + ((CReserveTransfer::DEFAULT_PER_STEP_FEE << 1) * (destination.destination.size() / DESTINATION_BYTE_DIVISOR));
}

CAmount CReserveTransfer::CalculateTransferFee() const
{
    // determine fee for this send
    if (IsFeeOutput() || (!IsPreConversion() && IsConversion()))
    {
        return 0;
    }
    return CalculateTransferFee(destination);
}

CCurrencyValueMap CReserveTransfer::CalculateFee(uint32_t flags, CAmount transferTotal, const uint160 &systemID) const
{
    CCurrencyValueMap feeMap;

    feeMap.valueMap[systemID] = CalculateTransferFee();

    // add conversion fees in source currency for conversions or pre-conversions
    if (IsConversion() || IsPreConversion())
    {
        feeMap.valueMap[currencyID] += CReserveTransactionDescriptor::CalculateConversionFee(transferTotal);
        if (IsReserveToReserve())
        {
            feeMap.valueMap[currencyID] <<= 1;
        }
    }

    return feeMap;
}

CReserveExchange::CReserveExchange(const UniValue &uni) : CTokenOutput(uni)
{
    nVersion = (uint32_t)uni_get_int(find_value(uni, "version"), VERSION_CURRENT);
    currencyID = GetDestinationID(DecodeDestination(uni_get_str(find_value(uni, "currencyid"))));
    nValue = AmountFromValue(find_value(uni, "value"));

    if (uni_get_bool(find_value(uni, "toreserve")))
    {
        flags |= TO_RESERVE;
    }
    if (uni_get_bool(find_value(uni, "limitorder")))
    {
        flags |= LIMIT;
    }
    if (uni_get_bool(find_value(uni, "fillorkill")))
    {
        flags |= FILL_OR_KILL;
    }
    if (uni_get_bool(find_value(uni, "sendoutput")))
    {
        flags |= SEND_OUTPUT;
    }

    try
    {
        nLimit = AmountFromValue(find_value(uni, "limitprice"));
        nValidBefore = uni_get_int(find_value(uni, "validbeforeblock"));
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        nVersion = VERSION_INVALID;
    }
}

CReserveExchange::CReserveExchange(const CTransaction &tx)
{
    bool orderFound = false;
    for (auto out : tx.vout)
    {
        COptCCParams p;
        if (IsPayToCryptoCondition(out.scriptPubKey, p))
        {
            if (p.evalCode == EVAL_RESERVE_EXCHANGE)
            {
                if (orderFound)
                {
                    nVersion = VERSION_INVALID;
                }
                else
                {
                    FromVector(p.vData[0], *this);
                    orderFound = true;
                }
            }
        }
    }
}

CCrossChainImport::CCrossChainImport(const CScript &script)
{
    COptCCParams p;
    if (IsPayToCryptoCondition(script, p) && p.IsValid())
    {
        // always take the first for now
        if (p.evalCode == EVAL_CROSSCHAIN_IMPORT && p.vData.size())
        {
            FromVector(p.vData[0], *this);
        }
    }
}

CCrossChainImport::CCrossChainImport(const CTransaction &tx, int32_t *pOutNum)
{
    for (int i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        if (IsPayToCryptoCondition(tx.vout[i].scriptPubKey, p) && p.IsValid())
        {
            // always take the first for now
            if (p.evalCode == EVAL_CROSSCHAIN_IMPORT && p.vData.size())
            {
                FromVector(p.vData[0], *this);
                if (pOutNum)
                {
                    *pOutNum = i;
                }
                break;
            }
        }
    }
}

CCurrencyState::CCurrencyState(const UniValue &obj)
{
    flags = uni_get_int(find_value(obj, "flags"));

    std::string cIDStr = uni_get_str(find_value(obj, "currencyid"));
    if (cIDStr != "")
    {
        CTxDestination currencyDest = DecodeDestination(cIDStr);
        currencyID = GetDestinationID(currencyDest);
    }

    if (flags & FLAG_FRACTIONAL)
    {
        auto CurrenciesArr = find_value(obj, "reservecurrencies");
        size_t numCurrencies;
        if (!CurrenciesArr.isArray() ||
            !(numCurrencies = CurrenciesArr.size()) ||
            numCurrencies > MAX_RESERVE_CURRENCIES)
        {
            flags &= ~FLAG_VALID;
            LogPrintf("Failed to proplerly specify currencies in reserve currency definition\n");
        }
        else
        {
            // store currencies, weights, and reserves
            try
            {
                for (int i = 0; i < CurrenciesArr.size(); i++)
                {
                    uint160 currencyID = GetDestinationID(DecodeDestination(uni_get_str(find_value(CurrenciesArr[i], "currencyid"))));
                    if (currencyID.IsNull())
                    {
                        LogPrintf("Invalid currency ID\n");
                        flags &= ~FLAG_VALID;
                        break;
                    }
                    currencies[i] = currencyID;
                    weights[i] = AmountFromValue(find_value(CurrenciesArr[i], "weight"));
                    reserves[i] = AmountFromValue(find_value(CurrenciesArr[i], "reserves"));
                }
            }
            catch(const std::exception& e)
            {
                std::cerr << e.what() << '\n';
                flags &= ~FLAG_VALID;
                LogPrintf("Invalid specification of currencies, weights, and/or reserves in initial definition of reserve currency\n");
            }
        }
    }

    if (!(flags & FLAG_VALID))
    {
        printf("Invalid currency specification, see debug.log for reason other than invalid flags\n");
        LogPrintf("Invalid currency specification\n");
    }
    else
    {
        try
        {
            initialSupply = AmountFromValue(find_value(obj, "initialsupply"));
            emitted = AmountFromValue(find_value(obj, "emitted"));
            supply = AmountFromValue(find_value(obj, "supply"));
        }
        catch(const std::exception& e)
        {
            std::cerr << e.what() << '\n';
            flags &= ~FLAG_VALID;
        }
    }
}

CCoinbaseCurrencyState::CCoinbaseCurrencyState(const CTransaction &tx, int *pOutIdx)
{
    int localIdx;
    int &i = pOutIdx ? *pOutIdx : localIdx;
    for (i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;
        if (IsPayToCryptoCondition(tx.vout[i].scriptPubKey, p))
        {
            if (p.evalCode == EVAL_CURRENCYSTATE && p.vData.size())
            {
                FromVector(p.vData[0], *this);
                break;
            }
        }
    }
}

std::vector<std::vector<CAmount>> ValueColumnsFromUniValue(const UniValue &uni,
                                                           const std::vector<std::string> &rowNames,
                                                           const std::vector<std::string> &columnNames)
{
    std::vector<std::vector<CAmount>> retVal;
    for (int i = 0; i < rowNames.size(); i++)
    {
        UniValue row = find_value(uni, rowNames[i]);
        if (row.isObject())
        {
            for (int j = 0; j < columnNames.size(); j++)
            {
                if (retVal.size() == j)
                {
                    retVal.emplace_back();
                }
                CAmount columnVal = 0;
                try
                {
                    columnVal = AmountFromValue(find_value(row, columnNames[j]));
                }
                catch(const std::exception& e)
                {
                    std::cerr << e.what() << '\n';
                }
                retVal[j].push_back(columnVal);
            }
        }
    }
    return retVal;
}


CCoinbaseCurrencyState::CCoinbaseCurrencyState(const UniValue &obj) : CCurrencyState(obj)
{
    std::vector<std::vector<CAmount>> columnAmounts;

    auto currenciesValue = find_value(obj, "currencies");
    std::vector<std::string> rowNames;
    for (int i = 0; i < currencies.size(); i++)
    {
        rowNames.push_back(EncodeDestination(CIdentityID(currencies[i])));
    }
    std::vector<std::string> columnNames({"reservein", "nativein", "reserveout", "lastconversionprice", "viaconversionprice", "fees", "conversionfees"});
    if (currenciesValue.isObject())
    {
        columnAmounts = ValueColumnsFromUniValue(currenciesValue, rowNames, columnNames);
        reserveIn = columnAmounts[0];
        nativeIn = columnAmounts[1];
        reserveOut = columnAmounts[2];
        conversionPrice = columnAmounts[4];
        viaConversionPrice = columnAmounts[3];
        fees = columnAmounts[5];
        conversionFees = columnAmounts[6];
    }
    nativeFees = uni_get_int64(find_value(obj, "nativefees"));
    nativeConversionFees = uni_get_int64(find_value(obj, "nativeconversionfees"));
}

CAmount CalculateFractionalOut(CAmount NormalizedReserveIn, CAmount Supply, CAmount NormalizedReserve, int32_t reserveRatio)
{
    static cpp_dec_float_50 one("1");
    static cpp_dec_float_50 bigSatoshi("100000000");
    cpp_dec_float_50 reservein(std::to_string(NormalizedReserveIn));
    reservein = reservein / bigSatoshi;
    cpp_dec_float_50 supply(std::to_string((Supply ? Supply : 1)));
    supply = supply / bigSatoshi;
    cpp_dec_float_50 reserve(std::to_string(NormalizedReserve ? NormalizedReserve : 1));
    reserve = reserve / bigSatoshi;
    cpp_dec_float_50 ratio(std::to_string(reserveRatio));
    ratio = ratio / bigSatoshi;

    //printf("reservein: %s\nsupply: %s\nreserve: %s\nratio: %s\n\n", reservein.str().c_str(), supply.str().c_str(), reserve.str().c_str(), ratio.str().c_str());

    int64_t fractionalOut = 0;

    // first check if anything to buy
    if (NormalizedReserveIn)
    {
        cpp_dec_float_50 supplyout = bigSatoshi * (supply * (pow((reservein / reserve) + one, ratio) - one));
        //printf("supplyout: %s\n", supplyout.str(0, std::ios_base::fmtflags::_S_fixed).c_str());

        if (!CCurrencyState::to_int64(supplyout, fractionalOut))
        {
            assert(false);
        }
    }
    return fractionalOut;
}

CAmount CalculateReserveOut(CAmount FractionalIn, CAmount Supply, CAmount NormalizedReserve, int32_t reserveRatio)
{
    static cpp_dec_float_50 one("1");
    static cpp_dec_float_50 bigSatoshi("100000000");
    cpp_dec_float_50 fractionalin(std::to_string(FractionalIn));
    fractionalin = fractionalin / bigSatoshi;
    cpp_dec_float_50 supply(std::to_string((Supply ? Supply : 1)));
    supply = supply / bigSatoshi;
    cpp_dec_float_50 reserve(std::to_string(NormalizedReserve ? NormalizedReserve : 1));
    reserve = reserve / bigSatoshi;
    cpp_dec_float_50 ratio(std::to_string(reserveRatio));
    ratio = ratio / bigSatoshi;

    //printf("fractionalin: %s\nsupply: %s\nreserve: %s\nratio: %s\n\n", fractionalin.str().c_str(), supply.str().c_str(), reserve.str().c_str(), ratio.str().c_str());

    int64_t reserveOut = 0;

    // first check if anything to buy
    if (FractionalIn)
    {
        cpp_dec_float_50 reserveout = bigSatoshi * (reserve * (one - pow(one - (fractionalin / supply), (one / ratio))));
        //printf("reserveout: %s\n", reserveout.str(0, std::ios_base::fmtflags::_S_fixed).c_str());

        if (!CCurrencyState::to_int64(reserveout, reserveOut))
        {
            assert(false);
        }
    }
    return reserveOut;
}

// This can handle multiple aggregated, bidirectional conversions in one block of transactions. To determine the conversion price, it 
// takes both input amounts of any number of reserves and the fractional currencies targeting those reserves to merge the conversion into one 
// merged calculation with the same price across currencies for all transactions in the block. It returns the newly calculated 
// conversion prices of the fractional reserve in the reserve currency.
std::vector<CAmount> CCurrencyState::ConvertAmounts(const std::vector<CAmount> &_inputReserves,
                                                    const std::vector<CAmount> &_inputFractional,
                                                    CCurrencyState &newState,
                                                    std::vector<std::vector<CAmount>> const *pCrossConversions,
                                                    std::vector<CAmount> *pViaPrices) const
{
    static arith_uint256 bigSatoshi(SATOSHIDEN);

    int32_t numCurrencies = currencies.size();
    std::vector<CAmount> inputReserves = _inputReserves;
    std::vector<CAmount> inputFractional = _inputFractional;

    newState = *this;
    std::vector<CAmount> rates(numCurrencies);
    std::vector<CAmount> initialRates = PricesInReserve();

    bool haveConversion = false;

    if (inputReserves.size() == inputFractional.size() && inputReserves.size() == numCurrencies && 
        (!pCrossConversions || pCrossConversions->size() == numCurrencies))
    {
        int i;
        for (i = 0; i < numCurrencies; i++)
        {
            if (!pCrossConversions || (*pCrossConversions)[i].size() != numCurrencies)
            {
                break;
            }
        }
        if (!pCrossConversions || i == numCurrencies)
        {
            for (auto oneIn : inputReserves)
            {
                if (oneIn)
                {
                    haveConversion = true;
                    break;
                }
            }
            if (!haveConversion)
            {
                for (auto oneIn : inputFractional)
                {
                    if (oneIn)
                    {
                        haveConversion = true;
                        break;
                    }
                }
            }
        }
    }
    else
    {
        printf("%s: invalid parameters\n", __func__);
    }
    
    if (!haveConversion)
    {
        return initialRates;
    }

    // DEBUG ONLY
    for (auto oneIn : inputReserves)
    {
        if (oneIn < 0)
        {
            printf("%s: invalid reserve input amount for conversion %ld\n", __func__, oneIn);
            break;
        }
    }
    for (auto oneIn : inputFractional)
    {
        if (oneIn < 0)
        {
            printf("%s: invalid fractional input amount for conversion %ld\n", __func__, oneIn);
            break;
        }
    }
    // END DEBUG ONLY

    // Create corresponding fractions of the supply for each currency to be used as starting calculation of that currency's value
    // Determine the equivalent amount of input and output based on current values. Balance each such that each currency has only
    // input or output, denominated in supply at the starting value.
    //
    // For each currency in either direction, sell to reserve or buy aggregate, we convert to a contribution of amount at the reserve
    // percent value. For example, consider 4 currencies, r1...r4, which are all 25% reserves of currency fr1. For simplicity of example,
    // assume 1000 reserve of each reserve currency, where all currencies are equal in value to each other at the outset, and a supply of
    // 4000, where each fr1 is equal in value to 1 of each component reserve. 
    // Now, consider the following cases:
    //
    // 1. purchase fr1 with 100 r1
    //      This is treated as a single 25% fractional purchase with respect to amount purchased, ending price, and supply change
    // 2. purchase fr1 with 100 r1, 100 r2, 100 r3, 100 r4
    //      This is treated as a common layer of purchase across 4 x 25% currencies, resulting in 100% fractional purchase divided 4 ways
    // 3. purchase fr1 with 100 r1, 50 r2, 25 r3
    //      This is treated as 3 separate purchases in order:
    //          a. one of 25 units across 3 currencies (3 x 25%), making a 75% fractional purchase of 75 units divided equally across 3 currencies
    //          b. one of 25 units across 2 currencies (2 x 25%), making a 50% fractional purchase of 50 units divided equally between r1 and r2
    //          c. one purchase of 50 units in r1 at 25% fractional purchase
    // 4. purchase fr1 with 100 r1, sell 100 fr1 to r2
    //          a. one fractional purchase of 100 units at 25%
    //          b. one fractional sell of 100 units at 25%
    //          c. do each in forward and reverse order and set conversion at mean between each
    // 5. purchase fr1 with 100 r1, 50 r2, sell 100 fr1 to r3, 50 to r4
    //          This consists of one composite (multi-layer) buy and one composite sell
    //          a. Compose one two layer purchase of 50 r1 + 50 r2 at 50% and 50 r1 at 25%
    //          b. Compose one two layer sell of 50 r3 + 50 r4 at 50% and 50 r3 at 25%
    //          c. execute each operation of a and b in forward and reverse order and set conversion at mean between results
    //

    std::multimap<CAmount, std::pair<CAmount, uint160>> fractionalIn, fractionalOut;

    // aggregate amounts of ins and outs across all currencies expressed in fractional values in both directions first buy/sell, then sell/buy
    std::map<uint160, std::pair<CAmount, CAmount>> fractionalInMap, fractionalOutMap;

    arith_uint256 bigSupply(supply);

    int32_t totalReserveWeight = 0;
    int32_t maxReserveRatio = 0;

    for (auto weight : weights)
    {
        maxReserveRatio = weight > maxReserveRatio ? weight : maxReserveRatio;
        totalReserveWeight += weight;
    }

    if (!maxReserveRatio)
    {
        LogPrintf("%s: attempting to convert amounts on non-reserve currency\n", __func__);
        return rates;
    }

    arith_uint256 bigMaxReserveRatio = arith_uint256(maxReserveRatio);
    arith_uint256 bigTotalReserveWeight = arith_uint256(totalReserveWeight);

    // reduce each currency change to a net inflow or outflow of fractional currency and
    // store both negative and positive in structures sorted by the net amount, adjusted
    // by the difference of the ratio between the weights of each currency
    for (int64_t i = 0; i < numCurrencies; i++)
    {
        arith_uint256 weight(weights[i]);
        //printf("%s: %ld\n", __func__, ReserveToNative(inputReserves[i], i));
        CAmount netFractional = inputFractional[i] - ReserveToNative(inputReserves[i], i);
        int64_t deltaRatio;
        if (netFractional > 0)
        {
            deltaRatio = ((arith_uint256(netFractional) * bigMaxReserveRatio) / weight).GetLow64();
            fractionalIn.insert(std::make_pair(deltaRatio, std::make_pair(netFractional, currencies[i])));
        }
        else if (netFractional < 0)
        {
            netFractional = -netFractional;
            deltaRatio = ((arith_uint256(netFractional) * bigMaxReserveRatio) / weight).GetLow64();
            fractionalOut.insert(std::make_pair(deltaRatio, std::make_pair(netFractional, currencies[i])));
        }
    }

    // create "layers" of equivalent value at different fractional percentages
    // across currencies going in or out at the same time, enabling their effect on the aggregate
    // to be represented by a larger fractional percent impact of "normalized reserve" on the currency, 
    // which results in accurate pricing impact simulating a basket of currencies.
    //
    // since we have all values sorted, the lowest non-zero value determines the first common layer, then next lowest, the next, etc.
    std::vector<std::pair<int32_t, std::pair<CAmount, std::vector<uint160>>>> fractionalLayersIn, fractionalLayersOut;
    auto reserveMap = GetReserveMap();

    CAmount layerAmount = 0;
    CAmount layerStart;

    for (auto inFIT = fractionalIn.upper_bound(layerAmount); inFIT != fractionalIn.end(); inFIT = fractionalIn.upper_bound(layerAmount))
    {
        // make a common layer out of all entries from here until the end
        int frIdx = fractionalLayersIn.size();
        layerStart = layerAmount;
        layerAmount = inFIT->first;
        CAmount layerHeight = layerAmount - layerStart;
        fractionalLayersIn.emplace_back(std::make_pair(0, std::make_pair(0, std::vector<uint160>())));
        for (auto it = inFIT; it != fractionalIn.end(); it++)
        {
            // reverse the calculation from layer height to amount for this currency, based on currency weight
            int32_t weight = weights[reserveMap[it->second.second]];
            CAmount curAmt = ((arith_uint256(layerHeight) * arith_uint256(weight) / bigMaxReserveRatio)).GetLow64();
            it->second.first -= curAmt;
            assert(it->second.first >= 0);

            fractionalLayersIn[frIdx].first += weight;
            fractionalLayersIn[frIdx].second.first += curAmt;
            fractionalLayersIn[frIdx].second.second.push_back(it->second.second);
        }
    }    

    layerAmount = 0;
    for (auto outFIT = fractionalOut.upper_bound(layerAmount); outFIT != fractionalOut.end(); outFIT = fractionalOut.upper_bound(layerAmount))
    {
        int frIdx = fractionalLayersOut.size();
        layerStart = layerAmount;
        layerAmount = outFIT->first;
        CAmount layerHeight = layerAmount - layerStart;
        fractionalLayersOut.emplace_back(std::make_pair(0, std::make_pair(0, std::vector<uint160>())));
        for (auto it = outFIT; it != fractionalOut.end(); it++)
        {
            int32_t weight = weights[reserveMap[it->second.second]];
            CAmount curAmt = ((arith_uint256(layerHeight) * arith_uint256(weight) / bigMaxReserveRatio)).GetLow64();
            it->second.first -= curAmt;
            assert(it->second.first >= 0);

            fractionalLayersOut[frIdx].first += weight;
            fractionalLayersOut[frIdx].second.first += curAmt;
            fractionalLayersOut[frIdx].second.second.push_back(it->second.second);
        }
    }    

    int64_t supplyAfterBuy = 0, supplyAfterBuySell = 0, supplyAfterSell = 0, supplyAfterSellBuy = 0;
    int64_t reserveAfterBuy = 0, reserveAfterBuySell = 0, reserveAfterSell = 0, reserveAfterSellBuy = 0;

    // first, loop through all buys layer by layer. calculate and divide the proceeds between currencies
    // in each participating layer, in accordance with each currency's relative percentage
    CAmount addSupply = 0;
    CAmount addNormalizedReserves = 0;
    for (auto &layer : fractionalLayersOut)
    {
        // each layer has a fractional percentage/weight and a total amount, determined by the total of all weights for that layer
        // and net amounts across all currencies in that layer. each layer also includes a list of all currencies.
        //
        // calculate a fractional buy at the total layer ratio for the amount specified
        // and divide the value according to the relative weight of each currency, adding to each entry of fractionalOutMap
        arith_uint256 bigLayerWeight = arith_uint256(layer.first);
        CAmount totalLayerReserves = ((bigSupply * bigLayerWeight) / bigSatoshi).GetLow64() + addNormalizedReserves;
        addNormalizedReserves += layer.second.first;
        CAmount newSupply = CalculateFractionalOut(layer.second.first, supply + addSupply, totalLayerReserves, layer.first);
        arith_uint256 bigNewSupply(newSupply);
        addSupply += newSupply;
        for (auto &id : layer.second.second)
        {
            auto idIT = fractionalOutMap.find(id);
            CAmount newSupplyForCurrency = ((bigNewSupply * weights[reserveMap[id]]) / bigLayerWeight).GetLow64();

            // initialize or add to the new supply for this currency
            if (idIT == fractionalOutMap.end())
            {
                fractionalOutMap[id] = std::make_pair(newSupplyForCurrency, int64_t(0));
            }
            else
            {
                idIT->second.first += newSupplyForCurrency;
            }
        }
    }

    supplyAfterBuy = supply + addSupply;
    assert(supplyAfterBuy >= 0);

    reserveAfterBuy = supply + addNormalizedReserves;
    assert(reserveAfterBuy >= 0);

    addSupply = 0;
    addNormalizedReserves = 0;
    CAmount addNormalizedReservesBB = 0, addNormalizedReservesAB = 0;

    // calculate sell both before and after buy through this loop
    for (auto &layer : fractionalLayersIn)
    {
        // first calculate sell before-buy, then after-buy
        arith_uint256 bigLayerWeight(layer.first);

        // before-buy starting point
        CAmount totalLayerReservesBB = ((bigSupply * bigLayerWeight) / bigSatoshi).GetLow64() + addNormalizedReserves;
        CAmount totalLayerReservesAB = ((arith_uint256(supplyAfterBuy) * bigLayerWeight) / bigSatoshi).GetLow64() + addNormalizedReserves;

        CAmount newNormalizedReserveBB = CalculateReserveOut(layer.second.first, supply + addSupply, totalLayerReservesBB + addNormalizedReservesBB, layer.first);
        CAmount newNormalizedReserveAB = CalculateReserveOut(layer.second.first, supplyAfterBuy + addSupply, totalLayerReservesAB + addNormalizedReservesAB, layer.first);

        // input fractional is burned and output reserves are removed from reserves
        addSupply -= layer.second.first;
        addNormalizedReservesBB -= newNormalizedReserveBB;
        addNormalizedReservesAB -= newNormalizedReserveAB;

        for (auto &id : layer.second.second)
        {
            auto idIT = fractionalInMap.find(id);
            CAmount newReservesForCurrencyBB = ((arith_uint256(newNormalizedReserveBB) * arith_uint256(weights[reserveMap[id]])) / bigLayerWeight).GetLow64();
            CAmount newReservesForCurrencyAB = ((arith_uint256(newNormalizedReserveAB) * arith_uint256(weights[reserveMap[id]])) / bigLayerWeight).GetLow64();

            // initialize or add to the new supply for this currency
            if (idIT == fractionalInMap.end())
            {
                fractionalInMap[id] = std::make_pair(newReservesForCurrencyBB, newReservesForCurrencyAB);
            }
            else
            {
                idIT->second.first += newReservesForCurrencyBB;
                idIT->second.second += newReservesForCurrencyAB;
            }
        }
    }

    supplyAfterSell = supply + addSupply;
    assert(supplyAfterSell >= 0);

    supplyAfterBuySell = supplyAfterBuy + addSupply;
    assert(supplyAfterBuySell >= 0);

    reserveAfterSell = supply + addNormalizedReservesBB;
    assert(reserveAfterSell >= 0);

    reserveAfterBuySell = reserveAfterBuy + addNormalizedReservesAB;
    assert(reserveAfterBuySell >= 0);

    addSupply = 0;
    addNormalizedReserves = 0;

    // now calculate buy after sell
    for (auto &layer : fractionalLayersOut)
    {
        arith_uint256 bigLayerWeight = arith_uint256(layer.first);
        CAmount totalLayerReserves = ((arith_uint256(supplyAfterSell) * bigLayerWeight) / bigSatoshi).GetLow64() + addNormalizedReserves;
        addNormalizedReserves += layer.second.first;
        CAmount newSupply = CalculateFractionalOut(layer.second.first, supplyAfterSell + addSupply, totalLayerReserves, layer.first);
        arith_uint256 bigNewSupply(newSupply);
        addSupply += newSupply;
        for (auto &id : layer.second.second)
        {
            auto idIT = fractionalOutMap.find(id);

            assert(idIT != fractionalOutMap.end());

            idIT->second.second += ((bigNewSupply * weights[reserveMap[id]]) / bigLayerWeight).GetLow64();
        }
    }

    // now loop through all currencies, calculate conversion rates for each based on mean of all prices that we calculate for
    // buy before sell and sell before buy
    std::vector<int64_t> fractionalSizes(numCurrencies,0);
    std::vector<int64_t> reserveSizes(numCurrencies,0);

    for (int i = 0; i < numCurrencies; i++)
    {
        // each coin has an amount of reserve in, an amount of fractional in, and potentially two delta amounts in one of the
        // fractionalInMap or fractionalOutMap maps, one for buy before sell and one for sell before buy.
        // add the mean of the delta amounts to the appropriate side of the equation and calculate a price for each
        // currency.
        auto fractionalOutIT = fractionalOutMap.find(currencies[i]);
        auto fractionalInIT = fractionalInMap.find(currencies[i]);

        auto inputReserve = inputReserves[i];
        auto inputFraction = inputFractional[i];
        reserveSizes[i] = inputReserve;
        fractionalSizes[i] = inputFraction;

        CAmount fractionDelta = 0, reserveDelta = 0;

        if (fractionalOutIT != fractionalOutMap.end())
        {
            arith_uint256 bigFractionDelta(fractionalOutIT->second.first);
            fractionDelta = ((bigFractionDelta + arith_uint256(fractionalOutIT->second.second)) >> 1).GetLow64();
            assert(inputFraction + fractionDelta > 0);

            fractionalSizes[i] += fractionDelta;
            rates[i] = ((arith_uint256(inputReserve) * bigSatoshi) / arith_uint256(fractionalSizes[i])).GetLow64();

            // add the new reserve and supply to the currency
            newState.supply += fractionDelta;

            // all reserves have been calculated using a substituted value, which was 1:1 for native initially
            newState.reserves[i] += inputFractional[i] ? NativeToReserveRaw(fractionDelta, rates[i]) : inputReserves[i];
        }
        else if (fractionalInIT != fractionalInMap.end())
        {
            arith_uint256 bigReserveDelta(fractionalInIT->second.first);
            CAmount adjustedReserveDelta = NativeToReserve(((bigReserveDelta + arith_uint256(fractionalInIT->second.second)) >> 1).GetLow64(), i);
            reserveSizes[i] += adjustedReserveDelta;
            assert(inputFraction > 0);

            rates[i] = ((arith_uint256(reserveSizes[i]) * bigSatoshi) / arith_uint256(inputFraction)).GetLow64();

            // subtract the fractional and reserve that has left the currency
            newState.supply -= inputFraction;
            newState.reserves[i] -= adjustedReserveDelta;
        }
    }

    // if we have cross conversions, complete a final conversion with the updated currency, including all of the
    // cross conversion outputs to their final currency destinations
    if (pCrossConversions)
    {
        bool convertRToR = false;
        std::vector<CAmount> reservesRToR(numCurrencies, 0);    // keep track of reserve inputs to convert to the fractional currency

        // now add all cross conversions, determine how much of the converted fractional should be converted back to each
        // reserve currency. after adding all together, convert all to each reserve and average the price again
        for (int i = 0; i < numCurrencies; i++)
        {
            // add up all conversion amounts for each fractional to each reserve-to-reserve conversion
            for (int j = 0; j < numCurrencies; j++)
            {
                // convert this much of currency indexed by i into currency indexed by j
                // figure out how much fractional the amount of currency represents and add it to the total 
                // fractionalIn for the currency indexed by j
                if ((*pCrossConversions)[i][j])
                {
                    convertRToR = true;
                    reservesRToR[i] += (*pCrossConversions)[i][j];
                }
            }
        }

        if (convertRToR)
        {
            std::vector<CAmount> scratchValues(numCurrencies, 0);
            std::vector<CAmount> fractionsToConvert(numCurrencies, 0);

            // add fractional created to be converted to its destination
            for (int i = 0; i < reservesRToR.size(); i++)
            {
                if (reservesRToR[i])
                {
                    for (int j = 0; j < (*pCrossConversions)[i].size(); j++)
                    {
                        if ((*pCrossConversions)[i][j])
                        {
                            fractionsToConvert[j] += ReserveToNativeRaw((*pCrossConversions)[i][j], rates[i]);
                        }
                    }
                }
            }

            std::vector<CAmount> _viaPrices;
            std::vector<CAmount> &viaPrices(pViaPrices ? *pViaPrices : _viaPrices);
            CCurrencyState intermediateState = newState;
            viaPrices = intermediateState.ConvertAmounts(scratchValues, fractionsToConvert, newState);
        }
    }

    for (int i = 0; i < rates.size(); i++)
    {
        if (!rates[i])
        {
            rates[i] = PriceInReserve(i);
        }
    }
    return rates;
}

CAmount CCurrencyState::ConvertAmounts(CAmount inputReserve, CAmount inputFraction, CCurrencyState &newState, int32_t reserveIndex) const
{
    int32_t numCurrencies = currencies.size();
    if (reserveIndex >= numCurrencies)
    {
        printf("%s: reserve index out of range\n", __func__);
        return 0;
    }
    std::vector<CAmount> inputReserves(numCurrencies);
    inputReserves[reserveIndex] = inputReserve;
    std::vector<CAmount> inputFractional(numCurrencies);
    inputFractional[reserveIndex] = inputFraction;
    std::vector<CAmount> retVal = ConvertAmounts(inputReserves,
                                                 inputFractional,
                                                 newState);
    return retVal[reserveIndex];
}

void CReserveTransactionDescriptor::AddReserveInput(const uint160 &currency, CAmount value)
{
    //printf("adding %ld:%s reserve input\n", value, EncodeDestination(CIdentityID(currency)).c_str());
    currencies[currency].reserveIn += value;
}

void CReserveTransactionDescriptor::AddReserveOutput(const uint160 &currency, CAmount value)
{
    //printf("adding %ld:%s reserve output\n", value, EncodeDestination(CIdentityID(currency)).c_str());
    currencies[currency].reserveOut += value;
}

void CReserveTransactionDescriptor::AddReserveOutConverted(const uint160 &currency, CAmount value)
{
    currencies[currency].reserveOutConverted += value;
}

void CReserveTransactionDescriptor::AddNativeOutConverted(const uint160 &currency, CAmount value)
{
    currencies[currency].nativeOutConverted += value;
}

void CReserveTransactionDescriptor::AddReserveConversionFees(const uint160 &currency, CAmount value)
{
    currencies[currency].reserveConversionFees += value;
}

void CReserveTransactionDescriptor::AddReserveExchange(const CReserveExchange &rex, int32_t outputIndex, int32_t nHeight)
{
    CAmount fee = 0;

    bool wasMarket = IsMarket();

    flags |= IS_RESERVE + IS_RESERVEEXCHANGE;

    if (IsLimit() || rex.flags & CReserveExchange::LIMIT)
    {
        if (wasMarket || (vRex.size() && (vRex.back().second.flags != rex.flags || (vRex.back().second.nValidBefore != rex.nValidBefore))))
        {
            flags |= IS_REJECT;
            return;
        }

        flags |= IS_LIMIT;

        if (rex.nValidBefore > nHeight)
        {
            flags |= IS_FILLORKILL;
            // fill or kill fee must be pre-subtracted, but is added back on success, making conversion the only
            // requred fee on success
            fee = CalculateConversionFee(rex.nValue + rex.FILL_OR_KILL_FEE);
            if (rex.flags & CReserveExchange::TO_RESERVE)
            {
                numSells += 1;
                nativeConversionFees += fee;
                AddNativeOutConverted(rex.currencyID, (rex.nValue + rex.FILL_OR_KILL_FEE) - fee);
                nativeOut += rex.nValue + rex.FILL_OR_KILL_FEE;             // output is always less fees
            }
            else
            {
                numBuys += 1;
                auto it = currencies.find(rex.currencyID);
                if (it != currencies.end())
                {
                    it->second.reserveOut += rex.nValue + rex.FILL_OR_KILL_FEE;
                    it->second.reserveOutConverted += rex.nValue + rex.FILL_OR_KILL_FEE - fee;
                    it->second.reserveConversionFees += fee;
                }
                else
                {
                    currencies[rex.currencyID] = CReserveInOuts(0, rex.nValue + rex.FILL_OR_KILL_FEE, rex.nValue + rex.FILL_OR_KILL_FEE - fee, 0, fee);
                }
            }
        }
        else
        {
            flags &= !IS_RESERVEEXCHANGE;                       // no longer a reserve exchange transaction, falls back to normal reserve tx
            flags |= IS_FILLORKILLFAIL;

            if (rex.flags & CReserveExchange::TO_RESERVE)
            {
                numSells += 1;
                nativeOut += rex.nValue;
            }
            else
            {
                numBuys += 1;
                AddReserveOutput(rex.currencyID, rex.nValue);
            }
        }
    }
    else
    {
        fee = CalculateConversionFee(rex.nValue);
        if (rex.flags & CReserveExchange::TO_RESERVE)
        {
            numSells += 1;
            nativeConversionFees += fee;
            AddNativeOutConverted(rex.currencyID, rex.nValue - fee);
            nativeOut += rex.nValue;                    // conversion fee is considered part of the total native out, since it will be attributed to conversion tx
        }
        else
        {
            numBuys += 1;
            if (!(flags & IS_IMPORT))
            {
                auto it = currencies.find(rex.currencyID);
                if (it != currencies.end())
                {
                    it->second.reserveOut += rex.nValue;
                    it->second.reserveOutConverted += rex.nValue - fee;
                    it->second.reserveConversionFees += fee;
                }
                else
                {
                    currencies[rex.currencyID] = CReserveInOuts(0, rex.nValue, rex.nValue - fee, 0, fee);
                }
            }
        }
    }                        
    vRex.push_back(std::make_pair(outputIndex, rex));
}

CAmount CReserveTransactionDescriptor::AllFeesAsNative(const CCurrencyState &currencyState) const
{
    CAmount nativeFees = NativeFees();
    CCurrencyValueMap reserveFees = ReserveFees();
    for (int i = 0; i < currencyState.currencies.size(); i++)
    {
        auto it = reserveFees.valueMap.find(currencyState.currencies[i]);
        if (it != reserveFees.valueMap.end())
        {
            nativeFees += currencyState.ReserveToNative(it->second, i);
        }
    }
    return nativeFees;
}

CAmount CReserveTransactionDescriptor::AllFeesAsNative(const CCurrencyState &currencyState, const std::vector<CAmount> &exchangeRates) const
{
    assert(exchangeRates.size() == currencyState.currencies.size());
    CAmount nativeFees = NativeFees();
    CCurrencyValueMap reserveFees = ReserveFees();
    for (int i = 0; i < currencyState.currencies.size(); i++)
    {
        auto it = reserveFees.valueMap.find(currencyState.currencies[i]);
        if (it != reserveFees.valueMap.end())
        {
            nativeFees += currencyState.ReserveToNativeRaw(it->second, exchangeRates[i]);
        }
    }
    return nativeFees;
}

CCurrencyValueMap CReserveTransactionDescriptor::ReserveFees(const uint160 &nativeID) const
{
    uint160 id = nativeID.IsNull() ? ASSETCHAINS_CHAINID : nativeID;
    CCurrencyValueMap retFees;
    for (auto &one : currencies)
    {
        // skip native
        if (one.first != id)
        {
            CAmount oneFee = one.second.reserveIn - (one.second.reserveOut - one.second.reserveOutConverted);
            if (oneFee)
            {
                retFees.valueMap[one.first] = oneFee;
            }
        }
    }
    return retFees;
}

CAmount CReserveTransactionDescriptor::NativeFees() const
{
    return nativeIn - nativeOut;
}

CCurrencyValueMap CReserveTransactionDescriptor::AllFeesAsReserve(const CCurrencyState &currencyState, int defaultReserve) const
{
    CCurrencyValueMap reserveFees = ReserveFees();

    auto it = reserveFees.valueMap.find(currencyState.currencies[defaultReserve]);
    if (it != reserveFees.valueMap.end())
    {
        it->second += currencyState.NativeToReserve(NativeFees(), defaultReserve);
    }
    else
    {
        reserveFees.valueMap[currencyState.currencies[defaultReserve]] = NativeFees();
    }
    return reserveFees;
}

CCurrencyValueMap CReserveTransactionDescriptor::AllFeesAsReserve(const CCurrencyState &currencyState, const std::vector<CAmount> &exchangeRates, int defaultReserve) const
{
    CCurrencyValueMap reserveFees = ReserveFees();

    auto it = reserveFees.valueMap.find(currencyState.currencies[defaultReserve]);
    if (it != reserveFees.valueMap.end())
    {
        it->second += currencyState.NativeToReserveRaw(NativeFees(), exchangeRates[defaultReserve]);
    }
    else
    {
        reserveFees.valueMap[currencyState.currencies[defaultReserve]] = NativeFees();
    }
    return reserveFees;
}

/*
 * Checks all structural aspects of the reserve part of a transaction that may have reserve inputs and/or outputs
 */
CReserveTransactionDescriptor::CReserveTransactionDescriptor(const CTransaction &tx, const CCoinsViewCache &view, int32_t nHeight) :
        flags(0),
        ptx(NULL),
        numBuys(0),
        numSells(0),
        numTransfers(0),
        nativeIn(0),
        nativeOut(0),
        nativeConversionFees(0)
{
    // market conversions can have any number of both buy and sell conversion outputs, this is used to make efficient, aggregated
    // reserve transfer operations with conversion

    // limit conversion outputs may have multiple outputs with different input amounts and destinations, 
    // but they must not be mixed in a transaction with any dissimilar set of conditions on the output, 
    // including mixing with market orders, parity of buy or sell, limit value and validbefore values, 
    // or the transaction is considered invalid

    // no inputs are valid at height 0
    if (!nHeight)
    {
        flags |= IS_REJECT;
        return;
    }

    // reserve descriptor transactions cannot run until identity activates
    if (!chainActive.LastTip() ||
        CConstVerusSolutionVector::activationHeight.ActiveVersion(nHeight) < CConstVerusSolutionVector::activationHeight.ACTIVATE_IDENTITY)
    {
        return;
    }

    bool isVerusActive = IsVerusActive();
    bool isPBaaSActivation = CConstVerusSolutionVector::activationHeight.IsActivationHeight(CActivationHeight::ACTIVATE_PBAAS, nHeight);

    CCurrencyDefinition importCurrencyDef;
    CCrossChainImport cci;
    CCrossChainExport ccx;

    CNameReservation nameReservation;
    CIdentity identity;
    CCurrencyDefinition newCurrencyDef;

    flags |= IS_VALID;

    for (int i = 0; i < tx.vout.size(); i++)
    {
        COptCCParams p;

        if (tx.vout[i].scriptPubKey.IsPayToCryptoCondition(p) && p.IsValid())
        {
            switch (p.evalCode)
            {
                case EVAL_IDENTITY_RESERVATION:
                {
                    // one name reservation per transaction
                    if (p.version < p.VERSION_V3 || !p.vData.size() || nameReservation.IsValid() || !(nameReservation = CNameReservation(p.vData[0])).IsValid())
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }
                    if (identity.IsValid())
                    {
                        if (identity.name == nameReservation.name)
                        {
                            flags |= IS_IDENTITY_DEFINITION + IS_HIGH_FEE;
                        }
                        else
                        {
                            flags &= ~IS_VALID;
                            flags |= IS_REJECT;
                            return;
                        }
                    }
                }
                break;

                case EVAL_IDENTITY_PRIMARY:
                {
                    // one identity per transaction
                    if (p.version < p.VERSION_V3 || !p.vData.size() || identity.IsValid() || !(identity = CIdentity(p.vData[0])).IsValid())
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }
                    flags |= IS_IDENTITY;
                    if (nameReservation.IsValid())
                    {
                        if (identity.name == nameReservation.name)
                        {
                            flags |= IS_IDENTITY_DEFINITION + IS_HIGH_FEE;
                        }
                        else
                        {
                            flags &= ~IS_VALID;
                            flags |= IS_REJECT;
                            return;
                        }
                    }
                }
                break;

                case EVAL_RESERVE_DEPOSIT:
                {
                    CReserveDeposit rd;
                    if (!p.vData.size() || !(rd = CReserveDeposit(p.vData[0])).IsValid())
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }
                    for (auto &oneCur : rd.reserveValues.valueMap)
                    {
                        if (oneCur.first != ASSETCHAINS_CHAINID)
                        {
                            AddReserveOutput(oneCur.first, oneCur.second);
                        }
                    }
                }
                break;

                case EVAL_RESERVE_OUTPUT:
                {
                    CTokenOutput ro;
                    if (!p.vData.size() || !(ro = CTokenOutput(p.vData[0])).IsValid())
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }
                    if (ro.nValue)
                    {
                        AddReserveOutput(ro);
                    }
                }
                break;

                case EVAL_RESERVE_TRANSFER:
                {
                    CReserveTransfer rt;
                    if (!p.vData.size() || !(rt = CReserveTransfer(p.vData[0])).IsValid())
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }
                    AddReserveTransfer(rt);
                }
                break;

                case EVAL_RESERVE_EXCHANGE:
                {
                    CReserveExchange rex;
                    if (isVerusActive || !p.vData.size() || !(rex = CReserveExchange(p.vData[0])).IsValid())
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }

                    // if we send the output to the reserve chain, it must be a TO_RESERVE transaction
                    if ((rex.flags & rex.SEND_OUTPUT) && !(rex.flags & rex.TO_RESERVE))
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }
                    AddReserveExchange(rex, i, nHeight);

                    if (IsReject())
                    {
                        flags &= ~IS_VALID;
                        return;
                    }
                }
                break;

                case EVAL_CROSSCHAIN_IMPORT:
                {
                    // if this is an import, add the amount imported to the reserve input and the amount of reserve output as
                    // the amount available to take from this transaction in reserve as an import fee
                    if (flags & IS_IMPORT || !p.vData.size() || !(cci = CCrossChainImport(p.vData[0])).IsValid())
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }

                    flags |= IS_IMPORT;

                    std::vector<CBaseChainObject *> chainObjs;
                    // either a fully valid import with an export or the first import either in block 1 or the chain definition
                    // on the Verus chain
                    std::vector<CCurrencyDefinition> txCurrencies = CCurrencyDefinition::GetCurrencyDefinitions(tx);
                    if ((nHeight == 1 && tx.IsCoinBase() && !IsVerusActive()) ||
                        (txCurrencies.size() == 1 &&
                        ((txCurrencies[0].parent == ASSETCHAINS_CHAINID) || 
                         (txCurrencies[0].GetID() == ASSETCHAINS_CHAINID && isPBaaSActivation)) &&
                        isVerusActive) ||
                        (tx.vout.back().scriptPubKey.IsOpReturn() &&
                        (chainObjs = RetrieveOpRetArray(tx.vout.back().scriptPubKey)).size() >= 1 &&
                        chainObjs[0]->objectType == CHAINOBJ_TRANSACTION_PROOF))
                    {
                        if (chainObjs.size())
                        {
                            CPartialTransactionProof &exportTxProof = ((CChainObject<CPartialTransactionProof> *)chainObjs[0])->object;
                            CTransaction exportTx;
                            std::vector<CBaseChainObject *> exportTransfers;

                            if (exportTxProof.txProof.proofSequence.size() == 3 &&
                                !exportTxProof.GetPartialTransaction(exportTx).IsNull() &&
                                (ccx = CCrossChainExport(exportTx)).IsValid() && 
                                exportTx.vout.back().scriptPubKey.IsOpReturn() &&
                                (exportTransfers = RetrieveOpRetArray(exportTx.vout.back().scriptPubKey)).size() &&
                                exportTransfers[0]->objectType == CHAINOBJ_RESERVETRANSFER)
                            {
                                // an import transaction is built from the export list
                                // we can rebuild it and confirm that it matches exactly
                                // we can also subtract all pre-converted reserve from input
                                // and verify that the output matches the proper conversion

                                // get the chain definition of the chain we are importing
                                std::vector<CTxOut> checkOutputs;

                                CPBaaSNotarization importNotarization;
                                importCurrencyDef = ConnectedChains.GetCachedCurrency(cci.importCurrencyID);
                                if (importCurrencyDef.IsToken() && !(importNotarization = CPBaaSNotarization(tx)).IsValid())
                                {
                                    flags |= IS_REJECT;
                                }
                                else
                                {
                                    CCoinbaseCurrencyState currencyState = importCurrencyDef.IsToken() ?
                                                                           importNotarization.currencyState :
                                                                           GetInitialCurrencyState(importCurrencyDef);
                                    importCurrencyDef.conversions = currencyState.conversionPrice;

                                    if (!currencyState.IsValid() ||
                                        !AddReserveTransferImportOutputs(cci.systemID, importCurrencyDef, currencyState, exportTransfers, checkOutputs))
                                    {
                                        flags |= IS_REJECT;
                                    }

                                    for (auto &oneOutCur : cci.totalReserveOutMap.valueMap)
                                    {
                                        AddReserveOutput(oneOutCur.first, oneOutCur.second);
                                    }
                                }
                                // TODO:PBAAS - hardening - validate all the outputs we got back as the same as what is in the transaction
                            }
                            else
                            {
                                flags |= IS_REJECT;
                            }
                        }
                    }
                    else
                    {
                        flags |= IS_REJECT;
                    }
                    DeleteOpRetObjects(chainObjs);
                    if (IsReject())
                    {
                        flags &= ~IS_VALID;
                        return;
                    }
                }
                break;

                // this check will need to be made complete by preventing mixing both here and where the others
                // are seen
                case EVAL_CROSSCHAIN_EXPORT:
                {
                    // cross chain export is incompatible with reserve exchange outputs
                    std::vector<CCurrencyDefinition> txCurrencies = CCurrencyDefinition::GetCurrencyDefinitions(tx);
                    if (IsReserveExchange() ||
                        (!(nHeight == 1 && tx.IsCoinBase() && !isVerusActive) &&
                         !(txCurrencies.size() &&
                          ((txCurrencies[0].parent == ASSETCHAINS_CHAINID) || 
                          (txCurrencies[0].GetID() == ASSETCHAINS_CHAINID && isPBaaSActivation))) &&
                        (flags & IS_IMPORT)))
                    {
                        flags &= ~IS_VALID;
                        flags |= IS_REJECT;
                        return;
                    }
                    flags |= IS_EXPORT;
                }
                break;

                case EVAL_CURRENCY_DEFINITION:
                {

                }
                break;
            }
        }
    }

    // we have all inputs, outputs, and fees, if check inputs, we can check all for consistency
    // inputs may be in the memory pool or on the blockchain
    CAmount dummyInterest;
    nativeOut = tx.GetValueOut();
    nativeIn = view.GetValueIn(nHeight, &dummyInterest, tx);

    CCurrencyValueMap importGeneratedCurrency;
    if (flags & IS_IMPORT)
    {
        importGeneratedCurrency = GeneratedImportCurrency(cci.systemID, importCurrencyDef.systemID, cci.importCurrencyID);
    }

    if (importGeneratedCurrency.valueMap.count(ASSETCHAINS_CHAINID))
    {
        nativeIn += importGeneratedCurrency.valueMap[ASSETCHAINS_CHAINID];
        importGeneratedCurrency.valueMap.erase(ASSETCHAINS_CHAINID);
    }

    // if it is a conversion to reserve, the amount in is accurate, since it is from the native coin, if converting to
    // the native PBaaS coin, the amount input is a sum of all the reserve token values of all of the inputs
    auto reservesIn = (view.GetReserveValueIn(nHeight, tx) + importGeneratedCurrency).CanonicalMap();
    for (auto &oneCur : currencies)
    {
        oneCur.second.reserveIn = 0;
    }
    if (reservesIn.valueMap.size())
    {
        flags |= IS_RESERVE;
        for (auto &oneCur : reservesIn.valueMap)
        {
            currencies[oneCur.first].reserveIn = oneCur.second;
        }
    }
    
    if (!IsReserve() && ReserveOutputMap().valueMap.size())
    {
        flags |= IS_RESERVE;
    }

    // TODO:PBAAS hardening total minimum required fees as we build the descriptor and
    // reject if not correct
    ptx = &tx;
}

// this is only valid when used after AddReserveTransferImportOutputs on an empty CReserveTransactionDwescriptor
CCurrencyValueMap CReserveTransactionDescriptor::GeneratedImportCurrency(const uint160 &fromSystemID, const uint160 &importSystemID, const uint160 &importCurrencyID) const
{
    // only currencies that are controlled by the exporting chain or created in conversion by the importing currency
    // can be created from nothing
    // add newly created currency here that meets those criteria
    CCurrencyValueMap retVal;
    for (auto one : currencies)
    {
        bool isImportCurrency = one.first == importCurrencyID;
        if ((one.second.nativeOutConverted && isImportCurrency) || 
              (one.second.reserveIn && fromSystemID != ASSETCHAINS_CHAINID && ConnectedChains.GetCachedCurrency(one.first).systemID == fromSystemID))
        {
            retVal.valueMap[one.first] = isImportCurrency ? one.second.nativeOutConverted : one.second.reserveIn;
        }
    }
    return retVal;
}

CReserveTransfer RefundExport(const CBaseChainObject *objPtr)
{
    if (objPtr->objectType == CHAINOBJ_RESERVETRANSFER)
    {
        CReserveTransfer &rt = ((CChainObject<CReserveTransfer> *)objPtr)->object;

        // convert full ID destinations to normal ID outputs, since it's refund, full ID will be on this chain already
        if (rt.destination.type == CTransferDestination::DEST_FULLID)
        {
            CIdentity(rt.destination.destination);
            rt.destination = CTransferDestination(CTransferDestination::DEST_ID, rt.destination.destination);
        }

        // turn it into a normal transfer, which will create an unconverted output
        rt.flags &= ~(CReserveTransfer::SEND_BACK | CReserveTransfer::PRECONVERT | CReserveTransfer::CONVERT);

        if (rt.flags & (CReserveTransfer::PREALLOCATE | CReserveTransfer::MINT_CURRENCY))
        {
            rt.flags &= ~(CReserveTransfer::PREALLOCATE | CReserveTransfer::MINT_CURRENCY);
            rt.nValue = 0;
        }
        rt.destCurrencyID = rt.currencyID;
        return rt;
    }
    return CReserveTransfer();
}

// the source currency indicates the system from which the import comes, but the imports may contain additional
// currencies that are supported in that system and are not limited to the native currency. Fees are assumed to
// be covered by the native currency of the source or source currency, if this is a reserve conversion. That 
// means that all explicit fees are assumed to be in the currency of the source.
bool CReserveTransactionDescriptor::AddReserveTransferImportOutputs(const uint160 &nativeSourceCurrencyID, 
                                                                    const CCurrencyDefinition &importCurrencyDef, 
                                                                    const CCoinbaseCurrencyState &importCurrencyState,
                                                                    const std::vector<CBaseChainObject *> &exportObjects, 
                                                                    std::vector<CTxOut> &vOutputs,
                                                                    CCoinbaseCurrencyState *pNewCurrencyState)
{
    // easy way to refer to return currency state or a dummy without conditionals
    CCoinbaseCurrencyState _newCurrencyState;
    if (!pNewCurrencyState)
    {
        pNewCurrencyState = &_newCurrencyState;
    }
    CCoinbaseCurrencyState &newCurrencyState = *pNewCurrencyState;
    newCurrencyState = importCurrencyState;
    newCurrencyState.ClearForNextBlock();

    bool isFractional = importCurrencyDef.IsFractional();

    // reserve currency amounts converted to fractional
    CCurrencyValueMap reserveConverted;

    // fractional currency amount and the reserve it is converted to
    CCurrencyValueMap fractionalConverted;

    std::map<uint160,int32_t> currencyIndexMap = importCurrencyDef.GetCurrenciesMap();

    uint160 systemDestID = importCurrencyDef.systemID;  // native on destination system
    uint160 importCurrencyID = importCurrencyDef.GetID();
    //printf("%s\n", importCurrencyDef.ToUniValue().write(1,2).c_str());

    // this matrix tracks n-way currency conversion
    // each entry contains the original amount of the row's (dim 0) currency to be converted to the currency position of its column
    int32_t numCurrencies = importCurrencyDef.currencies.size();
    std::vector<std::vector<CAmount>> crossConversions(numCurrencies, std::vector<CAmount>(numCurrencies, 0));
    int32_t systemDestIdx = currencyIndexMap.count(systemDestID) ? currencyIndexMap[systemDestID] : -1;

    // used to keep track of burned fractional currency. this currency is subtracted from the
    // currency supply, but not converted. In doing so, it can either raise the price of the fractional
    // currency in all other currencies, or increase the reserve ratio of all currencies by some slight amount.
    CAmount burnedChangePrice = 0;
    CAmount burnedChangeWeight = 0;
    CAmount secondaryBurned = 0;

    // this is cached here, but only used for pre-conversions
    CCoinbaseCurrencyState initialCurrencyState;
    CCurrencyValueMap preConvertedOutput;
    CCurrencyValueMap preConvertedReserves;

    // we do not change native in or conversion fees, but we define all of these members
    // EVAL_CROSSCHAIN_IMPORT must be the first output of an actual import transaction with any reserve
    // value output. since a notarization may be before it in the outputs, the notarization may not
    // have any reserve value out. it can start an import
    nativeIn = 0;
    numTransfers = 0;
    for (auto &oneInOut : currencies)
    {
        oneInOut.second.reserveIn = 0;
        oneInOut.second.reserveOut = 0;
    }

    CCcontract_info CC;
    CCcontract_info *cp;

    std::map<uint160, CAmount> preAllocMap;             // if this contains pre-allocations, only make the necessary map once
    CCurrencyValueMap transferFees;                     // calculated fees based on all transfers/conversions, etc.
    CCurrencyValueMap feeOutputs;                       // actual fee output amounts

    bool feeOutputStart = false;                        // fee outputs must come after all others, this indicates they have started
    int nFeeOutputs = 0;                                // number of fee outputs

    bool carveOutSet = false;
    int32_t totalCarveOut;
    CCurrencyValueMap totalCarveOuts;
    CAmount totalMinted = 0;
    CAmount exporterReward = 0;

    for (int i = 0; i < exportObjects.size(); i++)
    {
        if (exportObjects[i]->objectType == CHAINOBJ_RESERVETRANSFER)
        {
            CReserveTransfer curTransfer;

            if (importCurrencyState.IsRefunding())
            {
                curTransfer = RefundExport(exportObjects[i]);
            }
            else
            {
                curTransfer = ((CChainObject<CReserveTransfer> *)exportObjects[i])->object;
            }

            if (((importCurrencyID != curTransfer.currencyID) && (curTransfer.flags & curTransfer.IMPORT_TO_SOURCE)) ||
                ((importCurrencyID == curTransfer.currencyID) && !((curTransfer.flags & curTransfer.IMPORT_TO_SOURCE))))
            {
                printf("%s: Importing to source currency without flag or importing to destination with source flag\n", __func__);
                LogPrintf("%s: Importing to source currency without flag or importing to destination with source flag\n", __func__);
                return false;
            }

            //printf("currency transfer #%d:\n%s\n", i, curTransfer.ToUniValue().write(1,2).c_str());
            CCurrencyDefinition _currencyDest;
            const CCurrencyDefinition &currencyDest = (importCurrencyID == curTransfer.destCurrencyID) ?
                                                      importCurrencyDef :
                                                      (_currencyDest = ConnectedChains.GetCachedCurrency(curTransfer.destCurrencyID));

            if (!currencyDest.IsValid())
            {
                printf("%s: invalid currency or currency not found %s\n", __func__, EncodeDestination(CIdentityID(curTransfer.destCurrencyID)).c_str());
                LogPrintf("%s: invalid currency or currency not found %s\n", __func__, EncodeDestination(CIdentityID(curTransfer.destCurrencyID)).c_str());
                return false;
            }

            if (curTransfer.IsValid())
            {
                CTxOut newOut;
                numTransfers++;

                // every export has one fee output, which contains the
                // exporting recipient, who will get a reward output of the
                // export fee + 10% of any export fee amount over 0.0002.
                // the remainder goes into the block fee pool
                if (curTransfer.flags & curTransfer.FEE_OUTPUT)
                {
                    feeOutputStart = true;
                    if (nFeeOutputs++)
                    {
                        printf("%s: Only one fee output per import transaction\n", __func__);
                        LogPrintf("%s: Only one fee output per import transaction\n", __func__);
                        return false;
                    }
                    numTransfers--;

                    // convert all fees to the system currency of the import
                    // fees that started in fractional are already converted, so not considered
                    CAmount totalNativeFee = 0;
                    CAmount totalFractionalFee = 0;
                    if (isFractional)
                    {
                        // setup conversion matrix for fees that are converted to
                        // native from another reserve
                        for (auto &oneFee : transferFees.valueMap)
                        {
                            // only convert in second stage if we are going from one reserve to the system ID
                            if (oneFee.first != importCurrencyID && oneFee.first != systemDestID)
                            {
                                CAmount oneFeeValue = 0;
                                crossConversions[currencyIndexMap[oneFee.first]][systemDestIdx] += oneFee.second;
                                oneFeeValue = importCurrencyState.ReserveToNativeRaw(oneFee.second,
                                                                           importCurrencyState.conversionPrice[currencyIndexMap[oneFee.first]]);
                                totalFractionalFee += oneFeeValue;
                                if (systemDestID == importCurrencyID)
                                {
                                    AddNativeOutConverted(oneFee.first, oneFeeValue);
                                    nativeIn += oneFeeValue;
                                    totalNativeFee += oneFeeValue;
                                }
                            }
                            else if (oneFee.first == systemDestID)
                            {
                                totalNativeFee += oneFee.second;
                            }
                            else
                            {
                                totalFractionalFee += oneFee.second;
                            }
                        }
                        // if fractional currency is not native, one more conversion to native
                        if (totalFractionalFee && systemDestID != importCurrencyID)
                        {
                            CAmount newConvertedNative = CCurrencyState::NativeToReserveRaw(totalFractionalFee, importCurrencyState.viaConversionPrice[systemDestIdx]);
                            totalNativeFee += newConvertedNative;
                            AddReserveOutConverted(systemDestID, newConvertedNative);
                        }
                    }
                    else if (!transferFees.valueMap.count(systemDestID))
                    {
                        printf("%s: No valid fees found\n", __func__);
                        LogPrintf("%s: No valid fees found\n", __func__);
                        return false;
                    }
                    else
                    {
                        totalNativeFee = transferFees.valueMap[systemDestID];
                    }

                    // export fee is sent to the export pool of the sending
                    // system, exporter reward directly to the exporter
                    CAmount exportFee = CCrossChainExport::CalculateExportFeeRaw(totalNativeFee, numTransfers);
                    exporterReward = CCrossChainExport::ExportReward(exportFee);
                    if (totalNativeFee > exporterReward)
                    {
                        nativeOut += (totalNativeFee - exporterReward);
                    }

                    curTransfer = CReserveTransfer(CReserveTransfer::VALID + CReserveTransfer::FEE_OUTPUT,
                                                   systemDestID, exporterReward, 0, systemDestID, curTransfer.destination);
                }
                else if (feeOutputStart)
                {
                    printf("%s: Invalid non-fee output after fee output %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                    LogPrintf("%s: Invalid non-fee output after fee output %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                    return false;
                }
                else
                {
                    if (curTransfer.nFees < curTransfer.CalculateTransferFee())
                    {
                        printf("%s: Incorrect fee sent with export %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                        LogPrintf("%s: Incorrect fee sent with export %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                        return false;
                    }

                    if (curTransfer.currencyID == systemDestID && !(curTransfer.IsMint() || curTransfer.IsPreallocate()))
                    {
                        nativeIn += (curTransfer.nValue + curTransfer.nFees);
                        transferFees.valueMap[systemDestID] += curTransfer.nFees;
                    }
                    else
                    {
                        // when minting new currency or burning a fractional for conversion to a reserve,
                        // we only add fees
                        if (curTransfer.IsMint() || curTransfer.IsPreallocate())
                        {
                            nativeIn += curTransfer.nFees;
                            transferFees.valueMap[systemDestID] += curTransfer.nFees;
                            AddReserveInput(curTransfer.destCurrencyID, curTransfer.nValue);
                        }
                        else if (curTransfer.IsFeeDestNative())
                        {
                            nativeIn += curTransfer.nFees;
                            transferFees.valueMap[systemDestID] += curTransfer.nFees;
                            AddReserveInput(curTransfer.currencyID, curTransfer.nValue);
                        }
                        else
                        {
                            AddReserveInput(curTransfer.currencyID, curTransfer.nValue + curTransfer.nFees);
                            transferFees.valueMap[curTransfer.currencyID] += curTransfer.nFees;
                        }
                    }

                    if (curTransfer.flags & curTransfer.IsPreallocate())
                    {
                        // look up preallocation in the currency definition based on ID, add the correct amount to input
                        // and to the transfer for output
                        if (currencyDest.preAllocation.size() && !preAllocMap.size())
                        {
                            for (auto &onePreAlloc : currencyDest.preAllocation)
                            {
                                preAllocMap.insert(onePreAlloc);
                            }
                        }

                        auto it = preAllocMap.find(GetDestinationID(TransferDestinationToDestination(curTransfer.destination)));
                        if (it == preAllocMap.end() || it->second != curTransfer.nValue)
                        {
                            printf("%s: Invalid preallocation transfer\n", __func__);
                            LogPrintf("%s: Invalid preallocation transfer\n", __func__);
                            return false;
                        }
                    }
                }

                if (curTransfer.IsPreConversion())
                {
                    // first time through with preconvert, initialize the starting currency state
                    if (!initialCurrencyState.IsValid())
                    {
                        initialCurrencyState = ConnectedChains.GetCurrencyState(importCurrencyID, currencyDest.startBlock - 1);
                        if (!initialCurrencyState.IsValid())
                        {
                            printf("%s: Invalid currency for preconversion %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                            LogPrintf("%s: Invalid currency for preconversion %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                            return false;
                        }
                    }

                    // either the destination currency must be fractional or the source currency
                    // must be native
                    if (!isFractional && curTransfer.currencyID != importCurrencyDef.systemID)
                    {
                        printf("%s: Invalid conversion %s. Source must be native or destinaton must be fractional.\n", __func__, curTransfer.ToUniValue().write().c_str());
                        LogPrintf("%s: Invalid conversion %s. Source must be native or destinaton must be fractional\n", __func__, curTransfer.ToUniValue().write().c_str());
                        return false;
                    }

                    // get currency index
                    auto curIndexIt = currencyIndexMap.find(curTransfer.currencyID);
                    if (curIndexIt == currencyIndexMap.end())
                    {
                        printf("%s: Invalid currency for conversion %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                        LogPrintf("%s: Invalid currency for conversion %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                        return false;
                    }
                    int curIdx = curIndexIt->second;

                    // output the converted amount, minus fees, and generate a normal output that spends the net input of the import as native
                    // difference between all potential value out and what was taken unconverted as a fee in our fee output
                    CAmount preConversionFee = 0;
                    CAmount newCurrencyConverted = 0;
                    CAmount valueOut = curTransfer.nValue;

                    preConversionFee = CalculateConversionFee(curTransfer.nValue);
                    if (preConversionFee > curTransfer.nValue)
                    {
                        preConversionFee = curTransfer.nValue;
                    }

                    valueOut -= preConversionFee;

                    AddReserveConversionFees(curTransfer.currencyID, preConversionFee);

                    // convert fee to native currency according to the initial currency prices, where that was calculated,
                    // and add it to the fees already converted, so it won't affect later prices
                    if (curTransfer.currencyID != systemDestID)
                    {
                        CAmount intermediate = initialCurrencyState.ReserveToNativeRaw(preConversionFee, initialCurrencyState.conversionPrice[curIdx]);
                        if (systemDestID == importCurrencyID)
                        {
                            preConversionFee = intermediate;
                            AddNativeOutConverted(curTransfer.currencyID, preConversionFee);
                            nativeIn += preConversionFee;
                            preConvertedOutput.valueMap[curTransfer.currencyID] += preConversionFee;
                        }
                        else
                        {
                            preConversionFee = initialCurrencyState.NativeToReserveRaw(intermediate, initialCurrencyState.viaConversionPrice[systemDestIdx]);
                            nativeIn += preConversionFee;
                            AddReserveOutConverted(systemDestID, preConversionFee);
                            preConvertedReserves.valueMap[systemDestID] += preConversionFee;
                        }
                    }

                    // preConversionFee is always native when we get here
                    transferFees.valueMap[systemDestID] += preConversionFee;

                    newCurrencyConverted = initialCurrencyState.ReserveToNativeRaw(valueOut, initialCurrencyState.conversionPrice[curIdx]);

                    if (!carveOutSet)
                    {
                        totalCarveOut = importCurrencyDef.GetTotalCarveOut();
                        carveOutSet = true;
                    }
                    if (totalCarveOut > 0 && totalCarveOut < SATOSHIDEN)
                    {
                        CAmount newReserveIn = CCurrencyState::NativeToReserveRaw(valueOut, SATOSHIDEN - totalCarveOut);
                        totalCarveOuts.valueMap[curTransfer.currencyID] += valueOut - newReserveIn;
                        valueOut = newReserveIn;
                    }

                    if (curTransfer.currencyID != systemDestID)
                    {
                        // if this is a fractional currency, everything but fees and carveouts stay in reserve deposit
                        // else all that would be reserves is sent to chain ID
                        if (!isFractional)
                        {
                            AddReserveOutput(curTransfer.currencyID, valueOut);
                            std::vector<CTxDestination> dests({CIdentityID(importCurrencyID)});
                            CTokenOutput ro = CTokenOutput(curTransfer.currencyID, valueOut);
                            vOutputs.push_back(CTxOut(0, MakeMofNCCScript(CConditionObj<CTokenOutput>(EVAL_RESERVE_OUTPUT, dests, 1, &ro))));
                        }
                    }
                    else
                    {
                        // if it is not fractional, send to currency ID, else leave it in reserve deposit
                        if (!isFractional)
                        {
                            nativeOut += valueOut;
                            vOutputs.push_back(CTxOut(valueOut, GetScriptForDestination(CIdentityID(importCurrencyID))));
                        }
                    }

                    if (newCurrencyConverted)
                    {
                        preConvertedOutput.valueMap[curTransfer.currencyID] += newCurrencyConverted;
                        AddNativeOutConverted(curTransfer.currencyID, newCurrencyConverted);
                        AddNativeOutConverted(curTransfer.destCurrencyID, newCurrencyConverted);
                        if (curTransfer.destCurrencyID == systemDestID)
                        {
                            nativeOut += newCurrencyConverted;
                            nativeIn += newCurrencyConverted;
                            newOut = CTxOut(newCurrencyConverted, GetScriptForDestination(TransferDestinationToDestination(curTransfer.destination)));
                        }
                        else
                        {
                            AddReserveOutConverted(curTransfer.destCurrencyID, newCurrencyConverted);
                            AddReserveOutput(curTransfer.destCurrencyID, newCurrencyConverted);
                            AddReserveInput(curTransfer.destCurrencyID, newCurrencyConverted);
                            std::vector<CTxDestination> dests = std::vector<CTxDestination>({TransferDestinationToDestination(curTransfer.destination)});
                            CTokenOutput ro = CTokenOutput(curTransfer.destCurrencyID, newCurrencyConverted);
                            newOut = CTxOut(0, MakeMofNCCScript(CConditionObj<CTokenOutput>(EVAL_RESERVE_OUTPUT, dests, 1, &ro)));
                        }
                    }
                }
                else if (curTransfer.IsConversion())
                {
                    if (curTransfer.currencyID == curTransfer.destCurrencyID)
                    {
                        printf("%s: Conversion does not specify two currencies\n", __func__);
                        LogPrintf("%s: Conversion does not specify two currencies\n", __func__);
                        return false;
                    }

                    // either the source or destination must be a reserve currency of the other fractional currency
                    // if destination is a fractional currency of a reserve, we will mint currency
                    // if not, we will burn currency
                    bool toFractional = importCurrencyID == curTransfer.destCurrencyID &&
                                        currencyDest.IsFractional() && 
                                        currencyIndexMap.count(curTransfer.currencyID);

                    CCurrencyDefinition sourceCurrency = ConnectedChains.GetCachedCurrency(curTransfer.currencyID);

                    if (!sourceCurrency.IsValid())
                    {
                        printf("%s: Currency specified for conversion not found\n", __func__);
                        LogPrintf("%s: Currency specified for conversion not found\n", __func__);
                        return false;
                    }

                    if (!(toFractional || 
                        (importCurrencyID == curTransfer.currencyID &&
                         sourceCurrency.IsFractional() &&
                         currencyIndexMap.count(curTransfer.destCurrencyID))))
                    {
                        printf("%s: Conversion must be between a fractional currency and one of its reserves\n", __func__);
                        LogPrintf("%s: Conversion must be between a fractional currency and one of its reserves\n", __func__);
                        return false;
                    }

                    if (curTransfer.IsReserveToReserve() &&
                        (!toFractional ||
                         curTransfer.secondReserveID.IsNull() ||
                         curTransfer.secondReserveID == curTransfer.currencyID ||
                         !currencyIndexMap.count(curTransfer.secondReserveID)))
                    {
                        printf("%s: Invalid reserve to reserve transaction %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                        LogPrintf("%s: Invalid reserve to reserve transaction %s\n", __func__, curTransfer.ToUniValue().write().c_str());
                        return false;
                    }

                    const CCurrencyDefinition &fractionalCurrency = toFractional ? currencyDest : sourceCurrency;
                    const CCurrencyDefinition &reserveCurrency = toFractional ? sourceCurrency : currencyDest;
                    int reserveIdx = currencyIndexMap[reserveCurrency.GetID()];

                    assert(fractionalCurrency.IsValid() && 
                           reserveCurrency.IsValid() && 
                           fractionalCurrency.currencies[reserveIdx] == reserveCurrency.GetID());

                    // now, we know that we are converting from the source currency to the
                    // destination currency and also that one of them is a reserve of the other
                    // we convert using the provided currency state, and we update the currency 
                    // state to include newly minted or burned currencies.
                    CAmount valueOut = curTransfer.nValue;
                    CAmount preConversionFee = 0;
                    CAmount newCurrencyConverted = 0;

                    if (!(curTransfer.flags & curTransfer.FEE_OUTPUT))
                    {
                        preConversionFee = CalculateConversionFee(curTransfer.nValue);
                        if (curTransfer.IsReserveToReserve())
                        {
                            preConversionFee <<= 1;
                        }
                        if (preConversionFee > curTransfer.nValue)
                        {
                            preConversionFee = curTransfer.nValue;
                        }

                        AddReserveConversionFees(curTransfer.currencyID, preConversionFee);
                        transferFees.valueMap[curTransfer.currencyID] += preConversionFee;

                        valueOut -= preConversionFee;
                    }

                    if (toFractional)
                    {
                        reserveConverted.valueMap[curTransfer.currencyID] += valueOut;
                        newCurrencyConverted = importCurrencyState.ReserveToNativeRaw(valueOut, importCurrencyState.conversionPrice[reserveIdx]);
                    }
                    else
                    {
                        fractionalConverted.valueMap[curTransfer.destCurrencyID] += valueOut;
                        newCurrencyConverted = importCurrencyState.NativeToReserveRaw(valueOut, importCurrencyState.conversionPrice[reserveIdx]);
                    }

                    if (newCurrencyConverted)
                    {
                        uint160 outputCurrencyID;

                        if (curTransfer.IsReserveToReserve())
                        {
                            // we need to convert once more from fractional to a reserve currency
                            // we burn 0.025% of the fractional that was converted, and convert the rest to
                            // the specified reserve. since the burn depends on the first conversion, which
                            // it is not involved in, it is tracked separately and applied after the first conversion
                            int32_t outputCurrencIdx = currencyIndexMap[outputCurrencyID];
                            outputCurrencyID = curTransfer.secondReserveID;
                            newCurrencyConverted = CCurrencyState::NativeToReserveRaw(newCurrencyConverted, importCurrencyState.viaConversionPrice[outputCurrencIdx]);
                            crossConversions[reserveIdx][outputCurrencIdx] += valueOut;
                        }
                        else
                        {
                            outputCurrencyID = curTransfer.destCurrencyID;
                        }

                        if (toFractional && !curTransfer.IsReserveToReserve())
                        {
                            AddNativeOutConverted(curTransfer.currencyID, newCurrencyConverted);
                            AddNativeOutConverted(curTransfer.destCurrencyID, newCurrencyConverted);
                            if (curTransfer.destCurrencyID == systemDestID)
                            {
                                nativeOut += newCurrencyConverted;
                                nativeIn += newCurrencyConverted;
                            }
                            else
                            {
                                AddReserveOutConverted(curTransfer.destCurrencyID, newCurrencyConverted);
                                AddReserveInput(curTransfer.destCurrencyID, newCurrencyConverted);
                                AddReserveOutput(curTransfer.destCurrencyID, newCurrencyConverted);
                            }
                        }
                        else
                        {
                            AddReserveOutConverted(outputCurrencyID, newCurrencyConverted);
                            if (outputCurrencyID == systemDestID)
                            {
                                nativeOut += newCurrencyConverted;
                            }
                            else
                            {
                                AddReserveOutput(outputCurrencyID, newCurrencyConverted);
                            }

                            // if this originated as input fractional, burn the input currency
                            // if it was reserve to reserve, it was never added, and it's fee 
                            // value is left behind in the currency
                            if (!curTransfer.IsReserveToReserve())
                            {
                                AddNativeOutConverted(curTransfer.currencyID, -valueOut);
                            }
                        }

                        if (outputCurrencyID == systemDestID)
                        {
                            newOut = CTxOut(newCurrencyConverted, GetScriptForDestination(TransferDestinationToDestination(curTransfer.destination)));
                        }
                        else
                        {
                            std::vector<CTxDestination> dests = std::vector<CTxDestination>({TransferDestinationToDestination(curTransfer.destination)});
                            CTokenOutput ro = CTokenOutput(outputCurrencyID, newCurrencyConverted);
                            newOut = CTxOut(0, MakeMofNCCScript(CConditionObj<CTokenOutput>(EVAL_RESERVE_OUTPUT, dests, 1, &ro)));
                        }
                    }
                }
                else if ((curTransfer.flags & curTransfer.SEND_BACK) && 
                         curTransfer.nValue >= (curTransfer.DEFAULT_PER_STEP_FEE << 2) && 
                         curTransfer.currencyID == nativeSourceCurrencyID)
                {
                    // emit a reserve exchange output
                    cp = CCinit(&CC, EVAL_RESERVE_TRANSFER);
                    CPubKey pk = CPubKey(ParseHex(CC.CChexstr));

                    // transfer it back to the source chain and to our address
                    std::vector<CTxDestination> dests = std::vector<CTxDestination>({pk.GetID(), TransferDestinationToDestination(curTransfer.destination)});
                    
                    // naturally compress a full ID to an ID destination, since it is going back where it came from
                    CTxDestination sendBackAddr = TransferDestinationToDestination(curTransfer.destination);

                    CAmount fees = curTransfer.CalculateTransferFee();

                    CReserveTransfer rt = CReserveTransfer(CReserveExchange::VALID, 
                                                           curTransfer.currencyID, 
                                                           curTransfer.nValue - fees, 
                                                           fees, 
                                                           curTransfer.currencyID,
                                                           DestinationToTransferDestination(sendBackAddr));

                    CScript sendBackScript = MakeMofNCCScript(CConditionObj<CReserveTransfer>(EVAL_RESERVE_TRANSFER, dests, 1, &rt));

                    // if this is sending back to the same chain as its native currency, make it a native output
                    if (systemDestID == nativeSourceCurrencyID)
                    {
                        nativeOut += curTransfer.nValue;
                        newOut = CTxOut(curTransfer.nValue, sendBackScript);                    
                    }
                    else
                    {
                        AddReserveOutput(curTransfer.currencyID, curTransfer.nValue);
                        newOut = CTxOut(0, sendBackScript);                    
                    }
                }
                else
                {
                    // if we are supposed to burn a currency, it must be the import currency, and it
                    // is removed from the supply, which would change all calculations for price
                    if (curTransfer.IsBurn())
                    {
                        // if the source is fractional currency, it is burned
                        if (curTransfer.currencyID != importCurrencyID || !(isFractional || importCurrencyDef.IsToken()))
                        {
                            CCurrencyDefinition sourceCurrency = ConnectedChains.GetCachedCurrency(curTransfer.currencyID);
                            printf("%s: Attempting to burn %s, which is either not a token or fractional currency or not the import currency %s\n", __func__, sourceCurrency.name.c_str(), importCurrencyDef.name.c_str());
                            LogPrintf("%s: Attempting to burn %s, which is either not a token or fractional currency or not the import currency %s\n", __func__, sourceCurrency.name.c_str(), importCurrencyDef.name.c_str());
                            return false;
                        }
                        if (curTransfer.flags & curTransfer.IsBurnChangeWeight())
                        {
                            printf("%s: burning %s to change weight is not supported\n", __func__, importCurrencyDef.name.c_str());
                            LogPrintf("%s: burning %s to change weight is not supported\n", __func__, importCurrencyDef.name.c_str());
                            return false;
                        }
                        // burn the input fractional currency
                        AddNativeOutConverted(curTransfer.currencyID, -curTransfer.nValue);
                        burnedChangePrice += curTransfer.nValue;
                    }
                    else if (systemDestID == curTransfer.destCurrencyID)
                    {
                        nativeOut += curTransfer.nValue;
                        newOut = CTxOut(curTransfer.nValue, GetScriptForDestination(TransferDestinationToDestination(curTransfer.destination)));
                    }
                    else
                    {
                        // generate a reserve output of the amount indicated, less fees
                        // we will send using a reserve output, fee will be paid through coinbase by converting from reserve or not, depending on currency settings
                        std::vector<CTxDestination> dests = std::vector<CTxDestination>({TransferDestinationToDestination(curTransfer.destination)});
                        CTokenOutput ro = CTokenOutput(curTransfer.destCurrencyID, curTransfer.nValue);

                        // if this is a minting of currency
                        // this is used for both pre-allocation and also centrally, algorithmically, or externally controlled currencies
                        if ((curTransfer.IsMint() || curTransfer.IsPreallocate()) && curTransfer.destCurrencyID == importCurrencyID)
                        {
                            // pre-allocation is accounted for outside of this
                            // minting is emitted in new currency state
                            if (curTransfer.IsMint())
                            {
                                totalMinted += ro.nValue;
                            }
                            AddNativeOutConverted(curTransfer.destCurrencyID, ro.nValue);
                            if (curTransfer.destCurrencyID != systemDestID)
                            {
                                AddReserveOutConverted(curTransfer.destCurrencyID, ro.nValue);
                            }
                        }
                        AddReserveOutput(curTransfer.destCurrencyID, ro.nValue);
                        newOut = CTxOut(0, MakeMofNCCScript(CConditionObj<CTokenOutput>(EVAL_RESERVE_OUTPUT, dests, 1, &ro)));
                    }
                }
                if (newOut.nValue < 0)
                {
                    // if we get here, we have absorbed the entire transfer
                    LogPrintf("%s: skip creating output for import to %s\n", __func__, currencyDest.name.c_str());
                }
                else
                {
                    vOutputs.push_back(newOut);
                }
            }
            else
            {
                printf("%s: Invalid reserve transfer on export\n", __func__);
                LogPrintf("%s: Invalid reserve transfer on export\n", __func__);
                return false;
            }
        }
    }

    if ((totalCarveOuts = totalCarveOuts.CanonicalMap()).valueMap.size())
    {
        // add carveout outputs
        for (auto &oneCur : totalCarveOuts.valueMap)
        {
            // if we are creating a reserve import for native currency, it must be spent from native inputs on the destination system
            if (oneCur.first == systemDestID)
            {
                nativeOut += oneCur.second;
                vOutputs.push_back(CTxOut(oneCur.second, GetScriptForDestination(CIdentityID(importCurrencyID))));
            }
            else
            {
                // generate a reserve output of the amount indicated, less fees
                // we will send using a reserve output, fee will be paid through coinbase by converting from reserve or not, depending on currency settings
                std::vector<CTxDestination> dests = std::vector<CTxDestination>({CIdentityID(importCurrencyID)});
                CTokenOutput ro = CTokenOutput(oneCur.first, oneCur.second);
                AddReserveOutput(oneCur.first, oneCur.second);
                vOutputs.push_back(CTxOut(0, MakeMofNCCScript(CConditionObj<CTokenOutput>(EVAL_RESERVE_OUTPUT, dests, 1, &ro))));
            }
        }
    }

    // remove burned currency from supply
    if (burnedChangePrice > 0)
    {
        if (!(burnedChangePrice <= newCurrencyState.supply))
        {
            printf("%s: Invalid burn amount %ld\n", __func__, burnedChangePrice);
            LogPrintf("%s: Invalid burn amount %ld\n", __func__, burnedChangePrice);
            return false;
        }
        newCurrencyState.supply -= burnedChangePrice;
    }

    if (isFractional && (reserveConverted.CanonicalMap().valueMap.size() || fractionalConverted.CanonicalMap().valueMap.size()))
    {
        CCurrencyState dummyCurState;
        newCurrencyState.conversionPrice = 
            importCurrencyState.ConvertAmounts(reserveConverted.AsCurrencyVector(importCurrencyState.currencies),
                                               fractionalConverted.AsCurrencyVector(importCurrencyState.currencies),
                                               dummyCurState,
                                               &crossConversions,
                                               &newCurrencyState.viaConversionPrice);
    }

    std::vector<CAmount> vResConverted = reserveConverted.AsCurrencyVector(newCurrencyState.currencies);
    std::vector<CAmount> vResOutConverted = (ReserveOutConvertedMap(importCurrencyID) - preConvertedReserves).AsCurrencyVector(newCurrencyState.currencies);
    std::vector<CAmount> vFracConverted = fractionalConverted.AsCurrencyVector(newCurrencyState.currencies);
    std::vector<CAmount> vFracOutConverted = (NativeOutConvertedMap() - preConvertedOutput).AsCurrencyVector(newCurrencyState.currencies);

    for (int i = 0; i < newCurrencyState.currencies.size(); i++)
    {
        newCurrencyState.reserveIn[i] = vResConverted[i];
        newCurrencyState.reserveOut[i] = vResOutConverted[i];
        newCurrencyState.reserves[i] += vResConverted[i] - vResOutConverted[i];
        newCurrencyState.nativeIn[i] = vFracConverted[i];
        newCurrencyState.supply += vFracOutConverted[i];
    }

    if (totalMinted)
    {
        newCurrencyState.UpdateWithEmission(totalMinted);
    }

    // now, pull out all fractional data and sort out native vs. fractional
    if (currencies.count(systemDestID))
    {
        CReserveInOuts fractionalInOuts = currencies[systemDestID];
        newCurrencyState.nativeConversionFees = fractionalInOuts.reserveConversionFees;
        newCurrencyState.nativeFees = (transferFees.valueMap[systemDestID] + newCurrencyState.nativeConversionFees);
    }
    newCurrencyState.conversionFees = ReserveConversionFeesMap().AsCurrencyVector(newCurrencyState.currencies);
    newCurrencyState.fees = transferFees.AsCurrencyVector(newCurrencyState.currencies);

    // double check that the export fee taken as the fee output matches the export fee that should have been taken
    CCurrencyValueMap ReserveInputs;
    CCurrencyValueMap ReserveOutputs;
    CAmount systemOutConverted = 0;
    for (auto &oneInOut : currencies)
    {
        if (oneInOut.first == importCurrencyID)
        {
            if (oneInOut.first == systemDestID)
            {
                systemOutConverted += oneInOut.second.nativeOutConverted;
            }
        }
        else
        {
            ReserveInputs.valueMap[importCurrencyID] += oneInOut.second.nativeOutConverted;
            if (oneInOut.first == systemDestID)
            {
                systemOutConverted += oneInOut.second.reserveOutConverted;
            }
            if (oneInOut.second.reserveIn || oneInOut.second.reserveOutConverted)
            {
                ReserveInputs.valueMap[oneInOut.first] = oneInOut.second.reserveIn + oneInOut.second.reserveOutConverted;
            }
            if (oneInOut.second.reserveOut)
            {
                ReserveOutputs.valueMap[oneInOut.first] = oneInOut.second.reserveOut;
            }
        }
    }
    if (systemOutConverted)
    {
        // this does not have meaning besides a store of the system currency output that was converted
        currencies[importCurrencyID].reserveOutConverted = systemOutConverted;
    }
    if (nativeIn || systemOutConverted)
    {
        ReserveInputs.valueMap[importCurrencyDef.systemID] = nativeIn + systemOutConverted;
    }
    if (nativeOut)
    {
        ReserveOutputs.valueMap[importCurrencyDef.systemID] = nativeOut;
    }

    //printf("ReserveInputs: %s\nReserveOutputs: %s\nReserveInputs - ReserveOutputs: %s\n", ReserveInputs.ToUniValue().write(1,2).c_str(), ReserveOutputs.ToUniValue().write(1,2).c_str(), (ReserveInputs - ReserveOutputs).ToUniValue().write(1,2).c_str());
    if ((ReserveInputs - ReserveOutputs).HasNegative())
    {
        printf("%s: Too much fee taken by export, ReserveInputs: %s\nReserveOutputs: %s\n", __func__,
                ReserveInputs.ToUniValue().write(1,2).c_str(), 
                ReserveOutputs.ToUniValue().write(1,2).c_str());
        LogPrintf("%s: Too much fee taken by export\n", __func__);
        return false;
    }
    return true;
}

CMutableTransaction &CReserveTransactionDescriptor::AddConversionInOuts(CMutableTransaction &conversionTx, std::vector<CInputDescriptor> &conversionInputs, const CCurrencyValueMap &_exchangeRates, const CCurrencyState *pCurrencyState) const
{
    if (!IsReserveExchange() || IsFillOrKillFail())
    {
        return conversionTx;
    }

    bool noExchangeRate = false;
    CCurrencyState dummy;
    const CCurrencyState &currencyState = pCurrencyState ? *pCurrencyState : dummy;

    // set exchange rates as well as we can, either from explicit rates or currency state if possible
    CCurrencyValueMap __exchangeRates;
    const CCurrencyValueMap *pExchangeRates = &__exchangeRates;
    if (_exchangeRates.valueMap.size() != 0)
    {
        pExchangeRates = &_exchangeRates;
    }
    else
    {
        if (pCurrencyState && currencyState.IsFractional())
        {
            __exchangeRates = CCurrencyValueMap(currencyState.currencies, currencyState.PricesInReserve());
        }
        else
        {
            noExchangeRate = true;
        }
    }
    const CCurrencyValueMap &exchangeRates = *pExchangeRates;

    CAmount nativeFeesLeft = nativeConversionFees;
    for (auto &oneEntry : exchangeRates.valueMap)
    {
        auto it = currencies.find(oneEntry.first);
        if (it == currencies.end())
        {
            LogPrintf("%s: invalid conversion with no exchange rate, currency: %s\n", __func__, EncodeDestination(CIdentityID(oneEntry.first)).c_str());
        }
        else
        {
            nativeFeesLeft += currencyState.ReserveToNativeRaw(it->second.reserveConversionFees, oneEntry.second);
        }
    }

    uint256 txHash = ptx->GetHash();

    for (auto &indexRex : vRex)
    {
        COptCCParams p;
        ptx->vout[indexRex.first].scriptPubKey.IsPayToCryptoCondition(p);

        CCcontract_info CC;
        CCcontract_info *cp;

        CAmount fee = CalculateConversionFee(indexRex.second.nValue);
        CAmount amount = indexRex.second.nValue - fee;
        CAmount nativeFee, nativeAmount;

        auto rateIt = exchangeRates.valueMap.find(indexRex.second.currencyID);
        if (rateIt == exchangeRates.valueMap.end())
        {
            continue;
        }

        CAmount exchangeRate = rateIt->second;

        // if already native fees, don't convert, otherwise, do
        if (!(indexRex.second.flags & indexRex.second.TO_RESERVE))
        {
            nativeFee = fee;
            nativeAmount = ptx->vout[indexRex.first].nValue;
            amount = currencyState.NativeToReserveRaw(nativeAmount, exchangeRate);
            fee = currencyState.NativeToReserveRaw(nativeFee, exchangeRate);
        }
        else
        {
            nativeFee = currencyState.ReserveToNativeRaw(fee, exchangeRate);
            nativeAmount = currencyState.ReserveToNativeRaw(amount, exchangeRate);
        }

        if (nativeFee > nativeFeesLeft)
        {
            nativeFee = nativeFeesLeft;
        }
        nativeFeesLeft -= nativeFee;

        // add input...
        conversionTx.vin.push_back(CTxIn(txHash, indexRex.first, CScript()));

        // ... and input descriptor. we leave the CTxIn empty and use the one in the corresponding input, using the input descriptor for only
        // script and value
        conversionInputs.push_back(CInputDescriptor(ptx->vout[indexRex.first].scriptPubKey, ptx->vout[indexRex.first].nValue, CTxIn()));

        // if we should emit a reserve transfer or normal reserve output, sending output only occurs when converting from
        // native to a reserve currency
        if (indexRex.second.flags & indexRex.second.SEND_OUTPUT && 
            indexRex.second.flags & indexRex.second.TO_RESERVE &&
            nativeAmount > (CReserveTransfer::DEFAULT_PER_STEP_FEE << 1) << 1)
        {
            cp = CCinit(&CC, EVAL_RESERVE_TRANSFER);
            CPubKey pk = CPubKey(ParseHex(CC.CChexstr));

            // send the entire amount to a reserve transfer output of the controller
            const auto &reserveCurrencyIt = ConnectedChains.ReserveCurrencies().find(indexRex.second.currencyID);
            if (reserveCurrencyIt != ConnectedChains.ReserveCurrencies().end())
            {
                std::vector<CTxDestination> dests = std::vector<CTxDestination>({pk.GetID(), p.vKeys[0]});

                // create the transfer output with the converted amount less fees
                CReserveTransfer rt((uint32_t)CReserveTransfer::VALID,
                                    indexRex.second.currencyID,
                                    amount - (CReserveTransfer::DEFAULT_PER_STEP_FEE << 1),
                                    CReserveTransfer::DEFAULT_PER_STEP_FEE << 1,
                                    reserveCurrencyIt->second.systemID,
                                    CTransferDestination(p.vKeys[0].which(), GetDestinationBytes(p.vKeys[0])));

                // cast object to the most derived class to avoid template errors to a least derived class
                CTxDestination rtIndexDest(CKeyID(ConnectedChains.ThisChain().GetConditionID(EVAL_RESERVE_TRANSFER)));
                conversionTx.vout.push_back(CTxOut(0, MakeMofNCCScript(CConditionObj<CReserveTransfer>(EVAL_RESERVE_TRANSFER, dests, 1, &rt))));
            }
        }
        else if (indexRex.second.flags & indexRex.second.TO_RESERVE)
        {
            // send the net amount to the indicated destination, which is the first entry in the destinations of the original reserve/exchange by protocol
            std::vector<CTxDestination> dests = std::vector<CTxDestination>({p.vKeys[0]});

            // create the output with the unconverted amount less fees
            CTokenOutput ro(indexRex.second.currencyID, amount);

            conversionTx.vout.push_back(MakeCC1ofAnyVout(EVAL_RESERVE_OUTPUT, 0, dests, ro));
        }
        else
        {
            // convert amount to native from reserve and send as normal output
            amount = currencyState.ReserveToNative(amount, exchangeRates.valueMap.find(indexRex.second.currencyID)->second);
            conversionTx.vout.push_back(CTxOut(amount, GetScriptForDestination(p.vKeys[0])));
        }
    }
    return conversionTx;
}

CCurrencyValueMap CReserveTransactionDescriptor::ReserveInputMap(const uint160 &nativeID) const
{
    CCurrencyValueMap retVal;
    uint160 id = nativeID.IsNull() ? ASSETCHAINS_CHAINID : nativeID;
    for (auto &oneInOut : currencies)
    {
        // skip native
        if (oneInOut.first != id)
        {
            if (oneInOut.second.reserveIn)
            {
                retVal.valueMap[oneInOut.first] = oneInOut.second.reserveIn;
            }
        }
    }
    return retVal;
}

CCurrencyValueMap CReserveTransactionDescriptor::ReserveOutputMap(const uint160 &nativeID) const
{
    CCurrencyValueMap retVal;
    uint160 id = nativeID.IsNull() ? ASSETCHAINS_CHAINID : nativeID;
    for (auto &oneInOut : currencies)
    {
        // skip native
        if (oneInOut.first != id)
        {
            if (oneInOut.second.reserveOut)
            {
                retVal.valueMap[oneInOut.first] = oneInOut.second.reserveOut;
            }
        }
    }
    return retVal;
}

CCurrencyValueMap CReserveTransactionDescriptor::ReserveOutConvertedMap(const uint160 &nativeID) const
{
    CCurrencyValueMap retVal;
    uint160 id = nativeID.IsNull() ? ASSETCHAINS_CHAINID : nativeID;
    for (auto &oneInOut : currencies)
    {
        // skip native
        if (oneInOut.first != id)
        {
            if (oneInOut.second.reserveOutConverted)
            {
                retVal.valueMap[oneInOut.first] = oneInOut.second.reserveOutConverted;
            }
        }
    }
    return retVal;
}

CCurrencyValueMap CReserveTransactionDescriptor::NativeOutConvertedMap() const
{
    CCurrencyValueMap retVal;
    for (auto &oneInOut : currencies)
    {
        if (oneInOut.second.nativeOutConverted)
        {
            retVal.valueMap[oneInOut.first] = oneInOut.second.nativeOutConverted;
        }
    }
    return retVal;
}

CCurrencyValueMap CReserveTransactionDescriptor::ReserveConversionFeesMap() const
{
    CCurrencyValueMap retVal;
    for (auto &oneInOut : currencies)
    {
        if (oneInOut.second.reserveConversionFees)
        {
            retVal.valueMap[oneInOut.first] = oneInOut.second.reserveConversionFees;
        }
    }
    return retVal;
}

std::vector<CAmount> CReserveTransactionDescriptor::ReserveInputVec(const CCurrencyState &cState) const
{
    std::vector<CAmount> retVal(cState.currencies.size());
    std::map<uint160, int> curMap = cState.GetReserveMap();
    for (auto &oneInOut : currencies)
    {
        retVal[curMap[oneInOut.first]] = oneInOut.second.reserveIn;
    }
    return retVal;
}

std::vector<CAmount> CReserveTransactionDescriptor::ReserveOutputVec(const CCurrencyState &cState) const
{
    std::vector<CAmount> retVal(cState.currencies.size());
    std::map<uint160, int> curMap = cState.GetReserveMap();
    for (auto &oneInOut : currencies)
    {
        retVal[curMap[oneInOut.first]] = oneInOut.second.reserveOut;
    }
    return retVal;
}

std::vector<CAmount> CReserveTransactionDescriptor::ReserveOutConvertedVec(const CCurrencyState &cState) const
{
    std::vector<CAmount> retVal(cState.currencies.size());
    std::map<uint160, int> curMap = cState.GetReserveMap();
    for (auto &oneInOut : currencies)
    {
        retVal[curMap[oneInOut.first]] = oneInOut.second.reserveOutConverted;
    }
    return retVal;
}

std::vector<CAmount> CReserveTransactionDescriptor::NativeOutConvertedVec(const CCurrencyState &cState) const
{
    std::vector<CAmount> retVal(cState.currencies.size());
    std::map<uint160, int> curMap = cState.GetReserveMap();
    for (auto &oneInOut : currencies)
    {
        retVal[curMap[oneInOut.first]] = oneInOut.second.nativeOutConverted;
    }
    return retVal;
}

std::vector<CAmount> CReserveTransactionDescriptor::ReserveConversionFeesVec(const CCurrencyState &cState) const
{
    std::vector<CAmount> retVal(cState.currencies.size());
    std::map<uint160, int> curMap = cState.GetReserveMap();
    for (auto &oneInOut : currencies)
    {
        retVal[curMap[oneInOut.first]] = oneInOut.second.reserveConversionFees;
    }
    return retVal;
}

// this should be done no more than once to prepare a currency state to be updated to the next state
// emission occurs for a block before any conversion or exchange and that impact on the currency state is calculated
CCurrencyState &CCurrencyState::UpdateWithEmission(CAmount toEmit)
{
    initialSupply = supply;
    emitted = 0;

    // if supply is 0, reserve must be zero, and we cannot function as a reserve currency
    if (!IsFractional() || supply <= 0 || CCurrencyValueMap(currencies, reserves) <= CCurrencyValueMap())
    {
        if (supply < 0)
        {
            emitted = supply = toEmit;
        }
        else
        {
            emitted = toEmit;
            supply += toEmit;
        }
        return *this;
    }

    if (toEmit)
    {
        // first determine current ratio by adding up all currency weights
        CAmount InitialRatio = 0;
        for (auto weight : weights)
        {
            InitialRatio += weight;
        }

        // to balance rounding with truncation, we statistically add a satoshi to the initial ratio
        static arith_uint256 bigSatoshi(SATOSHIDEN);
        arith_uint256 bigInitial(InitialRatio);
        arith_uint256 bigEmission(toEmit);
        arith_uint256 bigSupply(supply);

        arith_uint256 bigScratch = (bigInitial * bigSupply * bigSatoshi) / (bigSupply + bigEmission);
        arith_uint256 bigRatio = bigScratch / bigSatoshi;
        // cap ratio at 1
        if (bigRatio >= bigSatoshi)
        {
            bigScratch = arith_uint256(SATOSHIDEN) * arith_uint256(SATOSHIDEN);
            bigRatio = bigSatoshi;
        }

        int64_t newRatio = bigRatio.GetLow64();
        int64_t remainder = (bigScratch - (bigRatio * SATOSHIDEN)).GetLow64();
        // form of bankers rounding, if odd, round up at half, if even, round down at half
        if (remainder > (SATOSHIDEN >> 1) || (remainder == (SATOSHIDEN >> 1) && newRatio & 1))
        {
            newRatio += 1;
        }

        // now, we must update all weights accordingly, based on the new, total ratio, by dividing the total among all the
        // weights, according to their current relative weight. because this also can be a source of rounding error, we will
        // distribute any modulus excess randomly among the currencies
        std::vector<CAmount> extraWeight(currencies.size());
        arith_uint256 bigRatioDelta(InitialRatio - newRatio);
        CAmount totalUpdates = 0;

        for (auto &weight : weights)
        {
            CAmount weightDelta = (bigRatioDelta * arith_uint256(weight) / bigSatoshi).GetLow64();
            weight -= weightDelta;
            totalUpdates += weightDelta;
        }

        CAmount updateExtra = (InitialRatio - newRatio) - totalUpdates;

        // if we have any extra, distribute it evenly and any mod, both deterministically and pseudorandomly
        if (updateExtra)
        {
            CAmount forAll = updateExtra / currencies.size();
            CAmount forSome = updateExtra % currencies.size();

            // get deterministic seed for linear congruential pseudorandom number for shuffle
            int64_t seed = supply + forAll + forSome;
            auto prandom = std::minstd_rand0(seed);

            for (int i = 0; i < extraWeight.size(); i++)
            {
                extraWeight[i] = forAll;
                if (forSome)
                {
                    extraWeight[i]++;
                    forSome--;
                }
            }
            // distribute the extra as evenly as possible
            std::shuffle(extraWeight.begin(), extraWeight.end(), prandom);
            for (int i = 0; i < weights.size(); i++)
            {
                weights[i] -= extraWeight[i];
            }
        }

        // update initial supply from what we currently have
        emitted = toEmit;
        supply = initialSupply + emitted;
    }
    return *this; 
}

// From a vector of transaction pointers, match all that are valid orders and can be matched,
// put them into a vector of reserve transaction descriptors, and return a new fractional reserve state,
// if pConversionTx is present, this will add one output to it for each transaction included
// and consider its size wen comparing to maxSerializeSize
CCoinbaseCurrencyState CCoinbaseCurrencyState::MatchOrders(const std::vector<CReserveTransactionDescriptor> &orders, 
                                                           std::vector<CReserveTransactionDescriptor> &reserveFills, 
                                                           std::vector<CReserveTransactionDescriptor> &noFills, 
                                                           std::vector<const CReserveTransactionDescriptor *> &expiredFillOrKills, 
                                                           std::vector<const CReserveTransactionDescriptor *> &rejects, 
                                                           std::vector<CAmount> &prices,
                                                           int32_t height, std::vector<CInputDescriptor> &conversionInputs,
                                                           int64_t maxSerializeSize, int64_t *pInOutTotalSerializeSize, CMutableTransaction *pConversionTx,
                                                           bool feesAsReserve) const
{
    // synthetic order book of limitBuys and limitSells sorted by limit, order of preference beyond limit sorting is random
    std::map<uint160, std::multimap<CAmount, CReserveTransactionDescriptor>> limitBuys;
    std::map<uint160, std::multimap<CAmount, CReserveTransactionDescriptor>> limitSells;        // limit orders are prioritized by limit
    std::multimap<CAmount, CReserveTransactionDescriptor> marketOrders;      // prioritized by fee rate
    CAmount totalNativeConversionFees = 0;
    CCurrencyValueMap totalReserveConversionFees;
    std::vector<CAmount> exchangeRates(PricesInReserve());
    CCurrencyValueMap exchangeRateMap = CCurrencyValueMap(currencies, exchangeRates);

    CMutableTransaction mConversionTx = pConversionTx ? *pConversionTx : CMutableTransaction();
    std::vector<CInputDescriptor> tmpConversionInputs(conversionInputs);

    int64_t totalSerializedSize = pInOutTotalSerializeSize ? *pInOutTotalSerializeSize + CCurrencyState::CONVERSION_TX_SIZE_MIN : CCurrencyState::CONVERSION_TX_SIZE_MIN;
    int64_t conversionSizeOverhead = 0;

    if (!(flags & FLAG_FRACTIONAL))
    {
        return CCoinbaseCurrencyState();
    }

    uint32_t tipTime = (chainActive.Height() >= height) ? chainActive[height]->nTime : chainActive.LastTip()->nTime;
    CCoinsViewCache view(pcoinsTip);

    // organize all valid reserve exchange transactions into multimaps, limitBuys, limitSells, and market orders
    // orders that should be treated as normal/failed fill or kill will go into refunds and invalid transactions into rejects
    for (int i = 0; i < orders.size(); i++)
    {
        if (orders[i].IsValid() && orders[i].IsReserveExchange())
        {
            // if this is a market order, put it in, if limit, put it in an order book, sorted by limit
            if (orders[i].IsMarket())
            {
                CAmount fee = orders[i].AllFeesAsNative(*this) + ReserveToNative(orders[i].ReserveConversionFeesMap()) + orders[i].nativeConversionFees;
                CFeeRate feeRate = CFeeRate(fee, GetSerializeSize(CDataStream(SER_NETWORK, PROTOCOL_VERSION), *orders[i].ptx));
                marketOrders.insert(std::make_pair(feeRate.GetFeePerK(), orders[i]));
            }
            else
            {
                assert(orders[i].IsLimit());
                // limit order, so put it in buy or sell
                if (orders[i].numBuys)
                {
                    limitBuys[orders[i].vRex[0].second.currencyID].insert(std::make_pair(orders[i].vRex[0].second.nLimit, orders[i]));
                }
                else
                {
                    assert(orders[i].numSells);
                    limitSells[orders[i].vRex[0].second.currencyID].insert(std::make_pair(orders[i].vRex[0].second.nLimit, orders[i]));
                }
            }
        }
        else if (orders[i].IsValid())
        {
            expiredFillOrKills.push_back(&(orders[i]));
        }
        else
        {
            rejects.push_back(&(orders[i]));
        }
    }

    // now we have all market orders in marketOrders multimap, sorted by feePerK, rejects that are expired or invalid in rejects
    // first, prune as many market orders as possible up to the maximum storage space available for market orders, which we calculate
    // as a functoin of the total space available and precentage of total orders that are market orders
    int64_t numLimitOrders = limitBuys.size() + limitSells.size();
    int64_t numMarketOrders = marketOrders.size();

    // if nothing to do, we are done, don't update anything
    if (!(reserveFills.size() + numLimitOrders + numMarketOrders))
    {
        return *this;
    }

    // 1. start from the current state and calculate what the price would be with market orders that fit, leaving room for limit orders
    // 2. add limit orders, first buys, as many as we can from the highest value and number downward, one at a time, until we run out or
    //    cannot add any more due to not meeting the price. then we do the same for sells if there are any available, and alternate until
    //    we either run out or cannot add from either side. within a specific limit, orders are sorted by largest first, which means
    //    there is no point in retracing if an element fails to be added
    // 3. calculate a final order price
    // 4. create and return a new, updated CCoinbaseCurrencyState
    CCoinbaseCurrencyState newState(*(CCurrencyState *)this);

    conversionSizeOverhead = GetSerializeSize(mConversionTx, SER_NETWORK, PROTOCOL_VERSION);

    int64_t curSpace = maxSerializeSize - (totalSerializedSize + conversionSizeOverhead);

    if (curSpace < 0)
    {
        printf("%s: no space available in block to include reserve orders\n", __func__);
        LogPrintf("%s: no space available in block to include reserve orders\n", __func__);
        return *this;
    }

    // the ones that are passed in reserveFills may be any valid reserve related transaction
    // we need to first process those with the assumption that they are included
    for (auto it = reserveFills.begin(); it != reserveFills.end(); )
    {
        auto &txDesc = *it;

        // add up the starting point for conversions
        if (txDesc.IsReserveExchange())
        {
            CMutableTransaction mtx;
            mtx = mConversionTx;
            txDesc.AddConversionInOuts(mtx, tmpConversionInputs, exchangeRateMap);
            conversionSizeOverhead = GetSerializeSize(mtx, SER_NETWORK, PROTOCOL_VERSION);
            int64_t tmpSpace = maxSerializeSize - (totalSerializedSize + conversionSizeOverhead);

            if (tmpSpace < 0)
            {
                // can't fit, so it's a noFill
                noFills.push_back(txDesc);
                it = reserveFills.erase(it);
            }
            else
            {
                curSpace = tmpSpace;

                totalNativeConversionFees += txDesc.nativeConversionFees;
                totalReserveConversionFees += txDesc.ReserveConversionFeesMap();

                if (feesAsReserve)
                {
                    newState.reserveIn = AddVectors(txDesc.ReserveOutConvertedVec(*this), newState.reserveIn);
                    newState.nativeIn = AddVectors(txDesc.NativeOutConvertedVec(*this), newState.nativeIn);
                    newState.nativeIn[0] += txDesc.NativeFees() + txDesc.nativeConversionFees;
                }
                else
                {
                    newState.reserveIn = AddVectors(
                                            AddVectors(txDesc.ReserveOutConvertedVec(*this), newState.reserveIn),
                                            AddVectors(txDesc.ReserveFees().AsCurrencyVector(currencies), txDesc.ReserveConversionFeesVec(*this))
                                         );
                    newState.nativeIn = AddVectors(txDesc.NativeOutConvertedVec(*this), newState.nativeIn);
                }

                mConversionTx = mtx;
                it++;
            }
        }
        else
        {
            if (feesAsReserve && newState.nativeIn.size())
            {
                newState.nativeIn[0] += txDesc.NativeFees();
            }
            else
            {
                newState.reserveIn = AddVectors(newState.reserveIn, txDesc.ReserveFees().AsCurrencyVector(currencies));
            }
            it++;
        }
    }

    int64_t marketOrdersSizeLimit = curSpace;
    int64_t limitOrdersSizeLimit = curSpace;
    if (numLimitOrders + numMarketOrders)
    {
        marketOrdersSizeLimit = ((arith_uint256(numMarketOrders) * arith_uint256(curSpace)) / arith_uint256(numLimitOrders + numMarketOrders)).GetLow64();
        limitOrdersSizeLimit = curSpace - marketOrdersSizeLimit;
    }
    
    if (limitOrdersSizeLimit < 1024 && maxSerializeSize > 2048)
    {
        marketOrdersSizeLimit = maxSerializeSize - 1024;
        limitOrdersSizeLimit = 1024;
    }

    for (auto marketOrder : marketOrders)
    {
        // add as many as we can fit, if we are able to, consider the output transaction overhead as well
        int64_t thisSerializeSize = GetSerializeSize(*marketOrder.second.ptx, SER_NETWORK, PROTOCOL_VERSION);
        CMutableTransaction mtx;
        mtx = mConversionTx;
        marketOrder.second.AddConversionInOuts(mtx, tmpConversionInputs, exchangeRateMap);
        conversionSizeOverhead = GetSerializeSize(mtx, SER_NETWORK, PROTOCOL_VERSION);
        if ((totalSerializedSize + thisSerializeSize + conversionSizeOverhead) <= marketOrdersSizeLimit)
        {
            totalNativeConversionFees += marketOrder.second.nativeConversionFees;
            totalReserveConversionFees += marketOrder.second.ReserveConversionFeesMap();
            mConversionTx = mtx;

            if (feesAsReserve)
            {
                newState.reserveIn = AddVectors(marketOrder.second.ReserveOutConvertedVec(*this), newState.reserveIn);
                newState.nativeIn = AddVectors(marketOrder.second.NativeOutConvertedVec(*this), newState.nativeIn);
                newState.nativeIn[0] += marketOrder.second.NativeFees() + marketOrder.second.nativeConversionFees;
            }
            else
            {
                newState.reserveIn = AddVectors(
                                        AddVectors(marketOrder.second.ReserveOutConvertedVec(*this), newState.reserveIn),
                                        AddVectors(marketOrder.second.ReserveFees().AsCurrencyVector(currencies), marketOrder.second.ReserveConversionFeesVec(*this))
                                        );
                newState.nativeIn = AddVectors(marketOrder.second.NativeOutConvertedVec(*this), newState.nativeIn);
            }

            reserveFills.push_back(marketOrder.second);
            totalSerializedSize += thisSerializeSize;
        }
        else
        {
            // can't fit, no fill
            noFills.push_back(marketOrder.second);
        }
    }

    // iteratively add limit orders first buy, then sell, until we no longer have anything to add
    // this must iterate because each time we add a buy, it may put another sell's limit within reach and
    // vice versa
    std::map<uint160, std::pair<std::multimap<CAmount, CReserveTransactionDescriptor>::iterator, CAmount>> buyPartial; // valid in loop for partial fill
    std::map<uint160, std::pair<std::multimap<CAmount, CReserveTransactionDescriptor>::iterator, CAmount>> sellPartial;

    limitOrdersSizeLimit = maxSerializeSize - totalSerializedSize;
    int64_t buyLimitSizeLimit = numLimitOrders ? totalSerializedSize + ((arith_uint256(limitBuys.size()) * arith_uint256(limitOrdersSizeLimit)) / arith_uint256(numLimitOrders)).GetLow64() : limitOrdersSizeLimit;

    CCurrencyState latestState = newState;

    // TODO - finish limit orders, which are significantly more complex after moving to
    // multi-reserves. until then, skip limit orders.
    /*
    for (bool tryagain = true; tryagain; )
    {
        tryagain = false;

        exchangeRates = ConvertAmounts(newState.reserveIn, newState.nativeIn, latestState);

        // starting with that exchange rate, add buys one at a time until we run out of buys to add or reach the limit,
        // possibly in a partial fill, then see if we can add any sells. we go back and forth until we have stopped being able to add
        // new orders. it would be possible to recognize a state where we are able to simultaneously fill a buy and a sell that are
        // the the same or very minimally overlapping limits
        
        for (auto limitBuysIt = limitBuys.rbegin(); limitBuysIt != limitBuys.rend() && exchangeRate > limitBuysIt->second.vRex.front().second.nLimit; limitBuysIt = limitBuys.rend())
        {
            // any time there are entries above the lower bound, we only actually look at the end, since we mutate the
            // search space each iteration and remove anything we've already added, making the end always the most in-the-money
            CReserveTransactionDescriptor &currentBuy = limitBuysIt->second;

            // it must first fit, space-wise
            int64_t thisSerializeSize = GetSerializeSize(*(currentBuy.ptx), SER_NETWORK, PROTOCOL_VERSION);

            CMutableTransaction mtx;
            if (pConversionTx)
            {
                mtx = *pConversionTx;
                currentBuy.AddConversionInOuts(mtx, tmpConversionInputs);
                conversionSizeOverhead = GetSerializeSize(mtx, SER_NETWORK, PROTOCOL_VERSION);
            }
            if ((totalSerializedSize + thisSerializeSize + conversionSizeOverhead) <= buyLimitSizeLimit)
            {
                CAmount newReserveIn, newNativeIn;
                if (feesAsReserve)
                {
                    newReserveIn = currentBuy.reserveOutConverted;
                    newNativeIn = currentBuy.nativeOutConverted + currentBuy.NativeFees() + currentBuy.nativeConversionFees;
                }
                else
                {
                    newReserveIn = currentBuy.reserveOutConverted + currentBuy.ReserveFees() + currentBuy.reserveConversionFees;
                    newNativeIn = currentBuy.nativeOutConverted;
                }

                // calculate fresh with all conversions together to see if we still meet the limit
                CAmount newExchange = ConvertAmounts(newState.ReserveIn + newReserveIn, newState.NativeIn + newNativeIn, latestState);

                if (newExchange <= currentBuy.vRex[0].second.nLimit)
                {
                    totalNativeConversionFees += currentBuy.nativeConversionFees;
                    totalReserveConversionFees += currentBuy.reserveConversionFees;

                    // update conversion transaction if we have one
                    if (pConversionTx)
                    {
                        *pConversionTx = mtx;
                    }

                    // add to the current buys, we will never do something to disqualify this, since all orders left are either
                    // the same limit or lower
                    reserveFills.push_back(currentBuy);
                    totalSerializedSize += thisSerializeSize;

                    newState.ReserveIn += newReserveIn;
                    newState.NativeIn += newNativeIn;

                    exchangeRate = newExchange;
                    limitBuys.erase(--limitBuys.end());
                }
                else
                {
                    // TODO:PBAAS support partial fills
                    break;
                }
            }
        }

        // now, iterate from lowest/most qualified sell limit first to highest/least qualified and attempt to put all in
        // if we can only fill an order partially, then do so
        for (auto limitSellsIt = limitSells.begin(); limitSellsIt != limitSells.end() && exchangeRate > limitSellsIt->second.vRex.front().second.nLimit; limitSellsIt = limitSells.begin())
        {
            CReserveTransactionDescriptor &currentSell = limitSellsIt->second;

            int64_t thisSerializeSize = GetSerializeSize(*currentSell.ptx, SER_NETWORK, PROTOCOL_VERSION);

            CMutableTransaction mtx;
            if (pConversionTx)
            {
                mtx = *pConversionTx;
                currentSell.AddConversionInOuts(mtx, tmpConversionInputs);
                conversionSizeOverhead = GetSerializeSize(mtx, SER_NETWORK, PROTOCOL_VERSION);
            }

            if ((totalSerializedSize + thisSerializeSize + conversionSizeOverhead) <= maxSerializeSize)
            {
                CAmount newReserveIn, newNativeIn;
                if (feesAsReserve)
                {
                    newReserveIn = currentSell.reserveOutConverted;
                    newNativeIn = currentSell.nativeOutConverted + currentSell.NativeFees() + currentSell.nativeConversionFees;
                }
                else
                {
                    newReserveIn = currentSell.reserveOutConverted + currentSell.ReserveFees() + currentSell.reserveConversionFees;
                    newNativeIn = currentSell.nativeOutConverted;
                }

                // calculate fresh with all conversions together to see if we still meet the limit
                CAmount newExchange = ConvertAmounts(newState.ReserveIn + newReserveIn, newState.NativeIn + newNativeIn, latestState);
                if (newExchange >= currentSell.vRex.front().second.nLimit)
                {
                    totalNativeConversionFees += currentSell.nativeConversionFees;
                    totalReserveConversionFees += currentSell.reserveConversionFees;

                    // update conversion transaction if we have one
                    if (pConversionTx)
                    {
                        *pConversionTx = mtx;
                    }

                    // add to the current sells, we will never do something to disqualify this, since all orders left are either
                    // the same limit or higher
                    reserveFills.push_back(currentSell);
                    totalSerializedSize += thisSerializeSize;
                    exchangeRate = newExchange;

                    newState.ReserveIn += newReserveIn;
                    newState.NativeIn += newNativeIn;

                    limitSells.erase(limitSellsIt);
                    tryagain = true;
                }
                else
                {
                    // we must be done with buys whether we created a partial or not
                    break;
                }
            }
        }
        buyLimitSizeLimit = maxSerializeSize - totalSerializedSize;
    }
    */

    // we can calculate total fees now, but to avoid a rounding error when converting native to reserve or vice versa on fees, 
    // we must loop through once more to calculate all fees for the currency state
    auto curMap = GetReserveMap();
    CCurrencyValueMap totalReserveFees;
    CAmount totalNativeFees = 0;
    CCurrencyValueMap reserveOutVal;
    CMutableTransaction mtx = pConversionTx ? *pConversionTx : CMutableTransaction();
    for (auto fill : reserveFills)
    {
        fill.AddConversionInOuts(mtx, conversionInputs, exchangeRateMap);
        reserveOutVal += NativeToReserveRaw(fill.NativeOutConvertedVec(latestState), exchangeRates);
        if (feesAsReserve)
        {
            totalReserveFees += fill.AllFeesAsReserve(newState, exchangeRates);
        }
        else
        {
            totalNativeFees += fill.AllFeesAsNative(newState, exchangeRates);
        }
    }
    if (pConversionTx)
    {
        *pConversionTx = mtx;
    }
    if (feesAsReserve)
    {
        totalReserveConversionFees.valueMap[currencies[0]] += NativeToReserve(totalNativeConversionFees, exchangeRates[0]);
        reserveOutVal += totalReserveConversionFees;
        totalReserveFees += totalReserveConversionFees;
    }
    else
    {
        totalNativeConversionFees = totalNativeConversionFees + ReserveToNativeRaw(totalReserveConversionFees, exchangeRates);
        totalNativeFees += totalNativeConversionFees;
    }

    newState = CCoinbaseCurrencyState(latestState, 
                                      totalNativeFees, totalNativeConversionFees,
                                      newState.reserveIn,
                                      newState.nativeIn,
                                      newState.reserveOut, 
                                      exchangeRates, 
                                      totalReserveFees.AsCurrencyVector(currencies), 
                                      totalReserveConversionFees.AsCurrencyVector(currencies));

    prices = exchangeRates;

    for (auto &curBuys : limitBuys)
    {
        for (auto &entry : curBuys.second)
        {
            noFills.push_back(entry.second);
        }
    }

    for (auto &curSells : limitSells)
    {
        for (auto &entry : curSells.second)
        {
            noFills.push_back(entry.second);
        }
    }

    // if no matches, no state updates
    if (!reserveFills.size())
    {
        return *this;
    }
    else
    {
        if (pInOutTotalSerializeSize)
        {
            *pInOutTotalSerializeSize = totalSerializedSize;
        }
        printf("%s: %s\n", __func__, newState.ToUniValue().write(1, 2).c_str());
        return newState;
    }
}

CAmount CCurrencyState::CalculateConversionFee(CAmount inputAmount, bool convertToNative, int currencyIndex) const
{
    arith_uint256 bigAmount(inputAmount);
    arith_uint256 bigSatoshi(SATOSHIDEN);

    // we need to calculate a fee based either on the amount to convert or the last price
    // times the reserve
    if (convertToNative)
    {
        int64_t price;
        cpp_dec_float_50 priceInReserve = PriceInReserveDecFloat50(currencyIndex);
        if (!to_int64(priceInReserve, price))
        {
            assert(false);
        }
        bigAmount = price ? (bigAmount * bigSatoshi) / arith_uint256(price) : 0;
    }

    CAmount fee = 0;
    fee = ((bigAmount * arith_uint256(CReserveExchange::SUCCESS_FEE)) / bigSatoshi).GetLow64();
    if (fee < CReserveExchange::MIN_SUCCESS_FEE)
    {
        fee = CReserveExchange::MIN_SUCCESS_FEE;
    }
    return fee;
}

CAmount CReserveTransactionDescriptor::CalculateConversionFeeNoMin(CAmount inputAmount)
{
    arith_uint256 bigAmount(inputAmount);
    arith_uint256 bigSatoshi(SATOSHIDEN);
    return ((bigAmount * arith_uint256(CReserveExchange::SUCCESS_FEE)) / bigSatoshi).GetLow64();
}

CAmount CReserveTransactionDescriptor::CalculateConversionFee(CAmount inputAmount)
{
    CAmount fee = CalculateConversionFeeNoMin(inputAmount);
    if (fee < CReserveExchange::MIN_SUCCESS_FEE)
    {
        fee = CReserveExchange::MIN_SUCCESS_FEE;
    }
    return fee;
}

// this calculates a fee that will be added to an amount and result in the same percentage as above,
// such that a total of the inputAmount + this returned fee, if passed to CalculateConversionFee, would return
// the same amount
CAmount CReserveTransactionDescriptor::CalculateAdditionalConversionFee(CAmount inputAmount)
{
    arith_uint256 bigAmount(inputAmount);
    arith_uint256 bigSatoshi(SATOSHIDEN);
    arith_uint256 conversionFee(CReserveExchange::SUCCESS_FEE);

    CAmount newAmount = ((bigAmount * bigSatoshi) / (bigSatoshi - conversionFee)).GetLow64();
    if (newAmount - inputAmount < CReserveExchange::MIN_SUCCESS_FEE)
    {
        newAmount = inputAmount + CReserveExchange::MIN_SUCCESS_FEE;
    }
    CAmount fee = CalculateConversionFee(newAmount);
    newAmount = inputAmount + fee;
    fee = CalculateConversionFee(newAmount);            // again to account for minimum fee
    fee += inputAmount - (newAmount - fee);             // add any additional difference
    return fee;
}

bool CFeePool::GetFeePool(CFeePool &feePool, uint32_t height)
{
    CBlock block;
    CTransaction coinbaseTx;
    feePool.SetInvalid();
    if (!height || chainActive.Height() < height)
    {
        height = chainActive.Height();
    }
    if (!height)
    {
        return true;
    }
    if (ReadBlockFromDisk(block, chainActive[height], Params().GetConsensus()))
    {
        coinbaseTx = block.vtx[0];
    }
    else
    {
        return false;
    }
    
    for (auto &txOut : coinbaseTx.vout)
    {
        COptCCParams p;
        if (txOut.scriptPubKey.IsPayToCryptoCondition(p) && p.IsValid() && p.evalCode == EVAL_FEE_POOL && p.vData.size())
        {
            feePool = CFeePool(p.vData[0]);
        }
    }
    return true;
}

bool CFeePool::GetCoinbaseFeePool(const CTransaction &coinbaseTx, CFeePool &feePool)
{
    if (coinbaseTx.IsCoinBase())
    {
        for (auto &txOut : coinbaseTx.vout)
        {
            COptCCParams p;
            if (txOut.scriptPubKey.IsPayToCryptoCondition(p) && p.IsValid() && p.evalCode == EVAL_FEE_POOL && p.vData.size())
            {
                feePool = CFeePool(p.vData[0]);
                return true;
            }
        }
    }
    return false;
}

bool ValidateFeePool(struct CCcontract_info *cp, Eval* eval, const CTransaction &tx, uint32_t nIn, bool fulfilled)
{
    // fee pool output is unspendable
    return false;
}

bool IsFeePoolInput(const CScript &scriptSig)
{
    return false;
}

bool PrecheckFeePool(const CTransaction &tx, int32_t outNum, CValidationState &state, uint32_t height)
{
    return true;
}

