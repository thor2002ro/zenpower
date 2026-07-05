/*
 * Zenpower - AMD Zen family CPU telemetry driver
 *
 * Copyright (c) 2018-2020 Ondrej Čerman
 * Copyright (c) 2024-2026 thor2002ro
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Based on k10temp by Clemens Ladisch.
 *
 * Docs:
 *   https://www.kernel.org/doc/Documentation/hwmon/hwmon-kernel-api.txt
 *   https://developer.amd.com/wp-content/resources/56255_3_03.PDF
 *
 * Sources:
 *   - Temperature monitoring    : k10temp
 *   - SVI2 decode               : LibreHardwareMonitor
 *   - SVI3 decode               : community RE (Raphael / Granite Ridge
 * verified)
 *   - CCD addresses / SMU regs  : experimental + RyzenAdj / zenergy
 *
 * Generation support matrix:
 *   Zen  (17h 0x01,0x08)        SVI2  Ryzen 1000/2000, Summit/Pinnacle Ridge
 *   Zen  APU (17h 0x11,0x18)    SVI2  Raven Ridge / Picasso
 *   Zen2 (17h 0x31)             SVI2  Castle Peak TR / Rome EPYC
 *   Zen2 APU (17h 0x60,0x68)    SVI2  Renoir / Lucienne
 *   Zen2 APU (17h 0x90)         SVI2  Van Gogh (Steam Deck)
 *   Zen2 (17h 0x71)             SVI2  Matisse Ryzen 3000
 *   Zen3 SP3 (19h 0x00-0x01)    SVI2  Milan EPYC / Chagall TR
 *   Zen3 (19h 0x21)             SVI2  Vermeer Ryzen 5000
 *   Zen3 APU (19h 0x50)         SVI2  Cezanne / Barcelo
 *   Zen3+ APU (19h 0x40,0x44)   SVI2  Rembrandt / Rembrandt-R
 *   Zen4 SP3 (19h 0x10-0x11)    SVI3  Genoa EPYC
 *   Zen4 (19h 0x61)             SVI3  Raphael Ryzen 7000
 *   Zen4 APU (19h 0x70-0x78)    SVI3  Phoenix / Hawk Point
 *   Zen5 APU (1Ah 0x20-0x24)    SVI3  Strix Point / Strix Halo
 *   Zen5 (1Ah 0x44)             SVI3  Granite Ridge Ryzen 9000
 *
 * Power channels exposed:
 *   power1  Core rail power  (SVI VRM telemetry, uW)
 *   power2  SoC  rail power  (SVI VRM telemetry, uW)
 *   power3  Package PPT      (SMU silicon limit, uW, when readable)
 *   power4  TDC x Vcore      (sustained current proxy, uW, informational)
 *   power5  EDC x Vcore      (peak current proxy, uW, informational)
 *   power6  RAPL package     (MSR energy counter average, uW, when available)
 *
 * NOTE: power3-5 use SMU scratch registers whose layout is community-
 * derived.  They hide themselves at probe time if the register returns
 * a sentinel or implausible value.
 */

#include <linux/bitops.h>
#include <linux/cpu.h>
#include <linux/hwmon.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/topology.h>
#include <linux/version.h>
#include <linux/workqueue.h>
#include <asm/msr.h>

/*
 * Linux 6.16 reorganised the AMD northbridge header from asm/amd_nb.h into
 * asm/amd/nb.h.  Guard both paths so the driver builds on kernels before and
 * after that rename.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#include <asm/amd/nb.h>
#else
#include <asm/amd_nb.h>
#endif

/* Kernel 6.16+ renamed rdmsrl_safe_on_cpu() to rdmsrq_safe_on_cpu(). */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define zenpower_rdmsrq_safe_on_cpu rdmsrq_safe_on_cpu
#else
#define zenpower_rdmsrq_safe_on_cpu rdmsrl_safe_on_cpu
#endif

MODULE_DESCRIPTION("AMD ZEN family CPU Sensors Driver");
MODULE_AUTHOR("thor2002ro");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.6.0");

/* ---- Module parameters -------------------------------------------------- */

static bool zen1_calc;
module_param(zen1_calc, bool, 0444);
MODULE_PARM_DESC(zen1_calc,
                 "Force Zen1 SVI2 current coefficients (default: auto)");

static bool force_svi2;
module_param(force_svi2, bool, 0444);
MODULE_PARM_DESC(force_svi2, "Force SVI2 decode path on all chips");

static bool force_svi3;
module_param(force_svi3, bool, 0444);
MODULE_PARM_DESC(force_svi3, "Force SVI3 decode path on all chips");

/* ---- PCI Device IDs ----------------------------------------------------- */

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

/* ---- SMN Register Addresses --------------------------------------------- */

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
#define ZEN_CCD_TEMP(x) (0x00059954 + ((x) * 4))
#define ZEN_CCD_TEMP_VALID BIT(11)
#define ZEN_CCD_TEMP_MASK 0x7ff

/*
 * SMU scratch / metrics registers (community-derived, best-effort).
 *
 * PPT format on Zen2/3: 32-bit integer, units = milliwatts.
 * PPT format on Zen4/5: 32-bit IEEE-754 single-precision float, units = watts.
 *
 * TDC/EDC limit format: unsigned 8.3 fixed-point amps in bits[10:0].
 *
 * All values returned to hwmon in microwatts.
 */
#define ZEN_SMU_PKG_PPT_ADDR 0x000398BC  /* Package power limit (PPT) */
#define ZEN_SMU_CORE_TDC_ADDR 0x000398C0 /* Sustained current cap (TDC) */
#define ZEN_SMU_CORE_EDC_ADDR 0x000398C4 /* Peak current cap (EDC) */
/*
 * ZEN_SMU_SOC_PPT_ADDR: SoC subsystem power register.  Present in debug dump
 * for investigative use but not exposed as a hwmon channel because its unit
 * and layout differ significantly across firmware versions and are not reliably
 * community-verified.  Do not use for power calculations without validation.
 */
#define ZEN_SMU_SOC_PPT_ADDR 0x0005994C

#define SMU_REG_SENTINEL_FF 0xFFFFFFFF
#define SMU_REG_SENTINEL_00 0x00000000

#define SMU_CURRENT_LIMIT_MASK 0x7ff
#define SMU_CURRENT_LIMIT_MAX_MA 500000

/* AMD RAPL MSRs */
#define MSR_AMD_RAPL_POWER_UNIT 0xc0010299
#define MSR_AMD_PKG_ENERGY_STATUS 0xc001029b

#define RAPL_ENERGY_UNIT_MASK 0x1f00
#define RAPL_ENERGY_UNIT_SHIFT 8
#define RAPL_ENERGY_STATUS_32_MASK 0xffffffffULL
/* Size the 32-bit accumulator cadence for high-power package limits. */
#define RAPL_ACCUM_POWER_W 1000
#define RAPL_ACCUM_MAX_EXP 28
#define RAPL_CHANNEL_PACKAGE 0
#define RAPL_CHANNEL_COUNT 1

/*
 * Plausibility window for SVI voltage auto-detection.
 * Any decoded voltage outside this range (mV) means the register layout
 * is wrong and the other protocol version should be tried.
 */
#define SVI_VOLTAGE_MIN_MV 600
#define SVI_VOLTAGE_MAX_MV 1600

#define F17H_TEMP_ADJUST_MASK 0x80000
#define MAX_CCD 8

#ifndef HWMON_CHANNEL_INFO
#define HWMON_CHANNEL_INFO(stype, ...)                                         \
  (&(struct hwmon_channel_info){.type = hwmon_##stype,                         \
                                .config = (u32[]){__VA_ARGS__, 0}})
#endif

/* ---- Zen generation enum ------------------------------------------------- */

