/* -*- Mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
   Copyright (C) 2010 Red Hat, Inc.

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#include "config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SHM_H
#include <sys/shm.h>
#endif

#ifdef HAVE_SYS_IPC_H
#include <sys/ipc.h>
#endif

#include "glib-compat.h"
#include "spice-client.h"
#include "spice-common.h"

#include "spice-marshal.h"
#include "spice-channel-priv.h"
#include "spice-session-priv.h"
#include "channel-display-priv.h"
#include "decode.h"
#include "common/rect.h"


/**
 * SECTION:channel-display
 * @short_description: remote display area
 * @title: Display Channel
 * @section_id:
 * @see_also: #SpiceChannel, and the GTK widget #SpiceDisplay
 * @stability: Stable
 * @include: channel-display.h
 *
 * A class that handles the rendering of the remote display and inform
 * of its updates.
 *
 * The creation of the main graphic buffer is signaled with
 * #SpiceDisplayChannel::display-primary-create.
 *
 * The update of regions is notified by
 * #SpiceDisplayChannel::display-invalidate signals.
 */

#define SPICE_DISPLAY_CHANNEL_GET_PRIVATE(obj)                                  \
    (G_TYPE_INSTANCE_GET_PRIVATE((obj), SPICE_TYPE_DISPLAY_CHANNEL, SpiceDisplayChannelPrivate))

#define MONITORS_MAX 256


#ifdef FUSIONDATA_DEV
/* add by yhoon17, 20170829 : */
#include <assert.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#ifndef G_OS_WIN32
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

typedef struct StreamRenderContext
{   
    int nSock;
    int nMajorProtocol;
    int nMinorProtocol;
    
    /* config */
    int nStreamPort;
    int nStreamOnMovieDetection;
    int nOnVariationCapture;
    int nMaxSamplingFps;
    int nAudioSyncNot;
    
    SpiceChannel *channel;
    SpiceRect dest;
    
    GThread *pstThread;
    GCond stCond;
    GMutex stMutex;
    
    GQueue *pstMsgQueue;
    GQueue *pstLatencyQueue;
    
    int nUse;
    int nStatus;
    int nRgbStride;
    uint32_t unSequenceNum;
    uint64_t ulStatTimeStamp;
    guint unTimeoutId;
    
    void *pData;
} STREAM_RENDER_CONTEXT_T;


typedef struct EncoderHeader
{   
    uint32_t unTotalSize;
    uint32_t unHeaderSize;
    uint32_t unDataSize;
    uint32_t nWidth;
    uint32_t nHeight;
    uint32_t nSequenceNum;
    uint32_t unCodec;
    uint32_t unCodecPrivateHeaderSize;
} ENCODER_HEADER_T;


typedef struct Es_Private_header
{   
    uint32_t unMp4esHeaderSize;
    uint32_t unWhetherSync;
    uint32_t unPaddingForMemAlign[4 - ( (sizeof(ENCODER_HEADER_T) / 32) % 4)];
} ES_PRIVATE_HEADER_T;
#endif

struct _SpiceDisplayChannelPrivate {
    GHashTable                  *surfaces;
    display_surface             *primary;
    display_cache               *images;
    display_cache               *palettes;
    SpiceImageCache             image_cache;
    SpicePaletteCache           palette_cache;
    SpiceImageSurfaces          image_surfaces;
    SpiceGlzDecoderWindow       *glz_window;
    display_stream              **streams;
    int                         nstreams;
    gboolean                    mark;
    guint                       mark_false_event_id;
    GArray                      *monitors;
    guint                       monitors_max;
    gboolean                    enable_adaptive_streaming;
#ifdef G_OS_WIN32
    HDC dc;
#endif
    char * report;
#ifdef FUSIONDATA_DEV
    /* add by yhoon17, 20180329 : 동영상 가속기 STREAM_RENDER_CONTEXT */
    STREAM_RENDER_CONTEXT_T stStreamRenderCtx;
#endif
};

G_DEFINE_TYPE(SpiceDisplayChannel, spice_display_channel, SPICE_TYPE_CHANNEL)

/* Properties */
enum {
    PROP_0,
    PROP_WIDTH,
    PROP_HEIGHT,
    PROP_MONITORS,
    PROP_MONITORS_MAX,
    PROP_REPORT,
    PROP_VA_SESSIONS,
};

enum {
    SPICE_DISPLAY_PRIMARY_CREATE,
    SPICE_DISPLAY_PRIMARY_DESTROY,
    SPICE_DISPLAY_INVALIDATE,
    SPICE_DISPLAY_MARK,

    SPICE_DISPLAY_LAST_SIGNAL,
};


#ifdef FUSIONDATA_DEV
/* add by yhoon17, 20170829 : */
enum
{
    STREAM_RENDER_START = 0,
    STREAM_RENDER_PAUSE_REQ,
    STREAM_RENDER_PAUSE_RSP,
    STREAM_RENDER_RUN_REQ,
    STREAM_RENDER_RUN_RSP,
    STREAM_RENDER_FIN_REQ,
    STREAM_RENDER_FIN_RSP,
};

int _nStreamPort = 0;
int _nStreamOnMovieDetection = 0;
int _nOnVariationCapture = 0;
int _nMaxSamplingFps = 0;
int _nAudioSyncNot = 0;

int ReadSpiceStreamConfig(void);
int GetOptFilePath(const gchar *pszName, char *pszPath);
int TrimSpace(char *pszStr);
int TrimLeftSpace(char *pszStr);
int TrimRightSpace(char *pszStr);

static void DisplayHandleStreamConfig(SpiceChannel *channel, SpiceMsgIn *in);
static void DisplayHandleStreamFrameData(SpiceChannel *channel, SpiceMsgIn *in);
SPICE_GNUC_UNUSED static void DisplayChannelSendMsgStreamConfig(SpiceChannel *channel, uint32_t unStreamPort, uint32_t unStreamOnMovieDetection, uint32_t unOnVariationCapture, uint32_t unMaxSamplingFps);
SPICE_GNUC_UNUSED static void DisplayChannelSendMsgStreamStatData(SpiceChannel *channel, uint32_t unBacklogSize);

static void StreamRenderContextInit(GObject *object);
static void StreamRenderContextDestructor(GObject *object);
void *StreamRenderThread(void *data);
static void JoinStreamRenderThread(GObject *object);
static void SetStreamRenderConfig(SpiceChannel *channel);
static int ProcessPauseFinRequest(SpiceChannel *channel);
static gboolean StreamFrameSchedule(SpiceChannel *channel);
static gboolean StreamFrameDataMoveIntoMsgq(SpiceChannel *channel);
static void SendStreamFrameDataToDecoder(SpiceChannel *channel, SpiceMsgIn *in);
static void ResetStreamFrameSchedule(SpiceChannel *channel);
static gboolean DisplayStreamRenderFrame(STREAM_RENDER_CONTEXT_T *pstStreamRenderCtx);
static uint32_t DisplayChannelGetDecodingQueueSize(SpiceChannel *channel);
static int ReadEncodedFrame(SpiceChannel *channel, char *recvHeaderbuf, char *dataFramebuf);
static void MsgInUnrefFuncGpointer(gpointer data, gpointer user_data);
static gboolean MsgInUnrefFuncSpiceMsgIn(SpiceMsgIn *in);
int GetStreamRenderSocket(SpiceChannel *channel);
int SendDataToSocket(int nSock, void *pData, int nLength);
int RecvDataFromSocket(int nSock, void *pData, int nLength);
#endif

static guint signals[SPICE_DISPLAY_LAST_SIGNAL];

static void spice_display_channel_up(SpiceChannel *channel);
static void channel_set_handlers(SpiceChannelClass *klass);

static void clear_surfaces(SpiceChannel *channel, gboolean keep_primary);
static void clear_streams(SpiceChannel *channel);
static display_surface *find_surface(SpiceDisplayChannelPrivate *c, guint32 surface_id);
static gboolean display_stream_render(display_stream *st);
static void spice_display_channel_reset(SpiceChannel *channel, gboolean migrating);
static void spice_display_channel_reset_capabilities(SpiceChannel *channel);
static void destroy_canvas(display_surface *surface);
static void _msg_in_unref_func(gpointer data, gpointer user_data);
static void display_session_mm_time_reset_cb(SpiceSession *session, gpointer data);

/* ------------------------------------------------------------------ */

static void spice_display_channel_dispose(GObject *object)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(object)->priv;

    if (c->mark_false_event_id != 0) {
        g_source_remove(c->mark_false_event_id);
        c->mark_false_event_id = 0;
    }

#ifdef FUSIONDATA_DEV
    /* add by yhoon17, 20180329 : 동영상 가속기 destructor */
    g_message("StreamRenderContext is disposed.");

    StreamRenderContextDestructor(object);

    g_message("StreamRenderContext is destructed.");
#endif

    if (G_OBJECT_CLASS(spice_display_channel_parent_class)->dispose)
        G_OBJECT_CLASS(spice_display_channel_parent_class)->dispose(object);
}

static void spice_display_channel_finalize(GObject *object)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(object)->priv;

    g_clear_pointer(&c->monitors, g_array_unref);
    clear_surfaces(SPICE_CHANNEL(object), FALSE);
    g_hash_table_unref(c->surfaces);
    clear_streams(SPICE_CHANNEL(object));
    g_clear_pointer(&c->palettes, cache_unref);



    if (G_OBJECT_CLASS(spice_display_channel_parent_class)->finalize)
        G_OBJECT_CLASS(spice_display_channel_parent_class)->finalize(object);
}

static void spice_display_channel_constructed(GObject *object)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(object)->priv;
    SpiceSession *s = spice_channel_get_session(SPICE_CHANNEL(object));

    g_return_if_fail(s != NULL);
    spice_session_get_caches(s, &c->images, &c->glz_window);
    c->palettes = cache_new(g_free);

    g_return_if_fail(c->glz_window != NULL);
    g_return_if_fail(c->images != NULL);
    g_return_if_fail(c->palettes != NULL);

    c->monitors = g_array_new(FALSE, TRUE, sizeof(SpiceDisplayMonitorConfig));
    spice_g_signal_connect_object(s, "mm-time-reset",
                                  G_CALLBACK(display_session_mm_time_reset_cb),
                                  SPICE_CHANNEL(object), 0);

#ifdef FUSIONDATA_DEV
    /* add by yhoon17, 20180329 :  동영상 가속기 init */
    memset( &c->stStreamRenderCtx, 0x00, sizeof(c->stStreamRenderCtx) );

    c->stStreamRenderCtx.pstMsgQueue = NULL;

    StreamRenderContextInit(object);

    g_message("StreamRenderContext constructed.");
#endif

    if (G_OBJECT_CLASS(spice_display_channel_parent_class)->constructed)
        G_OBJECT_CLASS(spice_display_channel_parent_class)->constructed(object);
}


