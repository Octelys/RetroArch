/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2024 - libretro team
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 *  more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with RetroArch. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * ws_server.c – Minimal WebSocket server for RetroArch using libwebsockets.
 *
 * The server binds exclusively to 127.0.0.1 on the caller-supplied port so
 * that it is only reachable from the local machine.  A dedicated background
 * thread owns the libwebsockets service loop, keeping it fully decoupled from
 * frame processing.
 *
 * Game-state messaging
 * --------------------
 * When a new client connects the server immediately sends it the current game
 * state JSON (see game_state.h).  When the active game changes the caller
 * invokes ws_server_notify_game_changed(); the server thread then broadcasts
 * the updated state to every connected client.
 *
 * Broadcast design
 * ----------------
 * ws_server_notify_game_changed() is safe to call from any thread.  It sets
 * a flag under the existing g_lock and wakes up the service thread via
 * lws_cancel_service().  The service thread checks the flag after each
 * lws_service() call and, if set, calls lws_callback_on_writable_all_protocol()
 * to schedule a LWS_CALLBACK_SERVER_WRITEABLE event for every connected
 * client.  The actual JSON write happens inside that callback, keeping all
 * libwebsockets I/O on the service thread.
 *
 * Platform notes:
 *   Linux  : link with -lwebsockets (or use pkg-config libwebsockets).
 *   macOS  : link with -lwebsockets (Homebrew: brew install libwebsockets).
 *   Windows: place libwebsockets headers/import-lib under
 *            deps/libwebsockets/{x64,arm64}/ and link with websockets.lib
 *            (see deps/libwebsockets/README.md).
 */

#include "ws_server.h"
#include "game_state.h"

#include <libwebsockets.h>
#include <rthreads/rthreads.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Some older libwebsockets versions (< 2.x / < 4.x) do not define all of the
 * option macros we use.  Provide safe no-op fall-backs so that ws_server.c
 * compiles against those versions as well. */
#ifndef LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT
#define LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT 0
#endif
#ifndef LWS_SERVER_OPTION_DISABLE_IPV6
#define LWS_SERVER_OPTION_DISABLE_IPV6 0
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------------- */

/* Maximum inbound frame payload the server will buffer per connection.
 * 4 KiB is sufficient for typical control messages; increase if larger
 * payloads are expected. */
#define WS_RX_BUFFER_BYTES 4096

/* Timeout (ms) passed to lws_service() each iteration.  A short value keeps
 * the thread responsive to stop requests without busy-spinning. */
#define WS_SERVICE_TIMEOUT_MS 10

/* Maximum JSON payload size (bytes) for a game-state message.
 * game_path alone can be up to 4096 chars; add room for all other fields
 * plus JSON syntax overhead. */
#define WS_MSG_MAX_BYTES 8192

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static struct lws_context *g_lws_ctx          = NULL;
static sthread_t           *g_thread          = NULL;
static slock_t             *g_lock            = NULL;
static bool                 g_running         = false;
static bool                 g_broadcast_pending = false;

/* -------------------------------------------------------------------------
 * Helper: write the current game state to a single client
 * ---------------------------------------------------------------------- */

/**
 * ws_write_game_state:
 * @wsi : the WebSocket connection to write to.
 *
 * Serialises the current game state as JSON and sends it to @wsi.
 * Must be called from within the libwebsockets service thread
 * (i.e. from a LWS_CALLBACK_SERVER_WRITEABLE handler).
 */
static void ws_write_game_state(struct lws *wsi)
{
   /* libwebsockets requires LWS_PRE bytes of padding before the payload. */
   unsigned char buf[LWS_PRE + WS_MSG_MAX_BYTES];
   size_t        len;

   len = game_state_to_json((char *)(buf + LWS_PRE), WS_MSG_MAX_BYTES);
   if (len == 0)
      return;

   lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
}

/* -------------------------------------------------------------------------
 * Protocol callback
 * ---------------------------------------------------------------------- */

static int callback_retroarch(struct lws *wsi,
      enum lws_callback_reasons reason,
      void *user, void *in, size_t len)
{
   (void)user;
   (void)in;
   (void)len;

   switch (reason)
   {
      case LWS_CALLBACK_ESTABLISHED:
         /* A new client has connected.  Schedule an immediate write so it
          * receives the current game state without waiting for a broadcast. */
         lws_callback_on_writable(wsi);
         break;

      case LWS_CALLBACK_SERVER_WRITEABLE:
         /* Send the current game state to this client. */
         ws_write_game_state(wsi);
         break;

      case LWS_CALLBACK_CLOSED:
         /* Client disconnected – nothing to clean up. */
         break;

      default:
         break;
   }

   return 0;
}