enum zen_generation {
  ZEN_GEN_1,
  ZEN_GEN_2,
  ZEN_GEN_3,
  ZEN_GEN_4,
  ZEN_GEN_5,
};

enum svi_version {
  SVI_VER_2,
  SVI_VER_3,
};

/* ---- Driver private data ------------------------------------------------- */

struct zenpower_data {
  struct pci_dev *pdev;
  int (*read_amdsmn_addr)(struct pci_dev *pdev, u16 node_id, u32 address,
                          u32 *regval);

  u32 svi_core_addr;
  u32 svi_soc_addr;
  u16 node_id;
  u8 cpu_id;
  u8 nodes_per_cpu;
  int temp_offset;

  enum zen_generation zen_gen;
  enum svi_version svi_ver;

  bool is_apu;
  bool kernel_smn_support;
  bool amps_visible;
  bool ccd_visible[MAX_CCD];

  /* SMU power channel visibility (probed at boot) */
  bool ppt_visible;
  bool tdc_visible;
  bool edc_visible;

  u32 smu_ppt_addr;
  u32 smu_tdc_addr;
  u32 smu_edc_addr;

  /*
   * true  => SMU register holds IEEE-754 float watts  (Zen4/5)
   * false => SMU register holds integer milliwatts    (Zen2/3)
   */
  bool smu_float_power;

  /* RAPL package power tracking */
  struct delayed_work rapl_accum_work;
  struct mutex rapl_lock;
  u64 rapl_accum_energy[RAPL_CHANNEL_COUNT];
  u64 rapl_prev_energy[RAPL_CHANNEL_COUNT];
  u64 rapl_prev_raw_energy[RAPL_CHANNEL_COUNT];
  ktime_t rapl_last_accum_time[RAPL_CHANNEL_COUNT];
  ktime_t rapl_prev_time[RAPL_CHANNEL_COUNT];
  bool rapl_available[RAPL_CHANNEL_COUNT];
  int rapl_cpu;
  u32 rapl_accum_interval_ms;
  u32 rapl_energy_unit; /* ESU exponent from MSR_AMD_RAPL_POWER_UNIT */
  bool rapl_counter_64bit;
  bool rapl_initialized;
  bool rapl_accum_running;
};

/* ---- Tctl offsets -------------------------------------------------------- */

struct tctl_offset {
  u8 family;   /* x86 family: 0x17 or 0x19 */
  u8 model_lo; /* inclusive model range low */
  u8 model_hi; /* inclusive model range high (0 = exact match) */
  const char *id;
  int offset; /* millidegrees C to subtract from Tctl */
};

static const struct tctl_offset tctl_offset_table[] = {
    {0x17, 0x01, 0x01, "AMD Ryzen 5 1600X", 20000},
    {0x17, 0x01, 0x01, "AMD Ryzen 7 1700X", 20000},
    {0x17, 0x01, 0x01, "AMD Ryzen 7 1800X", 20000},
    {0x17, 0x08, 0x08, "AMD Ryzen 7 2700X", 10000},
    {0x17, 0x01, 0x01, "AMD Ryzen Threadripper 19", 27000},
    {0x17, 0x01, 0x01, "AMD Ryzen Threadripper 29", 27000},
    {0x19, 0x00, 0x01, "AMD Ryzen Threadripper PRO 59", 27000},
    {0x19, 0x00, 0x01, "AMD Ryzen Threadripper 59", 27000},
};

static DEFINE_MUTEX(nb_smu_ind_mutex);
/*
 * multicpu: set when more than one physical CPU package is detected.
 * Written once at probe time; READ_ONCE/WRITE_ONCE enforce ordering so that
 * concurrent readers in read_labels() see a consistent value without a lock.
 */
static bool multicpu;

static u64 zenpower_mul_u64_u64_div_u64(u64 a, u64 b, u64 d) {
  if (!d)
    return ~0ULL;

  return mul_u64_u64_div_u64(a, b, d);
}

/*
 * amd_pci_dev_to_node_id() was removed from the public kernel API in 6.14
 * (commit c3e2e8f5).  Re-implement it here for kernels that no longer export
 * it; the logic is identical to the removed helper -- PCI slot minus the
 * slot number of node 0.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 14, 0)
static u16 amd_pci_dev_to_node_id(struct pci_dev *pdev) {
  return PCI_SLOT(pdev->devfn) - AMD_NODE0_PCI_SLOT;
}
#endif

/* ---- SMN access backends ------------------------------------------------- */

static int kernel_smn_read(struct pci_dev *pdev, u16 node_id, u32 address,
                           u32 *regval) {
  int err;

  err = amd_smn_read(node_id, address, regval);
  if (err)
    *regval = 0;

  return err;
}

/* Fallback: PCI index pair -- may be inaccurate on multi-die chips */
static int nb_index_read(struct pci_dev *pdev, u16 node_id, u32 address,
                         u32 *regval) {
  int err;

  *regval = 0;

  mutex_lock(&nb_smu_ind_mutex);
  err = pci_bus_write_config_dword(pdev->bus, PCI_DEVFN(0, 0), 0x60, address);
  if (!err)
    err = pci_bus_read_config_dword(pdev->bus, PCI_DEVFN(0, 0), 0x64,
                                    regval);
  mutex_unlock(&nb_smu_ind_mutex);

  if (err)
    return -EIO;

  if (PCI_POSSIBLE_ERROR(*regval)) {
    *regval = 0;
    return -ENODEV;
  }

  return 0;
}

/* ---- SVI2 decode --------------------------------------------------------- */
/*
 * Voltage bits[23:16] = VDDcor
 *   V (mV) = 1550 - 6.25 * VDDcor
 *
 * Core current bits[7:0] = IDDcor
 *   Zen/Zen+:  I (mA) = 1039.211 * IDDcor
 *   Zen2/3:    I (mA) =  658.823 * IDDcor
 *
 * SoC current bits[7:0] = IDDcor
 *   Zen/Zen+:  I (mA) =  360.772 * IDDcor
 *   Zen2/3:    I (mA) =  294.3   * IDDcor
 */
static u32 svi2_to_vcc(u32 p) {
  u32 vdd = (p >> 16) & 0xff;
  s32 mv = 1550 - (s32)((625u * vdd) / 100u);

  /*
   * VDDcor >= 249 makes the formula negative (rail shutdown / error state).
   * Clamp to 0 rather than silently wrapping to ~4 GV on the u32 path.
   */
  return (mv < 0) ? 0u : (u32)mv;
}

static u32 svi2_core_ma(u32 p, enum zen_generation gen) {
  u32 idd = p & 0xff;
  u32 fc = (gen >= ZEN_GEN_2) ? 658823 : 1039211;

  return (fc * idd) / 1000;
}

static u32 svi2_soc_ma(u32 p, enum zen_generation gen) {
  u32 idd = p & 0xff;
  u32 fc = (gen >= ZEN_GEN_2) ? 294300 : 360772;

  return (fc * idd) / 1000;
}

/* ---- SVI3 decode (Zen4/Zen5, community-verified on Raphael + Granite Ridge)
 */
/*
 * Voltage bits[15:8] = VDDcor
 *   V (mV) = 1550 - 6.25 * VDDcor
 *
 * The numeric formula is identical to SVI2; only the bit position of VDDcor
 * changes (bits[23:16] on SVI2, bits[15:8] on SVI3).  Community RE on Raphael
 * and Granite Ridge confirms this; the previously-used formula
 * "245 - 1.065 * VDDcor" is wrong -- it yields sub-200 mV for all realistic
 * VID values and would break the auto-detect plausibility check.
 *
 * Current bits[6:0] = IDDcor (7-bit)
 *   Core I (mA) = 1000 * IDDcor / 4
 *   SoC  I (mA) =  350 * IDDcor / 4
 *
 * NOTE: APU rail scaling may vary by SKU; these are best-available
 * community coefficients.  Use force_svi2 if readings look wrong.
 */
