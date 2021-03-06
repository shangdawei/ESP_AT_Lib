/**	
 * \file            esp_http_server.c
 * \brief           HTTP server based on callback API
 */
 
/*
 * Copyright (c) 2018 Tilen Majerle
 *  
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software, 
 * and to permit persons to whom the Software is furnished to do so, 
 * subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE
 * AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * This file is part of ESP-AT.
 *
 * Author:          Tilen MAJERLE <tilen@majerle.eu>
 */
#include "apps/esp_http_server.h"
#include "esp/esp_mem.h"
#include "ctype.h"

#define ESP_CFG_DBG_SERVER_TRACE            (ESP_CFG_DBG_SERVER | ESP_DBG_TYPE_TRACE)
#define ESP_CFG_DBG_SERVER_TRACE_WARNING    (ESP_CFG_DBG_SERVER | ESP_DBG_TYPE_TRACE | ESP_DBG_LVL_WARNING)
#define ESP_CFG_DBG_SERVER_TRACE_DANGER     (ESP_CFG_DBG_SERVER | ESP_DBG_TYPE_TRACE | ESP_DBG_LVL_DANGER)

/* Function prototypes, declarations in esp_http_server_fs.c file */
uint8_t     http_fs_data_open_file(const http_init_t* hi, http_fs_file_t* file, const char* path);
uint32_t    http_fs_data_read_file(const http_init_t* hi, http_fs_file_t* file, void** buff, size_t btr, size_t* br);
void        http_fs_data_close_file(const http_init_t* hi, http_fs_file_t* file);

/** Number of opened files in system */
uint16_t http_fs_opened_files_cnt;

#define CRLF                        "\r\n"

char http_uri[HTTP_MAX_URI_LEN + 1];
http_param_t http_params[HTTP_MAX_PARAMS];

/* HTTP init structure with user settings */
static const http_init_t* hi;

#if HTTP_USE_METHOD_NOTALLOWED_RESP
const char
http_data_method_not_allowed[] = ""
"HTTP/1.1 405 Method Not Allowed" CRLF
"Connection: close" CRLF
"Allow: GET"
#if HTTP_SUPPORT_POST
", POST"
#endif /* HTTP_SUPPORT_POST */
CRLF
CRLF
"";
#endif /* HTTP_USE_METHOD_NOTALLOWED_RESP */

/**
 * \brief           List of supported file names for index page
 */
static const char *
http_index_filenames[] = {
    "/index.shtml",
    "/index.shtm"
    "/index.ssi",
    "/index.html",
    "/index.htm"
};

/**
 * \brief           List of URI suffixes where SSI tags are supported
 */
static const char *
http_ssi_suffixes[] = {
    ".shtml",
    ".shtm",
    ".ssi"
};

/**
 * \brief           List of 404 URIs
 */
static const char *
http_404_uris[] = {
    "/404.shtml",
    "/404.shtm",
    "/404.ssi",
    "/404.html",
    "/404.htm",
};

/**
 * \brief           Compare 2 strings in case insensitive way
 * \param[in]       a: String a to compare
 * \param[in]       b: String b to compare
 * \return          0 if equal, non-zero otherwise
 */
int
strcmpi(const char* a, const char* b) {
    int d;
    for (;; a++, b++) {
        d = tolower(*a) - tolower(*b);
        if (d || !*a) {
            return d;
        }
    }
}

/**
 * \brief           Parse URI from HTTP request and copy it to linear memory location
 * \param[in]       p: Chain of pbufs from request
 * \return          espOK if successfully parsed, member of \ref espr_t otherwise
 */
