#include <asm-generic/errno-base.h>
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#ifndef rdmsrl_safe_on_cpu
#define rdmsrl_safe_on_cpu(cpu, reg, val) rdmsrq_safe_on_cpu(cpu, reg, val)
#define wrmsrl_safe_on_cpu(cpu, reg, val) wrmsrq_safe_on_cpu(cpu, reg, val)
#endif

struct zen_pstate_data {
  u8 min_perf;
  u8 max_perf;
};

static struct cpufreq_frequency_table table[] = {
    {.frequency = 800000, .driver_data = 16},
    {.frequency = 1200000, .driver_data = 22},
    {.frequency = 1600000, .driver_data = 80},
    {.frequency = 1900000, .driver_data = 95},
    {.frequency = 2500000, .driver_data = 122},
    {.frequency = 2900000, .driver_data = 132},
    {.frequency = 3200000, .driver_data = 166},
    {.frequency = CPUFREQ_TABLE_END},
};

static const struct dmi_system_id zen_pstate_dmi_table[] = {
    {
        .ident = "HP Mendocino System",
        .matches =
            {
                DMI_MATCH(DMI_BOARD_VENDOR, "HP"),
            },
    },
    {}};

MODULE_DESCRIPTION(
    "A cpufreq scaling driver for zen 2 mendocino chips (thanks hp)");
MODULE_LICENSE("GPL");

static int zen_pstate_target(struct cpufreq_policy *policy,
                             unsigned int index) {
  struct zen_pstate_data *data = policy->driver_data;
  unsigned int perf_level = policy->freq_table[index].driver_data;
  u64 req = 0;
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf_level);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, 0x80);
  pr_info("Writing 0x%llx to %d", req, MSR_AMD_CPPC_REQ);
  wrmsrl(MSR_AMD_CPPC_REQ, req);
  return 0;
}

static unsigned int zen_pstate_fast_switch(struct cpufreq_policy *policy,
                                           unsigned int requested_freq) {
  struct zen_pstate_data *data = policy->driver_data;
  struct cpufreq_frequency_table *table = policy->freq_table;
  unsigned int i;
  unsigned int perf_level = 0;
  u64 req = 0;
  for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++) {
    if (table[i].frequency >= requested_freq) {
      perf_level = table[i].driver_data;
      break;
    }
  }
  if (perf_level == 0) {
    for (i = 0; table[i].frequency != CPUFREQ_TABLE_END; i++)
      ;
    perf_level = table[i - 1].driver_data;
  }
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf_level);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, 0x80);
  wrmsrl(MSR_AMD_CPPC_REQ, req);
  return table[i].frequency;
}

static inline int enable_cppc(void) {
  u64 val;
  rdmsrl(MSR_AMD_CPPC_ENABLE, val);
  if (val & 1) {
    pr_info("CPPC already enabled by BIOS.\n");
    return 0;
  }
  pr_info("Attempting to force-enable CPPC...\n");
  wrmsrl(MSR_AMD_CPPC_ENABLE, 1);
  rdmsrl(MSR_AMD_CPPC_ENABLE, val);
  if (!(val & 1)) {
    pr_err("Force-enable failed. Hardware gate is hard-locked.\n");
    return -EPERM;
  }
  pr_info("CPPC force-enabled successfully.\n");
  return 0;
}

static int zen_pstate_cpu_init(struct cpufreq_policy *policy) {
  if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD) {
    return -ENODEV;
  }
  if (enable_cppc() != 0)
    return -ENODEV;
  u64 cap1;
  rdmsrl(MSR_AMD_CPPC_CAP1, cap1);
  struct zen_pstate_data *data = kzalloc(sizeof(*data), GFP_KERNEL);
  data->min_perf = FIELD_GET(AMD_CPPC_LOWEST_PERF_MASK, cap1);
  data->max_perf = FIELD_GET(AMD_CPPC_HIGHEST_PERF_MASK, cap1);
  policy->driver_data = data;
  policy->freq_table = table;
  return 0;
}
static void zen_pstate_cpu_exit(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  kfree(data);
  policy->driver_data = NULL;
}

static struct cpufreq_driver zen_pstate_cpufreq = {
    .name = "zen-pstate",
    .verify = cpufreq_generic_frequency_table_verify,
    .init = zen_pstate_cpu_init,
    .target_index = zen_pstate_target,
    .fast_switch = zen_pstate_fast_switch,
    .get = NULL,
    .exit = zen_pstate_cpu_exit,
    .attr = NULL,
};

static int __init zen_pstate_module_init(void) {
  if (!dmi_check_system(zen_pstate_dmi_table)) {
    pr_info("Non-HP system detected, proceeding with caution.\n");
  }
  return cpufreq_register_driver(&zen_pstate_cpufreq);
}

static void __exit zen_pstate_module_exit(void) {
  cpufreq_unregister_driver(&zen_pstate_cpufreq);
}

module_init(zen_pstate_module_init);
module_exit(zen_pstate_module_exit);
