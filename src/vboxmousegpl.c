/*
 * vboxmousegpl.c
 * VirtualBox Guest Additions Mouse Driver for BTRON3
 *
 * VirtualBox VMMDev PCI デバイス経由でホストのマウス絶対座標を取得し、
 * BTRON3 の kbpd ドライバにポインタイベントとして渡す。
 *
 * 参考資料:
 *   超漢字 PCI デバイス用デバイスドライバ説明書
 *     http://www.chokanji.com/developer/info/pcidrv.html
 *   favo430 (Wacom FAVO USB タブレットドライバ for BTRON3, GPL)
 *     https://yashiromann.sakura.ne.jp/prog/favo430/favo430.tar.gz
 *   VirtualBox OSE ソースコード VBox/VMMDev.h
 *
 * ソースコード:
 *   https://github.com/tadwg/virtualbox-additions
 *
 * Copyright (C) 2026
 *
 * このソースコードは Claude Sonnet 4.6 の支援を受けて書かれました。
 * Written with the assistance of Claude Sonnet 4.6 (Anthropic).
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "vboxmousegpl.h"

/* ================================================================
 * VMMDev 定数
 * ================================================================ */

/* VMMDev PCI ID */
#define VMMDEV_VENDOR_ID        0x80EE
#define VMMDEV_DEVICE_ID        0xCAFE

/* VMMDev リクエスト種別 */
#define VMMDEVREQ_GET_MOUSE_STATUS     1   /* VMMDevReq_GetMouseStatus   */
#define VMMDEVREQ_SET_MOUSE_STATUS     2   /* VMMDevReq_SetMouseStatus   */
#define VMMDEVREQ_GET_MOUSE_STATUS_EX  223 /* VMMDevReq_GetMouseStatusEx */
#define VMMDEVREQ_GET_HOST_VERSION  4   /* VMMDevReq_GetHostVersion */
#define VMMDEVREQ_REPORT_GUEST_INFO 50  /* VMMDevReq_ReportGuestInfo (登録必須) */

/* VMMDEV_VERSION: interfaceVersion for ReportGuestInfo */
#define VMMDEV_VERSION              0x00010004
#define VMMDEV_OSTYPE_OTHER         0x00000000  /* VBOXOSTYPE_Unknown */

/* VMMDev リクエストヘッダ バージョン */
#define VMMDEV_REQUEST_HEADER_VERSION  0x00010001


/* mouseFeatures フラグ */
#define VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE  0x0001  /* ゲストが絶対座標を受け取れる     */
#define VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE 0x0002  /* ホストが絶対座標を送りたい       */
#define VMMDEV_MOUSE_HOST_CAN_ABSOLUTE   0x0008  /* (旧) ホストが絶対座標デバイス持つ */
#define VMMDEV_MOUSE_NEW_PROTOCOL        0x0010  /* NEW_PROTOCOL: VMMDev経由絶対座標 */
#define VMMDEV_MOUSE_GUEST_USES_FULL_STATE_PROTOCOL 0x0080 /* フルstate取得 */

/* fButtons ビット定義 (VMMDevReqMouseStatusEx::fButtons) */
#define VMMDEV_MOUSE_BUTTON_LEFT    0x01  /* 左ボタン           */
#define VMMDEV_MOUSE_BUTTON_RIGHT   0x02  /* 右ボタン           */
#define VMMDEV_MOUSE_BUTTON_MIDDLE  0x04  /* ホイールプッシュ   */

/* VMMDev 割り込みイベントフラグ */
#define VMMDEV_EVENT_MOUSE_POSITION_CHANGED  0x00000002

/* VMMDev MMIO レジスタオフセット */
#define VMMDEV_MMIO_IRQ_STATUS_OFFSET  0x0000
#define VMMDEV_MMIO_IRQ_ACK_OFFSET     0x0004

/* ドライバ独自の詳細エラーコード (ER_NOSPT の下位16bit)
 *   ER_NOSPT | ED_VBOX_xxx の形で返す。
 *   上位16bit = 0xFFFD (EC_NOSPT=-3), 下位16bit = 原因箇所 */
#define ED_VBOX_KBPD_OPEN         0x0001  /* kbpd オープン失敗                        */
#define ED_VBOX_KBPD_MAILBOX      0x0002  /* kbpd メールボックスID取得失敗             */
#define ED_VBOX_VMMDEV_NOTFOUND   0x0003  /* VMMDev PCI デバイスが見つからない         */
#define ED_VBOX_BAR0              0x0004  /* BAR0 (I/O) 取得失敗                      */
/* GetHostVersion (requestType=4) */
#define ED_VBOX_HOSTVER_REQ       0x0005  /* GetHostVersion: vmmdev_request() 失敗    */
                                          /*   CnvPhysicalAddr 失敗 or plen < size    */
#define ED_VBOX_HOSTVER_RC        0x0006  /* GetHostVersion: VBox 側エラー応答        */
                                          /*   header.rc に VBox のエラーコードあり    */
