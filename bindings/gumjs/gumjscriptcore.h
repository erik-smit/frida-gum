/*
 * Copyright (C) 2015 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#ifndef __GUM_JSCRIPT_CORE_H__
#define __GUM_JSCRIPT_CORE_H__

#include "gumscript.h"
#include "gumscriptscheduler.h"

#include <gum/gumexceptor.h>
#include <JavaScriptCore/JavaScriptCore.h>

typedef struct _GumScriptCore GumScriptCore;

typedef void (* GumScriptCoreMessageEmitter) (GumScript * script,
    const gchar * message, GBytes * data);

struct _GumScriptCore
{
  GumScript * script;
  GumScriptCoreMessageEmitter message_emitter;
  GumScriptScheduler * scheduler;
  GumExceptor * exceptor;
  JSContextRef ctx;

  JSClassRef native_pointer;
};

G_GNUC_INTERNAL void _gum_script_core_init (GumScriptCore * self,
    GumScript * script, GumScriptCoreMessageEmitter message_emitter,
    GumScriptScheduler * scheduler, JSContextRef ctx, JSObjectRef scope);
G_GNUC_INTERNAL void _gum_script_core_realize (GumScriptCore * self);
G_GNUC_INTERNAL void _gum_script_core_flush (GumScriptCore * self);
G_GNUC_INTERNAL void _gum_script_core_dispose (GumScriptCore * self);
G_GNUC_INTERNAL void _gum_script_core_finalize (GumScriptCore * self);

G_GNUC_INTERNAL void _gum_script_core_emit_message (GumScriptCore * self,
    const gchar * message, GBytes * data);
G_GNUC_INTERNAL void _gum_script_core_post_message (GumScriptCore * self,
    const gchar * message);

G_GNUC_INTERNAL gchar * _gum_script_string_get (JSStringRef str);
G_GNUC_INTERNAL gchar * _gum_script_string_from_value (JSValueRef value,
    JSContextRef ctx);

G_GNUC_INTERNAL guint _gum_script_object_get_uint (JSObjectRef object,
    const gchar * key, JSContextRef ctx);
G_GNUC_INTERNAL gchar * _gum_script_object_get_string (JSObjectRef object,
    const gchar * key, JSContextRef ctx);
G_GNUC_INTERNAL void _gum_script_object_set (JSObjectRef object,
    const gchar * key, JSValueRef value, JSContextRef ctx);
G_GNUC_INTERNAL void _gum_script_object_set_callback (JSObjectRef object,
    const gchar * key, JSObjectCallAsFunctionCallback callback, gpointer data,
    JSContextRef ctx);

G_GNUC_INTERNAL void _gum_script_throw (JSValueRef * exception,
    JSContextRef ctx, const gchar * format, ...);
G_GNUC_INTERNAL void _gum_script_panic (JSValueRef exception, JSContextRef ctx);

#endif