static void spice_display_get_property(GObject    *object,
                                       guint       prop_id,
                                       GValue     *value,
                                       GParamSpec *pspec)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(object)->priv;

    switch (prop_id) {
    case PROP_WIDTH: {
        g_value_set_uint(value, c->primary ? c->primary->width : 0);
        break;
    }
    case PROP_HEIGHT: {
        g_value_set_uint(value, c->primary ? c->primary->height : 0);
        break;
    }
    case PROP_MONITORS: {
        g_value_set_boxed(value, c->monitors);
        break;
    }
    case PROP_MONITORS_MAX: {
        g_value_set_uint(value, c->monitors_max);
        break;
    }
    case PROP_REPORT: {
        g_value_set_static_string(value, c->report);
        break;
    }
    case PROP_VA_SESSIONS: {
        GSList *va_sessions = NULL;
        int i;
        for (i = 0; i < c->nstreams; ++i) {
            if (c->streams[i] && c->streams[i]->hw_accel)
                va_sessions = g_slist_prepend(va_sessions, c->streams[i]->vaapi_session);
        }
        g_value_set_pointer(value, va_sessions);
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

static void spice_display_set_property(GObject      *object,
                                       guint         prop_id,
                                       const GValue *value,
                                       GParamSpec   *pspec)
{
    switch (prop_id) {
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
        break;
    }
}

/* main or coroutine context */
static void spice_display_channel_reset(SpiceChannel *channel, gboolean migrating)
{
#ifdef FUSIONDATA_DEV
    /* add by yhoon17, 20180329 :  동영상 가속기 */
    g_message("spice_display_channel_reset()");
    StreamRenderContextDestructor( &channel->parent );
    StreamRenderContextInit( &channel->parent );
#endif
    /* palettes, images, and glz_window are cleared in the session */
    clear_streams(channel);
    clear_surfaces(channel, TRUE);

    SPICE_CHANNEL_CLASS(spice_display_channel_parent_class)->channel_reset(channel, migrating);
}

static void spice_display_channel_class_init(SpiceDisplayChannelClass *klass)
{
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    SpiceChannelClass *channel_class = SPICE_CHANNEL_CLASS(klass);

    gobject_class->finalize     = spice_display_channel_finalize;
    gobject_class->dispose      = spice_display_channel_dispose;
    gobject_class->get_property = spice_display_get_property;
    gobject_class->set_property = spice_display_set_property;
    gobject_class->constructed = spice_display_channel_constructed;

    channel_class->channel_up   = spice_display_channel_up;
    channel_class->channel_reset = spice_display_channel_reset;
    channel_class->channel_reset_capabilities = spice_display_channel_reset_capabilities;

    g_object_class_install_property
        (gobject_class, PROP_HEIGHT,
         g_param_spec_uint("height",
                           "Display height",
                           "The primary surface height",
                           0, G_MAXUINT, 0,
                           G_PARAM_READABLE |
                           G_PARAM_STATIC_STRINGS));

    g_object_class_install_property
        (gobject_class, PROP_WIDTH,
         g_param_spec_uint("width",
                           "Display width",
                           "The primary surface width",
                           0, G_MAXUINT, 0,
                           G_PARAM_READABLE |
                           G_PARAM_STATIC_STRINGS));

    /**
     * SpiceDisplayChannel:monitors:
     *
     * Current monitors configuration.
     *
     * Since: 0.13
     */
    g_object_class_install_property
        (gobject_class, PROP_MONITORS,
         g_param_spec_boxed("monitors",
                            "Display monitors",
                            "The monitors configuration",
                            G_TYPE_ARRAY,
                            G_PARAM_READABLE |
                            G_PARAM_STATIC_STRINGS));

    /**
     * SpiceDisplayChannel:monitors-max:
     *
     * The maximum number of monitors the server or guest supports.
     * May change during client lifetime, for instance guest may
     * reboot or dynamically adjust this.
     *
     * Since: 0.13
     */
    g_object_class_install_property
        (gobject_class, PROP_MONITORS_MAX,
         g_param_spec_uint("monitors-max",
                           "Max display monitors",
                           "The current maximum number of monitors",
                           1, MONITORS_MAX, 1,
                           G_PARAM_READABLE |
                           G_PARAM_STATIC_STRINGS));

    g_object_class_install_property
        (gobject_class, PROP_REPORT,
         g_param_spec_string("stream-report",
                             "Stream report",
                             "Report of stream properties",
                             "",
                             G_PARAM_READABLE |
                             G_PARAM_STATIC_STRINGS));

    g_object_class_install_property
        (gobject_class, PROP_VA_SESSIONS,
         g_param_spec_pointer("va-sessions",
                              "VA Sessions",
                              "VA sessions associated with streams where hw accel is active",
                              G_PARAM_READABLE |
                              G_PARAM_STATIC_STRINGS));

    /**
     * SpiceDisplayChannel::display-primary-create:
     * @display: the #SpiceDisplayChannel that emitted the signal
     * @format: %SPICE_SURFACE_FMT_32_xRGB or %SPICE_SURFACE_FMT_16_555;
     * @width: width resolution
     * @height: height resolution
     * @stride: the buffer stride ("width" padding)
     * @shmid: identifier of the shared memory segment associated with
     * the @imgdata, or -1 if not shm
     * @imgdata: pointer to surface buffer
     *
     * The #SpiceDisplayChannel::display-primary-create signal
     * provides main display buffer data.
     **/
    signals[SPICE_DISPLAY_PRIMARY_CREATE] =
        g_signal_new("display-primary-create",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceDisplayChannelClass,
                                     display_primary_create),
                     NULL, NULL,
                     g_cclosure_user_marshal_VOID__INT_INT_INT_INT_INT_POINTER,
                     G_TYPE_NONE,
                     6,
                     G_TYPE_INT, G_TYPE_INT, G_TYPE_INT,
                     G_TYPE_INT, G_TYPE_INT, G_TYPE_POINTER);

    /**
     * SpiceDisplayChannel::display-primary-destroy:
     * @display: the #SpiceDisplayChannel that emitted the signal
     *
     * The #SpiceDisplayChannel::display-primary-destroy signal is
     * emitted when the primary surface is freed and should not be
     * accessed anymore.
     **/
    signals[SPICE_DISPLAY_PRIMARY_DESTROY] =
        g_signal_new("display-primary-destroy",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceDisplayChannelClass,
                                     display_primary_destroy),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__VOID,
                     G_TYPE_NONE,
                     0);

    /**
     * SpiceDisplayChannel::display-invalidate:
     * @display: the #SpiceDisplayChannel that emitted the signal
     * @x: x position
     * @y: y position
     * @width: width
     * @height: height
     *
     * The #SpiceDisplayChannel::display-invalidate signal is emitted
     * when the rectangular region x/y/w/h of the primary buffer is
     * updated.
     **/
    signals[SPICE_DISPLAY_INVALIDATE] =
        g_signal_new("display-invalidate",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceDisplayChannelClass,
                                     display_invalidate),
                     NULL, NULL,
                     g_cclosure_user_marshal_VOID__INT_INT_INT_INT,
                     G_TYPE_NONE,
                     4,
                     G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);

    /**
     * SpiceDisplayChannel::display-mark:
     * @display: the #SpiceDisplayChannel that emitted the signal
     * @mark: %TRUE when the display mark has been received
     *
     * The #SpiceDisplayChannel::display-mark signal is emitted when
     * the %RED_DISPLAY_MARK command is received, and the display
     * should be exposed.
     **/
    signals[SPICE_DISPLAY_MARK] =
        g_signal_new("display-mark",
                     G_OBJECT_CLASS_TYPE(gobject_class),
                     G_SIGNAL_RUN_FIRST,
                     G_STRUCT_OFFSET(SpiceDisplayChannelClass,
                                     display_mark),
                     NULL, NULL,
                     g_cclosure_marshal_VOID__INT,
                     G_TYPE_NONE,
                     1,
                     G_TYPE_INT);

    g_type_class_add_private(klass, sizeof(SpiceDisplayChannelPrivate));

    sw_canvas_init();
    quic_init();
    rop3_init();
    channel_set_handlers(SPICE_CHANNEL_CLASS(klass));
}

/**
 * spice_display_get_primary:
 * @channel:
 * @surface_id:
 * @primary:
 *
 * Retrieve primary display surface @surface_id.
 *
 * Returns: %TRUE if the primary surface was found and its details
 * collected in @primary.
 */
gboolean spice_display_get_primary(SpiceChannel *channel, guint32 surface_id,
                                   SpiceDisplayPrimary *primary)
{
    g_return_val_if_fail(SPICE_IS_DISPLAY_CHANNEL(channel), FALSE);
    g_return_val_if_fail(primary != NULL, FALSE);

    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    display_surface *surface = find_surface(c, surface_id);

    if (surface == NULL)
        return FALSE;

    g_return_val_if_fail(surface->primary, FALSE);

    primary->format = surface->format;
    primary->width = surface->width;
    primary->height = surface->height;
    primary->stride = surface->stride;
    primary->shmid = surface->shmid;
    primary->data = surface->data;
    primary->marked = c->mark;
    CHANNEL_DEBUG(channel, "get primary %p", primary->data);

    return TRUE;
}

/* ------------------------------------------------------------------ */

static void image_put(SpiceImageCache *cache, uint64_t id, pixman_image_t *image)
{
    SpiceDisplayChannelPrivate *c =
        SPICE_CONTAINEROF(cache, SpiceDisplayChannelPrivate, image_cache);

    cache_add(c->images, id, pixman_image_ref(image));
}

typedef struct _WaitImageData
{
    gboolean lossy;
    SpiceImageCache *cache;
    uint64_t id;
    pixman_image_t *image;
} WaitImageData;

static gboolean wait_image(gpointer data)
{
    gboolean lossy;
    WaitImageData *wait = data;
    SpiceDisplayChannelPrivate *c =
        SPICE_CONTAINEROF(wait->cache, SpiceDisplayChannelPrivate, image_cache);
    pixman_image_t *image = cache_find_lossy(c->images, wait->id, &lossy);

    if (!image || (lossy && !wait->lossy))
        return FALSE;

    wait->image = pixman_image_ref(image);

    return TRUE;
}

static pixman_image_t *image_get(SpiceImageCache *cache, uint64_t id)
{
    WaitImageData wait = {
        .lossy = TRUE,
        .cache = cache,
        .id = id,
        .image = NULL
    };
    if (!g_coroutine_condition_wait(g_coroutine_self(), wait_image, &wait))
        SPICE_DEBUG("wait image got cancelled");

    return wait.image;
}

static void palette_put(SpicePaletteCache *cache, SpicePalette *palette)
{
    SpiceDisplayChannelPrivate *c =
        SPICE_CONTAINEROF(cache, SpiceDisplayChannelPrivate, palette_cache);

    cache_add(c->palettes, palette->unique,
              g_memdup(palette, sizeof(SpicePalette) +
                       palette->num_ents * sizeof(palette->ents[0])));
}

static SpicePalette *palette_get(SpicePaletteCache *cache, uint64_t id)
{
    SpiceDisplayChannelPrivate *c =
        SPICE_CONTAINEROF(cache, SpiceDisplayChannelPrivate, palette_cache);

    /* here the returned pointer is weak, no ref given to caller.  it
     * seems spice canvas usage is exclusively temporary, so it's ok.
     * palette_release is a noop. */
    return cache_find(c->palettes, id);
}

static void palette_remove(SpicePaletteCache *cache, uint64_t id)
{
    SpiceDisplayChannelPrivate *c =
        SPICE_CONTAINEROF(cache, SpiceDisplayChannelPrivate, palette_cache);

    cache_remove(c->palettes, id);
}

static void palette_release(SpicePaletteCache *cache, SpicePalette *palette)
{
    /* there is no refcount of palette, see palette_get() */
}

static void image_put_lossy(SpiceImageCache *cache, uint64_t id,
                            pixman_image_t *surface)
{
    SpiceDisplayChannelPrivate *c =
        SPICE_CONTAINEROF(cache, SpiceDisplayChannelPrivate, image_cache);

#ifndef NDEBUG
    g_warn_if_fail(cache_find(c->images, id) == NULL);
#endif

    cache_add_lossy(c->images, id, pixman_image_ref(surface), TRUE);
}

static void image_replace_lossy(SpiceImageCache *cache, uint64_t id,
                                pixman_image_t *surface)
{
    image_put(cache, id, surface);
}

static pixman_image_t* image_get_lossless(SpiceImageCache *cache, uint64_t id)
{
    WaitImageData wait = {
        .lossy = FALSE,
        .cache = cache,
        .id = id,
        .image = NULL
    };
    if (!g_coroutine_condition_wait(g_coroutine_self(), wait_image, &wait))
        SPICE_DEBUG("wait lossless got cancelled");

    return wait.image;
}

static SpiceCanvas *surfaces_get(SpiceImageSurfaces *surfaces,
                                 uint32_t surface_id)
{
    SpiceDisplayChannelPrivate *c =
        SPICE_CONTAINEROF(surfaces, SpiceDisplayChannelPrivate, image_surfaces);

    display_surface *s =
        find_surface(c, surface_id);

    return s ? s->canvas : NULL;
}

static SpiceImageCacheOps image_cache_ops = {
    .put = image_put,
    .get = image_get,

    .put_lossy = image_put_lossy,
    .replace_lossy = image_replace_lossy,
    .get_lossless = image_get_lossless,
};

static SpicePaletteCacheOps palette_cache_ops = {
    .put     = palette_put,
    .get     = palette_get,
    .release = palette_release,
};

static SpiceImageSurfacesOps image_surfaces_ops = {
    .get = surfaces_get
};

#if defined(G_OS_WIN32)
static HDC create_compatible_dc(void)
{
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        g_warning("create compatible DC failed");
    }
    return dc;
}
#endif

static void spice_display_channel_reset_capabilities(SpiceChannel *channel)
{
    spice_channel_set_capability(SPICE_CHANNEL(channel), SPICE_DISPLAY_CAP_SIZED_STREAM);
    spice_channel_set_capability(SPICE_CHANNEL(channel), SPICE_DISPLAY_CAP_MONITORS_CONFIG);
    spice_channel_set_capability(SPICE_CHANNEL(channel), SPICE_DISPLAY_CAP_COMPOSITE);
    spice_channel_set_capability(SPICE_CHANNEL(channel), SPICE_DISPLAY_CAP_A8_SURFACE);
#ifdef USE_LZ4
    spice_channel_set_capability(SPICE_CHANNEL(channel), SPICE_DISPLAY_CAP_LZ4_COMPRESSION);
#endif
    if (SPICE_DISPLAY_CHANNEL(channel)->priv->enable_adaptive_streaming) {
        spice_channel_set_capability(SPICE_CHANNEL(channel), SPICE_DISPLAY_CAP_STREAM_REPORT);
    }
}

static void destroy_surface(gpointer data)
{
    display_surface *surface = data;

    destroy_canvas(surface);
    g_slice_free(display_surface, surface);
}

static void spice_display_channel_init(SpiceDisplayChannel *channel)
{
    SpiceDisplayChannelPrivate *c;

    c = channel->priv = SPICE_DISPLAY_CHANNEL_GET_PRIVATE(channel);

    c->surfaces = g_hash_table_new_full(NULL, NULL, NULL, destroy_surface);
    c->image_cache.ops = &image_cache_ops;
    c->palette_cache.ops = &palette_cache_ops;
    c->image_surfaces.ops = &image_surfaces_ops;
#if defined(G_OS_WIN32)
    c->dc = create_compatible_dc();
#endif
    c->monitors_max = 1;

    if (g_getenv("SPICE_DISABLE_ADAPTIVE_STREAMING")) {
        SPICE_DEBUG("adaptive video disabled");
        c->enable_adaptive_streaming = FALSE;
    } else {
        c->enable_adaptive_streaming = TRUE;
    }
    spice_display_channel_reset_capabilities(SPICE_CHANNEL(channel));
}

/* ------------------------------------------------------------------ */

