/*
 * SWO Top for Blackmagic Probe and TTL Serial Interfaces
 * ======================================================
 *
 * Copyright (C) 2017, 2019  Dave Marples  <dave@marples.net>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the names Orbtrace, Orbuculum nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <elf.h>
#include <demangle.h>
#include "bfd_wrapper.h"
#if defined OSX
    #include <libusb.h>
#else
    #if defined LINUX
        #include <libusb-1.0/libusb.h>
    #else
    #include <libusb.h>
//#error "Unknown OS"
    #endif
#endif
#include <stdint.h>
#include <limits.h>
#include <termios.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "generics.h"
#include "uthash.h"
#include "git_version_info.h"
#include "generics.h"
#include "tpiuDecoder.h"
#include "itmDecoder.h"
#include "symbols.h"

#define CUTOFF              (10)             /* Default cutoff at 0.1% */
#define SERVER_PORT         (3443)           /* Server port definition */
#define TRANSFER_SIZE       (4096)           /* Maximum packet we might receive */
#define TOP_UPDATE_INTERVAL (1000)           /* Interval between each on screen update */


struct visitedAddr                           /* Structure for Hashmap of visited/observed addresses */
{
    uint64_t visits;
    struct nameEntry *n;

    UT_hash_handle hh;
};

struct reportLine

{
    uint64_t count;
    struct nameEntry *n;
};


/* ---------- CONFIGURATION ----------------- */
struct                                       /* Record for options, either defaults or from command line */
{
    bool useTPIU;                            /* Are we decoding via the TPIU? */
    bool json;                               /* Output in JSON format rather than human readable */
    bool reportFilenames;                    /* Report filenames for each routine? -- not presented via UI, intended for debug */
    uint32_t tpiuITMChannel;                 /* What channel? */
    bool forceITMSync;                       /* Must ITM start synced? */

    uint32_t hwOutputs;                      /* What hardware outputs are enabled */

    char *deleteMaterial;                    /* Material to delete off filenames for target */

    char *elffile;                           /* Target program config */

    char *outfile;                           /* File to output current information */
    char *logfile;                           /* File to output historic information */

    uint32_t cutscreen;                      /* Cut screen output after specified number of lines */
    uint32_t maxRoutines;                    /* Historic information to emit */
    bool lineDisaggregation;                 /* Aggregate per line or per function? */
    bool demangle;                           /* Do we want to demangle any C++ we come across? */
    uint64_t displayInterval;                /* What is the display interval? */

    int port;                                /* Source information */
    char *server;

} options =
{
    .forceITMSync = true,
    .useTPIU = false,
    .tpiuITMChannel = 1,
    .outfile = NULL,
    .logfile = NULL,
    .lineDisaggregation = false,
    .maxRoutines = 8,
    .demangle = true,
    .displayInterval = TOP_UPDATE_INTERVAL,
    .port = SERVER_PORT,
    .server = "localhost"
};

/* ----------- LIVE STATE ----------------- */
struct
{
    struct ITMDecoder i;                    /* The decoders and the packets from them */
    struct ITMPacket h;
    struct TPIUDecoder t;
    struct TPIUPacket p;

    struct SymbolSet *s;                    /* Symbols read from elf */
    struct nameEntry *n;                    /* Current table of recognised names */

    struct visitedAddr *addresses;         /* Addresses we received in the SWV */

    uint32_t interrupts;
    uint32_t sleeps;
    uint32_t notFound;
} _r;

// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Internally available routines
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================



// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Handler for individual message types from SWO
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _handleException( struct ITMDecoder *i, struct ITMPacket *p )

{

}
// ====================================================================================================
void _handleDWTEvent( struct ITMDecoder *i, struct ITMPacket *p )

{

}
// ====================================================================================================
void _handleSW( struct ITMDecoder *i )

{

}
// ====================================================================================================
uint64_t _timestamp( void )

{
    struct timeval te;
    gettimeofday( &te, NULL ); // get current time
    uint64_t milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000; // caculate milliseconds
    return milliseconds;
}
// ====================================================================================================
int _addresses_sort_fn( void *a, void *b )

{
    if ( ( ( ( struct visitedAddr * )a )->n->addr ) < ( ( ( struct visitedAddr * )b )->n->addr ) )
    {
        return -1;
    }

    if ( ( ( ( struct visitedAddr * )a )->n->addr ) > ( ( ( struct visitedAddr * )b )->n->addr ) )
    {
        return 1;
    }

    return 0;
}
// ====================================================================================================
int _routines_sort_fn( void *a, void *b )