static espr_t
http_parse_uri(esp_pbuf_p p) {
    size_t pos_s, pos_e, pos_crlf, uri_len;
                                                
    pos_s = esp_pbuf_strfind(p, " ", 0);        /* Find first " " in request header */
    if (pos_s == ESP_SIZET_MAX || (pos_s != 3 && pos_s != 4)) {
        return espERR;
    }
    pos_crlf = esp_pbuf_strfind(p, CRLF, 0);    /* Find CRLF position */
    if (pos_crlf == ESP_SIZET_MAX) {
        return espERR;
    }
    pos_e = esp_pbuf_strfind(p, " ", pos_s + 1);/* Find second " " in request header */
    if (pos_e == ESP_SIZET_MAX) {               /* If there is no second " " */
        /*
         * HTTP 0.9 request is "GET /\r\n" without
         * space between request URI and CRLF
         */
        pos_e = pos_crlf;                       /* Use the one from CRLF */
    }
    
    uri_len = pos_e - pos_s - 1;                /* Get length of uri */
    if (uri_len > HTTP_MAX_URI_LEN) {
        return espERR;
    }
    esp_pbuf_copy(p, http_uri, uri_len, pos_s + 1); /* Copy data from pbuf to linear memory */
    http_uri[uri_len] = 0;                      /* Set terminating 0 */
    
    return espOK;
}

/**
 * \brief           Extract parameters from user request URI
 * \param[in]       params: RAM variable with parameters
 * \return          Number of parameters extracted
 */
static size_t
http_get_params(char* params) {
    size_t cnt = 0, i;
    char *amp, *eq;
    
    if (params != NULL) {
        for (i = 0; params && i < HTTP_MAX_PARAMS; i++, cnt++) {
            http_params[i].name = params;
            
            eq = params;
            amp = strchr(params, '&');          /* Find next & in a sequence */
            if (amp) {                          /* In case we have it */
                *amp = 0;                       /* Replace it with 0 to end current param */
                params = ++amp;                 /* Go to next one */
            } else {
                params = NULL;
            }
            
            eq = strchr(eq, '=');               /* Find delimiter */
            if (eq) {
                *eq = 0;
                http_params[i].value = eq + 1;
            } else {
                http_params[i].value = NULL;
            }
        }
    }
    return cnt;
}

/**
 * \brief           Get file from uri in format /folder/file?param1=value1&...
 * \param[in]       hs: HTTP state
 * \param[in]       uri: Input URI to get file for
 * \return          1 on success, 0 otherwise
 */
uint8_t
http_get_file_from_uri(http_state_t* hs, const char* uri) {
    size_t uri_len;
    
    memset(&hs->resp_file, 0x00, sizeof(hs->resp_file));
    uri_len = strlen(uri);                      /* Get URI total length */
    if ((uri_len == 1 && uri[0] == '/') ||      /* Index file only requested */
        (uri_len > 1 && uri[0] == '/' && uri[1] == '?')) {  /* Index file + parameters */
        size_t i;
        /*
         * Scan index files and check if there is one from user
         * available to return as main file
         */
        for (i = 0; i < sizeof(http_index_filenames) / sizeof(http_index_filenames[0]); i++) {
            hs->resp_file_opened = http_fs_data_open_file(hi, &hs->resp_file, http_index_filenames[i]); /* Give me a file with desired path */
            if (hs->resp_file_opened) {         /* Do we have a file? */
                uri = http_index_filenames[i];  /* Set new URI for next of this func */
                break;
            }
        }
    }
    
    /*
     * We still don't have a file,
     * maybe there was a request for specific file and possible parameters
     */
    if (!hs->resp_file_opened) {
        char* req_params;
        size_t params_len;
        req_params = strchr(uri, '?');          /* Search for params delimiter */
        if (req_params) {                       /* We found parameters? */
            req_params[0] = 0;                  /* Reset everything at this point */
            req_params++;                       /* Skip NULL part and go to next one */
        }
        
        params_len = http_get_params(req_params);   /* Get request params from request */
        if (hi != NULL && hi->cgi != NULL) {    /* Check if any user specific controls to process */
            size_t i;
            for (i = 0; i < hi->cgi_count; i++) {
                if (!strcmp(hi->cgi[i].uri, uri)) {
                    uri = hi->cgi[i].fn(http_params, params_len);
                    break;
                }
            }
        }
        hs->resp_file_opened = http_fs_data_open_file(hi, &hs->resp_file, uri); /* Give me a new file now */
    }
    
    /*
     * We still don't have a file!
     * Try with 404 error page if available by user
     */
    if (!hs->resp_file_opened) {
        size_t i;
        for (i = 0; i < ESP_ARRAYSIZE(http_404_uris); i++) {
            uri = http_404_uris[i];
            hs->resp_file_opened = http_fs_data_open_file(hi, &hs->resp_file, uri); /* Get 404 error page */
            if (hs->resp_file_opened) {
                break;
            }
        }
    }
    
    /*
     * Check if SSI should be supported on this file
     */
    hs->is_ssi = 0;                             /* By default no SSI is supported */
    if (hs->resp_file_opened) {
        size_t i, uri_len, suffix_len;
        const char* suffix;
        
        uri_len = strlen(uri);                  /* Get length of URI */
        for (i = 0; i < ESP_ARRAYSIZE(http_ssi_suffixes); i++) {
            suffix = http_ssi_suffixes[i];      /* Get suffix */
            suffix_len = strlen(suffix);        /* Get length of suffix */
            
            if (suffix_len < uri_len && !strcmpi(suffix, &uri[uri_len - suffix_len])) {
                hs->is_ssi = 1;                 /* We have a SSI tag */
                break;
            }
        }
    }
    
    return hs->resp_file_opened;
}