static int create_canvas(SpiceChannel *channel, display_surface *surface)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;

    if (surface->primary) {
        if (c->primary) {
            if (c->primary->width == surface->width &&
                c->primary->height == surface->height) {
                CHANNEL_DEBUG(channel, "Reusing existing primary surface");
                return 0;
            }

            g_coroutine_signal_emit(channel, signals[SPICE_DISPLAY_PRIMARY_DESTROY], 0);

            g_hash_table_remove(c->surfaces, GINT_TO_POINTER(c->primary->surface_id));
        }

        CHANNEL_DEBUG(channel, "Create primary canvas");
#if defined(WITH_X11) && defined(HAVE_SYS_SHM_H)
        surface->shmid = shmget(IPC_PRIVATE, surface->size, IPC_CREAT | 0777);
        if (surface->shmid >= 0) {
            surface->data = shmat(surface->shmid, 0, 0);
            if (surface->data == NULL) {
                shmctl(surface->shmid, IPC_RMID, 0);
                surface->shmid = -1;
            }
        }
#else
        surface->shmid = -1;
#endif
    } else {
        surface->shmid = -1;
    }

    if (surface->shmid == -1)
        surface->data = g_malloc0(surface->size);

    g_return_val_if_fail(c->glz_window, 0);

    g_warn_if_fail(surface->canvas == NULL);
    g_warn_if_fail(surface->glz_decoder == NULL);
    g_warn_if_fail(surface->zlib_decoder == NULL);
    g_warn_if_fail(surface->jpeg_decoder == NULL);

    surface->glz_decoder = glz_decoder_new(c->glz_window);
    surface->zlib_decoder = zlib_decoder_new();
    surface->jpeg_decoder = jpeg_decoder_new();

    surface->canvas = canvas_create_for_data(surface->width,
                                             surface->height,
                                             surface->format,
                                             surface->data,
                                             surface->stride,
                                             &c->image_cache,
                                             &c->palette_cache,
                                             &c->image_surfaces,
                                             surface->glz_decoder,
                                             surface->jpeg_decoder,
                                             surface->zlib_decoder);

    g_return_val_if_fail(surface->canvas != NULL, 0);
    g_hash_table_insert(c->surfaces, GINT_TO_POINTER(surface->surface_id), surface);

    if (surface->primary) {
        g_warn_if_fail(c->primary == NULL);
        c->primary = surface;
        g_coroutine_signal_emit(channel, signals[SPICE_DISPLAY_PRIMARY_CREATE], 0,
                                surface->format, surface->width, surface->height,
                                surface->stride, surface->shmid, surface->data);

        if (!spice_channel_test_capability(channel, SPICE_DISPLAY_CAP_MONITORS_CONFIG)) {
            g_array_set_size(c->monitors, 1);
            SpiceDisplayMonitorConfig *config = &g_array_index(c->monitors, SpiceDisplayMonitorConfig, 0);
            config->x = config->y = 0;
            config->width = surface->width;
            config->height = surface->height;
            g_coroutine_object_notify(G_OBJECT(channel), "monitors");
        }
    }

    return 0;
}

static void destroy_canvas(display_surface *surface)
{
    if (surface == NULL)
        return;

    glz_decoder_destroy(surface->glz_decoder);
    zlib_decoder_destroy(surface->zlib_decoder);
    jpeg_decoder_destroy(surface->jpeg_decoder);

    if (surface->shmid == -1) {
        g_free(surface->data);
    }
#ifdef HAVE_SYS_SHM_H
    else {
        shmdt(surface->data);
        shmctl(surface->shmid, IPC_RMID, 0);
    }
#endif
    surface->shmid = -1;
    surface->data = NULL;

    surface->canvas->ops->destroy(surface->canvas);
    surface->canvas = NULL;
}

static display_surface *find_surface(SpiceDisplayChannelPrivate *c, guint32 surface_id)
{
    if (c->primary && c->primary->surface_id == surface_id)
        return c->primary;

    return g_hash_table_lookup(c->surfaces, GINT_TO_POINTER(surface_id));
}

/* main or coroutine context */
static void clear_surfaces(SpiceChannel *channel, gboolean keep_primary)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    GHashTableIter iter;
    display_surface *surface;

    if (!keep_primary) {
        c->primary = NULL;
        g_coroutine_signal_emit(channel, signals[SPICE_DISPLAY_PRIMARY_DESTROY], 0);
    }

    g_hash_table_iter_init(&iter, c->surfaces);
    while (g_hash_table_iter_next(&iter, NULL, (gpointer*)&surface)) {

        if (keep_primary && surface->primary) {
            CHANNEL_DEBUG(channel, "keeping existing primary surface, migration or reset");
            continue;
        }

        g_hash_table_iter_remove(&iter);
    }
}

/* coroutine context */
static void emit_invalidate(SpiceChannel *channel, SpiceRect *bbox)
{
    g_coroutine_signal_emit(channel, signals[SPICE_DISPLAY_INVALIDATE], 0,
                            bbox->left, bbox->top,
                            bbox->right - bbox->left,
                            bbox->bottom - bbox->top);
}

/* ------------------------------------------------------------------ */

/* coroutine context */
static void spice_display_channel_up(SpiceChannel *channel)
{
    SpiceMsgOut *out;
    SpiceSession *s = spice_channel_get_session(channel);
    SpiceMsgcDisplayInit init;
    SpiceMsgcDisplayPreferredCompression pref_comp_msg;
    int cache_size;
    int glz_window_size;
    SpiceImageCompression preferred_compression = SPICE_IMAGE_COMPRESSION_INVALID;

    g_object_get(s,
                 "cache-size", &cache_size,
                 "glz-window-size", &glz_window_size,
                 "preferred-compression", &preferred_compression,
                 NULL);
    CHANNEL_DEBUG(channel, "%s: cache_size %d, glz_window_size %d (bytes)", __FUNCTION__,
                  cache_size, glz_window_size);
    init.pixmap_cache_id = 1;
    init.glz_dictionary_id = 1;
    init.pixmap_cache_size = cache_size / 4; /* pixels */
    init.glz_dictionary_window_size = glz_window_size / 4; /* pixels */
    out = spice_msg_out_new(channel, SPICE_MSGC_DISPLAY_INIT);
    out->marshallers->msgc_display_init(out->marshaller, &init);
    spice_msg_out_send_internal(out);

    /* notify of existence of this monitor */
    g_coroutine_object_notify(G_OBJECT(channel), "monitors");

    if (spice_channel_test_capability(channel, SPICE_DISPLAY_CAP_PREF_COMPRESSION) &&
            preferred_compression > SPICE_IMAGE_COMPRESSION_INVALID) {
        pref_comp_msg.image_compression = preferred_compression;
        out = spice_msg_out_new(channel, SPICE_MSGC_DISPLAY_PREFERRED_COMPRESSION);
        out->marshallers->msgc_display_preferred_compression(out->marshaller, &pref_comp_msg);
        spice_msg_out_send_internal(out);
    }
}

#define DRAW(type) {                                                    \
        display_surface *surface =                                      \
            find_surface(SPICE_DISPLAY_CHANNEL(channel)->priv,          \
                op->base.surface_id);                                   \
        g_return_if_fail(surface != NULL);                              \
        surface->canvas->ops->draw_##type(surface->canvas, &op->base.box, \
                                          &op->base.clip, &op->data);   \
        if (surface->primary) {                                         \
            emit_invalidate(channel, &op->base.box);                    \
        }                                                               \
}

/* coroutine context */
static void display_handle_mode(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayMode *mode = spice_msg_in_parsed(in);
    display_surface *surface;

    g_warn_if_fail(c->mark == FALSE);

    surface = g_slice_new0(display_surface);
    surface->format  = mode->bits == 32 ?
        SPICE_SURFACE_FMT_32_xRGB : SPICE_SURFACE_FMT_16_555;
    surface->width   = mode->x_res;
    surface->height  = mode->y_res;
    surface->stride  = surface->width * 4;
    surface->size    = surface->height * surface->stride;
    surface->primary = true;
    create_canvas(channel, surface);
}

/* coroutine context */
static void display_handle_mark(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;

    CHANNEL_DEBUG(channel, "%s", __FUNCTION__);
    g_return_if_fail(c->primary != NULL);
#ifdef EXTRA_CHECKS
    g_warn_if_fail(c->mark == FALSE);
#endif

    c->mark = TRUE;
    g_coroutine_signal_emit(channel, signals[SPICE_DISPLAY_MARK], 0, TRUE);
}

/* coroutine context */
static void display_handle_reset(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    display_surface *surface = c->primary;

    CHANNEL_DEBUG(channel, "%s: TODO detach_from_screen", __FUNCTION__);

    if (surface != NULL)
        surface->canvas->ops->clear(surface->canvas);

    cache_clear(c->palettes);

    c->mark = FALSE;
    g_coroutine_signal_emit(channel, signals[SPICE_DISPLAY_MARK], 0, FALSE);
}

/* coroutine context */
static void display_handle_copy_bits(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayCopyBits *op = spice_msg_in_parsed(in);
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    display_surface *surface = find_surface(c, op->base.surface_id);

    g_return_if_fail(surface != NULL);
    surface->canvas->ops->copy_bits(surface->canvas, &op->base.box,
                                    &op->base.clip, &op->src_pos);
    if (surface->primary) {
        emit_invalidate(channel, &op->base.box);
    }
}

/* coroutine context */
static void display_handle_inv_list(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceResourceList *list = spice_msg_in_parsed(in);
    int i;

    for (i = 0; i < list->count; i++) {
        guint64 id = list->resources[i].id;

        switch (list->resources[i].type) {
        case SPICE_RES_TYPE_PIXMAP:
            if (!cache_remove(c->images, id))
                SPICE_DEBUG("fail to remove image %" G_GUINT64_FORMAT, id);
            break;
        default:
            g_return_if_reached();
            break;
        }
    }
}

/* coroutine context */
static void display_handle_inv_pixmap_all(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;

    spice_channel_handle_wait_for_channels(channel, in);
    cache_clear(c->images);
}

/* coroutine context */
static void display_handle_inv_palette(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayInvalOne* op = spice_msg_in_parsed(in);

    palette_remove(&c->palette_cache, op->id);
}

/* coroutine context */
static void display_handle_inv_palette_all(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;

    cache_clear(c->palettes);
}

/* ------------------------------------------------------------------ */

static void display_update_stream_region(display_stream *st)
{
    int i;

    switch (st->clip->type) {
    case SPICE_CLIP_TYPE_RECTS:
        region_clear(&st->region);
        for (i = 0; i < st->clip->rects->num_rects; i++) {
            region_add(&st->region, &st->clip->rects->rects[i]);
        }
        st->have_region = true;
        break;
    case SPICE_CLIP_TYPE_NONE:
    default:
        st->have_region = false;
        break;
    }
}

/* coroutine context */
static void display_handle_stream_create(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayStreamCreate *op = spice_msg_in_parsed(in);
    display_stream *st;

    CHANNEL_DEBUG(channel, "%s: id %d", __FUNCTION__, op->id);

    if (op->id >= c->nstreams) {
        int n = c->nstreams;
        if (!c->nstreams) {
            c->nstreams = 1;
        }
        while (op->id >= c->nstreams) {
            c->nstreams *= 2;
        }
        c->streams = realloc(c->streams, c->nstreams * sizeof(c->streams[0]));
        memset(c->streams + n, 0, (c->nstreams - n) * sizeof(c->streams[0]));
    }
    g_return_if_fail(c->streams[op->id] == NULL);
    c->streams[op->id] = g_new0(display_stream, 1);
    st = c->streams[op->id];

    st->msg_create = in;
    spice_msg_in_ref(in);
    st->clip = &op->clip;
    st->codec = op->codec_type;
    st->surface = find_surface(c, op->surface_id);
    st->msgq = g_queue_new();
    st->channel = channel;
    st->drops_seqs_stats_arr = g_array_new(FALSE, FALSE, sizeof(drops_sequence_stats));

    region_init(&st->region);
    display_update_stream_region(st);

    switch (st->codec) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        stream_mjpeg_init(st);
        break;
    }

    char *str = g_strdup_printf(
                    "HW accel %s, decode: 0ms, dropped 0, in rate 0, out rate 0",
                    st->hw_accel ? "on" : "off");
    g_free(c->report);
    c->report = str;
    g_coroutine_object_notify(G_OBJECT(channel), "stream-report");
}

/* coroutine or main context */
static gboolean display_stream_schedule(display_stream *st)
{
    SpiceSession *session = spice_channel_get_session(st->channel);
    guint32 time, d;
    SpiceStreamDataHeader *op;
    SpiceMsgIn *in;
    gboolean invalid_mm_time;

    SPICE_DEBUG("%s", __FUNCTION__);
    if (st->timeout || !session)
        return TRUE;

    time = spice_session_get_mm_time(session, &invalid_mm_time);
    in = g_queue_peek_head(st->msgq);

    if (in == NULL) {
        return TRUE;
    }

    op = spice_msg_in_parsed(in);
    if (invalid_mm_time) {
        SPICE_DEBUG("scheduling next stream render in %u ms", 0);
        st->timeout = g_timeout_add(0, (GSourceFunc)display_stream_render, st);
        return TRUE;
    }
    if (time < op->multi_media_time) {
        d = op->multi_media_time - time;
        SPICE_DEBUG("scheduling next stream render in %u ms", d);
        st->timeout = g_timeout_add(d, (GSourceFunc)display_stream_render, st);
        return TRUE;
    } else {
        SPICE_DEBUG("%s: rendering too late by %u ms (ts: %u, mmtime: %u), dropping ",
                    __FUNCTION__, time - op->multi_media_time,
                    op->multi_media_time, time);
        in = g_queue_pop_head(st->msgq);
        spice_msg_in_unref(in);
        st->num_drops_on_playback++;
        if (g_queue_get_length(st->msgq) == 0)
            return TRUE;
    }

    return FALSE;
}

static void stream_get_dest(display_stream *st)
{
    if (st->msg_data == NULL ||
        spice_msg_in_type(st->msg_data) != SPICE_MSG_DISPLAY_STREAM_DATA_SIZED) {
        SpiceMsgDisplayStreamCreate *info = spice_msg_in_parsed(st->msg_create);

        memcpy(&st->dst_rect, &info->dest, sizeof(SpiceRect));
    } else {
        SpiceMsgDisplayStreamDataSized *op = spice_msg_in_parsed(st->msg_data);

        memcpy(&st->dst_rect, &op->dest, sizeof(SpiceRect));
   }

}

static uint32_t stream_get_flags(display_stream *st)
{
    SpiceMsgDisplayStreamCreate *info = spice_msg_in_parsed(st->msg_create);

    return info->flags;
}

G_GNUC_INTERNAL
uint32_t stream_get_current_frame(display_stream *st, uint8_t **data)
{
    if (st->msg_data == NULL) {
        *data = NULL;
        return 0;
    }

    if (spice_msg_in_type(st->msg_data) == SPICE_MSG_DISPLAY_STREAM_DATA) {
        SpiceMsgDisplayStreamData *op = spice_msg_in_parsed(st->msg_data);

        *data = op->data;
        return op->data_size;
    } else {
        SpiceMsgDisplayStreamDataSized *op = spice_msg_in_parsed(st->msg_data);

        g_return_val_if_fail(spice_msg_in_type(st->msg_data) ==
                             SPICE_MSG_DISPLAY_STREAM_DATA_SIZED, 0);
        *data = op->data;
        return op->data_size;
   }

}

