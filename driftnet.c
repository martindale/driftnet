/*
 * driftnet.c:
 * Pick out images from passing network traffic.
 *
 * Copyright (c) 2001 Chris Lightfoot. All rights reserved.
 * Email: chris@ex-parrot.com; WWW: http://www.ex-parrot.com/~chris/
 *
 */

static const char rcsid[] = "$Id: driftnet.c,v 1.32 2003/10/16 11:56:37 chris Exp $";

#ifdef HAVE_CONFIG_H
    #include <config.h>
#endif

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pcap.h>

#include <netinet/in.h> /* needs to be before <arpa/inet.h> on OpenBSD */
#include <arpa/inet.h>
#ifdef HAVE_LIMITS_H
    #include <limits.h>
#endif

#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h> /* On many systems (Darwin...), stdio.h is a prerequisite. */
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include <sys/wait.h>

#include "log.h"
#include "tmpdir.h"
#include "driftnet.h"

#define SNAPLEN 262144      /* largest chunk of data we accept from pcap */
#define WRAPLEN 262144      /* out-of-order packet margin */

/* slots for storing information about connections */
connection *slots;
unsigned int slotsused, slotsalloc;

/* flags: verbose, adjunct mode, temporary directory to use, media types to
 * extract, beep on image. */
int extract_images = 1;
int verbose, adjunct, beep;
int tmpdir_specified;
char *tmpdir;
int max_tmpfiles;

enum mediatype extract_type = m_image;

/* ugh. */
pcap_t *pc;

#ifndef NO_DISPLAY_WINDOW
/* PID of display child and file descriptor on pipe to same. */
pid_t dpychld;
int dpychld_fd;

/* display.c */
int dodisplay(int argc, char *argv[]);
#endif /* !NO_DISPLAY_WINDOW */

/* playaudio.c */
void do_mpeg_player(void);

/* alloc_connection:
 * Find a free slot in which to allocate a connection object. */
connection *alloc_connection(void) {
    connection *C;
    for (C = slots; C < slots + slotsalloc; ++C) {
        if (!*C) return C;
    }
    /* No connection slots left. */
    slots = (connection*)xrealloc(slots, slotsalloc * 2 * sizeof(connection));
    memset(slots + slotsalloc, 0, slotsalloc * sizeof(connection));
    C = slots + slotsalloc;
    slotsalloc *= 2;
    return C;
}

/* find_connection:
 * Find a connection running between the two named addresses. */
connection *find_connection(const struct in_addr *src, const struct in_addr *dst, const short int sport, const short int dport) {
    connection *C;
    for (C = slots; C < slots + slotsalloc; ++C) {
        connection c = *C;
        if (c && c->sport == sport && c->dport == dport
            && memcmp(&(c->src), src, sizeof(struct in_addr)) == 0
            && memcmp(&(c->dst), dst, sizeof(struct in_addr)) == 0)
            return C;
    }
    return NULL;
}


/* sweep_connections:
 * Free finished connection slots. */
#define TIMEOUT 5
#define MAXCONNECTIONDATA   (8 * 1024 * 1024)

void sweep_connections(void) {
    time_t now;
    connection *C;
    now = time(NULL);
    for (C = slots; C < slots + slotsalloc; ++C) {
        if (*C) {
            connection c = *C;
            /* We discard connections which have seen no activity for TIMEOUT
             * or for which a FIN has been seen and for which there are no
             * gaps in the stream, or where more than MAXCONNECTIONDATA have
             * been captured. */
            if ((now - c->last) > TIMEOUT
                || (c->fin && (!c->blocks || !c->blocks->next))
                || c->len > MAXCONNECTIONDATA) {
                connection_extract_media(c, extract_type);
                connection_delete(c);
                *C = NULL;
            }
        }
    }
}

/* dump_data:
 * Print some binary data on a file descriptor. */
void dump_data(FILE *fp, const unsigned char *data, const unsigned int len) {
    const unsigned char *p;
    for (p = data; p < data + len; ++p) {
        if (isprint((int)*p)) fputc(*p, fp);
        else fprintf(fp, "\\x%02x", (unsigned int)*p);
    }
}

