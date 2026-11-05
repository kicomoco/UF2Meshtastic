// Replacement for the pico-sdk's pico_btstack_cyw43 glue.
//
// The stock implementation keeps BTstack's TLV (bonding keys) in the last
// sectors of the flash. Under the PicoCalc UF2 loader the application must not
// write to flash, so the TLV is kept in RAM and mirrored to the SD card.
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_tlv.h"
#include "hci.h"
#include "ble/le_device_db_tlv.h"

#include "pico/btstack_hci_transport_cyw43.h"
#include "pico/btstack_run_loop_async_context.h"
#include "pico/btstack_cyw43.h"

#include "storage.h"

bool btstack_cyw43_init(async_context_t *context) {
    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_async_context_get_instance(context));
    hci_init(hci_transport_cyw43_instance(), NULL);

    const btstack_tlv_t *tlv = storage_tlv_instance();
    void *ctx = storage_tlv_context();
    btstack_tlv_set_instance(tlv, ctx);
    le_device_db_tlv_configure(tlv, ctx);
    return true;
}

void btstack_cyw43_deinit(__unused async_context_t *context) {
    hci_power_control(HCI_POWER_OFF);
    hci_close();
    btstack_run_loop_async_context_deinit();
    btstack_run_loop_deinit();
    btstack_memory_deinit();
}
