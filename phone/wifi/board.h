/*
 * The SoC differences wland cares about: Pixel 4 (sm8150) by default, the Galaxy A9 2018 (sdm660) with
 * -DBOARD_A9Y (tools/a9y/wifi/build.sh). Addresses from each phone's stock DT.
 */
#ifndef BOARD_H
#define BOARD_H

#ifdef BOARD_A9Y
#define APCS_IPC		0x17911008ull	/* apcs_glb (qcom,sdm660-apcs-hmss-global) + 8 */
#define IPC_MODEM_SMP2P		14
#define IPC_MODEM_GLINK		15		/* glink_modem: mboxes = <&apcs_glb 15> */
#define MSA_PA			0x85700000ull	/* wlan_msa_mem */
/*
 * XNU manages only RAM from 4 GB up here, so pm_alloc memory is out of the modem's reach: the rmtfs
 * buffer (and its 4K guards) sits in RAM below 4 GB that XNU leaves alone, after the radio's pool.
 */
#define RMTFS_FIXED_PA		0xe1001000ull
#define FW_ROOT			"/usr/local/fw"	/* written by a9ydrop (tools/a9y/drop) */
#else
#define APCS_IPC		0x17c0000cull	/* apss_shared + 12: write BIT(n) */
#define IPC_MODEM_SMP2P		14
#define IPC_MODEM_GLINK		12
#define MSA_PA			0x8bc00000ull	/* pil_wlan_fw_region */
#define FW_ROOT			"/var/local/fw"
#endif

#endif