G_GNUC_INTERNAL
void stream_get_dimensions(display_stream *st, int *width, int *height)
{
    g_return_if_fail(width != NULL);
    g_return_if_fail(height != NULL);

    if (st->msg_data == NULL ||
        spice_msg_in_type(st->msg_data) != SPICE_MSG_DISPLAY_STREAM_DATA_SIZED) {
        SpiceMsgDisplayStreamCreate *info = spice_msg_in_parsed(st->msg_create);

        *width = info->stream_width;
        *height = info->stream_height;
    } else {
        SpiceMsgDisplayStreamDataSized *op = spice_msg_in_parsed(st->msg_data);

        *width = op->width;
        *height = op->height;
   }
}

/* main context */
static gboolean display_stream_render(display_stream *st)
{
    SpiceMsgIn *in;
    guint64 time1, time2;
    guint64 delta;

    st->timeout = 0;
    do {
        in = g_queue_pop_head(st->msgq);
        g_return_val_if_fail(in != NULL, FALSE);

        if (st->fskip_frame == 0) {
            time1 = g_get_monotonic_time();

            SpiceRect last_frame_dest;
            memcpy(&last_frame_dest, &st->dst_rect, sizeof(SpiceRect));
            st->msg_data = in;
            stream_get_dest(st);
            rect_union(&last_frame_dest, &st->dst_rect);

            switch (st->codec) {
            case SPICE_VIDEO_CODEC_TYPE_MJPEG:
                stream_mjpeg_data(st);
                break;
            }

            SpiceRect *dest = &st->dst_rect;
            if (st->out_frame) {
                int width;
                int height;
                uint8_t *data;
                int stride;

                stream_get_dimensions(st, &width, &height);

                data = st->out_frame;
                stride = width * sizeof(uint32_t);
                if (!(stream_get_flags(st) & SPICE_STREAM_FLAGS_TOP_DOWN)) {
                    data += stride * (height - 1);
                    stride = -stride;
                }

                st->surface->canvas->ops->put_image(
                    st->surface->canvas,
#ifdef G_OS_WIN32
                SPICE_DISPLAY_CHANNEL(st->channel)->priv->dc,
#endif
                    dest, data,
                    width, height, stride,
                    st->have_region ? &st->region : NULL);
            }

            if (st->hw_accel)
                dest = &last_frame_dest;
            if (st->surface->primary)
                g_signal_emit(st->channel, signals[SPICE_DISPLAY_INVALIDATE], 0,
                    dest->left, dest->top,
                    dest->right - dest->left,
                    dest->bottom - dest->top);

            time2 = g_get_monotonic_time();
            delta = (time2 - time1) / 1000;
            st->acum_decode_time += delta;
            st->decoded_frames++;
            uint8_t new_fskip_level = 0;
            if (delta > 120) {
                new_fskip_level = 3;
            } else if (delta > 80) {
                new_fskip_level = 2;
            } else if (delta > 40) {
                new_fskip_level = 1;
            }
            if (st->fskip_level != new_fskip_level) {
                SPICE_DEBUG("FSkip level: %u - MJPEG process time: %u ms\n", new_fskip_level,
                            (unsigned int) delta);
                st->fskip_level = new_fskip_level;
            }
            st->fskip_frame = st->fskip_level;
        } else {
            --st->fskip_frame;
        }

        st->msg_data = NULL;
        spice_msg_in_unref(in);

        in = g_queue_peek_head(st->msgq);
        if (in == NULL)
            break;

        if (display_stream_schedule(st))
            return FALSE;
    } while (1);

    return FALSE;
}
/* after a sequence of 3 drops, push a report to the server, even
 * if the report window is bigger */
#define STREAM_REPORT_DROP_SEQ_LEN_LIMIT 3

static void display_update_stream_report(SpiceDisplayChannel *channel, uint32_t stream_id,
                                         uint32_t frame_time, int32_t latency, gboolean invalid_mm_time)
{
    display_stream *st = channel->priv->streams[stream_id];
    guint64 now;

    if (!st->report_is_active) {
        return;
    }
    now = g_get_monotonic_time();

    if (st->report_num_frames == 0) {
        st->report_start_frame_time = frame_time;
        st->report_start_time = now;
    }
    st->report_num_frames++;

    if (latency < 0 && !invalid_mm_time) { // drop
        st->report_num_drops++;
        st->report_drops_seq_len++;
    } else {
        st->report_drops_seq_len = 0;
    }

    if (st->report_num_frames >= st->report_max_window ||
        now - st->report_start_time >= st->report_timeout ||
        st->report_drops_seq_len >= STREAM_REPORT_DROP_SEQ_LEN_LIMIT) {
        SpiceMsgcDisplayStreamReport report;
        SpiceSession *session = spice_channel_get_session(SPICE_CHANNEL(channel));
        SpiceMsgOut *msg;

        report.stream_id = stream_id;
        report.unique_id = st->report_id;
        report.start_frame_mm_time = st->report_start_frame_time;
        report.end_frame_mm_time = frame_time;
        report.num_frames = st->report_num_frames;
        report.num_drops = st-> report_num_drops;
        report.last_frame_delay = latency;
        if (spice_session_is_playback_active(session)) {
            report.audio_delay = spice_session_get_playback_latency(session);
        } else {
            report.audio_delay = UINT_MAX;
        }

        unsigned int dec_time = st->decoded_frames ?
                                st->acum_decode_time / st->decoded_frames :
                                0;
        st->decoded_frames = st->acum_decode_time = 0;
        guint64 elapsed = now - st->report_start_time;
        int in_fps = st->report_num_frames * 1000000 / elapsed;
        int out_fps = (st->report_num_frames - st-> report_num_drops) * 1000000 / elapsed;
        g_free(channel->priv->report);
        char *str = g_strdup_printf(
            "HW accel %s, decode: %-3ums, dropped %-2d, in rate %-2d, out rate %-2d",
            st->hw_accel ? "on" : "off",
            dec_time,
            st->report_num_drops,
            in_fps, out_fps);
        channel->priv->report = str;
        SPICE_DEBUG("Reporting stream %d, elapsed %u: %s", stream_id, (unsigned int)elapsed, str);
        g_coroutine_object_notify(G_OBJECT(channel), "stream-report");

        msg = spice_msg_out_new(SPICE_CHANNEL(channel), SPICE_MSGC_DISPLAY_STREAM_REPORT);
        msg->marshallers->msgc_display_stream_report(msg->marshaller, &report);
        spice_msg_out_send(msg);

        st->report_start_time = 0;
        st->report_start_frame_time = 0;
        st->report_num_frames = 0;
        st->report_num_drops = 0;
        st->report_drops_seq_len = 0;
    }
}

static void display_stream_reset_rendering_timer(display_stream *st)
{
    SPICE_DEBUG("%s", __FUNCTION__);
    if (st->timeout != 0) {
        g_source_remove(st->timeout);
        st->timeout = 0;
    }
    while (!display_stream_schedule(st)) {
    }
}

/*
 * Migration can occur between 2 spice-servers with different mm-times.
 * Then, the following cases can happen after migration completes:
 * (We refer to src/dst-time as the mm-times on the src/dst servers):
 *
 * (case 1) Frames with time ~= dst-time arrive to the client before the
 *          playback-channel updates the session's mm-time (i.e., the mm_time
 *          of the session is still based on the src-time).
 *     (a) If src-time < dst-time:
 *         display_stream_schedule schedules the next rendering to
 *         ~(dst-time - src-time) milliseconds from now.
 *         Since we assume monotonic mm_time, display_stream_schedule,
 *         returns immediately when a rendering timeout
 *         has already been set, and doesn't update the timeout,
 *         even after the mm_time is updated.
 *         When src-time << dst-time, a significant video frames loss will occur.
 *     (b) If src-time > dst-time
 *         Frames will be dropped till the mm-time will be updated.
 * (case 2) mm-time is synced with dst-time, but frames that were in the command
 *         ring during migration still arrive (such frames hold src-time).
 *    (a) If src-time < dst-time
 *        The frames that hold src-time will be dropped, since their
 *        mm_time < session-mm_time. But all the new frames that are generated in
 *        the driver after migration, will be rendered appropriately.
 *    (b) If src-time > dst-time
 *        Similar consequences as in 1 (a)
 * case 2 is less likely, since at takes at least 20 frames till the dst-server re-identifies
 * the video stream and starts sending stream data
 *
 * display_session_mm_time_reset_cb handles case 1.a, and
 * display_stream_test_frames_mm_time_reset handles case 2.b
 */

/* main context */
static void display_session_mm_time_reset_cb(SpiceSession *session, gpointer data)
{
    SpiceChannel *channel = data;
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    guint i;

    CHANNEL_DEBUG(channel, "%s", __FUNCTION__);

    for (i = 0; i < c->nstreams; i++) {
        display_stream *st;

        if (c->streams[i] == NULL) {
            continue;
        }
        SPICE_DEBUG("%s: stream-id %d", __FUNCTION__, i);
        st = c->streams[i];
        display_stream_reset_rendering_timer(st);
    }
}

/* coroutine context */
static void display_stream_test_frames_mm_time_reset(display_stream *st,
                                                     SpiceMsgIn *new_frame_msg,
                                                     guint32 mm_time)
{
    SpiceStreamDataHeader *tail_op, *new_op;
    SpiceMsgIn *tail_msg;

    SPICE_DEBUG("%s", __FUNCTION__);
    g_return_if_fail(new_frame_msg != NULL);
    tail_msg = g_queue_peek_tail(st->msgq);
    if (!tail_msg) {
        return;
    }
    tail_op = spice_msg_in_parsed(tail_msg);
    new_op = spice_msg_in_parsed(new_frame_msg);

    if (new_op->multi_media_time < tail_op->multi_media_time) {
        SPICE_DEBUG("new-frame-time < tail-frame-time (%u < %u):"
                    " reseting stream, id %d",
                    new_op->multi_media_time,
                    tail_op->multi_media_time,
                    new_op->id);
        g_queue_foreach(st->msgq, _msg_in_unref_func, NULL);
        g_queue_clear(st->msgq);
        display_stream_reset_rendering_timer(st);
    }
}

#define STREAM_PLAYBACK_SYNC_DROP_SEQ_LEN_LIMIT 5

/* coroutine context */
static void display_handle_stream_data(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceStreamDataHeader *op = spice_msg_in_parsed(in);
    display_stream *st;
    guint32 mmtime;
    int32_t latency;
    gboolean invalid_mm_time;

    g_return_if_fail(c != NULL);
    g_return_if_fail(c->streams != NULL);
    g_return_if_fail(c->nstreams > op->id);

    st =  c->streams[op->id];
    mmtime = spice_session_get_mm_time(spice_channel_get_session(channel), &invalid_mm_time);

    if (spice_msg_in_type(in) == SPICE_MSG_DISPLAY_STREAM_DATA_SIZED) {
        CHANNEL_DEBUG(channel, "stream %d contains sized data", op->id);
    }

    if (op->multi_media_time == 0) {
        g_critical("Received frame with invalid 0 timestamp! perhaps wrong graphic driver?");
        op->multi_media_time = mmtime + 100; /* workaround... */
    }

    if (!st->num_input_frames) {
        st->first_frame_mm_time = op->multi_media_time;
    }
    st->num_input_frames++;

    latency = op->multi_media_time - mmtime;
    if (!invalid_mm_time && latency < 0) {
        CHANNEL_DEBUG(channel, "stream data too late by %u ms (ts: %u, mmtime: %u), dropping",
                      mmtime - op->multi_media_time, op->multi_media_time, mmtime);
        st->arrive_late_time += mmtime - op->multi_media_time;
        st->num_drops_on_receive++;

        if (!st->cur_drops_seq_stats.len) {
            st->cur_drops_seq_stats.start_mm_time = op->multi_media_time;
        }
        st->cur_drops_seq_stats.len++;
        st->playback_sync_drops_seq_len++;
    } else {
        if (invalid_mm_time) {
            CHANNEL_DEBUG(channel, "Invalid mm_time. Not checking video-audio sync");
        } else {
            CHANNEL_DEBUG(channel, "video latency: %d", latency );
        }
        spice_msg_in_ref(in);
        display_stream_test_frames_mm_time_reset(st, in, mmtime);
        g_queue_push_tail(st->msgq, in);
        while (!display_stream_schedule(st)) {
        }
        if (st->cur_drops_seq_stats.len) {
            st->cur_drops_seq_stats.duration = op->multi_media_time -
                                               st->cur_drops_seq_stats.start_mm_time;
            g_array_append_val(st->drops_seqs_stats_arr, st->cur_drops_seq_stats);
            memset(&st->cur_drops_seq_stats, 0, sizeof(st->cur_drops_seq_stats));
            st->num_drops_seqs++;
        }
        st->playback_sync_drops_seq_len = 0;
    }
    if (c->enable_adaptive_streaming) {
        display_update_stream_report(SPICE_DISPLAY_CHANNEL(channel), op->id,
                                     op->multi_media_time, latency, invalid_mm_time);
        if (st->playback_sync_drops_seq_len >= STREAM_PLAYBACK_SYNC_DROP_SEQ_LEN_LIMIT) {
            spice_session_sync_playback_latency(spice_channel_get_session(channel));
            st->playback_sync_drops_seq_len = 0;
        }
    }
}

/* coroutine context */
static void display_handle_stream_clip(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayStreamClip *op = spice_msg_in_parsed(in);
    display_stream *st;

    g_return_if_fail(c != NULL);
    g_return_if_fail(c->streams != NULL);
    g_return_if_fail(c->nstreams > op->id);

    st = c->streams[op->id];

    if (st->msg_clip) {
        spice_msg_in_unref(st->msg_clip);
    }
    spice_msg_in_ref(in);
    st->msg_clip = in;
    st->clip = &op->clip;
    display_update_stream_region(st);
}

static void _msg_in_unref_func(gpointer data, gpointer user_data)
{
    spice_msg_in_unref(data);
}

