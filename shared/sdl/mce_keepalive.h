/*
 * mce_keepalive.h -- prevent display blanking via MCE (Aurora OS).
 *
 * Plain-C API; the implementation (mce_keepalive.c) uses GLib/GDBus
 * on the SYSTEM bus. No engine-specific types leak into this header —
 * the module is designed to be dropped into any project as-is.
 *
 * Why: during gamepad play (or any long stretch without touches) the
 * OS blanks the display right in the middle of the game. While the
 * app is in the foreground we periodically renew the MCE blanking
 * pause; on minimize or shutdown we cancel it so the screen behaves
 * normally again.
 *
 * Typical use:
 *   mce_keepalive_init();
 *   mce_keepalive_set_prevent_blanking( true );  // app in foreground
 *   ...
 *   // in the main loop, once per frame (same thread as init):
 *   mce_keepalive_pump();
 *   ...
 *   // on minimize and on shutdown:
 *   mce_keepalive_set_prevent_blanking( false );
 *   mce_keepalive_shutdown();
 */

#ifndef MCE_KEEPALIVE_H
#define MCE_KEEPALIVE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Connect to the system bus and check that MCE is present.
 * Returns false if the bus or the MCE service is unreachable — in
 * that case the module stays inert and all further calls are no-ops.
 */
bool mce_keepalive_init( void );

/* Cancel an active pause (if any) and disconnect from the bus. */
void mce_keepalive_shutdown( void );

/* True after a successful mce_keepalive_init(). */
bool mce_keepalive_is_available( void );

/*
 * Enable: send the blanking pause immediately and keep renewing it
 * in the background. Disable: stop renewing and cancel the pause.
 * Call with true when the app comes to the foreground, with false
 * when it is minimized or shutting down.
 */
void mce_keepalive_set_prevent_blanking( bool enable );
bool mce_keepalive_get_prevent_blanking( void );

/*
 * Drain the GLib main context — this drives the renewal timer.
 * MUST be called regularly from the same thread that called
 * mce_keepalive_init (typically the main loop, once per frame).
 * Non-blocking. If the project already pumps the GLib main context
 * elsewhere (e.g. a maliit client), this call can be omitted.
 */
void mce_keepalive_pump( void );

#ifdef __cplusplus
}
#endif

#endif /* MCE_KEEPALIVE_H */
