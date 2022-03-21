#include <IpmbSDRSensor.hpp>

const constexpr char* ipmbService = "xyz.openbmc_project.Ipmi.Channel.Ipmb";
const constexpr char* ipmbDbusPath = "/xyz/openbmc_project/Ipmi/Channel/Ipmb";
const constexpr char* ipmbInterface = "org.openbmc.Ipmb";
const constexpr char* ipmbMethod = "sendRequest";
static constexpr uint8_t lun = 0;

IpmbSDRDevice::IpmbSDRDevice(
    std::shared_ptr<sdbusplus::asio::connection>& dbusConnection,
    uint8_t cmdAddr) :
    commandAddress(cmdAddr << 2),
    hostIndex(cmdAddr + 1), conn(dbusConnection)
{}

bool validateStatus(boost::system::error_code ec,
                    const IpmbMethodType& response, int hostIndex)
{
    const int status = std::get<0>(response);
    if (ec || (status != 0))
    {
        std::cerr << "Error reading from IPMB SDR for host " << hostIndex
                  << "\n";
        return true;
    }
    return false;
}

/* This function will store the record count of the SDR sensors for each IPMB
 * bus */
void IpmbSDRDevice::getSDRRepositoryInfo()
{
    std::weak_ptr<IpmbSDRDevice> weakRef = weak_from_this();

    conn->async_method_call(
        [weakRef](boost::system::error_code ec,
                  const IpmbMethodType& response) {
        auto self = weakRef.lock();
        if (!self)
        {
            return;
        }

        auto status = std::bind_front(validateStatus, ec, response);
        if (status(self->hostIndex))
        {
            return;
        }

        const std::vector<uint8_t>& data = std::get<5>(response);
        const size_t sdrInfoDataSize = 14;

        if (data.size() < sdrInfoDataSize)
        {
            std::cerr << " IPMB Get SDR Repository Info data is empty for host "
                      << self->hostIndex << "\n";
            return;
        }

        uint8_t recordCountLSB = 1;
        uint8_t recordCountMSB = 2;

        uint16_t recordCount =
            (data[recordCountMSB] << 8) | data[recordCountLSB];

        self->reserveSDRRepository(recordCount);
        },
        ipmbService, ipmbDbusPath, ipmbInterface, ipmbMethod, commandAddress,
        sdr::netfnStorageReq, lun, sdr::cmdStorageGetSdrInfo, sdrCommandData);
}

/* This function will store the reserve ID for each IPMB bus index */
void IpmbSDRDevice::reserveSDRRepository(uint16_t recordCount)
{
    std::weak_ptr<IpmbSDRDevice> weakRef = weak_from_this();

    conn->async_method_call(
        [weakRef, recordCount](boost::system::error_code ec,
                               const IpmbMethodType& response) {
        auto self = weakRef.lock();
        if (!self)
        {
            return;
        }

        auto status = std::bind_front(validateStatus, ec, response);
        if (status(self->hostIndex))
        {
            return;
        }

        const std::vector<uint8_t>& data = std::get<5>(response);
        const size_t sdrReserveDataSize = 2;

        if (data.size() < sdrReserveDataSize)
        {
            std::cerr << " IPMB SDR Reserve Repository data is empty for host "
                      << self->hostIndex << "\n";
            return;
        }
        uint8_t resrvIDLSB = data[0];
        uint8_t resrvIDMSB = data[1];

        self->getSDRSensorData(recordCount, resrvIDLSB, resrvIDMSB);
        },
        ipmbService, ipmbDbusPath, ipmbInterface, ipmbMethod, commandAddress,
        sdr::netfnStorageReq, lun, sdr::cmdStorageRsrvSdr, sdrCommandData);
}

/* This function will read all the information related to the sensor
 * such as name, threshold value, unit, device address, SDR type */
