/* VO/HDMI and VDEC setup adapted from OpenIPC vdec (https://openipc.org).
 * Original source and license records are retained under third_party.
 * Targets Hi3536DV100 MPI. Never reset all channels or destroy all VB pools. */
#include "fpv/platform.h"
#include <mpi_sys.h>
#include <mpi_vb.h>
#include <mpi_vdec.h>
#include <mpi_vo.h>
#include <mpi_hdmi.h>
#include <hi_defines.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct {
    int vb, sys, decoder_pool, vdec, receiving, bound;
    int vo, layer, channel, hdmi_init, hdmi_open, hdmi_callback;
    int callbacks_ready, ready, have_timestamp;
    uint32_t last_timestamp;
    uint64_t extended_timestamp;
    HI_HDMI_ATTR_S hdmi_attr;
    HI_HDMI_CALLBACK_FUNC_S callback;
} display;
static pthread_mutex_t hdmi_lock = PTHREAD_MUTEX_INITIALIZER;

static MPP_CHN_S endpoint(MOD_ID_E module)
{
    MPP_CHN_S p = {0};
    p.enModId = module;
    return p;
}

static void hdmi_event(HI_HDMI_EVENT_TYPE_E event, HI_VOID *opaque)
{
    (void)opaque;
    pthread_mutex_lock(&hdmi_lock);
    if (display.callbacks_ready) {
        HI_S32 rc = HI_SUCCESS;
        if (event == HI_HDMI_EVENT_HOTPLUG) {
            HI_HDMI_SINK_CAPABILITY_S capability = {0};
            rc = HI_MPI_HDMI_GetSinkCapability(HI_HDMI_ID_0, &capability);
            if (rc == HI_SUCCESS) {
                /* Preserve configured output timing across hotplug. */
                rc = HI_MPI_HDMI_SetAttr(HI_HDMI_ID_0, &display.hdmi_attr);
                if (rc == HI_SUCCESS)
                    rc = HI_MPI_HDMI_Start(HI_HDMI_ID_0);
            }
        } else if (event == HI_HDMI_EVENT_NO_PLUG) {
            rc = HI_MPI_HDMI_Stop(HI_HDMI_ID_0);
        }
        if (rc != HI_SUCCESS)
            fprintf(stderr, "HDMI event %d failed: 0x%x\n", event, (unsigned)rc);
    }
    pthread_mutex_unlock(&hdmi_lock);
}

void fpv_display_close(void)
{
    display.ready = 0;
    /* Do not hold lock while unregister waits for an in-flight callback. */
    pthread_mutex_lock(&hdmi_lock);
    display.callbacks_ready = 0;
    pthread_mutex_unlock(&hdmi_lock);
    if (display.hdmi_callback)
        HI_MPI_HDMI_UnRegCallbackFunc(HI_HDMI_ID_0, &display.callback);
    if (display.hdmi_open) {
        HI_MPI_HDMI_Stop(HI_HDMI_ID_0);
        HI_MPI_HDMI_Close(HI_HDMI_ID_0);
    }
    if (display.hdmi_init)
        HI_MPI_HDMI_DeInit();
    MPP_CHN_S src = endpoint(HI_ID_VDEC), dst = endpoint(HI_ID_VOU);
    if (display.bound)
        HI_MPI_SYS_UnBind(&src, &dst);
    if (display.receiving)
        HI_MPI_VDEC_StopRecvStream(0);
    if (display.vdec)
        HI_MPI_VDEC_DestroyChn(0);
    if (display.channel)
        HI_MPI_VO_DisableChn(0, 0);
    if (display.layer)
        HI_MPI_VO_DisableVideoLayer(0);
    if (display.vo)
        HI_MPI_VO_Disable(0);
    if (display.decoder_pool)
        HI_MPI_VB_ExitModCommPool(VB_UID_VDEC);
    if (display.sys)
        HI_MPI_SYS_Exit();
    if (display.vb)
        HI_MPI_VB_Exit();
    memset(&display, 0, sizeof display);
}

