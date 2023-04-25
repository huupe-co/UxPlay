/**
 * RPiPlay - An open-source AirPlay mirroring server for Raspberry Pi
 * Copyright (C) 2019 Florian Draschbacher
 * Modified extensively to become 
 * UxPlay - An open-souce AirPlay mirroring server.
 * Modifications Copyright (C) 2021-23 F. Duncanh
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

#include <stddef.h>
#include <cstring>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <string>
#include <vector>
#include <fstream>
#include <atomic>

#ifdef _WIN32  /*modifications for Windows compilation */
#include <glib.h>
#include <unordered_map>
#include <winsock2.h>
#include <iphlpapi.h>
#else
#include <glib-unix.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <ifaddrs.h>
# ifdef __linux__
# include <netpacket/packet.h>
# else
# include <net/if_dl.h>
# endif
#endif

#include "log.h"
#include "lib/raop.h"
#include "lib/stream.h"
#include "lib/logger.h"
#include "lib/dnssd.h"
#include "renderers/video_renderer.h"
#include "renderers/audio_renderer.h"
#include "uxplay-lib.h"

#define VERSION "1.63"

#define SECOND_IN_USECS 1000000
#define SECOND_IN_NSECS 1000000000UL
#define DEFAULT_NAME "UxPlay"
#define DEFAULT_DEBUG_LOG true
#define LOWEST_ALLOWED_PORT 1024
#define HIGHEST_PORT 65535
#define NTP_TIMEOUT_LIMIT 5
#define BT709_FIX "capssetter caps=\"video/x-h264, colorimetry=bt709\""

static std::string server_name = DEFAULT_NAME;
static dnssd_t *dnssd = NULL;
static raop_t *raop = NULL;
static logger_t *render_logger = NULL;
static bool audio_sync = false;
static bool video_sync = false;
static int64_t audio_delay_alac = 0;
static int64_t audio_delay_aac = 0;
static bool relaunch_video = false;
static bool reset_loop = false;
static unsigned int open_connections= 0;
static std::string videosink = "autovideosink";
static videoflip_t videoflip[2] = { NONE , NONE };
static bool use_video = true;
static unsigned char compression_type = 0;
static std::string audiosink = "autoaudiosink";
static int  audiodelay = -1;
static bool use_audio = true;
static bool new_window_closing_behavior = true;
static bool close_window;
static std::string video_parser = "h264parse";
static std::string video_decoder = "decodebin";
static std::string video_converter = "videoconvert";
static bool show_client_FPS_data = false;
static unsigned int max_ntp_timeouts = NTP_TIMEOUT_LIMIT;
static FILE *video_dumpfile = NULL;
static std::string video_dumpfile_name = "videodump";
static int video_dump_limit = 0;
static int video_dumpfile_count = 0;
static int video_dump_count = 0;
static bool dump_video = false;
static unsigned char mark[] = { 0x00, 0x00, 0x00, 0x01 };
static FILE *audio_dumpfile = NULL;
static std::string audio_dumpfile_name = "audiodump";
static int audio_dump_limit = 0;
static int audio_dumpfile_count = 0;
static int audio_dump_count = 0;
static bool dump_audio = false;
static unsigned char audio_type = 0x00;
static unsigned char previous_audio_type = 0x00;
static bool fullscreen = false;
static std::string coverart_filename = "";
static bool do_append_hostname = true;
static bool use_random_hw_addr = false;
static unsigned short display[5] = {0}, tcp[3] = {0}, udp[3] = {0};
static bool debug_log = DEFAULT_DEBUG_LOG;
static bool bt709_fix = false;
static int max_connections = 2;
static unsigned short raop_port;
static unsigned short airplay_port;
static uint64_t remote_clock_offset = 0;
static struct uxplay_config app_config;
static std::atomic_bool uxplay_stop_flag;

