// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Driver for the GWMI ("gaming WMI") interface found on some Bitland
 * laptops, including the Thunderobot Radiant R16 (Hunter Blade 16).
 *
 * The firmware exposes a legacy ACPI-WMI data block with GUID
 * 644C5791-B7B0-4123-A90B-E93876E0DAAD.  Data is exchanged through the
 * WSAA()/WQAA() methods using a fixed 32-byte buffer:
 *
 *   offset 0  u16  command  - 0xFA00 read, 0xFB00 write
 *   offset 2  u16  function - see GWMI_FN_*
 *   offset 4  u32  arg0
 *   offset 8  u32  arg1
 *
 * The same buffer carries the reply, which is fetched with a separate
 * read (WQAA) after every command.
 *
 * The power mode is stored in the embedded controller (APMD, EC offset
 * 0x95) and directly drives the platform power limits, including the
 * dGPU TGP handed to the NVIDIA driver through NVPCF:
 *
 *   0 - performance: ~115 W TGP (+ Dynamic Boost up to the VBIOS limit)
 *   1 - balanced:     ~95 W TGP
 *   2 - low power:    ~80 W TGP
 *
 * Fan speeds are controlled through WMI function 0x0205, which writes the
 * "user fan" levels UFA1/UFA2/UFA3 in the EC (offsets 0x2C/0x2D/0x3E).  The
 * EC value is a direct fan level, not a duty cycle:
 *
 *   0x00         fans stopped
 *   0x10 - 0x64  roughly linear, ~780 - ~4820 rpm
 *   0x65 - 0xFE  clamped by the EC to 0x64 (maximum speed)
 *   0xFF         automatic (EC fan curve), the power-on default
 *
 * hwmon pwm*_input (0..255) is mapped linearly onto 0x10..0x64, so the whole
 * range is useful and manual mode never stops the fans (0 means "slowest").
 * pwm*_enable: 0 = full speed, 1 = manual, 2 = automatic (EC curve).
 *
 * Function 0x0205 has no read counterpart, so the current levels are read
 * back from the EC directly with ec_read().
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_profile.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#define DRV_NAME		"gwmi-wmi"
#define GWMI_CONTROL_GUID	"644C5791-B7B0-4123-A90B-E93876E0DAAD"

/* Commands */
#define GWMI_OP_GET		0xFA00
#define GWMI_OP_SET		0xFB00

/* Functions */
#define GWMI_FN_SENSORS		0x0200
#define GWMI_FN_FAN_LEVEL	0x0205
#define GWMI_FN_POWER_MODE	0x0300

/* Fan levels (EC values written by GWMI_FN_FAN_LEVEL) */
#define GWMI_FAN_CHANNELS	2
#define GWMI_FAN_AUTO		0xff
#define GWMI_FAN_FULL		0xfe
#define GWMI_FAN_MANUAL_MIN	0x10	/* slowest level that keeps the fan spinning */
#define GWMI_FAN_MANUAL_MAX	0x64	/* the EC clamps manual levels to 100 */
#define GWMI_FAN_DEFAULT	0x40	/* ~3000 rpm, used when entering manual */
#define GWMI_FAN_MAX_RPM	4820	/* tach reading at GWMI_FAN_MANUAL_MAX */

/* EC fan level registers: UFA1, UFA2 (UFA3 lives at 0x3e) */
#define EC_FAN_LEVEL_BASE	0x2c
#define EC_FAN_LEVEL3		0x3e

/* Reply buffer offsets */
#define GWMI_DAT0		0
#define GWMI_DAT1		2
#define GWMI_ARG0		4
#define GWMI_DAT2		4
#define GWMI_DAT3		8
#define GWMI_DAT4		12
#define GWMI_DAT5		16
#define GWMI_DAT6		20

#define GWMI_BUF_SIZE		32

enum gwmi_power_mode {
	GWMI_MODE_PERFORMANCE	= 0,
	GWMI_MODE_BALANCED	= 1,
	GWMI_MODE_LOW_POWER	= 2,
};