{
    int r = strcmp( ( ( struct visitedAddr * )a )->n->filename, ( ( struct visitedAddr * )b )->n->filename );

    if ( r )
    {
        return r;
    }


    r = strcmp( ( ( struct visitedAddr * )a )->n->function, ( ( struct visitedAddr * )b )->n->function ) ;

    if ( r )
    {
        return r;
    }

    return ( ( int )( ( struct visitedAddr * )a )->n->line ) - ( ( int )( ( struct visitedAddr * )b )->n->line );
}
// ====================================================================================================
int _report_sort_fn( const void *a, const void *b )

{
    return ( ( struct reportLine * )b )->count - ( ( struct reportLine * )a )->count;
}
// ====================================================================================================
void outputTop( void )

/* Produce the output */

{
    struct nameEntry *n;
    struct visitedAddr *a;


    uint32_t reportLines = 0;
    struct reportLine *report = NULL;

    uint32_t total = 0;
    uint64_t samples = 0;
    uint64_t dispSamples = 0;
    uint32_t percentage;
    uint32_t totPercent = 0;

    FILE *p = NULL;
    FILE *q = NULL;

    /* Put the address into order of the file and function names */
    HASH_SORT( _r.addresses, _routines_sort_fn );

    /* Now merge them together */
    for ( a = _r.addresses; a != NULL; a = a->hh.next )
    {
        if ( !a->visits )
        {
            continue;
        }

        if ( ( reportLines == 0 ) ||
                ( strcmp( report[reportLines - 1].n->filename, a->n->filename ) ) ||
                ( strcmp( report[reportLines - 1].n->function, a->n->function ) ) ||
                ( ( report[reportLines - 1].n->line != a->n->line ) && ( options.lineDisaggregation ) ) )
        {
            /* Make room for a report line */
            reportLines++;
            report = ( struct reportLine * )realloc( report, sizeof( struct reportLine ) * ( reportLines ) );
            report[reportLines - 1].n = a->n;
            report[reportLines - 1].count = 0;
        }

        report[reportLines - 1].count += a->visits;
        total += a->visits;
        a->visits = 0;
    }


    /* Now fold in any sleeping entries */
    report = ( struct reportLine * )realloc( report, sizeof( struct reportLine ) * ( reportLines + 1 ) );

    uint32_t addr = SLEEPING;
    HASH_FIND_INT( _r.addresses, &addr, a );

    if ( a )
    {
        n = a->n;
    }
    else
    {
        n = ( struct nameEntry * )malloc( sizeof( struct nameEntry ) );
    }

    n->filename = "";
    n->function = "** SLEEPING **";
    n->addr = 0;
    n->line = 0;

    report[reportLines].n = n;
    report[reportLines].count = _r.sleeps;
    reportLines++;
    total += _r.sleeps;
    _r.sleeps = 0;

    /* Now put the whole thing into order of number of samples */
    qsort( report, reportLines, sizeof( struct reportLine ), _report_sort_fn );

    /* This is the file retaining the current samples */
    if ( options.outfile )
    {
        p = fopen( options.outfile, "w" );
    }

    /* This is the file containing the historic samples */
    if ( options.logfile )
    {
        q = fopen( options.logfile, "a" );
    }

    if (!options.json)
      {
	fprintf( stdout, "\033[2J\033[;H" );
      }
    else
      {
	/* Start of frame in JSON format */

      }

    if ( total )
    {
        for ( uint32_t n = 0; n < reportLines; n++ )
        {
            percentage = ( report[n].count * 10000 ) / total;
            samples += report[n].count;

            if ( report[n].count )
            {
                char *d = NULL;

                if ( ( options.demangle ) && ( !options.reportFilenames ) )
                {
                    d = cplus_demangle( report[n].n->function, DMGL_AUTO );
                }

                if ( ( percentage >= CUTOFF ) && ( ( !options.cutscreen ) || ( n < options.cutscreen ) ) )
                {
                    dispSamples += report[n].count;
                    totPercent += percentage;

		    if (!options.json)
		      {
			fprintf( stdout, "%3d.%02d%% %8ld ", percentage / 100, percentage % 100, report[n].count );


			if ( ( options.reportFilenames ) && ( report[n].n->filename ) )
			  {
			    fprintf( stdout, "%s::", report[n].n->filename );
			  }

			if ( ( options.lineDisaggregation ) && ( report[n].n->line ) )
			  {
			    fprintf( stdout, "%s::%d" EOL, d ? d : report[n].n->function, report[n].n->line );
			  }
			else
			  {
			    fprintf( stdout, "%s" EOL, d ? d : report[n].n->function );
			  }
		      }
		    else
		      {
			/* Output in JSON Format */
		      }
                }

                if ( ( p )  && ( n < options.maxRoutines ) && ( percentage >= CUTOFF ) )
                {
                    if ( !options.lineDisaggregation )
                    {
                        fprintf( p, "%s,%3d.%02d" EOL, d ? d : report[n].n->function, percentage / 100, percentage % 100 );
                    }
                    else
                    {
                        fprintf( p, "%s::%d,%3d.%02d" EOL, d ? d : report[n].n->function, report[n].n->line, percentage / 100, percentage % 100 );
                    }
                }

                if ( ( q )  && ( percentage >= CUTOFF ) )
                {
                    if ( !options.lineDisaggregation )
                    {
                        fprintf( q, "%s,%3d.%02d" EOL, d ? d : report[n].n->function, percentage / 100, percentage % 100 );
                    }
                    else
                    {
                        fprintf( q, "%s::%d,%3d.%02d" EOL, d ? d : report[n].n->function, report[n].n->line, percentage / 100, percentage % 100 );
                    }
                }

            }
        }
    }

    if (!options.json)
      {
	fprintf( stdout, "-----------------" EOL );

	if ( samples == dispSamples )
	  {
	    fprintf( stdout, "%3d.%02d%% %8ld Samples" EOL, totPercent / 100, totPercent % 100, samples );
	  }
	else
	  {
	    fprintf( stdout, "%3d.%02d%% %8ld of %ld Samples" EOL, totPercent / 100, totPercent % 100, dispSamples, samples );
	  }
      }
    else
      {
	/* Close off JSON report */

      }

    if ( p )
    {
        fclose( p );
    }

    if ( q )
    {
        fprintf( q, "===================================" EOL );
        fclose( q );
    }

    genericsReport( V_INFO, "         Ovf=%3d  ITMSync=%3d TPIUSync=%3d ITMErrors=%3d" EOL,
                    ITMDecoderGetStats( &_r.i )->overflow,
                    ITMDecoderGetStats( &_r.i )->syncCount,
                    TPIUDecoderGetStats( &_r.t )->syncCount,
                    ITMDecoderGetStats( &_r.i )->ErrorPkt );

    /* ... and we are done with the report now, get rid of it */
    free( report );
}
// ====================================================================================================
void _handlePCSample( struct ITMDecoder *i, struct ITMPacket *p )

