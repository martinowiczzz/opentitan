// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "usbdpi.h"

const char *decode_pid[] = {
    "Rsvd",     //         0000b
    "OUT",      //         0001b
    "ACK",      //         0010b
    "DATA0",    //         0011b
    "PING",     //         0100b
    "SOF",      //         0101b
    "NYET",     //         0110b
    "DATA2",    //         0111b
    "SPLIT",    //         1000b
    "IN",       //         1001b
    "NAK",      //         1010b
    "DATA1",    //         1011b
    "PRE/ERR",  //         1100b
    "SETUP",    //         1101b
    "STALL",    //         1110b
    "MDATA"     //         1111b
};

#define MS_IDLE 0
#define MS_GET_PID 1
#define MS_GET_BYTES 2

#define M_NONE 0
#define M_HOST 1
#define M_DEVICE 2

#define SE0 0
#define DK 1
#define DJ 2

#define MON_BYTES_SIZE 1024

// Number of bytes in max output buffer line
#define MAX_OBUF 80

struct usb_monitor_ctx {
  FILE *file;
  int state;
  int driver;
  int pu;
  int line;
  int rawbits;
  int bits;
  int needbits;
  int byte;
  int sopAt;
  int lastpid;
  unsigned char bytes[MON_BYTES_SIZE + 2];
};

/**
 * Create and initialise a USB monitor instance
 */
usb_monitor_ctx_t *usb_monitor_init(const char *filename) {
  usb_monitor_ctx_t *mon =
      (usb_monitor_ctx_t *)calloc(1, sizeof(usb_monitor_ctx_t));
  assert(mon);

  mon->state = MS_IDLE;
  mon->driver = M_NONE;
  mon->pu = 0;
  mon->line = 0;

  mon->file = fopen(filename, "w");
  if (!mon->file) {
    fprintf(stderr, "USBDPI: Unable to open monitor file at %s: %s\n", filename,
            strerror(errno));
    free(mon);
    return NULL;
  }

  // more useful for tail -f
  setlinebuf(mon->file);
  printf(
      "\nUSBDPI: Monitor output file created at %s. Works well with tail:\n"
      "$ tail -f %s\n",
      filename, filename);

  return mon;
}

/**
 * Finalise a USB monitor
 */
void usb_monitor_fin(usb_monitor_ctx_t *mon) {
  fclose(mon->file);
  free(mon);
}

/**
 * Append a formatted message to the USB monitor log file
 */
void usb_monitor_log(usb_monitor_ctx_t *ctx, const char *fmt, ...) {
  char obuf[MAX_OBUF];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(obuf, MAX_OBUF, fmt, ap);
  va_end(ap);
  size_t written = fwrite(obuf, sizeof(char), (size_t)n, ctx->file);
  assert(written == (size_t)n);
}

#define DR_SIZE 128
static char dr[DR_SIZE];
char *pid_2data(int pid, unsigned char d0, unsigned char d1) {
  int comp_crc = CRC5((d1 & 7) << 8 | d0, 11);
  const char *crcok = (comp_crc == d1 >> 3) ? "OK" : "BAD";

  switch (pid) {
    case USB_PID_SOF:
      snprintf(dr, DR_SIZE, "SOF %03x (CRC5 %02x %s)", (d1 & 7) << 8 | d0,
               d1 >> 3, crcok);
      break;

    case USB_PID_SETUP:
    case USB_PID_OUT:
    case USB_PID_IN:
      snprintf(dr, DR_SIZE, "%s %d.%d (CRC5 %02x %s)", decode_pid[pid & 0xf],
               d0 & 0x7f, (d1 & 7) << 1 | d0 >> 7, d1 >> 3, crcok);
      break;

    case USB_PID_DATA0:
    case USB_PID_DATA1:
      snprintf(dr, DR_SIZE, "%s %02x, %02x (%s)", decode_pid[pid & 0xf], d0, d1,
               (d0 | d1) ? "CRC16 BAD" : "NULL");
      break;

    // Validate and log other PIDs
    default:
      if (((pid >> 4) ^ 0xf) == (pid & 0xf)) {
        snprintf(dr, DR_SIZE, "%s %02x, %02x (CRC5 %s)", decode_pid[pid & 0xf],
                 d0, d1, crcok);
      } else {
        snprintf(dr, DR_SIZE, "BAD PID %s %02x, %02x (CRC5 %s)",
                 decode_pid[pid & 0xf], d0, d1, crcok);
      }
      break;
  }
  return dr;
}

