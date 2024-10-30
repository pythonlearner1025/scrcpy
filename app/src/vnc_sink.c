#include "vnc_sink.h"
#include <rfb/keysym.h>

#include <string.h>

#include "util/log.h"
#include "util/str.h"
#include "input_events.h"
#include "trait/key_processor.h"

/** Downcast frame_sink to sc_vnc_sink */
#define DOWNCAST(SINK) container_of(SINK, struct sc_vnc_sink, frame_sink)


static bool
sc_vnc_frame_sink_open(struct sc_frame_sink *sink, const AVCodecContext *ctx) {
    assert(ctx->pix_fmt == AV_PIX_FMT_YUV420P);
    (void) sink;
    (void) ctx;
    return true;
}

static void
sc_vnc_frame_sink_close(struct sc_frame_sink *sink) {
    (void) sink;
}

static bool
sc_vnc_frame_sink_push(struct sc_frame_sink *sink, const AVFrame *frame) {
    struct sc_vnc_sink *vnc = DOWNCAST(sink);
    // XXX: ideally this would get "damage" regions from the decoder
    // to prevent marking the entire screen as modified if only a small
    // part changed
    if(frame->width != vnc->scrWidth || frame->height != vnc->scrHeight) {
        if(vnc->ctx) {
            sws_freeContext(vnc->ctx);
            vnc->ctx = NULL;
        }
    }
    if(vnc->ctx == NULL) {
        vnc->scrWidth = frame->width;
        vnc->scrHeight = frame->height;
        vnc->ctx = sws_getContext(frame->width, frame->height, AV_PIX_FMT_YUV420P,
                                  frame->width, frame->height, AV_PIX_FMT_RGBA,
                                  0, 0, 0, 0);
        if(vnc->ctx == NULL) {
            LOGE("could not make context");
            return false;
        }
        char *currentFrameBuffer = vnc->screen->frameBuffer;
        char *newFrameBuffer = (char *)malloc(vnc->scrWidth*vnc->scrHeight*vnc->bpp);
        rfbNewFramebuffer(vnc->screen, newFrameBuffer, vnc->scrWidth, vnc->scrHeight, 8, 3, vnc->bpp);
        free(currentFrameBuffer);
    }
    assert(vnc->ctx != NULL);

    int linesize[1] = {frame->width*vnc->bpp};
    uint8_t *const data[1] = {(uint8_t*)vnc->screen->frameBuffer};
    sws_scale(vnc->ctx, (const uint8_t * const *)frame->data, frame->linesize, 0, frame->height, data, linesize);

    rfbMarkRectAsModified(vnc->screen, 0, 0, frame->width, frame->height);
    return true;
}

bool
sc_vnc_sink_init(struct sc_vnc_sink *vs, const char *device_name, struct sc_controller *controller, struct sc_key_processor *key_processor, uint16_t port) {
    uint8_t placeholder_width = 32;
    uint8_t placeholder_height = 32;
    static const struct sc_frame_sink_ops ops = {
        .open = sc_vnc_frame_sink_open,
        .close = sc_vnc_frame_sink_close,
        .push = sc_vnc_frame_sink_push,
    };

    vs->frame_sink.ops = &ops;
    vs->bpp = 4;
    vs->screen = rfbGetScreen(0, NULL, placeholder_width, placeholder_height, 8, 3, vs->bpp);
    vs->screen->desktopName = device_name;
    vs->screen->alwaysShared = true;
    vs->screen->frameBuffer = (char *)malloc(placeholder_width * placeholder_height * vs->bpp);
    vs->screen->ptrAddEvent = ptr_add_event;
    vs->screen->kbdAddEvent = kbd_add_event;
    vs->mods_state = 0;
    vs->screen->screenData = vs;
    vs->was_down = false;
    vs->controller = controller;
    vs->key_processor = key_processor;
    vs->screen->port = port;
    vs->screen->ipv6port = port;
    rfbInitServer(vs->screen);
    rfbRunEventLoop(vs->screen, -1, true); // TODO: integrate into proper lifecycle
    return true;
}

