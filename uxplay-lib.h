/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified for:
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-23 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#ifndef UXPLAYLIB_H
#define UXPLAYLIB_H

#ifdef __cplusplus
extern "C" {
#endif

struct uxplay_config
{
    bool new_window_closing_behavior = false;
    char name[50] = "UxPlay";
    char videosink[50] = "autovideosink";
    char audiosink[50] = "autoaudiosink";
    char video_parser[50] = "h264parse";
    char video_decoder[50] = "decodebin";
    char video_converter[50] = "videoconvert";
    void  (*status_callback)(const char *status_string, const char *options);
};

int uxplay_start(struct uxplay_config config); 
int uxplay_stop();


#ifdef __cplusplus
}
#endif

#endif //UXPLAYLIB_H