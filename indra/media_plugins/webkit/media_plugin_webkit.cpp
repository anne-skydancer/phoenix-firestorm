/**
 * @file media_plugin_webkit.cpp
 * @brief WebKit2GTK (webkit2gtk-4.1 / GTK3) MOAP plugin for FreeBSD, where
 *        CEF/dullahan is unavailable. Renders web + image content offscreen
 *        into the viewer's shared-memory pixel buffer via the LLPlugin
 *        protocol.
 *
 * $LicenseInfo:firstyear=2008&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * $/LicenseInfo$
 */

#include "linden_common.h"
#include "indra_constants.h"
#include "llgl.h"
#include "llplugininstance.h"
#include "llpluginmessage.h"
#include "llpluginmessageclasses.h"
#include "media_plugin_base.h"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <cairo.h>
#include <cstring>

////////////////////////////////////////////////////////////////////////////////
class MediaPluginWebKit : public MediaPluginBase
{
public:
    MediaPluginWebKit(LLPluginInstance::sendMessageFunction host_send_func, void* host_user_data);
    ~MediaPluginWebKit();

    /*virtual*/ void receiveMessage(const char* message_string);

private:
    bool initGTK();
    void createWebView();
    void resizeTo(int w, int h);
    void update();          // pump GTK loop + blit dirty pixels
    void blitPixels();      // cairo surface -> mPixels (BGRA, v-flipped, stride-aware)
    void navigate(const std::string& uri);

    // ---- GObject signal trampolines (static -> instance) ----
    static void onLoadChanged(WebKitWebView* view, WebKitLoadEvent ev, gpointer self);
    static gboolean onLoadFailed(WebKitWebView* view, WebKitLoadEvent ev,
                                 gchar* failing_uri, GError* error, gpointer self);
    static void onTitleChanged(GObject* obj, GParamSpec* pspec, gpointer self);
    static void onUriChanged(GObject* obj, GParamSpec* pspec, gpointer self);

    // ---- outbound helpers ----
    void sendNavigateBegin();
    void sendNavigateComplete(int code, const std::string& uri);
    void sendLocationChanged(const std::string& uri);
    void sendNameText();

    // ---- input synthesis ----
    void mouseEvent(const std::string& event, int x, int y, int button);
    void scrollEvent(int x, int y, int clicks_x, int clicks_y);
    void keyEvent(const std::string& event, const LLSD& native_key_data);
    void textEvent(const std::string& event, const LLSD& native_key_data);

    GtkWidget*     mOffscreen;   // GtkOffscreenWindow
    WebKitWebView* mWebView;
    bool           mGtkReady;
    std::string    mCurrentUri;
};

////////////////////////////////////////////////////////////////////////////////
MediaPluginWebKit::MediaPluginWebKit(LLPluginInstance::sendMessageFunction host_send_func, void* host_user_data) :
    MediaPluginBase(host_send_func, host_user_data)
{
    mWidth = 0; mHeight = 0; mDepth = 4; mPixels = 0;
    mTextureWidth = 0; mTextureHeight = 0;
    mOffscreen = nullptr; mWebView = nullptr; mGtkReady = false;
}

MediaPluginWebKit::~MediaPluginWebKit()
{
    // GtkOffscreenWindow owns the WebView; destroying it releases both.
    if (mOffscreen) { gtk_widget_destroy(mOffscreen); mOffscreen = nullptr; mWebView = nullptr; }
}

////////////////////////////////////////////////////////////////////////////////
bool MediaPluginWebKit::initGTK()
{
    if (mGtkReady) return true;
    // Non-fatal init: the plugin must never exit() the SLPlugin host.
    if (!gtk_init_check(nullptr, nullptr))
    {
        setStatus(STATUS_ERROR);
        return false;
    }
    mGtkReady = true;
    return true;
}

