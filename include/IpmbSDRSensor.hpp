#pragma once

#include <sensor.hpp>

static constexpr uint8_t ipmbLeftShift = 2;
static constexpr uint8_t lun = 0;

using IpmbMethodType =
    std::tuple<int, uint8_t, uint8_t, uint8_t, uint8_t, std::vector<uint8_t>>;

enum class SDRType
{
    sdrType01 = 1,
    sdrType02 = 2,
    sdrType03 = 3
};

enum class SDRDataSize
{
    dataLenType01 = 72,
    dataLenType02 = 54,
    dataLenType03 = 36
};

enum class SDRCountType
{
    cntType01 = 3,
    cntType02 = 2,
    cntType03 = 1,
};

enum class SDRNameType
{
    nameLenType01 = 53,
    nameLenType02 = 35,
    nameLenType03 = 20,
    sdrLenBit = 0x1F
};

enum class SDRAddrType
{
    sdrAdrType01 = 56,
    sdrAdrType02 = 38,
    sdrAdrType03 = 21
};

namespace sdr
{
static constexpr uint8_t maxPosReadingMargin = 127;
static constexpr uint8_t negHandleValue = 24;
static constexpr uint16_t thermalConst = 256;

static constexpr uint8_t netfnStorageReq = 0x0a;
static constexpr uint8_t cmdStorageGetSdrInfo = 0x20;
static constexpr uint8_t cmdStorageRsrvSdr = 0x22;
static constexpr uint8_t cmdStorageGetSdr = 0x23;

static constexpr uint8_t perCountByte = 16;

static constexpr uint8_t sdrNxtRecLSB = 0;
static constexpr uint8_t sdrNxtRecMSB = 1;
static constexpr uint8_t sdrType = 5;
static constexpr uint8_t sdrSenNum = 9;

static constexpr uint8_t sdrThresAcce = 0x0C;
static constexpr uint8_t sdrSensCapab = 13;
static constexpr uint8_t sdrSensNoThres = 0;

static constexpr uint8_t sdrUnitType01 = 25;
static constexpr uint8_t sdrUpCriType01 = 43;
static constexpr uint8_t sdrLoCriType01 = 46;

static constexpr uint8_t mDataByte = 28;
static constexpr uint8_t mTolDataByte = 29;
static constexpr uint8_t bDataByte = 30;
static constexpr uint8_t bAcuDataByte = 31;
static constexpr uint8_t rbExpDataByte = 33;
static constexpr uint8_t bitShiftMsb = 6;
} // namespace sdr

struct SensorInfo
{
    std::string sensorReadName;
    uint8_t sensorUnit;
    double thresUpperCri;
    double thresLowerCri;
    uint16_t mValue;
    uint16_t bValue;
    uint8_t sensorNumber;
    uint8_t sensorSDRType;
    int8_t rExp;
    int8_t bExp;
    uint8_t negRead;
    uint8_t sensCap;
};

struct IpmbSDR : std::enable_shared_from_this<IpmbSDR>
{
  public:
    std::vector<uint8_t> getSdrData;
    uint16_t validRecordCount = 1;
    uint8_t iCnt = 0;
    uint8_t nextRecordIDLSB = 0;
    uint8_t nextRecordIDMSB = 0;

    inline static std::vector<uint8_t> sdrCommandData = {};
    inline static std::array<std::vector<SensorInfo>, 5> sensorRecord = {};

    inline static std::map<uint8_t, std::string> sensorUnits = {
        {1, "temperature"}, {4, "voltage"}, {5, "current"}, {6, "power"}};

    void ipmbGetSdrInfo(std::shared_ptr<sdbusplus::asio::connection>& conn,
                        uint8_t cmdAddr);

    void ipmbSdrRsrv(const std::shared_ptr<IpmbSDR>& self,
                     const std::shared_ptr<sdbusplus::asio::connection>& conn,
                     uint16_t rec, uint8_t cmd);

    void getSDRSensorData(
        const std::shared_ptr<IpmbSDR>& self,
        const std::shared_ptr<sdbusplus::asio::connection>& conn,
        uint16_t recordCount, uint8_t resrvIDLSB, uint8_t resrvIDMSB,
        uint8_t cmdAddr);

    void sdrDataCheck(std::vector<uint8_t> data, uint8_t cmdAddr, uint16_t rec,
                      uint16_t valid);

    void sdrThresholdCheck(std::vector<uint8_t> data, uint8_t busIndex,
                           std::string tempName, uint8_t unit, int neg,
                           uint8_t sensorType, uint8_t sensorID);
};
