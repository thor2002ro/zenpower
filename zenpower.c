/*
 * Based on k10temp by Clemens Ladisch.
 *
 * Docs:
 *   https://www.kernel.org/doc/Documentation/hwmon/hwmon-kernel-api.txt
 *   https://developer.amd.com/wp-content/resources/56255_3_03.PDF
 *
 * Sources:
 *   - Temperature monitoring: k10temp
 *   - SVI2 address and voltage formula: LibreHardwareMonitor
 *   - SVI3 address and formula: community reverse engineering / LHM
 *   - Current formulas, CCD temp addresses: experimental
 *
 * Generation support matrix:
 *   Zen  (17h 0x01, 0x08)         - SVI2 Ryzen/TR/EPYC
 *   Zen  APU (17h 0x11, 0x18)     - SVI2 Raven Ridge / Picasso
 *   Zen2 (17h 0x31)               - SVI2 Threadripper/EPYC (Castle Peak / Rome)
 *   Zen2 APU (17h 0x60, 0x68)     - SVI2 Renoir / Lucienne
 *   Zen2 APU (17h 0x90)           - SVI2 Van Gogh (Steam Deck)
 *   Zen2 (17h 0x71)               - SVI2 Ryzen 3000 (Matisse)
 *   Zen3 SP3/TR (19h 0x00-0x01)   - SVI2 Milan / Chagall
 *   Zen3 APU (19h 0x50)           - SVI2 Cezanne / Barcelo
 *   Zen3 (19h 0x21)               - SVI2 Ryzen 5000 (Vermeer)
 *   Zen3+ APU (19h 0x40, 0x44)    - SVI2 Rembrandt / Rembrandt-R
 *   Zen4 SP3 (19h 0x10-0x11)      - SVI3 Genoa (server)
 *   Zen4 (19h 0x61)               - SVI3 Raphael (Ryzen 7000 desktop)
 *   Zen4 APU (19h 0x70-0x78)      - SVI3 Phoenix / Hawk Point
 *   Zen5 APU (1Ah 0x20-0x24)      - SVI3 Strix Point / Strix Halo
 *   Zen5 (1Ah 0x44)               - SVI3 Granite Ridge (Ryzen 9000 desktop)
 */

#include <asm/amd_nb.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/pci.h>

MODULE_DESCRIPTION("AMD ZEN family CPU Sensors Driver");
MODULE_AUTHOR("Ondrej Čerman");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.2.0");

static bool zen1_calc;
module_param(zen1_calc, bool, 0);
MODULE_PARM_DESC(zen1_calc, "Set to 1 to force ZEN1 SVI2 current calculation");

/* ── PCI Device IDs ─────────────────────────────────────────────────────── */

/* Family 17h – Zen / Zen+ / Zen2 */
#ifndef PCI_DEVICE_ID_AMD_17H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_DF_F3 0x1463 /* Zen SP3 / Naples EPYC   */
#endif
#ifndef PCI_DEVICE_ID_AMD_17H_M10H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_M10H_DF_F3 0x15eb /* Zen+ Raven Ridge APU    */
#endif
#ifndef PCI_DEVICE_ID_AMD_17H_M20H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_M20H_DF_F3 0x15e8 /* Zen  Raven Ridge (alt)  */
#endif
#ifndef PCI_DEVICE_ID_AMD_17H_M30H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_M30H_DF_F3 0x1493 /* Zen2 Castle Peak TR     */
#endif
#ifndef PCI_DEVICE_ID_AMD_17H_M60H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_M60H_DF_F3 0x144b /* Zen2 Renoir APU         */
#endif
#ifndef PCI_DEVICE_ID_AMD_17H_M68H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_M68H_DF_F3 0x1448 /* Zen2 Lucienne APU       */
#endif
#ifndef PCI_DEVICE_ID_AMD_17H_M70H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_M70H_DF_F3 0x1443 /* Zen2 Matisse Ryzen 3000 */
#endif
#ifndef PCI_DEVICE_ID_AMD_17H_M90H_DF_F3
#define PCI_DEVICE_ID_AMD_17H_M90H_DF_F3                                       \
  0x1627 /* Zen2 Van Gogh (Steam Deck)                                         \
          */
#endif

/* Family 19h – Zen3 / Zen3+ / Zen4 */
#ifndef PCI_DEVICE_ID_AMD_19H_DF_F3
#define PCI_DEVICE_ID_AMD_19H_DF_F3 0x1653 /* Zen3 Milan EPYC         */
#endif
#ifndef PCI_DEVICE_ID_AMD_19H_M10H_DF_F3
#define PCI_DEVICE_ID_AMD_19H_M10H_DF_F3 0x14ad /* Zen4 Genoa server       */
#endif
#ifndef PCI_DEVICE_ID_AMD_19H_M21H_DF_F3
#define PCI_DEVICE_ID_AMD_19H_M21H_DF_F3 0x166a /* Zen3 Vermeer Ryzen 5000 */
#endif
#ifndef PCI_DEVICE_ID_AMD_19H_M40H_DF_F3
#define PCI_DEVICE_ID_AMD_19H_M40H_DF_F3 0x167d /* Zen3+ Rembrandt APU     */
#endif
#ifndef PCI_DEVICE_ID_AMD_19H_M50H_DF_F3
#define PCI_DEVICE_ID_AMD_19H_M50H_DF_F3 0x1649 /* Zen3 Cezanne/Barcelo APU*/
#endif
#ifndef PCI_DEVICE_ID_AMD_19H_M61H_DF_F3
#define PCI_DEVICE_ID_AMD_19H_M61H_DF_F3 0x14a4 /* Zen4 Raphael Ryzen 7000 */
#endif
#ifndef PCI_DEVICE_ID_AMD_19H_M70H_DF_F3
#define PCI_DEVICE_ID_AMD_19H_M70H_DF_F3                                       \
  0x14e0 /* Zen4 Phoenix/Hawk Point APU */