/* 95 byte png file with a 1x1 white square (single pixel): placeholder for coverart*/
static const unsigned char empty_image[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,  0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,  0x01, 0x03, 0x00, 0x00, 0x00, 0x25, 0xdb, 0x56,
    0xca, 0x00, 0x00, 0x00, 0x03, 0x50, 0x4c, 0x54,  0x45, 0x00, 0x00, 0x00, 0xa7, 0x7a, 0x3d, 0xda,
    0x00, 0x00, 0x00, 0x01, 0x74, 0x52, 0x4e, 0x53,  0x00, 0x40, 0xe6, 0xd8, 0x66, 0x00, 0x00, 0x00,
    0x0a, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63,  0x60, 0x00, 0x00, 0x00, 0x02, 0x00, 0x01, 0xe2,
    0x21, 0xbc, 0x33, 0x00, 0x00, 0x00, 0x00, 0x49,  0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

size_t write_coverart(const char *filename, const void *image, size_t len) {
    FILE *fp = fopen(filename, "wb");
    size_t count = fwrite(image, 1, len, fp);
    fclose(fp);
    return count;
}

static void update_status(const char *status_string, const char *options) {
    if (!app_config.status_callback) {
        return;
    }
    app_config.status_callback(status_string, options);
}  

static void dump_audio_to_file(unsigned char *data, int datalen, unsigned char type) {
    if (!audio_dumpfile && audio_type != previous_audio_type) {
        char suffix[20];
        std::string fn = audio_dumpfile_name;
        previous_audio_type = audio_type;
        audio_dumpfile_count++;
        audio_dump_count = 0;
        /* type 0x20 is lossless ALAC, type 0x80 is compressed AAC-ELD, type 0x10 is "other" */
        if (audio_type == 0x20) {
            snprintf(suffix, sizeof(suffix), ".%d.alac", audio_dumpfile_count);
        } else if (audio_type == 0x80) {
            snprintf(suffix, sizeof(suffix), ".%d.aac", audio_dumpfile_count);
        } else {
            snprintf(suffix, sizeof(suffix), ".%d.aud", audio_dumpfile_count);
        }
        fn.append(suffix);
        audio_dumpfile = fopen(fn.c_str(),"w");
        if (audio_dumpfile == NULL) {
            LOGE("could not open file %s for dumping audio frames",fn.c_str());
        }
    }

    if (audio_dumpfile) {
        fwrite(data, 1, datalen, audio_dumpfile);
        if (audio_dump_limit) {
            audio_dump_count++;
            if (audio_dump_count == audio_dump_limit) {
                fclose(audio_dumpfile);
                audio_dumpfile = NULL;
            }          
        }
    }
}

static void dump_video_to_file(unsigned char *data, int datalen) {
    /*  SPS NAL has (data[4] & 0x1f) = 0x07  */
    if ((data[4] & 0x1f) == 0x07  && video_dumpfile && video_dump_limit) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
        video_dumpfile = NULL;
        video_dump_count = 0;                     
    }

    if (!video_dumpfile) {
        std::string fn = video_dumpfile_name;
        if (video_dump_limit) {
            char suffix[20];
            video_dumpfile_count++;
            snprintf(suffix, sizeof(suffix), ".%d", video_dumpfile_count);
            fn.append(suffix);
	}
        fn.append(".h264");
        video_dumpfile = fopen (fn.c_str(),"w");
        if (video_dumpfile == NULL) {
            LOGE("could not open file %s for dumping h264 frames",fn.c_str());
        }
    }

    if (video_dumpfile) {
        if (video_dump_limit == 0) {
            fwrite(data, 1, datalen, video_dumpfile);
        } else if (video_dump_count < video_dump_limit) {
            video_dump_count++;
            fwrite(data, 1, datalen, video_dumpfile);
        }
    }
}

static gboolean reset_callback(gpointer loop) {
    if (reset_loop) {
        g_main_loop_quit((GMainLoop *) loop);
    }
    return TRUE;
}

static gboolean  sigint_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

static gboolean  sigterm_callback(gpointer loop) {
    relaunch_video = false;
    g_main_loop_quit((GMainLoop *) loop);
    return TRUE;
}

#ifdef _WIN32
struct signal_handler {
    GSourceFunc handler;
    gpointer user_data;
};

static std::unordered_map<gint, signal_handler> u = {};

void SignalHandler(int signum) {
    if (signum == SIGTERM || signum == SIGINT) {
        u[signum].handler(u[signum].user_data);
    }
}

guint g_unix_signal_add(gint signum, GSourceFunc handler, gpointer user_data) {
    u[signum] = signal_handler{handler, user_data};
    (void) signal(signum, SignalHandler);
    return 0;
}
#endif

static void main_loop()  {
    // guint connection_watch_id = 0;
    // guint gst_bus_watch_id = 0;
    // GMainLoop *loop = g_main_loop_new(NULL,FALSE);
    // relaunch_video = false;
    // if (use_video) {
    //     relaunch_video = true;
    //     gst_bus_watch_id = (guint) video_renderer_listen((void *)loop);
    // }
    // guint reset_watch_id = g_timeout_add(100, (GSourceFunc) reset_callback, (gpointer) loop);
    // guint sigterm_watch_id = g_unix_signal_add(SIGTERM, (GSourceFunc) sigterm_callback, (gpointer) loop);
    // guint sigint_watch_id = g_unix_signal_add(SIGINT, (GSourceFunc) sigint_callback, (gpointer) loop);
    // g_main_loop_run(loop);

    // if (gst_bus_watch_id > 0) g_source_remove(gst_bus_watch_id);
    // if (sigint_watch_id > 0) g_source_remove(sigint_watch_id);
    // if (sigterm_watch_id > 0) g_source_remove(sigterm_watch_id);
    // if (reset_watch_id > 0) g_source_remove(reset_watch_id);
    // g_main_loop_unref(loop);
    while (!uxplay_stop_flag)
    {
        usleep(100);
    }
}    

