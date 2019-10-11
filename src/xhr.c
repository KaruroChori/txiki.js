/*
 * QuickJS libuv bindings
 *
 * Copyright (c) 2019-present Saúl Ibarra Corretgé <s@saghul.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "curl-utils.h"
#include "private.h"

#include <ctype.h>
#include <string.h>

#ifdef QUV_HAVE_CURL


enum {
    XHR_EVENT_ABORT = 0,
    XHR_EVENT_ERROR,
    XHR_EVENT_LOAD,
    XHR_EVENT_LOAD_END,
    XHR_EVENT_LOAD_START,
    XHR_EVENT_PROGRESS,
    XHR_EVENT_READY_STATE_CHANGED,
    XHR_EVENT_TIMEOUT,
    XHR_EVENT_MAX,
};

enum {
    XHR_RSTATE_UNSENT = 0,
    XHR_RSTATE_OPENED,
    XHR_RSTATE_HEADERS_RECEIVED,
    XHR_RSTATE_LOADING,
    XHR_RSTATE_DONE,
};

enum {
    XHR_RTYPE_DEFAULT = 0,
    XHR_RTYPE_TEXT,
    XHR_RTYPE_ARRAY_BUFFER,
    XHR_RTYPE_JSON,
};

typedef struct {
    JSContext *ctx;
    JSValue events[XHR_EVENT_MAX];
    quv_curl_private_t curl_private;
    CURL *curl_h;
    CURLM *curlm_h;
    struct curl_slist *slist;
    bool sent;
    unsigned long timeout;
    short response_type;
    unsigned short ready_state;
    struct {
        char *raw;
        JSValue status;
        JSValue status_text;
    } status;
    struct {
        JSValue url;
        JSValue headers;
        JSValue response;
        JSValue response_text;
        DynBuf hbuf;
        DynBuf bbuf;
    } result;
} QUVXhr;

static JSClassID quv_xhr_class_id;

static void quv_xhr_finalizer(JSRuntime *rt, JSValue val) {
    QUVXhr *x = JS_GetOpaque(val, quv_xhr_class_id);
    if (x) {
        if (x->curl_h) {
            curl_multi_remove_handle(x->curlm_h, x->curl_h);
            curl_easy_cleanup(x->curl_h);
        }
        if (x->slist)
            curl_slist_free_all(x->slist);
        if (x->status.raw)
            js_free_rt(rt, x->status.raw);
        for (int i = 0; i < XHR_EVENT_MAX; i++)
            JS_FreeValueRT(rt, x->events[i]);
        JS_FreeValueRT(rt, x->status.status);
        JS_FreeValueRT(rt, x->status.status_text);
        JS_FreeValueRT(rt, x->result.url);
        JS_FreeValueRT(rt, x->result.headers);
        JS_FreeValueRT(rt, x->result.response);
        JS_FreeValueRT(rt, x->result.response_text);

        free(x);
    }
}

static void quv_xhr_mark(JSRuntime *rt, JSValueConst val, JS_MarkFunc *mark_func) {
    QUVXhr *x = JS_GetOpaque(val, quv_xhr_class_id);
    if (x) {
        for (int i = 0; i < XHR_EVENT_MAX; i++)
            JS_MarkValue(rt, x->events[i], mark_func);
        JS_MarkValue(rt, x->status.status, mark_func);
        JS_MarkValue(rt, x->status.status_text, mark_func);
        JS_MarkValue(rt, x->result.url, mark_func);
        JS_MarkValue(rt, x->result.headers, mark_func);
        JS_MarkValue(rt, x->result.response, mark_func);
        JS_MarkValue(rt, x->result.response_text, mark_func);
    }
}

static JSClassDef quv_xhr_class = {
    "XMLHttpRequest",
    .finalizer = quv_xhr_finalizer,
    .gc_mark = quv_xhr_mark,
};

static QUVXhr *quv_xhr_get(JSContext *ctx, JSValueConst obj) {
    return JS_GetOpaque2(ctx, obj, quv_xhr_class_id);
}

static void maybe_emit_event(QUVXhr *x, int event, JSValue arg) {
    JSContext *ctx = x->ctx;
    JSValue event_func = x->events[event];
    if (!JS_IsFunction(ctx, event_func)) {
        JS_FreeValue(ctx, arg);
        return;
    }

    JSValue func = JS_DupValue(ctx, event_func);
    JSValue ret = JS_Call(ctx, func, JS_UNDEFINED, 1, (JSValueConst *) &arg);
    if (JS_IsException(ret))
        quv_dump_error(ctx);

    JS_FreeValue(ctx, ret);
    JS_FreeValue(ctx, func);
    JS_FreeValue(ctx, arg);
}

static void curl__done_cb(CURLMsg *message, void *arg) {
    QUVXhr *x = arg;
    CHECK_NOT_NULL(x);

    CURL *easy_handle = message->easy_handle;
    CHECK_EQ(x->curl_h, easy_handle);

    CURLcode result = message->data.result;

    char *done_url = NULL;
    curl_easy_getinfo(easy_handle, CURLINFO_EFFECTIVE_URL, &done_url);
    if (done_url)
        x->result.url = JS_NewString(x->ctx, done_url);

    // The calling function will disengage the easy handle when this
    // function returns.
    x->curl_h = NULL;

    if (x->slist) {
        curl_slist_free_all(x->slist);
        x->slist = NULL;
    }

    x->ready_state = XHR_RSTATE_DONE;
    maybe_emit_event(x, XHR_EVENT_READY_STATE_CHANGED, JS_UNDEFINED);

    if (result == CURLE_OPERATION_TIMEDOUT)
        maybe_emit_event(x, XHR_EVENT_TIMEOUT, JS_UNDEFINED);

    maybe_emit_event(x, XHR_EVENT_LOAD_END, JS_UNDEFINED);

    if (result != CURLE_OPERATION_TIMEDOUT) {
        if (result != CURLE_OK)
            maybe_emit_event(x, XHR_EVENT_ERROR, JS_UNDEFINED);
        else
            maybe_emit_event(x, XHR_EVENT_LOAD, JS_UNDEFINED);
    }
}

static size_t curl__data_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    QUVXhr *x = userdata;
    CHECK_NOT_NULL(x);

    if (x->ready_state == XHR_RSTATE_HEADERS_RECEIVED) {
        x->ready_state = XHR_RSTATE_LOADING;
        maybe_emit_event(x, XHR_EVENT_READY_STATE_CHANGED, JS_UNDEFINED);
    }

    size_t realsize = size * nmemb;

    if (dbuf_put(&x->result.bbuf, (const uint8_t *) ptr, realsize))
        return -1;

    return realsize;
}

static size_t curl__header_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    static const char status_line[] = "HTTP/";
    static const char emptly_line[] = "\r\n";

    QUVXhr *x = userdata;
    CHECK_NOT_NULL(x);

    DynBuf *hbuf = &x->result.hbuf;
    size_t realsize = size * nmemb;

    if (strncmp(status_line, ptr, sizeof(status_line) - 1) == 0) {
        if (hbuf->size == 0) {
            // Fire loadstart on the first HTTP status line.
            maybe_emit_event(x, XHR_EVENT_LOAD_START, JS_UNDEFINED);
        } else {
            dbuf_free(hbuf);
            dbuf_init(hbuf);
        }
        if (x->status.raw) {
            js_free(x->ctx, x->status.raw);
            x->status.raw = NULL;
        }
        // Store status line without the protocol.
        const char *p = memchr(ptr, ' ', realsize);
        if (p) {
            *(ptr + realsize - 2) = '\0';
            x->status.raw = js_strdup(x->ctx, p + 1);
        }
    } else if (strncmp(emptly_line, ptr, sizeof(emptly_line) - 1) == 0) {
        // If the code is not a redirect, this is the final response.
        long code = -1;
        curl_easy_getinfo(x->curl_h, CURLINFO_RESPONSE_CODE, &code);
        if (code > -1 && code / 100 != 3) {
            CHECK_NOT_NULL(x->status.raw);
            x->status.status_text = JS_NewString(x->ctx, x->status.raw);
            x->status.status = JS_NewInt32(x->ctx, code);
            x->ready_state = XHR_RSTATE_HEADERS_RECEIVED;
            maybe_emit_event(x, XHR_EVENT_READY_STATE_CHANGED, JS_UNDEFINED);
            dbuf_putc(hbuf, '\0');
        }
    } else {
        const char *p = memchr(ptr, ':', realsize);
        if (p) {
            // Lowercae header names.
            for (char *tmp = ptr; tmp != p; tmp++)
                *tmp = tolower(*tmp);
            if (dbuf_put(hbuf, (const uint8_t *) ptr, realsize))
                return -1;
        }
    }

    return realsize;
}

static int curl__progress_cb(void *clientp,
                             curl_off_t dltotal,
                             curl_off_t dlnow,
                             curl_off_t ultotal,
                             curl_off_t ulnow) {
    QUVXhr *x = clientp;
    CHECK_NOT_NULL(x);

    if (x->ready_state == XHR_RSTATE_LOADING) {
        double cl = -1;
        curl_easy_getinfo(x->curl_h, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);
        JSContext *ctx = x->ctx;
        JSValue event = JS_NewObjectProto(ctx, JS_NULL);
        JS_DefinePropertyValueStr(ctx, event, "lengthComputable", JS_NewBool(ctx, cl > 0), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, event, "loaded", JS_NewInt64(ctx, dlnow), JS_PROP_C_W_E);
        JS_DefinePropertyValueStr(ctx, event, "total", JS_NewInt64(ctx, dltotal), JS_PROP_C_W_E);
        maybe_emit_event(x, XHR_EVENT_PROGRESS, event);
    }

    return 0;
}

static JSValue quv_xhr_constructor(JSContext *ctx, JSValueConst new_target, int argc, JSValueConst *argv) {
    JSValue obj = JS_NewObjectClass(ctx, quv_xhr_class_id);
    if (JS_IsException(obj))
        return obj;

    QUVXhr *x = calloc(1, sizeof(*x));
    if (!x) {
        JS_FreeValue(ctx, obj);
        return JS_EXCEPTION;
    }

    x->ctx = ctx;
    x->result.url = JS_NULL;
    x->result.headers = JS_NULL;
    x->result.response = JS_NULL;
    x->result.response_text = JS_NULL;
    dbuf_init(&x->result.hbuf);
    dbuf_init(&x->result.bbuf);
    x->ready_state = XHR_RSTATE_UNSENT;
    x->status.raw = NULL;
    x->status.status = JS_UNDEFINED;
    x->status.status_text = JS_UNDEFINED;
    x->slist = NULL;
    x->sent = false;

    for (int i = 0; i < XHR_EVENT_MAX; i++) {
        x->events[i] = JS_UNDEFINED;
    }

    quv_curl_init();

    x->curl_private.arg = x;
    x->curl_private.done_cb = curl__done_cb;

    x->curlm_h = quv__get_curlm(ctx);
    x->curl_h = curl_easy_init();
    curl_easy_setopt(x->curl_h, CURLOPT_PRIVATE, &x->curl_private);
    curl_easy_setopt(x->curl_h, CURLOPT_USERAGENT, "quv/1.0");
    curl_easy_setopt(x->curl_h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(x->curl_h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(x->curl_h, CURLOPT_NOSIGNAL, 1L);
#ifdef CURL_HTTP_VERSION_2
    curl_easy_setopt(x->curl_h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2);
#endif
    curl_easy_setopt(x->curl_h, CURLOPT_XFERINFOFUNCTION, curl__progress_cb);
    curl_easy_setopt(x->curl_h, CURLOPT_XFERINFODATA, x);
    curl_easy_setopt(x->curl_h, CURLOPT_WRITEFUNCTION, curl__data_cb);
    curl_easy_setopt(x->curl_h, CURLOPT_WRITEDATA, x);
    curl_easy_setopt(x->curl_h, CURLOPT_HEADERFUNCTION, curl__header_cb);
    curl_easy_setopt(x->curl_h, CURLOPT_HEADERDATA, x);
#if defined(_WIN32)
    curl_easy_setopt(x->curl_h, CURLOPT_CAINFO, "cacert.pem");
#endif

    JS_SetOpaque(obj, x);
    return obj;
}

static JSValue quv_xhr_event_get(JSContext *ctx, JSValueConst this_val, int magic) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, x->events[magic]);
}

static JSValue quv_xhr_event_set(JSContext *ctx, JSValueConst this_val, JSValueConst value, int magic) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    if (JS_IsFunction(ctx, value) || JS_IsUndefined(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, x->events[magic]);
        x->events[magic] = JS_DupValue(ctx, value);
    }
    return JS_UNDEFINED;
}

static JSValue quv_xhr_readystate_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, x->ready_state);
}

static JSValue quv_xhr_response_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    DynBuf *bbuf = &x->result.bbuf;
    if (bbuf->size == 0)
        return JS_NULL;
    if (JS_IsNull(x->result.response)) {
        switch (x->response_type) {
            case XHR_RTYPE_DEFAULT:
            case XHR_RTYPE_TEXT:
                x->result.response = JS_NewStringLen(ctx, (char *) bbuf->buf, bbuf->size);
                break;
            case XHR_RTYPE_ARRAY_BUFFER:
                x->result.response = JS_NewArrayBufferCopy(ctx, bbuf->buf, bbuf->size);
                break;
            case XHR_RTYPE_JSON:
                // It's necessary to null-terminate the string passed to JS_ParseJSON.
                dbuf_putc(bbuf, '\0');
                x->result.response = JS_ParseJSON(ctx, (char *) bbuf->buf, bbuf->size, "<xhr>");
                break;
            default:
                abort();
        }
    }
    return JS_DupValue(ctx, x->result.response);
}

static JSValue quv_xhr_responsetext_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    DynBuf *bbuf = &x->result.bbuf;
    if (bbuf->size == 0)
        return JS_NULL;
    if (JS_IsNull(x->result.response_text))
        x->result.response_text = JS_NewStringLen(ctx, (char *) bbuf->buf, bbuf->size);
    return JS_DupValue(ctx, x->result.response_text);
}

static JSValue quv_xhr_responsetype_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    switch (x->response_type) {
        case XHR_RTYPE_DEFAULT:
            return JS_NewString(ctx, "");
        case XHR_RTYPE_TEXT:
            return JS_NewString(ctx, "text");
        case XHR_RTYPE_ARRAY_BUFFER:
            return JS_NewString(ctx, "arraybuffer");
        case XHR_RTYPE_JSON:
            return JS_NewString(ctx, "json");
        default:
            abort();
    }
}

static JSValue quv_xhr_responsetype_set(JSContext *ctx, JSValueConst this_val, JSValueConst value) {
    static const char array_buffer[] = "arraybuffer";
    static const char json[] = "json";
    static const char text[] = "text";

    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;

    if (x->ready_state >= XHR_RSTATE_LOADING)
        JS_Throw(ctx, JS_NewString(ctx, "InvalidStateError"));

    const char *v = JS_ToCString(ctx, value);
    if (v) {
        if (strncmp(array_buffer, v, sizeof(array_buffer) - 1) == 0)
            x->response_type = XHR_RTYPE_ARRAY_BUFFER;
        else if (strncmp(json, v, sizeof(json) - 1) == 0)
            x->response_type = XHR_RTYPE_JSON;
        else if (strncmp(text, v, sizeof(text) - 1) == 0)
            x->response_type = XHR_RTYPE_TEXT;
        else if (strlen(v) == 0)
            x->response_type = XHR_RTYPE_DEFAULT;
        JS_FreeCString(ctx, v);
    }

    return JS_UNDEFINED;
}

static JSValue quv_xhr_responseurl_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, x->result.url);
}

static JSValue quv_xhr_status_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, x->status.status);
}

static JSValue quv_xhr_statustext_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    return JS_DupValue(ctx, x->status.status_text);
}

static JSValue quv_xhr_timeout_get(JSContext *ctx, JSValueConst this_val) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    return JS_NewInt32(ctx, x->timeout);
}

static JSValue quv_xhr_timeout_set(JSContext *ctx, JSValueConst this_val, JSValueConst value) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;

    int32_t timeout;
    if (JS_ToInt32(ctx, &timeout, value))
        return JS_EXCEPTION;

    x->timeout = timeout;

    if (!x->sent)
        curl_easy_setopt(x->curl_h, CURLOPT_TIMEOUT_MS, timeout);

    return JS_UNDEFINED;
}

static JSValue quv_xhr_upload_get(JSContext *ctx, JSValueConst this_val) {
    // TODO.
    return JS_UNDEFINED;
}

static JSValue quv_xhr_withcredentials_get(JSContext *ctx, JSValueConst this_val) {
    // TODO.
    return JS_UNDEFINED;
}

static JSValue quv_xhr_withcredentials_set(JSContext *ctx, JSValueConst this_val, JSValueConst value) {
    // TODO.
    return JS_UNDEFINED;
}

static JSValue quv_xhr_abort(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    if (x->curl_h) {
        curl_multi_remove_handle(x->curlm_h, x->curl_h);
        curl_easy_cleanup(x->curl_h);
        x->curl_h = NULL;
        x->curlm_h = NULL;
        x->ready_state = XHR_RSTATE_UNSENT;
        JS_FreeValue(ctx, x->status.status);
        x->status.status = JS_NewInt32(x->ctx, 0);
        JS_FreeValue(ctx, x->status.status_text);
        x->status.status_text = JS_NewString(ctx, "");

        maybe_emit_event(x, XHR_EVENT_ABORT, JS_UNDEFINED);
    }
    return JS_UNDEFINED;
}

static JSValue quv_xhr_getallresponseheaders(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    DynBuf *hbuf = &x->result.hbuf;
    if (hbuf->size == 0)
        return JS_NULL;
    if (JS_IsNull(x->result.headers))
        x->result.headers = JS_NewStringLen(ctx, (char *) hbuf->buf, hbuf->size - 1);  // Skip trailing null byte.
    return JS_DupValue(ctx, x->result.headers);
}

static JSValue quv_xhr_getresponseheader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    DynBuf *hbuf = &x->result.hbuf;
    if (hbuf->size == 0)
        return JS_NULL;
    const char *header_name = JS_ToCString(ctx, argv[0]);
    if (!header_name)
        return JS_EXCEPTION;

    // Lowercae header name
    for (char *tmp = (char *) header_name; *tmp; tmp++)
        *tmp = tolower(*tmp);

    DynBuf r;
    dbuf_init(&r);
    char *ptr = (char *) hbuf->buf;
    for (;;) {
        // Find the header name
        char *tmp = strstr(ptr, header_name);
        if (!tmp)
            break;
        // Find the end of the header, the \r
        char *p = strchr(tmp, '\r');
        if (!p)
            break;
        // Check if the header has a value
        char *p1 = memchr(tmp, ':', p - tmp);
        if (p1) {
            p1++;  // skip the ":"
            for (; *p1 == ' '; ++p1)
                ;
            // p1 now points to the start of the header value
            // check if it was a header without a value like x-foo:\r\n
            size_t size = p - p1;
            if (size > 0) {
                dbuf_put(&r, (const uint8_t *) p1, size);
                dbuf_putstr(&r, ", ");
            }
        }
        ptr = p;
    }

    JS_FreeCString(ctx, header_name);

    JSValue ret;
    if (r.size == 0)
        ret = JS_NULL;
    else
        ret = JS_NewStringLen(ctx, (const char *) r.buf, r.size - 2);  // skip the last ", "
    dbuf_free(&r);
    return ret;
}

static JSValue quv_xhr_open(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    static const char head_method[] = "HEAD";

    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;

    // TODO: support username and password.

    if (x->ready_state < XHR_RSTATE_OPENED) {
        JSValue method = argv[0];
        JSValue url = argv[1];
        const char *method_str = JS_ToCString(ctx, method);
        const char *url_str = JS_ToCString(ctx, url);

        if (strncasecmp(head_method, method_str, sizeof(head_method) - 1) == 0)
            curl_easy_setopt(x->curl_h, CURLOPT_NOBODY, 1L);
        else
            curl_easy_setopt(x->curl_h, CURLOPT_CUSTOMREQUEST, method_str);
        curl_easy_setopt(x->curl_h, CURLOPT_URL, url_str);

        JS_FreeCString(ctx, method_str);
        JS_FreeCString(ctx, url_str);

        x->ready_state = XHR_RSTATE_OPENED;
        maybe_emit_event(x, XHR_EVENT_READY_STATE_CHANGED, JS_UNDEFINED);
    }

    return JS_UNDEFINED;
}

static JSValue quv_xhr_overridemimetype(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    return JS_ThrowTypeError(ctx, "unsupported");
}

static JSValue quv_xhr_send(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    if (!x->sent) {
        JSValue arg = argv[0];
        if (JS_IsString(arg)) {
            size_t size;
            const char *body = JS_ToCStringLen(ctx, &size, arg);
            if (body) {
                curl_easy_setopt(x->curl_h, CURLOPT_POSTFIELDSIZE, (long) size);
                curl_easy_setopt(x->curl_h, CURLOPT_COPYPOSTFIELDS, body);
                JS_FreeCString(ctx, body);
            }
        }
        if (x->slist)
            curl_easy_setopt(x->curl_h, CURLOPT_HTTPHEADER, x->slist);
        curl_multi_add_handle(x->curlm_h, x->curl_h);
        x->sent = true;
    }
    return JS_UNDEFINED;
}

static JSValue quv_xhr_setrequestheader(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv) {
    QUVXhr *x = quv_xhr_get(ctx, this_val);
    if (!x)
        return JS_EXCEPTION;
    if (!JS_IsString(argv[0]))
        return JS_UNDEFINED;
    const char *h_name, *h_value = NULL;
    h_name = JS_ToCString(ctx, argv[0]);
    if (!JS_IsUndefined(argv[1]))
        h_value = JS_ToCString(ctx, argv[1]);
    char buf[CURL_MAX_HTTP_HEADER];
    if (h_value)
        snprintf(buf, sizeof(buf), "%s: %s", h_name, h_value);
    else
        snprintf(buf, sizeof(buf), "%s;", h_name);
    JS_FreeCString(ctx, h_name);
    if (h_value)
        JS_FreeCString(ctx, h_value);
    struct curl_slist *list = curl_slist_append(x->slist, buf);
    if (list)
        x->slist = list;
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry quv_xhr_proto_funcs[] = {
    JS_PROP_INT32_DEF("UNSENT", XHR_RSTATE_UNSENT, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("OPENED", XHR_RSTATE_OPENED, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("HEADERS_RECEIVED", XHR_RSTATE_HEADERS_RECEIVED, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("LOADING", XHR_RSTATE_LOADING, JS_PROP_ENUMERABLE),
    JS_PROP_INT32_DEF("DONE", XHR_RSTATE_DONE, JS_PROP_ENUMERABLE),
    JS_CGETSET_MAGIC_DEF("onabort", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_ABORT),
    JS_CGETSET_MAGIC_DEF("onerror", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_ERROR),
    JS_CGETSET_MAGIC_DEF("onload", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_LOAD),
    JS_CGETSET_MAGIC_DEF("onloadend", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_LOAD_END),
    JS_CGETSET_MAGIC_DEF("onloadstart", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_LOAD_START),
    JS_CGETSET_MAGIC_DEF("onprogress", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_PROGRESS),
    JS_CGETSET_MAGIC_DEF("onreadystatechange", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_READY_STATE_CHANGED),
    JS_CGETSET_MAGIC_DEF("ontimeout", quv_xhr_event_get, quv_xhr_event_set, XHR_EVENT_TIMEOUT),
    JS_CGETSET_DEF("readyState", quv_xhr_readystate_get, NULL),
    JS_CGETSET_DEF("response", quv_xhr_response_get, NULL),
    JS_CGETSET_DEF("responseText", quv_xhr_responsetext_get, NULL),
    JS_CGETSET_DEF("responseType", quv_xhr_responsetype_get, quv_xhr_responsetype_set),
    JS_CGETSET_DEF("responseURL", quv_xhr_responseurl_get, NULL),
    JS_CGETSET_DEF("status", quv_xhr_status_get, NULL),
    JS_CGETSET_DEF("statusText", quv_xhr_statustext_get, NULL),
    JS_CGETSET_DEF("timeout", quv_xhr_timeout_get, quv_xhr_timeout_set),
    JS_CGETSET_DEF("upload", quv_xhr_upload_get, NULL),
    JS_CGETSET_DEF("withCcredentials", quv_xhr_withcredentials_get, quv_xhr_withcredentials_set),
    JS_CFUNC_DEF("abort", 0, quv_xhr_abort),
    JS_CFUNC_DEF("getAllResponseHeaders", 0, quv_xhr_getallresponseheaders),
    JS_CFUNC_DEF("getResponseHeader", 1, quv_xhr_getresponseheader),
    JS_CFUNC_DEF("open", 5, quv_xhr_open),
    JS_CFUNC_DEF("overrideMimeType", 1, quv_xhr_overridemimetype),
    JS_CFUNC_DEF("send", 1, quv_xhr_send),
    JS_CFUNC_DEF("setRequestHeader", 2, quv_xhr_setrequestheader),
};

void quv_mod_xhr_init(JSContext *ctx, JSModuleDef *m) {
    JSValue proto, obj;

    /* XHR class */
    JS_NewClassID(&quv_xhr_class_id);
    JS_NewClass(JS_GetRuntime(ctx), quv_xhr_class_id, &quv_xhr_class);
    proto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, proto, quv_xhr_proto_funcs, countof(quv_xhr_proto_funcs));
    JS_SetClassProto(ctx, quv_xhr_class_id, proto);

    /* XHR object */
    obj = JS_NewCFunction2(ctx, quv_xhr_constructor, "XMLHttpRequest", 1, JS_CFUNC_constructor, 0);
    JS_SetModuleExport(ctx, m, "XMLHttpRequest", obj);
}

void quv_mod_xhr_export(JSContext *ctx, JSModuleDef *m) {
    JS_AddModuleExport(ctx, m, "XMLHttpRequest");
}

#endif