struct gwmi_wmi_data {
	struct wmi_device *wdev;
	struct mutex lock;	/* serializes the command/reply pairs */
	struct mutex fan_lock;	/* protects pwm[] and enable[] */
	struct device *pp_dev;
	struct device *hwmon_dev;
	u8 pwm[GWMI_FAN_CHANNELS];	/* desired EC fan level */
	u8 enable[GWMI_FAN_CHANNELS];	/* hwmon pwm*_enable: 0 full, 1 manual, 2 auto */
	u8 pwm_user[GWMI_FAN_CHANNELS];	/* last pwm*_input value written, 0..255 */
};

static int gwmi_call(struct gwmi_wmi_data *data, u16 op, u16 fn,
		     u32 arg0, u32 arg1, u32 arg2, u8 reply[GWMI_BUF_SIZE])
{
	struct wmi_buffer in;
	struct wmi_buffer out = { 0 };
	u8 buf[GWMI_BUF_SIZE] = { 0 };
	int ret;

	put_unaligned_le16(op, &buf[GWMI_DAT0]);
	put_unaligned_le16(fn, &buf[GWMI_DAT1]);
	put_unaligned_le32(arg0, &buf[GWMI_ARG0]);
	put_unaligned_le32(arg1, &buf[GWMI_ARG0 + 4]);
	put_unaligned_le32(arg2, &buf[GWMI_ARG0 + 8]);

	in.length = sizeof(buf);
	in.data = buf;

	guard(mutex)(&data->lock);

	ret = wmidev_set_block(data->wdev, 0, &in);
	if (ret)
		return ret;

	ret = wmidev_query_block(data->wdev, 0, &out, GWMI_BUF_SIZE);
	if (ret)
		return ret;

	memcpy(reply, out.data, GWMI_BUF_SIZE);
	kfree(out.data);

	return 0;
}

static int gwmi_get_u32(struct gwmi_wmi_data *data, u16 op, u16 fn,
			u32 arg0, u32 *value)
{
	u8 reply[GWMI_BUF_SIZE];
	int ret;

	ret = gwmi_call(data, op, fn, arg0, 0, 0, reply);
	if (ret)
		return ret;

	*value = get_unaligned_le32(&reply[GWMI_ARG0]);

	return 0;
}

static int gwmi_get_power_mode(struct gwmi_wmi_data *data, u8 *mode)
{
	u32 value;
	int ret;

	ret = gwmi_get_u32(data, GWMI_OP_GET, GWMI_FN_POWER_MODE, 0, &value);
	if (ret)
		return ret;

	if (value > GWMI_MODE_LOW_POWER)
		return -ENODATA;

	*mode = value;

	return 0;
}

static int gwmi_set_power_mode(struct gwmi_wmi_data *data, u8 mode)
{
	u8 reply[GWMI_BUF_SIZE];

	return gwmi_call(data, GWMI_OP_SET, GWMI_FN_POWER_MODE, mode, 0, 0,
			 reply);
}

/*
 * Fan control
 *
 * WMI function 0x0205 takes the EC fan levels in the first byte of arg0
 * (UFA1), arg1 (UFA2) and arg2 (UFA3); UFA3 is kept on "auto".  The EC
 * levels are read back directly because 0x0205 has no read counterpart.
 */

/* Caller must hold data->fan_lock */
static int gwmi_write_fan_levels(struct gwmi_wmi_data *data)
{
	u8 reply[GWMI_BUF_SIZE];

	lockdep_assert_held(&data->fan_lock);

	return gwmi_call(data, GWMI_OP_SET, GWMI_FN_FAN_LEVEL,
			 data->pwm[0], data->pwm[1], GWMI_FAN_AUTO, reply);
}

static int gwmi_read_fan_level(int channel, u8 *level)
{
	int ret = ec_read(EC_FAN_LEVEL_BASE + channel, level);

	return ret < 0 ? ret : 0;
}

/* pwm 0..255 -> EC 0x10..0x64; never returns a level that stops the fan */
static u8 gwmi_pwm_to_ec(long pwm)
{
	return GWMI_FAN_MANUAL_MIN +
	       DIV_ROUND_CLOSEST(pwm * (GWMI_FAN_MANUAL_MAX - GWMI_FAN_MANUAL_MIN),
				 255);
}

