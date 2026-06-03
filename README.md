# zen-pstate
zen pstate is a CPPC based cpufreq driver that works by modifying amd CPPC registers and outputting the optimal value to them
# Installing
you can install with make like this
```
  make
```
or if you compiled the kernel with clang use
```
make LLVM=1
```
# Contributing
contributions are very welcome but make sure that you compile and test them first
# Warning
This has only been tested on amd zen 2 cpus if you can independently test or verify that it works on other generations that will be appreciated, Its current status is experimental and you shouldn't use it unless it patche a specific error with your bios , besides that try adding to your cmdline
```
  amd-pstate=active
```