static u32 svi3_to_vcc(u32 p) {
  u32 vdd = (p >> 8) & 0xff; /* bits[15:8] -- VDDcor field for SVI3 */
  s32 mv = 1550 - (s32)((625u * vdd) / 100u);

  return (mv < 0) ? 0u : (u32)mv;
}

static u32 svi3_core_ma(u32 p) { return (1000 * (p & 0x7f)) / 4; }

static u32 svi3_soc_ma(u32 p) { return (350 * (p & 0x7f)) / 4; }

/* ---- Protocol-agnostic dispatch ----------------------------------------- */

static u32 plane_to_vcc_mv(u32 p, const struct zenpower_data *d) {
  return (d->svi_ver == SVI_VER_3) ? svi3_to_vcc(p) : svi2_to_vcc(p);
}

static u32 plane_core_ma(u32 p, const struct zenpower_data *d) {
  return (d->svi_ver == SVI_VER_3) ? svi3_core_ma(p)
                                   : svi2_core_ma(p, d->zen_gen);
}

static u32 plane_soc_ma(u32 p, const struct zenpower_data *d) {
  return (d->svi_ver == SVI_VER_3) ? svi3_soc_ma(p)
                                   : svi2_soc_ma(p, d->zen_gen);
}

/*
 * safe_power_uw() -- overflow-safe µW calculation.
 *
 * mA * mV = 10^-3 A * 10^-3 V = 10^-6 W = µW.
 *
 * At Threadripper/EPYC extremes (300 A * 1.4 V = 420 W = 420,000,000 µW)
 * a u32 product overflows at ~4295 W and a signed 32-bit long overflows at
 * ~2147 W.  Promote to u64 and clamp to LONG_MAX so the hwmon layer is safe.
 */
static long safe_power_uw(u32 ma, u32 mv) {
  u64 uw = (u64)ma * (u64)mv;

  return (uw > (u64)LONG_MAX) ? LONG_MAX : (long)uw;
}

/* ---- SMU float conversion ------------------------------------------------ */
/*
 * Zen4/5 SMU metrics registers hold power as IEEE-754 single-precision float
 * in watts.  We convert to microwatts using integer arithmetic only, avoiding
 * soft-float ABI issues on kernel builds without FPU.
 *
 * float layout: [31] sign | [30:23] biased exponent | [22:0] mantissa
 *
 * exp upper bound of 23: multi-MW range, safely above any realistic CPU power.
 * Multiply by 1,000,000 before shifting so fractional watts are preserved.
 */
static long smu_float_to_uw(u32 raw) {
  u32 sign = (raw >> 31) & 1;
  s32 exp = (s32)((raw >> 23) & 0xff) - 127;
  u32 mantissa = (raw & 0x7fffff) | 0x800000; /* implicit leading 1 */
  u64 uw_sc; /* mantissa scaled to microwatts before binary exponent shift */

  /* Reject: negative, zero, infinity/NaN (biased exp = 0xff) */
  if (sign || raw == 0 || ((raw >> 23) & 0xff) == 0xff)
    return 0;
  /* Reject sub-milliwatt noise and values above ~8 MW */
  if (exp < -10 || exp > 23)
    return 0;

  uw_sc = (u64)mantissa * 1000000ULL;

  if (exp >= 23)
    uw_sc <<= (u32)(exp - 23);
  else
    uw_sc >>= (u32)(23 - exp);

  return (long)min_t(u64, uw_sc, (u64)LONG_MAX);
}

static int read_smu_power_uw(struct zenpower_data *data, u32 addr, long *val) {
  u32 raw;
  int err;

  *val = 0;

  if (!addr)
    return -ENODEV;

  err = data->read_amdsmn_addr(data->pdev, data->node_id, addr, &raw);
  if (err)
    return err;

  if (raw == SMU_REG_SENTINEL_FF || raw == SMU_REG_SENTINEL_00)
    return 0;

  if (data->smu_float_power)
    *val = smu_float_to_uw(raw);
  else
    /* Integer milliwatts -> µW, clamped */
    *val = (long)min_t(u64, (u64)raw * 1000ULL, (u64)LONG_MAX);

  return 0;
}

static u32 smu_current_limit_raw_to_ma(u32 raw) {
  return ((raw & SMU_CURRENT_LIMIT_MASK) * 1000u) / 8u;
}

/* ---- SVI protocol auto-detection ---------------------------------------- */
/*
 * Strategy:
 *  1. Start with the generation-presumed SVI version from the model table.
 *  2. Read the core plane register and decode voltage under the presumed
 * version.
 *  3. If the result falls outside SVI_VOLTAGE_MIN_MV..SVI_VOLTAGE_MAX_MV,
 *     try the alternate version.
 *  4. If neither gives a plausible value (CPU idle / rail parked), keep the
 *     model-table presumption -- we cannot override with ambiguous data.
 *
 *  force_svi2 / force_svi3 module params bypass this entirely.
 */
static enum svi_version detect_svi_version(struct zenpower_data *data,
                                           enum svi_version presumed) {
  u32 plane, mv;

  if (force_svi2 && force_svi3) {
    pr_warn("zenpower: force_svi2 and force_svi3 are both set -- "
            "force_svi2 takes precedence\n");
    return SVI_VER_2;
  }
  if (force_svi2)
    return SVI_VER_2;
  if (force_svi3)
    return SVI_VER_3;
  if (!data->svi_core_addr)
    return presumed;

  if (data->read_amdsmn_addr(data->pdev, data->node_id, data->svi_core_addr,
                             &plane))
    return presumed;

  /* Test presumed version */
  mv = (presumed == SVI_VER_3) ? svi3_to_vcc(plane) : svi2_to_vcc(plane);
  if (mv >= SVI_VOLTAGE_MIN_MV && mv <= SVI_VOLTAGE_MAX_MV)
    return presumed;

  /* Test alternate */
  {
    enum svi_version alt = (presumed == SVI_VER_3) ? SVI_VER_2 : SVI_VER_3;
    mv = (alt == SVI_VER_3) ? svi3_to_vcc(plane) : svi2_to_vcc(plane);
    if (mv >= SVI_VOLTAGE_MIN_MV && mv <= SVI_VOLTAGE_MAX_MV) {
      pr_info("zenpower: SVI auto-detect: presumed SVI%d, plausibility "
              "selects SVI%d (plane=0x%08x decoded_mv=%u)\n",
              (presumed == SVI_VER_3) ? 3 : 2, (alt == SVI_VER_3) ? 3 : 2,
              plane, mv);
      return alt;
    }
  }

  /* Ambiguous -- retain model-table presumption */
  return presumed;
}

/* ---- SMU register usability probe --------------------------------------- */
/*
 * Returns true if the SMU register at 'addr' returns a non-sentinel value
 * that decodes to a physically plausible power in the range 1 mW..10 kW.
 */
static bool smu_power_reg_is_usable(struct zenpower_data *data, u32 addr) {
  long uw;
  int err;

  if (!addr)
    return false;
  err = read_smu_power_uw(data, addr, &uw);
  if (err)
    return false;
  /* Accept 1 uW to 10 kW */
  return (uw > 0 && uw <= 10000LL * 1000000LL);
}

static bool smu_current_limit_is_usable(struct zenpower_data *data, u32 addr) {
  u32 raw;
  u32 ma;
  int err;

  if (!addr)
    return false;
  err = data->read_amdsmn_addr(data->pdev, data->node_id, addr, &raw);
  if (err)
    return false;
  if (raw == SMU_REG_SENTINEL_FF || raw == SMU_REG_SENTINEL_00)
    return false;

  ma = smu_current_limit_raw_to_ma(raw);
  return (ma > 0 && ma <= SMU_CURRENT_LIMIT_MAX_MA);
}