void MediaPluginWebKit::createWebView()
{
    if (!mGtkReady || mWebView) return;

    mOffscreen = gtk_offscreen_window_new();
    mWebView   = WEBKIT_WEB_VIEW(webkit_web_view_new());

    // Opaque white background so ARGB32 premultiplied alpha is a no-op.
    GdkRGBA bg = { 1.0, 1.0, 1.0, 1.0 };
    webkit_web_view_set_background_color(mWebView, &bg);

    gtk_container_add(GTK_CONTAINER(mOffscreen), GTK_WIDGET(mWebView));
    gtk_widget_show_all(mOffscreen);

    g_signal_connect(mWebView, "load-changed", G_CALLBACK(onLoadChanged), this);
    g_signal_connect(mWebView, "load-failed",  G_CALLBACK(onLoadFailed),  this);
    g_signal_connect(mWebView, "notify::title", G_CALLBACK(onTitleChanged), this);
    g_signal_connect(mWebView, "notify::uri",   G_CALLBACK(onUriChanged),   this);
}

void MediaPluginWebKit::resizeTo(int w, int h)
{
    if (!mWebView || w <= 0 || h <= 0) return;
    gtk_window_set_default_size(GTK_WINDOW(mOffscreen), w, h);
    gtk_window_resize(GTK_WINDOW(mOffscreen), w, h);
    gtk_widget_set_size_request(GTK_WIDGET(mWebView), w, h);
}

////////////////////////////////////////////////////////////////////////////////
void MediaPluginWebKit::update()
{
    if (!mGtkReady) return;
    // Never call gtk_main(); pump exactly what is queued so we return to the host.
    while (gtk_events_pending())
        gtk_main_iteration_do(FALSE);
    blitPixels();
}

void MediaPluginWebKit::blitPixels()
{
    if (!mPixels || !mOffscreen || mWidth <= 0 || mHeight <= 0) return;

    cairo_surface_t* surf = gtk_offscreen_window_get_surface(GTK_OFFSCREEN_WINDOW(mOffscreen));
    if (!surf) return;
    cairo_surface_flush(surf);

    if (cairo_image_surface_get_format(surf) != CAIRO_FORMAT_ARGB32) return; // BGRA in LE memory
    const unsigned char* src = cairo_image_surface_get_data(surf);
    if (!src) return;

    int src_stride = cairo_image_surface_get_stride(surf);   // may exceed w*4 (cairo padding)
    int sw = cairo_image_surface_get_width(surf);
    int sh = cairo_image_surface_get_height(surf);

    int copy_w = (sw < mWidth ? sw : mWidth);
    int copy_h = (sh < mHeight ? sh : mHeight);
    int dst_stride = mWidth * mDepth;
    int row_bytes = copy_w * mDepth;

    // Vertical flip: coords_opengl=true means viewer row 0 == bottom.
    for (int y = 0; y < copy_h; ++y)
    {
        const unsigned char* s = src + (size_t)y * src_stride;
        unsigned char* d = mPixels + (size_t)(mHeight - 1 - y) * dst_stride;
        memcpy(d, s, row_bytes);
    }

    setDirty(0, 0, mWidth, mHeight);
}

void MediaPluginWebKit::navigate(const std::string& uri)
{
    if (!mWebView) return;
    mCurrentUri = uri;
    webkit_web_view_load_uri(mWebView, uri.c_str());
}

////////////////////////////////////////////////////////////////////////////////
// GObject signal trampolines
void MediaPluginWebKit::onLoadChanged(WebKitWebView* view, WebKitLoadEvent ev, gpointer self_ptr)
{
    MediaPluginWebKit* self = static_cast<MediaPluginWebKit*>(self_ptr);
    switch (ev)
    {
        case WEBKIT_LOAD_STARTED:
            self->setStatus(STATUS_LOADING);
            self->sendNavigateBegin();
            break;
        case WEBKIT_LOAD_COMMITTED:
        {
            const gchar* uri = webkit_web_view_get_uri(view);
            self->sendLocationChanged(uri ? uri : self->mCurrentUri);
            break;
        }
        case WEBKIT_LOAD_FINISHED:
        {
            const gchar* uri = webkit_web_view_get_uri(view);
            self->setStatus(STATUS_LOADED);
            self->sendNavigateComplete(200, uri ? uri : self->mCurrentUri);
            break;
        }
        default: break; // WEBKIT_LOAD_REDIRECTED
    }
}

