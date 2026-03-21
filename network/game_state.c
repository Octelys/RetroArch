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
 * game_state.c – Thread-safe in-memory store for the currently active game.
 *
 * The module holds a single ra_game_state_t record plus a boolean flag
 * that indicates whether a game is running.  All public functions
 * serialise access through a mutex so they may be called from any thread.
 */

#include "game_state.h"

#include <rthreads/rthreads.h>
#include <compat/strl.h>
#include <string/stdstring.h>

#include <string.h>
#include <stdio.h>

#ifdef HAVE_CHEEVOS
#include "../deps/rcheevos/include/rc_client.h"
#include "../deps/rcheevos/include/rc_consoles.h"
#include "ws_server.h"
#endif

/* -------------------------------------------------------------------------
 * Internal state
 * ---------------------------------------------------------------------- */

static slock_t       *g_state_lock = NULL;
static ra_game_state_t g_state;
static bool            g_is_running = false;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

void game_state_init(void)
{
   if (g_state_lock)
      return; /* already initialised */
   g_state_lock = slock_new();
   memset(&g_state, 0, sizeof(g_state));
   g_is_running = false;
}

void game_state_deinit(void)
{
   if (!g_state_lock)
      return;
   slock_free(g_state_lock);
   g_state_lock = NULL;
   g_is_running = false;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

/**
 * json_append_field:
 * @buf       : destination buffer.
 * @pos       : current write offset; updated on return.
 * @buf_size  : total size of @buf.
 * @key       : JSON key (must not contain characters needing escaping).
 * @value     : value string to escape and append.
 *
 * Appends   ,"key":"escaped-value"   to @buf starting at *pos.
 * Updates *pos.  Silently truncates if the buffer is too small.
 */
static void json_append_field(char *buf, size_t *pos,
      size_t buf_size, const char *key, const char *value)
{
   /* Write the key prefix */
   int n = snprintf(buf + *pos, buf_size - *pos, ",\"%s\":\"", key);
   if (n > 0)
      *pos += (size_t)n;

   /* Escape and copy the value character-by-character.
    * Reserve 2 bytes for a potential 2-byte escape sequence ('\\' + char);
    * the closing '"' is appended separately after the loop. */
   if (!string_is_empty(value))
   {
      const unsigned char *src = (const unsigned char *)value;
      while (*src && *pos + 2 < buf_size)
      {
         unsigned char c = *src++;
         if (c == '"' || c == '\\')
         {
            buf[(*pos)++] = '\\';
            buf[(*pos)++] = (char)c;
         }
         else if (c >= 0x20) /* skip bare control characters */
            buf[(*pos)++] = (char)c;
      }
   }

   /* Close the quoted value */
   if (*pos + 1 < buf_size)
      buf[(*pos)++] = '"';
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void game_state_set(const ra_game_state_t *state)
{
   if (!state || !g_state_lock)
      return;
   slock_lock(g_state_lock);
   g_state      = *state;
   g_is_running = true;
   slock_unlock(g_state_lock);
}

void game_state_clear(void)
{
   if (!g_state_lock)
      return;
   slock_lock(g_state_lock);
   memset(&g_state, 0, sizeof(g_state));
   g_is_running = false;
   slock_unlock(g_state_lock);
}

bool game_state_is_running(void)
{
   bool running;
   if (!g_state_lock)
      return false;
   slock_lock(g_state_lock);
   running = g_is_running;
   slock_unlock(g_state_lock);
   return running;
}

bool game_state_get(ra_game_state_t *out)
{
   bool running;
   if (!out || !g_state_lock)
      return false;
   slock_lock(g_state_lock);
   running = g_is_running;
   if (running)
      *out = g_state;
   slock_unlock(g_state_lock);
   return running;
}

size_t game_state_to_json(char *buf, size_t buf_size)
{
   ra_game_state_t snap;
   bool            running;
   size_t          pos = 0;
   int             n;

   if (!buf || buf_size < 2)
      return 0;

   memset(&snap, 0, sizeof(snap));

   if (g_state_lock)
   {
      slock_lock(g_state_lock);
      running = g_is_running;
      if (running)
         snap = g_state;
      slock_unlock(g_state_lock);
   }
   else
   {
      running = false;
   }

   if (!running)
   {
      n = snprintf(buf, buf_size, "{\"type\":\"no_game\"}");
      return (n > 0) ? (size_t)n : 0;
   }

   /* Open object and write the type field */
   n = snprintf(buf, buf_size, "{\"type\":\"game_playing\"");
   if (n <= 0)
      return 0;
   pos = (size_t)n;

   json_append_field(buf, &pos, buf_size, "game_id",      snap.game_id);
   json_append_field(buf, &pos, buf_size, "game_name",    snap.game_name);
   json_append_field(buf, &pos, buf_size, "game_path",    snap.game_path);
   json_append_field(buf, &pos, buf_size, "console_id",   snap.console_id);
   json_append_field(buf, &pos, buf_size, "console_name", snap.console_name);
   json_append_field(buf, &pos, buf_size, "core_name",    snap.core_name);
   json_append_field(buf, &pos, buf_size, "db_name",      snap.db_name);

   /* Close object */
   if (pos + 1 < buf_size)
   {
      buf[pos++] = '}';
      buf[pos]   = '\0';
   }

   return pos;
}

#ifdef HAVE_CHEEVOS
/**
 * game_state_update_from_cheevos:
 *
 * Sole entry-point for populating and broadcasting the WebSocket game
 * state.  Called from rcheevos_client_load_game_callback() once the
 * async RetroAchievements lookup has completed.  Builds a fresh
 * ra_game_state_t entirely from RA data + the ROM path:
 *
 *   id         → game_id      (authoritative numeric RA game ID)
 *   title      → game_name    (RA-canonical title)
 *   console_id → console_name (human-readable via rc_console_name())
 *   game_path  → game_path    (full filesystem path to the ROM)
 */
void game_state_update_from_cheevos(const rc_client_game_t *game,
      const char *game_path)
{
   ra_game_state_t state;

   if (!game || game->id == 0)
      return;

   memset(&state, 0, sizeof(state));

   /* Authoritative numeric RA game ID */
   snprintf(state.game_id, sizeof(state.game_id), "%u", (unsigned)game->id);

   /* RA-canonical game title */
   if (!string_is_empty(game->title))
      strlcpy(state.game_name, game->title, sizeof(state.game_name));

   /* Full filesystem path to the ROM */
   if (!string_is_empty(game_path))
      strlcpy(state.game_path, game_path, sizeof(state.game_path));

   /* Human-readable console name */
   if (game->console_id != 0)
   {
      const char *con = rc_console_name(game->console_id);
      if (!string_is_empty(con))
         strlcpy(state.console_name, con, sizeof(state.console_name));
   }

   game_state_set(&state);
   ws_server_notify_game_changed();
}
#endif