/* SetMouseStatus (requestType=3) */
#define ED_VBOX_SETMOUSE_REQ      0x0007  /* SetMouseStatus: vmmdev_request() 失敗    */
#define ED_VBOX_SETMOUSE_RC       0x0008  /* SetMouseStatus: VBox 側エラー応答        */
/* その他 */
#define ED_VBOX_GUESTINFO_REQ     0x000C  /* ReportGuestInfo: vmmdev_request() 失敗   */
#define ED_VBOX_GUESTINFO_RC      0x000D  /* ReportGuestInfo: VBox 側エラー応答       */
#define ED_VBOX_DEFINTHDR         0x0009  /* defIntHdr 失敗                           */
#define ED_VBOX_VCRE_TSK          0x000A  /* vcre_tsk 失敗                            */
#define ED_VBOX_STA_TSK           0x000B  /* sta_tsk 失敗                             */

/* タスク設定 */
#define TASK_PRIORITY   30
#define TASK_STACKSIZE  2048

/* ================================================================
 * VMMDev 構造体
 * ================================================================ */

typedef struct {
    UW  size;
    UW  version;
    UW  requestType;
    W   rc;
    UW  reserved1;
    UW  reserved2;
} VMMDevRequestHeader;

typedef struct {
    VMMDevRequestHeader header;  /* 24 bytes */
    UW  mouseFeatures;
    W   pointerXPos;
    W   pointerYPos;
} VMMDevReqMouseStatus;          /* header(24) + 12 = 36 bytes */

typedef struct {
    VMMDevReqMouseStatus Core;   /* 36 bytes */
    W   dz;                      /* ホイール垂直    */
    W   dw;                      /* ホイール水平    */
    UW  fButtons;                /* ボタン状態      */
} VMMDevReqMouseStatusEx;        /* 36 + 12 = 48 bytes */

typedef struct {
    VMMDevRequestHeader header;
    UH  major;      /* VirtualBox メジャーバージョン */
    UH  minor;      /* マイナーバージョン            */
    UW  build;      /* ビルド番号                    */
    UW  revision;   /* SVN リビジョン                */
    UW  features;   /* 機能フラグ                    */
} VMMDevReqHostVersion;

/* VMMDevReq_ReportGuestInfo (requestType=50)
 * ゲスト登録。これを送らないと他のリクエストが VERR_NOT_SUPPORTED になる */
typedef struct {
    VMMDevRequestHeader header;
    UW  interfaceVersion;   /* VMMDEV_VERSION = 0x00010004 */
    UW  osType;             /* VMMDEV_OSTYPE_OTHER = 0      */
} VMMDevReqGuestInfo;

/* ================================================================
 * 大域変数
 * ================================================================ */

static ID   gKbpdMailboxID        = -1;
static W    gKbpdDD               = -1;
static VP   gIoAddr               = NULL;   /* BAR0: I/O アドレス */
static VP   gMmioAddr             = NULL;   /* BAR1: MMIO 論理アドレス */
static W    gIrqNo                = -1;
static volatile BOOL gLoopFlag       = TRUE;
static volatile BOOL gPollTaskActive = FALSE;
static W             gGetMouseDbgCount = 0; /* get_mouse ログ抑制カウンタ */
static UH            gVBoxMajor = 0;        /* VirtualBox メジャーバージョン */
static UH            gVBoxMinor = 0;        /* VirtualBox マイナーバージョン */
static W             gLastVx    = -1;       /* 前回送信した vx            */
static W             gLastVy    = -1;       /* 前回送信した vy            */
static W             gLastMain  = -1;       /* 前回送信した main_btn      */

/* kbpd へ送信する PdInput パケット。
 * snd_msg() はポインタを渡すだけなので、kbpd がパケットを処理するまで
 * メモリを保持し続ける必要がある。大域変数として確保する。
 * kbpd は処理完了後に read フラグを 1 にする。                        */
static PdInput       gPdMsg;                /* 送信パケットバッファ       */
static PdInput2      gPdMsg2;               /* ホイール送信パケットバッファ */

/* VMMDev リクエスト構造体
 * CnvPhysicalAddr() に渡すため静的グローバル変数として配置する。
 * ドライバ(.bss)のグローバル変数はカーネルローダーが
 * ページ境界でマップするため、40バイト程度の構造体は
 * 1ページ内に収まり物理連続が保証される。
 * スタック変数・Smalloc は物理連続を保証しないため使わない。 */
static VMMDevReqMouseStatus gReqSetBuf;    /* SetMouseStatus 用 */
static VMMDevReqMouseStatus gReqGetBuf;    /* GetMouseStatus 用 */
static VMMDevReqHostVersion gReqGuestBuf;  /* GetHostVersion 用    */
static VMMDevReqGuestInfo   gReqGuestInfoBuf; /* ReportGuestInfo 用 */
static VMMDevReqMouseStatusEx gReqGetExBuf;  /* GetMouseStatusEx 用     */
static VMMDevReqMouseStatus   *gReqSet   = &gReqSetBuf;
static VMMDevReqMouseStatus   *gReqGet   = &gReqGetBuf;
static VMMDevReqMouseStatusEx *gReqGetEx = &gReqGetExBuf;

/* ================================================================
 * VMMDev I/O アクセス
 * ================================================================
 *
 * pcidrv.html: CnvPhysicalAddr() の注意
 *   論理アドレス空間が物理的に連続していない場合、
 *   plen < len になることがある。
 *   VMMDevReqMouseStatus は 28 バイト程度で 1 ページ(4KB)内に
 *   収まるため通常は分割されないが、チェックは行う。
 */
