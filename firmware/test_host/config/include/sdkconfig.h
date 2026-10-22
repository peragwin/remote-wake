#pragma once
/* run.sh compiles each binary with an explicit relay default; nothing here
 * mirrors the project's Kconfig choice. */
#ifndef CONFIG_RW_RELAY_URL
#error "build with -DCONFIG_RW_RELAY_URL=..."
#endif
