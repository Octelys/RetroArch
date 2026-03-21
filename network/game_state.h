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
 * game_state.h – In-memory store for the game currently being played.
 *
 * Provides a thread-safe, single-instance record of the active game.
 * Call game_state_set() when a game starts and game_state_clear() when
 * it ends.  game_state_to_json() serialises the current record as a
 * JSON object suitable for sending over the WebSocket server.
 */

#ifndef __RARCH_GAME_STATE_H
#define __RARCH_GAME_STATE_H

#include <stddef.h>
#include <boolean.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum lengths for each field (including NUL terminator). */
#define GAME_STATE_GAME_ID_LEN      64
#define GAME_STATE_GAME_NAME_LEN    512
#define GAME_STATE_GAME_PATH_LEN    4096
#define GAME_STATE_CONSOLE_ID_LEN   64
#define GAME_STATE_CONSOLE_NAME_LEN 256
#define GAME_STATE_CORE_NAME_LEN    256
#define GAME_STATE_DB_NAME_LEN      512

/**
 * ra_game_state_t:
 *
 * Describes the game that is currently loaded in RetroArch.
 *
 *   game_id       – CRC-32 checksum of the ROM as a hex string (may be
 *                   empty if unknown).
 *   game_name     – Base filename of the ROM without extension.
 *   game_path     – Full filesystem path to the ROM.
 *   console_id    – Short system/platform identifier supplied by the
 *                   core info database (e.g. "snes", "megadrive").
 *   console_name  – Human-readable platform name (e.g.
 *                   "Super Nintendo Entertainment System").
 *   core_name     – Name of the libretro core that is running the game.
 *   db_name       – Playlist/database name associated with the content.
 */
typedef struct
{
   char game_id      [GAME_STATE_GAME_ID_LEN];
   char game_name    [GAME_STATE_GAME_NAME_LEN];
   char game_path    [GAME_STATE_GAME_PATH_LEN];
   char console_id   [GAME_STATE_CONSOLE_ID_LEN];
   char console_name [GAME_STATE_CONSOLE_NAME_LEN];
   char core_name    [GAME_STATE_CORE_NAME_LEN];
   char db_name      [GAME_STATE_DB_NAME_LEN];
} ra_game_state_t;

/**
 * game_state_init:
 *
 * Initialises internal resources (mutex).  Must be called once before
 * any other game_state_* function.  Safe to call multiple times.
 */
void game_state_init(void);

/**
 * game_state_deinit:
 *
 * Releases internal resources.  After this call game_state_init() must
 * be called again before the API is used.
 */
void game_state_deinit(void);

/**
 * game_state_set:
 * @state : pointer to the state to store (copied internally).
 *
 * Records @state as the active game.  Thread-safe.
 */
void game_state_set(const ra_game_state_t *state);

/**
 * game_state_clear:
 *
 * Marks the current state as "no game running".  Thread-safe.
 */
void game_state_clear(void);

/**
 * game_state_is_running:
 *
 * Returns true when a game is currently set, false otherwise.
 * Thread-safe.
 */
bool game_state_is_running(void);

/**
 * game_state_get:
 * @out : destination struct to fill (must not be NULL).
 *
 * Copies the current state into @out.
 * Returns true when a game is active, false when no game is set (in
 * which case @out is not modified).  Thread-safe.
 */
bool game_state_get(ra_game_state_t *out);

/**
 * game_state_to_json:
 * @buf      : destination buffer.
 * @buf_size : total size of @buf in bytes.
 *
 * Serialises the current state as a JSON object into @buf.
 *
 * When a game is running the object has the shape:
 *   { "type":"game_playing",
 *     "game_id":"...", "game_name":"...", "game_path":"...",
 *     "console_id":"...", "console_name":"...",
 *     "core_name":"...", "db_name":"..." }
 *
 * When no game is running:
 *   { "type":"no_game" }
 *
 * Returns the number of bytes written to @buf (excluding the NUL
 * terminator), or 0 on error.  Thread-safe.
 */
size_t game_state_to_json(char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif

#endif /* __RARCH_GAME_STATE_H */