static long gwmi_ec_to_pwm(u8 level)
{
	if (level >= GWMI_FAN_MANUAL_MAX)
		return 255;
	if (level <= GWMI_FAN_MANUAL_MIN)
		return 0;

	return DIV_ROUND_CLOSEST((long)(level - GWMI_FAN_MANUAL_MIN) * 255,
				 GWMI_FAN_MANUAL_MAX - GWMI_FAN_MANUAL_MIN);
}

static int gwmi_fan_set_auto(struct gwmi_wmi_data *data)
{
	int i;

	guard(mutex)(&data->fan_lock);

	for (i = 0; i < GWMI_FAN_CHANNELS; i++) {
		data->pwm[i] = GWMI_FAN_AUTO;
		data->enable[i] = 2;
	}

	return gwmi_write_fan_levels(data);
}


static int gwmi_profile_probe(void *drvdata, unsigned long *choices)
{
	set_bit(PLATFORM_PROFILE_LOW_POWER, choices);
	set_bit(PLATFORM_PROFILE_BALANCED, choices);
	set_bit(PLATFORM_PROFILE_PERFORMANCE, choices);

	return 0;
}

static int gwmi_profile_get(struct device *dev,
			    enum platform_profile_option *profile)
{
	struct gwmi_wmi_data *data = dev_get_drvdata(dev);
	u8 mode;
	int ret;

	ret = gwmi_get_power_mode(data, &mode);
	if (ret)
		return ret;

	switch (mode) {
	case GWMI_MODE_LOW_POWER:
		*profile = PLATFORM_PROFILE_LOW_POWER;
		break;
	case GWMI_MODE_BALANCED:
		*profile = PLATFORM_PROFILE_BALANCED;
		break;
	case GWMI_MODE_PERFORMANCE:
		*profile = PLATFORM_PROFILE_PERFORMANCE;
		break;
	default:
		return -ENODATA;
	}

	return 0;
}

static int gwmi_profile_set(struct device *dev,
			    enum platform_profile_option profile)
{
	struct gwmi_wmi_data *data = dev_get_drvdata(dev);
	u8 mode;

