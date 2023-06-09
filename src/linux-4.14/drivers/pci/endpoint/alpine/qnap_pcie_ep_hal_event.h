#ifndef __QNAP_PCIE_EP_HAL_EVENT_H
#define __QNAP_PCIE_EP_HAL_EVENT_H

struct cable_hal_event_status {
	unsigned int modules;
	unsigned int port;
	unsigned int type;
	unsigned int cable_status;
};

extern void qnap_send_ep_cable_hal_event(struct cable_hal_event_status *ches);

#endif /* __QNAP_PCIE_EP_HAL_EVENT_H */
