#include "matter_info.h"

#include <app/server/Server.h>
#include <platform/DeviceInstanceInfoProvider.h>
#include <platform/PlatformManager.h>
#include <setup_payload/OnboardingCodesUtil.h>

using namespace chip;

namespace {
struct StackLock {
    StackLock() { DeviceLayer::PlatformMgr().LockChipStack(); }
    ~StackLock() { DeviceLayer::PlatformMgr().UnlockChipStack(); }
};

const RendezvousInformationFlags kRendezvous(RendezvousInformationFlag::kBLE);
} // namespace

extern "C" bool matter_get_qr_code(char *buf, size_t len)
{
    StackLock lock;
    MutableCharSpan span(buf, len);
    return GetQRCode(span, kRendezvous) == CHIP_NO_ERROR;
}

extern "C" bool matter_get_manual_code(char *buf, size_t len)
{
    StackLock lock;
    MutableCharSpan span(buf, len);
    return GetManualPairingCode(span, kRendezvous) == CHIP_NO_ERROR;
}

extern "C" int matter_fabric_vendors(uint16_t *vendor_ids, int max)
{
    StackLock lock;
    int n = 0;
    for (const auto &fabric : Server::GetInstance().GetFabricTable()) {
        if (n < max) {
            vendor_ids[n] = static_cast<uint16_t>(fabric.GetVendorId());
        }
        n++;
    }
    return n;
}

extern "C" bool matter_get_device_info(matter_device_info_t *info)
{
    StackLock lock;
    DeviceLayer::DeviceInstanceInfoProvider *p = DeviceLayer::GetDeviceInstanceInfoProvider();
    if (!p) {
        return false;
    }
    bool ok = p->GetVendorName(info->vendor_name, sizeof(info->vendor_name)) == CHIP_NO_ERROR;
    ok &= p->GetProductName(info->product_name, sizeof(info->product_name)) == CHIP_NO_ERROR;
    ok &= p->GetVendorId(info->vendor_id) == CHIP_NO_ERROR;
    ok &= p->GetProductId(info->product_id) == CHIP_NO_ERROR;
    return ok;
}