static W vmmdev_request(VP req, W size)
{
    VP  paddr;
    W   plen;

    plen = CnvPhysicalAddr(req, size, &paddr);
    if (plen < 0) {
        DEBUG_PRINT(("vboxmousegpl: CnvPhysicalAddr failed plen=%d\n", plen));
        return -1;
    }
    if (plen < size) {
        DEBUG_PRINT(("vboxmousegpl: CnvPhysicalAddr plen(%d) < size(%d)\n",
                     plen, size));
        return -1;
    }

    // DEBUG_PRINT(("vboxmousegpl: request type=%d laddr=0x%08X paddr=0x%08X\n",
    //              ((VMMDevRequestHeader *)req)->requestType,
    //              (UW)req, (UW)paddr));

    /* I/O ポートにリクエスト構造体の物理アドレスを 32bit OUT */
    out_w((W)(UW)gIoAddr, (UW)paddr);
    return 0;
}

/* ================================================================
 * VMMDev マウス操作
 * ================================================================ */

static W vmmdev_set_mouse_status(UW features)
{
    memset(gReqSet, 0, sizeof(*gReqSet));
    gReqSet->header.size        = sizeof(*gReqSet);
    gReqSet->header.version     = VMMDEV_REQUEST_HEADER_VERSION;
    gReqSet->header.requestType = VMMDEVREQ_SET_MOUSE_STATUS;
    gReqSet->header.rc          = -1;
    gReqSet->mouseFeatures      = features;
    gReqSet->pointerXPos        = 0;
    gReqSet->pointerYPos        = 0;

    if (vmmdev_request((VP)gReqSet, sizeof(*gReqSet)) < 0) {
        DEBUG_PRINT(("vboxmousegpl: SetMouseStatus request failed\n"));
        return -(ED_VBOX_SETMOUSE_REQ);
    }
    if (gReqSet->header.rc != 0) {
        DEBUG_PRINT(("vboxmousegpl: SetMouseStatus rc=%d (0x%08X) features=0x%08X\n",
                     gReqSet->header.rc, (UW)gReqSet->header.rc,
                     gReqSet->mouseFeatures));
        return -(ED_VBOX_SETMOUSE_RC);
    }
    DEBUG_PRINT(("vboxmousegpl: SetMouseStatus OK features=0x%08X\n",
                 gReqSet->mouseFeatures));
    return 0;
}

static W vmmdev_get_mouse_status(W *px, W *py)
{
    memset(gReqGet, 0, sizeof(*gReqGet));
    gReqGet->header.size        = sizeof(*gReqGet);
    gReqGet->header.version     = VMMDEV_REQUEST_HEADER_VERSION;
    gReqGet->header.requestType = VMMDEVREQ_GET_MOUSE_STATUS;
    gReqGet->header.rc          = -1;
    gReqGet->mouseFeatures      = 0;
    gReqGet->pointerXPos        = 0;
    gReqGet->pointerYPos        = 0;

    if (vmmdev_request((VP)gReqGet, sizeof(*gReqGet)) < 0) {
        if (gGetMouseDbgCount < 3) {
            DEBUG_PRINT(("vboxmousegpl: get_mouse request failed\n"));
            gGetMouseDbgCount++;
        }
        return -1;
    }
    if (gReqGet->header.rc != 0) {
        if (gGetMouseDbgCount < 3) {
            DEBUG_PRINT(("vboxmousegpl: get_mouse rc=%d features=0x%08X\n",
                         gReqGet->header.rc, gReqGet->mouseFeatures));
            gGetMouseDbgCount++;
        }
        return -1;
    }
    /* HOST_WANTS_ABSOLUTE (bit1) で判定。
     * 旧定数 HOST_CAN_ABSOLUTE (bit3) は VMMDev.h に存在しない。 */
    if (!(gReqGet->mouseFeatures & VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE)) {
        if (gGetMouseDbgCount < 3) {
            DEBUG_PRINT(("vboxmousegpl: get_mouse no HOST_WANTS_ABSOLUTE features=0x%08X\n",
                         gReqGet->mouseFeatures));
            gGetMouseDbgCount++;
        }
        return -1;
    }

    gGetMouseDbgCount = 0;   /* 成功したらリセット */
    *px = gReqGet->pointerXPos;
    *py = gReqGet->pointerYPos;
    DEBUG_PRINT(("vboxmousegpl: get_mouse vx=%d vy=%d features=0x%08X\n",
                 *px, *py, gReqGet->mouseFeatures));
    return 0;
}

/* vmmdev_get_mouse_status_ex:
 *   GetMouseStatusEx (requestType=223) で絶対座標＋ボタン状態を取得する。
 *   NEW_PROTOCOL 使用時はこちらを呼ぶ。
 *   HOST_WANTS_ABSOLUTE (bit1=0x0002) が立っていれば座標が有効。
 *
 * 戻り値: 0=成功, -1=失敗 */
