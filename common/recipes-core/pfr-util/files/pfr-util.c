/*
 * pfr-util.c
 *
 * Copyright 2015-present Facebook. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdio.h>
#include <syslog.h>
#include <getopt.h>
#include <openssl/sha.h>
#include <openbmc/obmc-i2c.h>
#include <openbmc/pal.h>

#define PFR_PROVISION    (1 << 0)
#define PFR_RD_PROVISION (1 << 1)
#define PFR_ST_HISTORY   (1 << 2)

#define PFM_B0_MAGIC 0xB6EAFD19
#define PFM_B0_SIZE  0x10
#define PFM_RT_ENTRY 0x90
#define PFM_RT_MAGIC 0xA757A046
#define PFM_RT_SIZE  0x70
#define PFM_PUBRK_X  0x10
#define PFM_PUBRK_Y  0x40

#define BMC_PFM_OFFSET 0x029e0000
#define BMC_RC_OFFSET  0x02a00000
#define BMC_STG_OFFSET 0x04a00000

#define PCH_PFM_OFFSET 0x02ff0000
#define PCH_RC_OFFSET  0x01bf0000
#define PCH_STG_OFFSET 0x007f0000

#if defined(PFR_MAILBOX_BUS)
#define PFR_BUS  PFR_MAILBOX_BUS
#define PFR_ADDR PFR_MAILBOX_ADDR
#else
#define PFR_BUS  4
#define PFR_ADDR 0xb0
#endif

#define INIT_ST_TBL(tbl, idx, str) tbl[idx] = str

enum {
  PC_PFR_BMC_PFM = 3,
};

enum {
  ERASE_PROVISION = 0x00,  // erase current provision
  PROV_ROOT_KEY   = 0x01,  // provision rook key hash
  PROV_PCH_OFFSET = 0x05,  // provision PCH offset
  PROV_BMC_OFFSET = 0x06,  // provision BMC offset
  LOCK_UFM        = 0x07,  // end of provision
  READ_ROOT_KEY   = 0x08,
  READ_PCH_OFFSET = 0x0C,
  READ_BMC_OFFSET = 0x0D,
};

enum {
  EXECUTE_CMD = 0x01,  // execute command
  FLUSH_FIFO  = 0x02,  // flush read/write FIFO
};

enum {
  ERR_NONE       = 0,
  ERR_UFM_LOCKED = 1,
  ERR_CMD_ERROR  = 2,
  ERR_CMD_BUSY   = 3,
  ERR_GET_PUBRK  = 4,
  ERR_UNKNOWN
};

static bool verbose = false;
static const char *st_table[256] = {0};

static const char *prov_err_str[ERR_UNKNOWN] = {
  "",
  "UFM Locked",
  "Command Error",
  "Command Busy",
  "Failed to get Root Key Hash"
};

static const char *option_list[] = {
  "--provision",
  "--read_provision",
  "--state_history",
  "--verbose"
};

static void
print_usage_help(void) {
  int i;

  printf("Usage: pfr-util <option>\n");
  printf("       option:\n");
  for (i = 0; i < sizeof(option_list)/sizeof(option_list[0]); i++) {
    printf("         %s\n", option_list[i]);
  }
}

static int
i2c_rw(int fd, uint8_t *tbuf, uint8_t tcnt, uint8_t *rbuf, uint8_t rcnt) {
  int ret = -1;
  int retry = 2;
  uint8_t buf[16];

  do {
    memcpy(buf, tbuf, tcnt);
    if (!(ret = i2c_rdwr_msg_transfer(fd, PFR_ADDR, buf, tcnt, rbuf, rcnt))) {
      break;
    }

    syslog(LOG_WARNING, "%s: i2c rw failed, offset: %02x", __func__, buf[0]);
    if (--retry > 0)
      msleep(20);
  } while (retry > 0);

  return ret;
}

static int
pfr_get_pubrk_hash(uint8_t *shasum) {
  FILE *fp;
  char rbuf[256], mtd_name[32], dev[16] = {0};
  uint8_t *rk_buf, *pubrk_x, *pubrk_y;
  int mtd_no, i;

  if ((fp = fopen("/proc/mtd", "r"))) {
    while (fgets(rbuf, sizeof(rbuf), fp)) {
      if ((sscanf(rbuf, "mtd%d: %*x %*x %s", &mtd_no, mtd_name) == 2) &&
          !strcmp("\"pfm\"", mtd_name)) {
        sprintf(dev, "/dev/mtd%d", mtd_no);
        break;
      }
    }
    fclose(fp);
  }

  if (!dev[0] || !(fp = fopen(dev, "rb"))) {
    printf("pfm not found\n");
    return -1;
  }

  fseek(fp, 0, SEEK_SET);
  if (fread(rbuf, 1, PFM_B0_SIZE, fp) != PFM_B0_SIZE) {
    syslog(LOG_ERR, "%s: read PFM block0 failed", __func__);
    return -1;
  }

  if ((*(uint32_t *)rbuf != PFM_B0_MAGIC) || (rbuf[8] != PC_PFR_BMC_PFM)) {
    printf("PFM block0 not found\n");
    return -1;
  }

  fseek(fp, PFM_RT_ENTRY, SEEK_SET);
  rk_buf = (uint8_t *)&rbuf[64];
  if (fread(rk_buf, 1, PFM_RT_SIZE, fp) != PFM_RT_SIZE) {
    syslog(LOG_ERR, "%s: read PFM root-entry failed", __func__);
    return -1;
  }

  if (*(uint32_t *)rk_buf != PFM_RT_MAGIC) {
    printf("PFM root-entry not found\n");
    return -1;
  }

  // reversed-pubrk_x + reversed-pubrk_y
  pubrk_x = rk_buf + PFM_PUBRK_X;
  pubrk_y = rk_buf + PFM_PUBRK_Y;
  for (i = 0; i < 32; i++) {
    rbuf[i] = pubrk_x[31-i];
    rbuf[32+i] = pubrk_y[31-i];
  }
  SHA256((uint8_t *)rbuf, 64, shasum);

  return 0;
}

static int
pfr_mailbox_cmd(int fd, uint8_t *tbuf, uint8_t tcnt, uint8_t *rbuf, uint8_t rcnt) {
  int ret = -1;
  int i, wait = 50;
  uint8_t buf[8];

  switch (tbuf[0]) {
    case 0x0A:  // wait cmd done
      for (i = 0; i < wait; i++) {
        msleep(10);
        if (!(ret = i2c_rw(fd, tbuf, 1, rbuf, 1)) && ((rbuf[0]&0x7) == 0x2))  // CMD_DONE
          break;
      }
      if (i >= wait) {
        if (!ret) {
          if (rbuf[0] & 0x04) {
            ret = (rbuf[0] & 0x10) ? ERR_UFM_LOCKED : ERR_CMD_ERROR;
          } else if (rbuf[0] & 0x01) {
            ret = ERR_CMD_BUSY;
          } else {
            ret = -1;
          }
        }

        syslog(LOG_WARNING, "%s: wait cmd done timeout", __func__);
        return ret;
      }
      break;

    case 0x0D:  // write to FIFO
      for (i = 0; i < tcnt; i++) {
        buf[0] = tbuf[0];
        buf[1] = tbuf[i+1];
        if ((ret = i2c_rw(fd, buf, 2, rbuf, 0))) {
          syslog(LOG_WARNING, "%s: write 0x%02x 0x%02x failed", __func__, buf[0], buf[1]);
          return ret;
        }
      }
      break;

    case 0x0E:  // read from FIFO
      for (i = 0; i < rcnt; i++) {
        if ((ret = i2c_rw(fd, tbuf, 1, &rbuf[i], 1))) {
          syslog(LOG_WARNING, "%s: read 0x%02x failed", __func__, tbuf[0]);
          return ret;
        }
      }
      break;

    default:
      if ((ret = i2c_rw(fd, tbuf, tcnt, rbuf, rcnt))) {
        syslog(LOG_WARNING, "%s: cmd 0x%02x failed", __func__, tbuf[0]);
        return ret;
      }
      break;
  }

  return ret;
}

static int
pfr_provision_cmd(int fd, uint8_t prov, uint8_t *tbuf, uint8_t tcnt, uint8_t *rbuf, uint8_t rcnt) {
  int ret = -1;
  uint8_t rd_prov = 0xFF;
  uint8_t buf[64];

  switch (prov) {
    case ERASE_PROVISION:
    case LOCK_UFM:
      break;
    case PROV_ROOT_KEY:
      rd_prov = READ_ROOT_KEY;
      break;
    case PROV_PCH_OFFSET:
      rd_prov = READ_PCH_OFFSET;
      break;
    case PROV_BMC_OFFSET:
      rd_prov = READ_BMC_OFFSET;
      break;
    default:
      return ret;
  }

  do {
    if (!tbuf) {
      break;
    }

    // provision
    if (tcnt) {
      buf[0] = 0x0c;
      buf[1] = FLUSH_FIFO;  // flush write FIFO
      if ((ret = pfr_mailbox_cmd(fd, buf, 2, rbuf, 0))) {
        return ret;
      }

      buf[0] = 0x0a;
      if ((ret = pfr_mailbox_cmd(fd, buf, 1, rbuf, 0))) {
        return ret;
      }

      buf[0] = 0x0d;  // write to FIFO
      memcpy(&buf[1], tbuf, tcnt);
      if ((ret = pfr_mailbox_cmd(fd, buf, tcnt, rbuf, 0))) {
        return ret;
      }
    }

    buf[0] = 0x0b;
    buf[1] = prov;
    if ((ret = pfr_mailbox_cmd(fd, buf, 2, rbuf, 0))) {
      return ret;
    }

    buf[0] = 0x0c;
    buf[1] = EXECUTE_CMD;
    if ((ret = pfr_mailbox_cmd(fd, buf, 2, rbuf, 0))) {
      return ret;
    }

    buf[0] = 0x0a;
    if ((ret = pfr_mailbox_cmd(fd, buf, 1, rbuf, 0))) {
      return ret;
    }
  } while (0);

  if (!rcnt) {
    return ret;
  }

  // read provisioned content
  buf[0] = 0x0b;
  buf[1] = rd_prov;
  if ((ret = pfr_mailbox_cmd(fd, buf, 2, rbuf, 0))) {
    return ret;
  }

  buf[0] = 0x0c;
  buf[1] = EXECUTE_CMD;
  if ((ret = pfr_mailbox_cmd(fd, buf, 2, rbuf, 0))) {
    return ret;
  }

  buf[0] = 0x0a;
  if ((ret = pfr_mailbox_cmd(fd, buf, 1, rbuf, 0))) {
    return ret;
  }

  buf[0] = 0x0e;  // read from FIFO
  if ((ret = pfr_mailbox_cmd(fd, buf, 1, rbuf, rcnt))) {
    return ret;
  }

  return ret;
}

static int
pfr_provision(int fd) {
  int ret = -1;
  uint8_t shasum[SHA256_DIGEST_LENGTH], i;
  uint8_t tbuf[64], rbuf[64];

  if (pfr_get_pubrk_hash(shasum)) {
    syslog(LOG_ERR, "%s: get root-key-hash failed", __func__);
    return ERR_GET_PUBRK;
  }

  // erase provision in UFM
  if ((ret = pfr_provision_cmd(fd, ERASE_PROVISION, tbuf, 0, rbuf, 0))) {
    syslog(LOG_ERR, "%s: erase provision failed", __func__);
    return ret;
  }

  // provision root-key-hash
  if ((ret = pfr_provision_cmd(fd, PROV_ROOT_KEY, shasum, 32, rbuf, 32))) {
    syslog(LOG_ERR, "%s: provision root-key-hash failed", __func__);
    return ret;
  }

  if (verbose) {
    printf("Root Key Hash --\n");
    for (i = 0; i < 32; i++) {
      if (i && !(i % 16)) {
        printf("\n");
      }
      printf("%02x ", rbuf[i]);
    }
    printf("\n\n");
  }

  if (memcmp(rbuf, shasum, sizeof(shasum))) {
    syslog(LOG_ERR, "%s: root-key-hash mismatched!", __func__);
    return -1;
  }

  // provision pch-offset
  *(uint32_t *)tbuf = PCH_PFM_OFFSET;
  *(uint32_t *)(tbuf+4) = PCH_RC_OFFSET;
  *(uint32_t *)(tbuf+8) = PCH_STG_OFFSET;
  if ((ret = pfr_provision_cmd(fd, PROV_PCH_OFFSET, tbuf, 12, rbuf, 12))) {
    syslog(LOG_ERR, "%s: provision pch-offset failed", __func__);
    return ret;
  }

  if (verbose) {
    printf("PCH Offset --\n");
    printf("Active PFM Offset: %08X\n", *(uint32_t *)rbuf);
    printf("Recovery Offset  : %08X\n", *(uint32_t *)(rbuf+4));
    printf("Staging Offset   : %08X\n\n", *(uint32_t *)(rbuf+8));
  }

  if (memcmp(rbuf, tbuf, 12)) {
    syslog(LOG_ERR, "%s: pch-offset mismatched!", __func__);
    return -1;
  }

  // provision bmc-offset
  *(uint32_t *)tbuf = BMC_PFM_OFFSET;
  *(uint32_t *)(tbuf+4) = BMC_RC_OFFSET;
  *(uint32_t *)(tbuf+8) = BMC_STG_OFFSET;
  if ((ret = pfr_provision_cmd(fd, PROV_BMC_OFFSET, tbuf, 12, rbuf, 12))) {
    syslog(LOG_ERR, "%s: provision bmc-offset failed", __func__);
    return ret;
  }

  if (verbose) {
    printf("BMC Offset --\n");
    printf("Active PFM Offset: %08X\n", *(uint32_t *)rbuf);
    printf("Recovery Offset  : %08X\n", *(uint32_t *)(rbuf+4));
    printf("Staging Offset   : %08X\n\n", *(uint32_t *)(rbuf+8));
  }

  if (memcmp(rbuf, tbuf, 12)) {
    syslog(LOG_ERR, "%s: bmc-offset mismatched!", __func__);
    return -1;
  }

  // lock UFM
  if ((ret = pfr_provision_cmd(fd, LOCK_UFM, tbuf, 0, rbuf, 0))) {
    syslog(LOG_ERR, "%s: lock UFM failed", __func__);
    return ret;
  }

  return ret;
}

static int
pfr_read_provision(int fd) {
  int ret = -1;
  uint8_t rbuf[64], i;

  // read root-key-hash
  if ((ret = pfr_provision_cmd(fd, PROV_ROOT_KEY, NULL, 0, rbuf, 32))) {
    syslog(LOG_ERR, "%s: read root-key-hash failed", __func__);
    return ret;
  }

  printf("Root Key Hash --\n");
  for (i = 0; i < 32; i++) {
    if (i && !(i % 16)) {
      printf("\n");
    }
    printf("%02x ", rbuf[i]);
  }
  printf("\n\n");

  // read pch-offset
  if ((ret = pfr_provision_cmd(fd, PROV_PCH_OFFSET, NULL, 0, rbuf, 12))) {
    syslog(LOG_ERR, "%s: read pch-offset failed", __func__);
    return ret;
  }

  printf("PCH Offset --\n");
  printf("Active PFM Offset: %08X\n", *(uint32_t *)rbuf);
  printf("Recovery Offset  : %08X\n", *(uint32_t *)(rbuf+4));
  printf("Staging Offset   : %08X\n\n", *(uint32_t *)(rbuf+8));

  // read bmc-offset
  if ((ret = pfr_provision_cmd(fd, PROV_BMC_OFFSET, NULL, 0, rbuf, 12))) {
    syslog(LOG_ERR, "%s: read bmc-offset failed", __func__);
    return ret;
  }

  printf("BMC Offset --\n");
  printf("Active PFM Offset: %08X\n", *(uint32_t *)rbuf);
  printf("Recovery Offset  : %08X\n", *(uint32_t *)(rbuf+4));
  printf("Staging Offset   : %08X\n\n", *(uint32_t *)(rbuf+8));

  return ret;
}

static int
pfr_state_history(int fd) {
  uint8_t tbuf[8], rbuf[80];
  uint8_t i, idx;

  for (i = 0; i < 64; i += 16) {
    tbuf[0] = 0xC0 + i;
    if (i2c_rw(fd, tbuf, 1, &rbuf[i], 16)) {
      syslog(LOG_ERR, "%s: read state-history failed", __func__);
      return -1;
    }
  }

  // Platform State
  // CPLD Nios firmware T-1 flow
  INIT_ST_TBL(st_table, 0x01, "CPLD_NIOS_WAITING_TO_START");
  INIT_ST_TBL(st_table, 0x02, "CPLD_NIOS_STARTED");
  INIT_ST_TBL(st_table, 0x03, "ENTER_T-1");
  INIT_ST_TBL(st_table, 0x06, "BMC_FLASH_AUTHENTICATION");
  INIT_ST_TBL(st_table, 0x07, "PCH_FLASH_AUTHENTICATION");
  INIT_ST_TBL(st_table, 0x08, "AUTHENTICATION_FAILED_LOCKDOWN");
  INIT_ST_TBL(st_table, 0x09, "ENTER_T0");
  // Timed boot progress
  INIT_ST_TBL(st_table, 0x0A, "T0_BMC_BOOTED");
  INIT_ST_TBL(st_table, 0x0B, "T0_ME_BOOTED");
  INIT_ST_TBL(st_table, 0x0C, "T0_ACM_BOOTED");
  INIT_ST_TBL(st_table, 0x0D, "T0_BIOS_BOOTED");
  INIT_ST_TBL(st_table, 0x0E, "T0_BOOT_COMPLETE");
  // Update event
  INIT_ST_TBL(st_table, 0x10, "PCH_FW_UPDATE");
  INIT_ST_TBL(st_table, 0x11, "BMC_FW_UPDATE");
  INIT_ST_TBL(st_table, 0x12, "CPLD_UPDATE");
  INIT_ST_TBL(st_table, 0x13, "CPLD_UPDATE_IN_RECOVERY_MODE");
  // Recovery
  INIT_ST_TBL(st_table, 0x40, "T-1_FW_RECOVERY");
  INIT_ST_TBL(st_table, 0x41, "T-1_FORCED_ACTIVE_FW_RECOVERY");
  INIT_ST_TBL(st_table, 0x42, "WDT_TIMEOUT_RECOVERY");
  INIT_ST_TBL(st_table, 0x43, "CPLD_RECOVERY_IN_RECOVERY_MODE");
  // PIT
  INIT_ST_TBL(st_table, 0x44, "PIT_L1_LOCKDOWN");
  INIT_ST_TBL(st_table, 0x45, "PIT_L2_FW_SEALED");
  INIT_ST_TBL(st_table, 0x46, "PIT_L2_PCH_HASH_MISMATCH_LOCKDOWN");
  INIT_ST_TBL(st_table, 0x47, "PIT_L2_BMC_HASH_MISMATCH_LOCKDOWN");
  // Quanta front panel LED control
  INIT_ST_TBL(st_table, 0xF1, "CPLD_RECOVERY_FAILED_LOCKDOWN");
  INIT_ST_TBL(st_table, 0xF2, "BMC_WDT_RECOVERY_3TIME_FAILED");

  // Last Panic
  INIT_ST_TBL(st_table, 0x51, "PCH_UPDATE_INTENT");
  INIT_ST_TBL(st_table, 0x52, "BMC_UPDATE_INTENT");
  INIT_ST_TBL(st_table, 0x53, "BMC_RESET_DETECTED");
  INIT_ST_TBL(st_table, 0x54, "BMC_WDT_EXPIRED");
  INIT_ST_TBL(st_table, 0x55, "ME_WDT_EXPIRED");
  INIT_ST_TBL(st_table, 0x56, "ACM_WDT_EXPIRED");
  INIT_ST_TBL(st_table, 0x57, "IBB_WDT_EXPIRED");
  INIT_ST_TBL(st_table, 0x58, "OBB_WDT_EXPIRED");
  INIT_ST_TBL(st_table, 0x59, "ACM_IBB_OBB_AUTH_FAILED");

  // Last Recovery
  INIT_ST_TBL(st_table, 0x61, "PCH_ACTIVE_FAIL");
  INIT_ST_TBL(st_table, 0x62, "PCH_RECOVERY_FAIL");
  INIT_ST_TBL(st_table, 0x63, "ME_LAUNCH_FAIL");
  INIT_ST_TBL(st_table, 0x64, "ACM_LAUNCH_FAIL");
  INIT_ST_TBL(st_table, 0x65, "IBB_LAUNCH_FAIL");
  INIT_ST_TBL(st_table, 0x66, "OBB_LAUNCH_FAIL");
  INIT_ST_TBL(st_table, 0x67, "BMC_ACTIVE_FAIL");
  INIT_ST_TBL(st_table, 0x68, "BMC_RECOVERY_FAIL");
  INIT_ST_TBL(st_table, 0x69, "BMC_LAUNCH_FAIL");
  INIT_ST_TBL(st_table, 0x6A, "FORCED_ACTIVE_FW_RECOVERY");

  // BMC auth
  INIT_ST_TBL(st_table, 0x71, "BMC_AUTH_FAILED, AUTH_ACTIVE");
  INIT_ST_TBL(st_table, 0x72, "BMC_AUTH_FAILED, AUTH_RECOVERY");
  INIT_ST_TBL(st_table, 0x73, "BMC_AUTH_FAILED, AUTH_ACTIVE_AND_RECOVERY");
  INIT_ST_TBL(st_table, 0x74, "BMC_AUTH_FAILED, AUTH_ALL_REGIONS");

  // PCH auth
  INIT_ST_TBL(st_table, 0x81, "PCH_AUTH_FAILED, AUTH_ACTIVE");
  INIT_ST_TBL(st_table, 0x82, "PCH_AUTH_FAILED, AUTH_RECOVERY");
  INIT_ST_TBL(st_table, 0x83, "PCH_AUTH_FAILED, AUTH_ACTIVE_AND_RECOVERY");
  INIT_ST_TBL(st_table, 0x84, "PCH_AUTH_FAILED, AUTH_ALL_REGIONS");

  // Update from PCH
  INIT_ST_TBL(st_table, 0x91, "UPDATE_FROM_PCH_FAILED, INVALID_UPDATE_INTENT");
  INIT_ST_TBL(st_table, 0x92, "UPDATE_FROM_PCH_FAILED, FW_UPDATE_INVALID_SVN");
  INIT_ST_TBL(st_table, 0x93, "UPDATE_FROM_PCH_FAILED, FW_UPDATE_AUTH_FAILED");
  INIT_ST_TBL(st_table, 0x94, "UPDATE_FROM_PCH_FAILED, FW_UPDATE_EXCEEDED_MAX_FAILED_ATTEMPTS");
  INIT_ST_TBL(st_table, 0x95, "UPDATE_FROM_PCH_FAILED, ACTIVE_FW_UPDATE_NOT_ALLOWED");
  INIT_ST_TBL(st_table, 0x96, "UPDATE_FROM_PCH_FAILED, RECOVERY_FW_UPDATE_AUTH_FAILED");
  INIT_ST_TBL(st_table, 0xA0, "UPDATE_FROM_PCH_FAILED, CPLD_UPDATE_INVALID_SVN");
  INIT_ST_TBL(st_table, 0xA1, "UPDATE_FROM_PCH_FAILED, CPLD_UPDATE_AUTH_FAILED");
  INIT_ST_TBL(st_table, 0xA2, "UPDATE_FROM_PCH_FAILED, CPLD_UPDATE_EXCEEDED_MAX_FAILED_ATTEMPTS");

  // Update from BMC
  INIT_ST_TBL(st_table, 0xB1, "UPDATE_FROM_BMC_FAILED, INVALID_UPDATE_INTENT");
  INIT_ST_TBL(st_table, 0xB2, "UPDATE_FROM_BMC_FAILED, FW_UPDATE_INVALID_SVN");
  INIT_ST_TBL(st_table, 0xB3, "UPDATE_FROM_BMC_FAILED, FW_UPDATE_AUTH_FAILED");
  INIT_ST_TBL(st_table, 0xB4, "UPDATE_FROM_BMC_FAILED, FW_UPDATE_EXCEEDED_MAX_FAILED_ATTEMPTS");
  INIT_ST_TBL(st_table, 0xB5, "UPDATE_FROM_BMC_FAILED, ACTIVE_FW_UPDATE_NOT_ALLOWED");
  INIT_ST_TBL(st_table, 0xB6, "UPDATE_FROM_BMC_FAILED, RECOVERY_FW_UPDATE_AUTH_FAILED");
  INIT_ST_TBL(st_table, 0xC0, "UPDATE_FROM_BMC_FAILED, CPLD_UPDATE_INVALID_SVN");
  INIT_ST_TBL(st_table, 0xC1, "UPDATE_FROM_BMC_FAILED, CPLD_UPDATE_AUTH_FAILED");
  INIT_ST_TBL(st_table, 0xC2, "UPDATE_FROM_BMC_FAILED, CPLD_UPDATE_EXCEEDED_MAX_FAILED_ATTEMPTS");

  if (rbuf[0] > rbuf[1]) {
    rbuf[1] += 64;
  }
  printf("Offset  Code    Description\n");
  printf("================================================================================\n");
  for (i = rbuf[0]; i <= rbuf[1]; i++) {
    idx = i % 64;
    if ((idx > 1) && rbuf[idx] && st_table[rbuf[idx]]) {
      printf("[%02X]    (0x%02X)  %s\n", 0xC0+idx, rbuf[idx], st_table[rbuf[idx]]);
    }
  }

  return 0;
}

int
main(int argc, char **argv) {
  int ret = -1, retry = 0;
  int opt, ifd = 0;
  uint32_t operations = 0;
  uint8_t tbuf[8], rbuf[8];
  static struct option long_opts[] = {
    {"provision", no_argument, 0, 'p'},
    {"read_provision", no_argument, 0, 'r'},
    {"state_history", no_argument, 0, 's'},
    {"verbose", no_argument, 0, 'v'},
    {0,0,0,0},
  };

  do {
    if (argc < 2) {
      break;
    }

    while ((opt = getopt_long(argc, argv, "prsv", long_opts, NULL)) != -1) {
      switch (opt) {
        case 'p':
          operations |= PFR_PROVISION;
          break;
        case 'r':
          operations |= PFR_RD_PROVISION;
          break;
        case 's':
          operations |= PFR_ST_HISTORY;
          break;
        case 'v':
          verbose = true;
          break;
        default:
          printf("Unknown option\n");
          break;
      }
    }
    if (!operations) {
      break;
    }

    if ((ifd = i2c_cdev_slave_open(PFR_BUS, (PFR_ADDR>>1), I2C_SLAVE_FORCE_CLAIM)) < 0) {
      printf("open i2c%d failed\n", PFR_BUS);
      return ret;
    }

    tbuf[0] = 0x00;
    if ((ret = i2c_rw(ifd, tbuf, 1, rbuf, 1))) {
      printf("access mailbox failed, bus=%d, addr=0x%02X\n", PFR_BUS, PFR_ADDR);
      close(ifd);
      return ret;
    }

    do {
      if (operations & PFR_PROVISION) {
        do {
          ret = pfr_provision(ifd);
          if (ret && retry) {
            msleep(100);
            syslog(LOG_WARNING, "retry pfr_provision");
          }
        } while (ret && (retry-- > 0));

        printf("provision %s", (!ret)?"succeeded":"failed");
        if ((ret > 0) && (ret < ERR_UNKNOWN)) {
          printf(" (%s)", prov_err_str[ret]);
        }
        printf("\n");
        break;
      }

      if (operations & PFR_RD_PROVISION) {
        if ((ret = pfr_read_provision(ifd))) {
          printf("read provision failed\n");
        }
        break;
      }

      if (operations & PFR_ST_HISTORY) {
        if ((ret = pfr_state_history(ifd))) {
          printf("get state history failed\n");
        }
        break;
      }
    } while (0);

    if (ifd > 0) {
      close(ifd);
    }
    return ret;
  } while (0);

  print_usage_help();
  return ret;
}
