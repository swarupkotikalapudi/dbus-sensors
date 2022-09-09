#include "NVMeIntf.hpp"

#include <boost/asio.hpp>
#include <sdbusplus/bus.hpp>

#include <thread>

/* */
class NVMeMi : public NVMeMiIntf, public std::enable_shared_from_this<NVMeMi>
{
  public:
    NVMeMi(boost::asio::io_context& io, sdbusplus::bus_t& dbus, int bus,
           int addr);
    virtual ~NVMeMi() override;

    int getNID() const override
    {
        return nid;
    }
    int getEID() const override
    {
        return eid;
    }
    void miSubsystemHealthStatusPoll(
        std::function<void(const std::error_code&,
                           nvme_mi_nvm_ss_health_status*)>&& cb) override;
    void miScanCtrl(std::function<void(const std::error_code&,
                                       const std::vector<nvme_mi_ctrl_t>&)>
                        cb) override;
    void adminIdentify(nvme_mi_ctrl_t ctrl, nvme_identify_cns cns,
                       uint32_t nsid, uint16_t cntid,
                       std::function<void(const std::error_code&,
                                          std::span<uint8_t>)>&& cb) override;

  private:
    static nvme_root_t nvmeRoot;

    boost::asio::io_context& io;
    sdbusplus::bus_t& dbus;
    nvme_mi_ep_t nvmeEP;

    int nid;
    uint8_t eid;
    std::string mctpPath;

    boost::asio::io_context workerIO;
    std::jthread thread;
};