int fpv_display_open(const fpv_config *cfg, char *error, size_t error_size)
{
    HI_S32 rc = 0;
    const char *stage = "configuration";
    if (display.vb || !cfg || cfg->width < 16 || cfg->height < 16 || cfg->width > 4096 ||
        cfg->height > 2160 || cfg->display_buffers < 1 || cfg->display_buffers > 8 ||
        (strcmp(cfg->output_mode, "720p60") && strcmp(cfg->output_mode, "1080p60"))) {
        if (error && error_size)
            snprintf(error, error_size, "invalid or already open Hi3536 display");
        return -1;
    }
    HI_U32 out_width = !strcmp(cfg->output_mode, "1080p60") ? 1920U : 1280U;
    HI_U32 out_height = out_width == 1920U ? 1080U : 720U;
    VO_INTF_SYNC_E timing = out_width == 1920U ? VO_OUTPUT_1080P60 : VO_OUTPUT_720P60;
    /* Reject an active decoder instead of destroying firmware-owned channels. */
    for (int ch = 0; ch < VDEC_MAX_CHN_NUM; ++ch) {
        VDEC_CHN_ATTR_S existing;
        if (HI_MPI_VDEC_GetChnAttr(ch, &existing) == HI_SUCCESS) {
            if (error && error_size)
                snprintf(error, error_size,
                         "VDEC channel %d already active; use dedicated MPP ownership", ch);
            return -1;
        }
    }
#define HI_TRY(call)                                                                               \
    do {                                                                                           \
        stage = #call;                                                                             \
        rc = (call);                                                                               \
        if (rc != HI_SUCCESS)                                                                      \
            goto fail;                                                                             \
    } while (0)
    VB_CONF_S vb = {0};
    vb.u32MaxPoolCnt = 16;
    HI_TRY(HI_MPI_VB_SetConf(&vb));
    HI_TRY(HI_MPI_VB_Init());
    display.vb = 1;
    MPP_SYS_CONF_S sys = {.u32AlignWidth = 16};
    HI_TRY(HI_MPI_SYS_SetConf(&sys));
    HI_TRY(HI_MPI_SYS_Init());
    display.sys = 1;
    /* SDK module pools supply decoded pictures and temporal MV buffers. */
    memset(&vb, 0, sizeof vb);
    HI_U32 width = ((HI_U32)cfg->width + 15U) & ~15U;
    HI_U32 height = ((HI_U32)cfg->height + 15U) & ~15U;
    vb.u32MaxPoolCnt = 2;
    vb.astCommPool[0].u32BlkCnt = (HI_U32)cfg->display_buffers + 4U;
    vb.astCommPool[1].u32BlkCnt = 4;
    VB_PIC_BLK_SIZE(width, height, PT_H265, vb.astCommPool[0].u32BlkSize);
    VB_PMV_BLK_SIZE(width, height, PT_H265, vb.astCommPool[1].u32BlkSize);
    HI_TRY(HI_MPI_VB_SetModPoolConf(VB_UID_VDEC, &vb));
    HI_TRY(HI_MPI_VB_InitModCommPool(VB_UID_VDEC));
    display.decoder_pool = 1;
    VDEC_CHN_ATTR_S decoder = {0};
    decoder.enType = PT_H265;
    decoder.u32BufSize = 8U * 1024U * 1024U;
    decoder.u32Priority = 128;
    decoder.u32PicWidth = width;
    decoder.u32PicHeight = height;
    decoder.stVdecVideoAttr.bTemporalMvpEnable = HI_TRUE;
    decoder.stVdecVideoAttr.enMode = VIDEO_MODE_STREAM;
    decoder.stVdecVideoAttr.u32RefFrameNum = 2;
    HI_TRY(HI_MPI_VDEC_CreateChn(0, &decoder));
    display.vdec = 1;
    HI_TRY(HI_MPI_VDEC_SetDisplayMode(0, VIDEO_DISPLAY_MODE_PREVIEW));
    HI_TRY(HI_MPI_VO_SetDispBufLen(0, (HI_U32)cfg->display_buffers));
    HI_TRY(HI_MPI_VO_SetVideoLayerPartitionMode(0, VO_PART_MODE_SINGLE));
    VO_PUB_ATTR_S vo = {0};
    vo.enIntfType = VO_INTF_HDMI;
    vo.enIntfSync = timing;
    HI_TRY(HI_MPI_VO_SetPubAttr(0, &vo));
    HI_TRY(HI_MPI_VO_Enable(0));
    display.vo = 1;
    VO_VIDEO_LAYER_ATTR_S layer = {0};
    layer.enPixFormat = PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    layer.u32DispFrmRt = 60;
    layer.stDispRect.u32Width = out_width;
    layer.stDispRect.u32Height = out_height;
    layer.stImageSize.u32Width = out_width;
    layer.stImageSize.u32Height = out_height;
    HI_TRY(HI_MPI_VO_SetVideoLayerAttr(0, &layer));
    HI_TRY(HI_MPI_VO_EnableVideoLayer(0));
    display.layer = 1;
    VO_CHN_ATTR_S channel = {0};
    channel.u32Priority = 0;
    channel.stRect.u32Width = out_width;
    channel.stRect.u32Height = out_height;
    HI_TRY(HI_MPI_VO_SetChnAttr(0, 0, &channel));
    VO_CHN_PARAM_S channel_param = {0};
    channel_param.stAspectRatio.enMode = ASPECT_RATIO_AUTO;
    HI_TRY(HI_MPI_VO_SetChnParam(0, 0, &channel_param));
    HI_TRY(HI_MPI_VO_EnableChn(0, 0));
    display.channel = 1;
    MPP_CHN_S src = endpoint(HI_ID_VDEC), dst = endpoint(HI_ID_VOU);
    HI_TRY(HI_MPI_SYS_Bind(&src, &dst));
    display.bound = 1;
    HI_TRY(HI_MPI_VDEC_StartRecvStream(0));
    display.receiving = 1;
    HI_TRY(HI_MPI_HDMI_Init());
    display.hdmi_init = 1;
    HI_TRY(HI_MPI_HDMI_Open(HI_HDMI_ID_0));
    display.hdmi_open = 1;
    HI_TRY(HI_MPI_HDMI_GetAttr(HI_HDMI_ID_0, &display.hdmi_attr));
    HI_HDMI_ATTR_S *hdmi = &display.hdmi_attr;
    hdmi->bEnableHdmi = HI_TRUE;
    hdmi->bEnableVideo = HI_TRUE;
    hdmi->enVideoFmt = out_width == 1920U ? HI_HDMI_VIDEO_FMT_1080P_60 : HI_HDMI_VIDEO_FMT_720P_60;
    hdmi->enVidOutMode = HI_HDMI_VIDEO_MODE_RGB444;
    hdmi->enDeepColorMode = HI_HDMI_DEEP_COLOR_OFF;
    hdmi->bxvYCCMode = HI_FALSE;
    hdmi->bEnableAudio = HI_FALSE;
    hdmi->bEnableAviInfoFrame = HI_TRUE;
    hdmi->bEnableAudInfoFrame = HI_FALSE;
    hdmi->bEnableSpdInfoFrame = HI_FALSE;
    hdmi->bEnableMpegInfoFrame = HI_FALSE;
    hdmi->bHDCPEnable = HI_FALSE;
    hdmi->b3DEnable = HI_FALSE;
    hdmi->enDefaultMode = HI_HDMI_FORCE_HDMI;
    HI_TRY(HI_MPI_HDMI_SetAttr(HI_HDMI_ID_0, hdmi));
    display.callback.pfnHdmiEventCallback = hdmi_event;
    display.callback.pPrivateData = &display;
    HI_TRY(HI_MPI_HDMI_RegCallbackFunc(HI_HDMI_ID_0, &display.callback));
    display.hdmi_callback = 1;
    pthread_mutex_lock(&hdmi_lock);
    display.callbacks_ready = 1;
    pthread_mutex_unlock(&hdmi_lock);
    HI_TRY(HI_MPI_HDMI_Start(HI_HDMI_ID_0));
    display.ready = 1;
    return 0;
fail:
    if (error && error_size)
        snprintf(error, error_size, "%s failed (0x%x)", stage, (unsigned)rc);
    fpv_display_close();
    return -1;
#undef HI_TRY
}