{
    struct visitedAddr *a;
    uint32_t pc;

    if ( p->len == 1 )
    {
        /* This is a sleep packet */
        _r.sleeps++;
    }
    else
    {
        pc = ( p->d[3] << 24 ) | ( p->d[2] << 16 ) | ( p->d[1] << 8 ) | ( p->d[0] );

        HASH_FIND_INT( _r.addresses, &pc, a );

        if ( a )
        {
            a->visits++;
        }
        else
        {
            struct nameEntry n;

            /* Find a matching name record if there is one */
            SymbolLookup( _r.s, pc, &n, options.deleteMaterial );

            /* This is a new entry - record it */

            a = ( struct visitedAddr * )calloc( 1, sizeof( struct visitedAddr ) );
            a->visits = 1;

            a->n = ( struct nameEntry * )malloc( sizeof( struct nameEntry ) );
            memcpy( a->n, &n, sizeof( struct nameEntry ) );
            HASH_ADD_INT( _r.addresses, n->addr, a );
        }
    }
}
// ====================================================================================================
void _flushHash( void )

{
    struct visitedAddr *a;
    UT_hash_handle hh;

    for ( a = _r.addresses; a != NULL; a = hh.next )
    {
        hh = a->hh;
        free( a );
    }

    _r.addresses = NULL;
}
// ====================================================================================================
void _handleHW( struct ITMDecoder *i )