static int parse_hw_addr (std::string str, std::vector<char> &hw_addr) {
    for (int i = 0; i < str.length(); i += 3) {
        hw_addr.push_back((char) stol(str.substr(i), NULL, 16));
    }
    return 0;
}

static std::string find_mac () {
/*  finds the MAC address of a network interface *
 *  in a Windows, Linux, *BSD or macOS system.   */
    std::string mac = "";
    char str[3];
#ifdef _WIN32
    ULONG buflen = sizeof(IP_ADAPTER_ADDRESSES);
    PIP_ADAPTER_ADDRESSES addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
    if (addresses == NULL) { 					
        return mac;
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == ERROR_BUFFER_OVERFLOW) {
        free(addresses);
        addresses = (IP_ADAPTER_ADDRESSES*) malloc(buflen);
        if (addresses == NULL) {
            return mac;
        }
    }
    if (GetAdaptersAddresses(AF_UNSPEC, 0, NULL, addresses, &buflen) == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES address = addresses; address != NULL; address = address->Next) {
            if (address->PhysicalAddressLength != 6                 /* MAC has 6 octets */
                || (address->IfType != 6 && address->IfType != 71)  /* Ethernet or Wireless interface */
                || address->OperStatus != 1) {                      /* interface is up */
                continue;
            }
            mac.erase();
            for (int i = 0; i < 6; i++) {
                sprintf(str,"%02x", int(address->PhysicalAddress[i]));
                mac = mac + str;
                if (i < 5) mac = mac + ":";
            }
	    break;
        }
    }
    free(addresses);
    return mac;
#else
    struct ifaddrs *ifap, *ifaptr;
    int non_null_octets = 0;
    unsigned char octet[6], *ptr;
    if (getifaddrs(&ifap) == 0) {
        for(ifaptr = ifap; ifaptr != NULL; ifaptr = ifaptr->ifa_next) {
            if(ifaptr->ifa_addr == NULL) continue;
#ifdef __linux__
            if (ifaptr->ifa_addr->sa_family != AF_PACKET) continue;
            struct sockaddr_ll *s = (struct sockaddr_ll*) ifaptr->ifa_addr;
            for (int i = 0; i < 6; i++) {
                if ((octet[i] = s->sll_addr[i]) != 0) non_null_octets++;
            }
#else    /* macOS and *BSD */
            if (ifaptr->ifa_addr->sa_family != AF_LINK) continue;
            ptr = (unsigned char *) LLADDR((struct sockaddr_dl *) ifaptr->ifa_addr);
            for (int i= 0; i < 6 ; i++) {
                if ((octet[i] = *ptr) != 0) non_null_octets++;
                ptr++;
            }
#endif
            if (non_null_octets) {
                mac.erase();
                for (int i = 0; i < 6 ; i++) {
                    sprintf(str,"%02x", octet[i]);
                    mac = mac + str;
                    if (i < 5) mac = mac + ":";
                }
                break;
            }
        }
    }
    freeifaddrs(ifap);
#endif
    return mac;
}

#define MULTICAST 0
#define LOCAL 1
#define OCTETS 6
static std::string random_mac () {
    char str[3];
    int octet = rand() % 64;
    octet = (octet << 1) + LOCAL;
    octet = (octet << 1) + MULTICAST;
    snprintf(str,3,"%02x",octet);
    std::string mac_address(str);
    for (int i = 1; i < OCTETS; i++) {
        mac_address = mac_address + ":";
        octet =  rand() % 256;
        snprintf(str,3,"%02x",octet);
        mac_address = mac_address + str;
    }
    return mac_address;
}

