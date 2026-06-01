savedcmd_zen_pstate.mod := printf '%s\n'   zen_pstate.o | awk '!x[$$0]++ { print("./"$$0) }' > zen_pstate.mod
