#include <zephyr/kernel.h>
#include <zephyr/stats/stats.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/drivers/hwinfo.h>
#include <stdbool.h>

#include "common.h"

#define LOG_LEVEL LOG_LEVEL_DBG
LOG_MODULE_REGISTER(security_demo);

#ifdef CONFIG_TASK_WDT
#include <zephyr/task_wdt/task_wdt.h>
#define WDT_NODE DT_ALIAS(watchdog0)
#endif

#define STORAGE_PARTITION_LABEL storage_partition
#define STORAGE_PARTITION_ID    PARTITION_ID(STORAGE_PARTITION_LABEL)
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(cstorage);
static struct fs_mount_t littlefs_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &cstorage,
    .storage_dev = (void *)STORAGE_PARTITION_ID,
    .mnt_point = "/lfs1"
};

void print_deviceid()
{
    uint8_t id[16];   /* buffer for the ID */
    ssize_t len;

    /* Passing NULL defaults to the "system device" (SoC) */
    len = hwinfo_get_device_id(id, sizeof(id));

    if (len > 0)
    {
        printk("Device ID (len=%d): ", (int)len);
        for (int i = 0; i < len; i++)
        {
            printk("%02x", id[i]);
        }
        printk("\n");
    }
    else
    {
        printk("Device ID not available on this platform\n");
    }
}

int main(void)
{
    int ret;
#ifdef CONFIG_TASK_WDT
    const struct device *const hw_wdt_dev = DEVICE_DT_GET_OR_NULL(WDT_NODE);

    if (!device_is_ready(hw_wdt_dev))
    {
        LOG_ERR("Hardware watchdog not ready; ignoring it.\n");
        ret = task_wdt_init(NULL);
    }
    else
    {
        ret = task_wdt_init(hw_wdt_dev);
    }
    int task_wdt_id = task_wdt_add(10000U, NULL, NULL);
#endif

    print_deviceid();
    ret = fs_mount(&littlefs_mnt);
    if (ret < 0) {
        LOG_ERR("Error mounting littlefs [%d]", ret);
    }
    init_wifi();
    start_mqtt_client();
	LOG_INF("build time: " __DATE__ " " __TIME__);

	while (1)
    {
#ifdef CONFIG_TASK_WDT
        task_wdt_feed(task_wdt_id);
#endif
		k_sleep(K_MSEC(1500));
	}
	return 0;
}
