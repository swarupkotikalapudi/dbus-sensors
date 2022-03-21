/*
// Copyright (c) 2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include <IpmbSDRSensor.hpp>

uint8_t busIndex(uint8_t cmdAddr)
{
    uint8_t busValue = ((cmdAddr >> ipmbLeftShift) + 1);
    return busValue;
}

/* This function will store the record count of the SDR sensors for each IPMB
 * bus */

void IpmbSDR::ipmbGetSdrInfo(std::shared_ptr<sdbusplus::asio::connection>& conn,
                             uint8_t cmdAddr)
{
    std::shared_ptr<IpmbSDR> sharedSDRPtr = std::make_shared<IpmbSDR>();

    conn->async_method_call(
        [sharedSDRPtr, conn, cmdAddr](boost::system::error_code ec,
                                      const IpmbMethodType& response) {
            const int status = std::get<0>(response);

            if (ec || status)
            {
                std::cerr << "Error reading from IpmbGetSdrInfo for host "
                          << busIndex(cmdAddr) << "\n";
                return;
            }

            const std::vector<uint8_t> data = std::get<5>(response);

            if (data.empty())
            {
                std::cerr << " IPMB get SDR info is empty for host "
                          << busIndex(cmdAddr) << "\n";
                return;
            }
            uint16_t recordCount = ((data[2] << 8) | data[1]);

            sharedSDRPtr->ipmbSdrRsrv(sharedSDRPtr, conn, recordCount, cmdAddr);
        },
        ipmbService, ipmbDbusPath, ipmbInterface, ipmbMethod, cmdAddr,
        sdr::netfnStorageReq, lun, sdr::cmdStorageGetSdrInfo, sdrCommandData);
}

/* This function will store the reserve ID for each IPMB bus index */

void IpmbSDR::ipmbSdrRsrv(
    const std::shared_ptr<IpmbSDR>& sharedSDRPtr,
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    uint16_t recordCount, uint8_t cmdAddr)
{
    conn->async_method_call(
        [sharedSDRPtr, conn, cmdAddr, recordCount](
            boost::system::error_code ec, const IpmbMethodType& response) {
            const int status = std::get<0>(response);

            if (ec || status)
            {
                std::cerr << "Error reading from IpmbSDR Reserve for host "
                          << busIndex(cmdAddr) << "\n";
                return;
            }

            const std::vector<uint8_t> data = std::get<5>(response);

            if (data.empty())
            {
                std::cerr << " IPMB SDR get Reserve ID is empty for host "
                          << busIndex(cmdAddr) << "\n";
                return;
            }
            uint8_t resrvIDLSB = data[0];
            uint8_t resrvIDMSB = data[1];

            sharedSDRPtr->getSDRSensorData(sharedSDRPtr, conn, recordCount,
                                           resrvIDLSB, resrvIDMSB, cmdAddr);
        },
        ipmbService, ipmbDbusPath, ipmbInterface, ipmbMethod, cmdAddr,
        sdr::netfnStorageReq, lun, sdr::cmdStorageRsrvSdr, sdrCommandData);
}

/* This function will read all the information related to the sensor
 * such as name, threshold value, unit, device address, SDR type */