gboolean MediaPluginWebKit::onLoadFailed(WebKitWebView*, WebKitLoadEvent, gchar*, GError*, gpointer self_ptr)
{
    static_cast<MediaPluginWebKit*>(self_ptr)->setStatus(STATUS_ERROR);
    return FALSE; // let WebKit show its default error page
}

void MediaPluginWebKit::onTitleChanged(GObject*, GParamSpec*, gpointer self_ptr)
{
    static_cast<MediaPluginWebKit*>(self_ptr)->sendNameText();
}

void MediaPluginWebKit::onUriChanged(GObject*, GParamSpec*, gpointer self_ptr)
{
    MediaPluginWebKit* self = static_cast<MediaPluginWebKit*>(self_ptr);
    const gchar* uri = webkit_web_view_get_uri(self->mWebView);
    if (uri) self->sendLocationChanged(uri);
}

////////////////////////////////////////////////////////////////////////////////
// Outbound helpers
void MediaPluginWebKit::sendNavigateBegin()
{
    LLPluginMessage m(LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER, "navigate_begin");
    m.setValue("uri", mCurrentUri);
    m.setValueBoolean("history_back_available", mWebView && webkit_web_view_can_go_back(mWebView));
    m.setValueBoolean("history_forward_available", mWebView && webkit_web_view_can_go_forward(mWebView));
    sendMessage(m);
}

void MediaPluginWebKit::sendNavigateComplete(int code, const std::string& uri)
{
    LLPluginMessage m(LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER, "navigate_complete");
    m.setValueS32("result_code", code);
    m.setValue("uri", uri);
    m.setValueBoolean("history_back_available", mWebView && webkit_web_view_can_go_back(mWebView));
    m.setValueBoolean("history_forward_available", mWebView && webkit_web_view_can_go_forward(mWebView));
    sendMessage(m);
}

void MediaPluginWebKit::sendLocationChanged(const std::string& uri)
{
    LLPluginMessage m(LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER, "location_changed");
    m.setValue("uri", uri);
    sendMessage(m);
}

void MediaPluginWebKit::sendNameText()
{
    const gchar* title = mWebView ? webkit_web_view_get_title(mWebView) : nullptr;
    LLPluginMessage m(LLPLUGIN_MESSAGE_CLASS_MEDIA, "name_text");
    m.setValue("name", title ? title : "");
    m.setValueBoolean("history_back_available", mWebView && webkit_web_view_can_go_back(mWebView));
    m.setValueBoolean("history_forward_available", mWebView && webkit_web_view_can_go_forward(mWebView));
    sendMessage(m);
}

////////////////////////////////////////////////////////////////////////////////
// Input synthesis (first-cut; tune focus/grab during bring-up)
void MediaPluginWebKit::mouseEvent(const std::string& event, int x, int y, int button)
{
    if (!mWebView) return;
    GdkWindow* win = gtk_widget_get_window(GTK_WIDGET(mWebView));
    if (!win) return;

    if (event == "down" || event == "up" || event == "double_click")
    {
        GdkEventType t = (event == "up") ? GDK_BUTTON_RELEASE
                         : (event == "double_click") ? GDK_2BUTTON_PRESS : GDK_BUTTON_PRESS;
        GdkEvent* e = gdk_event_new(t);
        e->button.window = GDK_WINDOW(g_object_ref(win));
        e->button.x = x; e->button.y = y;
        e->button.button = (guint)(button + 1); // GDK buttons are 1-based
        e->button.time = GDK_CURRENT_TIME;
        if (event == "down" || event == "double_click")
            gtk_widget_grab_focus(GTK_WIDGET(mWebView)); // mirror CEF setFocus()
        gtk_main_do_event(e);
        gdk_event_free(e);
    }
    else // move
    {
        GdkEvent* e = gdk_event_new(GDK_MOTION_NOTIFY);
        e->motion.window = GDK_WINDOW(g_object_ref(win));
        e->motion.x = x; e->motion.y = y;
        e->motion.time = GDK_CURRENT_TIME;
        gtk_main_do_event(e);
        gdk_event_free(e);
    }
}