static W vmmdev_get_mouse_status_ex(W *px, W *py, W *pmain)
{
    memset(gReqGetEx, 0, sizeof(*gReqGetEx));
    gReqGetEx->Core.header.size        = sizeof(*gReqGetEx);
    gReqGetEx->Core.header.version     = VMMDEV_REQUEST_HEADER_VERSION;
    gReqGetEx->Core.header.requestType = VMMDEVREQ_GET_MOUSE_STATUS_EX;
    gReqGetEx->Core.header.rc          = -1;

    if (vmmdev_request((VP)gReqGetEx, sizeof(*gReqGetEx)) < 0) {
        DEBUG_PRINT(("vboxmousegpl: get_mouse_ex request failed\n"));
        return -1;
    }
    if (gReqGetEx->Core.header.rc != 0) {
        DEBUG_PRINT(("vboxmousegpl: get_mouse_ex rc=%d features=0x%08X\n",
                     gReqGetEx->Core.header.rc,
                     gReqGetEx->Core.mouseFeatures));
        return -1;
    }
    if (!(gReqGetEx->Core.mouseFeatures & VMMDEV_MOUSE_HOST_WANTS_ABSOLUTE)) {
        DEBUG_PRINT(("vboxmousegpl: get_mouse_ex no HOST_WANTS_ABSOLUTE features=0x%08X\n",
                     gReqGetEx->Core.mouseFeatures));
        return -1;
    }

    *px    = gReqGetEx->Core.pointerXPos;
    *py    = gReqGetEx->Core.pointerYPos;
    *pmain = (gReqGetEx->fButtons & 0x01) ? 1 : 0;
    /* 座標・ボタンが変化した時のみ出力（静止中の大量出力を防ぐ） */
    if (*px != gLastVx || *py != gLastVy || *pmain != gLastMain) {
        DEBUG_PRINT(("vboxmousegpl: get_mouse_ex vx=%d vy=%d btn=0x%02X features=0x%08X\n",
                     *px, *py, gReqGetEx->fButtons,
                     gReqGetEx->Core.mouseFeatures));
    }
    return 0;
}

/* vmmdev_get_host_version の戻り値:
 *   0                        成功
 *  -ED_VBOX_HOSTVER_REQ   vmmdev_request() 失敗
 *  -ED_VBOX_HOSTVER_RC    VirtualBox 側エラー応答 (header.rc != 0) */
static W vmmdev_get_host_version(VOID)
{
    memset(&gReqGuestBuf, 0, sizeof(gReqGuestBuf));
    gReqGuestBuf.header.size        = sizeof(gReqGuestBuf);
    gReqGuestBuf.header.version     = VMMDEV_REQUEST_HEADER_VERSION;
    gReqGuestBuf.header.requestType = VMMDEVREQ_GET_HOST_VERSION;
    gReqGuestBuf.header.rc          = -1;

    if (vmmdev_request((VP)&gReqGuestBuf, sizeof(gReqGuestBuf)) < 0)
        return -(ED_VBOX_HOSTVER_REQ);
    if (gReqGuestBuf.header.rc != 0)
        return -(ED_VBOX_HOSTVER_RC);
    gVBoxMajor = gReqGuestBuf.major;
    gVBoxMinor = gReqGuestBuf.minor;
    DEBUG_PRINT(("vboxmousegpl: VBox version %d.%d.%d (r%d) features=0x%08X\n",
                 gReqGuestBuf.major, gReqGuestBuf.minor,
                 gReqGuestBuf.build, gReqGuestBuf.revision,
                 gReqGuestBuf.features));
    return 0;
}

/* vmmdev_report_guest_info の戻り値:
 *   0                        成功
 *  -ED_VBOX_GUESTINFO_REQ   vmmdev_request() 失敗
 *  -ED_VBOX_GUESTINFO_RC    VirtualBox 側エラー応答
 *
 * VirtualBox はこれを受け取るまで SetMouseStatus 等を
 * VERR_NOT_SUPPORTED (-37) で拒否する。GetHostVersion の後に必ず送ること。 */
static W vmmdev_report_guest_info(VOID)
{
    memset(&gReqGuestInfoBuf, 0, sizeof(gReqGuestInfoBuf));
    gReqGuestInfoBuf.header.size        = sizeof(gReqGuestInfoBuf);
    gReqGuestInfoBuf.header.version     = VMMDEV_REQUEST_HEADER_VERSION;
    gReqGuestInfoBuf.header.requestType = VMMDEVREQ_REPORT_GUEST_INFO;
    gReqGuestInfoBuf.header.rc          = -1;
    gReqGuestInfoBuf.interfaceVersion   = VMMDEV_VERSION;
    gReqGuestInfoBuf.osType             = VMMDEV_OSTYPE_OTHER;

    if (vmmdev_request((VP)&gReqGuestInfoBuf, sizeof(gReqGuestInfoBuf)) < 0)
        return -(ED_VBOX_GUESTINFO_REQ);
    if (gReqGuestInfoBuf.header.rc != 0) {
        DEBUG_PRINT(("vboxmousegpl: ReportGuestInfo rc=%d\n",
                     gReqGuestInfoBuf.header.rc));
        return -(ED_VBOX_GUESTINFO_RC);
    }
    DEBUG_PRINT(("vboxmousegpl: ReportGuestInfo OK\n"));
    return 0;
}