/* get_link_level_hdr_length:
 * Find out how long the link-level header is, based on the datalink layer
 * type. This is based on init_linktype in the libpcap distribution; I
 * don't know why libpcap doesn't expose the information directly. The
 * constants here are taken from 0.6.2, but I've added #ifdefs in the hope
 * that it will still compile with earlier versions. */
int get_link_level_hdr_length(int type)
{
    switch (type) {
        case DLT_EN10MB:
            return 14;

        case DLT_SLIP:
            return 16;

        case DLT_SLIP_BSDOS:
            return 24;

        case DLT_NULL:
#ifdef DLT_LOOP
        case DLT_LOOP:
#endif
            return 4;

        case DLT_PPP:
#ifdef DLT_C_HDLC
        case DLT_C_HDLC:
#endif
#ifdef DLT_PPP_SERIAL
        case DLT_PPP_SERIAL:
#endif
            return 4;

        case DLT_PPP_BSDOS:
            return 24;

        case DLT_FDDI:
            return 21;

        case DLT_IEEE802:
            return 22;
            
#ifdef DLT_ATM_RFC1483
        case DLT_ATM_RFC1483:
            return 8;
#endif

#ifdef DLT_PRISM_HEADER
        case DLT_PRISM_HEADER:
            return 32;
#endif
            
        case DLT_RAW:
            return 0;

#ifdef DLT_ATM_CLIP
        case DLT_ATM_CLIP:	/* Linux ATM defines this */
            return 8;
#endif

#ifdef DLT_LINUX_SLL
        case DLT_LINUX_SLL:	/* fake header for Linux cooked socket */
            return 16;
#endif

#ifdef DLT_IEEE802_11           /* 802.11 wireless ethernet */
        case DLT_IEEE802_11:
            return 32; /* 20030606 email from Nikhil Bobb */ /*44; */
#endif
            
        default:;
    }
    log_msg(LOG_ERROR, "unknown data link type %d", type);
    exit(1);
}


/* usage:
 * Print usage information. */
void usage(FILE *fp) {
    fprintf(fp,
"driftnet, version %s\n"
"Capture images from network traffic and display them in an X window.\n"
#ifdef NO_DISPLAY_WINDOW
"\n"
"Actually, this version of driftnet was compiled with the NO_DISPLAY_WINDOW\n"
"option, so that it can only be used in adjunct mode. See below.\n"
#endif /* NO_DISPLAY_WINDOW */
"\n"
"Synopsis: driftnet [options] [filter code]\n"
"\n"
"Options:\n"
"\n"
"  -h               Display this help message.\n"
"  -v               Verbose operation.\n"
"  -b               Beep when a new image is captured.\n"
"  -i interface     Select the interface on which to listen (default: all\n"
"                   interfaces).\n"
"  -f file          Instead of listening on an interface, read captured\n"
"                   packets from a pcap dump file; file can be a named pipe\n"
"                   for use with Kismet or similar.\n"
"  -p               Do not put the listening interface into promiscuous mode.\n"
"  -a               Adjunct mode: do not display images on screen, but save\n"
"                   them to a temporary directory and announce their names on\n"
"                   standard output.\n"
"  -m number        Maximum number of images to keep in temporary directory\n"
"                   in adjunct mode.\n"
"  -d directory     Use the named temporary directory.\n"
"  -x prefix        Prefix to use when saving images.\n"
"  -s               Attempt to extract streamed audio data from the network,\n"
"                   in addition to images. At present this supports MPEG data\n"
"                   only.\n"
"  -S               Extract streamed audio but not images.\n"
"  -M command       Use the given command to play MPEG audio data extracted\n"
"                   with the -s option; this should process MPEG frames\n"
"                   supplied on standard input. Default: `mpg123 -'.\n"
"\n"
"Filter code can be specified after any options in the manner of tcpdump(8).\n"
"The filter code will be evaluated as `tcp and (user filter code)'\n"
"\n"
"You can save images to the current directory by clicking on them.\n"
"\n"
"Adjunct mode is designed to be used by other programs which want to use\n"
"driftnet to gather images from the network. With the -m option, driftnet will\n"
"silently drop images if more than the specified number of images are saved\n"
"in its temporary directory. It is assumed that some other process is\n"
"collecting and deleting the image files.\n"
"\n"
"driftnet, copyright (c) 2001-2 Chris Lightfoot <chris@ex-parrot.com>\n"
"home page: http://www.ex-parrot.com/~chris/driftnet/\n"
"\n"
"This program is free software; you can redistribute it and/or modify\n"
"it under the terms of the GNU General Public License as published by\n"
"the Free Software Foundation; either version 2 of the License, or\n"
"(at your option) any later version.\n"
"\n",
            DRIFTNET_VERSION);
}

