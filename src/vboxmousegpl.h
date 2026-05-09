/*
 * vboxmousegpl.h
 * VirtualBox Guest Additions Mouse Driver for BTRON3
 *
 * Copyright (C) 2024
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * インクルード方針:
 *   basic.h を先頭に置いて __TYPEDEF_H__ を定義してから
 *   itron/ ヘッダ・driver/ ヘッダを並べる。
 *   basic.h が先にあれば driver/kbpd.h も重複なくインクルードできる。
 */

#ifndef __VBOXMOUSEGPL_H__
#define __VBOXMOUSEGPL_H__

/* ---- 基本型 ---- */
#include <basic.h>              /* __TYPEDEF_H__ 定義 → 後続の重複防止 */

/* ---- itron カーネル API (inner.h 相当) ---- */
#include <itron/itron.h>        /* B,H,W 等は __TYPEDEF_H__ でスキップ */
#include <itron/ierrno.h>
#include <itron/ifncode.h>
#include <itron/idebug.h>       /* T_REGS 等 */
#include <itron/istddef.h>
#include <itron/isyscall.h>     /* snd_msg(ID, T_MSG*) */

/* ---- ドライバ用ヘッダ ---- */
#include <driver/kbpd.h>        /* PdInput, PdInStat, PdRange,
                                   InputCmd, DevError,
                                   DN_KPINPUT, DN_PDRANGE,
                                   PDIN_XMAX, PDIN_YMAX          */
#include <driver/pcat/sys.h>    /* out_w, in_w (inline)           */
#include <driver/pcat/pci.h>    /* searchPciDev 等                */
#include <kernel/segment.h>     /* MapMemory, UnmapMemory         */
#include <driver/hwres.h>       /* defIntHdr, rsvHwRes            */
#include <bsys/cons_io.h>
#include <util/debug.h>         /* DEBUG_PRINT                    */
#include <tstring.h>            /* eucstotcs                      */
#include <kernel/util.h>        /* Smalloc, Sfree                 */

/* ---- btron/device.h の必要分 ---- */
#ifndef D_READ
#define D_READ  0x01
#endif
extern W  b_opn_dev(TC *name, UINT mode, void *arg);
extern ER b_cls_dev(W dd, UINT mode, void *arg);
extern ER b_rea_dev(W dd, W start, void *buf, W size, W *asize, void *arg);

/* math */
extern double floor(double x);

#endif /* __VBOXMOUSEGPL_H__ */
