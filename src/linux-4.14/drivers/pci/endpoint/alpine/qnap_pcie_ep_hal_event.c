#include <linux/types.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/printk.h>

#include <qnap/hal_event.h>

#include <asm/io.h>

#include "qnap_pcie_ep_hal_event.h"

void qnap_send_ep_cable_hal_event(struct cable_hal_event_status *ches)
{
	NETLINK_EVT hal_event;
	struct __trct_cable_current_status_action *cs;
	int ret;

	memset(&hal_event, 0, sizeof(NETLINK_EVT));

#if 1
	pr_info("ep/cable: report qnap hal event: type = HAL_EVENT_ENCLOSURE, ches->modules = 0x%x, ches->port = 0x%x, ches->type = 0x%x, ches->cable_status = 0x%x\n",
										ches->modules, ches->port, ches->type, ches->cable_status);
#endif

	hal_event.type = HAL_EVENT_ENCLOSURE;
	hal_event.arg.action = TRCT_CABLE_CURRENT_STATUS;
	cs = &(hal_event.arg.param.trct_cable_current_status_action);
#if 1
	cs->modules = ches->modules;
	cs->port = ches->port;
	cs->type = ches->type;
	cs->cable_status = ches->cable_status;
#endif
	send_hal_netlink(&hal_event);

	return;
}