/* ---- RAPL power backend -------------------------------------------------- */

static int zenpower_rapl_pick_cpu(struct zenpower_data *data) {
  int cpu;

  for_each_online_cpu(cpu) {
    if (topology_physical_package_id(cpu) == data->cpu_id) {
      data->rapl_cpu = cpu;
      return 0;
    }
  }

  if (data->cpu_id)
    return -ENODEV;

  cpu = cpumask_first(cpu_online_mask);
  if (cpu >= nr_cpu_ids)
    return -ENODEV;

  data->rapl_cpu = cpu;
  return 0;
}

static int zenpower_rapl_read_msr(struct zenpower_data *data, u32 msr,
                                  u64 *val) {
  int err;

  if (data->rapl_cpu < 0 || !cpu_online(data->rapl_cpu)) {
    err = zenpower_rapl_pick_cpu(data);
    if (err)
      return err;
  }

  return zenpower_rdmsrq_safe_on_cpu(data->rapl_cpu, msr, val);
}

static bool zenpower_rapl_counter_is_64bit(void) {
  /*
   * Default unknown readable RAPL counters to 32-bit accumulation.  A false
   * 64-bit assumption turns a 32-bit wrap into a huge power spike; the 32-bit
   * accumulator is safe for 64-bit counters, just less direct.
   */
  if (boot_cpu_data.x86 == 0x19) {
    switch (boot_cpu_data.x86_model) {
    case 0x10:
    case 0x11:
    case 0x90:
    case 0xa0:
      return true;
    }
  }

  if (boot_cpu_data.x86 == 0x1a) {
    switch (boot_cpu_data.x86_model) {
    case 0x00:
    case 0x02:
    case 0x10:
    case 0x11:
      return true;
    }
  }

  return false;
}

static u64 zenpower_rapl_normalize_counter(struct zenpower_data *data,
                                           u64 energy) {
  return data->rapl_counter_64bit ? energy
                                  : energy & RAPL_ENERGY_STATUS_32_MASK;
}

static u64 zenpower_rapl_counter_delta(struct zenpower_data *data,
                                       u64 energy_now, u64 energy_prev) {
  if (data->rapl_counter_64bit)
    return energy_now - energy_prev;

  return (u32)energy_now - (u32)energy_prev;
}

static u32 zenpower_rapl_accum_interval_ms(u32 energy_unit) {
  u32 exp = 0;
  u64 ms;

  if (energy_unit < 31)
    exp = min_t(u32, RAPL_ACCUM_MAX_EXP, 31 - energy_unit);

  ms = div_u64(1000ULL * BIT_ULL(exp), RAPL_ACCUM_POWER_W);
  return (u32)max_t(u64, ms, 1);
}

static bool zenpower_rapl_accum_deadline_missed(struct zenpower_data *data,
                                                int channel, ktime_t now) {
  s64 elapsed_ms;
  u64 wrap_guard_ms;

  if (data->rapl_counter_64bit || !data->rapl_accum_interval_ms)
    return false;

  elapsed_ms =
      ktime_to_ms(ktime_sub(now, data->rapl_last_accum_time[channel]));
  if (elapsed_ms <= 0)
    return false;

  wrap_guard_ms = (u64)data->rapl_accum_interval_ms * 2;
  return (u64)elapsed_ms >= wrap_guard_ms;
}

static int zenpower_rapl_accumulate_channel(struct zenpower_data *data,
                                            int channel, u64 *energy) {
  u64 energy_now;
  ktime_t now;
  int err;

  err = zenpower_rapl_read_msr(data, MSR_AMD_PKG_ENERGY_STATUS, &energy_now);
  if (err)
    return err;

  energy_now = zenpower_rapl_normalize_counter(data, energy_now);

  if (data->rapl_counter_64bit) {
    if (energy)
      *energy = energy_now;
    return 0;
  }

  now = ktime_get();
  if (zenpower_rapl_accum_deadline_missed(data, channel, now)) {
    /*
     * The 32-bit counter may have wrapped more than once while accumulation was
     * stalled.  Drop the unknowable delta, re-baseline the raw counter, and make
     * the next foreground read start a fresh dEnergy/dTime window.
     */
    data->rapl_prev_raw_energy[channel] = energy_now;
    data->rapl_last_accum_time[channel] = now;
    data->rapl_prev_energy[channel] = data->rapl_accum_energy[channel];
    data->rapl_prev_time[channel] = now;
    return -EAGAIN;
  }

  data->rapl_accum_energy[channel] +=
      zenpower_rapl_counter_delta(data, energy_now,
                                  data->rapl_prev_raw_energy[channel]);
  data->rapl_prev_raw_energy[channel] = energy_now;
  data->rapl_last_accum_time[channel] = now;

  if (energy)
    *energy = data->rapl_accum_energy[channel];

  return 0;
}

static void zenpower_rapl_accum_work(struct work_struct *work) {
  struct zenpower_data *data =
      container_of(to_delayed_work(work), struct zenpower_data,
                   rapl_accum_work);
  int channel;

  if (!READ_ONCE(data->rapl_accum_running))
    return;

  mutex_lock(&data->rapl_lock);
  for (channel = 0; channel < RAPL_CHANNEL_COUNT; channel++) {
    if (data->rapl_available[channel])
      zenpower_rapl_accumulate_channel(data, channel, NULL);
  }
  mutex_unlock(&data->rapl_lock);

  if (READ_ONCE(data->rapl_accum_running))
    schedule_delayed_work(&data->rapl_accum_work,
                          msecs_to_jiffies(data->rapl_accum_interval_ms));
}

static int zenpower_rapl_init(struct zenpower_data *data, struct device *dev) {
  ktime_t now;
  u32 energy_unit;
  u64 val;
  int err;

  err = zenpower_rapl_pick_cpu(data);
  if (err)
    return err;

  err = zenpower_rapl_read_msr(data, MSR_AMD_RAPL_POWER_UNIT, &val);
  if (err)
    return err;

  energy_unit = (val & RAPL_ENERGY_UNIT_MASK) >> RAPL_ENERGY_UNIT_SHIFT;
  data->rapl_energy_unit = energy_unit;
  data->rapl_counter_64bit = zenpower_rapl_counter_is_64bit();
  data->rapl_accum_interval_ms =
      zenpower_rapl_accum_interval_ms(data->rapl_energy_unit);

  err = zenpower_rapl_read_msr(data, MSR_AMD_PKG_ENERGY_STATUS, &val);
  if (err)
    return err;
  val = zenpower_rapl_normalize_counter(data, val);
  data->rapl_accum_energy[RAPL_CHANNEL_PACKAGE] = 0;
  data->rapl_prev_raw_energy[RAPL_CHANNEL_PACKAGE] = val;
  data->rapl_prev_energy[RAPL_CHANNEL_PACKAGE] =
      data->rapl_counter_64bit ? val : 0;
  data->rapl_available[RAPL_CHANNEL_PACKAGE] = true;

  now = ktime_get();
  data->rapl_last_accum_time[RAPL_CHANNEL_PACKAGE] = now;
  data->rapl_prev_time[RAPL_CHANNEL_PACKAGE] = now;
  data->rapl_initialized = true;

  return 0;
}

static void zenpower_rapl_start(struct zenpower_data *data) {
  if (!data->rapl_initialized || data->rapl_counter_64bit)
    return;

  WRITE_ONCE(data->rapl_accum_running, true);
  schedule_delayed_work(&data->rapl_accum_work,
                        msecs_to_jiffies(data->rapl_accum_interval_ms));
}