#endif

/* Family 1Ah – Zen5 */
#ifndef PCI_DEVICE_ID_AMD_1AH_M20H_DF_F3
#define PCI_DEVICE_ID_AMD_1AH_M20H_DF_F3 0x153a /* Zen5 Strix Point APU    */
#endif
#ifndef PCI_DEVICE_ID_AMD_1AH_M44H_DF_F3
#define PCI_DEVICE_ID_AMD_1AH_M44H_DF_F3                                       \
  0x1533 /* Zen5 Granite Ridge desktop                                         \
          */
#endif

/* ── SMN Register Addresses ─────────────────────────────────────────────── */

#define F17H_M01H_REPORTED_TEMP_CTRL 0x00059800
#define F17H_M01H_SVI 0x0005A000

/* SVI2 telemetry plane registers (various chips) */
#define F17H_M01H_SVI_TEL_PLANE0                                               \
  (F17H_M01H_SVI + 0x0C) /* Zen/Zen+ Ryzen      */
#define F17H_M01H_SVI_TEL_PLANE1 (F17H_M01H_SVI + 0x10)
#define F17H_M30H_SVI_TEL_PLANE0                                               \
  (F17H_M01H_SVI + 0x14) /* Zen2 TR/EPYC        */
#define F17H_M30H_SVI_TEL_PLANE1 (F17H_M01H_SVI + 0x10)
#define F17H_M60H_SVI_TEL_PLANE0                                               \
  (F17H_M01H_SVI + 0x0C) /* Zen2 APU (Renoir)   */
#define F17H_M60H_SVI_TEL_PLANE1 (F17H_M01H_SVI + 0x10)
#define F17H_M70H_SVI_TEL_PLANE0                                               \
  (F17H_M01H_SVI + 0x10) /* Zen2 Ryzen (Matisse)*/
#define F17H_M70H_SVI_TEL_PLANE1 (F17H_M01H_SVI + 0x0C)
/* Zen3 SP3/TR */
#define F19H_M01H_SVI_TEL_PLANE0 (F17H_M01H_SVI + 0x14)
#define F19H_M01H_SVI_TEL_PLANE1 (F17H_M01H_SVI + 0x10)
/* Zen3 Ryzen desktop */
#define F19H_M21H_SVI_TEL_PLANE0 (F17H_M01H_SVI + 0x10)
#define F19H_M21H_SVI_TEL_PLANE1 (F17H_M01H_SVI + 0x0C)
/* Zen3 APU (Cezanne/Barcelo, Rembrandt) */
#define F19H_M50H_SVI_TEL_PLANE0 (F17H_M01H_SVI + 0x0C)
#define F19H_M50H_SVI_TEL_PLANE1 (F17H_M01H_SVI + 0x10)

/* SVI3 telemetry plane registers (Zen4 / Zen5) – community-verified offsets  */
#define F19H_M61H_SVI3_TEL_PLANE0                                              \
  (F17H_M01H_SVI + 0x10) /* Zen4 Raphael        */
#define F19H_M61H_SVI3_TEL_PLANE1 (F17H_M01H_SVI + 0x0C)
#define F19H_M70H_SVI3_TEL_PLANE0                                              \
  (F17H_M01H_SVI + 0x0C) /* Zen4 Phoenix APU    */
#define F19H_M70H_SVI3_TEL_PLANE1 (F17H_M01H_SVI + 0x10)
#define F1AH_M20H_SVI3_TEL_PLANE0                                              \
  (F17H_M01H_SVI + 0x0C) /* Zen5 Strix APU      */
#define F1AH_M20H_SVI3_TEL_PLANE1 (F17H_M01H_SVI + 0x10)
#define F1AH_M44H_SVI3_TEL_PLANE0                                              \
  (F17H_M01H_SVI + 0x10) /* Zen5 Granite Ridge  */
#define F1AH_M44H_SVI3_TEL_PLANE1 (F17H_M01H_SVI + 0x0C)

/* CCD temperature registers */
#define F17H_M70H_CCD_TEMP(x) (0x00059954 + ((x) * 4)) /* Zen2 / Zen3       */
#define F19H_CCD_TEMP(x) (0x00059954 + ((x) * 4))      /* Zen3 / Zen4 same  */