void MediaPluginWebKit::scrollEvent(int x, int y, int clicks_x, int clicks_y)
{
    if (!mWebView) return;
    GdkWindow* win = gtk_widget_get_window(GTK_WIDGET(mWebView));
    if (!win) return;
    GdkEvent* e = gdk_event_new(GDK_SCROLL);
    e->scroll.window = GDK_WINDOW(g_object_ref(win));
    e->scroll.x = x; e->scroll.y = y;
    e->scroll.direction = GDK_SCROLL_SMOOTH;
    e->scroll.delta_x = clicks_x;
    e->scroll.delta_y = clicks_y;
    e->scroll.time = GDK_CURRENT_TIME;
    gtk_main_do_event(e);
    gdk_event_free(e);
}

void MediaPluginWebKit::keyEvent(const std::string& event, const LLSD& native_key_data)
{
    // TODO: map native_key_data (virtual_key/modifiers) -> GdkEventKey (keyval + hardware_keycode)
    // and dispatch GDK_KEY_PRESS/GDK_KEY_RELEASE via gtk_main_do_event().
    (void)event; (void)native_key_data;
}

void MediaPluginWebKit::textEvent(const std::string& event, const LLSD& native_key_data)
{
    // TODO: synthesize per-character GDK_KEY_PRESS events for printable unicode input.
    (void)event; (void)native_key_data;
}