/* terminate_on_signal:
 * Terminate on receipt of an appropriate signal. */
sig_atomic_t foad;

void terminate_on_signal(int s) {
    extern pid_t mpeg_mgr_pid; /* in playaudio.c */
    /* Pass on the signal to the MPEG player manager so that it can abort,
     * since it won't die when the pipe into it dies. */
    if (mpeg_mgr_pid)
        kill(mpeg_mgr_pid, s);
    foad = s;
}

/* setup_signals:
 * Set up signal handlers. */
void setup_signals(void) {
    int *p;
    /* Signals to ignore. */
    int ignore_signals[] = {SIGPIPE, 0};
    /* Signals which mean we should quit, killing the display child if
     * applicable. */
    int terminate_signals[] = {SIGTERM, SIGINT, /*SIGSEGV,*/ SIGBUS, SIGCHLD, 0};
    struct sigaction sa;

    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    
    for (p = ignore_signals; *p; ++p) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = SIG_IGN;
        sigaction(*p, &sa, NULL);
    }

    for (p = terminate_signals; *p; ++p) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = terminate_on_signal;
        sigaction(*p, &sa, NULL);
    }
}

/* connection_string:
 * Return a string of the form w.x.y.z:foo -> a.b.c.d:bar for a pair of
 * addresses and ports. */
char *connection_string(const struct in_addr s, const unsigned short s_port, const struct in_addr d, const unsigned short d_port) {
    static char buf[50] = {0};
    sprintf(buf, "%s:%d -> ", inet_ntoa(s), (int)s_port);
    sprintf(buf + strlen(buf), "%s:%d", inet_ntoa(d), (int)d_port);
    return buf;
}

/* process_packet:
 * Callback which processes a packet captured by libpcap. */
int pkt_offset; /* offset of IP packet within wire packet */