static bool get_display_settings (std::string value, unsigned short *w, unsigned short *h, unsigned short *r) {
    // assume str  = wxh@r is valid if w and h are positive decimal integers
    // with no more than 4 digits, r < 256 (stored in one byte).
    char *end;
    std::size_t pos = value.find_first_of("x");
    if (pos == std::string::npos) return false;
    std::string str1 = value.substr(pos+1);
    value.erase(pos);
    if (value.length() == 0 || value.length() > 4 || value[0] == '-') return false;
    *w = (unsigned short) strtoul(value.c_str(), &end, 10);
    if (*end || *w == 0)  return false;
    pos = str1.find_first_of("@");
    if(pos != std::string::npos) {
        std::string str2 = str1.substr(pos+1);
        if (str2.length() == 0 || str2.length() > 3 || str2[0] == '-') return false;
        *r = (unsigned short) strtoul(str2.c_str(), &end, 10);
        if (*end || *r == 0 || *r > 255) return false;
        str1.erase(pos);
    }
    if (str1.length() == 0 || str1.length() > 4 || str1[0] == '-') return false;
    *h = (unsigned short) strtoul(str1.c_str(), &end, 10);
    if (*end || *h == 0) return false;
    return true;
}

static bool get_value (const char *str, unsigned int *n) {
    // if n > 0 str must be a positive decimal <= input value *n  
    // if n = 0, str must be a non-negative decimal
    if (strlen(str) == 0 || strlen(str) > 10 || str[0] == '-') return false;
    char *end;
    unsigned long l = strtoul(str, &end, 10);
    if (*end) return false;
    if (*n && (l == 0 || l > *n)) return false;
    *n = (unsigned int) l;
    return true;
}

static bool get_ports (int nports, std::string option, const char * value, unsigned short * const port) {
    /*valid entries are comma-separated values port_1,port_2,...,port_r, 0 < r <= nports */
    /*where ports are distinct, and are in the allowed range.                            */
    /*missing values are consecutive to last given value (at least one value needed).    */
    char *end;
    unsigned long l;
    std::size_t pos;
    std::string val(value), str;
    for (int i = 0; i <= nports ; i++)  {
        if(i == nports) break;
        pos = val.find_first_of(',');
        str = val.substr(0,pos);
        if(str.length() == 0 || str.length() > 5 || str[0] == '-') break;
        l = strtoul(str.c_str(), &end, 10);
        if (*end || l < LOWEST_ALLOWED_PORT || l > HIGHEST_PORT) break;
         *(port + i) = (unsigned short) l;
        for  (int j = 0; j < i ; j++) {
            if( *(port + j) == *(port + i)) break;
        }
        if(pos == std::string::npos) {
            if (nports + *(port + i) > i + 1 + HIGHEST_PORT) break;
            for (int j = i + 1; j < nports; j++) {
                *(port + j) = *(port + j - 1) + 1;
            }
            return true;
        }
        val.erase(0, pos+1);
    }
    LOGE("invalid \"%s %s\", all %d ports must be in range [%d,%d]",
         option.c_str(), value, nports, LOWEST_ALLOWED_PORT, HIGHEST_PORT);
    return false;
}

static bool get_videoflip (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
        case 'I':
            *videoflip = INVERT;
            break;
        case 'H':
            *videoflip = HFLIP;
            break;
        case 'V':
            *videoflip = VFLIP;
            break;
        default:
            return false;
    }
    return true;
}

static bool get_videorotate (const char *str, videoflip_t *videoflip) {
    if (strlen(str) > 1) return false;
    switch (str[0]) {
        case 'L':
            *videoflip = LEFT;
            break;
        case 'R':
            *videoflip = RIGHT;
            break;
        default:
            return false;
    }
    return true;
}