/* Package power reporting via SMU scratch (not universally supported) */
#define ZEN_SMU_CORE_PWR_ADDR 0x000598BC
#define ZEN_SMU_SOC_PWR_ADDR 0x0005994C

#define F17H_TEMP_ADJUST_MASK 0x80000

/* Maximum CCDs per die for each supported generation */
#define MAX_CCD 8

#ifndef HWMON_CHANNEL_INFO
#define HWMON_CHANNEL_INFO(stype, ...)                                         \
  (&(struct hwmon_channel_info){.type = hwmon_##stype,                         \
                                .config = (u32[]){__VA_ARGS__, 0}})
#endif

/* ── Zen generation enum ─────────────────────────────────────────────────── */

enum zen_generation {
  ZEN_GEN_1, /* Zen, Zen+                  */
  ZEN_GEN_2, /* Zen2                       */
  ZEN_GEN_3, /* Zen3, Zen3+                */
  ZEN_GEN_4, /* Zen4 (SVI3)                */
  ZEN_GEN_5, /* Zen5 (SVI3)                */
};

/* ── Driver private data ─────────────────────────────────────────────────── */

struct zenpower_data {
  struct pci_dev *pdev;
  void (*read_amdsmn_addr)(struct pci_dev *pdev, u16 node_id, u32 address,
                           u32 *regval);
  u32 svi_core_addr;
  u32 svi_soc_addr;
  u16 node_id;
  u8 cpu_id;
  u8 nodes_per_cpu;
  int temp_offset;
  enum zen_generation zen_gen;
  bool is_apu;
  bool kernel_smn_support;
  bool amps_visible;
  bool ccd_visible[MAX_CCD];
};

/* ── Tctl offsets for specific SKUs ─────────────────────────────────────── */

struct tctl_offset {
  u8 family;   /* x86 family: 0x17 or 0x19 */
  u8 model_lo; /* inclusive model range low */
  u8 model_hi; /* inclusive model range high (0 = exact match) */
  const char *id;
  int offset; /* millidegrees C to subtract from Tctl */
};

static const struct tctl_offset tctl_offset_table[] = {
    /* Zen / Zen+ desktop */
    {0x17, 0x01, 0x01, "AMD Ryzen 5 1600X", 20000},
    {0x17, 0x01, 0x01, "AMD Ryzen 7 1700X", 20000},
    {0x17, 0x01, 0x01, "AMD Ryzen 7 1800X", 20000},
    {0x17, 0x08, 0x08, "AMD Ryzen 7 2700X", 10000},
    /* Zen Threadripper */
    {0x17, 0x01, 0x01, "AMD Ryzen Threadripper 19",
     27000}, /* 1900X/1920X/1950X */
    {0x17, 0x01, 0x01, "AMD Ryzen Threadripper 29",
     27000}, /* 2920X/2950X/2970WX/2990WX */
    /* Zen2 desktop – no offset needed (Tdie == junction) */
    /* Zen3 desktop */
    {0x19, 0x21, 0x21, "AMD Ryzen 9 5900X", 0},
    {0x19, 0x21, 0x21, "AMD Ryzen 9 5950X", 0},
    /* Zen3 Threadripper / PRO */
    {0x19, 0x00, 0x01, "AMD Ryzen Threadripper PRO 59", 27000},
    {0x19, 0x00, 0x01, "AMD Ryzen Threadripper 59", 27000},
};

/* ── Mutex & multi-CPU state ─────────────────────────────────────────────── */

static DEFINE_MUTEX(nb_smu_ind_mutex);
static bool multicpu;

/* ── hwmon visibility ────────────────────────────────────────────────────── */

static umode_t zenpower_is_visible(const void *rdata,
                                   enum hwmon_sensor_types type, u32 attr,
                                   int channel) {
  const struct zenpower_data *data = rdata;

  switch (type) {
  case hwmon_temp:
    /* Tccd1-8 (channels 2..9): hide if not detected */
    if (channel >= 2 && !data->ccd_visible[channel - 2])
      return 0;
    break;

  case hwmon_curr:
  case hwmon_power:
    if (!data->amps_visible)
      return 0;
    if (channel == 0 && !data->svi_core_addr)
      return 0;
    if (channel == 1 && !data->svi_soc_addr)
      return 0;
    break;

  case hwmon_in:
    if (channel == 0) /* padding for index alignment */
      return 0;
    if (channel == 1 && !data->svi_core_addr)
      return 0;
    if (channel == 2 && !data->svi_soc_addr)
      return 0;
    break;

  default:
    break;
  }

  return 0444;
}

/* ── SVI2 decode helpers ─────────────────────────────────────────────────── */
/*
 * SVI2 voltage: bits [23:16] = VDDcor
 *   U (mV) = 1550 - 6.25 * VDDcor
 */
static u32 svi2_plane_to_vcc(u32 p) {
  u32 vdd = (p >> 16) & 0xff;

  return 1550 - ((625 * vdd) / 100);
}

