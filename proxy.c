/*
 * Starter code for proxy lab.
 * Feel free to modify this code in whatever way you wish.
 */

/* Some useful includes to help you get started */

#include "csapp.h"
#include "http_parser.h"

#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif

/*
 * Max cache and object sizes
 * You might want to move these to the file containing your cache implementation
 */
#define MAX_CACHE_SIZE (1024 * 1024)
#define MAX_OBJECT_SIZE (100 * 1024)
#define HOSTLEN 256
#define SERVLEN 8
#define MAX_URL_SIZE (100)

/*
 * String to use for the User-Agent header.
 */
static const char *header_user_agent = "User-Agent: Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20191101 Firefox/63.0.1\r\n";

/*
 * String to use for the Connection header.
 */
static const char *connection = "Connection: close\r\n";

/*
 * String to use for the Proxy-Connection header.
 */
static const char *proxy_connection = "Proxy-Connection: close\r\n";

/* Typedef for convenience */
typedef struct sockaddr SA;

/* Information about a connected client. */
typedef struct {
    struct sockaddr_in addr; // Socket address
    socklen_t addrlen;       // Socket address length
    int connfd;              // Client connection file descriptor
    char host[HOSTLEN];      // Client host
    char serv[SERVLEN];      // Client service (port)
} client_info;

/* use looped cache to keeps recently used Web objects in memory as key-value
 * storage */
typedef struct web {
    int ref_c;              // reference count for synchronization
    size_t obj_size;        // size of associated object
    char url[MAX_URL_SIZE]; // treat the url as a key
    char *obj_ptr;          // pointer to the web object
    struct web *prev;
    struct web *next;
} webinfo_t;

void serve(client_info *client);
void clienterror(int fd, const char *errnum, const char *shortmsg);
void *thread(void *vargp);
webinfo_t *find_url(const char *url, pthread_mutex_t mutex);
webinfo_t *insert_url(const char *url, pthread_mutex_t mutex);
bool link_obj(webinfo_t *web, char *obj_ptr, size_t obj_size,
              pthread_mutex_t mutex, bool fail_cached);

webinfo_t *web_start = NULL; // the start of webinfo_t
pthread_mutex_t mutex;

/** @brief find if the `url` is stored in the cache
 * @param url the url we are looking for inside the cache
 * @param mutex
 * @return return a pointer to the webinfo_t associated the url or NULL if no
 * found
 */
webinfo_t *find_url(const char *url, pthread_mutex_t mutex) {

    webinfo_t *webp;

    /* case 1: empty cache */
    if ((webp = web_start->next) == NULL) {
        return webp;
    }

    /* case 2: url is the first `web` object in the cache */
    if (strcmp(webp->url, url) == 0) {
        webp->ref_c += 1;
        return webp;
    }
    /* case 3: url not the first `web` object in the cache */
    webp = webp->next;
    while (webp != web_start->next) {
        if (strcmp(webp->url, url) == 0) {
            webp->ref_c += 1;

            /* Move the `web` to the head of the cache*/
            webp->prev->next = webp->next;
            webp->next->prev = webp->prev;

            webp->prev = web_start->next->prev;
            webp->next = web_start->next;

            web_start->next->prev->next = webp;
            web_start->next->prev = webp;
            web_start->next = webp;
            return webp;
        }
        webp = webp->next;
    }

    /* case 4: Not inside the cache */
    return NULL;
}

/** @brief insert `url` inside the cache
 *  @param `url` to be inserted inside the cache
 *  @param mutex
 *  @return a pointer to the newly inserted `url` if succeed; NULL if fail
 */
webinfo_t *insert_url(const char *url, pthread_mutex_t mutex) {
    /* case 1: `url` is already inside the cache */
    if (find_url(url, mutex) != NULL) {
        return NULL; // fail insertion since it's alreadt in the cache
    }
    /* case 2: insert `url` to the head of the cache */
    webinfo_t *web_url = calloc(1, sizeof(webinfo_t));
    memcpy(web_url->url, url, strlen(url) + 1);
    web_url->ref_c += 1;
    // empty cache
    if (web_start->next == NULL) {
        web_start->next = web_url;
        web_url->prev = web_url;
        web_url->next = web_url;
    } else {
        web_url->prev = web_start->next->prev;
        web_url->next = web_start->next;
        web_start->next->prev->next = web_url;
        web_start->next->prev = web_url;
        web_start->next = web_url;
    }

    return web_url;
}

/** @brief link the web object with `web` that has associated url in the cache
 * @param web the web to be linked with the object
 * @param obj_ptr pointer to the object
 * @param obj_size size of the object
 * @param mutex
 * @param fail_cached indication of whether the link is valid
 * @return true is link successfullty; otherwise false
 */
