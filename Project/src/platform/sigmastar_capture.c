/* Sensor/VIF/VPE/VENC setup adapted from OpenIPC star, Copyright (c) OpenIPC,
 * https://openipc.org, MIT License. See third_party source and license records.
 * This backend targets the Infinity6E ABI, not the incompatible Maruko ABI. */
#define _POSIX_C_SOURCE 200809L
#include "fpv/platform.h"
#include "capture_annexb.h"
#include <mi_sensor.h>
#include <mi_sys.h>
#include <mi_vif.h>
#include <mi_vpe.h>
#include <mi_venc.h>
#include <mi_isp.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
    int sys, sensor, vif_dev, vif_port, vpe, vpe_started, vpe_port;
    int vif_bound, venc, receiving, venc_bound, fd, fps;
    uint64_t frames, first_pts;
    int have_pts;
    fpv_capture_callback callback;
    void *opaque;
    uint8_t *au;
    size_t au_capacity;
} cap = {.fd = -1};

static MI_SYS_ChnPort_t port(MI_ModuleId_e module)
{
    MI_SYS_ChnPort_t p = {0};
    p.eModId = module;
    return p;
}

void fpv_capture_close(void)
{
    MI_SYS_ChnPort_t vif = port(E_MI_MODULE_ID_VIF);
    MI_SYS_ChnPort_t vpe = port(E_MI_MODULE_ID_VPE);
    MI_SYS_ChnPort_t venc = port(E_MI_MODULE_ID_VENC);
    if (cap.venc_bound)
        MI_SYS_UnBindChnPort(&vpe, &venc);
    if (cap.receiving)
        MI_VENC_StopRecvPic(0);
    if (cap.fd >= 0)
        MI_VENC_CloseFd(0);
    if (cap.venc)
        MI_VENC_DestroyChn(0);
    if (cap.vpe_port)
        MI_VPE_DisablePort(0, 0);
    if (cap.vif_bound)
        MI_SYS_UnBindChnPort(&vif, &vpe);
    if (cap.vpe_started)
        MI_VPE_StopChannel(0);
    if (cap.vpe)
        MI_VPE_DestroyChannel(0);
    if (cap.vif_port)
        MI_VIF_DisableChnPort(0, 0);
    if (cap.vif_dev)
        MI_VIF_DisableDev(0);
    if (cap.sensor)
        MI_SNR_Disable(0);
    if (cap.sys)
        MI_SYS_Exit();
    free(cap.au);
    memset(&cap, 0, sizeof cap);
    cap.fd = -1;
}