/*
 * SVI2 current: bits [7:0] = IDDcor
 *   Zen/Zen+ core:  I (mA) = 1039.211 * IDDcor
 *   Zen2/3 core:    I (mA) =  658.823 * IDDcor
 *   Zen/Zen+ SoC:   I (mA) =  360.772 * IDDcor
 *   Zen2/3 SoC:     I (mA) =  294.3   * IDDcor
 */
static u32 svi2_get_core_current(u32 plane, enum zen_generation gen) {
  u32 idd = plane & 0xff;
  u32 fc = (gen >= ZEN_GEN_2) ? 658823 : 1039211;

  return (fc * idd) / 1000;
}

static u32 svi2_get_soc_current(u32 plane, enum zen_generation gen) {
  u32 idd = plane & 0xff;
  u32 fc = (gen >= ZEN_GEN_2) ? 294300 : 360772;

  return (fc * idd) / 1000;
}

/* ── SVI3 decode helpers (Zen4 / Zen5) ──────────────────────────────────── */
/*
 * SVI3 voltage: bits [15:8] = VDDcor (8-bit field, 0-based)
 *   U (mV) = 245 - 1.065 * VDDcor     (desktop Raphael / Granite Ridge)
 * For APU the range is slightly different but same formula holds.
 * Multiply by 1000 to keep mV integer arithmetic:
 *   U (µV) = 245000 - 1065 * VDDcor   → return mV = (245000 - 1065*vdd) / 1000
 * Note: community-verified on Ryzen 7 7700X and 9700X.
 */
static u32 svi3_plane_to_vcc(u32 p) {
  u32 vdd = (p >> 8) & 0xff;
  u32 uv = 245000 - 1065 * vdd; /* µV */

  return uv / 1000; /* mV */
}

/*
 * SVI3 current: bits [7:0] = IDDcor (7-bit meaningful, LSB reserved on some)
 *   Core I (mA) = 1000 * IDDcor / 4    (desktop)
 *   SoC  I (mA) =  350 * IDDcor / 4    (desktop SoC rail)
 * APU rails differ; these are best-effort approximations.
 */
static u32 svi3_get_core_current(u32 plane) {
  u32 idd = plane & 0x7f; /* 7-bit */

  return (1000 * idd) / 4; /* mA */
}

static u32 svi3_get_soc_current(u32 plane) {
  u32 idd = plane & 0x7f;

  return (350 * idd) / 4; /* mA */
}

/* ── Dispatch helpers ────────────────────────────────────────────────────── */

static u32 plane_to_vcc(u32 p, enum zen_generation gen) {
  return (gen >= ZEN_GEN_4) ? svi3_plane_to_vcc(p) : svi2_plane_to_vcc(p);
}

static u32 get_core_current(u32 p, enum zen_generation gen) {
  return (gen >= ZEN_GEN_4) ? svi3_get_core_current(p)
                            : svi2_get_core_current(p, gen);
}

static u32 get_soc_current(u32 p, enum zen_generation gen) {
  return (gen >= ZEN_GEN_4) ? svi3_get_soc_current(p)
                            : svi2_get_soc_current(p, gen);
}

/* ── Temperature reading ─────────────────────────────────────────────────── */

static unsigned int get_ctl_temp(struct zenpower_data *data) {
  u32 regval;
  unsigned int temp;

  data->read_amdsmn_addr(data->pdev, data->node_id,
                         F17H_M01H_REPORTED_TEMP_CTRL, &regval);
  temp = (regval >> 21) * 125;
  if (regval & F17H_TEMP_ADJUST_MASK)
    temp -= 49000;
  return temp;
}

static unsigned int get_ccd_temp(struct zenpower_data *data, u32 ccd_addr) {
  u32 regval;

  data->read_amdsmn_addr(data->pdev, data->node_id, ccd_addr, &regval);
  return (regval & 0xfff) * 125 - 305000;
}

/* ── Debug sysfs ─────────────────────────────────────────────────────────── */

static const int debug_addrs_arr[] = {
    F17H_M01H_SVI + 0x08,  F17H_M01H_SVI + 0x0C,  F17H_M01H_SVI + 0x10,
    F17H_M01H_SVI + 0x14,  ZEN_SMU_CORE_PWR_ADDR, ZEN_SMU_SOC_PWR_ADDR,
    F17H_M70H_CCD_TEMP(0), F17H_M70H_CCD_TEMP(1), F17H_M70H_CCD_TEMP(2),
    F17H_M70H_CCD_TEMP(3), F17H_M70H_CCD_TEMP(4), F17H_M70H_CCD_TEMP(5),
    F17H_M70H_CCD_TEMP(6), F17H_M70H_CCD_TEMP(7),
};