void process_packet(u_char *user, const struct pcap_pkthdr *hdr, const u_char *pkt) {
    struct ip ip;
    struct tcphdr tcp;
    struct in_addr s, d;
    int off, len, delta;
    connection *C, c;

    log_msg(LOG_INFO, ".");

    memcpy(&ip, pkt + pkt_offset, sizeof(ip));
    memcpy(&s, &ip.ip_src, sizeof(ip.ip_src));
    memcpy(&d, &ip.ip_dst, sizeof(ip.ip_dst));

    memcpy(&tcp, pkt + pkt_offset + (ip.ip_hl << 2), sizeof(tcp));
    off = pkt_offset + (ip.ip_hl << 2) + (tcp.th_off << 2);
    len = hdr->caplen - off;

    /* XXX fragmented packets and other nasties. */
    
    /* try to find the connection slot associated with this. */
    C = find_connection(&s, &d, ntohs(tcp.th_sport), ntohs(tcp.th_dport));

    /* no connection at all, so we need to allocate one. */
    if (!C) {
        log_msg(LOG_INFO, "new connection: %s", connection_string(s, ntohs(tcp.th_sport), d, ntohs(tcp.th_dport)));
        C = alloc_connection();
        *C = connection_new(&s, &d, ntohs(tcp.th_sport), ntohs(tcp.th_dport));
        /* This might or might not be an entirely new connection (SYN flag
         * set). Either way we need a sequence number to start at. */
        (*C)->isn = ntohl(tcp.th_seq);
    }

    /* Now we need to process this segment. */
    c = *C;
    delta = 0;/*tcp.syn ? 1 : 0;*/

    /* NB (STD0007):
     *    SEG.LEN = the number of octets occupied by the data in the
     *    segment (counting SYN and FIN) */
#if 0
    if (tcp.syn)
        /* getting a new isn. */
        c->isn = htonl(tcp.seq);
#endif

    if (tcp.th_flags & TH_RST) {
        /* Looks like this connection is bogus, and so might be a
         * connection going the other way. */
        log_msg(LOG_INFO, "connection reset: %s", connection_string(s, ntohs(tcp.th_sport), d, ntohs(tcp.th_dport)));
        
        connection_delete(c);
        *C = NULL;

        if ((C = find_connection(&d, &s, ntohs(tcp.th_dport), ntohs(tcp.th_sport)))) {
            connection_delete(*C);
            *C = NULL;
        }

        return;
    }

    if (len > 0) {
        /* We have some data in the packet. If this data occurred after
         * the first data we collected for this connection, then save it
         * so that we can look for images. Otherwise, discard it. */
        unsigned int offset;
        
        offset = ntohl(tcp.th_seq);

        /* Modulo 2**32 arithmetic; offset = seq - isn + delta. */
        if (offset < (c->isn + delta))
            offset = 0xffffffff - (c->isn + delta - offset);
        else
            offset -= c->isn + delta;
        
        if (offset > c->len + WRAPLEN) {
            /* Out-of-order packet. */
            log_msg(LOG_INFO, "out of order packet: %s", connection_string(s, ntohs(tcp.th_sport), d, ntohs(tcp.th_dport)));
        } else {
            connection_push(c, pkt + off, offset, len);
            connection_extract_media(c, extract_type);
        }
    }
    if (tcp.th_flags & TH_FIN) {
        /* Connection closing; mark it as closed, but let sweep_connections
         * free it if appropriate. */
        log_msg(LOG_INFO, "connection closing: %s, %d bytes transferred", connection_string(s, ntohs(tcp.th_sport), d, ntohs(tcp.th_dport)), c->len);
        c->fin = 1;
    }

    /* sweep out old connections */
    sweep_connections();
}

/* packet_capture_thread:
 * Thread in which packet capture runs. */
void *packet_capture_thread(void *v) {
    while (!foad)
        pcap_dispatch(pc, -1, process_packet, NULL);
    return NULL;
}

/* main:
 * Entry point. Process command line options, start up pcap and enter capture
 * loop. */
char optstring[] = "abd:f:hi:M:m:pSsvx:";

