#include <asm/msr-index.h>
#include <asm/msr.h>
#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>

#define ZEN_PSTATE_TRANSITION_LATENCY 20000
#define ZEN_PSTATE_TRANSITION_DELAY 1000

struct zen_pstate_data {
  u8 min_perf;
  u8 max_perf;
  u8 last_epp;
  u8 last_des_perf;
  int suspended;
};

static inline int get_epp_hint(unsigned int frequency) {
  if (frequency <= 2000000) // Sets EPP hint 0xFF is powersave 0x80 is balanced
                            // performance 0x00 is performance 0xBF is balanced
                            // powersave as per amd-pstate.c
    return 0xFF;
  else if (frequency <= 2800000)
    return 0xBF;
  else if (frequency <= 3500000)
    return 0x80;
  return 0x00;
}

MODULE_DESCRIPTION("A cpufreq scaling driver for zen 2 mendocino chips");
MODULE_LICENSE("GPL");

static int enable_cppc(void) {
  u64 val;
  rdmsrl(MSR_AMD_CPPC_ENABLE, val);
  if (val & 1) {
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

static int zen_pstate_resume(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  u64 req = 0;
  int ret;
  if (data->suspended) {
    u64 val;
    rdmsrl(MSR_AMD_CPPC_ENABLE, val);
    if (val & 1) {
      goto request;
    } else {
      if (enable_cppc() != 0) {
        pr_info("Couldnt enable CPPC, Shutting down");
        return -EPERM;
      }
      goto request;
    }
  }
  return 0;
request:
  req &= ~(AMD_CPPC_MAX_PERF_MASK | AMD_CPPC_MIN_PERF_MASK |
           AMD_CPPC_DES_PERF_MASK | AMD_CPPC_EPP_PERF_MASK);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, data->last_des_perf);
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, data->last_epp);
  ret = wrmsrq_on_cpu(policy->cpu, MSR_AMD_CPPC_REQ ,req);
  return ret;
}

static int zen_pstate_suspend(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  data->suspended = 1;
  return 0;
}

static inline int get_desired_perf(unsigned int frequency) {
  return 8 + (((frequency - 400000) * (166 - 8)) / (4300000 - 400000));
}

static int zen_pstate_verify(struct cpufreq_policy_data *policy) {
  cpufreq_verify_within_limits(policy, 400000, 4300000); // Still hardcoded 
  return 0;
}

static int zen_pstate_target(struct cpufreq_policy *policy,
                             unsigned int target_freq,
                             unsigned int relation) {
  struct zen_pstate_data *data = policy->driver_data;
  unsigned int perf_level = get_desired_perf(target_freq);
  u64 req = 0;
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf_level);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK,
                    get_epp_hint(target_freq));
  wrmsrq_on_cpu(policy->cpu, MSR_AMD_CPPC_REQ ,req);
  policy->cur = target_freq;
  return 0;
}

static unsigned int zen_pstate_fast_switch(struct cpufreq_policy *policy,
                                           unsigned int requested_freq) {
  struct zen_pstate_data *data = policy->driver_data;
  u64 req = 0;
  unsigned int perf_level = get_desired_perf(requested_freq);
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf_level);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, get_epp_hint(requested_freq));
  wrmsrl(MSR_AMD_CPPC_REQ, req);
  policy->cur = requested_freq;
  return requested_freq;
}

static int zen_pstate_cpu_init(struct cpufreq_policy *policy) {
  if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD) {
    pr_info("Not an AMD Cpu");
    return -ENODEV;
  }
  if (enable_cppc() != 0) {
    pr_info("Enabling CPPC Failed");
    return -ENODEV;
  }
  u64 cap1;
  rdmsrl(MSR_AMD_CPPC_CAP1, cap1);
  struct zen_pstate_data *data = kzalloc(sizeof(*data), GFP_KERNEL);
  if (!data) {
    pr_info("Kzalloc failed\n");
    return -ENOMEM;
  }
  data->min_perf = FIELD_GET(AMD_CPPC_LOWEST_PERF_MASK, cap1);
  data->max_perf = FIELD_GET(AMD_CPPC_HIGHEST_PERF_MASK, cap1);
  policy->driver_data = data;
  policy->freq_table = NULL;
  policy->cpuinfo.min_freq =
      400000; // currently hardcoded plaaning on making them dynamic
  policy->cpuinfo.max_freq = 4300000;
  policy->min = policy->cpuinfo.min_freq;
  policy->max = policy->cpuinfo.max_freq;
  policy->cpuinfo.transition_latency = ZEN_PSTATE_TRANSITION_LATENCY; // Defined as per amd-pstate
  policy->transition_delay_us = ZEN_PSTATE_TRANSITION_DELAY;
  return 0;
}
static void zen_pstate_cpu_exit(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  kfree(data);
  policy->driver_data = NULL;
}

static struct cpufreq_driver zen_pstate_cpufreq = {
    .name = "zen-pstate",
    .verify = zen_pstate_verify,
    .init = zen_pstate_cpu_init,
    .suspend = zen_pstate_suspend,
    .resume = zen_pstate_resume,
    .target = zen_pstate_target,
    .fast_switch = zen_pstate_fast_switch,
    .get = NULL,
    .exit = zen_pstate_cpu_exit,
    .attr = NULL,
};

static int __init zen_pstate_module_init(void) {
  int ret;
  ret = cpufreq_register_driver(&zen_pstate_cpufreq);
  if (ret != 0)
    pr_err("Initalization failed with error code %d\n", ret);
  return ret;
}

static void __exit zen_pstate_module_exit(void) {
  cpufreq_unregister_driver(&zen_pstate_cpufreq);
}

module_init(zen_pstate_module_init);
module_exit(zen_pstate_module_exit);