static void process_metadata(int count, const char *dmap_tag, const unsigned char* metadata, int datalen) {
    int dmap_type = 0;
    /* DMAP metadata items can be strings (dmap_type = 9); other types are byte, short, int, long, date, and list.  *
     * The DMAP item begins with a 4-character (4-letter) "dmap_tag" string that identifies the type.               */

    if (debug_log) {
        printf("%d: dmap_tag [%s], %d\n", count, dmap_tag, datalen);
    }

    /* UTF-8 String-type DMAP tags seen in Apple Music Radio are processed here.   *
     * (DMAP tags "asal", "asar", "ascp", "asgn", "minm" ). TODO expand this */  
    
    if (datalen == 0) {
        return;
    }

    if (dmap_tag[0] == 'a' && dmap_tag[1] == 's') {
        dmap_type = 9;
        switch (dmap_tag[2]) {
        case 'a':
            switch (dmap_tag[3]) {
            case 'a':
                printf("Album artist: ");  /*asaa*/
                break;
            case 'l':
                printf("Album: ");  /*asal*/
                break;
            case 'r':
                printf("Artist: ");  /*asar*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;    
        case 'c':
            switch (dmap_tag[3]) {
            case 'm':
                printf("Comment: ");  /*ascm*/
                break;
            case 'n':
                printf("Content description: ");  /*ascn*/
                break;
            case 'p':
                printf("Composer: ");  /*ascp*/
                break;
            case 't':
                printf("Category: ");  /*asct*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        case 's':
            switch (dmap_tag[3]) {
            case 'a':
                printf("Sort Artist: "); /*assa*/
                break;
            case 'c':
                printf("Sort Composer: ");  /*assc*/
                break;
            case 'l':
                printf("Sort Album artist: ");  /*assl*/
                break;
            case 'n':
                printf("Sort Name: ");  /*assn*/
                break;
            case 's':
                printf("Sort Series: ");  /*asss*/
                break;
            case 'u':
                printf("Sort Album: ");  /*assu*/
                break;
            default:
                dmap_type = 0;
                break;
            }
            break;
        default:
	    if (strcmp(dmap_tag, "asdt") == 0) {
                printf("Description: ");
            } else if (strcmp (dmap_tag, "asfm") == 0) {
                printf("Format: ");
            } else if (strcmp (dmap_tag, "asgn") == 0) {
                printf("Genre: ");
            } else if (strcmp (dmap_tag, "asky") == 0) {
                printf("Keywords: ");
            } else if (strcmp (dmap_tag, "aslc") == 0) {
                printf("Long Content Description: ");
            } else {
                dmap_type = 0;
            }
            break;
        }
    } else if (strcmp (dmap_tag, "minm") == 0) {
        dmap_type = 9;
        printf("Title: ");
    }

    if (dmap_type == 9) {
        char *str = (char *) calloc(1, datalen + 1);
        memcpy(str, metadata, datalen);
        printf("%s", str);
        free(str);
    } else if (debug_log) {
        for (int i = 0; i < datalen; i++) {
            if (i > 0 && i % 16 == 0) printf("\n");
            printf("%2.2x ", (int) metadata[i]);
        }
    }
    printf("\n");
}

static int parse_dmap_header(const unsigned char *metadata, char *tag, int *len) {
    const unsigned char *header = metadata;

    bool istag = true;
    for (int i = 0; i < 4; i++) {
        tag[i] =  (char) *header;
	if (!isalpha(tag[i])) {
            istag = false;
        }
        header++;
    }

    *len = 0;
    for (int i = 0; i < 4; i++) {
        *len <<= 8;
        *len += (int) *header;
        header++;
    }
    if (!istag || *len < 0) {
        return 1;
    }
    return 0;
}

static int register_dnssd() {
    int dnssd_error;    
    if ((dnssd_error = dnssd_register_raop(dnssd, raop_port))) {
        if (dnssd_error == -65537) {
             LOGE("No DNS-SD Server found (DNSServiceRegister call returned kDNSServiceErr_Unknown)");
        } else {
             LOGE("dnssd_register_raop failed with error code %d\n"
                  "mDNS Error codes are in range FFFE FF00 (-65792) to FFFE FFFF (-65537) "
                  "(see Apple's dns_sd.h)", dnssd_error);
        }
        return -3;
    }
    if ((dnssd_error = dnssd_register_airplay(dnssd, airplay_port))) {
        LOGE("dnssd_register_airplay failed with error code %d\n"
             "mDNS Error codes are in range FFFE FF00 (-65792) to FFFE FFFF (-65537) "
             "(see Apple's dns_sd.h)", dnssd_error);
        return -4;
    }
    return 0;
}

static void unregister_dnssd() {
    if (dnssd) {
        dnssd_unregister_raop(dnssd);
        dnssd_unregister_airplay(dnssd);
    }
    return;
}

static void stop_dnssd() {
    if (dnssd) {
        unregister_dnssd();
        dnssd_destroy(dnssd);
        dnssd = NULL;
	return;
    }	
}

static int start_dnssd(std::vector<char> hw_addr, std::string name) {
    int dnssd_error;
    if (dnssd) {
        LOGE("start_dnssd error: dnssd != NULL");
        return 2;
    }
    dnssd = dnssd_init(name.c_str(), strlen(name.c_str()), hw_addr.data(), hw_addr.size(), &dnssd_error);
    if (dnssd_error) {
        LOGE("Could not initialize dnssd library!");
        return 1;
    }
    return 0;
}


// Server callbacks
extern "C" void conn_init (void *cls) {
    open_connections++;
    //LOGD("Open connections: %i", open_connections);
    //video_renderer_update_background(1);
    update_status("connect", "");
}