#if HTTP_SUPPORT_POST
/**
 * \brief           Send the received pbuf to user space
 * \param[in]       hs: HTTP state context
 * \param[in]       pbuf: Pbuf with received data
 * \param[in]       offset: Offset in pbuf where to start reading the buffer
 */
static void
http_post_send_to_user(http_state_t* hs, esp_pbuf_p pbuf, size_t offset) {
    esp_pbuf_p new_pbuf;

    if (hi == NULL || hi->post_data_fn == NULL) {
        return;
    }
    
    new_pbuf = esp_pbuf_skip(pbuf, offset, &offset);    /* Skip pbufs and create this one */
    if (new_pbuf != NULL) {
        esp_pbuf_advance(new_pbuf, offset);     /* Advance pbuf for remaining bytes */
    
        hi->post_data_fn(hs, new_pbuf);         /* Notify user with data */
    }
}
#endif /* HTTP_SUPPORT_POST */

/**
 * \brief           Read next part of response file
 * \param[in]       ht: HTTP state
 */
static uint32_t
read_resp_file(http_state_t* hs) {
    uint32_t len = 0;
    
    if (!hs->resp_file_opened) {                /* File should be opened at this point! */
        return 0;
    }
    
    hs->buff_ptr = 0;                           /* Reset buffer pointer at this point */
    
    /*
     * Is our memory set for some reason?
     */
    if (hs->buff != NULL) {                     /* Do we have already something in our buffer? */
        if (!hs->resp_file.is_static) {         /* If file is not static... */
            esp_mem_free((void *)hs->buff);     /* ...free the memory... */
        }
        hs->buff = NULL;                        /* ...and reset pointer */
    }
    
    /*
     * Is buffer set to NULL?
     * In this case set a pointer to static memory in case of static file or
     * allocate memory for dynamic file and read it
     */
    if (hs->buff == NULL) {                     /* Do we have a buffer empty? */
        len = http_fs_data_read_file(hi, &hs->resp_file, NULL, 0, NULL);    /* Get number of remaining bytes to read in file */
        if (len) {                              /* Is there anything to read? On static files, this should be valid only once */
            if (hs->resp_file.is_static) {      /* On static files... */
                len = http_fs_data_read_file(hi, &hs->resp_file, (void **)&hs->buff, len, NULL);    /* ...simply set file pointer */
                hs->buff_len = len;             /* Set buffer length */
                if (!len) {                     /* Empty read? */
                    hs->buff = NULL;            /* Reset buffer */
                }
            } else {
                if (len > ESP_CFG_CONN_MAX_DATA_LEN) {  /* Limit to maximal length */
                    len = ESP_CFG_CONN_MAX_DATA_LEN;
                }
                hs->buff_ptr = 0;               /* Reset read pointer */
                do {
                    hs->buff_len = len;         /* Set length... */
                    hs->buff = (const void *)esp_mem_alloc(hs->buff_len);   /* ...and try to allocate memory for it */
                    if (hs->buff != NULL) {     /* Is memory ready? */
                        /*
                         * Read file directly and stop everything
                         */
                        if (http_fs_data_read_file(hi, &hs->resp_file, (void **)&hs->buff, hs->buff_len, NULL) == 0) {
                            esp_mem_free((void *)hs->buff); /* Release the memory */
                            hs->buff = NULL;    /* Reset pointer */
                        }
                        break;
                    }
                } while ((len >>= 1) > 64);
            }
        }
    }
    
    return hs->buff != NULL;                    /* Do we have our memory ready? */
}

