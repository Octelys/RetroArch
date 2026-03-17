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
 * A single libwebsockets "retroarch" protocol is registered; derived projects
 * can extend the callback to add application-level message handling.
 *
 * Platform notes:
 *   Linux  : link with -lwebsockets (or use pkg-config libwebsockets).
 *   macOS  : link with -lwebsockets (Homebrew: brew install libwebsockets).
 *   Windows: place libwebsockets headers/import-lib under
 *            deps/libwebsockets/{x64,arm64}/ and link with websockets.lib
 *            (see deps/libwebsockets/README.md).
 */

#include "ws_server.h"

#include <libwebsockets.h>
#include <rthreads/rthreads.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static struct lws_context *g_lws_ctx    = NULL;
static sthread_t           *g_thread    = NULL;
static slock_t             *g_lock      = NULL;
static bool                 g_running   = false;

/* -------------------------------------------------------------------------
 * Protocol callback
 * ---------------------------------------------------------------------- */

static int callback_retroarch(struct lws *wsi,
      enum lws_callback_reasons reason,
      void *user, void *in, size_t len)
{
   (void)user;

   switch (reason)
   {
      case LWS_CALLBACK_ESTABLISHED:
         /* A new client has connected. */
         break;

      case LWS_CALLBACK_RECEIVE:
         /* Echo the received data back to the sender as a demonstration.
          * Replace this block with real message-handling logic as needed. */
         if (in && len > 0)
         {
            /* LWS_PRE bytes of padding are required before the payload. */
            unsigned char *buf = (unsigned char *)malloc(LWS_PRE + len);
            if (buf)
            {
               memcpy(buf + LWS_PRE, in, len);
               lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
               free(buf);
            }
            else
               fprintf(stderr, "[ws_server] malloc failed for echo buffer "
                               "(%lu bytes).\n", (unsigned long)(LWS_PRE + len));
         }
         break;

      case LWS_CALLBACK_CLOSED:
         /* Client disconnected. */
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
      WS_RX_BUFFER_BYTES,   /* rx buffer size */
      0, NULL, 0
   },
   LWS_PROTOCOL_LIST_TERM
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

      slock_lock(g_lock);
      running = g_running;
      slock_unlock(g_lock);

      if (!running)
         break;

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
   g_running = true;
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