	switch (profile) {
	case PLATFORM_PROFILE_LOW_POWER:
		mode = GWMI_MODE_LOW_POWER;
		break;
	case PLATFORM_PROFILE_BALANCED:
		mode = GWMI_MODE_BALANCED;
		break;
	case PLATFORM_PROFILE_PERFORMANCE:
		mode = GWMI_MODE_PERFORMANCE;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return gwmi_set_power_mode(data, mode);
}

static const struct platform_profile_ops gwmi_profile_ops = {
	.probe = gwmi_profile_probe,
	.profile_get = gwmi_profile_get,
	.profile_set = gwmi_profile_set,
};

/*
 * Sensors returned by GWMI_FN_SENSORS:
 *   DAT2 - CPU temperature (hotspot), degrees C
 *   DAT3 - GPU temperature (hotspot), degrees C
 *   DAT4 - fan 1 speed, RPM
 *   DAT5 - fan 2 speed, RPM
 */
static int gwmi_read_sensors(struct gwmi_wmi_data *data,
			     u32 *cpu_temp, u32 *gpu_temp,
			     u32 *fan1, u32 *fan2)
{
	u8 reply[GWMI_BUF_SIZE];
	int ret;

	ret = gwmi_call(data, GWMI_OP_GET, GWMI_FN_SENSORS, 0, 0, 0, reply);
	if (ret)
		return ret;

	*cpu_temp = get_unaligned_le32(&reply[GWMI_DAT2]);
	*gpu_temp = get_unaligned_le32(&reply[GWMI_DAT3]);
	*fan1 = get_unaligned_le32(&reply[GWMI_DAT4]);
	*fan2 = get_unaligned_le32(&reply[GWMI_DAT5]);

	return 0;
}

static umode_t gwmi_hwmon_is_visible(const void *drvdata,
				     enum hwmon_sensor_types type,
				     u32 attr, int channel)
{
	switch (type) {
	case hwmon_temp:
	case hwmon_fan:
		return 0444;
	case hwmon_pwm:
		return 0644;
	default:
		return 0;
	}
}

static int gwmi_pwm_read(struct gwmi_wmi_data *data, u32 attr, int channel,
			 long *val)
{
	u32 cpu_temp, gpu_temp, fan[GWMI_FAN_CHANNELS];
	u8 level;
	int ret;

	ret = gwmi_read_fan_level(channel, &level);
	if (ret)
		return ret;

	if (attr == hwmon_pwm_enable) {
		if (level == GWMI_FAN_AUTO)
			*val = 2;
		else if (level >= GWMI_FAN_FULL)
			*val = 0;
		else
			*val = 1;

		return 0;
	}

	if (level == GWMI_FAN_AUTO) {
		/* EC is in charge: report the output estimated from the tach */
		ret = gwmi_read_sensors(data, &cpu_temp, &gpu_temp,
					&fan[0], &fan[1]);
		if (ret)
			return ret;

		*val = clamp_val(DIV_ROUND_CLOSEST(fan[channel] * 255,
						   GWMI_FAN_MAX_RPM), 0, 255);
		return 0;
	}

	/*
	 * The EC level has fewer steps than pwm 0..255.  Report the value the
	 * user wrote while it still matches the EC, so a write reads back
	 * exactly (monitoring tools verify this).
	 */
	scoped_guard(mutex, &data->fan_lock) {
		if (gwmi_pwm_to_ec(data->pwm_user[channel]) == level) {
			*val = data->pwm_user[channel];
			return 0;
		}
	}

	*val = gwmi_ec_to_pwm(level);

	return 0;
}

static int gwmi_pwm_write(struct gwmi_wmi_data *data, u32 attr, int channel,
			  long val)
{
	u8 old_pwm, old_enable, old_user;
	int ret;

	guard(mutex)(&data->fan_lock);

	old_pwm = data->pwm[channel];
	old_enable = data->enable[channel];
	old_user = data->pwm_user[channel];

	if (attr == hwmon_pwm_input) {
		if (val < 0 || val > 255)
			return -EINVAL;

		data->pwm[channel] = gwmi_pwm_to_ec(val);
		data->pwm_user[channel] = val;
		data->enable[channel] = 1;
	} else if (attr == hwmon_pwm_enable) {
		switch (val) {
		case 0:	/* no control, fans at full speed */
			data->pwm[channel] = GWMI_FAN_FULL;
			data->enable[channel] = 0;
			break;
		case 1:	/* manual, driven by pwm*_input */
			if (data->enable[channel] != 1) {
				data->enable[channel] = 1;
				if (data->pwm[channel] >= GWMI_FAN_FULL)
					data->pwm[channel] = GWMI_FAN_DEFAULT;
			}
			break;
		case 2:	/* automatic, EC fan curve */
			data->pwm[channel] = GWMI_FAN_AUTO;
			data->enable[channel] = 2;
			break;
		default:
			return -EINVAL;
		}
	} else {
		return -EOPNOTSUPP;
	}

	ret = gwmi_write_fan_levels(data);
	if (ret) {
		/* keep the cached state in sync with what the EC really has */
		data->pwm[channel] = old_pwm;
		data->enable[channel] = old_enable;
		data->pwm_user[channel] = old_user;
	}

	return ret;
}

static int gwmi_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long *val)
{
	struct gwmi_wmi_data *data = dev_get_drvdata(dev);
	u32 cpu_temp, gpu_temp, fan1, fan2;
	int ret;

	if (type == hwmon_pwm)
		return gwmi_pwm_read(data, attr, channel, val);

	ret = gwmi_read_sensors(data, &cpu_temp, &gpu_temp, &fan1, &fan2);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_temp:
		*val = (channel == 0 ? cpu_temp : gpu_temp) * 1000;
		return 0;
	case hwmon_fan:
		*val = channel == 0 ? fan1 : fan2;
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int gwmi_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, long val)
{
	struct gwmi_wmi_data *data = dev_get_drvdata(dev);

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	return gwmi_pwm_write(data, attr, channel, val);
}

static const char * const gwmi_temp_labels[] = { "cpu", "gpu" };
static const char * const gwmi_fan_labels[] = { "fan1", "fan2" };