static void destroy_stream(SpiceChannel *channel, int id)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    display_stream *st;
    guint64 drops_duration_total = 0;
    guint32 num_out_frames;
    int i;

    g_return_if_fail(c != NULL);
    g_return_if_fail(c->streams != NULL);
    g_return_if_fail(c->nstreams > id);

    st = c->streams[id];
    if (!st)
        return;

    num_out_frames = st->num_input_frames - st->num_drops_on_receive - st->num_drops_on_playback;
    CHANNEL_DEBUG(channel, "%s: id=%d #in-frames=%d out/in=%.2f "
        "#drops-on-receive=%d avg-late-time(ms)=%.2f "
        "#drops-on-playback=%d", __FUNCTION__,
        id,
        st->num_input_frames,
        num_out_frames / (double)st->num_input_frames,
        st->num_drops_on_receive,
        st->num_drops_on_receive ? st->arrive_late_time / ((double)st->num_drops_on_receive): 0,
        st->num_drops_on_playback);
    if (st->num_drops_seqs) {
        CHANNEL_DEBUG(channel, "%s: #drops-sequences=%u ==>", __FUNCTION__, st->num_drops_seqs);
    }
    for (i = 0; i < st->num_drops_seqs; i++) {
            drops_sequence_stats *stats = &g_array_index(st->drops_seqs_stats_arr,
                                                         drops_sequence_stats,
                                                         i);
            drops_duration_total += stats->duration;
            CHANNEL_DEBUG(channel, "%s: \t len=%u start-ms=%u duration-ms=%u", __FUNCTION__,
                                   stats->len,
                                   stats->start_mm_time - st->first_frame_mm_time,
                                   stats->duration);
    }
    if (st->num_drops_seqs) {
        CHANNEL_DEBUG(channel, "%s: drops-total-duration=%"G_GUINT64_FORMAT" ==>", __FUNCTION__, drops_duration_total);
    }

    g_array_free(st->drops_seqs_stats_arr, TRUE);

    switch (st->codec) {
    case SPICE_VIDEO_CODEC_TYPE_MJPEG:
        stream_mjpeg_cleanup(st);
        break;
    }

    // If HW decode, force repaint of last frame's area
    if (st->hw_accel && st->surface->primary)
        g_signal_emit(st->channel, signals[SPICE_DISPLAY_INVALIDATE], 0,
            st->dst_rect.left, st->dst_rect.top,
            st->dst_rect.right - st->dst_rect.left,
            st->dst_rect.bottom - st->dst_rect.top);

    if (st->msg_clip)
        spice_msg_in_unref(st->msg_clip);
    spice_msg_in_unref(st->msg_create);

    g_queue_foreach(st->msgq, _msg_in_unref_func, NULL);
    g_queue_free(st->msgq);
    if (st->timeout != 0)
        g_source_remove(st->timeout);
    g_free(st);
    c->streams[id] = NULL;

    g_free(c->report);
    c->report = g_strdup("");
    g_coroutine_object_notify(G_OBJECT(channel), "stream-report");
}

static void clear_streams(SpiceChannel *channel)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    int i;

    for (i = 0; i < c->nstreams; i++) {
        destroy_stream(channel, i);
    }
    g_free(c->streams);
    c->streams = NULL;
    c->nstreams = 0;
}

/* coroutine context */
static void display_handle_stream_destroy(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayStreamDestroy *op = spice_msg_in_parsed(in);

    g_return_if_fail(op != NULL);
    CHANNEL_DEBUG(channel, "%s: id %d", __FUNCTION__, op->id);
    destroy_stream(channel, op->id);
}

/* coroutine context */
static void display_handle_stream_destroy_all(SpiceChannel *channel, SpiceMsgIn *in)
{
    clear_streams(channel);
}

/* coroutine context */
static void display_handle_stream_activate_report(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayStreamActivateReport *op = spice_msg_in_parsed(in);
    display_stream *st;

    g_return_if_fail(c != NULL);
    g_return_if_fail(c->streams != NULL);
    g_return_if_fail(c->nstreams > op->stream_id);

    st = c->streams[op->stream_id];
    g_return_if_fail(st != NULL);

    st->report_is_active = TRUE;
    st->report_id = op->unique_id;
    st->report_max_window = op->max_window_size;
    st->report_timeout = op->timeout_ms * 1000;
    st->report_start_time = 0;
    st->report_start_frame_time = 0;
    st->report_num_frames = 0;
    st->report_num_drops = 0;
    st->report_drops_seq_len = 0;
}

/* ------------------------------------------------------------------ */

/* coroutine context */
static void display_handle_draw_fill(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawFill *op = spice_msg_in_parsed(in);
    DRAW(fill);
}

/* coroutine context */
static void display_handle_draw_opaque(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawOpaque *op = spice_msg_in_parsed(in);
    DRAW(opaque);
}

/* coroutine context */
static void display_handle_draw_copy(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawCopy *op = spice_msg_in_parsed(in);
    DRAW(copy);
}

/* coroutine context */
static void display_handle_draw_blend(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawBlend *op = spice_msg_in_parsed(in);
    DRAW(blend);
}

/* coroutine context */
static void display_handle_draw_blackness(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawBlackness *op = spice_msg_in_parsed(in);
    DRAW(blackness);
}

static void display_handle_draw_whiteness(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawWhiteness *op = spice_msg_in_parsed(in);
    DRAW(whiteness);
}

/* coroutine context */
static void display_handle_draw_invers(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawInvers *op = spice_msg_in_parsed(in);
    DRAW(invers);
}

/* coroutine context */
static void display_handle_draw_rop3(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawRop3 *op = spice_msg_in_parsed(in);
    DRAW(rop3);
}

/* coroutine context */
static void display_handle_draw_stroke(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawStroke *op = spice_msg_in_parsed(in);
    DRAW(stroke);
}

/* coroutine context */
static void display_handle_draw_text(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawText *op = spice_msg_in_parsed(in);
    DRAW(text);
}

/* coroutine context */
static void display_handle_draw_transparent(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawTransparent *op = spice_msg_in_parsed(in);
    DRAW(transparent);
}

/* coroutine context */
static void display_handle_draw_alpha_blend(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawAlphaBlend *op = spice_msg_in_parsed(in);
    DRAW(alpha_blend);
}

/* coroutine context */
static void display_handle_draw_composite(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayDrawComposite *op = spice_msg_in_parsed(in);
    DRAW(composite);
}

/* coroutine context */
static void display_handle_surface_create(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgSurfaceCreate *create = spice_msg_in_parsed(in);
    display_surface *surface = g_slice_new0(display_surface);

    surface->surface_id = create->surface_id;
    surface->format = create->format;
    surface->width  = create->width;
    surface->height = create->height;
    surface->stride = create->width * 4;
    surface->size   = surface->height * surface->stride;

    if (create->flags & SPICE_SURFACE_FLAGS_PRIMARY) {
        SPICE_DEBUG("primary flags: %d", create->flags);
        surface->primary = true;
        create_canvas(channel, surface);
        if (c->mark_false_event_id != 0) {
            g_source_remove(c->mark_false_event_id);
            c->mark_false_event_id = FALSE;
        }
    } else {
        surface->primary = false;
        create_canvas(channel, surface);
    }
}

static gboolean display_mark_false(gpointer data)
{
    SpiceChannel *channel = data;
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;

    c->mark = FALSE;
    g_signal_emit(channel, signals[SPICE_DISPLAY_MARK], 0, FALSE);

    c->mark_false_event_id = 0;
    return FALSE;
}

/* coroutine context */
static void display_handle_surface_destroy(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgSurfaceDestroy *destroy = spice_msg_in_parsed(in);
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    display_surface *surface;

    g_return_if_fail(destroy != NULL);

    surface = find_surface(c, destroy->surface_id);
    if (surface == NULL) {
        /* this is not a problem in spicec, it happens as well and returns.. */
        /* g_warn_if_reached(); */
        return;
    }
    if (surface->primary) {
        int id = spice_channel_get_channel_id(channel);
        CHANNEL_DEBUG(channel, "%d: FIXME primary destroy, but is display really disabled?", id);
        /* this is done with a timeout in spicec as well, it's *ugly* */
        if (id != 0 && c->mark_false_event_id == 0) {
            c->mark_false_event_id = g_timeout_add_seconds(1, display_mark_false, channel);
        }
        c->primary = NULL;
        g_coroutine_signal_emit(channel, signals[SPICE_DISPLAY_PRIMARY_DESTROY], 0);
    }

    g_hash_table_remove(c->surfaces, GINT_TO_POINTER(surface->surface_id));
}

#define CLAMP_CHECK(x, low, high)  (((x) > (high)) ? TRUE : (((x) < (low)) ? TRUE : FALSE))

/* coroutine context */
static void display_handle_monitors_config(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceMsgDisplayMonitorsConfig *config = spice_msg_in_parsed(in);
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    guint i;

    g_return_if_fail(config != NULL);
    g_return_if_fail(config->count > 0);

    CHANNEL_DEBUG(channel, "received new monitors config from guest: n: %d/%d", config->count, config->max_allowed);

    c->monitors_max = config->max_allowed;
    if (CLAMP_CHECK(c->monitors_max, 1, MONITORS_MAX)) {
        g_warning("MonitorConfig max_allowed is not within permitted range, clamping");
        c->monitors_max = CLAMP(c->monitors_max, 1, MONITORS_MAX);
    }

    if (CLAMP_CHECK(config->count, 1, c->monitors_max)) {
        g_warning("MonitorConfig count is not within permitted range, clamping");
        config->count = CLAMP(config->count, 1, c->monitors_max);
    }

    c->monitors = g_array_set_size(c->monitors, config->count);

    for (i = 0; i < config->count; i++) {
        SpiceDisplayMonitorConfig *mc = &g_array_index(c->monitors, SpiceDisplayMonitorConfig, i);
        SpiceHead *head = &config->heads[i];
        CHANNEL_DEBUG(channel, "monitor id: %u, surface id: %u, +%u+%u-%ux%u",
                    head->id, head->surface_id,
                    head->x, head->y, head->width, head->height);
        mc->id = head->id;
        mc->surface_id = head->surface_id;
        mc->x = head->x;
        mc->y = head->y;
        mc->width = head->width;
        mc->height = head->height;
    }

    g_coroutine_object_notify(G_OBJECT(channel), "monitors");
}

static void channel_set_handlers(SpiceChannelClass *klass)
{
    static const spice_msg_handler handlers[] = {
        [ SPICE_MSG_DISPLAY_MODE ]               = display_handle_mode,
        [ SPICE_MSG_DISPLAY_MARK ]               = display_handle_mark,
        [ SPICE_MSG_DISPLAY_RESET ]              = display_handle_reset,
        [ SPICE_MSG_DISPLAY_COPY_BITS ]          = display_handle_copy_bits,
        [ SPICE_MSG_DISPLAY_INVAL_LIST ]         = display_handle_inv_list,
        [ SPICE_MSG_DISPLAY_INVAL_ALL_PIXMAPS ]  = display_handle_inv_pixmap_all,
        [ SPICE_MSG_DISPLAY_INVAL_PALETTE ]      = display_handle_inv_palette,
        [ SPICE_MSG_DISPLAY_INVAL_ALL_PALETTES ] = display_handle_inv_palette_all,

        [ SPICE_MSG_DISPLAY_STREAM_CREATE ]      = display_handle_stream_create,
        [ SPICE_MSG_DISPLAY_STREAM_DATA ]        = display_handle_stream_data,
        [ SPICE_MSG_DISPLAY_STREAM_CLIP ]        = display_handle_stream_clip,
        [ SPICE_MSG_DISPLAY_STREAM_DESTROY ]     = display_handle_stream_destroy,
        [ SPICE_MSG_DISPLAY_STREAM_DESTROY_ALL ] = display_handle_stream_destroy_all,
        [ SPICE_MSG_DISPLAY_STREAM_DATA_SIZED ]  = display_handle_stream_data,
        [ SPICE_MSG_DISPLAY_STREAM_ACTIVATE_REPORT ] = display_handle_stream_activate_report,

        [ SPICE_MSG_DISPLAY_DRAW_FILL ]          = display_handle_draw_fill,
        [ SPICE_MSG_DISPLAY_DRAW_OPAQUE ]        = display_handle_draw_opaque,
        [ SPICE_MSG_DISPLAY_DRAW_COPY ]          = display_handle_draw_copy,
        [ SPICE_MSG_DISPLAY_DRAW_BLEND ]         = display_handle_draw_blend,
        [ SPICE_MSG_DISPLAY_DRAW_BLACKNESS ]     = display_handle_draw_blackness,
        [ SPICE_MSG_DISPLAY_DRAW_WHITENESS ]     = display_handle_draw_whiteness,
        [ SPICE_MSG_DISPLAY_DRAW_INVERS ]        = display_handle_draw_invers,
        [ SPICE_MSG_DISPLAY_DRAW_ROP3 ]          = display_handle_draw_rop3,
        [ SPICE_MSG_DISPLAY_DRAW_STROKE ]        = display_handle_draw_stroke,
        [ SPICE_MSG_DISPLAY_DRAW_TEXT ]          = display_handle_draw_text,
        [ SPICE_MSG_DISPLAY_DRAW_TRANSPARENT ]   = display_handle_draw_transparent,
        [ SPICE_MSG_DISPLAY_DRAW_ALPHA_BLEND ]   = display_handle_draw_alpha_blend,
        [ SPICE_MSG_DISPLAY_DRAW_COMPOSITE ]     = display_handle_draw_composite,

        [ SPICE_MSG_DISPLAY_SURFACE_CREATE ]     = display_handle_surface_create,
        [ SPICE_MSG_DISPLAY_SURFACE_DESTROY ]    = display_handle_surface_destroy,

        [ SPICE_MSG_DISPLAY_MONITORS_CONFIG ]    = display_handle_monitors_config,
#ifdef FUSIONDATA_DEV
        /* add by yhoon17, 20180329 : 동영상 가속기 */
        [ SPICE_MSGC_DISPLAY_StreamConfig ]      = DisplayHandleStreamConfig,
        [ SPICE_MSGC_DISPLAY_StreamFrameData ]   = DisplayHandleStreamFrameData,
#endif
    };
    

    spice_channel_set_handlers(klass, handlers, G_N_ELEMENTS(handlers));
    
}