void IpmbSDRDevice::getSDRSensorData(uint16_t recordCount, uint8_t resrvIDLSB,
                                     uint8_t resrvIDMSB)
{
    std::weak_ptr<IpmbSDRDevice> weakRef = weak_from_this();

    uint8_t loopCount = sdr::perCountByte * iCnt;
    std::vector<uint8_t> commandData = {resrvIDLSB,      resrvIDMSB,
                                        nextRecordIDLSB, nextRecordIDMSB,
                                        loopCount,       sdr::perCountByte};

    conn->async_method_call(
        [weakRef, recordCount, resrvIDLSB, resrvIDMSB](
            boost::system::error_code ec, const IpmbMethodType& response) {
        auto self = weakRef.lock();
        if (!self)
        {
            return;
        }

        auto status = std::bind_front(validateStatus, ec, response);
        if (status(self->hostIndex))
        {
            return;
        }

        const std::vector<uint8_t>& data = std::get<5>(response);
        const size_t sdrSensorDataSize = 18;

        if (data.size() < sdrSensorDataSize)
        {
            std::cerr << "IPMB SDR sensor data is empty for host "
                      << self->hostIndex << "\n";
            return;
        }

        self->handleSDRData(data, recordCount, resrvIDLSB, resrvIDMSB);
        },
        ipmbService, ipmbDbusPath, ipmbInterface, ipmbMethod, commandAddress,
        sdr::netfnStorageReq, lun, sdr::cmdStorageGetSdr, commandData);
}

/* This function will handle the sensor data received by IPMB response */
void IpmbSDRDevice::handleSDRData(const std::vector<uint8_t>& data,
                                  uint16_t recordCount, uint8_t resrvIDLSB,
                                  uint8_t resrvIDMSB)
{
    sdrData.insert(sdrData.end(), data.begin(), data.end());

    /* dataLength represents the size of data for SDR types */
    uint8_t dataLength = sdrData[sdr::dataLengthByte] + sdr::dataLengthByte + 1;

    /*  If sdrData size is less than dataLength, it will call getSDRSensorData
     *  function recursively till all the data is received.
     */
    if (sdrData.size() < dataLength)
    {
        iCnt++;
        getSDRSensorData(recordCount, resrvIDLSB, resrvIDMSB);
    }
    else
    {
        /*  After all the data is received, it is passed to checkSDRData
         *  function. Next sensor record ID is stored based on the previous
         *  record ID. Vector of sdrData is cleared to store next sensor data.
         *  validRecordCount is incremented and getSDRSensorData function is
         *  called to proceed with next set of sensors.
         */
        checkSDRData(sdrData, dataLength);
        iCnt = 0;
        nextRecordIDLSB = sdrData[sdr::sdrNxtRecLSB];
        nextRecordIDMSB = sdrData[sdr::sdrNxtRecMSB];
        sdrData.clear();

        if (validRecordCount == recordCount)
        {
            /* Once all the sensors are read and recordCount matched, it will
             * return. */
            nextRecordIDLSB = 0;
            nextRecordIDMSB = 0;
            return;
        }
        validRecordCount++;
        getSDRSensorData(recordCount, resrvIDLSB, resrvIDMSB);
    }
}

/* This function will convert the SDR sensor data such as sensor unit, name, ID,
 * type from decimal to readable format */
void IpmbSDRDevice::checkSDRData(std::vector<uint8_t>& data,
                                 uint8_t dataLength) const
{
    if (data.size() < dataLength)
    {
        return;
    }

    SDR01DataFields* resp = std::bit_cast<SDR01DataFields*>(data.data());

    /* sdrType represents the SDR Type (Byte 5) such as 1, 2, 3 */
    uint8_t sdrType = resp->sdrType;

    if (sdrType != static_cast<uint8_t>(SDRType::sdrType01))
    {
        return;
    }

    /*  dataLen represents the data length (Byte 6) for SDR sensor */
    int dataLen = resp->sdrDataLength;

    /* iStrLen represents the length of the sensor name for SDR Type 1 */
    int strLen = (resp->nameLenType) & (sdr::sdrLenBit);

    /* iStrAddr represents the starting byte (Byte 56) for SDR sensor name */
    int strAddr =
        dataLen + ((dataLen / (sdr::perCountByte)) * 4) - (strLen - 1);

    /* Below for loop will convert the bytes to string and form a sensor name */

    std::string tempName(data.begin() + strAddr,
                         data.begin() + strAddr + strLen);

    checkSDRThreshold(resp, hostIndex, tempName);
}