static ssize_t debug_data_show(struct device *dev,
                               struct device_attribute *attr, char *buf) {
  struct zenpower_data *data = dev_get_drvdata(dev);
  int i, len = 0;
  u32 smndata;

  len += sprintf(buf + len, "KERN_SUP:  %d\n", data->kernel_smn_support);
  len += sprintf(buf + len, "NODE %d; CPU %d; N/CPU: %d\n", data->node_id,
                 data->cpu_id, data->nodes_per_cpu);
  len += sprintf(buf + len, "ZEN_GEN:   %d\n", data->zen_gen);
  len += sprintf(buf + len, "IS_APU:    %d\n", data->is_apu);
  len += sprintf(buf + len, "AMPS_VIS:  %d\n", data->amps_visible);
  len += sprintf(buf + len, "SVI_CORE:  %08x\n", data->svi_core_addr);
  len += sprintf(buf + len, "SVI_SOC:   %08x\n", data->svi_soc_addr);

  for (i = 0; i < ARRAY_SIZE(debug_addrs_arr); i++) {
    data->read_amdsmn_addr(data->pdev, data->node_id, debug_addrs_arr[i],
                           &smndata);
    len += sprintf(buf + len, "%08x = %08x\n", debug_addrs_arr[i], smndata);
  }

  return len;
}

/* ── hwmon read ──────────────────────────────────────────────────────────── */

static int zenpower_read(struct device *dev, enum hwmon_sensor_types type,
                         u32 attr, int channel, long *val) {
  struct zenpower_data *data = dev_get_drvdata(dev);
  u32 plane;

  switch (type) {

  /* ── Temperatures ──────────────────────────────────────────────── */
  case hwmon_temp:
    if (attr == hwmon_temp_max) {
      /* Tdie max: 95 °C per AMD specs */
      *val = 95 * 1000;
      return 0;
    }
    if (attr != hwmon_temp_input)
      return -EOPNOTSUPP;

    switch (channel) {
    case 0: /* Tdie */
      *val = get_ctl_temp(data) - data->temp_offset;
      break;
    case 1: /* Tctl */
      *val = get_ctl_temp(data);
      break;
    case 2 ... 9: /* Tccd1-8 */
      *val = get_ccd_temp(data, F17H_M70H_CCD_TEMP(channel - 2));
      break;
    default:
      return -EOPNOTSUPP;
    }
    break;

  /* ── Voltage (hwmon_in uses 0-based indexing, we pad ch 0) ─────── */
  case hwmon_in:
    if (channel == 0)
      return -EOPNOTSUPP;
    channel -= 1;
    /* fall through into shared current/power/voltage handler */
    fallthrough;

  /* ── Current / Power ───────────────────────────────────────────── */
  case hwmon_curr:
  case hwmon_power:
    if (attr != hwmon_in_input && attr != hwmon_curr_input &&
        attr != hwmon_power_input)
      return -EOPNOTSUPP;

    switch (channel) {
    case 0:
      data->read_amdsmn_addr(data->pdev, data->node_id, data->svi_core_addr,
                             &plane);
      break;
    case 1:
      data->read_amdsmn_addr(data->pdev, data->node_id, data->svi_soc_addr,
                             &plane);
      break;
    default:
      return -EOPNOTSUPP;
    }

    switch (type) {
    case hwmon_in:
      /* millivolts */
      *val = plane_to_vcc(plane, data->zen_gen);
      break;
    case hwmon_curr:
      /* milliamps */
      *val = (channel == 0) ? get_core_current(plane, data->zen_gen)
                            : get_soc_current(plane, data->zen_gen);
      break;
    case hwmon_power:
      /*
       * Power in microwatts (µW):
       *   mA * mV = 10⁻³A * 10⁻³V = 10⁻⁶W = µW  ✓
       */
      if (channel == 0) {
        *val = (long)get_core_current(plane, data->zen_gen) *
               plane_to_vcc(plane, data->zen_gen);
      } else {
        *val = (long)get_soc_current(plane, data->zen_gen) *
               plane_to_vcc(plane, data->zen_gen);
      }
      break;
    default:
      break;
    }
    break;

  default:
    return -EOPNOTSUPP;
  }

  return 0;
}

/* ── hwmon label tables ──────────────────────────────────────────────────── */

static const char *const zenpower_temp_label[][10] = {
    {"Tdie", "Tctl", "Tccd1", "Tccd2", "Tccd3", "Tccd4", "Tccd5", "Tccd6",
     "Tccd7", "Tccd8"},
    {"cpu0 Tdie", "cpu0 Tctl", "cpu0 Tccd1", "cpu0 Tccd2", "cpu0 Tccd3",
     "cpu0 Tccd4", "cpu0 Tccd5", "cpu0 Tccd6", "cpu0 Tccd7", "cpu0 Tccd8"},
    {"cpu1 Tdie", "cpu1 Tctl", "cpu1 Tccd1", "cpu1 Tccd2", "cpu1 Tccd3",
     "cpu1 Tccd4", "cpu1 Tccd5", "cpu1 Tccd6", "cpu1 Tccd7", "cpu1 Tccd8"},
};

static const char *const zenpower_in_label[][3] = {
    {"", "SVI_Core", "SVI_SoC"},
    {"", "cpu0 SVI_Core", "cpu0 SVI_SoC"},
    {"", "cpu1 SVI_Core", "cpu1 SVI_SoC"},
};

static const char *const zenpower_curr_label[][2] = {
    {"SVI_C_Core", "SVI_C_SoC"},
    {"cpu0 SVI_C_Core", "cpu0 SVI_C_SoC"},
    {"cpu1 SVI_C_Core", "cpu1 SVI_C_SoC"},
};