static void zenpower_rapl_stop(struct zenpower_data *data) {
  WRITE_ONCE(data->rapl_accum_running, false);
  cancel_delayed_work_sync(&data->rapl_accum_work);
}

static int zenpower_rapl_read_power(struct zenpower_data *data, int channel,
                                    long *val) {
  ktime_t now, delta;
  u64 energy_now;
  u64 energy_delta;
  u64 divisor;
  u64 uw;
  s64 delta_ms;
  int err;

  if (channel < 0 || channel >= RAPL_CHANNEL_COUNT ||
      !data->rapl_initialized || !data->rapl_available[channel])
    return -EOPNOTSUPP;

  mutex_lock(&data->rapl_lock);

  err = zenpower_rapl_accumulate_channel(data, channel, &energy_now);
  if (err)
    goto out_unlock;

  now = ktime_get();
  delta = ktime_sub(now, data->rapl_prev_time[channel]);
  delta_ms = ktime_to_ms(delta);
  if (delta_ms <= 0) {
    err = -EAGAIN;
    goto out_unlock;
  }

  energy_delta = energy_now - data->rapl_prev_energy[channel];

  if ((u64)delta_ms > (~0ULL >> data->rapl_energy_unit)) {
    err = -ERANGE;
    goto out_unlock;
  }
  divisor = (u64)delta_ms << data->rapl_energy_unit;
  if (!divisor) {
    err = -ERANGE;
    goto out_unlock;
  }

  uw = zenpower_mul_u64_u64_div_u64(energy_delta, 1000000000ULL, divisor);
  *val = (long)min_t(u64, uw, (u64)LONG_MAX);

  data->rapl_prev_energy[channel] = energy_now;
  data->rapl_prev_time[channel] = now;

out_unlock:
  mutex_unlock(&data->rapl_lock);
  return err;
}

/* ---- Temperature reading ------------------------------------------------- */

static int get_ctl_temp(struct zenpower_data *data, long *val) {
  u32 regval;
  long temp;
  int err;

  err = data->read_amdsmn_addr(data->pdev, data->node_id,
                               F17H_M01H_REPORTED_TEMP_CTRL, &regval);
  if (err)
    return err;

  temp = (regval >> 21) * 125;
  if (regval & F17H_TEMP_ADJUST_MASK)
    temp -= 49000;

  *val = temp;
  return 0;
}

static int get_ccd_temp(struct zenpower_data *data, u32 addr, long *val) {
  u32 regval;
  int err;

  err = data->read_amdsmn_addr(data->pdev, data->node_id, addr, &regval);
  if (err)
    return err;

  /*
   * Linux k10temp-compatible decode:
   * bits[10:0] hold the temperature in 0.125 C units with a -49 C offset;
   * bit 11 is a validity flag and must not be part of the value.
   */
  *val = (long)(regval & ZEN_CCD_TEMP_MASK) * 125L - 49000L;
  return 0;
}

/* ---- hwmon visibility ---------------------------------------------------- */

static umode_t zenpower_is_visible(const void *rdata,
                                   enum hwmon_sensor_types type, u32 attr,
                                   int channel) {
  const struct zenpower_data *data = rdata;

  switch (type) {
  case hwmon_temp:
    if (channel >= 2 && !data->ccd_visible[channel - 2])
      return 0;
    break;

  case hwmon_curr:
    if (!data->amps_visible)
      return 0;
    if (channel == 0 && !data->svi_core_addr)
      return 0;
    if (channel == 1 && !data->svi_soc_addr)
      return 0;
    break;

  case hwmon_in:
    if (channel == 0)
      return 0; /* padding */
    if (channel == 1 && !data->svi_core_addr)
      return 0;
    if (channel == 2 && !data->svi_soc_addr)
      return 0;
    break;

  case hwmon_power:
    if (channel <= 1 && !data->amps_visible)
      return 0;
    if (channel == 0 && !data->svi_core_addr)
      return 0;
    if (channel == 1 && !data->svi_soc_addr)
      return 0;
    if (channel == 2 && !data->ppt_visible)
      return 0;
    if (channel == 3 && !data->tdc_visible)
      return 0;
    if (channel == 4 && !data->edc_visible)
      return 0;
    if (channel == 5 && !data->rapl_available[RAPL_CHANNEL_PACKAGE])
      return 0;
    if (channel == 5 && attr == hwmon_power_input)
      return 0440;
    break;

  default:
    break;
  }

  return 0444;
}

/* ---- hwmon read ---------------------------------------------------------- */

static int zenpower_read(struct device *dev, enum hwmon_sensor_types type,
                         u32 attr, int channel, long *val) {
  struct zenpower_data *data = dev_get_drvdata(dev);
  u32 plane;
  int err;

  switch (type) {

  /* Temperatures */
  case hwmon_temp:
    if (attr == hwmon_temp_max) {
      /*
       * Tdie max = Tctl max - Tctl_offset.
       * AMD spec says Tctl max = 95°C for all Zen desktop/TR.
       * Tdie channel (ch0) must subtract the chip-specific offset.
       * Tctl channel (ch1) has no offset, so its max is always 95°C.
       * CCD channels have no published max; skip (handled by default).
       */
      if (channel == 0)
        *val = 95000 - data->temp_offset;
      else
        *val = 95000;
      return 0;
    }
    if (attr != hwmon_temp_input)
      return -EOPNOTSUPP;

    switch (channel) {
    case 0:
      err = get_ctl_temp(data, val);
      if (err)
        return err;
      *val -= data->temp_offset;
      break;
    case 1:
      err = get_ctl_temp(data, val);
      if (err)
        return err;
      break;
    case 2 ... 9:
      err = get_ccd_temp(data, ZEN_CCD_TEMP(channel - 2), val);
      if (err)
        return err;
      break;
    default:
      return -EOPNOTSUPP;
    }
    break;

  /* Voltage (hwmon_in uses 0-based indexing; pad ch0 to align with SVI) */
  case hwmon_in:
    if (channel == 0)
      return -EOPNOTSUPP;
    channel -= 1;
    fallthrough;

  /* Current */
  case hwmon_curr:
    if (attr != hwmon_in_input && attr != hwmon_curr_input)
      return -EOPNOTSUPP;

    switch (channel) {
    case 0:
      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->svi_core_addr, &plane);
      break;
    case 1:
      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->svi_soc_addr, &plane);
      break;
    default:
      return -EOPNOTSUPP;
    }
    if (err)
      return err;

    *val = (type == hwmon_in) ? plane_to_vcc_mv(plane, data)
                              : ((channel == 0) ? plane_core_ma(plane, data)
                                                : plane_soc_ma(plane, data));
    break;

  /* Power */
  case hwmon_power:
    if (attr != hwmon_power_input)
      return -EOPNOTSUPP;

    switch (channel) {
    /*
     * ch0 / ch1: SVI VRM rail power in uW.
     *
     * This is what the voltage regulator reports it is delivering.
     * It is NOT the same as PPT -- it includes VRM conversion losses
     * and does not reflect the SMU power governance limit.
     */
    case 0:
      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->svi_core_addr, &plane);
      if (err)
        return err;
      *val = safe_power_uw(plane_core_ma(plane, data),
                           plane_to_vcc_mv(plane, data));
      break;
    case 1:
      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->svi_soc_addr, &plane);
      if (err)
        return err;
      *val = safe_power_uw(plane_soc_ma(plane, data),
                           plane_to_vcc_mv(plane, data));
      break;

    /*
     * ch2: Package PPT -- SMU-governed whole-package power limit.
     *
     * This is silicon power as AMD defines it (what you see in
     * HWiNFO / Ryzen Master as "Package Power").  It is a different
     * physical quantity from the VRM rail readings above.
     *
     * Units: uW.  Converted from SMU register (integer mW or float W).
     */
    case 2:
      err = read_smu_power_uw(data, data->smu_ppt_addr, val);
      if (err)
        return err;
      break;

    /*
     * ch3: TDC proxy -- sustained current limit x current Vcore.
     *
     * The TDC register holds the sustained current ceiling in amps
     * (unsigned 8.3 fixed-point format in bits[10:0]).
     * We multiply by the live Vcore reading to produce a power proxy.
     * This is informational -- it represents headroom, not consumption.
     */
    case 3: {
      u32 tdc_raw, vcore_plane, tdc_ma, mv;

      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->smu_tdc_addr, &tdc_raw);
      if (err)
        return err;
      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->svi_core_addr, &vcore_plane);
      if (err)
        return err;
      tdc_ma = (tdc_raw == SMU_REG_SENTINEL_FF ||
                tdc_raw == SMU_REG_SENTINEL_00)
                   ? 0
                   : smu_current_limit_raw_to_ma(tdc_raw);
      mv = plane_to_vcc_mv(vcore_plane, data);
      *val = safe_power_uw(tdc_ma, mv);
      break;
    }

    /*
     * ch4: EDC proxy -- peak (electrical design current) limit x Vcore.
     *
     * Same construction as TDC but for the short-burst ceiling.
     * Represents peak allowable power envelope for boost transients.
     */
    case 4: {
      u32 edc_raw, vcore_plane, edc_ma, mv;

      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->smu_edc_addr, &edc_raw);
      if (err)
        return err;
      err = data->read_amdsmn_addr(data->pdev, data->node_id,
                                   data->svi_core_addr, &vcore_plane);
      if (err)
        return err;
      edc_ma = (edc_raw == SMU_REG_SENTINEL_FF ||
                edc_raw == SMU_REG_SENTINEL_00)
                   ? 0
                   : smu_current_limit_raw_to_ma(edc_raw);
      mv = plane_to_vcc_mv(vcore_plane, data);
      *val = safe_power_uw(edc_ma, mv);
      break;
    }

    /*
     * ch5: RAPL package power in uW.
     *
     * RAPL reports monotonically increasing energy counters; power is the
     * average rate between this read and the previous read for the channel.
     * This provides an MSR-backed package-power fallback/comparison path
     * when the CPU exposes AMD RAPL counters.
     */
    case 5:
      err = zenpower_rapl_read_power(data, RAPL_CHANNEL_PACKAGE, val);
      if (err)
        return err;
      break;

    default:
      return -EOPNOTSUPP;
    }
    break;

  default:
    return -EOPNOTSUPP;
  }

  return 0;
}