#ifdef FUSIONDATA_DEV
/**************************************************
 * FUSIONDATA 에서 추가 개발한 Functions
 **************************************************/

/*
 * Description : JClient에서 생성하는 spice_stream.config file read
 * Date        : 20180329
 */
int ReadSpiceStreamConfig(void)
{

    int i = 0, j = 0;
    int nFlag = FALSE;
    int *pnSetValue = NULL;
    char szBuf[ 1024 ];
    char szValue[ 1024 ];
    char szData[ 4096 ];
    FILE *pstFp = NULL;


    /* default config value */
    _nStreamPort = 0;
    _nStreamOnMovieDetection = 0;
    _nOnVariationCapture = 1;
    _nMaxSamplingFps = 30;
    _nAudioSyncNot = 0;

    memset(szBuf, 0x00, sizeof(szBuf));

    GetOptFilePath("spice_stream.config", szBuf);

    g_message("%s, szBuf = %s", __FUNCTION__, szBuf);

    while ( (pstFp = fopen(szBuf, "r")) == NULL )
    {
        if ( (pstFp = fopen(szBuf, "w")) == NULL )
        {
            g_printerr("fopen() failed");
            return -1;
        }
        else
        {
            memset(szData, 0x00, sizeof(szData));

            sprintf(szData,
                "# If value is <0, original spice is processed.\n"
                "# if stream_port=0, combined display channel is used.\n"
                "# if stream_port>1000, use stream_port as binding port for stream connection\n"
                "# else stream_port+spice_(tls)port is used as binding port for stream connection\n"
                "stream_port=%d\n"
                "# The following parameter set whether auto turning on stream_encoding only when video is detected\n"
                "# movie is to be detected when user starts to play movie like youtube video\n"
                "stream_onMovieDetection=%d\n"
                "# if 0, static capture at max_samplig_fps, else caputre only when screen variation occurs\n"
                "# this can reduce encoding cpu overhead\n"
                "on_variation_capture=%d\n"
                "# max stream sampling fps is set, which is used to reduce server cpu load at the cost of QoE\n"
                "max_sampling_fps=%d\n"
                "# audio_sync_not is set, which is used to sync video with audio by delaying video frame display\n"
                "# if audio_sync_not is 1, do not try to sync video to audio, which enhance delay performance(i.e,low delay)\n"
                "audio_sync_not=%d\n",
                _nStreamPort, _nStreamOnMovieDetection, _nOnVariationCapture, _nMaxSamplingFps, _nAudioSyncNot);

            fwrite(szData, 1, strlen(szData), pstFp);

            fclose(pstFp);
        }
    }

    g_message("read option values in spice_stream.config");

    while ( !(feof(pstFp)) )
    {
        i = 0, nFlag = FALSE;

        memset(szBuf, 0x00, sizeof(szBuf));

        fgets(szBuf, sizeof(szBuf), pstFp);

        TrimSpace(szBuf);

        if (strncmp(szBuf, "stream_port", strlen("stream_port")) == 0)
        {
            nFlag = TRUE;
            pnSetValue = &_nStreamPort;
            i = strlen("stream_port");
        }
        else if (strncmp(szBuf, "stream_onMovieDetection", strlen("stream_onMovieDetection")) == 0)
        {
            nFlag = TRUE;
            pnSetValue = &_nStreamOnMovieDetection;
            i = strlen("stream_onMovieDetection");
        }
        else if (strncmp(szBuf, "on_variation_capture", strlen("on_variation_capture")) == 0)
        {
            nFlag = TRUE;
            pnSetValue = &_nOnVariationCapture;
            i = strlen("on_variation_capture");
        }
        else if (strncmp(szBuf, "max_sampling_fps", strlen("max_sampling_fps")) == 0)
        {
            nFlag = TRUE;
            pnSetValue = &_nMaxSamplingFps;
            i = strlen("max_sampling_fps");
        }
        else if (strncmp(szBuf, "audio_sync_not", strlen("audio_sync_not")) == 0)
        {
            nFlag = TRUE;
            pnSetValue = &_nAudioSyncNot;
            i = strlen("audio_sync_not");
        }

        if (nFlag == TRUE)
        {
            memset(szValue, 0x00, sizeof(szValue));

            for ( ; szBuf[i]; i++)
            {
                if ( !(szBuf[i] == ' ' || szBuf[i] == '\t' || szBuf[i] == '=') )
                {
                    break;
                }
            }

            for (j = 0; szBuf[i]; i++)
            {
                if (szBuf[i] == ' ' || szBuf[i] == '\t' || szBuf[i] == '\n')
                {
                    break;
                }
                else
                {
                    szValue[j++] = szBuf[i];
                }
            }

            szValue[j] = '\0';

            *pnSetValue = atoi(szValue);
        }
    }

    g_message("option values in spice_stream.config");
    g_message("stream_port             : [ %d ]", _nStreamPort);
    g_message("stream_onMovieDetection : [ %d ]", _nStreamOnMovieDetection);
    g_message("on_variation_capture    : [ %d ]", _nOnVariationCapture);
    g_message("max_sampling_fps        : [ %d ]", _nMaxSamplingFps);
    g_message("audio_sync_not          : [ %d ]", _nAudioSyncNot);

    fclose(pstFp);

    return 0;
}


/*
 * Description : SPC/bin/opt에 있는 file full path 리턴
 * Date        : 20180329
 */
int GetOptFilePath(const gchar *pszName, char *pszPath)
{
    gboolean nSuccess = FALSE;
    gchar *pszFullName = NULL;
    gchar *pszBaseName = NULL;
    gchar *pszStr = NULL;


#ifdef G_OS_WIN32
    pszFullName = g_path_get_dirname( g_find_program_in_path( g_get_prgname() ) );
#else
    char szArBuf[ 256 ];

    memset(szArBuf, 0x00, sizeof(szArBuf));

    readlink("/proc/self/exe", szArBuf, 256);

    pszFullName = g_path_get_dirname(szArBuf);
#endif

    nSuccess = g_path_is_absolute(pszFullName);

    if (nSuccess)
    {
        pszBaseName = pszFullName;
    }
    else
    {
        pszBaseName = g_get_current_dir();
    }

    pszStr = g_build_filename(pszBaseName, "opt", pszName, NULL);

    strncpy(pszPath, pszStr, strlen(pszStr));

    g_free(pszStr);

    return 0;
}


/*
 * Description : 공백 제거
 * Date        : 20180329
 */
int TrimSpace(char *pszStr)
{
    TrimLeftSpace(pszStr);
    TrimRightSpace(pszStr);

    return 0;
}


/*
 * Description : 왼쪽 공백 제거
 * Date        : 20180329
 */
int TrimLeftSpace(char *pszStr)
{
    int i = 0;
    char szTemp[ 1024 ];


    for (i = 0; pszStr[i]; i++)
    {
        if ( !(isspace( (int) pszStr[i] )) )
        {
            break;
        }
    }

    if (i && pszStr[i])
    {
        memset(szTemp, 0x00, sizeof(szTemp));

        strcpy(szTemp, pszStr + i);

        memset(pszStr, 0x00, sizeof(1024));

        strcpy(pszStr, szTemp);
    }

    return 0;
}


/*
 * Description : 오른쪽 공백 제거
 * Date        : 20180329
 */
int TrimRightSpace(char *pszStr)
{
    int i = 0;
    int nLength = strlen(pszStr);


    for (i = (nLength - 1); i >= 0; i--)
    {
        if (isspace( (int) pszStr[i] ))
        {
            pszStr[i] = '\0';
        }
        else
        {
            break;
        }
    }

    return 0;
}


/*
 * Description : Stream Config Message 처리
 * Date        : 20180329
 */
static void DisplayHandleStreamConfig(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayStreamConfig *op = spice_msg_in_parsed(in);


    g_message("server altinative stream info");
    g_message("stream_port             : [ %d ]", op->nStreamPort);
    g_message("stream_onMovieDetection : [ %u ]", op->unStreamOnMovieDetection);
    g_message("on_variation_capture    : [ %u ]", op->unOnVariationCapture);
    g_message("max_sampling_fps        : [ %u ]", op->unMaxSamplingFps);

    c->stStreamRenderCtx.nMajorProtocol = op->unStreamOnMovieDetection;
    c->stStreamRenderCtx.nMinorProtocol = op->unOnVariationCapture;

    SetStreamRenderConfig(channel);

    if (c->stStreamRenderCtx.nStreamPort == 0)
    {
        c->stStreamRenderCtx.nUse = 1;

        DisplayChannelSendMsgStreamConfig(channel,
            c->stStreamRenderCtx.nStreamPort, c->stStreamRenderCtx.nStreamOnMovieDetection,
            c->stStreamRenderCtx.nOnVariationCapture, c->stStreamRenderCtx.nMaxSamplingFps);
    }
}


/*
 * Description : Stream Frame Data Message 처리
 * Date        : 20180329
 */
static void DisplayHandleStreamFrameData(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayStreamFrameData *op = spice_msg_in_parsed(in);
    int q_size = 0;
    int latency_q_size = 0;


    if (op->unMultiMediaTime == 0 || c->stStreamRenderCtx.nAudioSyncNot == 1)
    {
        SendStreamFrameDataToDecoder(channel, in);
        return;
    }

    if (c->stStreamRenderCtx.unSequenceNum != 0 && op->unId != (++c->stStreamRenderCtx.unSequenceNum))
    {
        ResetStreamFrameSchedule(channel);
    }

    if (c->stStreamRenderCtx.unSequenceNum == 0)
    {
        c->stStreamRenderCtx.unSequenceNum = op->unId;
    }

    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    q_size = g_queue_get_length(c->stStreamRenderCtx.pstMsgQueue);

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

    latency_q_size = g_queue_get_length(c->stStreamRenderCtx.pstLatencyQueue);

    if (q_size + latency_q_size >= 7)
    {
        if (c->stStreamRenderCtx.nMajorProtocol >= 2)
        {
            if (g_get_monotonic_time() - c->stStreamRenderCtx.ulStatTimeStamp > 1000 * 1000)
            {
                DisplayChannelSendMsgStreamStatData(channel, DisplayChannelGetDecodingQueueSize(channel));
            }
        }

        g_message("stream frame data qsize = %d, latency msg q-size = %d", q_size, latency_q_size);
    }

    spice_msg_in_ref(in);

    g_queue_push_tail(c->stStreamRenderCtx.pstLatencyQueue, in);

    StreamFrameSchedule(channel);
}


/*
 * Description : Stream Config Message 설정
 * Date        : 20180329
 */
SPICE_GNUC_UNUSED static void DisplayChannelSendMsgStreamConfig(SpiceChannel *channel, uint32_t unStreamPort, uint32_t unStreamOnMovieDetection, uint32_t unOnVariationCapture, uint32_t unMaxSamplingFps)
{
    SPICE_GNUC_UNUSED SpiceSession *session = spice_channel_get_session(channel);
    SpiceMsgDisplayStreamConfig stStreamConfig;
    SpiceMsgOut *msg;


    stStreamConfig.nStreamPort = unStreamPort;
    stStreamConfig.unStreamOnMovieDetection = unStreamOnMovieDetection;
    stStreamConfig.unOnVariationCapture = unOnVariationCapture;
    stStreamConfig.unMaxSamplingFps = unMaxSamplingFps;
    

    msg = spice_msg_out_new(channel, SPICE_MSGC_DISPLAY_StreamConfig);
    msg->marshallers->msg_display_stream_config(msg->marshaller, &stStreamConfig);
    spice_msg_out_send(msg);
}


/*
 * Description : Stream Stat Data Message 설정
 * Date        : 20180329
 */
SPICE_GNUC_UNUSED static void DisplayChannelSendMsgStreamStatData(SpiceChannel *channel, uint32_t unBacklogSize)
{
    SPICE_GNUC_UNUSED SpiceSession *session = spice_channel_get_session(channel);
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayStreamStatData stStreamStatData;
    SpiceMsgOut *msg;


    stStreamStatData.unStreamBacklogSize = unBacklogSize;

    msg = spice_msg_out_new(channel, SPICE_MSGC_DISPLAY_StreamStatData);
    msg->marshallers->msg_display_stream_stat_data(msg->marshaller, &stStreamStatData);
    spice_msg_out_send(msg);

    c->stStreamRenderCtx.ulStatTimeStamp = g_get_monotonic_time();
}


/*
 * Description : StreamRenderContext init
 * Date        : 20180329
 */
static void StreamRenderContextInit(GObject *object)
{
    SpiceChannel *channel = SPICE_CHANNEL(object);
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    GError *error = NULL;


    c->stStreamRenderCtx.channel = SPICE_CHANNEL(object);
    c->stStreamRenderCtx.pData = 0;

    c->stStreamRenderCtx.pstThread = 0;

    c->stStreamRenderCtx.nStreamPort = 0;
    c->stStreamRenderCtx.nStreamOnMovieDetection = 1;
    c->stStreamRenderCtx.nOnVariationCapture = 1;
    c->stStreamRenderCtx.nMaxSamplingFps = 30;
    c->stStreamRenderCtx.nAudioSyncNot = 0;

    c->stStreamRenderCtx.unSequenceNum = 0;
    c->stStreamRenderCtx.unTimeoutId = 0;

    c->stStreamRenderCtx.nUse = 0;

    if (c->stStreamRenderCtx.pstMsgQueue == NULL)
    {
        c->stStreamRenderCtx.pstMsgQueue = g_queue_new();
        c->stStreamRenderCtx.pstLatencyQueue = g_queue_new();

        g_mutex_init( &c->stStreamRenderCtx.stMutex );
        g_cond_init( &c->stStreamRenderCtx.stCond );
    }

    if (c->stStreamRenderCtx.pstThread == (GThread *) 0)
    {
        c->stStreamRenderCtx.nStatus = STREAM_RENDER_START;

        if ( !(c->stStreamRenderCtx.pstThread = g_thread_create(StreamRenderThread, channel, FALSE, &error)) )
        {
            g_printerr("Error: %s", error->message);
            exit(-1);
        }

        g_message("gtk created stream receiving thread");
    }

    g_message("StreamRenderContext inited");
}