/**
 * \brief           Send response using SSI processing
 * \param[in]       hs: HTTP state
 */
static void
send_response_ssi(http_state_t* hs) {
    uint8_t reset = 0;
    uint8_t ch;
    
    ESP_DEBUGF(ESP_CFG_DBG_SERVER_TRACE, "SERVER: processing with SSI\r\n");
    
    /*
     * First get available memory in output buffer
     */
    esp_conn_write(hs->conn, NULL, 0, 0, &hs->conn_mem_available);  /* Get available memory and/or create a new buffer if possible */
    
    /*
     * Check if we have to send temporary buffer,
     * because of wrong TAG format set by user
     */
    if (hs->ssi_tag_buff_written < hs->ssi_tag_buff_ptr) {  /* Do we have to send something from SSI buffer? */
        size_t len;
        len = ESP_MIN(hs->ssi_tag_buff_ptr - hs->ssi_tag_buff_written, hs->conn_mem_available);
        if (len) {                              /* More data to send? */
            esp_conn_write(hs->conn, &hs->ssi_tag_buff[hs->ssi_tag_buff_written], len, 0, &hs->conn_mem_available);
            hs->written_total += len;           /* Increase total number of written elements */
            hs->ssi_tag_buff_written += len;    /* Increase total number of written SSI buffer */
            
            if (hs->ssi_tag_buff_written == hs->ssi_tag_buff_ptr) {
                hs->ssi_tag_buff_ptr = 0;       /* Reset pointer */
            }
        }
    }
    
    /*
     * Are we ready to read more data?
     */
    if (hs->buff == NULL || hs->buff_ptr == hs->buff_len) {
        read_resp_file(hs);                     /* Read more file at this point */
    }
    
    /*
     * Process remaining SSI tag buffer
     * Buffer should be ready from response file function call
     */
    if (hs->buff != NULL) {
        while (hs->buff_ptr < hs->buff_len && hs->conn_mem_available) { /* Process entire buffer if possible */
            ch = hs->buff[hs->buff_ptr];        /* Get next character */
            switch (hs->ssi_state) {
                case HTTP_SSI_STATE_WAIT_BEGIN: {
                    if (ch == HTTP_SSI_TAG_START[0]) {
                        hs->ssi_tag_buff[0] = ch;
                        hs->ssi_tag_buff_ptr = 1;
                        hs->ssi_state = HTTP_SSI_STATE_BEGIN;
                    } else {
                        reset = 1;
                    }
                    break;
                }
                case HTTP_SSI_STATE_BEGIN: {
                    if (hs->ssi_tag_buff_ptr < HTTP_SSI_TAG_START_LEN &&
                        ch == HTTP_SSI_TAG_START[hs->ssi_tag_buff_ptr]) {
                        hs->ssi_tag_buff[hs->ssi_tag_buff_ptr++] = ch;
                            
                        if (hs->ssi_tag_buff_ptr == HTTP_SSI_TAG_START_LEN) {
                            hs->ssi_state = HTTP_SSI_STATE_TAG;
                            hs->ssi_tag_len = 0;
                        }
                    } else {
                        reset = 1;
                    }
                    break;
                }
                case HTTP_SSI_STATE_TAG: {
                    if (ch == HTTP_SSI_TAG_END[0]) {
                        hs->ssi_tag_buff[hs->ssi_tag_buff_ptr++] = ch;
                        hs->ssi_state = HTTP_SSI_STATE_END;
                    } else {
                        if ((hs->ssi_tag_buff_ptr - HTTP_SSI_TAG_START_LEN) < HTTP_SSI_TAG_MAX_LEN) {
                            hs->ssi_tag_buff[hs->ssi_tag_buff_ptr++] = ch;
                            hs->ssi_tag_len++;
                        } else {
                            reset = 1;
                        }
                    }
                    break;
                }
                case HTTP_SSI_STATE_END: {
                    if ((hs->ssi_tag_buff_ptr - HTTP_SSI_TAG_START_LEN - hs->ssi_tag_len) < HTTP_SSI_TAG_END_LEN &&
                        ch == HTTP_SSI_TAG_END[(hs->ssi_tag_buff_ptr - HTTP_SSI_TAG_START_LEN - hs->ssi_tag_len)]) {
                        
                        hs->ssi_tag_buff[hs->ssi_tag_buff_ptr++] = ch;
                        
                        /*
                         * Did we reach end of tag and are ready to get replacement from user?
                         */
                        if (hs->ssi_tag_buff_ptr == (HTTP_SSI_TAG_START_LEN + hs->ssi_tag_len + HTTP_SSI_TAG_END_LEN)) {
                            hs->ssi_tag_buff[HTTP_SSI_TAG_START_LEN + hs->ssi_tag_len] = 0;
                            
                            if (hi != NULL && hi->ssi_fn != NULL) {
                                hi->ssi_fn(hs, &hs->ssi_tag_buff[HTTP_SSI_TAG_START_LEN], hs->ssi_tag_len);
                            }
                            
                            hs->ssi_state = HTTP_SSI_STATE_WAIT_BEGIN;
                            hs->ssi_tag_len = 0;
                            hs->ssi_tag_buff_ptr = 0;   /* Manually reset everything to prevent anything to be sent */
                        }
                    } else {
                        reset = 1;
                    }
                    break;
                }
                default:
                    break;
            }
            
            if (reset) {
                reset = 0;
                if (hs->ssi_tag_buff_ptr) {     /* Do we have to send something from temporary TAG buffer? */
                    size_t len;
                    
                    len = ESP_MIN(hs->ssi_tag_buff_ptr, hs->conn_mem_available);
                    esp_conn_write(hs->conn, hs->ssi_tag_buff, len, 0, &hs->conn_mem_available);
                    hs->written_total += len;   /* Increase total written length */
                    hs->ssi_tag_buff_written = len; /* Set length of number of written buffer */
                    if (len == hs->ssi_tag_buff_ptr) {
                        hs->ssi_tag_buff_ptr = 0;
                    }
                }
                if (hs->conn_mem_available) {   /* Is there memory to write a current byte? */
                    esp_conn_write(hs->conn, &ch, 1, 0, &hs->conn_mem_available);
                    hs->written_total++;
                    hs->buff_ptr++;
                }
                hs->ssi_state = HTTP_SSI_STATE_WAIT_BEGIN;
            } else {
                hs->buff_ptr++;
            }
        }
    }
    esp_conn_write(hs->conn, NULL, 0, 1, &hs->conn_mem_available);  /* Flush to output if possible */
}