/**
 * Per-cycle monitoring of the USB
 */
void usb_monitor(usb_monitor_ctx_t *mon, int loglevel, int tick, bool hdrive,
                 uint32_t p2d, uint32_t d2p, uint8_t *lastpid) {
  int dp, dn;
  int log, compact;
  log = (loglevel & 0x2);
  compact = (loglevel & 0x1);

  assert(mon);

  if ((d2p & D2P_DP_EN) || (d2p & D2P_DN_EN) || (d2p & D2P_D_EN)) {
    if (hdrive) {
      fprintf(mon->file, "mon: %8d: Bus clash\n", tick);
    }
    if (d2p & D2P_TX_USE_D_SE0) {
      if ((d2p & D2P_SE0) || !(d2p & D2P_D_EN)) {
        dp = 0;
        dn = 0;
      } else {
        dp = (d2p & D2P_D) ? 1 : 0;
        dn = (d2p & D2P_D) ? 0 : 1;
      }
    } else {
      dp = ((d2p & D2P_DP_EN) && (d2p & D2P_DP)) ? 1 : 0;
      dn = ((d2p & D2P_DN_EN) && (d2p & D2P_DN)) ? 1 : 0;
    }
    mon->driver = M_DEVICE;
  } else if (hdrive) {
    if (d2p & D2P_RX_ENABLE) {
      if (p2d & (P2D_DP | P2D_DN)) {
        dp = (p2d & P2D_D) ? 1 : 0;
        dn = (p2d & P2D_D) ? 0 : 1;
      } else {
        dp = 0;
        dn = 0;
      }
    } else {
      dp = (p2d & P2D_DP) ? 1 : 0;
      dn = (p2d & P2D_DN) ? 1 : 0;
    }
    mon->driver = M_HOST;
  } else {
    if ((mon->driver != M_NONE) || (mon->pu != (d2p & D2P_PU))) {
      if (log) {
        if (d2p & D2P_PU) {
          fprintf(mon->file, "mon: %8d: Idle, FS resistor (d2p 0x%x)\n", tick,
                  d2p);
        } else {
          fprintf(mon->file, "mon: %8d: Idle, SE0\n", tick);
        }
      }
      mon->driver = M_NONE;
      mon->pu = (d2p & D2P_PU);
    }
    mon->line = 0;
    return;
  }
  // If the DN pullup is there then swap
  if (d2p & D2P_DNPU) {
    int tmp = dp;
    dp = dn;
    dn = tmp;
  }
  mon->line = (mon->line << 2) | dp << 1 | dn;

  if (mon->state == MS_IDLE) {
    if ((mon->line & 0xfff) == ((DK << 10) | (DJ << 8) | (DK << 6) | (DJ << 4) |
                                (DK << 2) | (DK << 0))) {
      if (log) {
        fprintf(mon->file, "mon: %8d: (%c) SOP\n", tick,
                mon->driver == M_HOST ? 'H' : 'D');
      }
      mon->sopAt = tick;
      mon->state = MS_GET_PID;
      mon->needbits = 8;
    }
    return;
  }
  if ((mon->line & 0x3f) == ((SE0 << 4) | (SE0 << 2) | (DJ << 0))) {
    if ((log || compact) && (mon->state == MS_GET_BYTES) && (mon->byte > 0)) {
      int i;
      int text = 1;
      uint32_t pkt_crc16, comp_crc16;

      if (compact && mon->byte == 2) {
        fprintf(mon->file, "mon: %8d -- %8d: (%c) SOP, PID %s, EOP\n",
                mon->sopAt, tick, mon->driver == M_HOST ? 'H' : 'D',
                pid_2data(mon->lastpid, mon->bytes[0], mon->bytes[1]));
      } else if (compact && mon->byte == 1) {
        fprintf(mon->file, "mon: %8d -- %8d: (%c) SOP, PID %s %02x EOP\n",
                mon->sopAt, tick, mon->driver == M_HOST ? 'H' : 'D',
                decode_pid[mon->lastpid & 0xf], mon->bytes[0]);
      } else {
        if (compact) {
          fprintf(mon->file, "mon: %8d -- %8d: (%c) SOP, PID %s, EOP\n",
                  mon->sopAt, tick, mon->driver == M_HOST ? 'H' : 'D',
                  decode_pid[mon->lastpid & 0xf]);
        }
        fprintf(mon->file,
                "mon:     %s: ", mon->driver == M_HOST ? "h->d" : "d->h");
        comp_crc16 = CRC16(mon->bytes, mon->byte - 2);
        pkt_crc16 =
            mon->bytes[mon->byte - 2] | (mon->bytes[mon->byte - 1] << 8);
        // Display the data field as interleaved hexadecimal values and
        // printable characters
        for (i = 0; i < mon->byte - 2; i++) {
          fprintf(mon->file, "%02x%s", mon->bytes[i],
                  ((i & 0xf) == 0xf)           ? "\nmon:           "
                  : ((i + 1) == mon->byte - 2) ? " "
                                               : ", ");
          if ((mon->bytes[i] == 0x0d) || (mon->bytes[i] == 0x0a)) {
            mon->bytes[i] = '_';
          } else if (mon->bytes[i] == 0) {
            mon->bytes[i] = '?';
          } else if ((mon->bytes[i] < 32) || (mon->bytes[i] > 127)) {
            text = 0;
          }
        }
        // Display the received CRC16 value
        fprintf(mon->file, "(CRC16 %02x %02x", mon->bytes[mon->byte - 2],
                mon->bytes[mon->byte - 1]);
        if (comp_crc16 == pkt_crc16) {
          fprintf(mon->file, "%s OK)\n",
                  (mon->byte == MON_BYTES_SIZE) ? "..." : "");
        } else {
          fprintf(mon->file,
                  "%s BAD)\nmon:           CRC16 %04x BAD expected %04x\n",
                  (mon->byte == MON_BYTES_SIZE) ? "..." : "", pkt_crc16,
                  comp_crc16);
        }
        if (text && mon->byte > 2) {
          // Terminate at the end of the data field
          mon->bytes[mon->byte - 2] = '\0';
          fprintf(mon->file, "mon:          %s\n", mon->bytes);
        }
      }
    } else if (compact) {
      fprintf(mon->file, "mon: %8d -- %8d: (%c) SOP, PID %s EOP\n", mon->sopAt,
              tick, mon->driver == M_HOST ? 'H' : 'D',
              decode_pid[mon->lastpid & 0xf]);
    }
    if (log) {
      fprintf(mon->file, "mon: %8d: (%c) EOP\n", tick,
              mon->driver == M_HOST ? 'H' : 'D');
    }
    mon->state = MS_IDLE;
    return;
  }
  int newbit = (((mon->line & 0xc) >> 2) == (mon->line & 0x3)) ? 1 : 0;
  mon->rawbits = (mon->rawbits << 1) | newbit;
  if ((mon->rawbits & 0x7e) == 0x7e) {
    if (newbit == 1) {
      fprintf(mon->file, "mon: %8d: (%c) Bitstuff error, got 1 after 0x%x\n",
              tick, mon->driver == M_HOST ? 'H' : 'D', mon->rawbits);
    }
    /* Ignore bit stuff bit */
    return;
  }
  mon->bits = (mon->bits >> 1) | (newbit << 7);
  mon->needbits--;
  if (mon->needbits) {
    return;
  }
  switch (mon->state) {
    case MS_GET_PID:
      // Any byte for which the upper nybble is not the exact complement
      // of the lower nybble is invalid
      if (((mon->bits ^ 0xf0) >> 4) ^ (mon->bits & 0x0f)) {
        if (log) {
          fprintf(mon->file, "mon: %8d: (%c) BAD PID 0x%x\n", tick,
                  mon->driver == M_HOST ? 'H' : 'D', mon->bits);
        }
      } else {
        *lastpid = mon->bits;
        mon->lastpid = mon->bits;
        if (log) {
          fprintf(mon->file, "mon: %8d: (%c) PID %s (0x%x)\n", tick,
                  mon->driver == M_HOST ? 'H' : 'D',
                  decode_pid[mon->bits & 0xf], mon->bits);
        }
      }
      mon->state = MS_GET_BYTES;
      mon->needbits = 8;
      mon->byte = 0;
      break;

    case MS_GET_BYTES:
      mon->bytes[mon->byte] = mon->bits & 0xff;
      mon->needbits = 8;
      if (mon->byte < MON_BYTES_SIZE) {
        mon->byte++;
      }
      break;
  }
}