/*
 * Description : StreamRenderContext destructor
 * Date        : 20180329
 */
static void StreamRenderContextDestructor(GObject *object)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(object)->priv;


    JoinStreamRenderThread(object);

    g_message("joined stream thread at destructor");

    ResetStreamFrameSchedule( &SPICE_DISPLAY_CHANNEL(object)->parent );

    if (c->stStreamRenderCtx.pstMsgQueue)
    {
        g_queue_foreach(c->stStreamRenderCtx.pstMsgQueue, MsgInUnrefFuncGpointer, NULL);
        g_queue_free(c->stStreamRenderCtx.pstMsgQueue);
        c->stStreamRenderCtx.pstMsgQueue = NULL;

        g_queue_foreach(c->stStreamRenderCtx.pstLatencyQueue, MsgInUnrefFuncGpointer, NULL);
        g_queue_free(c->stStreamRenderCtx.pstLatencyQueue);
        c->stStreamRenderCtx.pstLatencyQueue = NULL;

        g_mutex_clear( &c->stStreamRenderCtx.stMutex );
        g_cond_clear( &c->stStreamRenderCtx.stCond );
    }

    c->stStreamRenderCtx.pData = 0;
    c->stStreamRenderCtx.channel = NULL;
}


/*
 * Description : StreamRender thread
 * Date        : 20180329
 */
void *StreamRenderThread(void *data)
{
    SpiceChannel *channel = data;
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceRect dest;
    display_surface *surface = NULL;
    ENCODER_HEADER_T *pstHeader = NULL;
    ES_PRIVATE_HEADER_T *pstMp4esHeader;
    AVCodecContext *pstCodecCtx = NULL;
    AVCodec *pstCodec = NULL;
    AVFrame *pstFrame = NULL;
    AVPacket stPacket;
    AVPicture stPicture;
    int nSock = 0;
    int nFrameFinished = 0;
    int nSize = 0;
    int nWidth = 0;
    int nHeight = 0;
    int nHeader = 0;
    int nLen = 0;
    int nFlag = TRUE;
    uint8_t uszRecvHeaderBuf[1000];
    uint8_t *puszRgbData = NULL;
    uint8_t *puszDataFrameBuf = NULL;
    struct SwsContext *pstSwsCtx = NULL;


    /* read stream port config, if needed */
    if(c->stStreamRenderCtx.nUse != 1)
    {
        SetStreamRenderConfig(channel);
    }

    /* set stream port */
    if(c->stStreamRenderCtx.nStreamPort < 0)
    {
        //if <0, then acts as a normal original spice client, don't make encoding thread.
        g_message("original spice mode");
        c->stStreamRenderCtx.pstThread = 0;

        return NULL;
    }
    
    if (c->stStreamRenderCtx.nStreamPort != 0 && !(c->stStreamRenderCtx.nUse) )
    {
        if ( (c->stStreamRenderCtx.nSock = nSock = GetStreamRenderSocket(channel)) < 0)
        {
            g_printerr("error to get stream socket connection");
            g_message("error to get stream socket connection");
            c->stStreamRenderCtx.pstThread = 0;

            return NULL;
        }
        
    }
    else
    {
        g_message("use combined channel for display stream");

        c->stStreamRenderCtx.nSock = -1;

        if (c->stStreamRenderCtx.nStreamPort == 0 && !(c->stStreamRenderCtx.nUse) )
        {
            g_message("use combined stream channel, but not recieved server confirm msg yet. waiting..");
            c->stStreamRenderCtx.nUse = 1;
        }
    }

    avcodec_register_all();
    av_register_all();

    memset(uszRecvHeaderBuf, 0x00, sizeof(uszRecvHeaderBuf));

    puszDataFrameBuf = (uint8_t *) malloc (1920 * 1080 * 4 * 4);

    pstHeader = (ENCODER_HEADER_T *) uszRecvHeaderBuf;

    do {
        if (ProcessPauseFinRequest(channel) < 0)
        {
            c->stStreamRenderCtx.pstThread = 0;
            return 0;
        }

        if (ReadEncodedFrame(channel, (char *) pstHeader, (char *) puszDataFrameBuf) < 0)
        {
            c->stStreamRenderCtx.pstThread = 0;
            c->stStreamRenderCtx.nStatus = STREAM_RENDER_FIN_RSP;

            g_cond_signal( &c->stStreamRenderCtx.stCond );

            return 0;
        }
        pstMp4esHeader = (ES_PRIVATE_HEADER_T *) ((char *) uszRecvHeaderBuf + sizeof(ENCODER_HEADER_T));

        nSize = pstHeader->unDataSize;

        assert(pstHeader->unTotalSize == pstHeader->unHeaderSize + pstHeader->unDataSize);
        assert(pstHeader->unHeaderSize == sizeof(ENCODER_HEADER_T) + pstHeader->unCodecPrivateHeaderSize);

        nHeader = (pstHeader->unCodec == 60000 ? pstMp4esHeader->unMp4esHeaderSize : 0);

        nWidth = pstHeader->nWidth;
        nHeight = pstHeader->nHeight;

        //g_message("do - while in nWidth = %d, nHeight = %d",  nWidth, nHeight);

        if ( (nFlag == TRUE) || (pstHeader->nSequenceNum == 0) || (nHeader != 0) ||
            (nWidth != pstCodecCtx->width) || (nHeight != pstCodecCtx->height) )
        {
            if (nFlag == TRUE)
            {
                nFlag = FALSE;
            }
            else
            {
                av_free_packet( &stPacket );
                av_free(pstFrame);
                avcodec_close(pstCodecCtx);
            }

            av_init_packet( &stPacket );

            /* find the mpeg1 video decoder */
            if ( !(pstCodec = avcodec_find_decoder(pstHeader->unCodec == 60000 ? CODEC_ID_MPEG4 : AV_CODEC_ID_H264)) )
            {
                g_printerr("avcodec_find_decoder() failed.");
                g_message("avcodec_find_decoder() failed.");
                exit(1);
            }

            pstCodecCtx = avcodec_alloc_context3(pstCodec);

            pstFrame = avcodec_alloc_frame();

            if (pstCodec->capabilities & CODEC_CAP_TRUNCATED)
            {
                /* we do not send complete frames */
                pstCodecCtx->flags |= CODEC_FLAG_TRUNCATED;
            }

            if (avcodec_open2(pstCodecCtx, pstCodec, NULL) < 0)
            {
                g_printerr("avcodec_open2() failed.");
                exit(1);
            }

            pstCodecCtx->width = nWidth;
            pstCodecCtx->height = nHeight;
            pstCodecCtx->pix_fmt = PIX_FMT_YUV420P;

            avpicture_fill( (AVPicture *) &stPicture, NULL, PIX_FMT_RGB32, pstCodecCtx->width, pstCodecCtx->height );
        }

        stPacket.size = nSize;
        stPacket.data = puszDataFrameBuf;

        while (stPacket.size > 0)
        {
            if ( (nLen = avcodec_decode_video2(pstCodecCtx, pstFrame, &nFrameFinished, &stPacket)) < 0 )
            {
                g_printerr("avcodec_decode_video2() failed.");
                g_message("avcodec_decode_video2() failed.");
                exit(1);
            }

            if (nFrameFinished)
            {
                if (puszRgbData)
                {
                    free(puszRgbData);
                    puszRgbData = NULL;
                }

                puszRgbData = malloc(pstCodecCtx->width * pstCodecCtx->height * 4);
                stPicture.data[0] = puszRgbData;


                /* Convert the image from its native format to RGB */
                pstSwsCtx = sws_getContext(pstCodecCtx->width, pstCodecCtx->height, pstCodecCtx->pix_fmt, pstCodecCtx->width, pstCodecCtx->height, PIX_FMT_RGB32, SWS_BICUBIC, NULL, NULL, NULL);

                if (pstSwsCtx == NULL)
                {
                    g_printerr("sws_getContext() failed.");
                    g_message("sws_getContext() failed.");
                    exit(0);
                }

                sws_scale(pstSwsCtx, (const uint8_t * const*) pstFrame->data, pstFrame->linesize, 0, pstFrame->height, stPicture.data, stPicture.linesize);
                sws_freeContext(pstSwsCtx);


                surface = find_surface(c, 0);

                if (surface && surface->canvas)
                {
                    assert(surface);
                    assert(surface->canvas);

                    dest.left = dest.top = 0;
                    dest.right = pstCodecCtx->width;
                    dest.bottom = pstCodecCtx->height;

                    c->stStreamRenderCtx.nRgbStride = stPicture.linesize[0];
                    c->stStreamRenderCtx.channel = channel;
                    c->stStreamRenderCtx.dest = dest;

                    /* if finish request is detected, then don't go wait condition with redering frame. */
                    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

                    if ( (c->stStreamRenderCtx.nStatus == STREAM_RENDER_PAUSE_REQ) ||
                        (c->stStreamRenderCtx.nStatus == STREAM_RENDER_FIN_REQ) )
                    {
                        g_message("received PAUSE_REQ or FIN_REQ, skipping rendering in encoding thread");

                        g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

                        break;
                    }

                    c->stStreamRenderCtx.pData = stPicture.data[0];

                    g_idle_add( (GSourceFunc) DisplayStreamRenderFrame, &c->stStreamRenderCtx);

                    while (c->stStreamRenderCtx.pData != 0)
                    {
                        g_cond_wait( &c->stStreamRenderCtx.stCond, &c->stStreamRenderCtx.stMutex );
                    }

                    g_mutex_unlock(&c->stStreamRenderCtx.stMutex);
                }
                else
                {
                    g_printerr("!! error primary screen is not detected !!!");
                }
            }

            stPacket.size -= nLen;
            stPacket.data += nLen;
        }

        stPacket.data = NULL;
        stPacket.size = 0;

    } while(1);

    if (puszRgbData)
    {
        free(puszRgbData);
    }

    av_free_packet( &stPacket );
    av_free(pstFrame);
    avcodec_close(pstCodecCtx);

#ifdef G_OS_WIN32
    closesocket(nSock);
#else
    close(nSock);
#endif

    c->stStreamRenderCtx.pstThread = 0;

    return 0;
}


/*
 * Description : StreamRender thread join
 * Date        : 20180329
 */
static void JoinStreamRenderThread(GObject *object)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(object)->priv;


    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    while ( (c->stStreamRenderCtx.pstThread != 0) &&
        (c->stStreamRenderCtx.nStatus != STREAM_RENDER_START) &&
        (c->stStreamRenderCtx.nStatus != STREAM_RENDER_FIN_RSP) )
    {
        g_message("send FIN_REQ, wait for FIN_RSP of encoding thread from spice_display_channel_reset coroutine");

#ifdef G_OS_WIN32
        closesocket(c->stStreamRenderCtx.nSock);
#else
        close(c->stStreamRenderCtx.nSock);
#endif
        c->stStreamRenderCtx.nSock = -1;

        c->stStreamRenderCtx.nStatus = STREAM_RENDER_FIN_REQ;
        c->stStreamRenderCtx.pData = 0;

        g_cond_signal( &c->stStreamRenderCtx.stCond );
        g_cond_wait( &c->stStreamRenderCtx.stCond, &c->stStreamRenderCtx.stMutex );
    }

    if (c->stStreamRenderCtx.nStatus == STREAM_RENDER_START)
    {
        c->stStreamRenderCtx.nStatus = STREAM_RENDER_FIN_REQ;
    }

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

    while (c->stStreamRenderCtx.pstThread > (GThread *) 0)
    {
        usleep(20 * 1000);
    }
}


/*
 * Description : StreamRender config set
 * Date        : 20180329
 */
