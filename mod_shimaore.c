/*
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * This module is `mod_shimaore`
 * (c) 2024-2025 Stéphane Alnet <stephane@shimaore.net>
 *
 */

#include <switch.h>
#include <switch_apr.h>
#include <sys/socket.h>

/* Prototypes */
SWITCH_MODULE_LOAD_FUNCTION(mod_shimaore_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_shimaore_shutdown);

/* SWITCH_MODULE_DEFINITION(name, load, shutdown, runtime)
 * Defines a switch_loadable_module_function_table_t and a static const char[] modname
 */
SWITCH_MODULE_DEFINITION(mod_shimaore, mod_shimaore_load, mod_shimaore_shutdown, NULL);

typedef enum {
    /* Raw audio over UDP, native system byte order */
    SHIMAORE_FRAMING_PLAIN,
    /* L16 (network byte order) audio inside RTP framing */
    SHIMAORE_FRAMING_RTP_L16,
} shimaore_framing_t;

typedef struct shimaore_unicast_context_s {
    switch_socket_t *socket;

    uint32_t buncher_position;
    uint32_t buncher_frame_count;
    uint32_t buncher_maximum;
    /* recommended buffer size is 8192, way below the default 64k MTU on Linux loopback interface */
    uint8_t buncher_buffer[2*SWITCH_RECOMMENDED_BUFFER_SIZE];
    uint8_t packet_buffer[2*SWITCH_RECOMMENDED_BUFFER_SIZE];

    shimaore_framing_t framing;
    uint32_t rtp_ssrc; /* provided by app */
    uint16_t rtp_sequence_number;/* initial value SHOULD be random */
    uint32_t rtp_timestamp; /* initial value SHOULD be random */
} shimaore_context_t;

/* Bunch every ten frames, i.e. every 200ms at 20ms sampling time,
 * making for 3200 bytes UDP payload for single channel SLIN16 at 8kHz.
 */
enum {
    BUNCHER_MAXIMUM_PACKET_COUNT = 10
};

char SHIMAORE_UNICAST_BUG[] = "_shimaore_unicast_bug_";

/*** Unicast ***/
static void shimaore_send(shimaore_context_t *context) {
   switch_size_t len = 0;
   len = context->buncher_position;

    switch (context->framing) {
        case SHIMAORE_FRAMING_PLAIN: {
           /* Explicitly ignore errors */
           switch_socket_send(context->socket, context->buncher_buffer, &len);
            break;
        }
        case SHIMAORE_FRAMING_RTP_L16: {
            /* L16 per RFC 3511 section 4.5.11 */
            /* Network byte order */
            context->packet_buffer[0] = 2 << 6; /* version 2, no padding, no extension, no CSRC */
            context->packet_buffer[1] = 96; /* no marker, dynamic */
            /* sequence number */
            context->packet_buffer[2] = context->rtp_sequence_number >> 8;
            context->packet_buffer[3] = context->rtp_sequence_number;
            /* Timestamp */
            context->packet_buffer[4] = context->rtp_timestamp >> 24;
            context->packet_buffer[5] = context->rtp_timestamp >> 16;
            context->packet_buffer[6] = context->rtp_timestamp >> 8;
            context->packet_buffer[7] = context->rtp_timestamp;
            /* SSRC identifier */
            context->packet_buffer[8] = context->rtp_ssrc >> 24;
            context->packet_buffer[9] = context->rtp_ssrc >> 16;
            context->packet_buffer[10] = context->rtp_ssrc >> 8;
            context->packet_buffer[11] = context->rtp_ssrc;
            /* Payload */
            memcpy(context->packet_buffer+12, context->buncher_buffer, len);

#if __BYTE_ORDER == __LITTLE_ENDIAN
            switch_swap_linear((int16_t *)context->packet_buffer+12,len/2);
#endif
            len += 12;
            switch_socket_send(context->socket, context->packet_buffer, &len);

            context->rtp_sequence_number++;
            context->rtp_timestamp += context->buncher_position;
        }
    }
    context->buncher_position = 0;
    context->buncher_frame_count = 0;
}

