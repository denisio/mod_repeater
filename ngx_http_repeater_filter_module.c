/*
 * The HTTP repeater is Copyright (C) 2010 Denis Erygin
 * and licensed under the GNU General Public License, version 2.
 * Bug reports, feedback, admiration, abuse, etc, to: denis.erygin@gmail.com
 *
 * Configuration (nginx.conf):
 * . . .
 * server {
 *   . . .
 *   repeater 127.0.0.1:10000;
 *   . . .
 * }
 * . . .
 */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#define DEFAULT_PORT 10000

typedef struct {
    ngx_str_t*          data;
    ngx_chain_t*        out;
    ngx_http_request_t* r;
    ngx_uint_t          content_length;
    ngx_uint_t          counter;
    
    ngx_uint_t          done;
    ngx_http_request_t* wait;
} ngx_http_draft_ctx_t;


typedef struct {
    ngx_str_t  repeater;
    ngx_flag_t debug;
} ngx_http_repeater_loc_conf_t;

static ngx_int_t ngx_http_repeater_filter_init      ( ngx_conf_t* cf );
static void*     ngx_http_repeater_create_loc_conf  ( ngx_conf_t* cf );
static char*     ngx_http_repeater_merge_loc_conf   ( ngx_conf_t* cf, void* parent, void* child );
static int       send_request                       ( char* host, int port, const void* buf, size_t len );

void dump_request ( ngx_http_request_t* r );
const char* get_str_error ( ngx_int_t rc );

static ngx_command_t  ngx_http_repeater_filter_commands[] = {
    { ngx_string("repeater"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_repeater_loc_conf_t, repeater),
      NULL },
    { ngx_string("debug"),
      NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_repeater_loc_conf_t, debug),
      NULL },
    ngx_null_command
};

static ngx_http_module_t  ngx_http_repeater_filter_module_ctx = {
    NULL,                                  // preconfiguration 
    ngx_http_repeater_filter_init,         // postconfiguration 

    NULL,                                  // create main configuration 
    NULL,                                  // init main configuration 

    NULL,                                  // create server configuration 
    NULL,                                  // merge server configuration 

    ngx_http_repeater_create_loc_conf,     // create location configuration 
    ngx_http_repeater_merge_loc_conf       // merge location configuration 
};


ngx_module_t  ngx_http_repeater_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_repeater_filter_module_ctx,  // module context 
    ngx_http_repeater_filter_commands,     // module directives 
    NGX_HTTP_MODULE,                       // module type 
    NULL,                                  // init master 
    NULL,                                  // init module 
    NULL,                                  // init process 
    NULL,                                  // init thread 
    NULL,                                  // exit thread 
    NULL,                                  // exit process 
    NULL,                                  // exit master 
    NGX_MODULE_V1_PADDING
};


static ngx_http_output_header_filter_pt  ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt    ngx_http_next_body_filter;