extern "C" void conn_destroy (void *cls) {
    //video_renderer_update_background(-1);
    open_connections--;
    //LOGD("Open connections: %i", open_connections);
    if (open_connections == 0) {
        remote_clock_offset = 0;
    }
    update_status("connection destroy", "");
}

extern "C" void conn_reset (void *cls, int timeouts, bool reset_video) {
    LOGI("***ERROR lost connection with client (network problem?)");
    if (timeouts) {
        LOGI("   Client no-response limit of %d timeouts (%d seconds) reached:", timeouts, 3*timeouts);
        LOGI("   Sometimes the network connection may recover after a longer delay:\n"
             "   the default timeout limit n = %d can be changed with the \"-reset n\" option", NTP_TIMEOUT_LIMIT);
    }
    printf("reset_video %d\n",(int) reset_video);
    close_window = reset_video;    /* leave "frozen" window open if reset_video is false */
    raop_stop(raop);
    reset_loop = true;
    update_status("connection reset", "");
}

extern "C" void conn_teardown(void *cls, bool *teardown_96, bool *teardown_110) {
    if (*teardown_110 && close_window) {
        reset_loop = true;
    }
    update_status("connection teardown", "");
}

extern "C" void audio_process (void *cls, raop_ntp_t *ntp, audio_decode_struct *data) {
    if (dump_audio) {
        dump_audio_to_file(data->data, data->data_len, (data->data)[0] & 0xf0);
    }
    if (use_audio) {
        if (!remote_clock_offset) {
            remote_clock_offset = data->ntp_time_local - data->ntp_time_remote;
        }
        data->ntp_time_remote = data->ntp_time_remote + remote_clock_offset;
        if (data->ct == 2 && audio_delay_alac) {
            data->ntp_time_remote = (uint64_t) ((int64_t) data->ntp_time_remote  + audio_delay_alac);
        } else if (audio_delay_aac) {
            data->ntp_time_remote = (uint64_t) ((int64_t) data->ntp_time_remote + audio_delay_aac);
        }
      audio_renderer_render_buffer(data->data, &(data->data_len), &(data->seqnum), &(data->ntp_time_remote));
    }
}

extern "C" void video_process (void *cls, raop_ntp_t *ntp, h264_decode_struct *data) {
    if (dump_video) {
        dump_video_to_file(data->data, data->data_len);
    }
    if (use_video) {
        if (!remote_clock_offset) {
            remote_clock_offset = data->ntp_time_local - data->ntp_time_remote;
        }
        data->ntp_time_remote = data->ntp_time_remote + remote_clock_offset;
        video_renderer_render_buffer(data->data, &(data->data_len), &(data->nal_count), &(data->ntp_time_remote));
    }
}

extern "C" void audio_flush (void *cls) {
    if (use_audio) {
        audio_renderer_flush();
    }
}

extern "C" void video_flush (void *cls) {
    if (use_video) {
        video_renderer_flush();
    }
}

extern "C" void audio_set_volume (void *cls, float volume) {
    if (use_audio) {
        audio_renderer_set_volume(volume);
    }
}

extern "C" void audio_get_format (void *cls, unsigned char *ct, unsigned short *spf, bool *usingScreen, bool *isMedia, uint64_t *audioFormat) {
    unsigned char type;
    LOGI("ct=%d spf=%d usingScreen=%d isMedia=%d  audioFormat=0x%lx",*ct, *spf, *usingScreen, *isMedia, (unsigned long) *audioFormat);
    switch (*ct) {
    case 2:
        type = 0x20;
        break;
    case 8:
        type = 0x80;
        break;
    default:
        type = 0x10;
        break;
    }
    if (audio_dumpfile && type != audio_type) {
        fclose(audio_dumpfile);
        audio_dumpfile = NULL;
    }
    audio_type = type;
    
    if (use_audio) {
      audio_renderer_start(ct);
    }

    if (coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }
}

extern "C" void video_report_size(void *cls, float *width_source, float *height_source, float *width, float *height) {
    if (use_video) {
        video_renderer_size(width_source, height_source, width, height);
        update_status("video_report_size", "");
    }
}

extern "C" void audio_set_coverart(void *cls, const void *buffer, int buflen) {
    if (buffer && coverart_filename.length()) {
        write_coverart(coverart_filename.c_str(), buffer, buflen);
        LOGI("coverart size %d written to %s", buflen,  coverart_filename.c_str());
    }
}