static int gwmi_hwmon_read_string(struct device *dev,
				  enum hwmon_sensor_types type,
				  u32 attr, int channel, const char **str)
{
	switch (type) {
	case hwmon_temp:
		*str = gwmi_temp_labels[channel];
		return 0;
	case hwmon_fan:
		*str = gwmi_fan_labels[channel];
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info * const gwmi_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	NULL
};

static const struct hwmon_ops gwmi_hwmon_ops = {
	.is_visible = gwmi_hwmon_is_visible,
	.read = gwmi_hwmon_read,
	.write = gwmi_hwmon_write,
	.read_string = gwmi_hwmon_read_string,
};

static const struct hwmon_chip_info gwmi_chip_info = {
	.ops = &gwmi_hwmon_ops,
	.info = gwmi_hwmon_info,
};

static int gwmi_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct gwmi_wmi_data *data;
	bool reset_fans = false;
	u8 mode, level;
	int i, ret;

	data = devm_kzalloc(&wdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->wdev = wdev;
	ret = devm_mutex_init(&wdev->dev, &data->lock);
	if (ret)
		return ret;

	ret = devm_mutex_init(&wdev->dev, &data->fan_lock);
	if (ret)
		return ret;

	dev_set_drvdata(&wdev->dev, data);

	/* Make sure this is really the GWMI control block */
	ret = gwmi_get_power_mode(data, &mode);
	if (ret) {
		dev_err(&wdev->dev, "failed to read power mode: %d\n", ret);
		return ret;
	}

	dev_info(&wdev->dev, "power mode: %u\n", mode);

	for (i = 0; i < GWMI_FAN_CHANNELS; i++) {
		data->pwm[i] = GWMI_FAN_AUTO;
		data->enable[i] = 2;

		if (gwmi_read_fan_level(i, &level)) {
			dev_warn(&wdev->dev,
				 "failed to read fan %d level\n", i + 1);
			continue;
		}

		if (level != GWMI_FAN_AUTO)
			reset_fans = true;
	}

	/* A manual fan level left over from a crash must not survive a reload */
	if (reset_fans) {
		dev_info(&wdev->dev, "restoring automatic fan control\n");
		ret = gwmi_fan_set_auto(data);
		if (ret)
			dev_warn(&wdev->dev,
				 "failed to restore automatic fan control: %d\n",
				 ret);
	}

	data->pp_dev = devm_platform_profile_register(&wdev->dev, DRV_NAME,
						      data, &gwmi_profile_ops);
	if (IS_ERR(data->pp_dev))
		return dev_err_probe(&wdev->dev, PTR_ERR(data->pp_dev),
				     "failed to register platform profile\n");

	/* hwmon rejects '-' in chip names */
	data->hwmon_dev = devm_hwmon_device_register_with_info(&wdev->dev,
							       "gwmi_wmi", data,
							       &gwmi_chip_info,
							       NULL);
	if (IS_ERR(data->hwmon_dev))
		return dev_err_probe(&wdev->dev, PTR_ERR(data->hwmon_dev),
				     "failed to register hwmon device\n");

	return 0;
}

static const struct wmi_device_id gwmi_wmi_id_table[] = {
	{ GWMI_CONTROL_GUID, NULL },
	{}
};
MODULE_DEVICE_TABLE(wmi, gwmi_wmi_id_table);

static void gwmi_wmi_remove(struct wmi_device *wdev)
{
	struct gwmi_wmi_data *data = dev_get_drvdata(&wdev->dev);

	gwmi_fan_set_auto(data);
}

static int gwmi_wmi_resume(struct device *dev)
{
	struct gwmi_wmi_data *data = dev_get_drvdata(dev);

	guard(mutex)(&data->fan_lock);

	return gwmi_write_fan_levels(data);
}

static DEFINE_SIMPLE_DEV_PM_OPS(gwmi_wmi_pm_ops, NULL, gwmi_wmi_resume);

static struct wmi_driver gwmi_wmi_driver = {
	.driver = {
		.name = DRV_NAME,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.pm = pm_ptr(&gwmi_wmi_pm_ops),
	},
	.id_table = gwmi_wmi_id_table,
	.probe = gwmi_wmi_probe,
	.remove = gwmi_wmi_remove,
};

module_wmi_driver(gwmi_wmi_driver);

MODULE_AUTHOR("mkondratev");
MODULE_DESCRIPTION("GWMI power profile driver for Bitland/Thunderobot laptops");
MODULE_LICENSE("GPL");