static const char *const zenpower_power_label[][2] = {
    {"SVI_P_Core", "SVI_P_SoC"},
    {"cpu0 SVI_P_Core", "cpu0 SVI_P_SoC"},
    {"cpu1 SVI_P_Core", "cpu1 SVI_P_SoC"},
};

static int zenpower_read_labels(struct device *dev,
                                enum hwmon_sensor_types type, u32 attr,
                                int channel, const char **str) {
  struct zenpower_data *data;
  u8 i = 0;

  if (multicpu) {
    data = dev_get_drvdata(dev);
    if (data->cpu_id <= 1)
      i = data->cpu_id + 1;
  }

  switch (type) {
  case hwmon_temp:
    *str = zenpower_temp_label[i][channel];
    break;
  case hwmon_in:
    *str = zenpower_in_label[i][channel];
    break;
  case hwmon_curr:
    *str = zenpower_curr_label[i][channel];
    break;
  case hwmon_power:
    *str = zenpower_power_label[i][channel];
    break;
  default:
    return -EOPNOTSUPP;
  }
  return 0;
}

/* ── SMN access backends ─────────────────────────────────────────────────── */

static void kernel_smn_read(struct pci_dev *pdev, u16 node_id, u32 address,
                            u32 *regval) {
  amd_smn_read(node_id, address, regval);
}

/* Fallback from k10temp – may give inaccurate results on multi-die chips */
static void nb_index_read(struct pci_dev *pdev, u16 node_id, u32 address,
                          u32 *regval) {
  mutex_lock(&nb_smu_ind_mutex);
  pci_bus_write_config_dword(pdev->bus, PCI_DEVFN(0, 0), 0x60, address);
  pci_bus_read_config_dword(pdev->bus, PCI_DEVFN(0, 0), 0x64, regval);
  mutex_unlock(&nb_smu_ind_mutex);
}

/* ── hwmon channel descriptors ───────────────────────────────────────────── */

static const struct hwmon_channel_info *zenpower_info[] = {
    HWMON_CHANNEL_INFO(
        temp, HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_LABEL, /* Tdie  (ch0) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tctl  (ch1) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tccd1 (ch2) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tccd2 (ch3) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tccd3 (ch4) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tccd4 (ch5) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tccd5 (ch6) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tccd6 (ch7) */
        HWMON_T_INPUT | HWMON_T_LABEL,                     /* Tccd7 (ch8) */
        HWMON_T_INPUT | HWMON_T_LABEL),                    /* Tccd8 (ch9) */

    HWMON_CHANNEL_INFO(in, HWMON_I_LABEL,              /* pad  (ch0 – hidden) */
                       HWMON_I_INPUT | HWMON_I_LABEL,  /* Core voltage (ch1)  */
                       HWMON_I_INPUT | HWMON_I_LABEL), /* SoC  voltage (ch2)  */

    HWMON_CHANNEL_INFO(curr,
                       HWMON_C_INPUT | HWMON_C_LABEL,  /* Core current (ch0)  */
                       HWMON_C_INPUT | HWMON_C_LABEL), /* SoC  current (ch1)  */

    HWMON_CHANNEL_INFO(power,
                       HWMON_P_INPUT | HWMON_P_LABEL,  /* Core power   (ch0)  */
                       HWMON_P_INPUT | HWMON_P_LABEL), /* SoC  power   (ch1)  */

    NULL};

static const struct hwmon_ops zenpower_hwmon_ops = {
    .is_visible = zenpower_is_visible,
    .read = zenpower_read,
    .read_string = zenpower_read_labels,
};

static const struct hwmon_chip_info zenpower_chip_info = {
    .ops = &zenpower_hwmon_ops,
    .info = zenpower_info,
};

/* ── sysfs extras ────────────────────────────────────────────────────────── */

static DEVICE_ATTR_RO(debug_data);

static struct attribute *zenpower_attrs[] = {&dev_attr_debug_data.attr, NULL};
static const struct attribute_group zenpower_group = {.attrs = zenpower_attrs};
__ATTRIBUTE_GROUPS(zenpower);

/* ── probe ───────────────────────────────────────────────────────────────── */