/* ================================================================
 * kbpd へのポインタイベント送信
 * (favo430 の InterruptHandlerTask / TransformPoint を参考)
 * ================================================================ */

#define PDMSG_WAIT_MAX  50      /* 最大待ちポーリング回数 (50 × 10ms = 500ms) */

/* pdmsg_wait_pd: PdInput パケットの read フラグが立つまで待つ
 *   戻り値: 0=ready, -1=timeout */
static W pdmsg_wait_pd(PdInput *p)
{
    W wait;
    for (wait = 0; wait < PDMSG_WAIT_MAX; wait++) {
        if (p->stat.read != 0) return 0;
        dly_tsk(10);
    }
    return -1;
}

/* pdmsg_wait_pd2: PdInput2 パケットの read フラグが立つまで待つ
 *   戻り値: 0=ready, -1=timeout */
static W pdmsg_wait_pd2(PdInput2 *p)
{
    W wait;
    for (wait = 0; wait < PDMSG_WAIT_MAX; wait++) {
        if (p->stat.read != 0) return 0;
        dly_tsk(10);
    }
    return -1;
}

/* post_pointer_event:
 *   座標・ボタン状態を PdInput (INP_PD) で kbpd に送信する。
 *   main_btn : 左ボタン状態 (0/1)
 *   sub_btn  : 右ボタン状態 (0/1)
 *   wheel    : ホイール回転量 (dz, 0=なし)
 *   mid_btn  : ホイールプッシュ (0/1) → onebut=1, qpress=1 で送信 */
static VOID post_pointer_event(W vx, W vy,
                                W main_btn, W sub_btn,
                                W wheel,    W mid_btn)
{
    double  ox, vy_d;

    /* VMMDev 座標 (0-0xFFFF) → BTRON3 論理座標 (0 - PDIN_XMAX-1 / PDIN_YMAX-1) */
    ox   = floor((double)vx * (double)(PDIN_XMAX - 1) / (double)0xFFFF);
    vy_d = floor((double)vy * (double)(PDIN_YMAX - 1) / (double)0xFFFF);

    if (ox   < 0)              ox   = 0;
    if (vy_d < 0)              vy_d = 0;
    if (ox   > PDIN_XMAX - 1)  ox   = PDIN_XMAX - 1;
    if (vy_d > PDIN_YMAX - 1)  vy_d = PDIN_YMAX - 1;

    /* ---- PdInput (INP_PD): 座標・左右ボタン・ホイールプッシュ ---- */
    if (pdmsg_wait_pd(&gPdMsg) < 0) {
        DEBUG_PRINT(("vboxmousegpl: post_event timeout, drop\n"));
    } else {
        memset(&gPdMsg, 0, sizeof(gPdMsg));
        gPdMsg.stat.read   = 0;
        gPdMsg.stat.cmd    = INP_PD;
        gPdMsg.stat.err    = DEV_OK;
        gPdMsg.stat.abs    = 1;
        gPdMsg.stat.main   = main_btn;
        gPdMsg.stat.sub    = sub_btn;
        if (mid_btn) {
            /* ホイールプッシュ: onebut=1, qpress=1 */
            gPdMsg.stat.onebut = 1;
            gPdMsg.stat.qpress = 1;
        }
        gPdMsg.xpos = (H)ox;
        gPdMsg.ypos = (H)vy_d;
        DEBUG_PRINT(("vboxmousegpl: post_event xpos=%d ypos=%d "
                     "main=%d sub=%d mid=%d\n",
                     gPdMsg.xpos, gPdMsg.ypos,
                     main_btn, sub_btn, mid_btn));
        snd_msg(gKbpdMailboxID, (T_MSG *)&gPdMsg);
    }

    /* ---- PdInput2 (INP_PD2): ホイール回転 ---- */
    if (wheel != 0) {
        if (pdmsg_wait_pd2(&gPdMsg2) < 0) {
            DEBUG_PRINT(("vboxmousegpl: post_wheel timeout, drop\n"));
        } else {
            memset(&gPdMsg2, 0, sizeof(gPdMsg2));
            gPdMsg2.stat.read = 0;
            gPdMsg2.stat.cmd  = INP_PD2;
            gPdMsg2.stat.err  = DEV_OK;
            gPdMsg2.wheel     = (H)wheel;
            DEBUG_PRINT(("vboxmousegpl: post_wheel wheel=%d\n", wheel));
            snd_msg(gKbpdMailboxID, (T_MSG *)&gPdMsg2);
        }
    }
}

/* ================================================================
 * 割り込みハンドラ
 *
 * pcidrv.html の注意:
 *   - PCI 割り込みは共有なので、自デバイスからの割り込みか
 *     必ず確認してから処理する。
 *   - EndOfInt() は呼ばない。
 *   - 処理後に割り込み要因を必ずクリアする（レベルトリガのため）。
 * ================================================================ */

