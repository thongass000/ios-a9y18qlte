/* The sdm660 modem's self-authenticating boot (mss_a9y.c). */
#ifndef MSS_H
#define MSS_H
int mss_boot(void);		/* 0: authenticated and out of reset */
void mss_shutdown(void);
#endif
