#ifndef _MINOS_PRINT_H_
#define _MINOS_PRINT_H_

void log_init(void);

int level_print(char *fmt, ...);

#define	pr_debug(...)	level_print("[04]DEBUG : " __VA_ARGS__)
#define pr_info(...)	level_print("[03]INFO  : " __VA_ARGS__)
#define pr_warning(...)	level_print("[02]WARN  : " __VA_ARGS__)
#define pr_error(...)	level_print("[01]ERROR : " __VA_ARGS__)
#define pr_fatal(...)	level_print("[00]FATAL : " __VA_ARGS__)

#define printf(...)	level_print("[03]INFO  : " __VA_ARGS__)

#endif
