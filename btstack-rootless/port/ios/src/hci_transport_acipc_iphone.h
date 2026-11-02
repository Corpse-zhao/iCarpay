/*
 * Apple ConvergedIPC (PCIe) HCI transport for iPhone 11 and later.
 *
 * These devices do not expose the legacy `com.apple.uart.bluetooth` netgraph
 * node; the Bluetooth controller sits behind `bluetooth-pcie` and is driven
 * through AppleConvergedTransport.dylib, exactly like BlueTool does.
 */

#ifndef HCI_TRANSPORT_ACIPC_IPHONE_H
#define HCI_TRANSPORT_ACIPC_IPHONE_H

#if defined __cplusplus
extern "C" {
#endif

#include "hci_transport.h"

/* 1 when /usr/lib/AppleConvergedTransport.dylib exists and exports the API */
int hci_transport_acipc_iphone_present(void);

/* 1 when the library is usable (same check, kept for symmetry with skywalk) */
int hci_transport_acipc_iphone_available(void);

const hci_transport_t *hci_transport_acipc_iphone_instance(void);

#if defined __cplusplus
}
#endif

#endif
