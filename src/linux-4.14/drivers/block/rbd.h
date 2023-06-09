#ifndef __RBD_H
#define __RBD_H

struct rbd_device;

/* rbd.c helpers */
void rbd_warn(struct rbd_device *rbd_dev, const char *fmt, ...);
void rbd_acknowledge_notify(struct rbd_device *rbd_dev, u64 notify_id,
				   u64 cookie);
int rbd_acknowledge_notify_scsi_event(struct rbd_device *rbd_dev,
					  u64 notify_op, u32 notify_timeout);
extern int rbd_attach_tcm_dev(struct rbd_device *rbd_dev, void *data);
extern int rbd_detach_tcm_dev(struct rbd_device *rbd_dev);

#if defined(CONFIG_TCM_IBLOCK) || defined(CONFIG_TCM_IBLOCK_MODULE)
extern void rbd_tcm_reset_notify_handle(void *data, u64 notify_id, u64 cookie);
extern int rbd_tcm_register(void);
extern void rbd_tcm_unregister(void);
#else
void rbd_tcm_reset_notify_handle(void *data, u64 notify_id)
{
}
int rbd_tcm_register(void)
{
	return 0;
}
void rbd_tcm_unregister(void)
{
}
#endif /* CONFIG_TCM_IBLOCK || CONFIG_TCM_IBLOCK_MODULE */

#endif /* __RBD_H */