void
sc_vnc_sink_destroy(struct sc_vnc_sink *vs) {
    if(vs->screen) {
        rfbShutdownServer(vs->screen, true);
        free(vs->screen->frameBuffer);
        rfbScreenCleanup(vs->screen);
    }
    if(vs->ctx) {
        sws_freeContext(vs->ctx);
    }
}

void
ptr_add_event(int buttonMask, int x, int y, rfbClientPtr cl) {
    struct sc_vnc_sink *vnc = cl->screen->screenData;
    // buttonMask is 3 bits: MOUSE_RIGHT | MOUSE_MIDDLE | MOUSE_LEFT
    // value of 1 in that bit indicates it's being pressed; 0 indicates released

    // TODO: only doing left click
    bool up = (buttonMask & 0x1) == 0;
    struct sc_control_msg msg;
    struct sc_size screen_size = {vnc->scrWidth, vnc->scrHeight};
    struct sc_point point = {x, y};

    msg.type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT;
    if(vnc->was_down && !up) {
        msg.inject_touch_event.action = AMOTION_EVENT_ACTION_MOVE;
    } else {
        msg.inject_touch_event.action = up ? AMOTION_EVENT_ACTION_UP : AMOTION_EVENT_ACTION_DOWN;
    }
    msg.inject_touch_event.position.screen_size = screen_size;
    msg.inject_touch_event.position.point = point;
    msg.inject_touch_event.pointer_id = POINTER_ID_VIRTUAL_FINGER;
    // TODO: how to decide vs POINTER_ID_VIRTUAL_MOUSE?

    msg.inject_touch_event.pressure = up ? 0.0f : 1.0f;
    msg.inject_touch_event.action_button = 0;
    msg.inject_touch_event.buttons = 0;

    if (!sc_controller_push_msg(vnc->controller, &msg)) {
        LOGW("Could not request 'inject virtual finger event'");
    }

    rfbDefaultPtrAddEvent(buttonMask, x, y, cl);
    vnc->was_down = !up;
}

void kbd_add_event(rfbBool down, rfbKeySym key, rfbClientPtr cl) {
    struct sc_vnc_sink *vs = (struct sc_vnc_sink *)cl->screen->screenData;
    
    if (!vs->key_processor) {
        LOGD("No key processor available");
        return;
    }

    LOGD("VNC key event - key: 0x%x, down: %d", (unsigned int)key, down);
    LOGD("Current mods_state before: 0x%x", vs->mods_state);
    // Track modifier state
    uint16_t mod = 0;
    switch (key) {
        case XK_Shift_L:
            LOGD("Left Shift detected");
            if (down) {
                vs->mods_state |= (SC_MOD_LSHIFT | 0x1000);  // Add the additional flags
            } else {
                vs->mods_state &= ~(SC_MOD_LSHIFT | 0x1000);
            }
            break;
        case XK_Shift_R:
            if (down) {
                vs->mods_state |= (SC_MOD_RSHIFT | 0x1000);
            } else {
                vs->mods_state &= ~(SC_MOD_RSHIFT | 0x1000);
            }
            break;
    }

    enum sc_keycode keycode;
    switch (key) {
        case XK_BackSpace:
            keycode = SC_KEYCODE_BACKSPACE;
            break;
        case '_':
            keycode = '-';
            break;
        case '?':
            keycode = '/';
            break;
        case '>':
            keycode = '.';
            break;
        case '<':
            keycode = ',';
            break;
        case '~':
            keycode = '`';
            break;
        case '|':
            keycode = '\\';
        case '"':
            keycode = '\'';
            break;
        case ':':
            keycode = ';';
            break;
        default:
            if (key >= 'A' && key <= 'Z') {
                keycode = key + ('a' - 'A');  // Convert to lowercase
            } else if (key >= 0x21 && key <= 0x29) {
                keycode = key + 16; // Convert to 1-9 numbers
            } else {
                keycode = key;
            }
            break;
    }

    struct sc_key_event event = {
        .action = down ? SC_ACTION_DOWN : SC_ACTION_UP,
        .keycode = keycode,
        .mods_state = vs->mods_state,
        .repeat = 0
    };

    LOGD("Sending key event - keycode: 0x%x, action: %d, mods_state: 0x%x",
         keycode, event.action, event.mods_state);

    vs->key_processor->ops->process_key(vs->key_processor, &event, 0);
}