static switch_bool_t shimaore_unicast_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type)
{
    shimaore_context_t *context = (shimaore_context_t *) user_data;
    if (!context) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "No context in callback!\n");
        return SWITCH_TRUE;
    }

    switch (type) {
    case SWITCH_ABC_TYPE_INIT:
        {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_INFO, "bug: init");
        }
        break;
    case SWITCH_ABC_TYPE_CLOSE:
        {
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_INFO, "bug: close");

            if (context->buncher_position > 0) {
                shimaore_send(context);
            }

        }
        break;
    case SWITCH_ABC_TYPE_READ:
        {
            // switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_INFO, "bug: read");

            if (!context->socket) {
                // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "No socket in callback!\n");
                return SWITCH_TRUE;
            }

            {
                uint32_t flags = 0;
                switch_frame_t read_frame = { 0 };
                read_frame.data = context->buncher_buffer + context->buncher_position;
                read_frame.buflen = SWITCH_RECOMMENDED_BUFFER_SIZE;

                // switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_INFO, "bug: reading frame");
                if (switch_core_media_bug_read(bug, &read_frame, SWITCH_TRUE) != SWITCH_STATUS_SUCCESS) {
                    // switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_INFO, "bug: no frame");
                    return SWITCH_TRUE;
                }

                /* Append to the buffer */
                context->buncher_position += read_frame.datalen;
                context->buncher_frame_count += 1;

                // switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_INFO, "bug: got frame %d\n", read_frame.datalen);

                /* If we have less that the recommended size left or we already processed the proper number of frames, send out and reset. */
                if (context->buncher_position >= SWITCH_RECOMMENDED_BUFFER_SIZE || context->buncher_frame_count >= context->buncher_maximum) {
                    shimaore_send(context);
                }
            }
        }
        break;
    default:
        // switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(switch_core_media_bug_get_session(bug)), SWITCH_LOG_INFO, "bug: other");
        break;
    }

    return SWITCH_TRUE;
}

