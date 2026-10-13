// SPDX-License-Identifier: GPL-2.0
/*
 * honor_fan - fan monitoring and control for HONOR MagicBook Pro 16 (DRA-XX)
 *
 * The firmware exposes no usable fan interface: the ACPI PNP0C0B fans are
 * decoration (their control method \_SB.PC00.LPCB.UPFS is an empty stub) and
 * no fan hwmon exists.  The EC does carry tachometers and a manual-speed
 * override, but only in *extended* EC RAM, reachable through a memory-mapped
 * command mailbox.
 *
 * Protocol recovered from this machine's own DSDT (methods RDER / WTER /
 * GFNS / SFCS).  See docs/ec_protocol.md.
 *
 *   mailbox @ 0xFE0B0400   CMDB STAT NUMB DAT0..DAT4 DATR DAR1 DAR2
 *   EC RAM mirror @ 0xFE0B0300  (temperature probes TP00..TP0A at 0x20..0x2B)
 *
 * The AML declares both regions QWordAcc + Lock, so we use 64-bit
 * read-modify-write accesses and hold the ACPI global lock, exactly as the
 * interpreter would, to stay race-free against firmware AML.
 */

#define pr_fmt(fmt) "honor_fan: " fmt

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#define ECRAM_PHYS	0xFE0B0300UL
#define ECRAM_LEN	0x100
#define MBOX_PHYS	0xFE0B0400UL
#define MBOX_LEN	0x80

/* mailbox register offsets */
#define MB_CMDB		0x00
#define MB_STAT		0x01
#define MB_NUMB		0x02
#define MB_DAT0		0x03
#define MB_DAT1		0x04
#define MB_DAT2		0x05
#define MB_DAT3		0x06
#define MB_DATR		0x08

/* mailbox commands */
#define CMD_READ	0x80
#define CMD_WRITE	0x81
#define CMD_UNLOCK	0x29
#define UNLOCK_MAGIC	0x4D

/* extended EC addresses (from GFNS / SFCS) */
#define EXT_FAN0_LO	0x20020A00
#define EXT_FAN0_HI	0x20020A01
#define EXT_FAN1_LO	0x20020A02
#define EXT_FAN1_HI	0x20020A03
#define EXT_FAN0_GEAR	0x20020A04
#define EXT_FAN1_GEAR	0x20020A05
#define EXT_FSUD	0x20020A1B	/* manual-mode selector */
#define EXT_FSVL	0x20020A1C	/* manual speed value   */

/* plain EC RAM */
#define ECRAM_TP00	0x20		/* TP00..TP09 = 0x20..0x29, TP0A = 0x2B */
#define ECRAM_ZNCJ	0x79		/* secondary fan handle, 0xA0 = auto    */

static int fsvl_max = 42;
module_param(fsvl_max, int, 0644);
MODULE_PARM_DESC(fsvl_max,
	"FSVL for pwm 255; target rpm = (16 + FSVL) * 100, so 42 => ~5800 rpm (default 42)");

static bool enable_control;
module_param(enable_control, bool, 0444);
MODULE_PARM_DESC(enable_control,
	"Allow writing fan speed (default 0 = monitoring only). The EC loses sync under sustained write traffic and can leave the fans stopped until left idle ~40s; see README.");

static int write_delay_ms = 150;
module_param(write_delay_ms, int, 0644);
MODULE_PARM_DESC(write_delay_ms,
	"Delay after each EC write, ms (default 150). Lower values desync the EC.");

static int fsud_manual = 1;
module_param(fsud_manual, int, 0644);
MODULE_PARM_DESC(fsud_manual, "FSUD token that selects manual mode (default 1)");

static bool fw_path = true;
module_param(fw_path, bool, 0644);
MODULE_PARM_DESC(fw_path,
	"Program fans via the firmware \\SFCS method instead of raw mailbox pokes (default 1)");

static int release_step_ms = 8000;
module_param(release_step_ms, int, 0644);
MODULE_PARM_DESC(release_step_ms,
	"Settle time between the steps of a staged manual->auto release, ms (default 8000)");