/* This function will convert the raw value of threshold for each sensor */
void IpmbSDRDevice::checkSDRThreshold(SDR01DataFields* resp, int busIndex,
                                      std::string tempName)
{
    uint8_t bitShiftMsb = 2;

    /* linear represents the sensor's linearization (Byte 27) */
    uint8_t linear = resp->sdrLinear;
    if (linear != 0)
    {
        return;
    }

    /* sdrSensCapability (Byte 13) and(&) with sdrThresAccess(0x0C) will declare
     * whether threshold is present for each sensor */
    int threshold = (resp->sdrSensCapability) & (sdr01_command::sdrThresAccess);

    /* mData        - 10 bits
     * mDataByte    - Byte 28 - 8 bits LSB
     * mTolDataByte - Byte 29 - 2 bits MSB [7-6]
     */
    uint16_t mData =
        ((resp->mTolDataByte & 0xC0) << bitShiftMsb) | resp->mDataByte;

    /* bData        - 10 bits
     * bDataByte    - Byte 30 - 8 bits LSB
     * bAcuDataByte - Byte 31 - 2 bits MSB [7-6]
     */
    uint16_t bData =
        ((resp->bAcuDataByte & 0xC0) << bitShiftMsb) | resp->bDataByte;

    /* rbExpDataByte (Byte 33) represents the exponent value
     *  Bit [3-0] - B Exponent 2's complement signed bit.
     *  Bit [7-4] - R Exponent 2's complement signed bit.
     */
    int8_t bExpVal = resp->rbExpDataByte & 0xF;
    if (bExpVal > 7)
    {
        bExpVal = (~bExpVal + 1) & 0xF;
    }

    /* Shifting the data to right by 4, since rExpVal has 4 bits from 4 to 7 in
     * byte 33 */
    int8_t rExpVal = (resp->rbExpDataByte >> 4) & 0xF;
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

    double bDataVal = bData * pow(10, bExpVal);
    double expVal = pow(10, rExpVal);

    double thresUpCri =
        sensorValCalculation(mData, bDataVal, expVal, resp->sdrUpCriType);
    double thresLoCri =
        sensorValCalculation(mData, bDataVal, expVal, resp->sdrLoCriType);

    struct SensorInfo temp;

    temp.sensorReadName = std::move(tempName);
    temp.sensorUnit = resp->sdrUnitType;

    temp.thresUpperCri = thresUpCri;
    temp.thresLowerCri = thresLoCri;

    temp.sensorNumber = resp->sdrSenNum;
    temp.sensCap = threshold;

    sensorRecord[busIndex].emplace_back(std::move(temp));

    SensorValConversion val = {mData, bDataVal, expVal, resp->sdrNegHandle};

    sensorValRecord[busIndex][resp->sdrSenNum] = val;
}

/* This function will calculate the sensor's threshold value */
double IpmbSDRDevice::sensorValCalculation(uint16_t mValue, double bValue,
                                           double expValue, double value)
{
    double sensorValue = ((mValue * value) + bValue) * expValue;
    return sensorValue;
}

/* This function will convert the SDR sensor value if value exceeds reading
 * margin */
double IpmbSDRDevice::dataConversion(double value,
                                     const uint8_t& commandAddress,
                                     std::vector<uint8_t> data)
{
    if (data.empty())
    {
        std::cerr << " Sensor value is empty in data Conversion \n";
        return std::numeric_limits<double>::quiet_NaN();
    }

    double dataVal = value;
    uint8_t busIndex = (commandAddress >> ipmbLeftShift) + 1;
    uint8_t sensorNum = data[0];

    SensorValConversion temp = sensorValRecord[busIndex][sensorNum];

    dataVal =
        sensorValCalculation(temp.mValue, temp.bValue, temp.expoVal, value);

    if (dataVal > sdr01_command::maxPosReadingMargin)
    {
        // Negative reading handle
        if (sdr01_command::twosCompVal == temp.negRead)
        {
            dataVal -= sdr01_command::thermalConst;
        }
    }

    return dataVal;
}