/* API Interface Function */
#define SHIMAORE_UNICAST_API_SYNTAX "<uuid> [start|stop] [remote_port=<port>] [remote_ip=<ip>] [local_ip=<ip>] [local_port=<port>] [frames_per_packet=<count>] [rtp_ssrc=<number>]"
SWITCH_STANDARD_API(shimaore_unicast_api_function)
{
    switch_core_session_t *rsession = NULL;
    switch_channel_t *channel = NULL;
    shimaore_context_t *context;
    char *mycmd = NULL;
    int argc = 0;
    char *argv[25] = { 0 };
    char *uuid = NULL;
    char *action = NULL;
    const char *function = "shimaore_unicast";

    if (zstr(cmd)) {
        goto usage;
    }

    if (!(mycmd = strdup(cmd))) {
        goto usage;
    }

    argc = switch_separate_string(mycmd, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

    if (argc < 2) {
        goto usage;
    }

    uuid = argv[0];
    action = argv[1];

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "uuid = %s, action = %s\n", uuid, action);

    rsession = switch_core_session_locate(uuid);
    if (!rsession) {
        stream->write_function(stream, "-ERR Cannot locate session!\n");
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "uuid = %s, action = %s, cannot locate session\n", uuid, action);
        goto done;
    }

    channel = switch_core_session_get_channel(rsession);
    if (!channel) {
        stream->write_function(stream, "-ERR Cannot locate channel!\n");
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "uuid = %s, action = %s, cannot locate channel\n", uuid, action);
        goto done;
    }

    if (!zstr(action) && !strcasecmp(action, "stop")) {
        switch_media_bug_t *bug;

        if ((bug = (switch_media_bug_t *) switch_channel_get_private(channel, SHIMAORE_UNICAST_BUG))) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "uuid = %s, action = %s, media bug found\n", uuid, action);
            switch_channel_set_private(channel, SHIMAORE_UNICAST_BUG, NULL);
            switch_core_media_bug_remove(rsession, &bug);
            stream->write_function(stream, "+OK Success\n");
        } else {
            stream->write_function(stream, "+OK Not activated\n");
        }
        goto done;
    }

    if (zstr(action) || strcasecmp(action, "start")) {
        goto usage;
    }

    {
        switch_media_bug_t *bug;

        if ((bug = (switch_media_bug_t *) switch_channel_get_private(channel, SHIMAORE_UNICAST_BUG))) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "uuid = %s, action = %s, already started\n", uuid, action);
            stream->write_function(stream, "-ERR Unicast already activated\n");
            goto done;
        }
    }

    if (argc < 3) {
        goto usage;
    }

    context = (shimaore_context_t *) switch_core_session_alloc(rsession, sizeof(*context));
    assert(context != NULL);
    context->buncher_position = 0;
    context->buncher_frame_count = 0;
    context->buncher_maximum = BUNCHER_MAXIMUM_PACKET_COUNT;
    context->framing = SHIMAORE_FRAMING_PLAIN;
    context->rtp_ssrc = 0;
    context->rtp_sequence_number = rand();
    context->rtp_timestamp = rand();

    char localhost[] = "127.0.0.1";
    char *local_ip = localhost;
    char *remote_ip = localhost;
    int local_port = 5876;
    int remote_port = 0;

    for (uint i = 2; i < argc; i++) {
        char *key = argv[i];
        char *sign = strchr(argv[i],'=');
        if (sign == NULL) {
            goto usage;
        }
        char *value = sign+1;
        if (*value == '\0') {
            goto usage;
        }
        *sign = '\0';
        if (!strcmp(key,"remote_ip")) {
            remote_ip = value;
            continue;
        }
        if (!strcmp(key,"remote_port")) {
            remote_port = atoi(value);
            continue;
        }
        if (!strcmp(key,"local_ip")) {
            local_ip = value;
            continue;
        }
        if (!strcmp(key,"local_port")) {
            remote_port = atoi(value);
            continue;
        }
        if (!strcmp(key,"frames_per_packet")) {
            context->buncher_maximum = atoi(value);
            continue;
        }
        if (!strcmp(key,"rtp_ssrc")) {
            context->framing = SHIMAORE_FRAMING_RTP_L16;
            context->rtp_ssrc = atoi(value);
            continue;
        }
        goto usage;
    }

    if (remote_port <= 0) {
        goto usage;
    }
    if (local_port <= 0) {
        goto usage;
    }
    if (context->buncher_maximum <= 0 || context->buncher_maximum > BUNCHER_MAXIMUM_PACKET_COUNT) {
        goto usage;
    }

    /** Create socket */
    {
        switch_sockaddr_t *local_addr;
        switch_sockaddr_t *remote_addr;

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_DEBUG, "connect %s:%d->%s:%d\n",
                          local_ip, local_port, remote_ip, remote_port);

        if (switch_sockaddr_info_get(&local_addr,
                                     local_ip, SWITCH_UNSPEC, local_port, 0,
                                     switch_core_session_get_pool(rsession)) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure for local!\n");
            goto done;
        }

        if (switch_sockaddr_info_get(&remote_addr,
                                     remote_ip, SWITCH_UNSPEC, remote_port, 0,
                                     switch_core_session_get_pool(rsession)) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure for remote!\n");
            goto done;
        }

        if (switch_socket_create(&context->socket, AF_INET, SOCK_DGRAM, 0, switch_core_session_get_pool(rsession)) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure creating socket!\n");
            goto done;
        }

        if (switch_socket_opt_set(context->socket, SWITCH_SO_REUSEADDR, 1) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure setting socket re-use!\n");
            goto done;
        }

        if (switch_socket_opt_set(context->socket, SWITCH_SO_NONBLOCK, 1) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure setting socket non-blocking!\n");
            goto done;
        }

        if (switch_socket_bind(context->socket, local_addr) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure binding socket!\n");
            goto done;
        }

        if (switch_socket_connect(context->socket, remote_addr) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure connecting socket!\n");
            goto done;
        }

        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_INFO, "Created unicast connection %s:%d->%s:%d\n",
                          local_ip, local_port, remote_ip, remote_port);
    }

    /** Create media bug */
    {
        switch_media_bug_flag_t flags = SMBF_READ_STREAM;
        switch_media_bug_t *bug;
        switch_status_t status;

        if ((status = switch_core_media_bug_add(rsession, function, NULL,
                                                shimaore_unicast_bug_callback, context, 0, flags, &bug)) != SWITCH_STATUS_SUCCESS) {
            stream->write_function(stream, "-ERR Failure!\n");
            switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_INFO, "Creating media bug failed");
            goto done;
        }

        switch_channel_set_private(channel, SHIMAORE_UNICAST_BUG, bug);
        stream->write_function(stream, "+OK Success\n");
        switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(rsession), SWITCH_LOG_INFO, "Created media bug");
        goto done;
    }


 usage:
    stream->write_function(stream, "-USAGE: %s\n", SHIMAORE_UNICAST_API_SYNTAX);

 done:
    if (rsession) {
        switch_core_session_rwunlock(rsession);
    }

    switch_safe_free(mycmd);
    return SWITCH_STATUS_SUCCESS;
}

///////


/* Macro expands to: switch_status_t mod_signalwire_load(switch_loadable_module_interface_t **module_interface, switch_memory_pool_t *pool) */
SWITCH_MODULE_LOAD_FUNCTION(mod_shimaore_load)
{
    switch_api_interface_t *api_interface = NULL;

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    SWITCH_ADD_API(api_interface, "shimaore_unicast", "unicast bug", shimaore_unicast_api_function, SHIMAORE_UNICAST_API_SYNTAX);

    switch_console_set_complete("add shimaore_unicast ::console::list_uuid ::[starttop] remote_port= remote_ip= local_ip= local_port= frames_per_packet= rtp_ssrc=");

    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_shimaore_shutdown)
{
    return SWITCH_STATUS_UNLOAD;
}


/* For Emacs:
* Local Variables:
* mode:c
* indent-tabs-mode:t
* tab-width:4
* c-basic-offset:4
* End:
* For VIM:
* vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
*/