struct honor_fan {
	struct device *hwmon;
	void __iomem *mb;
	void __iomem *ecram;
	struct mutex lock;	/* serialises mailbox transactions */
	u8 pwm;
	bool manual;
	u8 last_fsud, last_fsvl;	/* what the EC was last told */
	bool programmed;
};

static struct platform_device *honor_fan_pdev;

/* --- mailbox primitives: 64-bit RMW, matching AML QWordAcc semantics ----- */

static u8 ecram_read8(struct honor_fan *hf, unsigned int off)
{
	unsigned int shift = (off & 7u) * 8;

	return (readq(hf->ecram + (off & ~7u)) >> shift) & 0xFF;
}

static u8 mb_read8(struct honor_fan *hf, unsigned int off)
{
	unsigned int shift = (off & 7u) * 8;

	return (readq(hf->mb + (off & ~7u)) >> shift) & 0xFF;
}

static void mb_write8(struct honor_fan *hf, unsigned int off, u8 val)
{
	unsigned int qoff = off & ~7u;
	unsigned int shift = (off & 7u) * 8;
	u64 v = readq(hf->mb + qoff);

	v &= ~(0xFFULL << shift);
	v |= (u64)val << shift;
	writeq(v, hf->mb + qoff);
}

/* Poll CMDB until the EC clears it.  AML uses Sleep(2ms) steps. */
static int mb_wait(struct honor_fan *hf, unsigned int tries)
{
	while (tries--) {
		if (mb_read8(hf, MB_CMDB) == 0)
			return 0;
		usleep_range(1800, 2400);
	}
	return -ETIMEDOUT;
}

static void mb_set_addr(struct honor_fan *hf, u32 addr)
{
	mb_write8(hf, MB_DAT0, (addr >> 24) & 0xFF);
	mb_write8(hf, MB_DAT1, (addr >> 16) & 0xFF);
	mb_write8(hf, MB_DAT2, (addr >> 8) & 0xFF);
	mb_write8(hf, MB_DAT3, addr & 0xFF);
}

/* RDER */
static int ec_ext_read(struct honor_fan *hf, u32 addr, u8 *out)
{
	u32 glk;
	int ret;

	if (ACPI_FAILURE(acpi_acquire_global_lock(0x7D0, &glk)))
		return -EBUSY;

	mb_set_addr(hf, addr);
	mb_write8(hf, MB_NUMB, 1);
	mb_write8(hf, MB_CMDB, CMD_READ);

	ret = mb_wait(hf, 0x100);
	if (!ret)
		*out = mb_read8(hf, MB_DATR);

	acpi_release_global_lock(glk);
	return ret;
}

/* WTER: unlock handshake, then the byte write */
static int ec_ext_write(struct honor_fan *hf, u32 addr, u8 val)
{
	u32 glk;
	int ret;

	if (ACPI_FAILURE(acpi_acquire_global_lock(0x7D0, &glk)))
		return -EBUSY;

	mb_write8(hf, MB_DATR, UNLOCK_MAGIC);
	mb_write8(hf, MB_CMDB, CMD_UNLOCK);
	ret = mb_wait(hf, 0x1000);
	if (ret)
		goto out;

	mb_set_addr(hf, addr);
	mb_write8(hf, MB_DATR, val);
	mb_write8(hf, MB_NUMB, 1);
	mb_write8(hf, MB_CMDB, CMD_WRITE);
	ret = mb_wait(hf, 0x1000);
out:
	acpi_release_global_lock(glk);
	/*
	 * The EC loses sync if writes arrive back-to-back: it starts
	 * ignoring FSUD/FSVL and leaves the fans stopped.  Writes issued
	 * from a shell ~150 ms apart never failed, 10 ms apart did, so pace
	 * generously -- fan changes are not latency-sensitive.
	 */
	msleep(write_delay_ms);
	return ret;
}

