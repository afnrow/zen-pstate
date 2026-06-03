#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "zen_pstate.h"
#include <asm/msr-index.h>
#include <asm/msr.h>
#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>

MODULE_DESCRIPTION("A cpufreq scaling driver for zen 2 mendocino chips");
MODULE_LICENSE("GPL");

static int boost = 1;
module_param(boost, int, 0644);
MODULE_PARM_DESC(boost, "Enabling and Disabling CPU Boost");

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
  if (READ_ONCE(data->suspend)) {
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
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, 8);
  ret = wrmsrq_on_cpu(policy->cpu, MSR_AMD_CPPC_REQ, req);
  WRITE_ONCE(data->suspend, 0);
  return ret;
}

static u8 freq_to_perf(struct zen_pstate_data *data, unsigned int freq) {
  unsigned int min_freq = 400000; // Eventually phase these out in favour of
                                  // better perf or epp selected by schedutil
  unsigned int max_freq = 4300000;
  if (freq <= min_freq)
    return data->min_perf;
  if (freq >= max_freq)
    return data->max_perf;
  return data->min_perf +
         ((freq - min_freq) * (data->max_perf - data->min_perf) /
          (max_freq - min_freq));
}

static int zen_pstate_suspend(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  WRITE_ONCE(data->suspend, 1);
  return 0;
}

static int zen_pstate_verify(struct cpufreq_policy_data *policy) {
  cpufreq_verify_within_limits(policy, 400000,
                               4300000); // Still hardcoded
  return 0;
}

static int zen_pstate_online(struct cpufreq_policy *policy) {
  return enable_cppc();
}

static int zen_pstate_offline(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  u64 req = 0;
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->min_perf);
  req |=
      FIELD_PREP(AMD_CPPC_DES_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK,
                    EPP_POWERSAVE);
  wrmsrq_on_cpu(policy->cpu, MSR_AMD_CPPC_REQ, req);
  return 0;
}

static int zen_pstate_set_policy(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  u64 req = 0;
  u8 epp, perf;
  perf = freq_to_perf(data, policy->max);
  if (policy->policy == CPUFREQ_POLICY_PERFORMANCE)
    epp = EPP_PERFORMANCE;
  else
    epp = EPP_BALANCED_POWERSAVE;
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, data->min_perf);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, data->max_perf);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, epp);
  wrmsrq_on_cpu(policy->cpu, MSR_AMD_CPPC_REQ, req);
  return 0;
}

static void zen_pstate_update_limits(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  u64 req = 0;
  u8 perf;
  u8 epp = 8;
  perf = freq_to_perf(data, policy->max);
  req |= FIELD_PREP(AMD_CPPC_MIN_PERF_MASK, policy->min);
  req |= FIELD_PREP(AMD_CPPC_MAX_PERF_MASK, policy->max);
  req |= FIELD_PREP(AMD_CPPC_DES_PERF_MASK, perf);
  req |= FIELD_PREP(AMD_CPPC_EPP_PERF_MASK, epp);
  wrmsrq_on_cpu(policy->cpu, MSR_AMD_CPPC_REQ, req);
}

static int zen_pstate_set_boost(struct cpufreq_policy *policy, int state) {
  struct zen_pstate_data *data = policy->driver_data;
  u64 cap1;
  rdmsrl(MSR_AMD_CPPC_CAP1, cap1);
  if (state) {
    data->max_perf = FIELD_GET(AMD_CPPC_HIGHEST_PERF_MASK, cap1);
  } else {
    data->max_perf = FIELD_GET(AMD_CPPC_NOMINAL_PERF_MASK, cap1);
  }
  zen_pstate_update_limits(policy);
  return 0;
}

static int zen_pstate_cpu_init(struct cpufreq_policy *policy) {
  if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD && boot_cpu_data.x86 >= 0x19) {
    pr_info("A non AMD CPU or pre Zen 2 archetichure");
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
  spin_lock_init(&data->lock);
  data->min_perf = FIELD_GET(AMD_CPPC_LOWEST_PERF_MASK, cap1);
  data->max_perf = FIELD_GET(AMD_CPPC_HIGHEST_PERF_MASK, cap1);
  data->boost = boost;
  policy->driver_data = data;
  policy->freq_table = NULL;
  policy->cpuinfo.min_freq =
      400000; // currently hardcoded plaaning on making them dynamic
  policy->cpuinfo.max_freq = 4300000;
  policy->min = policy->cpuinfo.min_freq;
  policy->max = policy->cpuinfo.max_freq;
  policy->cpuinfo.transition_latency =
      ZEN_PSTATE_TRANSITION_LATENCY; // Defined as per amd-pstate
  policy->transition_delay_us = ZEN_PSTATE_TRANSITION_DELAY;
  return 0;
}
static void zen_pstate_cpu_exit(struct cpufreq_policy *policy) {
  struct zen_pstate_data *data = policy->driver_data;
  kfree(data);
  policy->driver_data = NULL;
}

static struct cpufreq_driver zen_pstate_cpufreq = {
    .flags = CPUFREQ_NEED_UPDATE_LIMITS | CPUFREQ_CONST_LOOPS,
    .name = "zen-pstate",
    .verify = zen_pstate_verify,
    .init = zen_pstate_cpu_init,
    .suspend = zen_pstate_suspend,
    .resume = zen_pstate_resume,
    .offline = zen_pstate_offline,
    .online = zen_pstate_online,
    .update_limits = zen_pstate_update_limits,
    .setpolicy = zen_pstate_set_policy,
    .set_boost = zen_pstate_set_boost,
    .exit = zen_pstate_cpu_exit,
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