static ngx_int_t ngx_http_repeater_header_filter ( ngx_http_request_t* r )
{
    char* buf;
    char* host, *pos;
    size_t length; 
    int port = DEFAULT_PORT;
    ngx_uint_t i;
    ngx_list_part_t* part;
    ngx_table_elt_t* header;
    ngx_http_repeater_loc_conf_t* conf;

    if ( r != r->main )
       return ngx_http_next_header_filter (r);

    conf = ngx_http_get_module_loc_conf ( r, ngx_http_repeater_filter_module );
    if (!conf) return NGX_ERROR;
    if ( conf->repeater.len < 1 || r != r->main )
       return ngx_http_next_header_filter (r);

    host = ngx_pcalloc( r->pool, (conf->repeater.len + 1) * sizeof(char) );
    if (!host) return NGX_ERROR;
    memcpy(host, conf->repeater.data, conf->repeater.len);
    
    pos = strchr(host, ':');
    if (pos) {
       *pos = '\0';
       pos++;
       port = atoi(pos);
    }

    length = r->request_line.len + sizeof(CRLF) - 1;
    
    part   = &r->headers_in.headers.part;
    header = part->elts;

    for (i = 0; ; i++)
    {
       if ( i >= part->nelts ) {
          if ( part->next == NULL )
              break;
          part   = part->next;
          header = part->elts;
          i      = 0;
       }
       if ( header[i].key.len < 1 && header[i].value.len < 1 ) continue;
       if ( strncmp ((const char*)header[i].key.data, "Host", (size_t)header[i].key.len) == 0 ) 
           continue;
       
       length += header[i].key.len + sizeof(": ") - 1 + header[i].value.len;
       length += sizeof(CRLF) - 1;
    }
    
    length += sizeof(CRLF) - 1;

    buf = ngx_pcalloc ( r->pool, (length + 1) * sizeof(char) );
    if (!buf) return NGX_ERROR;

    char* req = ngx_pcalloc ( r->pool, (r->request_line.len + 1) * sizeof(char) );
    if (req) memcpy ( req, r->request_line.data, r->request_line.len );
    
    strcat(buf, req);
    strcat(buf, CRLF);
            
    for (i = 0; ; i++) 
    {
      if ( i >= part->nelts ) {
         if (part->next == NULL)
             break;
         part   = part->next;
         header = part->elts;
         i      = 0;
      }
      if (header[i].key.len < 1 && header[i].value.len < 1) continue;
      if ( strncmp ((const char*)header[i].key.data, "Host", header[i].key.len) == 0 ) 
          continue;
      
      char* key = ngx_pcalloc(r->pool, (header[i].key.len + 1) * sizeof(char));
      if (key) memcpy(key, header[i].key.data, header[i].key.len);
      
      char* val = ngx_pcalloc(r->pool, (header[i].value.len + 1) * sizeof(char));      
      if (val) memcpy(val, header[i].value.data, header[i].value.len);
      
      //printf("%s: %s\n", key, val);
      
      strcat(buf, key);
      strcat(buf, ": ");
      strcat(buf, val);
      strcat(buf, CRLF);
   }
   strcat(buf, CRLF);
   
   //printf("[%d]\n[%s]\n", length, buf);
   
   send_request(host, port, buf, length);

   return ngx_http_next_header_filter (r);
}

static ngx_int_t ngx_http_repeater_body_filter ( ngx_http_request_t* r, ngx_chain_t* in )
{
   return ngx_http_next_body_filter ( r, in );
}

static ngx_int_t ngx_http_repeater_filter_init ( ngx_conf_t* cf )
{
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter  = ngx_http_repeater_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter  = ngx_http_repeater_body_filter;
    
    return NGX_OK;
}

static void* ngx_http_repeater_create_loc_conf ( ngx_conf_t* cf )
{
    ngx_http_repeater_loc_conf_t* conf;
    
    conf = ngx_pcalloc ( cf->pool, sizeof(ngx_http_repeater_loc_conf_t) );
    if ( conf == NULL ) return NGX_CONF_ERROR;

    conf->debug = NGX_CONF_UNSET; 
    
    return conf;
}

static char* ngx_http_repeater_merge_loc_conf ( ngx_conf_t* cf, void* parent, void* child )
{
    ngx_http_repeater_loc_conf_t* prev = parent;
    ngx_http_repeater_loc_conf_t* conf = child;
        
    ngx_conf_merge_str_value ( conf->repeater, prev->repeater, "");
    ngx_conf_merge_value     ( conf->debug, prev->debug, 0 );

    return NGX_CONF_OK;
}

static uint32_t get_addr ( const char* host )
{
   int a;
   struct hostent* h;
   struct in_addr* addr;
   
   a = inet_addr(host);
   if ( !a || a != -1 )
      return a;
   
   h = gethostbyname(host);
   if ( h != NULL ) {
      addr = (struct in_addr*) *h->h_addr_list;
      return ((uint32_t)(addr->s_addr));   
   }
   
   return inet_addr("127.0.0.1");
}

static int send_request ( char* host, int port, const void* buf, size_t len )
{
    int sock, bytes;
    struct sockaddr_in client, server;
    
    if ( !host || !buf || !len ) return 0;
    
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if ( sock < 0 ) {
       //puts("Error send_request(): socket() failed!");
       return 0;
    }
    
    memset ( (char*)&server, 0, sizeof(server) );
    memset ( (char*)&client, 0, sizeof(client) );

    client.sin_family      = AF_INET;
    client.sin_addr.s_addr = htonl(INADDR_ANY);
    client.sin_port        = 0;
    server.sin_family      = AF_INET;
    server.sin_addr.s_addr = get_addr(host);
    server.sin_port        = htons(port);

    if ( bind(sock, (struct sockaddr*)&client, sizeof(client) ) < 0 ) {
       //puts("Error send_request(): bind() failed!");
       return 0;
    }
    
    bytes = sendto ( sock, buf, len, 0, (const struct sockaddr*)&server, sizeof(server) );
    close(sock);
    
    return bytes;
}