/* ---- Label tables -------------------------------------------------------- */

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

/*
 * Power channel labels match the channel indices in zenpower_read():
 *   [0] Core VRM rail power  (ch0)
 *   [1] SoC  VRM rail power  (ch1)
 *   [2] Package PPT          (ch2)
 *   [3] TDC x Vcore proxy    (ch3)
 *   [4] EDC x Vcore proxy    (ch4)
 *   [5] RAPL package power   (ch5)
 */
static const char *const zenpower_power_label[][6] = {
    {
        "SVI_P_Core",
        "SVI_P_SoC",
        "PPT_Package",
        "TDC_Core_proxy",
        "EDC_Core_proxy",
        "RAPL_P_Package",
    },
    {
        "cpu0 SVI_P_Core",
        "cpu0 SVI_P_SoC",
        "cpu0 PPT_Package",
        "cpu0 TDC_Core_proxy",
        "cpu0 EDC_Core_proxy",
        "cpu0 RAPL_P_Package",
    },
    {
        "cpu1 SVI_P_Core",
        "cpu1 SVI_P_SoC",
        "cpu1 PPT_Package",
        "cpu1 TDC_Core_proxy",
        "cpu1 EDC_Core_proxy",
        "cpu1 RAPL_P_Package",
    },
};

static int zenpower_read_labels(struct device *dev,
                                enum hwmon_sensor_types type, u32 attr,
                                int channel, const char **str) {
  struct zenpower_data *data;
  u8 i = 0;

  if (READ_ONCE(multicpu)) {
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

/* ---- Debug sysfs --------------------------------------------------------- */

static const u32 debug_addrs[] = {
    F17H_M01H_SVI + 0x08,  F17H_M01H_SVI + 0x0C,  F17H_M01H_SVI + 0x10,
    F17H_M01H_SVI + 0x14,  ZEN_SMU_PKG_PPT_ADDR,  ZEN_SMU_SOC_PPT_ADDR,
    ZEN_SMU_CORE_TDC_ADDR, ZEN_SMU_CORE_EDC_ADDR, ZEN_CCD_TEMP(0),
    ZEN_CCD_TEMP(1),       ZEN_CCD_TEMP(2),       ZEN_CCD_TEMP(3),
    ZEN_CCD_TEMP(4),       ZEN_CCD_TEMP(5),       ZEN_CCD_TEMP(6),
    ZEN_CCD_TEMP(7),
};

static ssize_t debug_data_show(struct device *dev,
                               struct device_attribute *attr, char *buf) {
  struct zenpower_data *data = dev_get_drvdata(dev);
  ssize_t len = 0;
  int i;
  int err;
  u32 raw;

#define DBGPR(fmt, ...)                                                        \
  len += scnprintf(buf + len, PAGE_SIZE - len, fmt, ##__VA_ARGS__)

  DBGPR("KERN_SUP:   %d\n", data->kernel_smn_support);
  DBGPR("NODE %u; CPU %u; N/CPU: %u\n", data->node_id, data->cpu_id,
        data->nodes_per_cpu);
  DBGPR("ZEN_GEN:    %d\n", (int)data->zen_gen);
  DBGPR("SVI_VER:    %d\n", (data->svi_ver == SVI_VER_3) ? 3 : 2);
  DBGPR("IS_APU:     %d\n", data->is_apu);
  DBGPR("AMPS_VIS:   %d\n", data->amps_visible);
  DBGPR("PPT_VIS:    %d\n", data->ppt_visible);
  DBGPR("TDC_VIS:    %d\n", data->tdc_visible);
  DBGPR("EDC_VIS:    %d\n", data->edc_visible);
  DBGPR("RAPL_INIT:  %d\n", data->rapl_initialized);
  DBGPR("RAPL_CPU:   %d\n", data->rapl_cpu);
  DBGPR("RAPL_PKG:   %d\n", data->rapl_available[RAPL_CHANNEL_PACKAGE]);
  DBGPR("RAPL_ESU:   %u\n", data->rapl_energy_unit);
  DBGPR("RAPL_BITS:  %u\n", data->rapl_counter_64bit ? 64 : 32);
  DBGPR("RAPL_POLL:  %u\n", data->rapl_accum_interval_ms);
  DBGPR("FLOAT_PWR:  %d\n", data->smu_float_power);
  DBGPR("SVI_CORE:   %08x\n", data->svi_core_addr);
  DBGPR("SVI_SOC:    %08x\n", data->svi_soc_addr);
  DBGPR("SMU_PPT:    %08x\n", data->smu_ppt_addr);
  DBGPR("SMU_TDC:    %08x\n", data->smu_tdc_addr);
  DBGPR("SMU_EDC:    %08x\n", data->smu_edc_addr);
  DBGPR("---\n");

  for (i = 0; i < ARRAY_SIZE(debug_addrs); i++) {
    if (len >= PAGE_SIZE - 20)
      break; /* stop before overrun */
    err = data->read_amdsmn_addr(data->pdev, data->node_id, debug_addrs[i],
                                 &raw);
    if (err)
      DBGPR("%08x = err %d\n", debug_addrs[i], err);
    else
      DBGPR("%08x = %08x\n", debug_addrs[i], raw);
  }

#undef DBGPR
  return len;
}

/* ---- hwmon channel descriptors ------------------------------------------- */

static const struct hwmon_channel_info *zenpower_info[] = {
    HWMON_CHANNEL_INFO(temp,
                       HWMON_T_INPUT | HWMON_T_MAX | HWMON_T_LABEL, /* Tdie  */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tctl  */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tccd1 */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tccd2 */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tccd3 */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tccd4 */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tccd5 */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tccd6 */
                       HWMON_T_INPUT | HWMON_T_LABEL,               /* Tccd7 */
                       HWMON_T_INPUT | HWMON_T_LABEL),              /* Tccd8 */

    HWMON_CHANNEL_INFO(in, HWMON_I_LABEL,              /* pad (ch0 hidden) */
                       HWMON_I_INPUT | HWMON_I_LABEL,  /* Core voltage     */
                       HWMON_I_INPUT | HWMON_I_LABEL), /* SoC  voltage     */

    HWMON_CHANNEL_INFO(curr, HWMON_C_INPUT | HWMON_C_LABEL, /* Core current */
                       HWMON_C_INPUT | HWMON_C_LABEL), /* SoC  current     */

    HWMON_CHANNEL_INFO(power,
                       HWMON_P_INPUT | HWMON_P_LABEL,  /* Core rail power  */
                       HWMON_P_INPUT | HWMON_P_LABEL,  /* SoC  rail power  */
                       HWMON_P_INPUT | HWMON_P_LABEL,  /* PPT package      */
                       HWMON_P_INPUT | HWMON_P_LABEL,  /* TDC proxy        */
                       HWMON_P_INPUT | HWMON_P_LABEL,  /* EDC proxy        */
                       HWMON_P_INPUT | HWMON_P_LABEL), /* RAPL package     */

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

/* ---- sysfs extras -------------------------------------------------------- */

static DEVICE_ATTR_RO(debug_data);

static struct attribute *zenpower_attrs[] = {&dev_attr_debug_data.attr, NULL};
static const struct attribute_group zenpower_group = {.attrs = zenpower_attrs};
__ATTRIBUTE_GROUPS(zenpower);

/* ---- probe --------------------------------------------------------------- */

static int zenpower_probe(struct pci_dev *pdev,
                          const struct pci_device_id *id) {
  struct device *dev = &pdev->dev;
  struct zenpower_data *data;
  struct device *hwmon_dev;
  struct pci_dev *misc;
  enum svi_version presumed_svi = SVI_VER_2;
  bool multinode;
  u8 node_of_cpu;
  int i, ccd_check = 0;
  u32 val;

  data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
  if (!data)
    return -ENOMEM;

  data->pdev = pdev;
  pci_set_drvdata(pdev, data);
  data->zen_gen = ZEN_GEN_1;
  data->svi_ver = SVI_VER_2;
  data->read_amdsmn_addr = nb_index_read;
  data->kernel_smn_support = false;
  data->amps_visible = false;
  data->ppt_visible = false;
  data->tdc_visible = false;
  data->edc_visible = false;
  data->smu_float_power = false;
  data->rapl_initialized = false;
  data->rapl_accum_running = false;
  data->rapl_counter_64bit = false;
  data->rapl_cpu = -1;
  data->rapl_accum_interval_ms = 0;
  data->rapl_energy_unit = 0;
  mutex_init(&data->rapl_lock);
  INIT_DELAYED_WORK(&data->rapl_accum_work, zenpower_rapl_accum_work);
  data->temp_offset = 0;
  data->node_id = 0;
  for (i = 0; i < MAX_CCD; i++)
    data->ccd_visible[i] = false;

  /* Prefer kernel amd_smn_read() */
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
    WRITE_ONCE(multicpu, true);

  /* Default SMU addresses -- overridden per generation where known */
  data->smu_ppt_addr = ZEN_SMU_PKG_PPT_ADDR;
  data->smu_tdc_addr = ZEN_SMU_CORE_TDC_ADDR;
  data->smu_edc_addr = ZEN_SMU_CORE_EDC_ADDR;

  /* ----------------------------------------------------------------
   * Per-family, per-model initialisation
   * ---------------------------------------------------------------- */

  if (boot_cpu_data.x86 == 0x17) {
    switch (boot_cpu_data.x86_model) {

    case 0x01: /* Zen  -- Summit Ridge / Naples EPYC */
    case 0x08: /* Zen+ -- Pinnacle Ridge             */
      data->zen_gen = ZEN_GEN_1;
      data->amps_visible = true;
      if (multinode) {
        if (node_of_cpu == 0)
          data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE0;
        if (node_of_cpu == 1)
          data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
      } else {
        data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
        data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
      }
      ccd_check = 4;
      break;

    case 0x11: /* Zen  APU -- Raven Ridge */
    case 0x18: /* Zen+ APU -- Picasso     */
      data->zen_gen = ZEN_GEN_1;
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
      break;

    case 0x31: /* Zen2 TR -- Castle Peak / Rome EPYC */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 TR/EPYC (%s coefficients)\n",
               zen1_calc ? "Zen1" : "Zen2");
      data->amps_visible = true;
      data->svi_core_addr = F17H_M30H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M30H_SVI_TEL_PLANE1;
      ccd_check = 8;
      break;

    case 0x60: /* Zen2 APU -- Renoir   */
    case 0x68: /* Zen2 APU -- Lucienne */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 Renoir/Lucienne APU (%s)\n",
               zen1_calc ? "Zen1" : "Zen2");
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F17H_M60H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M60H_SVI_TEL_PLANE1;
      break;

    case 0x71: /* Zen2 -- Matisse Ryzen 3000 */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 Matisse (%s coefficients)\n",
               zen1_calc ? "Zen1" : "Zen2");
      data->amps_visible = true;
      data->svi_core_addr = F17H_M70H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M70H_SVI_TEL_PLANE1;
      ccd_check = 8;
      break;

    case 0x90: /* Zen2 APU -- Van Gogh (Steam Deck) */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_2;
      dev_info(dev, "Zen2 Van Gogh APU (%s)\n", zen1_calc ? "Zen1" : "Zen2");
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F17H_M60H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M60H_SVI_TEL_PLANE1;
      break;

    default:
      dev_warn(dev, "Unknown 17h model 0x%02x -- Zen1 defaults\n",
               boot_cpu_data.x86_model);
      data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
      break;
    }

  } else if (boot_cpu_data.x86 == 0x19) {
    switch (boot_cpu_data.x86_model) {

    case 0x00 ... 0x01: /* Zen3 -- Milan EPYC / Chagall TR */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3 SP3/TR (%s coefficients)\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->amps_visible = true;
      data->svi_core_addr = F19H_M01H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M01H_SVI_TEL_PLANE1;
      ccd_check = 8;
      break;

    case 0x10 ... 0x11: /* Zen4 -- Genoa EPYC (SVI3) */
      data->zen_gen = ZEN_GEN_4;
      presumed_svi = SVI_VER_3;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M61H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F19H_M61H_SVI3_TEL_PLANE1;
      data->smu_float_power = true;
      ccd_check = 8;
      dev_info(dev, "Zen4 Genoa EPYC (SVI3)\n");
      break;

    case 0x21: /* Zen3 -- Vermeer Ryzen 5000 */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3 Vermeer (%s coefficients)\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->amps_visible = true;
      data->svi_core_addr = F19H_M21H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M21H_SVI_TEL_PLANE1;
      ccd_check = 2;
      break;

    case 0x40: /* Zen3+ APU -- Rembrandt   */
    case 0x44: /* Zen3+ APU -- Rembrandt-R */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3+ Rembrandt APU (%s)\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M50H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M50H_SVI_TEL_PLANE1;
      break;

    case 0x50: /* Zen3 APU -- Cezanne / Barcelo */
      data->zen_gen = zen1_calc ? ZEN_GEN_1 : ZEN_GEN_3;
      dev_info(dev, "Zen3 Cezanne/Barcelo APU (%s)\n",
               zen1_calc ? "Zen1" : "Zen2-compat");
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M50H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M50H_SVI_TEL_PLANE1;
      break;

    case 0x61: /* Zen4 desktop -- Raphael Ryzen 7000 (SVI3) */
      data->zen_gen = ZEN_GEN_4;
      presumed_svi = SVI_VER_3;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M61H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F19H_M61H_SVI3_TEL_PLANE1;
      data->smu_float_power = true;
      ccd_check = 2;
      dev_info(dev, "Zen4 Raphael desktop (SVI3)\n");
      break;

    case 0x70 ... 0x78: /* Zen4 APU -- Phoenix / Hawk Point (SVI3) */
      data->zen_gen = ZEN_GEN_4;
      presumed_svi = SVI_VER_3;
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F19H_M70H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F19H_M70H_SVI3_TEL_PLANE1;
      data->smu_float_power = true;
      dev_info(dev, "Zen4 Phoenix/HawkPoint APU (SVI3)\n");
      break;

    default:
      dev_warn(dev, "Unknown 19h model 0x%02x -- Zen3 defaults\n",
               boot_cpu_data.x86_model);
      data->zen_gen = ZEN_GEN_3;
      data->svi_core_addr = F19H_M21H_SVI_TEL_PLANE0;
      data->svi_soc_addr = F19H_M21H_SVI_TEL_PLANE1;
      break;
    }

  } else if (boot_cpu_data.x86 == 0x1a) {
    switch (boot_cpu_data.x86_model) {

    case 0x20 ... 0x24: /* Zen5 APU -- Strix Point / Strix Halo */
      data->zen_gen = ZEN_GEN_5;
      presumed_svi = SVI_VER_3;
      data->is_apu = true;
      data->amps_visible = true;
      data->svi_core_addr = F1AH_M20H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F1AH_M20H_SVI3_TEL_PLANE1;
      data->smu_float_power = true;
      dev_info(dev, "Zen5 Strix Point APU (SVI3)\n");
      break;

    case 0x44: /* Zen5 desktop -- Granite Ridge Ryzen 9000 (SVI3) */
      data->zen_gen = ZEN_GEN_5;
      presumed_svi = SVI_VER_3;
      data->amps_visible = true;
      data->svi_core_addr = F1AH_M44H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F1AH_M44H_SVI3_TEL_PLANE1;
      data->smu_float_power = true;
      ccd_check = 2;
      dev_info(dev, "Zen5 Granite Ridge desktop (SVI3)\n");
      break;

    default:
      dev_warn(dev, "Unknown 1Ah model 0x%02x -- Zen5 defaults\n",
               boot_cpu_data.x86_model);
      data->zen_gen = ZEN_GEN_5;
      presumed_svi = SVI_VER_3;
      data->svi_core_addr = F1AH_M20H_SVI3_TEL_PLANE0;
      data->svi_soc_addr = F1AH_M20H_SVI3_TEL_PLANE1;
      data->smu_float_power = true;
      break;
    }

  } else {
    dev_warn(dev, "Unknown CPU family 0x%02x -- Zen1 defaults\n",
             boot_cpu_data.x86);
    data->svi_core_addr = F17H_M01H_SVI_TEL_PLANE0;
    data->svi_soc_addr = F17H_M01H_SVI_TEL_PLANE1;
  }

  /* ---- SVI protocol auto-detect --------------------------------------- */
  data->svi_ver = detect_svi_version(data, presumed_svi);

  /* ---- CCD detection -------------------------------------------------- */
  for (i = 0; i < ccd_check; i++) {
    if (!data->read_amdsmn_addr(pdev, data->node_id, ZEN_CCD_TEMP(i), &val) &&
        (val & ZEN_CCD_TEMP_VALID))
      data->ccd_visible[i] = true;
  }

  /* ---- Tctl offset ---------------------------------------------------- */
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

  /* ---- SMU register usability ----------------------------------------- */
  /*
   * Only expose power3..5 when the backing SMU register returns a
   * non-sentinel, physically plausible value in the expected units.  Hides the
   * channels gracefully on chips or firmware versions that leave the scratch
   * space uninitialised.
   */
  data->ppt_visible = smu_power_reg_is_usable(data, data->smu_ppt_addr);
  data->tdc_visible =
      smu_current_limit_is_usable(data, data->smu_tdc_addr) &&
      data->svi_core_addr != 0;
  data->edc_visible =
      smu_current_limit_is_usable(data, data->smu_edc_addr) &&
      data->svi_core_addr != 0;

  /*
   * AMD RAPL package energy is socket-scoped and readable from any core in the
   * package.  On multi-node packages, expose it only from the first DF node so
   * userspace does not see duplicate package-power channels.
   */
  if (node_of_cpu == 0) {
    int rapl_err = zenpower_rapl_init(data, dev);

    if (rapl_err)
      dev_dbg(dev, "RAPL unavailable (%d)\n", rapl_err);
  } else {
    dev_dbg(dev, "RAPL skipped on package node %u\n",
            (unsigned int)node_of_cpu);
  }

  /* ---- Register with hwmon -------------------------------------------- */
  hwmon_dev = devm_hwmon_device_register_with_info(
      dev, "zenpower", data, &zenpower_chip_info, zenpower_groups);
  if (IS_ERR(hwmon_dev))
    return PTR_ERR(hwmon_dev);

  zenpower_rapl_start(data);

  return 0;
}

static void zenpower_remove(struct pci_dev *pdev) {
  struct zenpower_data *data = pci_get_drvdata(pdev);

  if (data)
    zenpower_rapl_stop(data);
}

/* ---- PCI ID table -------------------------------------------------------- */

static const struct pci_device_id zenpower_id_table[] = {
    /* Family 17h – Zen / Zen+ / Zen2 */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_DF_F3)}, /* Zen   SP3/Naples     */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M10H_DF_F3)}, /* Zen+  Raven Ridge    */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M20H_DF_F3)}, /* Zen   Raven (alt)    */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M30H_DF_F3)}, /* Zen2  Castle Peak TR */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_M60H_DF_F3)}, /* Zen2  Renoir APU */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_17H_M68H_DF_F3)}, /* Zen2  Lucienne APU   */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_M70H_DF_F3)}, /* Zen2  Matisse */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_17H_M90H_DF_F3)}, /* Zen2  Van Gogh */
    /* Family 19h – Zen3 / Zen3+ / Zen4 */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_DF_F3)}, /* Zen3  Milan EPYC     */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_M10H_DF_F3)}, /* Zen4  Genoa EPYC */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_M21H_DF_F3)}, /* Zen3  Vermeer */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_19H_M40H_DF_F3)}, /* Zen3+ Rembrandt APU  */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_19H_M50H_DF_F3)}, /* Zen3  Cezanne APU    */
    {PCI_VDEVICE(AMD, PCI_DEVICE_ID_AMD_19H_M61H_DF_F3)}, /* Zen4  Raphael */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_19H_M70H_DF_F3)}, /* Zen4  Phoenix APU    */
    /* Family 1Ah – Zen5 */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_1AH_M20H_DF_F3)}, /* Zen5  Strix Point    */
    {PCI_VDEVICE(AMD,
                 PCI_DEVICE_ID_AMD_1AH_M44H_DF_F3)}, /* Zen5  Granite Ridge  */
    {}};
MODULE_DEVICE_TABLE(pci, zenpower_id_table);

static struct pci_driver zenpower_driver = {
    .name = "zenpower",
    .id_table = zenpower_id_table,
    .probe = zenpower_probe,
    .remove = zenpower_remove,
};

module_pci_driver(zenpower_driver);
