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

   /* Escape and copy the value character-by-character */
   if (!string_is_empty(value))
   {
      const unsigned char *src = (const unsigned char *)value;
      while (*src && *pos + 3 < buf_size) /* +3: escape + char + closing " */
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