/* --- firmware method path: \SFCS / \IFCI / \GSFV ------------------------ */
/*
 * DSDT layout, dissected on DRA-XX with `iasl -d` (all root-level, none
 * called by the firmware itself -- they are OS-facing interfaces):
 *
 *   \SFCS(buf): byte[2]=FSUD, word[3]=FSVL -> WTER(0x20020A1B) + WTER(0x20020A1C)
 *   \IFCI(buf): byte[2]=SFAN -> ECWT to ZNCJ (plain EC RAM offset 0x79)
 *   \GSFV(buf): returns RFAN=ZNCJ as BUFF byte 1
 *   _QF8 (EC query, fired by the EC itself): ECWT(0xA0, ZNCJ) + Notify(IETM)
 *               -- the EC unilaterally hands fan ownership back to firmware.
 *
 * Evaluating \SFCS reuses the firmware's own ECMT mutex and Sleep(2ms)
 * pacing, which raw mailbox pokes bypass.  All three return a 256-byte
 * BUFF with STAT at byte 0 (0 = ok).
 */
static int fw_call(const char *path, u8 b2, u8 wlo, u8 *rfan)
{
	union acpi_object arg;
	struct acpi_object_list args;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	u8 in[5] = { 0, 0, b2, wlo, 0 };
	int ret = 0;

	arg.type = ACPI_TYPE_BUFFER;
	arg.buffer.length = sizeof(in);
	arg.buffer.pointer = in;
	args.count = 1;
	args.pointer = &arg;

	status = acpi_evaluate_object(NULL, (acpi_string)path, &args, &output);
	if (ACPI_FAILURE(status)) {
		pr_warn("%s: evaluation failed: %s\n", path, acpi_format_exception(status));
		return -EIO;
	}

	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER || obj->buffer.length < 2) {
		pr_warn("%s: malformed return buffer\n", path);
		ret = -EIO;
		goto out;
	}
	if (obj->buffer.pointer[0] != 0) {
		pr_warn("%s: firmware returned STAT=0x%02X\n", path, obj->buffer.pointer[0]);
		ret = -EIO;
		goto out;
	}
	if (rfan)
		*rfan = obj->buffer.pointer[1];
out:
	ACPI_FREE(obj);
	return ret;
}

/* Single extended-EC byte write through the firmware's own WTER, falling
 * back to a raw mailbox poke when fw_path is off.  WTER takes two integer
 * args (address, value) and returns 0 on success. */
static int fw_wter(struct honor_fan *hf, u32 addr, u8 val)
{
	union acpi_object args_obj[2];
	struct acpi_object_list args;
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object *obj;
	acpi_status status;
	int ret = -EIO;

	if (!fw_path)
		return ec_ext_write(hf, addr, val);

	args_obj[0].type = ACPI_TYPE_INTEGER;
	args_obj[0].integer.value = addr;
	args_obj[1].type = ACPI_TYPE_INTEGER;
	args_obj[1].integer.value = val;
	args.count = 2;
	args.pointer = args_obj;

	status = acpi_evaluate_object(NULL, "\\_SB.PC00.LPCB.H_EC.WTER",
				      &args, &output);
	if (ACPI_FAILURE(status)) {
		pr_warn("WTER evaluation failed: %s\n", acpi_format_exception(status));
		return -EIO;
	}

	obj = output.pointer;
	if (obj && obj->type == ACPI_TYPE_INTEGER && obj->integer.value == 0)
		ret = 0;
	else
		pr_warn("WTER(0x%08X, 0x%02X) returned failure\n", addr, val);
	ACPI_FREE(obj);
	return ret;
}

static int fan_rpm(struct honor_fan *hf, int idx, long *val)
{
	u8 lo, hi;
	int ret;

	ret = ec_ext_read(hf, idx ? EXT_FAN1_LO : EXT_FAN0_LO, &lo);
	if (ret)
		return ret;
	ret = ec_ext_read(hf, idx ? EXT_FAN1_HI : EXT_FAN0_HI, &hi);
	if (ret)
		return ret;

	*val = lo | (hi << 8);
	return 0;
}

/*
 * One FSUD/FSVL programming pass for manual targets.  Both registers are
 * always written together; a stale FSVL is what latches the EC with the
 * fans stopped.  (The manual->auto release path in apply_pwm is exempt: it
 * zeroes FSVL first, then FSUD, with a settle between the two steps.)
 */