static VOID vmmdev_inthdr(UW par)
{
    UW  events;
    W   vx, vy;

    /* MMIO 割り込みステータスレジスタを読む */
    events = in_w((W)((UW)gMmioAddr + VMMDEV_MMIO_IRQ_STATUS_OFFSET));

    /* 自デバイスからの割り込みでなければ何もしない */
    if (!(events & VMMDEV_EVENT_MOUSE_POSITION_CHANGED)) return;

    /* 座標取得してイベント送信 */
    if (vmmdev_get_mouse_status(&vx, &vy) == 0) {
        post_pointer_event(vx, vy, 0, 0, 0, 0);
    }

    /* 割り込み要因クリア（ACK レジスタに処理済みビットを書き戻す）*/
    out_w((W)((UW)gMmioAddr + VMMDEV_MMIO_IRQ_ACK_OFFSET),
          events & VMMDEV_EVENT_MOUSE_POSITION_CHANGED);
}

/* ================================================================
 * ポーリングタスク（割り込み未使用時のフォールバック）
 * ================================================================ */

static VOID poll_task(W *pParam)
{
    W vx, vy, main_btn;

    DEBUG_PRINT(("vboxmousegpl: poll_task start\n"));
    gPollTaskActive = TRUE;

    while (gLoopFlag) {
        dly_tsk(10);    /* 10ms 待機 */
        main_btn = 0;
        /* NEW_PROTOCOL: GetMouseStatusEx を優先使用 */
        /* VBox 7.0 以降: GetMouseStatusEx (ボタン・ホイール対応)
         * VBox 6.x 以前: GetMouseStatus にフォールバック (座標のみ) */
        if (gVBoxMajor >= 7) {
            if (vmmdev_get_mouse_status_ex(&vx, &vy, &main_btn) == 0) {
                W sub_btn = (gReqGetEx->fButtons & VMMDEV_MOUSE_BUTTON_RIGHT)  ? 1 : 0;
                W mid_btn = (gReqGetEx->fButtons & VMMDEV_MOUSE_BUTTON_MIDDLE) ? 1 : 0;
                W wheel   = (W)gReqGetEx->dz;
                if (vx != gLastVx || vy != gLastVy || main_btn != gLastMain
                    || sub_btn || mid_btn || wheel != 0) {
                    post_pointer_event(vx, vy, main_btn, sub_btn, wheel, mid_btn);
                    gLastVx   = vx;
                    gLastVy   = vy;
                    gLastMain = main_btn;
                }
            }
        } else {
            /* VBox 6.x: GetMouseStatus (requestType=1) で座標のみ取得 */
            if (vmmdev_get_mouse_status(&vx, &vy) == 0) {
                if (vx != gLastVx || vy != gLastVy) {
                    post_pointer_event(vx, vy, 0, 0, 0, 0);
                    gLastVx = vx;
                    gLastVy = vy;
                }
            }
        }
    }

    DEBUG_PRINT(("vboxmousegpl: poll_task end\n"));
    gPollTaskActive = FALSE;
    exd_tsk();
}

/* ================================================================
 * VMMDev 初期化
 *
 * pcidrv.html の手順に準拠:
 *   1. searchPciDev() でデバイスを探す
 *   2. getPciBaseAddr() で BAR0(I/O), BAR1(MMIO) を取得
 *   3. MapMemory() で MMIO を論理アドレスにマップ
 *      ※ MM_CDIS (キャッシュ禁止) を付ける（MMIO レジスタに必須）
 *   4. inPciConfB() で IRQ 番号を取得
 *   5. rsvHwRes() で IRQ を SHARED_IRQ 付きで予約
 *   6. ゲスト能力をホストに通知
 * ================================================================ */