int fpv_capture_open(const fpv_config *cfg, fpv_capture_callback callback, void *opaque,
                     char *error, size_t error_size)
{
    MI_S32 result = 0;
    const char *stage = "configuration";
    if (cap.sys || !cfg || !callback || cfg->sensor_mode < 0 || cfg->sensor_mode > 255 ||
        cfg->fps < 1 || cfg->width < 16 || cfg->height < 16 || cfg->width > 65535 ||
        cfg->height > 65535 || (cfg->width & 1) || (cfg->height & 1) || cfg->bitrate_kbps < 1 ||
        (unsigned)cfg->bitrate_kbps > UINT32_MAX / 1000U || cfg->gop < 1) {
        if (error && error_size)
            snprintf(error, error_size, "invalid or already open MI capture");
        return -1;
    }
#define MI_TRY(call)                                                                               \
    do {                                                                                           \
        stage = #call;                                                                             \
        result = (call);                                                                           \
        if (result)                                                                                \
            goto fail;                                                                             \
    } while (0)
    MI_TRY(MI_SYS_Init());
    cap.sys = 1;
    MI_TRY(MI_SNR_SetPlaneMode(0, 0));
    MI_SNR_Res_t res = {0};
    MI_TRY(MI_SNR_GetRes(0, (MI_U8)cfg->sensor_mode, &res));
    if ((MI_U32)cfg->fps > res.u32MaxFps || (MI_U32)cfg->fps < res.u32MinFps) {
        stage = "sensor mode FPS range";
        result = -1;
        goto fail;
    }
    MI_TRY(MI_SNR_SetRes(0, (MI_U8)cfg->sensor_mode));
    MI_TRY(MI_SNR_SetFps(0, (MI_U32)cfg->fps));
    MI_TRY(MI_SNR_Enable(0));
    cap.sensor = 1;
    MI_SNR_PADInfo_t pad = {0};
    MI_SNR_PlaneInfo_t plane = {0};
    MI_TRY(MI_SNR_GetPadInfo(0, &pad));
    MI_TRY(MI_SNR_GetPlaneInfo(0, 0, &plane));
    MI_SYS_PixelFormat_e format =
        (MI_SYS_PixelFormat_e)RGB_BAYER_PIXEL(plane.ePixPrecision, plane.eBayerId);
    if (cfg->width > plane.stCapRect.u16Width || cfg->height > plane.stCapRect.u16Height) {
        stage = "output exceeds selected sensor mode dimensions";
        result = -1;
        goto fail;
    }
    MI_VIF_DevAttr_t dev = {0};
    dev.eIntfMode = pad.eIntfMode;
    dev.eHDRType = E_MI_VIF_HDR_TYPE_OFF;
    dev.eClkEdge = E_MI_VIF_CLK_EDGE_DOUBLE;
    dev.eBitOrder = E_MI_VIF_BITORDER_NORMAL;
    dev.eWorkMode = E_MI_VIF_WORK_MODE_RGB_REALTIME;
    dev.eDataSeq = pad.eIntfMode == E_MI_VIF_MODE_MIPI ? pad.unIntfAttr.stMipiAttr.eDataYUVOrder
                                                       : E_MI_VIF_INPUT_DATA_YUYV;
    MI_TRY(MI_VIF_SetDevAttr(0, &dev));
    MI_TRY(MI_VIF_EnableDev(0));
    cap.vif_dev = 1;
    MI_VIF_ChnPortAttr_t input = {0};
    input.stCapRect = plane.stCapRect;
    input.stDestSize.u16Width = plane.stCapRect.u16Width;
    input.stDestSize.u16Height = plane.stCapRect.u16Height;
    input.ePixFormat = format;
    MI_TRY(MI_VIF_SetChnPortAttr(0, 0, &input));
    MI_TRY(MI_VIF_EnableChnPort(0, 0));
    cap.vif_port = 1;
    MI_VPE_ChannelAttr_t vpe_attr = {0};
    vpe_attr.eSensorBindId = E_MI_VPE_SENSOR0;
    vpe_attr.eRunningMode = E_MI_VPE_RUN_REALTIME_MODE;
    vpe_attr.eHDRType = E_MI_VPE_HDR_TYPE_OFF;
    vpe_attr.ePixFmt = format;
    /* Input allocation must retain sensor size when VPE resizes output. */
    vpe_attr.u16MaxW = plane.stCapRect.u16Width;
    vpe_attr.u16MaxH = plane.stCapRect.u16Height;
    MI_TRY(MI_VPE_CreateChannel(0, &vpe_attr));
    cap.vpe = 1;
    MI_TRY(MI_VPE_StartChannel(0));
    cap.vpe_started = 1;
    MI_SYS_ChnPort_t vif = port(E_MI_MODULE_ID_VIF), vpe = port(E_MI_MODULE_ID_VPE);
    MI_SYS_ChnPort_t venc = port(E_MI_MODULE_ID_VENC);
    MI_TRY(MI_SYS_BindChnPort2(&vif, &vpe, cfg->fps, cfg->fps, E_MI_SYS_BIND_TYPE_REALTIME, 0));
    cap.vif_bound = 1;
    MI_VPE_PortMode_t output = {0};
    output.ePixelFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420;
    output.u16Width = (MI_U16)cfg->width;
    output.u16Height = (MI_U16)cfg->height;
    MI_TRY(MI_VPE_SetPortMode(0, 0, &output));
    MI_TRY(MI_VPE_EnablePort(0, 0));
    cap.vpe_port = 1;
    /* Sensor name selects tuning, never an executable shell command. */
    char tuning[128];
    if (cfg->sensor[0]) {
        for (const char *s = cfg->sensor; *s; ++s) {
            if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
                  (*s >= '0' && *s <= '9') || *s == '_' || *s == '-')) {
                stage = "invalid sensor tuning name";
                result = -1;
                goto fail;
            }
        }
        snprintf(tuning, sizeof tuning, "/etc/sensors/%s.bin", cfg->sensor);
        if (access(tuning, R_OK) == 0)
            MI_TRY(MI_ISP_API_CmdLoadBinFile(0, tuning, 1234));
        else
            fprintf(stderr, "MI capture: tuning %s unavailable; using firmware ISP defaults\n",
                    tuning);
    }
    MI_ISP_AE_EXPO_LIMIT_TYPE_t exposure = {0};
    MI_TRY(MI_ISP_AE_GetExposureLimit(0, &exposure));
    exposure.u32MaxShutterUS = 1000000U / (MI_U32)cfg->fps;
    MI_TRY(MI_ISP_AE_SetExposureLimit(0, &exposure));
    MI_VENC_ChnAttr_t encoder = {0};
    encoder.stVeAttr.eType = E_MI_VENC_MODTYPE_H265E;
    encoder.stVeAttr.stAttrH265e.bByFrame = 1;
    encoder.stVeAttr.stAttrH265e.u32PicWidth = cfg->width;
    encoder.stVeAttr.stAttrH265e.u32PicHeight = cfg->height;
    encoder.stVeAttr.stAttrH265e.u32MaxPicWidth = cfg->width;
    encoder.stVeAttr.stAttrH265e.u32MaxPicHeight = cfg->height;
    encoder.stVeAttr.stAttrH265e.u32BFrameNum = 0;
    encoder.stVeAttr.stAttrH265e.u32RefNum = 1;
    encoder.stRcAttr.eRcMode = E_MI_VENC_RC_MODE_H265CBR;
    encoder.stRcAttr.stAttrH265Cbr.u32Gop = cfg->gop;
    encoder.stRcAttr.stAttrH265Cbr.u32BitRate = (MI_U32)cfg->bitrate_kbps * 1000U;
    encoder.stRcAttr.stAttrH265Cbr.u32SrcFrmRateNum = cfg->fps;
    encoder.stRcAttr.stAttrH265Cbr.u32SrcFrmRateDen = 1;
    MI_TRY(MI_VENC_CreateChn(0, &encoder));
    cap.venc = 1;
    MI_TRY(MI_VENC_StartRecvPic(0));
    cap.receiving = 1;
    MI_TRY(MI_SYS_BindChnPort2(&vpe, &venc, cfg->fps, cfg->fps, E_MI_SYS_BIND_TYPE_FRAME_BASE, 0));
    cap.venc_bound = 1;
    cap.fd = MI_VENC_GetFd(0);
    if (cap.fd < 0) {
        stage = "MI_VENC_GetFd";
        result = cap.fd;
        goto fail;
    }
    cap.callback = callback;
    cap.opaque = opaque;
    cap.fps = cfg->fps;
    return 0;