void IpmbSDR::getSDRSensorData(
    const std::shared_ptr<IpmbSDR>& sharedSDRPtr,
    const std::shared_ptr<sdbusplus::asio::connection>& conn,
    uint16_t recordCount, uint8_t resrvIDLSB, uint8_t resrvIDMSB,
    uint8_t cmdAddr)
{
    uint8_t loopCount = sdr::perCountByte * iCnt;
    std::vector<uint8_t> commandData = {resrvIDLSB,      resrvIDMSB,
                                        nextRecordIDLSB, nextRecordIDMSB,
                                        loopCount,       sdr::perCountByte};

    conn->async_method_call(
        [sharedSDRPtr, conn, recordCount, resrvIDLSB, resrvIDMSB, cmdAddr](
            boost::system::error_code ec, const IpmbMethodType& response) {
            const int status = std::get<0>(response);
            if (ec || status)
            {
                std::cerr << "Error reading from getSensorData for host "
                          << busIndex(cmdAddr) << "\n";
                return;
            }

            const std::vector<uint8_t> data = std::get<5>(response);
            if (data.empty())
            {
                std::cerr << "IPMB SDR sensor data is empty for host "
                          << busIndex(cmdAddr) << "\n";
                return;
            }

            for (size_t d : data)
            {
                sharedSDRPtr->getSdrData.push_back(d);
            }

            if ((sharedSDRPtr->validRecordCount <= recordCount) &&
                (sharedSDRPtr->iCnt <
                 static_cast<uint8_t>(SDRCountType::cntType01)))
            {
                sharedSDRPtr->iCnt += 1;
                sharedSDRPtr->getSDRSensorData(sharedSDRPtr, conn, recordCount,
                                               resrvIDLSB, resrvIDMSB, cmdAddr);
            }
            else if ((sharedSDRPtr->validRecordCount == recordCount) &&
                     (sharedSDRPtr->iCnt ==
                      static_cast<uint8_t>(SDRCountType::cntType01)))
            {
                sharedSDRPtr->sdrDataCheck(sharedSDRPtr->getSdrData, cmdAddr,
                                           recordCount,
                                           sharedSDRPtr->validRecordCount);
                sharedSDRPtr->getSdrData.clear();
                sharedSDRPtr->validRecordCount = 1;
                sharedSDRPtr->nextRecordIDLSB = 0;
                sharedSDRPtr->nextRecordIDMSB = 0;
                return;
            }
            else
            {
                sharedSDRPtr->sdrDataCheck(sharedSDRPtr->getSdrData, cmdAddr,
                                           recordCount,
                                           sharedSDRPtr->validRecordCount);
                sharedSDRPtr->validRecordCount += 1;
                sharedSDRPtr->iCnt = 0;
                sharedSDRPtr->nextRecordIDLSB =
                    sharedSDRPtr->getSdrData[sdr::sdrNxtRecLSB];
                sharedSDRPtr->nextRecordIDMSB =
                    sharedSDRPtr->getSdrData[sdr::sdrNxtRecMSB];
                sharedSDRPtr->getSdrData.clear();
                sharedSDRPtr->getSDRSensorData(sharedSDRPtr, conn, recordCount,
                                               resrvIDLSB, resrvIDMSB, cmdAddr);
            }
        },
        ipmbService, ipmbDbusPath, ipmbInterface, ipmbMethod, cmdAddr,
        sdr::netfnStorageReq, lun, sdr::cmdStorageGetSdr, commandData);
}

/* This function will convert the SDR sensor data such as sensor unit, name, ID,
 * type from decimal to readable format */

void IpmbSDR::sdrDataCheck(std::vector<uint8_t> data, uint8_t cmdAddr,
                           uint16_t recordCount, uint16_t validRecordCount)
{
    if (data[sdr::sdrType] != static_cast<uint8_t>(SDRType::sdrType01))
    {
        return;
    }

    if (data.size() != static_cast<uint8_t>(SDRDataSize::dataLenType01))
    {
        std::cerr << "SDR Type 1 data length is invalid for host "
                  << busIndex(cmdAddr) << "\n";
        return;
    }

    if (validRecordCount == recordCount)
    {
        std::cerr << "SDR sensor for host " << busIndex(cmdAddr) << "\n";
    }

    /* sdrType (Byte 5) represents the SDR Type such as 1, 2, 3 */

    uint8_t sensorType = data[sdr::sdrType];

    /* sdrSenNum (Byte 9) represents the sensor ID */

    uint8_t sensorID = data[sdr::sdrSenNum];

    /* negHandleValue (Byte 24) represents the sensor with max reading margin */

    int neg = data[sdr::negHandleValue];

    /* sdrUnitType01 (Byte 25) represents sensor unit */

    uint8_t unit = data[sdr::sdrUnitType01];

    /* sdrAdrType01 (Byte 56) represents the starting byte for SDR type 1 sensor
     * name */

    int iStrAddr = static_cast<uint8_t>(SDRAddrType::sdrAdrType01);

    /* sdrLenType01 (Byte 53) and(&) with sdrLenBit(0x1F) will give the
     * length of the sensor name of SDR Type 1 */

    int iStrLen = ((data[static_cast<uint8_t>(SDRNameType::nameLenType01)]) &
                   (static_cast<int>(SDRNameType::sdrLenBit)));

    /* Below for loop will convert the bytes to string and form a sensor name */

    std::string tempName;
    for (int iLoop = 0; iLoop < iStrLen; iLoop++)
    {
        tempName = std::string(data.data() + iStrAddr,
                               data.data() + iStrAddr + iStrLen);
    }

    sdrThresholdCheck(data, busIndex(cmdAddr), tempName, unit, neg, sensorType,
                      sensorID);
}