static W vmmdev_init(VOID)
{
    W   caddr;
    W   t;
    VP  io_addr,   mmio_addr;
    W   io_size,   mmio_size;
    W   irq;
    ERR err;

    /* --- Step1: VMMDev を探す --- */
    DEBUG_PRINT(("vboxmousegpl: search VMMDev vendor=0x%04X dev=0x%04X\n",
                 VMMDEV_VENDOR_ID, VMMDEV_DEVICE_ID));
    caddr = searchPciDev(VMMDEV_VENDOR_ID, VMMDEV_DEVICE_ID);
    if (caddr < 0) {
        DEBUG_PRINT(("vboxmousegpl: VMMDev not found\n"));
        return -(ED_VBOX_VMMDEV_NOTFOUND);
    }
    DEBUG_PRINT(("vboxmousegpl: found caddr=0x%08X\n", caddr));

    /* --- Step2: BAR0 から I/O アドレス取得 --- */
    t = getPciBaseAddr(caddr, PCR_BASEADDR_0, &io_addr, &io_size);
    if (!isBaseAddrIO(t)) {
        DEBUG_PRINT(("vboxmousegpl: BAR0 is not I/O\n"));
        return -(ED_VBOX_BAR0);
    }
    DEBUG_PRINT(("vboxmousegpl: I/O addr=0x%08X size=0x%X\n",
                 (UW)io_addr, io_size));
    gIoAddr = io_addr;

    /* --- Step3: BAR2 から MMIO アドレス取得 → カーネル空間にマップ ---
     *   VMMDev のレイアウト:
     *     BAR0: I/O ポート (リクエスト送信用)
     *     BAR1: VMMDev RAM 共有メモリ 4MB (今回は不使用)
     *     BAR2: VMMDev MMIO 16KB (IRQ ステータス・ACK レジスタ)
     *   実機確認: BAR1=F0400000[4MB], BAR2=F0800000[16KB]
     *   pcidrv.html: MM_CDIS (キャッシュ禁止) を付けること          */
    t = getPciBaseAddr(caddr, PCR_BASEADDR_2, &mmio_addr, &mmio_size);
    if (isBaseAddr32(t) && mmio_size > 0) {
        err = MapMemory(mmio_addr, mmio_size,
                        MM_SYSTEM | MM_READ | MM_WRITE | MM_CDIS,
                        &gMmioAddr);
        if (err == ER_OK) {
            DEBUG_PRINT(("vboxmousegpl: MMIO(BAR2) mapped to 0x%08X size=0x%X\n",
                         (UW)gMmioAddr, mmio_size));
        } else {
            DEBUG_PRINT(("vboxmousegpl: MapMemory(BAR2) failed, no interrupt\n"));
            gMmioAddr = NULL;
        }
    } else {
        DEBUG_PRINT(("vboxmousegpl: BAR2 not 32bit memory, no interrupt\n"));
        gMmioAddr = NULL;
    }

    /* --- Step4: IRQ 番号取得 --- */
    /* ポーリングモード強制（割り込みモードのテスト前に使用） */
    /* #define FORCE_POLLING */
    irq = (W)inPciConfB(caddr, PCR_IRQLIN);
    DEBUG_PRINT(("vboxmousegpl: raw IRQ=%d (forcing polling mode)\n", irq));
    irq = -1;   /* ポーリングモード強制 */
    if (irq >= 3 && irq <= 15) {
        gIrqNo = irq;
        DEBUG_PRINT(("vboxmousegpl: IRQ=%d\n", gIrqNo));

        /* --- Step5: IRQ を SHARED_IRQ 付きで予約 ---
         *   pcidrv.html: PCI 割り込みは共有なので SHARED_IRQ を付ける */
        err = rsvHwRes(HW_IRQ(gIrqNo),
                       gIrqNo | SHARED_IRQ,
                       gIrqNo | SHARED_IRQ);
        if (err < ER_OK) {
            DEBUG_PRINT(("vboxmousegpl: rsvHwRes failed (%d), polling\n",
                         err));
            gIrqNo = -1;
        }
    } else {
        DEBUG_PRINT(("vboxmousegpl: no valid IRQ, polling mode\n"));
        gIrqNo = -1;
    }

    /* --- Step5b-1: GetHostVersion でホストバージョンを確認 ---
     *   VirtualBox と通信できているかを確認する。            */
    {
        W gret = vmmdev_get_host_version();
        if (gret < 0) {
            DEBUG_PRINT(("vboxmousegpl: GetHostVersion failed\n"));
            if (gMmioAddr != NULL) { UnmapMemory(gMmioAddr); gMmioAddr = NULL; }
            if (gIrqNo >= 0)       { relHwRes(HW_IRQ(gIrqNo)); gIrqNo = -1; }
            return gret;    /* -(ED_VBOX_HOSTVER_REQ) or -(ED_VBOX_HOSTVER_RC) */
        }
    }

    /* --- Step5b-2: ReportGuestInfo でゲスト登録 ---
     *   VirtualBox はこれを受け取るまで SetMouseStatus 等を
     *   VERR_NOT_SUPPORTED (-37) で拒否する。必須。         */
    {
        W gret = vmmdev_report_guest_info();
        if (gret < 0) {
            DEBUG_PRINT(("vboxmousegpl: ReportGuestInfo failed\n"));
            if (gMmioAddr != NULL) { UnmapMemory(gMmioAddr); gMmioAddr = NULL; }
            if (gIrqNo >= 0)       { relHwRes(HW_IRQ(gIrqNo)); gIrqNo = -1; }
            return gret;    /* -(ED_VBOX_GUESTINFO_REQ) or -(ED_VBOX_GUESTINFO_RC) */
        }
    }

    /* --- Step6: ゲスト能力をホストに通知 --- */
    {
        W sret = vmmdev_set_mouse_status(VMMDEV_MOUSE_GUEST_CAN_ABSOLUTE);
        if (sret < 0) {
            DEBUG_PRINT(("vboxmousegpl: SetMouseStatus failed\n"));
            if (gMmioAddr != NULL) { UnmapMemory(gMmioAddr); gMmioAddr = NULL; }
            if (gIrqNo >= 0)       { relHwRes(HW_IRQ(gIrqNo)); gIrqNo = -1; }
            return sret;    /* -(ED_VBOX_SETMOUSE_REQ) or -(ED_VBOX_SETMOUSE_RC) */
        }
    }
    DEBUG_PRINT(("vboxmousegpl: init OK\n"));
    return 0;
}

/* ================================================================
 * ドライバエントリポイント
 * ================================================================ */