int main(int argc, char *argv[]) {
    char *interface = NULL, *filterexpr = NULL;
    int promisc = 1;
    struct bpf_program filter;
    char ebuf[PCAP_ERRBUF_SIZE];
    int c;
#ifndef NO_DISPLAY_WINDOW
    extern char *savedimgpfx;       /* in display.c */
#endif
    extern char *audio_mpeg_player; /* in playaudio.c */
    int newpfx = 0;
    int mpeg_player_specified = 0;
    char *dumpfile = NULL;
    
    pthread_t packetth;
    connection *C;

    /* Handle command-line options. */
    opterr = 0;
    while ((c = getopt(argc, argv, optstring)) != -1) {
        switch(c) {
            case 'h':
                usage(stdout);
                return 0;

            case 'i':
                if (dumpfile) {
                    log_msg(LOG_ERROR, "can't specify -i and -f");
                    return -1;
                }
                interface = optarg;
                break;

            case 'v':
                verbose = 1;
                break;

            case 'b':
                if (!isatty(1))
                    log_msg(LOG_ERROR, "can't beep unless standard output is a terminal");
                else 
                    beep = 1;
                break;

            case 'p':
                promisc = 0;
                break;

            case 's':
                extract_type |= m_audio;
                break;

            case 'S':
                extract_type = m_audio;
                break;

            case 'M':
                audio_mpeg_player = optarg;
                mpeg_player_specified = 1;
                break;

            case 'a':
                adjunct = 1;
                break;

            case 'm':
                max_tmpfiles = atoi(optarg);
                if (max_tmpfiles <= 0) {
                    log_msg(LOG_ERROR, "`%s' does not make sense for -m", optarg);
                    return -1;
                }
                break;

            case 'd':
                tmpdir = optarg;
                tmpdir_specified = 1; /* so we don't delete it. */
                break;

            case 'f':
                if (interface) {
                    log_msg(LOG_ERROR, "can't specify -i and -f");
                    return -1;
                }
                dumpfile = optarg;
                break;

#ifndef NO_DISPLAY_WINDOW
            case 'x':
                savedimgpfx = optarg;
                newpfx = 1;
                break;
#endif

            case '?':
            default:
                if (strchr(optstring, optopt))
                    log_msg(LOG_ERROR, "option -%c requires an argument", optopt);
                else
                    log_msg(LOG_ERROR, "unrecognised option -%c", optopt);
                usage(stderr);
                return 1;
        }
    }

    if (verbose) {
        set_loglevel(LOG_INFO);
    }
    
#ifdef NO_DISPLAY_WINDOW
    if (!adjunct) {
        log_msg(LOG_ERROR, "this version of driftnet was compiled without display support");
        log_msg(LOG_ERROR, "use the -a option to run it in adjunct mode");
        return -1;
    }
#endif /* !NO_DISPLAY_WINDOW */
    
    /* Let's not be too fascist about option checking.... */
    if (max_tmpfiles && !adjunct) {
        log_msg(LOG_WARNING, "-m only makes sense with -a");
        max_tmpfiles = 0;
    }

    if (adjunct && newpfx)
        log_msg(LOG_WARNING, "-x ignored -a");

    if (mpeg_player_specified && !(extract_type & m_audio))
        log_msg(LOG_WARNING, "-M only makes sense with -s");

    if (mpeg_player_specified && adjunct)
        log_msg(LOG_WARNING, "-M ignored with -a");

    if (max_tmpfiles && adjunct)
        log_msg(LOG_INFO, "a maximum of %d images will be buffered", max_tmpfiles);

    if (beep && adjunct)
        log_msg(LOG_WARNING, "can't beep in adjunct mode");

    /* In adjunct mode, it's important that the attached program gets
     * notification of images in a timely manner. Make stdout line-buffered
     * for this reason. */
    if (adjunct)
        setvbuf(stdout, NULL, _IOLBF, 0);

    /* If a directory name has not been specified, then we need to create one.
     * Otherwise, check that it's a directory into which we may write files. */
    if (tmpdir) {
        check_dir_is_rw(tmpdir);
        set_tmpdir(tmpdir, TMPDIR_USER_OWNED, max_tmpfiles);

    } else {
        /* need to make a temporary directory. */
        set_tmpdir(make_tmpdir(), TMPDIR_APP_OWNED, max_tmpfiles);
    }

    log_msg(LOG_INFO, "using temporary file directory %s", get_tmpdir());

    if (!interface && !(interface = pcap_lookupdev(ebuf))) {
        log_msg(LOG_ERROR, "pcap_lookupdev: %s", ebuf);
        log_msg(LOG_ERROR, "try specifying an interface with -i");
        return -1;
    }

    log_msg(LOG_INFO, "listening on %s%s", interface ? interface : "all interfaces", promisc ? " in promiscuous mode" : "");

    /* Build up filter. */
    if (optind < argc) {
        if (dumpfile)
            log_msg(LOG_WARNING, "filter code ignored with dump file");
        else {
            char **a;
            int l;
            for (a = argv + optind, l = sizeof("tcp and ()"); *a; l += strlen(*a) + 1, ++a);
            filterexpr = calloc(l, 1);
            strcpy(filterexpr, "tcp and (");
            for (a = argv + optind; *a; ++a) {
                strcat(filterexpr, *a);
                if (*(a + 1)) strcat(filterexpr, " ");
            }
            strcat(filterexpr, ")");
        }
    } else filterexpr = "tcp";

    log_msg(LOG_INFO, "using filter expression `%s'", filterexpr);
    

#ifndef NO_DISPLAY_WINDOW
    if (newpfx && !adjunct)
        log_msg(LOG_INFO, "using saved image prefix `%s'", savedimgpfx);
#endif

    setup_signals();
    
    /* Start up the audio player, if required. */
    if (!adjunct && (extract_type & m_audio))
        do_mpeg_player();
    
#ifndef NO_DISPLAY_WINDOW
    /* Possibly fork to start the display child process */
    if (!adjunct && (extract_type & m_image)) {
        int pfd[2];
        pipe(pfd);
        switch (dpychld = fork()) {
            case 0:
                /* we are the child */
                close(pfd[1]);
                dpychld_fd = pfd[0];
                dodisplay(argc, argv);
                return -1;

            case -1:
                perror(PROGNAME "fork");
                return -1;

            default:
                close(pfd[0]);
                dpychld_fd = pfd[1];
                log_msg(LOG_INFO, "started display child, pid %d", (int)dpychld);
                break;
        }
    } else log_msg(LOG_INFO, "operating in adjunct mode");
#endif /* !NO_DISPLAY_WINDOW */
 
    /* Start up pcap. */
    if (dumpfile) {
        if (!(pc = pcap_open_offline(dumpfile, ebuf))) {
            log_msg(LOG_ERROR, "pcap_open_offline: %s", ebuf);
            return -1;
        }   
    } else {
        if (!(pc = pcap_open_live(interface, SNAPLEN, promisc, 1000, ebuf))) {
            log_msg(LOG_ERROR, "pcap_open_live: %s", ebuf);

            if (getuid() != 0)
                log_msg(LOG_ERROR, "perhaps you need to be root?");
            else if (!interface)
                log_msg(LOG_ERROR, "perhaps try selecting an interface with the -i option?");
                
            return -1;
        }
    
        /* Only apply a filter to live packets. Is this right? */
        if (pcap_compile(pc, &filter, (char*)filterexpr, 1, 0) == -1) {
            log_msg(LOG_ERROR, "pcap_compile: %s", pcap_geterr(pc));
            return -1;
        }
        
        if (pcap_setfilter(pc, &filter) == -1) {
            log_msg(LOG_ERROR, "pcap_setfilter: %s", pcap_geterr(pc));
            return -1;
        }
    }

    /* Figure out the offset from the start of a returned packet to the data in
     * it. */
    pkt_offset = get_link_level_hdr_length(pcap_datalink(pc));
    log_msg(LOG_INFO, "link-level header length is %d bytes", pkt_offset);

    slotsused = 0;
    slotsalloc = 64;
    slots = (connection*)xcalloc(slotsalloc, sizeof(connection));

    /* Actually start the capture stuff up. Unfortunately, on many platforms,
     * libpcap doesn't have read timeouts, so we start the thing up in a
     * separate thread. Yay! */
    pthread_create(&packetth, NULL, packet_capture_thread, NULL);

    while (!foad)
        sleep(1);

    if (verbose) {
        if (foad == SIGCHLD) {
            pid_t pp;
            int st;
            while ((pp = waitpid(-1, &st, WNOHANG)) > 0) {
                if (WIFEXITED(st))
                    log_msg(LOG_INFO, "child process %d exited with status %d", (int)pp, WEXITSTATUS(st));
                else if (WIFSIGNALED(st))
                    log_msg(LOG_INFO, "child process %d killed by signal %d", (int)pp, WTERMSIG(st));
                else
                    log_msg(LOG_INFO, "child process %d died, not sure why", (int)pp);
            }
            
        } else
            log_msg(LOG_INFO, "caught signal %d", foad);
    }
    
    pthread_cancel(packetth); /* make sure thread quits even if it's stuck in pcap_dispatch */
    pthread_join(packetth, NULL);
    
    /* Clean up. */
/*    pcap_freecode(pc, &filter);*/ /* not on some systems... */
    pcap_close(pc);
    clean_tmpdir();

    /* Easier for memory-leak debugging if we deallocate all this here.... */
    for (C = slots; C < slots + slotsalloc; ++C)
        if (*C) connection_delete(*C);
    xfree(slots);

    return 0;
}