/* This function will convert the raw value of threshold for each sensor */

void IpmbSDR::sdrThresholdCheck(std::vector<uint8_t> data, uint8_t busIndex,
                                std::string tempName, uint8_t unit, int neg,
                                uint8_t sensorType, uint8_t sensorID)
{
    struct SensorInfo temp;

    /* sdrSensCapab (Byte 13) and(&) with sdrThresAcce(0x0C) will declare
     * whether threshold is present for each sensor */

    int threshold = ((data[sdr::sdrSensCapab]) & (sdr::sdrThresAcce));
    temp.sensCap = threshold;

    /* mData        - 10 bits
     * mDataByte    - Byte 28 - 8 bits LSB
     * mTolDataByte - Byte 29 - 2 bits MSB [7-6]
     *
     * mTolDataByte (MSB) is right shifted by 6 and again left shifted by 8 to
     * make a 10 bit value, then OR(|) with mDataByte (LSB)
     */

    uint16_t mData = ((data[sdr::mTolDataByte] >> sdr::bitShiftMsb) << 8) |
                     data[sdr::mDataByte];

    /* bData        - 10 bits
     * bDataByte    - Byte 30 - 8 bits LSB
     * bAcuDataByte - Byte 31 - 2 bits MSB [7-6]
     *
     * bAcuDataByte (MSB) is right shifted by 6 and again left shifted by 8 to
     * make a 10 bit value, then OR(|) with bDataByte (LSB)
     */

    uint16_t bData = ((data[sdr::bAcuDataByte] >> sdr::bitShiftMsb) << 8) |
                     data[sdr::bDataByte];

    /* rbExpDataByte (Byte 33) represents the exponent value
     *  Bit [3-0] - B Exponent 2's complement signed bit.
     *  Bit [7-4] - R Exponent 2's complement signed bit.
     */

    int8_t bExpVal = data[sdr::rbExpDataByte] & 0xF;
    if (bExpVal > 7)
    {
        bExpVal = (~bExpVal + 1) & 0xF;
    }

    /* Shifting the data to right by 4, since rExpVal has 4 bits from 4 to 7 in
     * byte 33 */

    int8_t rExpVal = (data[sdr::rbExpDataByte] >> 4) & 0xF;
    if (rExpVal > 7)
    {
        rExpVal = (~rExpVal + 1) & 0xF;
        rExpVal = -rExpVal;
    }

    /* Sensor Threshold Reading Conversion
     *
     *  Y = ((Mx + (B * 10^K1)) * (10^K2))
     *
     *  X  - Raw value of threshold
     *  M  - mData Value
     *  B  - bData Value
     *  K1 - Signed Exponent of bExpVal
     *  K2 - Signed Exponent of rExpVal
     */

    double thresUpCri =
        ((mData * data[sdr::sdrUpCriType01]) + (bData * pow(10, bExpVal))) *
        (pow(10, rExpVal));
    double thresLoCri =
        ((mData * data[sdr::sdrLoCriType01]) + (bData * pow(10, bExpVal))) *
        (pow(10, rExpVal));

    temp.sensorReadName = std::move(tempName);
    temp.sensorUnit = unit;

    temp.thresUpperCri = thresUpCri;
    temp.thresLowerCri = thresLoCri;
    temp.mValue = mData;
    temp.bValue = bData;
    temp.bExp = bExpVal;
    temp.rExp = rExpVal;

    temp.sensorNumber = sensorID;
    temp.sensorSDRType = sensorType;

    temp.negRead = neg;
    temp.sensCap = threshold;

    if (busIndex <= 4)
    {
        sensorRecord[busIndex].push_back(temp);
    }
}