bool link_obj(webinfo_t *web, char *obj_ptr, size_t obj_size,
              pthread_mutex_t mutex, bool fail_cached) {

    size_t occupied = 0;
    size_t remain = 0;
    webinfo_t *web_p = web->next;

    // Sum the current total object size inside the cache
    do {
        if (web_p->obj_ptr)
            occupied += web_p->obj_size;
        web_p = web_p->next;
    } while (web_p != web->next);
    /* remain free space */
    remain = MAX_CACHE_SIZE - occupied;

    /* case 1: failed cache: Remove the newly added `web` */
    if (fail_cached) {
        /* only one web in cache */
        if (web_start->next->next == web_start->next) {
            web_start->next = NULL;
        } else {
            /* web to be removed was added at the beginning of the cache */
            if (web_start->next == web) {
                web_start->next = web->next;
            }
            web->prev->next = web->next;
            web->next->prev = web->prev;
        }
        free(web);
        return false;
    }
    /* remain cache size enough for storing obj_size */
    else if (obj_size <= remain) {
        web->obj_ptr = obj_ptr;
        web->obj_size = obj_size;
        return true;
        /* least-recently-used (LRU) eviction policy */
    } else {
        webinfo_t *last = web_start->next->prev;
        webinfo_t *web_newlast;
        size_t objsize_after = remain;
        do {
            if ((last->obj_size > 0) && (last->ref_c == 0)) {
                web_newlast = last->prev;
                objsize_after += last->obj_size;
                last->prev->next = last->next;
                last->next->prev = last->prev;
                /* free the LRU web obj */
                free(last->obj_ptr);
                free(last);
                if (web_newlast != last)
                    last = web_newlast;
            } else {
                last = last->prev;
            }
        } while (objsize_after < obj_size || last != web_start->next->prev);
        /* enough space to store new web obj after eviction */
        if (objsize_after >= obj_size) {
            web->obj_ptr = obj_ptr;
            web->obj_size = obj_size;
            return true;
            /* Still NOT ENOUGHT */
        } else {
            /* remove */
            if (web_start->next == web) {
                web_start->next = web->next;
            }
            web->prev->next = web->next;
            web->next->prev = web->prev;
            free(web);
            return false;
        }
    }
}

/* clienterror - returns an error message to the client */
void clienterror(int fd, const char *errnum, const char *shortmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;

    /* Build the HTTP response body */
    bodylen = snprintf(body, MAXBUF,
                       "<!DOCTYPE html>\r\n"
                       "<html>\r\n"
                       "<head><title>Proxy Error</title></head>\r\n"
                       "<body bgcolor=\"ffffff\">\r\n"
                       "<h1>%s: %s</h1>\r\n"
                       "<hr /><em>The Proxy server</em>\r\n"
                       "</body></html>\r\n",
                       errnum, shortmsg);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }

    /* Build the HTTP response headers */
    buflen = snprintf(buf, MAXLINE,
                      "HTTP/1.0 %s %s\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      errnum, shortmsg, bodylen);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }

    /* Write the headers */
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }

    /* Write the body */
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}

/*
 * serve - handle one HTTP request/response transaction
 */