fail:
    if (error && error_size)
        snprintf(error, error_size, "%s failed (0x%x)", stage, (unsigned)result);
    fpv_capture_close();
    return -1;
#undef MI_TRY
}

int fpv_capture_poll(int timeout_ms)
{
    if (cap.fd < 0)
        return -1;
    struct pollfd fd = {.fd = cap.fd, .events = POLLIN};
    int rc = poll(&fd, 1, timeout_ms);
    if (rc < 0)
        return errno == EINTR ? 0 : -1;
    if (!rc)
        return 0;
    if (!(fd.revents & POLLIN))
        return -1;
    MI_VENC_ChnStat_t stat = {0};
    if (MI_VENC_Query(0, &stat))
        return -1;
    if (!stat.u32CurPacks)
        return 0;
    if (stat.u32CurPacks > 4096)
        return -1;
    MI_VENC_Stream_t stream = {0};
    stream.u32PackCount = stat.u32CurPacks;
    stream.pstPack = calloc(stream.u32PackCount, sizeof *stream.pstPack);
    if (!stream.pstPack)
        return -1;
    MI_VENC_Pack_t *allocated_packs = stream.pstPack;
    if (MI_VENC_GetStream(0, &stream, 0)) {
        free(stream.pstPack);
        return -1;
    }
    rc = -1;
    size_t bytes = 0;
    if (!stream.u32PackCount || stream.u32PackCount > stat.u32CurPacks ||
        !stream.pstPack[stream.u32PackCount - 1].bFrameEnd)
        goto release;
    for (MI_U32 i = 0; i < stream.u32PackCount; ++i) {
        MI_VENC_Pack_t *pack = &stream.pstPack[i];
        if (!pack->pu8Addr || pack->u32Offset > pack->u32Len ||
            pack->u32Len - pack->u32Offset > 16U * 1024U * 1024U - bytes)
            goto release;
        bytes += pack->u32Len - pack->u32Offset;
    }
    if (bytes > cap.au_capacity) {
        uint8_t *new_au = realloc(cap.au, bytes);
        if (!new_au)
            goto release;
        cap.au = new_au;
        cap.au_capacity = bytes;
    }
    size_t cursor = 0;
    for (MI_U32 i = 0; i < stream.u32PackCount; ++i) {
        MI_VENC_Pack_t *pack = &stream.pstPack[i];
        size_t length = pack->u32Len - pack->u32Offset;
        memcpy(cap.au + cursor, pack->pu8Addr + pack->u32Offset, length);
        cursor += length;
    }
    /* Interpret this ABI's PTS as microseconds; confirm units on the board.
     * All NALs of one AU share the timestamp; this is not IMU time sync. */
    uint64_t pts = stream.pstPack[0].u64PTS;
    uint64_t ticks = cap.frames * 90000U / (unsigned)cap.fps;
    if (!cap.have_pts) {
        cap.first_pts = pts;
        cap.have_pts = 1;
    }
    if (pts) {
        if (pts >= cap.first_pts) {
            uint64_t us = pts - cap.first_pts;
            ticks = (us / 1000000U) * 90000U + (us % 1000000U) * 9U / 100U;
        }
    }
    ++cap.frames;
    rc = fpv_hw_emit_au(cap.au, bytes, (uint32_t)ticks, cap.callback, cap.opaque);
release:
    /* Every successful GetStream is paired, including malformed/callback errors. */
    if (MI_VENC_ReleaseStream(0, &stream))
        rc = -1;
    free(allocated_packs);
    return rc < 0 ? -1 : 1;
}