{
    struct ITMPacket p;
    ITMGetPacket( i, &p );

    switch ( p.srcAddr )
    {
        // --------------
        case 0: /* DWT Event */
            break;

        // --------------
        case 1: /* Exception */
            _handleException( i, &p );
            break;

        // --------------
        case 2: /* PC Counter Sample */
            _handlePCSample( i, &p );
            break;

        // --------------
        default:
            break;
            // --------------
    }
}
// ====================================================================================================
void _itmPumpProcess( char c )

/* Handle individual characters into the itm decoder */

{
    switch ( ITMPump( &_r.i, c ) )
    {
        // ------------------------------------
        case ITM_EV_NONE:
            break;

        // ------------------------------------
        case ITM_EV_UNSYNCED:
            genericsReport( V_WARN, "ITM Lost Sync (%d)" EOL, ITMDecoderGetStats( &_r.i )->lostSyncCount );
            break;

        // ------------------------------------
        case ITM_EV_SYNCED:
            genericsReport( V_WARN, "ITM In Sync (%d)" EOL, ITMDecoderGetStats( &_r.i )->syncCount );
            break;

        // ------------------------------------
        case ITM_EV_OVERFLOW:
            genericsReport( V_WARN, "ITM Overflow (%d)" EOL, ITMDecoderGetStats( &_r.i )->overflow );
            break;

        // ------------------------------------
        case ITM_EV_RESERVED_PACKET_RXED:
            genericsReport( V_INFO, "Reserved Packet Received" EOL );
            break;

        // ------------------------------------
        case ITM_EV_XTN_PACKET_RXED:
            genericsReport( V_INFO, "Unknown Extension Packet Received" EOL );
            break;

        // ------------------------------------
        case ITM_EV_ERROR:
            genericsReport( V_WARN, "ITM Error" EOL );
            break;

        // ------------------------------------
        case ITM_EV_TS_PACKET_RXED:
            break;

        // ------------------------------------
        case ITM_EV_SW_PACKET_RXED:
            _handleSW( &_r.i );
            break;

        // ------------------------------------
        case ITM_EV_HW_PACKET_RXED:
            _handleHW( &_r.i );
            break;

            // ------------------------------------
    }
}
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
// Protocol pump for decoding messages
// ====================================================================================================
// ====================================================================================================
// ====================================================================================================
void _protocolPump( uint8_t c )

/* Top level protocol pump */

{
    if ( options.useTPIU )
    {
        switch ( TPIUPump( &_r.t, c ) )
        {
            // ------------------------------------
            case TPIU_EV_NEWSYNC:
                genericsReport( V_INFO, "TPIU In Sync (%d)" EOL, TPIUDecoderGetStats( &_r.t )->syncCount );

            case TPIU_EV_SYNCED:
                ITMDecoderForceSync( &_r.i, true );
                break;

            // ------------------------------------
            case TPIU_EV_RXING:
            case TPIU_EV_NONE:
                break;

            // ------------------------------------
            case TPIU_EV_UNSYNCED:
                genericsReport( V_WARN, "TPIU Lost Sync (%d)" EOL, TPIUDecoderGetStats( &_r.t )->lostSync );
                ITMDecoderForceSync( &_r.i, false );
                break;

            // ------------------------------------
            case TPIU_EV_RXEDPACKET:
                if ( !TPIUGetPacket( &_r.t, &_r.p ) )
                {
                    genericsReport( V_WARN, "TPIUGetPacket fell over" EOL );
                }

                for ( uint32_t g = 0; g < _r.p.len; g++ )
                {
                    if ( _r.p.packet[g].s == options.tpiuITMChannel )
                    {
                        _itmPumpProcess( _r.p.packet[g].d );
                        continue;
                    }

                    if ( _r.p.packet[g].s != 0 )
                    {
                        genericsReport( V_WARN, "Unknown TPIU channel %02x" EOL, _r.p.packet[g].s );
                    }
                }

                break;

            // ------------------------------------
            case TPIU_EV_ERROR:
                genericsReport( V_WARN, "****ERROR****" EOL );
                break;
                // ------------------------------------
        }
    }
    else
    {
        /* There's no TPIU in use, so this goes straight to the ITM layer */
        _itmPumpProcess( c );
    }
}
// ====================================================================================================
void _printHelp( char *progName )

