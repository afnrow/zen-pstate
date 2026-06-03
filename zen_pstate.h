#ifndef __LINUX_ZEN_PSTATE_H
#define __LINUX_ZEN_PSTATE_H

#include <linux/spinlock.h>

struct zen_pstate_data {
  spinlock_t lock;
  u8 min_perf;
  u8 max_perf;
  int suspend;
  int boost;
};

#define ZEN_PSTATE_TRANSITION_LATENCY 20000
#define ZEN_PSTATE_TRANSITION_DELAY 1000

#define EPP_POWERSAVE 0xFF
#define EPP_BALANCED_POWERSAVE 0xBF
#define EPP_BALANCED_PERFORMANCE 0x80
#define EPP_PERFORMANCE 0x00

#endif