static int zenpower_probe(struct pci_dev *pdev,
                          const struct pci_device_id *id) {
  struct device *dev = &pdev->dev;
  struct zenpower_data *data;
  struct device *hwmon_dev;
  struct pci_dev *misc;
  int i, ccd_check = 0;
  bool multinode;
  u8 node_of_cpu;
  u32 val;

  data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
  if (!data)
    return -ENOMEM;

  data->pdev = pdev;
  data->zen_gen = ZEN_GEN_1; /* safe default */
  data->is_apu = false;
  data->read_amdsmn_addr = nb_index_read;
  data->kernel_smn_support = false;
  data->amps_visible = false;
  data->temp_offset = 0;
  data->node_id = 0;
  for (i = 0; i < MAX_CCD; i++)
    data->ccd_visible[i] = false;

  /* Prefer kernel amd_smn_read() over the PCI index hack */
  for (i = 0; i < amd_nb_num(); i++) {
    misc = node_to_amd_nb(i)->misc;
    if (pdev->vendor == misc->vendor && pdev->device == misc->device) {
      data->kernel_smn_support = true;
      data->read_amdsmn_addr = kernel_smn_read;
      data->node_id = amd_pci_dev_to_node_id(pdev);
      break;
    }
  }

  /* CPUID_Fn8000001E_ECX[10:8] = NodesPerProcessor */
  data->nodes_per_cpu = 1 + ((cpuid_ecx(0x8000001E) >> 8) & 0b111);
  multinode = (data->nodes_per_cpu > 1);
  node_of_cpu = data->node_id % data->nodes_per_cpu;
  data->cpu_id = data->node_id / data->nodes_per_cpu;

  if (data->cpu_id > 0)
    multicpu = true;

  /* ──────────────────────────────────────────────────────────────
   * Per-family, per-model configuration
   * ────────────────────────────────────────────────────────────── */

  if (boot_cpu_data.x86 == 0x17) {
    switch (boot_cpu_data.x86_model) {

    case 0x01: /* Zen  – Ryzen 1000 / Naples EPYC */
    case 0x08: /* Zen+ – Ryzen 2000 / Pinnacle Ridge */
      data->zen_gen = ZEN_GEN_1;
      data->amps_visible = true;

      if (multinode) { /* Threadripper / EPYC */
        if (node_of_cpu == 0)
          data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE0;
        if (node_of_cpu == 1)
          data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
      } else { /* Normal Ryzen desktop */
        data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
        data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
      }
      ccd_check = 4;
      break;

    case 0x11: /* Zen  APU – Raven Ridge */
    case 0x18: /* Zen+ APU – Picasso     */
      data->zen_gen = ZEN_GEN_1;
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
      /* APUs have no CCDs */
      break;

    case 0x31: /* Zen2 – Castle Peak TR / Rome EPYC */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 TR/EPYC: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2");
      data->amps_visible = true;
      data->svi_core_addr = F17H_M30H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M30H_SVI_TEL_PLANE1;
      ccd_check = 8;
      break;

    case 0x60: /* Zen2 APU – Renoir  */
    case 0x68: /* Zen2 APU – Lucienne */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 APU: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2");
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F17H_M60H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M60H_SVI_TEL_PLANE1;
      break;

    case 0x71: /* Zen2 – Matisse Ryzen 3000 */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 Ryzen: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2");
      data->amps_visible = true;
      data->svi_core_addr = F17H_M70H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M70H_SVI_TEL_PLANE1;
      ccd_check = 8;
      break;

    case 0x90: /* Zen2 APU – Van Gogh (Steam Deck) */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 Van Gogh APU: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2");
      data->is_apu = true;
      data->amps_visible = true;
      /* Van Gogh uses same layout as Renoir */
      data->svi_core_addr = F17H_M60H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M60H_SVI_TEL_PLANE1;
      break;

    default:
      dev_warn(dev, "Unknown 17h model 0x%02x – using defaults\n",
               boot_cpu_data.x86_model);
      data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
      break;
    }

  } else if (boot_cpu_data.x86 == 0x19) {
    switch (boot_cpu_data.x86_model) {

    case 0x00 ... 0x01: /* Zen3 – Milan EPYC / Chagall TR */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3 SP3/TR: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->amps_visible = true;
      data->svi_core_addr = F19H_M01H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M01H_SVI_TEL_PLANE1;
      ccd_check = 8;
      break;

    case 0x10 ... 0x11: /* Zen4 – Genoa EPYC (SVI3) */
      data->zen_gen = ZEN_GEN_4;
      data->amps_visible = true;
      /* Genoa uses same register layout as Raphael for now */
      data->svi_core_addr = F19H_M61H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F19H_M61H_SVI3_TEL_PLANE1;
      ccd_check = 8;
      dev_info(dev, "Zen4 Genoa EPYC (SVI3)\n");
      break;

    case 0x21: /* Zen3 – Vermeer Ryzen 5000 desktop */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3 Ryzen: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->amps_visible = true;
      data->svi_core_addr = F19H_M21H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M21H_SVI_TEL_PLANE1;
      ccd_check = 2;
      break;

    case 0x40: /* Zen3+ APU – Rembrandt (Ryzen 6000) */
    case 0x44: /* Zen3+ APU – Rembrandt-R             */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3+ Rembrandt APU: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M50H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M50H_SVI_TEL_PLANE1;
      break;

    case 0x50: /* Zen3 APU – Cezanne / Barcelo (Ryzen 5000U/G) */
      data->zen_gen = (zen1_calc) ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3 Cezanne/Barcelo APU: using %s formula\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M50H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M50H_SVI_TEL_PLANE1;
      break;

    case 0x61: /* Zen4 desktop – Raphael (Ryzen 7000) (SVI3) */
      data->zen_gen = ZEN_GEN_4;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M61H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F19H_M61H_SVI3_TEL_PLANE1;
      ccd_check = 2;
      dev_info(dev, "Zen4 Raphael desktop (SVI3)\n");
      break;

    case 0x70 ... 0x78: /* Zen4 APU – Phoenix / Hawk Point (SVI3) */
      data->zen_gen = ZEN_GEN_4;
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M70H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F19H_M70H_SVI3_TEL_PLANE1;
      dev_info(dev, "Zen4 Phoenix/HawkPoint APU (SVI3)\n");
      break;

    default:
      dev_warn(dev, "Unknown 19h model 0x%02x – using Zen3 defaults\n",
               boot_cpu_data.x86_model);
      data->zen_gen = ZEN_GEN_3;
      data->svi_core_addr = F19H_M21H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M21H_SVI_TEL_PLANE1;
      break;
    }

  } else if (boot_cpu_data.x86 == 0x1a) {
    switch (boot_cpu_data.x86_model) {

    case 0x20 ... 0x24: /* Zen5 APU – Strix Point / Strix Halo */
      data->zen_gen = ZEN_GEN_5;
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F1AH_M20H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F1AH_M20H_SVI3_TEL_PLANE1;
      dev_info(dev, "Zen5 Strix Point APU (SVI3)\n");
      break;

    case 0x44: /* Zen5 desktop – Granite Ridge (Ryzen 9000) */
      data->zen_gen = ZEN_GEN_5;
      data->amps_visible = true;
      data->svi_core_addr = F1AH_M44H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F1AH_M44H_SVI3_TEL_PLANE1;
      ccd_check = 2;
      dev_info(dev, "Zen5 Granite Ridge desktop (SVI3)\n");
      break;

    default:
      dev_warn(dev, "Unknown 1Ah model 0x%02x – using Zen5 defaults\n",
               boot_cpu_data.x86_model);
      data->zen_gen = ZEN_GEN_5;
      data->svi_core_addr = F1AH_M20H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F1AH_M20H_SVI3_TEL_PLANE1;
      break;
    }

  } else {
    dev_warn(dev, "Unknown CPU family 0x%02x – using Zen1 defaults\n",
             boot_cpu_data.x86);
    data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
    data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
  }

  /* Detect which CCDs are actually present (non-zero temperature) */
  for (i = 0; i < ccd_check; i++) {
    data->read_amdsmn_addr(pdev, data->node_id, F17H_M70H_CCD_TEMP(i), &val);
    if ((val & 0xfff) > 0)
      data->ccd_visible[i] = true;
  }

  /* Apply per-SKU Tctl offset */
  for (i = 0; i < ARRAY_SIZE(tctl_offset_table); i++) {
    const struct tctl_offset *e = &tctl_offset_table[i];

    if (boot_cpu_data.x86 == e->family &&
        boot_cpu_data.x86_model >= e->model_lo &&
        (e->model_hi == 0 || boot_cpu_data.x86_model <= e->model_hi) &&
        strstr(boot_cpu_data.x86_model_id, e->id)) {
      data->temp_offset = e->offset;
      break;
    }
  }

  hwmon_dev = devm_hwmon_device_register_with_info(
      dev, "zenpower", data, &zenpower_chip_info, zenpower_groups);

  return PTR_ERR_OR_ZERO(hwmon_dev);
}