{
    fprintf( stdout, "Usage: %s <htv> <-e ElfFile> <-g filename> <-o filename> -r <routines> <-i channel> <-p port> <-s server>" EOL, progName );
    fprintf( stdout, "        c: <num> Cut screen output after number of lines" EOL );
    fprintf( stdout, "        d: <DeleteMaterial> to take off front of filenames" EOL );
    fprintf( stdout, "        D: Switch off C++ symbol demangling" EOL );
    fprintf( stdout, "        e: <ElfFile> to use for symbols" EOL );
    fprintf( stdout, "        g: <LogFile> append historic records to specified file" EOL );
    fprintf( stdout, "        h: This help" EOL );
    fprintf( stdout, "        i: <channel> Set ITM Channel in TPIU decode (defaults to 1)" EOL );
    fprintf( stdout, "        I: <interval> Display interval in milliseconds (defaults to %d mS)" EOL, TOP_UPDATE_INTERVAL );
    fprintf( stdout, "        j: Output in JSON format" EOL);
    fprintf( stdout, "        l: Aggregate per line rather than per function" EOL );
    fprintf( stdout, "        n: Enforce sync requirement for ITM (i.e. ITM needs to issue syncs)" EOL );
    fprintf( stdout, "        o: <filename> to be used for output live file" EOL );
    fprintf( stdout, "        r: <routines> to record in live file (default %d routines)" EOL, options.maxRoutines );
    fprintf( stdout, "        s: <Server>:<Port> to use" EOL );
    fprintf( stdout, "        t: Use TPIU decoder" EOL );
    fprintf( stdout, "        v: <level> Verbose mode 0(errors)..3(debug)" EOL );
}
// ====================================================================================================
int _processOptions( int argc, char *argv[] )

{
    int c;

    while ( ( c = getopt ( argc, argv, "c:d:De:g:hi:I:jlm:no:r:s:tv:" ) ) != -1 )
        switch ( c )
        {
            // ------------------------------------
            case 'c':
                options.cutscreen = atoi( optarg );
                break;

            // ------------------------------------
            case 'e':
                options.elffile = optarg;
                break;

            // ------------------------------------
            case 'g':
                options.logfile = optarg;
                break;

            // ------------------------------------
            case 'd':
                options.deleteMaterial = optarg;
                break;

            // ------------------------------------
            case 'D':
                options.demangle = false;
                break;

            // ------------------------------------
            case 'I':
                options.displayInterval = ( uint64_t ) ( atof( optarg ) );
                break;

            // ------------------------------------
            case 'j':
	      options.json = true;
                break;

		// ------------------------------------
            case 'l':
                options.lineDisaggregation = true;
                break;

            // ------------------------------------
            case 'r':
                options.maxRoutines = atoi( optarg );
                break;

            // ------------------------------------

            case 'n':
                options.forceITMSync = false;
                break;

            // ------------------------------------
            case 'o':
                options.outfile = optarg;
                break;

            // ------------------------------------
            case 'v':
                genericsSetReportLevel( atoi( optarg ) );
                break;

            // ------------------------------------
            case 't':
                options.useTPIU = true;
                break;

            // ------------------------------------
            case 'i':
                options.tpiuITMChannel = atoi( optarg );
                break;

            // ------------------------------------
            case 's':
                options.server = optarg;

                // See if we have an optional port number too
                char *a = optarg;

                while ( ( *a ) && ( *a != ':' ) )
                {
                    a++;
                }

                if ( *a == ':' )
                {
                    *a = 0;
                    options.port = atoi( ++a );
                }

                if ( !options.port )
                {
                    options.port = SERVER_PORT;
                }

                break;

            // ------------------------------------
            case 'h':
                _printHelp( argv[0] );
                return false;

            // ------------------------------------
            case '?':
                if ( optopt == 'b' )
                {
                    genericsReport( V_ERROR, "Option '%c' requires an argument." EOL, optopt );
                }
                else if ( !isprint ( optopt ) )
                {
                    genericsReport( V_ERROR, "Unknown option character `\\x%x'." EOL, optopt );
                }

                return false;

            // ------------------------------------
            default:
                genericsReport( V_ERROR, "Unknown option %c" EOL, optopt );
                return false;
                // ------------------------------------
        }

    if ( ( options.useTPIU ) && ( !options.tpiuITMChannel ) )
    {
        genericsReport( V_ERROR, "TPIU set for use but no channel set for ITM output" EOL );
        return false;
    }

    if ( !options.elffile )
    {
        genericsReport( V_ERROR, "Elf File not specified" EOL );
        exit( -2 );
    }

    genericsReport( V_INFO, "orbtop V" VERSION " (Git %08X %s, Built " BUILD_DATE ")" EOL, GIT_HASH, ( GIT_DIRTY ? "Dirty" : "Clean" ) );

    genericsReport( V_INFO, "Server           : %s:%d" EOL, options.server, options.port );
    genericsReport( V_INFO, "Delete Mat       : %s" EOL, options.deleteMaterial ? options.deleteMaterial : "None" );
    genericsReport( V_INFO, "Elf File         : %s" EOL, options.elffile );
    genericsReport( V_INFO, "ForceSync        : %s" EOL, options.forceITMSync ? "true" : "false" );
    genericsReport( V_INFO, "C++ Demangle     : %s" EOL, options.demangle ? "true" : "false" );
    genericsReport( V_INFO, "Display Interval : %d mS" EOL, options.displayInterval );
    genericsReport( V_INFO, "Log File         : %s" EOL, options.logfile ? options.logfile : "None" );

    if ( options.useTPIU )
    {
        genericsReport( V_INFO, "Using TPIU  : true (ITM on channel %d)" EOL, options.tpiuITMChannel );
    }

    return true;
}
// ====================================================================================================
int main( int argc, char *argv[] )