static int program_fan(struct honor_fan *hf, u8 fsud, u8 fsvl)
{
	int ret;

	if (fw_path) {
		/* One interpreter call programs both bytes with the
		 * firmware's own ECMT locking and Sleep(2ms) pacing. */
		ret = fw_call("\\SFCS", fsud, fsvl, NULL);
	} else {
		ret = ec_ext_write(hf, EXT_FSUD, fsud);
		if (ret)
			return ret;
		ret = ec_ext_write(hf, EXT_FSVL, fsvl);
	}
	if (ret)
		return ret;

	hf->last_fsud = fsud;
	hf->last_fsvl = fsvl;
	hf->programmed = true;
	msleep(write_delay_ms);
	return 0;
}

static int apply_pwm(struct honor_fan *hf)
{
	u8 fsud = 0, fsvl = 0;

	/*
	 * FSVL is an increment above the fan's idle base, in units of
	 * 100 rpm: target_rpm = (base + FSVL) * 100, base 16 for fan1 and
	 * 14 for fan2.  pwm 0 therefore means idle (~1600 rpm), never off.
	 */
	if (hf->manual) {
		fsud = fsud_manual;
		fsvl = DIV_ROUND_CLOSEST((int)hf->pwm * fsvl_max, 255);
	}

	/*
	 * Every EC transaction is a chance to desync it, so do nothing at
	 * all when the setting has not actually changed.  Userspace writing
	 * pwm1 and pwm1_enable back to back would otherwise reprogram twice.
	 */
	if (hf->programmed && hf->last_fsud == fsud && hf->last_fsvl == fsvl)
		return 0;

	/*
	 * The initial (0,0) is the EC's power-on state; "programming" it
	 * would only spend mailbox transactions for nothing.
	 */
	if (!hf->programmed && fsud == 0 && fsvl == 0) {
		hf->last_fsud = 0;
		hf->last_fsvl = 0;
		hf->programmed = true;
		return 0;
	}

	/*
	 * Program the target directly, in every direction.  In particular
	 * there is deliberately NO release-before-rearm here: overwriting
	 * FSUD/FSVL while manual is accepted by the EC, but a (0,0)
	 * release followed by a quick re-arm wedges it with both fans
	 * stopped.  Likewise there is no readback confirm -- the tachometer
	 * reports the result on its own timescale, and extra mailbox
	 * traffic right after a program only adds desync risk.
	 */
	if (fsud != 0 || !hf->programmed || hf->last_fsud == 0)
		return program_fan(hf, fsud, fsvl);

	/*
	 * Leaving manual mode: step down to idle-manual first, settle,
	 * then hand ownership back, settling again.  A back-to-back (0,0)
	 * pair across the mode transition wedges the EC with both fans
	 * stopped; spaced single-byte steps do not.
	 */
	if (hf->last_fsvl != 0) {
		int ret = fw_wter(hf, EXT_FSVL, 0);

		if (ret)
			return ret;
		hf->last_fsvl = 0;
		msleep(release_step_ms);
	}
	{
		int ret = fw_wter(hf, EXT_FSUD, 0);

		if (ret)
			return ret;
		hf->last_fsud = 0;
		hf->programmed = true;
		msleep(release_step_ms);
		return 0;
	}
}

/* --- hwmon ------------------------------------------------------------- */

static const char *const temp_labels[] = {
	"CPU", "SEN2", "TP02", "SEN5", "SEN3",
	"SEN8", "SEN7", "TP07", "SEN6", "SEN4", "SEN1",
};

static umode_t honor_fan_is_visible(const void *data, enum hwmon_sensor_types type,
				    u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		return 0444;
	case hwmon_temp:
		return 0444;
	case hwmon_pwm:
		return enable_control ? 0644 : 0444;
	default:
		return 0;
	}
}

