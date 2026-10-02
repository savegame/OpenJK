/*
 * mce_keepalive.c -- GDBus implementation of the MCE blanking-pause client.
 *
 * MCE (Mode Control Entity) owns display power management on Aurora OS
 * (and Sailfish OS). It lives on the SYSTEM bus:
 *   service   com.nokia.mce
 *   object    /com/nokia/mce/request
 *   interface com.nokia.mce.request
 *   methods   req_display_blanking_pause         -- pause display blanking
 *             req_display_cancel_blanking_pause  -- cancel the pause
 *
 * The pause is time-limited (~60 s), so while prevention is enabled a
 * GLib timer re-sends req_display_blanking_pause every
 * MCE_RENEW_INTERVAL_SEC seconds. The timer fires when the default
 * GLib main context is iterated — that is what mce_keepalive_pump()
 * does once per frame.
 *
 * All calls are fire-and-forget (G_DBUS_CALL_FLAGS_NO_AUTO_START, no
 * reply awaited): the game loop must never block on DBus.
 *
 * Build: pkg-config --cflags --libs glib-2.0 gio-2.0
 */

#include "mce_keepalive.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#define MCE_LOG_TAG "mce_keepalive"
#define mce_log( ... )  g_message( MCE_LOG_TAG ": " __VA_ARGS__ )
#define mce_warn( ... ) g_warning( MCE_LOG_TAG ": " __VA_ARGS__ )

#define MCE_SERVICE        "com.nokia.mce"
#define MCE_PATH           "/com/nokia/mce/request"
#define MCE_INTERFACE      "com.nokia.mce.request"
#define MCE_PREVENT_BLANK  "req_display_blanking_pause"
#define MCE_CANCEL_PREVENT "req_display_cancel_blanking_pause"

/* The MCE blanking pause expires after 60 s — renew a bit earlier. */
#define MCE_RENEW_INTERVAL_SEC 55

static struct
{
	GDBusConnection *bus;
	guint            renew_timer_id;
	bool             prevent;
	bool             ready;
} g;

/* ------------------------------------------------------------------------- */

static void mce_send( const gchar *method )
{
	if( !g.ready || !g.bus )
		return;

	g_dbus_connection_call(
		g.bus,
		MCE_SERVICE, MCE_PATH, MCE_INTERFACE, method,
		NULL, /* no arguments */
		NULL, /* no reply type  */
		G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
		NULL, NULL, NULL ); /* fire-and-forget */
}

static gboolean mce_renew_cb( gpointer user_data )
{
	(void)user_data;
	if( g.prevent )
		mce_send( MCE_PREVENT_BLANK );
	return G_SOURCE_CONTINUE;
}

static bool mce_name_has_owner( void )
{
	GError *err = NULL;
	GVariant *reply = g_dbus_connection_call_sync(
		g.bus,
		"org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "NameHasOwner",
		g_variant_new( "(s)", MCE_SERVICE ),
		G_VARIANT_TYPE( "(b)" ),
		G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err );

	if( !reply )
	{
		mce_warn( "NameHasOwner(%s) failed: %s", MCE_SERVICE,
			err ? err->message : "?" );
		g_clear_error( &err );
		return false;
	}

	gboolean owned = FALSE;
	g_variant_get( reply, "(b)", &owned );
	g_variant_unref( reply );
	return owned ? true : false;
}

/* ------------------------------------------------------------------------- */

bool mce_keepalive_init( void )
{
	if( g.ready )
		return true;

	memset( &g, 0, sizeof( g ));

	GError *err = NULL;
	g.bus = g_bus_get_sync( G_BUS_TYPE_SYSTEM, NULL, &err );
	if( !g.bus )
	{
		mce_warn( "system bus unreachable: %s", err ? err->message : "?" );
		g_clear_error( &err );
		return false;
	}

	if( !mce_name_has_owner())
	{
		mce_warn( "MCE service %s not present on the system bus",
			MCE_SERVICE );
		g_object_unref( g.bus );
		g.bus = NULL;
		return false;
	}

	g.ready = true;
	mce_log( "connected to %s", MCE_SERVICE );
	return true;
}

void mce_keepalive_shutdown( void )
{
	if( !g.ready )
		return;

	if( g.prevent )
		mce_keepalive_set_prevent_blanking( false );

	g_object_unref( g.bus );
	memset( &g, 0, sizeof( g ));
}

bool mce_keepalive_is_available( void )
{
	return g.ready;
}

void mce_keepalive_set_prevent_blanking( bool enable )
{
	if( !g.ready || enable == g.prevent )
		return;

	g.prevent = enable;

	if( enable )
	{
		mce_send( MCE_PREVENT_BLANK );
		if( !g.renew_timer_id )
			g.renew_timer_id = g_timeout_add_seconds(
				MCE_RENEW_INTERVAL_SEC, mce_renew_cb, NULL );
	}
	else
	{
		if( g.renew_timer_id )
		{
			g_source_remove( g.renew_timer_id );
			g.renew_timer_id = 0;
		}
		mce_send( MCE_CANCEL_PREVENT );
	}
}

bool mce_keepalive_get_prevent_blanking( void )
{
	return g.prevent;
}

void mce_keepalive_pump( void )
{
	GMainContext *ctx = g_main_context_default();
	while( g_main_context_pending( ctx ))
		g_main_context_iteration( ctx, FALSE );
}