int fpv_display_write(const uint8_t *annexb, size_t size, uint32_t timestamp,
                      int end_of_access_unit)
{
    if (!display.ready || !annexb || size < 5 || size > 4U * 1024U * 1024U + 4U)
        return -1;
    size_t prefix =
        annexb[0] == 0 && annexb[1] == 0 && annexb[2] == 1
            ? 3U
            : (size >= 6 && annexb[0] == 0 && annexb[1] == 0 && annexb[2] == 0 && annexb[3] == 1
                   ? 4U
                   : 0U);
    if (!prefix || (annexb[prefix] & 0x80) || !(annexb[prefix + 1] & 7))
        return -1;
    /* Extend RTP rollover before converting 90 kHz to SDK microseconds. */
    uint64_t extended = display.extended_timestamp;
    if (!display.have_timestamp)
        extended = 0;
    else {
        int32_t delta = (int32_t)(timestamp - display.last_timestamp);
        if (delta > 0)
            extended += (uint32_t)delta;
    }
    VDEC_STREAM_S stream = {0};
    stream.pu8Addr = (HI_U8 *)(uintptr_t)annexb;
    stream.u32Len = (HI_U32)size;
    stream.u64PTS = (extended / 90000U) * 1000000U + (extended % 90000U) * 100U / 9U;
    stream.bEndOfFrame = end_of_access_unit ? HI_TRUE : HI_FALSE;
    /* Stream mode accepts NALs rather than assuming each NAL is a full frame.
     * A bounded wait applies backpressure without dropping a random slice. */
    HI_S32 rc = HI_MPI_VDEC_SendStream(0, &stream, 100);
    if (rc != HI_SUCCESS) {
        fprintf(stderr, "HI_MPI_VDEC_SendStream failed: 0x%x\n", (unsigned)rc);
        return -1;
    }
    display.extended_timestamp = extended;
    display.last_timestamp = timestamp;
    display.have_timestamp = 1;
    return 0;
}