static int honor_fan_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	struct honor_fan *hf = dev_get_drvdata(dev);
	int ret = 0;
	u8 raw;

	mutex_lock(&hf->lock);

	switch (type) {
	case hwmon_fan:
		ret = fan_rpm(hf, channel, val);
		break;
	case hwmon_temp:
		/* TP00..TP09 are contiguous; TP0A sits at 0x2B, skipping 0x2A */
		raw = ecram_read8(hf, ECRAM_TP00 + (channel == 10 ? 11 : channel));
		*val = (s8)raw * 1000;
		break;
	case hwmon_pwm:
		if (attr == hwmon_pwm_enable)
			*val = hf->manual ? 1 : 2;
		else
			*val = hf->pwm;
		break;
	default:
		ret = -EOPNOTSUPP;
	}

	mutex_unlock(&hf->lock);
	return ret;
}

static int honor_fan_read_string(struct device *dev, enum hwmon_sensor_types type,
				 u32 attr, int channel, const char **str)
{
	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;
	*str = temp_labels[channel];
	return 0;
}

static int honor_fan_write(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long val)
{
	struct honor_fan *hf = dev_get_drvdata(dev);
	int ret;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;
	if (!enable_control)
		return -EPERM;

	switch (attr) {
	case hwmon_pwm_enable:
		if (val != 1 && val != 2)
			return -EINVAL;
		break;
	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;
		break;
	default:
		return -EOPNOTSUPP;
	}

	mutex_lock(&hf->lock);

	if (attr == hwmon_pwm_enable)
		hf->manual = (val == 1);
	else
		hf->pwm = val;

	ret = apply_pwm(hf);
	mutex_unlock(&hf->lock);
	return ret;
}

static const struct hwmon_channel_info *const honor_fan_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(temp,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
		HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops honor_fan_hwmon_ops = {
	.is_visible = honor_fan_is_visible,
	.read = honor_fan_read,
	.read_string = honor_fan_read_string,
	.write = honor_fan_write,
};

static const struct hwmon_chip_info honor_fan_chip_info = {
	.ops = &honor_fan_hwmon_ops,
	.info = honor_fan_info,
};

/* --- raw extended-EC access, for determining the FSVL encoding ---------- */

static u32 dbg_addr = EXT_FAN0_LO;

static ssize_t ec_addr_show(struct device *dev, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "0x%08X\n", dbg_addr);
}

static ssize_t ec_addr_store(struct device *dev, struct device_attribute *a,
			     const char *buf, size_t len)
{
	u32 v;

	if (kstrtou32(buf, 0, &v))
		return -EINVAL;
	dbg_addr = v;
	return len;
}

static ssize_t ec_data_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct honor_fan *hf = dev_get_drvdata(dev);
	int ret;
	u8 v;

	mutex_lock(&hf->lock);
	ret = ec_ext_read(hf, dbg_addr, &v);
	mutex_unlock(&hf->lock);

	return ret ? ret : sysfs_emit(buf, "0x%02X\n", v);
}

static ssize_t ec_data_store(struct device *dev, struct device_attribute *a,
			     const char *buf, size_t len)
{
	struct honor_fan *hf = dev_get_drvdata(dev);
	int ret;
	u8 v;

	if (kstrtou8(buf, 0, &v))
		return -EINVAL;

	mutex_lock(&hf->lock);
	ret = ec_ext_write(hf, dbg_addr, v);
	mutex_unlock(&hf->lock);

	return ret ? ret : len;
}

static DEVICE_ATTR_RW(ec_addr);
static DEVICE_ATTR_RW(ec_data);

/* ZNCJ (plain EC 0x79) via \GSFV / \IFCI: readback tells whether the EC
 * fired _QF8 and took fan ownership back (0xA0 = firmware auto). */
static ssize_t zncj_show(struct device *dev, struct device_attribute *a, char *buf)
{
	struct honor_fan *hf = dev_get_drvdata(dev);
	u8 v;
	int ret;

	mutex_lock(&hf->lock);
	ret = fw_call("\\GSFV", 0, 0, &v);
	mutex_unlock(&hf->lock);

	return ret ? ret : sysfs_emit(buf, "0x%02X\n", v);
}