/**
 * \brief           Send more data without SSI tags parsing
 * \param[in]       hs: HTTP state
 */
static void
send_response_no_ssi(http_state_t* hs) {
    if (hs->buff == NULL || hs->written_total == hs->sent_total) {
        read_resp_file(hs);                     /* Try to read response file */
    }
    
    ESP_DEBUGF(ESP_CFG_DBG_SERVER_TRACE, "SERVER processing NO SSI\r\n");
    
    /*
     * Do we have a file? 
     * Static file should be processed only once at the end 
     * as entire memory can be send at a time
     */
    if (hs->buff != NULL) {
        if (esp_conn_send(hs->conn, hs->buff, hs->buff_len, NULL, 0) == espOK) {
            hs->written_total += hs->buff_len;  /* Set written total length */
        }
    }
}

/**
 * \brief           Send response back to connection
 * \param[in]       hs: HTTP state
 * \param[in]       ft: Flag indicating function was called first time to send the response
 */
static void
send_response(http_state_t* hs, uint8_t ft) {
    uint8_t close = 0;
    
    if (!hs->process_resp ||                    /* Not yet ready to process response? */
        (hs->written_total && hs->written_total != hs->sent_total)) {   /* Did we wrote something but didn't send yet? */
        return;
    }

    /*
     * Do we have a file ready to be send?
     * At this point it should be opened already if request method is valid
     */
    if (hs->resp_file_opened) {
        /*
         * Process and send more data to output
         */
        if (hs->is_ssi) {                       /* In case of SSI request, process data using SSI */
            send_response_ssi(hs);              /* Send response using SSI parsing */
        } else {
            send_response_no_ssi(hs);           /* Send response without SSI parsing */
        }
        
        /*
         * Shall we hare directly close a connection if buff is NULL?
         * Maybe check first if problem was memory and try next time again
         *
         * Currently this is a solution to close the file
         */
        if (hs->buff == NULL) {                 /* Sent everything or problem somehow? */
            close = 1;
        }
    } else  {
#if HTTP_USE_METHOD_NOTALLOWED_RESP
        if (hs->req_method == HTTP_METHOD_NOTALLOWED) {  /* Is request method not allowed? */
            esp_conn_send(hs->conn, http_data_method_not_allowed, sizeof(http_data_method_not_allowed) - 1, NULL, 0);
            /* Don't set number of bytes written to preven recursion */
        }
#endif /* HTTP_USE_METHOD_NOT_ALLOWED_RESPONSE */
        close = 1;                              /* Close connection, file is not opened */
    }
    
    if (close) {
        esp_conn_close(hs->conn, 0);            /* Close the connection as no file opened in this case */
    }
}