extern "C" void audio_set_metadata(void *cls, const void *buffer, int buflen) {
    char dmap_tag[5] = {0x0};
    const unsigned char *metadata = (const  unsigned char *) buffer;
    int datalen;
    int count = 0;

    printf("==============Audio Metadata=============\n");

    if (buflen < 8) {
        LOGE("received invalid metadata, length %d < 8", buflen);
        return;
    } else if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
        LOGE("received invalid metadata, tag [%s]  datalen %d", dmap_tag, datalen);
        return;
    }
    metadata += 8;
    buflen -= 8;

    if (strcmp(dmap_tag, "mlit") != 0 || datalen != buflen) {
        LOGE("received metadata with tag %s, but is not a DMAP listingitem, or datalen = %d !=  buflen %d",
             dmap_tag, datalen, buflen);
        return;
    }
    while (buflen >= 8) {
        count++;
        if (parse_dmap_header(metadata, dmap_tag, &datalen)) {
            LOGE("received metadata with invalid DMAP header:  tag = [%s],  datalen = %d", dmap_tag, datalen);
            return;
        }
        metadata += 8;
        buflen -= 8;
        process_metadata(count, (const char *) dmap_tag, metadata, datalen);
        metadata += datalen;
        buflen -= datalen;
    }
    if (buflen != 0) {
      LOGE("%d bytes of metadata were not processed", buflen);
    }
}

extern "C" void log_callback (void *cls, int level, const char *msg) {
    switch (level) {
        case LOGGER_DEBUG: {
            LOGD("%s", msg);
            break;
        }
        case LOGGER_WARNING: {
            LOGW("%s", msg);
            break;
        }
        case LOGGER_INFO: {
            LOGI("%s", msg);
            break;
        }
        case LOGGER_ERR: {
            LOGE("%s", msg);
            break;
        }
        default:
            break;
    }
}

int start_raop_server (unsigned short display[5], unsigned short tcp[3], unsigned short udp[3], bool debug_log) {
    int dnssd_error;
    raop_callbacks_t raop_cbs;
    memset(&raop_cbs, 0, sizeof(raop_cbs));
    raop_cbs.conn_init = conn_init;
    raop_cbs.conn_destroy = conn_destroy;
    raop_cbs.conn_reset = conn_reset;
    raop_cbs.conn_teardown = conn_teardown;
    raop_cbs.audio_process = audio_process;
    raop_cbs.video_process = video_process;
    raop_cbs.audio_flush = audio_flush;
    raop_cbs.video_flush = video_flush;
    raop_cbs.audio_set_volume = audio_set_volume;
    raop_cbs.audio_get_format = audio_get_format;
    raop_cbs.video_report_size = video_report_size;
    raop_cbs.audio_set_metadata = audio_set_metadata;
    raop_cbs.audio_set_coverart = audio_set_coverart;
    
    /* set max number of connections = 2 to protect against capture by new client */
    raop = raop_init(max_connections, &raop_cbs);
    if (raop == NULL) {
        LOGE("Error initializing raop!");
        return -1;
    }

    /* write desired display pixel width, pixel height, refresh_rate, max_fps, overscanned.  */
    /* use 0 for default values 1920,1080,60,30,0; these are sent to the Airplay client      */

    if (display[0]) raop_set_plist(raop, "width", (int) display[0]);
    if (display[1]) raop_set_plist(raop, "height", (int) display[1]);
    if (display[2]) raop_set_plist(raop, "refreshRate", (int) display[2]);
    if (display[3]) raop_set_plist(raop, "maxFPS", (int) display[3]);
    if (display[4]) raop_set_plist(raop, "overscanned", (int) display[4]);
 
    if (show_client_FPS_data) raop_set_plist(raop, "clientFPSdata", 1);
    raop_set_plist(raop, "max_ntp_timeouts", max_ntp_timeouts);
    if (audiodelay >= 0) raop_set_plist(raop, "audio_delay_micros", audiodelay);

    /* network port selection (ports listed as "0" will be dynamically assigned) */
    raop_set_tcp_ports(raop, tcp);
    raop_set_udp_ports(raop, udp);
    
    raop_set_log_callback(raop, log_callback, NULL);
    raop_set_log_level(raop, debug_log ? RAOP_LOG_DEBUG : LOGGER_INFO);

    raop_port = raop_get_port(raop);
    raop_start(raop, &raop_port);
    raop_set_port(raop, raop_port);

    if (tcp[2]) {
        airplay_port = tcp[2];
    } else {
        /* is there a problem if this coincides with a randomly-selected tcp raop_mirror_data port? 
         * probably not, as the airplay port is only used for initial client contact */
        airplay_port = (raop_port != HIGHEST_PORT ? raop_port + 1 : raop_port - 1);
    }
    if (dnssd) {
        raop_set_dnssd(raop, dnssd);
    } else {
        LOGE("raop_set failed to set dnssd");
        return -2;
    }
    return 0;
}