static void SetStreamRenderConfig(SpiceChannel *channel)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceSession *session = spice_channel_get_session(channel);
    SpiceSessionPrivate *s = session->priv;

    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    ReadSpiceStreamConfig();

    c->stStreamRenderCtx.nStreamPort = atoi(spice_session_get_streamport(session));
    c->stStreamRenderCtx.nStreamOnMovieDetection = atoi(spice_session_get_streamonmoviedetection(session));
    c->stStreamRenderCtx.nOnVariationCapture = atoi(spice_session_get_onvariationcapture(session));
    c->stStreamRenderCtx.nMaxSamplingFps = atoi(spice_session_get_maxsamplingfps(session));
    c->stStreamRenderCtx.nAudioSyncNot = atoi(spice_session_get_audiosyncnot(session));

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

    g_message("set stream_render_context as a spice_stream.config file.");
    g_message("stream_port             : [ %d ]", c->stStreamRenderCtx.nStreamPort);
    g_message("stream_onMovieDetection : [ %d ]", c->stStreamRenderCtx.nStreamOnMovieDetection);
    g_message("on_variation_capture    : [ %d ]", c->stStreamRenderCtx.nOnVariationCapture);
    g_message("max_sampling_fps        : [ %d ]", c->stStreamRenderCtx.nMaxSamplingFps);
    g_message("audio_sync_not          : [ %d ]", c->stStreamRenderCtx.nAudioSyncNot);
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static int ProcessPauseFinRequest(SpiceChannel *channel)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;


    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    if (c->stStreamRenderCtx.nStatus == STREAM_RENDER_FIN_REQ)
    {
        g_message("received FIN_REQ, set FIN_RSP and finish in encoding thread");

        c->stStreamRenderCtx.nStatus = STREAM_RENDER_FIN_RSP;

        g_cond_signal( &c->stStreamRenderCtx.stCond );

        g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

        return -1;
    }

    while ( (c->stStreamRenderCtx.nStatus == STREAM_RENDER_PAUSE_REQ) ||
        (c->stStreamRenderCtx.nStatus == STREAM_RENDER_PAUSE_RSP) )
    {
        g_message("received PAUSE_REQ, set PAUSE_RSP and condition_wait in encoding thread");

        c->stStreamRenderCtx.nStatus = STREAM_RENDER_PAUSE_RSP;

        g_cond_signal( &c->stStreamRenderCtx.stCond );

        g_cond_wait( &c->stStreamRenderCtx.stCond, &c->stStreamRenderCtx.stMutex );
    }

    c->stStreamRenderCtx.nStatus = STREAM_RENDER_RUN_RSP;

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

    return 0;
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static gboolean StreamFrameSchedule(SpiceChannel *channel)
{
    SpiceSession *session = spice_channel_get_session(channel);
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgDisplayStreamFrameData *op;
    SpiceMsgIn *in;
    guint32 mmtime = 0;
    guint32 audio_delay = 0;
    int32_t latency = 0;
    gboolean invalid_mm_time;


    if (c->stStreamRenderCtx.unTimeoutId)
    {
        return TRUE;
    }

    if ( (in = g_queue_peek_head(c->stStreamRenderCtx.pstLatencyQueue)) == NULL )
    {
        return TRUE;
    }

    op = spice_msg_in_parsed(in);

    mmtime = spice_session_get_mm_time(session, &invalid_mm_time);

    if (spice_session_is_playback_active(session))
    {
        audio_delay = spice_session_get_playback_latency(session);
    }

    latency = op->unMultiMediaTime - mmtime;


    if (audio_delay > 0)
    {
        latency = op->unMultiMediaTime + 400 + 15 - audio_delay - mmtime + 80;
    }

    if (latency < 0)
    {
        if (audio_delay > 0)
        {
            g_message("video stream data too late by %d ms (ts: %u, session-mmtime: %u, audio_delay:%u)",
                latency * (-1), op->unMultiMediaTime, mmtime, audio_delay);
        }

        latency = 0;
    }

    //g_message("StreamFrameSchedule() video latency: %d", latency); 
    CHANNEL_DEBUG(channel, "video latency: %d", latency);

    c->stStreamRenderCtx.unTimeoutId = g_timeout_add(latency, (GSourceFunc) StreamFrameDataMoveIntoMsgq, channel);

    return TRUE;
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static gboolean StreamFrameDataMoveIntoMsgq(SpiceChannel *channel)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    SpiceMsgIn *in;


    c->stStreamRenderCtx.unTimeoutId = 0;

    if ( (in = g_queue_pop_head(c->stStreamRenderCtx.pstLatencyQueue)) != NULL )
    {
        SendStreamFrameDataToDecoder(channel, in);

        spice_msg_in_unref(in);

        StreamFrameSchedule(channel);
    }

    return FALSE;
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static void SendStreamFrameDataToDecoder(SpiceChannel *channel, SpiceMsgIn *in)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;


    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    spice_msg_in_ref(in);

    g_queue_push_tail(c->stStreamRenderCtx.pstMsgQueue, in);

    g_cond_signal( &c->stStreamRenderCtx.stCond );

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static void ResetStreamFrameSchedule(SpiceChannel *channel)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;


    if (c->stStreamRenderCtx.unTimeoutId != 0)
    {
        g_source_remove(c->stStreamRenderCtx.unTimeoutId);
        c->stStreamRenderCtx.unTimeoutId = 0;
    }

    g_queue_foreach(c->stStreamRenderCtx.pstLatencyQueue, MsgInUnrefFuncGpointer, NULL);
    g_queue_clear(c->stStreamRenderCtx.pstLatencyQueue);

    c->stStreamRenderCtx.unSequenceNum = 0;
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static gboolean DisplayStreamRenderFrame(STREAM_RENDER_CONTEXT_T *pstStreamRenderCtx)
{
    SpiceChannel *channel = pstStreamRenderCtx->channel;
    SpiceDisplayChannelPrivate *c = NULL;
    SpiceRect dest = pstStreamRenderCtx->dest;
    display_surface *surface = NULL;


    assert(channel);

    c = SPICE_DISPLAY_CHANNEL(channel)->priv;

    assert(c);


    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    if (pstStreamRenderCtx->pData == 0)
    {
        g_mutex_unlock( &c->stStreamRenderCtx.stMutex );
        goto done;
    }

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

    surface = find_surface(c, 0);

    if (surface && surface->canvas)
    {
        surface->canvas->ops->put_image(surface->canvas,
#ifdef G_OS_WIN32
            c->dc,
#endif
            &dest, pstStreamRenderCtx->pData,
            dest.right, dest.bottom,
            pstStreamRenderCtx->nRgbStride, NULL);
    }

    g_signal_emit(channel, signals[SPICE_DISPLAY_INVALIDATE], 0, dest.left, dest.top, dest.right - dest.left, dest.bottom - dest.top);

done:
    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    pstStreamRenderCtx->pData = 0;

    g_cond_signal( &pstStreamRenderCtx->stCond );

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

    return G_SOURCE_REMOVE;
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static uint32_t DisplayChannelGetDecodingQueueSize(SpiceChannel *channel)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    int q_size = 0;
    int latency_q_size = 0;


    if ( !(c->stStreamRenderCtx.pstMsgQueue) )
    {
        return 0;
    }

    g_mutex_lock( &c->stStreamRenderCtx.stMutex );

    q_size = g_queue_get_length(c->stStreamRenderCtx.pstMsgQueue);

    g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

    latency_q_size = g_queue_get_length(c->stStreamRenderCtx.pstLatencyQueue);

    return q_size + latency_q_size;
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static int ReadEncodedFrame(SpiceChannel *channel, char *recvHeaderbuf, char *dataFramebuf)
{
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    ENCODER_HEADER_T *header = (ENCODER_HEADER_T *) recvHeaderbuf;
    SpiceMsgIn *in;
    SpiceMsgDisplayStreamFrameData *op;
    int nLeft = 0;
    int nRecv = 0;
    int nRet = 0;
    int private_header_read_flag = 0;
    int nSize = 0;

    if (c->stStreamRenderCtx.nUse)
    {
        g_mutex_lock( &c->stStreamRenderCtx.stMutex );

        while ( (g_queue_get_length(c->stStreamRenderCtx.pstMsgQueue) == 0) &&
            (c->stStreamRenderCtx.nStatus != STREAM_RENDER_FIN_REQ) )
        {
            if (c->stStreamRenderCtx.nStatus == STREAM_RENDER_PAUSE_REQ)
            {
                c->stStreamRenderCtx.nStatus = STREAM_RENDER_PAUSE_RSP;

                g_cond_signal( &c->stStreamRenderCtx.stCond );
            }

            g_cond_wait( &c->stStreamRenderCtx.stCond, &c->stStreamRenderCtx.stMutex );
        }

        if (c->stStreamRenderCtx.nStatus == STREAM_RENDER_FIN_REQ)
        {
            g_message("received FIN_REQ, set FIN_RSP and finish in encoding thread");

            c->stStreamRenderCtx.nStatus = STREAM_RENDER_FIN_RSP;

            g_cond_signal( &c->stStreamRenderCtx.stCond );

            g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

            return -1;
        }

        in = g_queue_pop_head(c->stStreamRenderCtx.pstMsgQueue);

        g_mutex_unlock( &c->stStreamRenderCtx.stMutex );

        op = spice_msg_in_parsed(in);

        memcpy(recvHeaderbuf, op->uszData, sizeof(ENCODER_HEADER_T));

        if (header->unCodecPrivateHeaderSize > 0)
        {
            memcpy(recvHeaderbuf + sizeof(ENCODER_HEADER_T), op->uszData + sizeof(ENCODER_HEADER_T), header->unCodecPrivateHeaderSize);
        }

        memcpy(dataFramebuf, op->uszData + sizeof(ENCODER_HEADER_T) + header->unCodecPrivateHeaderSize, header->unDataSize);

        assert(op->unDataSize == sizeof(ENCODER_HEADER_T) + header->unCodecPrivateHeaderSize + header->unDataSize);

        g_idle_add( (GSourceFunc) MsgInUnrefFuncSpiceMsgIn, in );
    }
    else
    {
        nLeft = sizeof(ENCODER_HEADER_T);
        nRecv = nRet = private_header_read_flag = 0;

        while (nLeft > 0)
        {
            if ( (nRet = RecvDataFromSocket(c->stStreamRenderCtx.nSock, (char *) recvHeaderbuf + nRecv, nLeft)) < 0 )
            {
                g_printerr("connection read fail");
                return -1;
            }

            nLeft -= nRet;
            nRecv += nRet;

            if (nLeft == 0 && !private_header_read_flag && header->unCodecPrivateHeaderSize > 0)
            {
                nLeft = header->unCodecPrivateHeaderSize;
                private_header_read_flag = 1;
            }
        }

        nSize = header->unDataSize;

        if (header->unTotalSize != header->unHeaderSize + header->unDataSize)
        {
            g_printerr("header value is not valid. you may be connecting wrong stream port. check config");
        }

        assert(header->unTotalSize == header->unHeaderSize + header->unDataSize);
        assert(header->unHeaderSize == sizeof(ENCODER_HEADER_T) + header->unCodecPrivateHeaderSize);

        nLeft = nSize;
        nRecv = 0;

        if ( (nRet = RecvDataFromSocket(c->stStreamRenderCtx.nSock, (char *) dataFramebuf + nRecv, nLeft)) < 0 )
        {
            g_printerr("connection read fail");
            return -1;
        }
    }

    return 0;
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static void MsgInUnrefFuncGpointer(gpointer data, gpointer user_data)
{
    spice_msg_in_unref(data);
}


/*
 * Description : 동영상 가속기
 * Date        : 20180329
 */
static gboolean MsgInUnrefFuncSpiceMsgIn(SpiceMsgIn *in)
{
    spice_msg_in_unref(in);

    return FALSE;
}


/*
 * Description : socket 생성
 * Date        : 20180329
 */
int GetStreamRenderSocket(SpiceChannel *channel)
{
    SpiceSession *session = spice_channel_get_session(channel);
    SpiceDisplayChannelPrivate *c = SPICE_DISPLAY_CHANNEL(channel)->priv;
    char g_spice_ip[120];
    int g_spice_port = 0;
    struct sockaddr_in serv_addr;
    int sockFD = 0;
    int stream_port = 0;
    char szTemp[ 1024 ];
    char *session_host = NULL;
    char *s_port = NULL;
    char *s_tls_port = NULL;
    GStrv s_secure_channels = NULL;
    const char *name = NULL;
    gchar *port = NULL;
    gchar *endptr = NULL;
    gboolean use_tls = FALSE;


    if ( (sockFD = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1)
    {   
        g_printerr("Could not create socket");
    }

    memset(&serv_addr, 0x00, sizeof(serv_addr));


    g_object_get(session,
                 "host", &session_host,
                 "port", &s_port,
                 "secure-channels", &s_secure_channels,
                 "tls-port", &s_tls_port,
                 NULL);

    g_message("main channel's ip:%s, port:%s", session_host, s_port);
    strcpy(g_spice_ip, session_host);


    name = spice_channel_type_to_string(channel->priv->channel_type);

    if (spice_strv_contains(s_secure_channels, "all") || spice_strv_contains(s_secure_channels, name))
    {
        use_tls = TRUE;
    }

    if (use_tls)
    {
        g_message("%s channel is to use tls", name);
    }

    if (s_port == NULL)
    {
        use_tls = TRUE;
    }

    port = use_tls ? s_tls_port : s_port;
    port = (s_tls_port != NULL ? s_tls_port : s_port);

    if (port == NULL)
    {
        g_printerr("main port or tls_port that is used is NULL");
        return -1;
    }

    stream_port = strtol(port, &endptr, 10);

    if (stream_port == 0 && s_port != NULL)
    {
        port = s_port;

        stream_port = strtol(port, &endptr, 10);
    }

    if (*port == '\0' || *endptr != '\0' || stream_port <= 0 || stream_port > G_MAXUINT16)
    {
        g_printerr("Invalid stream port value %d", stream_port);
        return -1;
    }


    if (c->stStreamRenderCtx.nStreamPort > 1000)
    {
        g_spice_port = c->stStreamRenderCtx.nStreamPort;
    }
    else
    {
        g_spice_port = stream_port + c->stStreamRenderCtx.nStreamPort;
    }

    g_message("spice ip:%s, stream port:%d", g_spice_ip, g_spice_port);


    if (strcmp(g_spice_ip, "localhost") == 0)
    {
        sprintf(g_spice_ip, "127.0.0.1");
    }

    serv_addr.sin_addr.s_addr = inet_addr(g_spice_ip);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(g_spice_port);

    if (connect(sockFD, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
    {
        g_printerr("Error : Connect Failed. at connecting to server with port %d", g_spice_port);
        return -1;
    }

    g_message("Connection completed");

    memset(szTemp, 0x00, sizeof(szTemp));

    sprintf(szTemp, "stream_port=%d\nstream_onMovieDetection=%d\non_variation_capture=%d\nmax_sampling_fps=%d\n",
        c->stStreamRenderCtx.nStreamPort, c->stStreamRenderCtx.nStreamOnMovieDetection,
        c->stStreamRenderCtx.nOnVariationCapture, c->stStreamRenderCtx.nMaxSamplingFps);

    if (SendDataToSocket(sockFD, szTemp, strlen(szTemp)) < 0)
    {
        g_printerr("error at sending token");
        exit(-1);
    }

    return sockFD;
}


/*
 * Description : data send
 * Date        : 20180329
 */
int SendDataToSocket(int nSock, void *pData, int nLength)
{
    int nSize = nLength;
    int nRet = 0;


    while (nSize > 0)
    {
        nRet = send(nSock, (char *) pData + (nLength - nSize), nSize, 0);

        if ( nRet < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
        {
            continue;
        }

        if (nRet < 0)
        {
            return nRet;
        }

        nSize -= nRet;
    }

    return nLength;
}


/*
 * Description : data receive
 * Date        : 20180329
 */
int RecvDataFromSocket(int nSock, void *pData, int nLength)
{
    int nSize = nLength;
    int nRet = 0;


    while (nSize > 0)
    {
        nRet = recv(nSock, (char *)pData + (nLength - nSize), nSize, 0);

        if ( nRet < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) )
        {
            continue;
        }

        if (nRet < 0)
        {
            return nRet;
        }

        nSize -= nRet;
    }

    return nLength;
}


#endif