{
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    uint8_t cbw[TRANSFER_SIZE];
    uint64_t lastTime;

    ssize_t t;
    int flag = 1;

    /* Fill in a time to start from */
    lastTime = _timestamp();

    if ( !_processOptions( argc, argv ) )
    {
        exit( -1 );
    }

    /* Get the symbols from file */
    _r.s = SymbolSetCreate( options.elffile );

    if ( !_r.s )
    {
        exit( -3 );
    }

    /* Reset the TPIU handler before we start */
    TPIUDecoderInit( &_r.t );
    ITMDecoderInit( &_r.i, options.forceITMSync );

    sockfd = socket( AF_INET, SOCK_STREAM, 0 );
    setsockopt( sockfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof( flag ) );

    if ( sockfd < 0 )
    {
        perror( "Error creating socket\n" );
        return -1;
    }

    /* Now open the network connection */
    bzero( ( char * ) &serv_addr, sizeof( serv_addr ) );
    server = gethostbyname( options.server );

    if ( !server )
    {
        perror( "Cannot find host" );
        return -1;
    }

    serv_addr.sin_family = AF_INET;
    bcopy( ( char * )server->h_addr,
           ( char * )&serv_addr.sin_addr.s_addr,
           server->h_length );
    serv_addr.sin_port = htons( options.port );

    if ( connect( sockfd, ( struct sockaddr * ) &serv_addr, sizeof( serv_addr ) ) < 0 )
    {
        perror( "Could not connect" );
        return -1;
    }

    while ( ( t = read( sockfd, cbw, TRANSFER_SIZE ) ) > 0 )
    {
        uint8_t *c = cbw;

        while ( t-- )
        {
            _protocolPump( *c++ );
        }

        if ( ( _timestamp() - lastTime ) > options.displayInterval )
        {
            lastTime = _timestamp();
            outputTop();

            if ( !SymbolSetCheckValidity( &_r.s, options.elffile ) )
            {
                /* Make sure old references are invalidated */
                _flushHash();

                if ( _r.s )
                {
                    genericsReport( V_WARN, "Reloaded %s" EOL, options.elffile );
                }
                else
                {
                    /* Its possible the file was in the process of being written, so wait before testing again */
                    usleep( 1000000 );
                    genericsReport( V_WARN, "Attempt second reload of %s" EOL, options.elffile );

                    if ( !SymbolSetCheckValidity( &_r.s, options.elffile ) )
                    {
                        genericsReport( V_ERROR, "Elf file was lost" EOL );
                        return -1;
                    }
                }
            }
        }

        /* Check to make sure there's not an unexpected TPIU in here */
        if ( ITMDecoderGetStats( &_r.i )->tpiuSyncCount )
        {
            genericsReport( V_WARN, "Got a TPIU sync while decoding ITM...did you miss a -t option?" EOL );
            break;
        }
    }

    if ( ( !ITMDecoderGetStats( &_r.i )->tpiuSyncCount ) )
    {
        genericsReport( V_ERROR, "Read failed" EOL );
    }

    close( sockfd );
    return -2;
}
// ====================================================================================================