/* -------------------------------------------------------------------------
 * Protocol table
 * ---------------------------------------------------------------------- */

static struct lws_protocols g_protocols[] = {
   {
      "retroarch",          /* protocol name */
      callback_retroarch,   /* callback */
      0,                    /* per-session data size */
      WS_RX_BUFFER_BYTES    /* rx buffer size */
   },
   { NULL, NULL, 0, 0 }    /* terminator – compatible with all lws versions */
};

/* -------------------------------------------------------------------------
 * Background service thread
 * ---------------------------------------------------------------------- */

static void ws_server_thread(void *userdata)
{
   (void)userdata;

   for (;;)
   {
      bool running;
      bool broadcast;

      slock_lock(g_lock);
      running   = g_running;
      broadcast = g_broadcast_pending;
      if (broadcast)
         g_broadcast_pending = false;
      slock_unlock(g_lock);

      if (!running)
         break;

      /* If a broadcast was requested, schedule a writeable callback for
       * every connected client before servicing events.  This call is
       * safe here because we are on the service thread. */
      if (broadcast)
         lws_callback_on_writable_all_protocol(g_lws_ctx, &g_protocols[0]);

      /* lws_service() blocks for at most WS_SERVICE_TIMEOUT_MS milliseconds,
       * then returns so we can re-check the stop flag. */
      lws_service(g_lws_ctx, WS_SERVICE_TIMEOUT_MS);
   }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

bool ws_server_init(unsigned port)
{
   struct lws_context_creation_info info;

   if (g_lws_ctx)
      return true; /* already running */

   memset(&info, 0, sizeof(info));

   info.port      = (int)port;
   info.iface     = "127.0.0.1"; /* loopback only */
   info.protocols = g_protocols;
   info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT
                  | LWS_SERVER_OPTION_DISABLE_IPV6;
   info.gid       = -1;
   info.uid       = -1;

   g_lws_ctx = lws_create_context(&info);
   if (!g_lws_ctx)
   {
      fprintf(stderr, "[ws_server] Failed to create libwebsockets context "
                      "(port %u).\n", port);
      return false;
   }

   g_lock = slock_new();
   if (!g_lock)
   {
      fprintf(stderr, "[ws_server] Failed to create mutex.\n");
      lws_context_destroy(g_lws_ctx);
      g_lws_ctx = NULL;
      return false;
   }

   slock_lock(g_lock);
   g_running          = true;
   g_broadcast_pending = false;
   slock_unlock(g_lock);

   g_thread = sthread_create(ws_server_thread, NULL);
   if (!g_thread)
   {
      fprintf(stderr, "[ws_server] Failed to create service thread.\n");
      slock_lock(g_lock);
      g_running = false;
      slock_unlock(g_lock);
      slock_free(g_lock);
      g_lock    = NULL;
      lws_context_destroy(g_lws_ctx);
      g_lws_ctx = NULL;
      return false;
   }

   fprintf(stderr, "[ws_server] Listening on 127.0.0.1:%u\n", port);
   return true;
}

void ws_server_destroy(void)
{
   if (!g_lws_ctx)
      return;

   /* Signal the service thread to stop. */
   if (g_lock)
   {
      slock_lock(g_lock);
      g_running = false;
      slock_unlock(g_lock);
   }

   /* Wake libwebsockets so the thread exits lws_service() promptly. */
   lws_cancel_service(g_lws_ctx);

   /* Wait for the thread to finish. */
   if (g_thread)
   {
      sthread_join(g_thread);
      g_thread = NULL;
   }

   if (g_lock)
   {
      slock_free(g_lock);
      g_lock = NULL;
   }

   lws_context_destroy(g_lws_ctx);
   g_lws_ctx = NULL;
}

void ws_server_notify_game_changed(void)
{
   if (!g_lws_ctx || !g_lock)
      return;

   /* Set the broadcast flag under the lock so the service thread picks it
    * up safely, then wake the service loop. */
   slock_lock(g_lock);
   g_broadcast_pending = true;
   slock_unlock(g_lock);

   lws_cancel_service(g_lws_ctx);
}