void serve(client_info *client) {
    // Get some extra info about the client (hostname/port)
    int res = getnameinfo((SA *)&client->addr, client->addrlen, client->host,
                          sizeof(client->host), client->serv,
                          sizeof(client->serv), 0);
    if (res == 0) {
        sio_printf("Accepted connection from %s:%s\n", client->host,
                   client->serv);
    } else {
        fprintf(stderr, "getnameinfo failed: %s\n", gai_strerror(res));
    }

    rio_t rio;
    rio_readinitb(&rio, client->connfd);
    rio_t rio_s;

    /* Read request line */
    char buf[MAXLINE];
    if (rio_readlineb(&rio, buf, MAXLINE) <= 0) {
        return;
    }

    const char *method;
    const char *host;
    const char *uri;
    const char *port;
    const char *version;
    int clientfd = -1;
    bool from_cache = true;
    /* create a new instance of a parser struct */
    parser_t *p = parser_new();
    /** use parser_parse_line to get the state of the parser
     * and `parser_retrieve`  to fetch a specific value
     */
    if (parser_parse_line(p, buf) == REQUEST) { // parsed request line
        if (parser_retrieve(p, METHOD, &method) < 0) {
            return;
        }
        if (parser_retrieve(p, HOST, &host) < 0) {
            return;
        }
        if (parser_retrieve(p, URI, &uri) < 0) {
            return;
        }
        if (parser_retrieve(p, PORT, &port) < 0) {
            return;
        }
        if (parser_retrieve(p, HTTP_VERSION, &version) < 0) {
            return;
        }

        /* request made to server */
        char req_server[MAXLINE];
        /* Error handling */
        if (strcmp(method, "POST") == 0) { /* respond to a POST request with the
                                            501 Not Implemented status code. */
            clienterror(client->connfd, "501", "Not Implemented");
            return;
        }
        /* if found, return obj from cache */
        webinfo_t *web_cached;
        if ((web_cached = find_url(uri, mutex)) != NULL) {
            from_cache = true;
            rio_writen(client->connfd, web_cached->obj_ptr,
                       web_cached->obj_size);
            pthread_mutex_lock(&mutex);
            web_cached->ref_c -= 1;
            pthread_mutex_unlock(&mutex);
        } else {
            from_cache = false;
            /* Open connection to server at <hostname, port> */
            clientfd = open_clientfd(host, port);
            if (clientfd == -1) {
                fprintf(stderr,
                        "Failed to open connection to server at: %s:%s\n", host,
                        port);
                return;
            }
            rio_readinitb(&rio_s, clientfd);
            // parse the request
            sprintf(req_server, "%s %s HTTP/1.0\r\n", method, uri);
            rio_writen(clientfd, req_server, strlen(req_server));
        }
    }
    // adapted from echo.c 	Mar.29	Network Programming (Part
    // I).pptx
    /*  forward client additional request header to server unchanged */
    if (!from_cache) {
        /* Header */
        char header[MAXLINE];
        // Request headers
        sprintf(header, "HOST: %s:%s\r\n", host, port);
        sprintf(header, "%s%s", header, header_user_agent);
        sprintf(header, "%s%s", header, connection);
        sprintf(header, "%s%s", header, proxy_connection);
        rio_writen(clientfd, header, strlen(header));
        char header_rest[MAXLINE];
        size_t n;
        while ((n = rio_readlineb(&rio, header_rest, MAXLINE)) > 0) {
            rio_writen(clientfd, header_rest, n);
            if (*header_rest == '\r')
                break;
        }
        char client_buf[MAXLINE];
        char *obj_ptr1 = (char *)malloc(MAX_OBJECT_SIZE);
        size_t obj_size;
        char *obj_ptr2 = obj_ptr1;
        bool fail_cached = false;
        size_t obj_cached = 0;

        while ((obj_size = rio_readnb(&rio_s, client_buf, MAXLINE)) > 0) {
            rio_writen(client->connfd, client_buf, obj_size);
            obj_cached += obj_size;
            /* cashe the object if it doesn't exceed MAX_OBJECT_SIZE */
            if (obj_cached <= MAX_OBJECT_SIZE) {
                memcpy(obj_ptr2, client_buf, obj_size);
                obj_ptr2 += obj_size;
            } else {
                fail_cached = true;
            }
        }

        pthread_mutex_lock(&mutex);
        /* cache uri */
        webinfo_t *web;
        bool success_link = false;
        if ((web = insert_url(uri, mutex)) != NULL) {
            success_link =
                link_obj(web, obj_ptr1, obj_cached, mutex, fail_cached);
        }
        pthread_mutex_unlock(&mutex);

        if (success_link) {
            pthread_mutex_lock(&mutex);
            web->ref_c -= 1;
            pthread_mutex_unlock(&mutex);
        }
        close(clientfd);
    }
    parser_free(p);
}

/* Thread routine */
//  Code adapted from echoservert.c found in Apr.5 Concurrent
//  programming.pptx
void *thread(void *vargp) {
    client_info *client = malloc(sizeof(client_info));
    client->connfd = *((int *)vargp);
    pthread_detach(pthread_self());
    /* Free storage allocated to hold connfd */
    free(vargp);
    /* Connection is established; serve client */
    serve(client);
    close(client->connfd);
    free(client); //
    return NULL;
}

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    pthread_t tid;
    /* Ignore sigpipe signal */
    signal(SIGPIPE, SIG_IGN);
    web_start = calloc(1, sizeof(webinfo_t));
    web_start->prev = NULL;
    web_start->next = NULL;
    pthread_mutex_init(&mutex, NULL);
    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "Failed to listen on port: %s\n", argv[1]);
        exit(1);
    }
    while (1) {
        /* Allocate space on the stack for client info */
        client_info client_data;
        client_info *client = &client_data;

        /* Initialize the length of the address */
        client->addrlen = sizeof(client->addr);
        connfdp = malloc(sizeof(int));
        /* accept() will block until a client connects to the port */
        *connfdp = accept(listenfd, (SA *)&client->addr, &client->addrlen);
        if (*connfdp < 0) {
            perror("accept");
            continue;
        }
        pthread_create(&tid, NULL, thread, connfdp);
    }
    free(web_start);
}