static void stop_raop_server () {
    if (raop) {
        raop_destroy(raop);
        raop = NULL;
    }
    return;
}

int uxplay_start (struct uxplay_config config) {
    std::vector<char> server_hw_addr;
    std::string mac_address;

    uxplay_stop_flag = false;

    app_config = config;
    server_name = app_config.name;
    video_converter = app_config.video_converter;
    audiosink = app_config.audiosink;
    video_decoder = app_config.video_decoder;
    video_parser = app_config.video_parser;
    videosink = app_config.videosink;
    new_window_closing_behavior = app_config.new_window_closing_behavior;
    debug_log = app_config.debug_log;

    LOGI("UxPlay %s: An Open-Source AirPlay mirroring and audio-streaming server.", VERSION);

    if (!gstreamer_init()) {
        LOGE ("stopping");
        exit (1);
    }

    render_logger = logger_init();
    logger_set_callback(render_logger, log_callback, NULL);
    logger_set_level(render_logger, debug_log ? LOGGER_DEBUG : LOGGER_INFO);

    if (use_audio) {
      audio_renderer_init(render_logger, audiosink.c_str(), &audio_sync, &video_sync);
      update_status("audio inited", "");
    } else {
        LOGI("audio_disabled");
    }

    if (use_video) {
        video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(),
                            video_decoder.c_str(), video_converter.c_str(), videosink.c_str(), &fullscreen, &video_sync);
        update_status("video inited", "");
        video_renderer_start();
        update_status("video started", "");
    }

    if (udp[0]) {
        LOGI("using network ports UDP %d %d %d TCP %d %d %d", udp[0], udp[1], udp[2], tcp[0], tcp[1], tcp[2]);
    }

    if (!use_random_hw_addr) {
        mac_address = find_mac();
    }
    if (mac_address.empty()) {
        srand(time(NULL) * getpid());
        mac_address = random_mac();
        LOGI("using randomly-generated MAC address %s",mac_address.c_str());
    } else {
        LOGI("using system MAC address %s",mac_address.c_str());
    }
    parse_hw_addr(mac_address, server_hw_addr);
    mac_address.clear();

    if (coverart_filename.length()) {
        LOGI("any AirPlay audio cover-art will be written to file  %s",coverart_filename.c_str());
        write_coverart(coverart_filename.c_str(), (const void *) empty_image, sizeof(empty_image));
    }

    restart:
    if (start_dnssd(server_hw_addr, server_name)) {
        goto cleanup;
    }
    if (start_raop_server(display, tcp, udp, debug_log)) {
        stop_dnssd();
        goto cleanup;
    }
    if (register_dnssd()) {
        stop_raop_server();
        stop_dnssd();
        goto cleanup;
    }
    reconnect:
    compression_type = 0;
    close_window = new_window_closing_behavior; 
    update_status("main loop started", "");
    main_loop();
    update_status("main loop stoped", "");
    if (relaunch_video || reset_loop) {
        if(reset_loop) {
            reset_loop = false;
        } else {
            raop_stop(raop);
        }
        if (use_audio) audio_renderer_stop();
        if (use_video && close_window) {
            video_renderer_destroy();
            video_renderer_init(render_logger, server_name.c_str(), videoflip, video_parser.c_str(),
                                video_decoder.c_str(), video_converter.c_str(), videosink.c_str(), &fullscreen,
                                &video_sync);
            video_renderer_start();
        }
        if (relaunch_video) {
            unsigned short port = raop_get_port(raop);
            raop_start(raop, &port);
            raop_set_port(raop, port);
            goto reconnect;
        } else {
            LOGI("Re-launching RAOP server...");
            stop_raop_server();
            stop_dnssd();
            goto restart;
        }
    } else {
        LOGI("Stopping...");
        stop_raop_server();
        stop_dnssd();
    }
    cleanup:
    if (use_audio) {
        audio_renderer_destroy();
    }
    if (use_video)  {
        video_renderer_destroy();
    }
    logger_destroy(render_logger);
    render_logger = NULL;
    if(audio_dumpfile) {
        fclose(audio_dumpfile);
    }
    if (video_dumpfile) {
        fwrite(mark, 1, sizeof(mark), video_dumpfile);
        fclose(video_dumpfile);
    }
    if (coverart_filename.length()) {
	remove (coverart_filename.c_str());
    }
    update_status("stoped", "");
    return 0;
}

int uxplay_stop() {
    uxplay_stop_flag = true;
    return 0;
}
