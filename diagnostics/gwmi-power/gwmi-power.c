// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Тестовый драйвер для интерфейса "GWMI" (Thunderobot R16 / Bitland).
 *
 * Legacy WMI data block 644C5791-B7B0-4123-A90B-E93876E0DAAD.
 *   WSAA(instance, buf[32])  - запись/команда
 *   WQAA(instance)           - чтение результата
 *
 * Раскладка буфера (little-endian):
 *   [0..1]  GWF0  - 0xFA00 = чтение, 0xFB00 = запись
 *   [2..3]  GWF1  - функция (0x0206 - режим OMCC, 0x0300 - режим APMD, ...)
 *   [4..7]  GWD0  - аргумент 0
 *   [8..11] GWD2  - аргумент 1
 *
 * Используется только для диагностики: параметр mode=N выставляет режим
 * 0/1/2 через штатный ACPI-путь.
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/wmi.h>
#include <linux/unaligned.h>

#define DRV_NAME	"gwmi-power"
#define GWMI_GUID	"644C5791-B7B0-4123-A90B-E93876E0DAAD"

#define GWMI_GET	0xFA00
#define GWMI_SET	0xFB00
#define GWMI_FN_SYSTEM_PER_MODE	0x0206
#define GWMI_FN_POWER_MODE	0x0300

static int mode = -1;
module_param(mode, int, 0444);
MODULE_PARM_DESC(mode, "Режим для установки: 0, 1, 2 (-1 = только чтение)");

static int gwmi_call(struct wmi_device *wdev, u16 op, u16 fn,
		     u32 d0, u32 d1, u8 out[32])
{
	struct wmi_buffer in;
	struct wmi_buffer ob = { 0 };
	u8 buf[32] = { 0 };
	int ret;

	put_unaligned_le16(op, &buf[0]);
	put_unaligned_le16(fn, &buf[2]);
	put_unaligned_le32(d0, &buf[4]);
	put_unaligned_le32(d1, &buf[8]);

	in.length = sizeof(buf);
	in.data = buf;

	ret = wmidev_set_block(wdev, 0, &in);
	if (ret) {
		dev_warn(&wdev->dev, "set_block %04x/%04x: %d\n", op, fn, ret);
		return ret;
	}

	ret = wmidev_query_block(wdev, 0, &ob, 32);
	if (ret) {
		dev_warn(&wdev->dev, "query_block: %d\n", ret);
		return ret;
	}

	memcpy(out, ob.data, 32);
	kfree(ob.data);

	return 0;
}

static void gwmi_dump(struct wmi_device *wdev, const char *tag, u16 op, u16 fn)
{
	u8 out[32];
	int ret;

	ret = gwmi_call(wdev, op, fn, 0, 0, out);
	if (ret)
		return;

	dev_info(&wdev->dev, "%s: ret=%d [%08x %08x %08x %08x %08x %08x]\n",
		 tag, ret,
		 get_unaligned_le32(&out[4]), get_unaligned_le32(&out[8]),
		 get_unaligned_le32(&out[12]), get_unaligned_le32(&out[16]),
		 get_unaligned_le32(&out[20]), get_unaligned_le32(&out[24]));
}

static int gwmi_power_probe(struct wmi_device *wdev, const void *context)
{
	u8 out[32];
	int ret;

	dev_info(&wdev->dev, "probe entry, mode=%d\n", mode);

	gwmi_dump(wdev, "SystemPerMode", GWMI_GET, GWMI_FN_SYSTEM_PER_MODE);
	gwmi_dump(wdev, "PowerMode", GWMI_GET, GWMI_FN_POWER_MODE);
	gwmi_dump(wdev, "Sensors", GWMI_GET, 0x0200);
	gwmi_dump(wdev, "Sensors207", GWMI_GET, 0x0207);
	gwmi_dump(wdev, "FanCurve", GWMI_GET, 0x0205);

	if (mode < 0)
		return 0;

	if (mode > 2) {
		dev_err(&wdev->dev, "mode must be 0..2\n");
		return -EINVAL;
	}

	ret = gwmi_call(wdev, GWMI_SET, GWMI_FN_POWER_MODE, mode, 0, out);
	if (ret)
		return ret;

	dev_info(&wdev->dev, "SET PowerMode(%d) done\n", mode);
	gwmi_dump(wdev, "PowerMode after", GWMI_GET, GWMI_FN_POWER_MODE);
	gwmi_dump(wdev, "SystemPerMode after", GWMI_GET, GWMI_FN_SYSTEM_PER_MODE);

	return 0;
}

static const struct wmi_device_id gwmi_power_id_table[] = {
	{ GWMI_GUID, NULL },
	{}
};
MODULE_DEVICE_TABLE(wmi, gwmi_power_id_table);

static struct wmi_driver gwmi_power_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.id_table = gwmi_power_id_table,
	.probe = gwmi_power_probe,
};

module_wmi_driver(gwmi_power_driver);

MODULE_AUTHOR("diagnostic");
MODULE_DESCRIPTION("GWMI power mode probe for Thunderobot R16");
MODULE_LICENSE("GPL");