static ssize_t zncj_store(struct device *dev, struct device_attribute *a,
			  const char *buf, size_t len)
{
	struct honor_fan *hf = dev_get_drvdata(dev);
	u8 v, back;
	int ret;

	if (kstrtou8(buf, 0, &v))
		return -EINVAL;

	mutex_lock(&hf->lock);
	ret = fw_call("\\IFCI", v, 0, &back);
	mutex_unlock(&hf->lock);
	if (ret)
		return ret;
	if (back != v)
		pr_warn("IFCI(0x%02X) read back 0x%02X\n", v, back);
	msleep(write_delay_ms);
	return len;
}

static DEVICE_ATTR_RW(zncj);

static struct attribute *honor_fan_attrs[] = {
	&dev_attr_ec_addr.attr,
	&dev_attr_ec_data.attr,
	&dev_attr_zncj.attr,
	NULL
};
ATTRIBUTE_GROUPS(honor_fan);

/* --- probe ------------------------------------------------------------- */

static int honor_fan_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct honor_fan *hf;
	long rpm0 = -1, rpm1 = -1;

	hf = devm_kzalloc(dev, sizeof(*hf), GFP_KERNEL);
	if (!hf)
		return -ENOMEM;

	mutex_init(&hf->lock);
	hf->pwm = 255;
	hf->manual = false;

	hf->mb = devm_ioremap(dev, MBOX_PHYS, MBOX_LEN);
	hf->ecram = devm_ioremap(dev, ECRAM_PHYS, ECRAM_LEN);
	if (!hf->mb || !hf->ecram) {
		dev_err(dev, "failed to map EC windows\n");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, hf);
	dev_set_drvdata(dev, hf);

	fan_rpm(hf, 0, &rpm0);
	fan_rpm(hf, 1, &rpm1);
	dev_info(dev, "EC mailbox online; fan0=%ld rpm fan1=%ld rpm\n", rpm0, rpm1);

	hf->hwmon = devm_hwmon_device_register_with_info(dev, "honor_fan", hf,
							 &honor_fan_chip_info, NULL);
	return PTR_ERR_OR_ZERO(hf->hwmon);
}

static void honor_fan_remove(struct platform_device *pdev)
{
	struct honor_fan *hf = platform_get_drvdata(pdev);

	/*
	 * Only touch the EC if we actually took manual control.  Every write
	 * is a mailbox transaction, and issuing them needlessly on unload is
	 * itself enough to desync the EC and stop the fans.
	 */
	mutex_lock(&hf->lock);
	if (hf->manual) {
		hf->manual = false;
		apply_pwm(hf);
	}
	mutex_unlock(&hf->lock);
}

static struct platform_driver honor_fan_driver = {
	.driver = {
		.name = "honor_fan",
		.dev_groups = honor_fan_groups,
	},
	.probe = honor_fan_probe,
	.remove = honor_fan_remove,
};

static const struct dmi_system_id honor_fan_dmi[] = {
	{ .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "HONOR"),
		DMI_MATCH(DMI_PRODUCT_NAME, "DRA-XX"),
	} },
	{ }
};
MODULE_DEVICE_TABLE(dmi, honor_fan_dmi);

static bool force;
module_param(force, bool, 0444);
MODULE_PARM_DESC(force, "skip the DMI model check");

static int __init honor_fan_init(void)
{
	int ret;

	if (!dmi_check_system(honor_fan_dmi) && !force) {
		pr_info("unsupported model; load with force=1 to override\n");
		return -ENODEV;
	}

	ret = platform_driver_register(&honor_fan_driver);
	if (ret)
		return ret;

	honor_fan_pdev = platform_device_register_simple("honor_fan", -1, NULL, 0);
	if (IS_ERR(honor_fan_pdev)) {
		platform_driver_unregister(&honor_fan_driver);
		return PTR_ERR(honor_fan_pdev);
	}
	return 0;
}

static void __exit honor_fan_exit(void)
{
	platform_device_unregister(honor_fan_pdev);
	platform_driver_unregister(&honor_fan_driver);
}

module_init(honor_fan_init);
module_exit(honor_fan_exit);

MODULE_DESCRIPTION("Fan monitoring/control for HONOR MagicBook Pro 16 (DRA-XX)");
MODULE_LICENSE("GPL");
