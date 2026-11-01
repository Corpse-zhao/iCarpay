/*
 * Runtime-selected Apple ConvergedIPC/Skywalk HCI transport for iOS.
 *
 * This backend is intentionally discovered at runtime.  Legacy UART devices
 * continue to use hci_transport_h4_iphone.c.
 */

#ifndef HCI_TRANSPORT_SKYWALK_IPHONE_H
#define HCI_TRANSPORT_SKYWALK_IPHONE_H

#include "hci_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

int hci_transport_skywalk_iphone_present(void);
int hci_transport_skywalk_iphone_available(void);
const hci_transport_t *hci_transport_skywalk_iphone_instance(void);

#ifdef __cplusplus
}
#endif

#endif