/* ── PCI device table ────────────────────────────────────────────────────── */

static const struct pci_device_id zenpower_id_table[] = {
    /* Family 17h – Zen / Zen+ / Zen2 */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_DF_F3)}, /* Zen SP3/Naples      */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_M10H_DF_F3)}, /* Zen+ Raven Ridge */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M20H_DF_F3)}, /* Zen Raven (alt DID) */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M30H_DF_F3)}, /* Zen2 Castle Peak TR */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_M60H_DF_F3)}, /* Zen2 Renoir APU */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M68H_DF_F3)}, /* Zen2 Lucienne APU   */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M70H_DF_F3)}, /* Zen2 Matisse Ryzen  */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_M90H_DF_F3)}, /* Zen2 Van Gogh */

    /* Family 19h – Zen3 / Zen3+ / Zen4 */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_DF_F3)}, /* Zen3 Milan EPYC     */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_M10H_DF_F3)}, /* Zen4 Genoa EPYC */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_19H_M21H_DF_F3)}, /* Zen3 Vermeer Ryzen  */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_19H_M40H_DF_F3)}, /* Zen3+ Rembrandt APU */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_M50H_DF_F3)}, /* Zen3 Cezanne APU */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_19H_M61H_DF_F3)}, /* Zen4 Raphael Ryzen  */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_M70H_DF_F3)}, /* Zen4 Phoenix APU */

    /* Family 1Ah – Zen5 */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_1AH_M20H_DF_F3)}, /* Zen5 Strix Point */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_1AH_M44H_DF_F3)}, /* Zen5 Granite Ridge  */
    {}};
MODULE_DEVICE_TABLE(pci, zenpower_id_table);

static struct pci_driver zenpower_driver = {
    .name = "zenpower",
    .id_table = zenpower_id_table,
    .probe = zenpower_probe,
};

module_pci_driver(zenpower_driver);
