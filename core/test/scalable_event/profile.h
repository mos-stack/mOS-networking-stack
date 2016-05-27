#ifndef __PROFILE_H_
#define __PROFILE_H_

#include <sys/time.h>

#ifndef PROFILE_MAX_VAR
#define PROFILE_MAX_VAR 20
#endif

#ifdef PROFILE_STATIC
#define STATIC static
#else
#define STATIC
#endif

#ifdef PROFILE_ON

#define PROFILE_LOCK() \
	__lock = __sync_lock_test_and_set(&__st_lock, 1)

#define PROFILE_UNLOCK() \
	if (__lock == 0) __sync_lock_release(&__st_lock)

#define PROFILE_INIT() \
	STATIC struct timeval __tv_total_from, __tv_total_to; \
STATIC long __c_total_us = 0, __c_etc_us = 0; \
STATIC long *__var_list[PROFILE_MAX_VAR] = {NULL}; \
STATIC char *__name_list[PROFILE_MAX_VAR] = {NULL}; \
char __lock = 0; \
static char __st_lock = 0

#define PROFILE_START() \
	do { \
		if (__lock == 0) gettimeofday(&__tv_total_from, NULL); \
	} while (0)

#define PROFILE_END() \
	do {\
		if (__lock == 0) { \
			gettimeofday(&__tv_total_to, NULL); \
			__c_total_us \
			+= (__tv_total_to.tv_sec - __tv_total_from.tv_sec) * 1000000 \
			+ (__tv_total_to.tv_usec - __tv_total_from.tv_usec); \
		} \
	} while (0)

#define PROFILE_VAR(var) \
	STATIC struct timeval __##var##_tv_from, __##var##_tv_to; \
STATIC long __##var##_us = 0; \
STATIC int __iter_##var = -1; \
{if (__lock == 0 && __iter_##var == -1) \
	for (__iter_##var = 0; __iter_##var < PROFILE_MAX_VAR; __iter_##var++) \
	if (__var_list[__iter_##var] == NULL) { \
		__var_list[__iter_##var] = &__##var##_us; \
		__name_list[__iter_##var] = (char *)#var; break;}}

#define PROFILE_FROM(var) \
	do { \
		if (__lock == 0) \
		gettimeofday(&__##var##_tv_from, NULL); \
	} while (0)

#define PROFILE_TO(var) \
	do { \
		if (__lock == 0) { \
			gettimeofday(&__##var##_tv_to, NULL); \
			__##var##_us \
			+= (__##var##_tv_to.tv_sec - __##var##_tv_from.tv_sec) * 1000000 \
			+ (__##var##_tv_to.tv_usec - __##var##_tv_from.tv_usec); \
		} \
	} while (0)

#define PROFILE_PRINT(fp) \
	do { int i; \
		if (__lock == 0) { \
			__c_etc_us = __c_total_us; \
			for (i = 0; i < PROFILE_MAX_VAR; i++) \
			if (__var_list[i]) __c_etc_us -= *__var_list[i]; \
			fprintf(fp, "[PROFILE %s]\n", __func__); \
			for (i = 0; i < PROFILE_MAX_VAR; i++) \
			if (__var_list[i] != NULL) { \
				printf("                              : %11.3lf ms (%5.2lf %%)", \
						(double)*__var_list[i] / 1000.0, \
						100 * (double)*__var_list[i] / (double)__c_total_us); \
				printf("\r%s\n", __name_list[i]); \
			} \
			fprintf(fp, "etc. (includes the overhead)  : %11.3lf ms (%5.2lf %%)\n", \
					(double)__c_etc_us / 1000.0, \
					100 * (double)__c_etc_us / (double)__c_total_us); \
			fprintf(fp, "total                         : %11.3lf ms\n", \
					(double)__c_total_us / 1000.0); \
			fflush(fp); \
		} \
	} while (0)

#else /* PROFILE_ON */
#define PROFILE_LOCK()
#define PROFILE_UNLOCK()
#define PROFILE_INIT()
#define PROFILE_START()
#define PROFILE_END()
#define PROFILE_VAR(args...)
#define PROFILE_FROM(args...)
#define PROFILE_TO(args...)
#define PROFILE_PRINT(args...)
#endif /* PROFILE_ON */

#endif /* __PROFILE_H_ */
