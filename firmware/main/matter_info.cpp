#include "matter_info.h"

#include <atomic>

#include <app/server/Server.h>
#include <esp_matter.h> /* kCommissioningSession* events */
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

std::atomic<bool> s_pairing;
std::atomic<unsigned> s_completions;
} // namespace

/* Called on the CHIP task. Pairing runs from the setup (PASE) session until commissioning completes
 * or the fail-safe expires. The session itself ends before completion on Thread (Home finishes over
 * Thread), so its end only counts when the fail-safe was never armed (cancelled early). */
void matter_info_on_event(const DeviceLayer::ChipDeviceEvent *event)
{
    switch (event->Type) {
    case DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        s_pairing = true;
        break;
    case DeviceLayer::DeviceEventType::kCommissioningComplete:
        s_pairing = false;
        s_completions++;
        break;
    case DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        s_pairing = false;
        break;
    case DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        if (!Server::GetInstance().GetFailSafeContext().IsFailSafeArmed()) {
            s_pairing = false;
        }
        break;
    default:
        break;
    }
}

extern "C" bool matter_pairing_active(void)
{
    return s_pairing;
}

extern "C" unsigned matter_pairing_completions(void)
{
    return s_completions;
}

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