/**
 * \brief           Server connection callback
 * \param[in]       cb: Pointer to callback data
 * \return          espOK on success, member of \ref espr_t otherwise
 */
static espr_t
http_evt_cb(esp_cb_t* cb) {
    uint8_t close = 0;
    esp_conn_p conn;
    http_state_t* hs = NULL;
    
    conn = esp_conn_get_from_evt(cb);           /* Get connection from event */
    if (conn != NULL) {
        hs = esp_conn_get_arg(conn);            /* Get connection argument */
    }
    switch (cb->type) {
        /*
         * A new connection just became active
         */
        case ESP_CB_CONN_ACTIVE: {
            hs = esp_mem_calloc(1, sizeof(*hs));
            if (hs != NULL) {
                hs->conn = conn;                /* Save connection handle */
                esp_conn_set_arg(conn, hs);     /* Set argument for connection */
            } else {
                ESP_DEBUGF(ESP_CFG_DBG_SERVER_TRACE_WARNING, "SERVER cannot allocate memory for http state\r\n");
                close = 1;                      /* No memory, close the connection */
            }
            break;
        }
        
        /*
         * Data received on connection
         */
        case ESP_CB_CONN_DATA_RECV: {
            esp_pbuf_p p = cb->cb.conn_data_recv.buff;
            size_t pos;
            
            if (hs != NULL) {                   /* Do we have a valid http state? */
                /*
                 * Check if we have to receive headers data first
                 * before we can proceed with everything else
                 */
                if (!hs->headers_received) {    /* Are we still waiting for headers data? */
                    if (hs->p == NULL) {
                        hs->p = p;              /* This is a first received packet */
                    } else {
                        esp_pbuf_cat(hs->p, p); /* Add new packet to the end of linked list of recieved data */
                    }
                
                    /*
                     * Check if headers are fully received.
                     * To know this, search for "\r\n\r\n" sequence in received data
                     */
                    if ((pos = esp_pbuf_strfind(hs->p, CRLF CRLF, 0)) != ESP_SIZET_MAX) {
                        uint8_t http_uri_parsed;
                        ESP_DEBUGF(ESP_CFG_DBG_SERVER_TRACE, "SERVER HTTP headers received!\r\n");
                        hs->headers_received = 1;   /* Flag received headers */
                        
                        /*
                         * Parse the URI, process request and open response file
                         */
                        http_uri_parsed = http_parse_uri(hs->p) == espOK;
                        
#if HTTP_SUPPORT_POST                        
                        /*
                         * Check for request method used on this connection
                         */
                        if (!esp_pbuf_strcmp(hs->p, "POST ", 0)) {
                            size_t data_pos, pbuf_total_len;
                        
                            hs->req_method = HTTP_METHOD_POST;  /* Save a new value as POST method */
                        
                        
                            /*
                             * At this point, all headers are received
                             * We can start process them into something useful
                             */
                            data_pos = pos + 4; /* Ignore 4 bytes of CRLF sequence */
                            
                            /*
                             * Try to find content length on this request
                             * search for 2 possible values "Content-Length" or "content-length" parameters
                             */
                            hs->content_length = 0;
                            if (((pos = esp_pbuf_strfind(hs->p, "Content-Length:", 0)) != ESP_SIZET_MAX) ||
                                (pos = esp_pbuf_strfind(hs->p, "content-length:", 0)) != ESP_SIZET_MAX) {
                                uint8_t ch;
                                
                                pos += 15;      /* Skip this part */
                                if (esp_pbuf_get_at(hs->p, pos, &ch) && ch == ' ') {
                                    pos++;
                                }
                                esp_pbuf_get_at(hs->p, pos, &ch);
                                while (ch >= '0' && ch <= '9') {
                                    hs->content_length = 10 * hs->content_length + (ch - '0');
                                    pos++;
                                    if (!esp_pbuf_get_at(hs->p, pos, &ch)) {
                                        break;
                                    }
                                }
                            }
                            
                            /*
                             * Check if we are expecting any data on POST request
                             */
                            if (hs->content_length) {
                                /*
                                 * Call user POST start method here
                                 * to notify him to prepare himself to receive POST data
                                 */
                                if (hi != NULL && hi->post_start_fn != NULL) {
                                    hi->post_start_fn(hs, http_uri, hs->content_length);
                                }
                                
                                /*
                                 * Check if there is anything to send already
                                 * to user from data part of request
                                 */
                                pbuf_total_len = esp_pbuf_length(hs->p, 1); /* Get total length of current received pbuf */
                                if ((pbuf_total_len - data_pos) > 0) {
                                    hs->content_received = pbuf_total_len - data_pos;
                                    
                                    /*
                                     * Send data to user
                                     */
                                    http_post_send_to_user(hs, hs->p, data_pos);
                                    
                                    /*
                                     * Did we receive everything in single packet?
                                     * Close POST loop at this point and notify user
                                     */
                                    if (hs->content_received >= hs->content_length) {
                                        hs->process_resp = 1;   /* Process with response to user */
                                        if (hi != NULL && hi->post_end_fn != NULL) {
                                            hi->post_end_fn(hs);
                                        }
                                    }
                                }
                            } else {
                                hs->process_resp = 1;
                            }
                        } else 
#else
                        ESP_UNUSED(pos);        /* Unused variable */
#endif /* HTTP_SUPPORT_POST */
                        if (!esp_pbuf_strcmp(hs->p, "GET ", 0)) {
                            hs->req_method = HTTP_METHOD_GET;
                            hs->process_resp = 1;   /* Process with response to user */
                        } else {
                            hs->req_method = HTTP_METHOD_NOTALLOWED;
                            hs->process_resp = 1;
                        }
                        
                        /*
                         * If uri was parsed succssfully and if method is allowed,
                         * then open and prepare file for future response
                         */
                        if (http_uri_parsed && hs->req_method != HTTP_METHOD_NOTALLOWED) {
                            http_get_file_from_uri(hs, http_uri);   /* Open file */
                        }
                    }
#if HTTP_SUPPORT_POST
                } else {
                    /*
                     * We are receiving request data now
                     * as headers are already received
                     */
                    if (hs->req_method == HTTP_METHOD_POST) {
                        /*
                         * Did we receive all the data on POST?
                         */
                        if (hs->content_received < hs->content_length) {
                            size_t tot_len;
                            
                            tot_len = esp_pbuf_length(p, 1);/* Get length of pbuf */
                            hs->content_received += tot_len;
                            
                            http_post_send_to_user(hs, p, 0);   /* Send data directly to user */
                            
                            /**
                             * Check if everything received
                             */
                            if (hs->content_received >= hs->content_length) {
                                hs->process_resp = 1;   /* Process with response to user */
                                
                                /*
                                 * Stop the response part here!
                                 */
                                if (hi != NULL && hi->post_end_fn) {
                                    hi->post_end_fn(hs);
                                }
                            }
                        }
#endif /* HTTP_SUPPORT_POST */
                    } else {
                        /* Protocol violation at this point! */
                    }
                    esp_pbuf_free(p);           /* Free packet buffer */
                }
                
                /* Do the processing on response */
                if (hs->process_resp) {
                    send_response(hs, 1);       /* Send the response data */
                }
            } else {
                esp_pbuf_free(p);               /* Free packet buffer */
                close = 1;
            }
            esp_conn_recved(conn, p);           /* Notify stack about received data */
            break;
        }
        
        /*
         * Data were successfully sent on a connection
         */
        case ESP_CB_CONN_DATA_SENT: {
            if (hs != NULL) {
                ESP_DEBUGF(ESP_CFG_DBG_SERVER_TRACE,
                    "Server data sent with %d bytes\r\n", (int)cb->cb.conn_data_sent.sent);
                hs->sent_total += cb->cb.conn_data_sent.sent;   /* Increase number of bytes sent */
                send_response(hs, 0);           /* Send more data if possible */
            } else {
                close = 1;
            }
            break;
        }
        
        /*
         * There was a problem with sending connection data
         */
        case ESP_CB_CONN_DATA_SEND_ERR: {
            ESP_DEBUGF(ESP_CFG_DBG_SERVER_TRACE_DANGER, "SERVER data send error. Closing connection..\r\n");
            close = 1;                          /* Close the connection */
            break;
        }
        
        /*
         * Connection was just closed, either forced by user or by remote side
         */
        case ESP_CB_CONN_CLOSED: {
            ESP_DEBUGF(ESP_CFG_DBG_SERVER_TRACE, "SERVER connection closed\r\n");
            if (hs != NULL) {
#if HTTP_SUPPORT_POST
                if (hs->req_method == HTTP_METHOD_POST) {
                    if (hs->content_received < hs->content_length) {
                        if (hi != NULL && hi->post_end_fn) {
                            hi->post_end_fn(hs);
                        }
                    }
                }
#endif /* HTTP_SUPPORT_POST */
                if (hs->p != NULL) {
                    esp_pbuf_free(hs->p);       /* Free packet buffer */
                    hs->p = NULL;
                }
                if (hs->resp_file_opened) {     /* Is file opened? */
                    uint8_t is_static = hs->resp_file.is_static;
                    http_fs_data_close_file(hi, &hs->resp_file);    /* Close file at this point */
                    if (!is_static && hs->buff != NULL) {
                        esp_mem_free((void *)hs->buff); /* Free the memory */
                        hs->buff = NULL;
                    }
                    hs->resp_file_opened = 0;   /* File is not opened anymore */
                }
                esp_mem_free(hs);
                hs = NULL;
            }
            break;
        }
        
        /*
         * Poll the connection
         */
        case ESP_CB_CONN_POLL: {
            if (hs != NULL) {
                send_response(hs, 0);           /* Send more data if possible */
            } else {
                close = 1;
            }
            break;
        }
        default:
            break;
    }
    
    if (close) {                                /* Do we have to close a connection? */
        esp_conn_close(conn, 0);                /* Close a connection */
    }
    
    return espOK;
}

/**
 * \brief           Initialize HTTP server at specific port
 * \param[in]       init: Initialization structure for server
 * \param[in]       port: Port for HTTP server, usually 80
 * \return          espOK on success, member of \ref espr_t otherwise
 */
espr_t
esp_http_server_init(const http_init_t* init, uint16_t port) {
    espr_t res;
    if ((res = esp_set_server(port, ESP_CFG_MAX_CONNS / 2, 80, http_evt_cb, 1)) == espOK) {
        hi = init;
    }
    return res;
}

/**
 * \brief           Write data directly to connection from callback
 * \note            This function may only be called from SSI callback function for HTTP server
 * \param[in]       hs: HTTP state
 * \param[in]       data: Data to write
 * \param[in]       len: Length of bytes to write
 * \return          Number of bytes written
 */
size_t
esp_http_server_write(http_state_t* hs, const void* data, size_t len) {
    esp_conn_write(hs->conn, data, len, 0, &hs->conn_mem_available);
    hs->written_total += len;                   /* Increase total length */
    return len;
}