////////////////////////////////////////////////////////////////////////////////
void MediaPluginWebKit::receiveMessage(const char* message_string)
{
    LLPluginMessage message_in;
    if (message_in.parse(message_string) < 0) return;

    std::string message_class = message_in.getClass();
    std::string message_name  = message_in.getName();

    if (message_class == LLPLUGIN_MESSAGE_CLASS_BASE)
    {
        if (message_name == "init")
        {
            initGTK();

            LLPluginMessage message("base", "init_response");
            LLSD versions = LLSD::emptyMap();
            versions[LLPLUGIN_MESSAGE_CLASS_BASE]          = LLPLUGIN_MESSAGE_CLASS_BASE_VERSION;
            versions[LLPLUGIN_MESSAGE_CLASS_MEDIA]         = LLPLUGIN_MESSAGE_CLASS_MEDIA_VERSION;
            versions[LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER] = LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER_VERSION;
            message.setValueLLSD("versions", versions);
            message.setValue("plugin_version", "WebKit2GTK plugin 0.1.0 (webkit2gtk-4.1)");
            sendMessage(message);
        }
        else if (message_name == "idle")
        {
            update();
        }
        else if (message_name == "cleanup")
        {
            LLPluginMessage message("base", "goodbye");
            sendMessage(message);
            mDeleteMe = true;
        }
        else if (message_name == "force_exit")
        {
            mDeleteMe = true;
        }
        else if (message_name == "shm_added")
        {
            SharedSegmentInfo info;
            info.mAddress = message_in.getValuePointer("address");
            info.mSize    = (size_t)message_in.getValueS32("size");
            std::string name = message_in.getValue("name");
            mSharedSegments.insert(SharedSegmentMap::value_type(name, info));
        }
        else if (message_name == "shm_remove")
        {
            std::string name = message_in.getValue("name");
            SharedSegmentMap::iterator iter = mSharedSegments.find(name);
            if (iter != mSharedSegments.end())
            {
                if (mPixels == iter->second.mAddress)
                {
                    mPixels = NULL;
                    mTextureSegmentName.clear();
                }
                mSharedSegments.erase(iter);
            }
            LLPluginMessage message("base", "shm_remove_response");
            message.setValue("name", name);
            sendMessage(message);
        }
    }
    else if (message_class == LLPLUGIN_MESSAGE_CLASS_MEDIA)
    {
        if (message_name == "init")
        {
            createWebView();

            mDepth = 4;
            LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "texture_params");
            message.setValueS32("default_width", 1024);
            message.setValueS32("default_height", 1024);
            message.setValueS32("depth", mDepth);
            message.setValueU32("internalformat", GL_RGB);
            message.setValueU32("format", GL_BGRA_EXT);      // cairo ARGB32 == BGRA in LE memory
            message.setValueU32("type", GL_UNSIGNED_BYTE);
            message.setValueBoolean("coords_opengl", true);  // we v-flip in blitPixels()
            sendMessage(message);
        }
        else if (message_name == "size_change")
        {
            std::string name = message_in.getValue("name");
            S32 width          = message_in.getValueS32("width");
            S32 height         = message_in.getValueS32("height");
            S32 texture_width  = message_in.getValueS32("texture_width");
            S32 texture_height = message_in.getValueS32("texture_height");

            if (!name.empty())
            {
                SharedSegmentMap::iterator iter = mSharedSegments.find(name);
                if (iter != mSharedSegments.end())
                {
                    mPixels        = (unsigned char*)iter->second.mAddress;
                    mTextureSegmentName = name;
                    mWidth         = width;
                    mHeight        = height;
                    mTextureWidth  = texture_width;
                    mTextureHeight = texture_height;
                    resizeTo(mWidth, mHeight);
                }
            }

            LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "size_change_response");
            message.setValue("name", name);
            message.setValueS32("width", width);
            message.setValueS32("height", height);
            message.setValueS32("texture_width", texture_width);
            message.setValueS32("texture_height", texture_height);
            sendMessage(message);
        }
        else if (message_name == "load_uri")
        {
            navigate(message_in.getValue("uri"));
        }
        else if (message_name == "mouse_event")
        {
            mouseEvent(message_in.getValue("event"),
                       message_in.getValueS32("x"),
                       message_in.getValueS32("y"),
                       message_in.getValueS32("button"));
        }
        else if (message_name == "scroll_event")
        {
            scrollEvent(message_in.getValueS32("x"), message_in.getValueS32("y"),
                        message_in.getValueS32("clicks_x"), message_in.getValueS32("clicks_y"));
        }
        else if (message_name == "text_event")
        {
            textEvent(message_in.getValue("event"), message_in.getValueLLSD("native_key_data"));
        }
        else if (message_name == "key_event")
        {
            keyEvent(message_in.getValue("event"), message_in.getValueLLSD("native_key_data"));
        }
        // --- deferrable stubs ---
        else if (message_name == "set_user_data_path") { /* TODO: WebKitWebsiteDataManager cache dir */ }
        else if (message_name == "set_language_code")  { /* TODO */ }
        else if (message_name == "execute_javascript") { /* TODO: webkit_web_view_run_javascript */ }
        else if (message_name == "set_cookie")         { /* TODO: WebKitCookieManager */ }
    }
    else if (message_class == LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER)
    {
        if      (message_name == "browse_stop")    { if (mWebView) webkit_web_view_stop_loading(mWebView); }
        else if (message_name == "browse_reload")  { if (mWebView) webkit_web_view_reload(mWebView); }
        else if (message_name == "browse_forward") { if (mWebView) webkit_web_view_go_forward(mWebView); }
        else if (message_name == "browse_back")    { if (mWebView) webkit_web_view_go_back(mWebView); }
        // set_page_zoom_factor, cookies, user-agent, inspector, etc. -> TODO
    }
}

////////////////////////////////////////////////////////////////////////////////
int init_media_plugin(LLPluginInstance::sendMessageFunction host_send_func,
    void* host_user_data,
    LLPluginInstance::sendMessageFunction* plugin_send_func,
    void** plugin_user_data)
{
    MediaPluginWebKit* self = new MediaPluginWebKit(host_send_func, host_user_data);
    *plugin_send_func = MediaPluginWebKit::staticReceiveMessage;
    *plugin_user_data = (void*)self;
    return 0;
}
