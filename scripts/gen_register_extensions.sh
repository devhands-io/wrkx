#!/bin/sh
# Generate obj/register_extensions.c — the extension dispatch shim.
# Usage: gen_register_extensions.sh [fn1 fn2 ...]
# With no arguments, generates an empty (no-op) dispatcher.
#
# The generated file is included in the wrkx binary build; it calls each
# active extension's init function in the order given on the command line.

printf '#include "wrkx_extension.h"\n'
for fn in "$@"; do
    printf 'void %s(const wrkx_extension_api *);\n' "$fn"
done
printf 'void wrkx_register_all_extensions(const wrkx_extension_api *api) {\n'
printf '    (void)api;\n'
for fn in "$@"; do
    printf '    %s(api);\n' "$fn"
done
printf '}\n'
