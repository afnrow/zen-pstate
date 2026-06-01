#include <asm/msr-index.h>
#include <asm/msr.h>
#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

struct zen_pstate_data {
  u8 min_perf;
  u8 max_perf;
};

struct cpufreq_frequency_table table[] = {
    {.frequency = 800000, .driver_data = 8},
    {.frequency = 1221000, .driver_data = 20},
    {.frequency = 1643000, .driver_data = 40},
    {.frequency = 2065000, .driver_data = 60},
    {.frequency = 2486000, .driver_data = 80},
    {.frequency = 2908000, .driver_data = 100},
    {.frequency = 3330000, .driver_data = 120},
    {.frequency = 3751000, .driver_data = 140},
    {.frequency = 4173000, .driver_data = 160},
    {.frequency = CPUFREQ_TABLE_END},
};

static inline int get_epp_hint(unsigned int frequency) {
  if (frequency <= 2500000) // Sets EPP hint 0xFF is powersave 0x80 is balanced
                            // 0x00 is performance
    return 0xFF;
  else if (frequency <= 3500000)
    return 0x80;
  return 0x00;
}

MODULE_DESCRIPTION("A cpufreq scaling driver for zen 2 mendocino chips");
MODULE_LICENSE("GPL");

static int zen_pstate_target(struct cpufreq_policy *policy,
                             unsigned int index) {
  struct zen_pstate_data *data = policy->driver_data;
  unsigned int perf_level = policy->freq_table[index].driver_data;
  u64 req = 0;
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf_level);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK,
                    get_epp_hint(policy->freq_table[index].frequency));
  wrmsrl(MSR_AMD_CPPC_REQ, req);
  return 0;
}

static unsigned int zen_pstate_fast_switch(struct cpufreq_policy *policy,
                                           unsigned int requested_freq) {
  struct zen_pstate_data *data = policy->driver_data;
  struct cpufreq_frequency_table *table = policy->freq_table;
  unsigned int i = 0, best_i = 0;
  u64 req = 0;
  while (table[i].frequency != CPUFREQ_TABLE_END) {
    if (table[i].frequency >= requested_freq &&
        table[best_i].frequency < requested_freq) {
      best_i = i;
    }
    i++;
  }
  unsigned int perf_level = table[best_i].driver_data;
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf_level);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, get_epp_hint(requested_freq));
  wrmsrl(MSR_AMD_CPPC_REQ, req);
  return table[best_i].frequency;
}

static inline int enable_cppc(void) {
  u64 val;
  rdmsrl(MSR_AMD_CPPC_ENABLE, val);
  if (val & 1) {
    pr_info("CPPC already enabled by BIOS\n");
    return 0;
  }
  pr_info("Attempting to force-enable CPPC... \n");
  wrmsrl(MSR_AMD_CPPC_ENABLE, 1);
  rdmsrl(MSR_AMD_CPPC_ENABLE, val);
  if (!(val & 1)) {
    pr_err("Forcing CPPC failed. Hardware gate is locked\n");
    return -EPERM;
  }
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
  return cpufreq_register_driver(&zen_pstate_cpufreq);
}

static void __exit zen_pstate_module_exit(void) {
  cpufreq_unregister_driver(&zen_pstate_cpufreq);
}

module_init(zen_pstate_module_init);
module_exit(zen_pstate_module_exit);