ERR main(Bool StartUp, TC *arg)
{
    ERR     ret;
    TC      tcName[6];
    W       rsize;
    T_CTSK  ctsk;
    ID      idPollTask = -1;
    W       param = 0;

    DEBUG_PRINT(("vboxmousegpl: main(%d) build=" __DATE__ " " __TIME__ "\n", StartUp));

    /* ---- 終了処理 ---- */
    if (TRUE != StartUp) {
        DEBUG_PRINT(("vboxmousegpl: shutdown\n"));
        gLoopFlag = FALSE;

        /* ゲスト能力通知を解除 */
        vmmdev_set_mouse_status(0);

        /* MMIO 解放 */
        if (gMmioAddr != NULL) {
            UnmapMemory(gMmioAddr);
            gMmioAddr = NULL;
        }

        /* 割り込みハンドラ登録解除
         * pcidrv.html: defIntHdr(-irq, ...) で解除 */
        if (gIrqNo >= 0) {
            defIntHdr(-gIrqNo, vmmdev_inthdr, 0);
            relHwRes(HW_IRQ(gIrqNo));
        }

        /* kbpd クローズ */
        if (gKbpdDD > 0) {
            b_cls_dev(gKbpdDD, 0, NULL);
        }

        /* ポーリングタスク終了待ち */
        while (gPollTaskActive) { /* spin */ }

        return ER_OK;
    }

    /* ---- 起動処理 ---- */
    gLoopFlag = TRUE;
    gPdMsg.stat.read  = 1;  /* 最初の送信を許可（未送信状態） */
    gPdMsg2.stat.read = 1;  /* ホイール用も同様               */

    /* 1. kbpd オープン */
    eucstotcs(tcName, "kbpd");
    gKbpdDD = b_opn_dev(tcName, D_READ, NULL);
    if (!(gKbpdDD > 0)) {
        DEBUG_PRINT(("vboxmousegpl: kbpd open failed (%d)\n", gKbpdDD));
        return ER_NOSPT | ED_VBOX_KBPD_OPEN;
    }

    /* 2. kbpd メールボックス ID 取得 */
    ret = b_rea_dev(gKbpdDD, DN_KPINPUT,
                    (B*)&gKbpdMailboxID, sizeof(ID), &rsize, NULL);
    if (ret != 0) {
        DEBUG_PRINT(("vboxmousegpl: DN_KPINPUT failed (%d)\n", ret));
        b_cls_dev(gKbpdDD, 0, NULL);
        return ER_NOSPT | ED_VBOX_KBPD_MAILBOX;
    }
    DEBUG_PRINT(("vboxmousegpl: kbpd mailbox ID=%d\n", gKbpdMailboxID));



    /* 3. VMMDev 初期化 */
    {
        W initret = vmmdev_init();
        if (initret < 0) {
            W detail = (W)(-(initret));   /* vmmdev_init は -(ED_VBOX_xxx) を返す */
            DEBUG_PRINT(("vboxmousegpl: vmmdev_init failed detail=0x%04X\n",
                         detail));
            b_cls_dev(gKbpdDD, 0, NULL);
            return ER_NOSPT | detail;
        }
    }

    /* 4. 割り込みモード or ポーリングモード */
    if (gIrqNo >= 0 && gMmioAddr != NULL) {
        /* 割り込みモード
         * pcidrv.html: defIntHdr() を使う（def_int() ではない）*/
        DEBUG_PRINT(("vboxmousegpl: interrupt mode IRQ=%d\n", gIrqNo));
        ret = defIntHdr(gIrqNo, vmmdev_inthdr, 0);
        if (ret < ER_OK) {
            DEBUG_PRINT(("vboxmousegpl: defIntHdr failed (%d)\n", ret));
            relHwRes(HW_IRQ(gIrqNo));  gIrqNo = -1;
            if (gMmioAddr != NULL) { UnmapMemory(gMmioAddr); gMmioAddr = NULL; }
            b_cls_dev(gKbpdDD, 0, NULL);
            return ER_NOSPT | ED_VBOX_DEFINTHDR;
        }
    }

    if (gIrqNo < 0) {
        /* ポーリングモード（フォールバック）*/
        DEBUG_PRINT(("vboxmousegpl: polling mode\n"));
        memset(&ctsk, 0, sizeof(ctsk));
        ctsk.exinf   = &param;
        ctsk.task    = (FP)poll_task;
        ctsk.itskpri = TASK_PRIORITY;
        ctsk.stksz   = TASK_STACKSIZE;
        ctsk.tskatr  = TA_HLNG | TA_RNG0;

        idPollTask = vcre_tsk(&ctsk);
        if (idPollTask < 0) {
            DEBUG_PRINT(("vboxmousegpl: vcre_tsk failed\n"));
            if (gMmioAddr != NULL) { UnmapMemory(gMmioAddr); gMmioAddr = NULL; }
            b_cls_dev(gKbpdDD, 0, NULL);
            return ER_NOSPT | ED_VBOX_VCRE_TSK;
        }
        ret = sta_tsk(idPollTask, (INT)param);
        if (ret != 0) {
            DEBUG_PRINT(("vboxmousegpl: sta_tsk failed\n"));
            del_tsk(idPollTask);
            if (gMmioAddr != NULL) { UnmapMemory(gMmioAddr); gMmioAddr = NULL; }
            b_cls_dev(gKbpdDD, 0, NULL);
            return ER_NOSPT | ED_VBOX_STA_TSK;
        }
    }

    DEBUG_PRINT(("vboxmousegpl: startup OK\n"));
    return ER_OK;
}
