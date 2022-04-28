#ifdef _MSC_VER
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <direct.h>
#include <fcntl.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

#ifdef _MSC_VER
typedef SOCKET socket_t;
typedef HANDLE poll_event_t;
typedef int addrlen_t;
static bool is_invalid( socket_t s ) { return s == INVALID_SOCKET; }
static poll_event_t open_connection_event( socket_t s ) {
  u_long mode = 1;
  ioctlsocket( s, FIONBIO, &mode );
  WSAEVENT event = WSACreateEvent();
  WSAEventSelect( s, event, FD_READ | FD_CLOSE );
  return event;
}
static void close_connection_event( poll_event_t event, socket_t &s ) {
  WSACloseEvent( event );
  closesocket( s );
  s = INVALID_SOCKET;
}
static void reset_connection_event( poll_event_t event ) {
  WSAResetEvent( event );
}
static void reset_console_event( poll_event_t event ) {}
#define STDIN_FILENO _fileno( stdin )
#define STDOUT_FILENO _fileno( stdout )
#define __attribute__(x)
typedef struct _stat64 os_stat;
static int os_open( const char *path, int oflag, int mode ) {
  return _open( path, oflag | _O_BINARY, mode );
}
static ssize_t os_write( int fd, const void *buf, size_t count ) {
  return _write( fd, buf, (uint32_t) count );
}
static int os_fstat( int fd, os_stat *statbuf ) {
  return _fstat64( fd, statbuf );
}
static int os_close( int fd ) {
  return _close( fd );
}
#else
typedef int socket_t;
typedef struct pollfd poll_event_t;
typedef socklen_t addrlen_t;
static bool is_invalid( socket_t s ) { return s < 0; }
static const int INVALID_SOCKET = -1;
static poll_event_t open_connection_event( socket_t s ) {
  ::fcntl( s, F_SETFL, O_NONBLOCK | ::fcntl( s, F_GETFL ) );
  poll_event_t ev;
  ev.fd      = s;
  ev.events  = POLLIN | POLLHUP;
  ev.revents = 0;
  return ev;
}
static void close_connection_event( poll_event_t, socket_t &s ) {
  ::close( s );
  s = INVALID_SOCKET;
}
static void reset_connection_event( poll_event_t &ev ) { ev.revents = 0; }
static void reset_console_event( poll_event_t &ev ) { ev.revents = 0; }
static int closesocket( socket_t s ) { return ::close( s ); }
typedef struct stat os_stat;
static int os_open( const char *path, int oflag, int mode ) {
  return ::open( path, oflag, mode );
}
static ssize_t os_write( int fd, const void *buf, size_t count ) {
  return ::write( fd, buf, count );
}
static int os_fstat( int fd, os_stat *statbuf ) {
  return ::fstat( fd, statbuf );
}
static int os_close( int fd ) noexcept {
  return ::close( fd );
}
static const char *os_map_read( size_t size,  int fd ) {
  const char *map;
  map = (const char *) ::mmap( 0, size, PROT_READ, MAP_PRIVATE, fd, 0 );
  if ( map == MAP_FAILED )
    return NULL;
  return map;
}
static void os_unmap_read( const char *map,  size_t size ) {
  ::munmap( (void *) map, size );
}
#endif
struct Poller;

static poll_event_t null_event;
struct EventDispatch {
  Poller & poll;
  int      index;
  EventDispatch( Poller &p ) : poll( p ), index( -1 ) {}
  virtual bool         dispatch( void ) noexcept { return true; }
  virtual poll_event_t get_event( void ) noexcept { return null_event; }
  virtual void         close( void ) noexcept {}
};

static inline uint32_t min_val( uint32_t i,  uint32_t j ) {
  return i < j ? i : j;
}

struct Column {
  uint32_t start, end, off;
  Column( uint32_t beg,  uint32_t end ) {
    this->init( beg, end );
  }
  void init( uint32_t beg,  uint32_t end ) {
    this->start = beg;
    this->end   = end;
    this->off   = 0;
  }
};

struct BufQueue {
  static const uint32_t BUF_INCR = 64 * 1024;
  char * buf;
  uint32_t off, len, size;

  BufQueue() : buf( 0 ), off( 0 ), len( 0 ), size( 0 ) {}

  void reset( void ) {
    if ( this->buf != NULL )
      ::free( this->buf );
    this->buf  = NULL;
    this->off  = 0;
    this->len  = 0;
    this->size = 0;
  }
  char *append_buf( uint32_t &avail ) {
    if ( this->off == this->len ) {
      this->off = 0;
      this->len = 0;
    }
    if ( this->off > this->size / 4 ) {
      this->len -= this->off;
      ::memmove( this->buf, &this->buf[ this->off ], this->len );
      this->off = 0;
    }
    return this->append_incr( 0, avail );
  }
  void append_data( const void *data,  uint32_t datalen ) {
    for (;;) {
      uint32_t avail = this->size - this->len;
      if ( avail >= datalen ) {
        ::memcpy( &this->buf[ this->len ], data, datalen );
        this->len += datalen;
        return;
      }
      this->size += BUF_INCR;
      this->buf   = (char *) ::realloc( this->buf, this->size );
    }
  }
  void append_incr( uint32_t used ) {
    this->len += used;
  }
  char *append_incr( uint32_t used,  uint32_t &avail ) {
    this->len += used;
    avail = this->size - this->len;
    if ( avail < BUF_INCR / 2 ) { /* always have at least 512b */
      this->size += BUF_INCR;
      avail      += BUF_INCR;
      this->buf   = (char *) ::realloc( this->buf, this->size );
    }
    return &this->buf[ this->len ];
  }
  void append_msg( const char *msg,  uint32_t msg_len ) {
    uint32_t avail = this->size - this->len;
    if ( avail < msg_len ) {
      uint32_t incr = BUF_INCR - 1;
      this->size = ( ( this->len + msg_len ) + incr ) & ~incr;
      this->buf  = (char *) ::realloc( this->buf, this->size );
    }
    ::memcpy( &this->buf[ this->len ], msg, msg_len );
    this->len += msg_len;
  }
  char *consume_buf( uint32_t &avail ) {
    return this->consume_incr( 0, avail );
  }
  void consume_incr( uint32_t used ) {
    this->off += used;
  }
  char *consume_incr( uint32_t used,  uint32_t &avail ) {
    this->off += used;
    avail = this->len - this->off;
    return &this->buf[ this->off ];
  }
  int print( const char *fmt, ... ) __attribute__((format(printf,2,3))) {
    uint32_t buflen;
    char *ptr = this->append_buf( buflen );
    va_list args;
    va_start( args, fmt );
    int n = ::vsnprintf( ptr, buflen, fmt, args );
    va_end( args );
    this->append_incr( (uint32_t) n );
    return n;
  }
  void print_col( Column &col,  const char *fmt,  uint32_t pad,
                  uint32_t len,  const char *data ) {
    if ( pad + len + col.off >= col.end ) {
      if ( col.off > col.start ) {
        this->append_data( "\n", 1 );
        col.off = 0;
      }
    }
    if ( col.off < col.start )
      this->pad_to( col, col.start );
    this->print( fmt, len, data );
    col.off += pad + len;
  }
  void pad_to( Column &col,  uint32_t pos ) {
    if ( pos > col.off ) {
      uint32_t len = pos - col.off, i = len, buflen;
      char *ptr = this->append_buf( buflen );
      do { *ptr++ = ' '; } while ( --i != 0 );
      col.off = pos;
      this->append_incr( len );
    }
  }
  enum {
    CTCP_DELIM       = 0x01,
    IRC_BOLD         = 0x02,
    IRC_COLOR        = 0x03,
    IRC_HEX_COLOR    = 0x04,
    IRC_RESET        = 0x0f,
    IRC_REVERSE      = 0x16,
    IRC_ITALICS      = 0x1d,
    IRC_UNDERLINE    = 0x1f,
    IRC_STIKETHROUGH = 0x1e,
    IRC_MONOSPACE    = 0x11
  };
  void print_text( Column &col,  uint32_t len,  const char *text ) {
    static const uint8_t irc_color_tab[] = {
      /* Whitee */ 255,255,255, /* 00 -> 15 */
      /* Bllack */ 0,0,0,
      /* Bleeww */ 0,0,127,
      /* GReeen */ 0,147,0,
      /* LReder */ 255,0,0,
      /* Brwnsh */ 127,0,0,
      /* Pourpl */ 156,0,156,
      /* Orgish */ 252,127,0,
      /* Yeller */ 255,255,0,
      /* LGreen */ 0,252,0,
      /* Caying */ 0,147,147,
      /* LCAYAN */ 0,255,255,
      /* LBleww */ 0,0,252,
      /* Pnkish */ 255,0,255,
      /* GReeay */ 127,127,127,
      /* LGraay */ 210,210,210
    };
    static const int rgb_cnt =
      sizeof( irc_color_tab ) / sizeof( irc_color_tab[ 0 ] ) / 3;
    static const uint8_t ansi_tab[] = { /* 16 -> 98 */
      52 , 94 , 100, 58 , 22 , 29 , 23 , 24 , 17 , 54 , 53 , 89 ,
      88 , 130, 142, 64 , 28 , 35 , 30 , 25 , 18 , 91 , 90 , 125,
      124, 166, 184, 106, 34 , 49 , 37 , 33 , 19 , 129, 127, 161,
      196, 208, 226, 154, 46 , 86 , 51 , 75 , 21 , 171, 201, 198,
      203, 215, 227, 191, 83 , 122, 87 , 111, 63 , 177, 207, 205,
      217, 223, 229, 193, 157, 158, 159, 153, 147, 183, 219, 212,
      16 , 233, 235, 237, 239, 241, 244, 247, 250, 254, 231 };
    static const int ansi_cnt =
      sizeof( ansi_tab ) / sizeof( ansi_tab[ 0 ] );
    static const char * ansi_normal = ANSI_NORMAL;

    uint32_t word_len,
             spc_len;
    bool     in_ctcp  = false,
             in_color = false;

    while ( len > 0 && text[ len - 1 ] == ' ' )
      len -= 1;
    while ( len > 0 ) {
      if ( col.off == col.end ) {
        this->append_data( "\n", 1 );
        col.off = 0;
      }
      if ( col.off < col.start )
        this->pad_to( col, col.start );

      if ( text[ 0 ] < 0x20 ) {
        int          color_sz = 0;
        const char * color    = NULL;
        char         code     = *text++;
        len--;
        switch ( code ) {
          case CTCP_DELIM:       /* 0x01 */
            color_sz = ( in_ctcp ? ANSI_YELLOW_SIZE : ANSI_NORMAL_SIZE );
            color    = ( in_ctcp ? ANSI_YELLOW      : ansi_normal );
            break;
          case IRC_BOLD:         /* 0x02 */
            color_sz = ANSI_BOLD_SIZE;
            color    = ANSI_BOLD;
            break;
          case IRC_COLOR:        /* 0x03 */
            if ( len > 0 && isdigit( text[ 0 ] ) ) {
              int fg = 0, bg = -1;
              fg = text[ 0 ] - '0';
              text++; len--;
              if ( len > 0 && isdigit( text[ 0 ] ) ) { 
                fg = fg * 10 + ( text[ 0 ] - '0' );
                text++; len--;
              }
              /* fg and bg */
              if ( len > 1 && text[ 0 ] == ',' && isdigit( text[ 1 ] ) ) {
                text++; len--;
                bg = text[ 0 ] - '0';
                text++; len--;
                if ( len > 0 && isdigit( text[ 0 ] ) ) {
                  bg = bg * 10 + ( text[ 0 ] - '0' );
                  text++; len--;
                }
              }
              if ( fg < rgb_cnt ) {
                const uint8_t *f = &irc_color_tab[ fg * 3 ];
                this->print( ANSI_24BIT_FG_FMT, f[ 0 ], f[ 1 ], f[ 2 ] );
              }
              else if ( fg < rgb_cnt + ansi_cnt ) {
                this->print( ANSI_256_FG_FMT, ansi_tab[ fg - rgb_cnt ] );
              }
              if ( bg != -1 ) {
                if ( bg < rgb_cnt ) {
                  const uint8_t *b = &irc_color_tab[ bg ];
                  this->print( ANSI_24BIT_BG_FMT, b[ 0 ], b[ 1 ], b[ 2 ] );
                }
                else if ( bg < rgb_cnt + ansi_cnt ) {
                  this->print( ANSI_256_BG_FMT, ansi_tab[ bg - rgb_cnt ] );
                }
              }
            }
            else {
          case IRC_RESET:
              color_sz = ANSI_NORMAL_SIZE;
              color    = ansi_normal;
            }
            break;
          case IRC_REVERSE:
            color_sz = ANSI_REVERSE_SIZE;
            color    = ANSI_REVERSE;
            break;
          case IRC_HEX_COLOR: {
            uint32_t rgb = 0, i;
            if ( len < 6 )
              break;
            for ( i = 0; i < 6; i++ ) {
              uint8_t hexd;
              if ( text[ i ] >= '0' && text[ i ] <= '9' )
                hexd = text[ i ] - '0';
              else if ( text[ i ] >= 'a' && text[ i ] <= 'f' )
                hexd = 10 + ( text[ i ] - 'a' );
              else if ( text[ i ] >= 'A' && text[ i ] <= 'F' )
                hexd = 10 + ( text[ i ] - 'A' );
              else
                break;
              rgb = ( rgb << 4 ) | hexd;
            }
            if ( i != 6 )
              break;
            this->print( ANSI_24BIT_FG_FMT, ( rgb >> 16 ) & 0xffU,
                         ( rgb >> 8 ) & 0xffU, rgb & 0xffU );
            break;
          }
          case IRC_ITALICS:      /* 0x1D */
            color_sz = ANSI_ITALIC_SIZE;
            color    = ANSI_ITALIC;
            break;
          case IRC_UNDERLINE:    /* 0x1F */
            color_sz = ANSI_UNDERLINE_SIZE;
            color    = ANSI_UNDERLINE;
            break;
          case IRC_STIKETHROUGH: /* 0x1E */
            color_sz = ANSI_STRIKETHROUGH_SIZE;
            color    = ANSI_STRIKETHROUGH;
            break;
          case IRC_MONOSPACE:    /* 0x11 */
          default:
            break;
        }
        if ( color_sz != 0 ) {
          in_color = ( color != ansi_normal );
          this->append_data( color, color_sz );
        }
        continue;
      }
      for ( word_len = 0; &text[ word_len ] < &text[ len ]; word_len++ ) {
        if ( text[ word_len ] <= ' ' )
          break;
      }
      for ( spc_len = word_len; &text[ spc_len ] < &text[ len ]; spc_len++ ) {
        if ( text[ spc_len ] != ' ' )
          break;
      }
      if ( col.off + spc_len <= col.end ) { /* enough for word + space */
        this->append_data( text, spc_len );
        col.off += spc_len;
        text = &text[ spc_len ];
        len -= spc_len;
        continue;
      }
      if ( col.off + word_len <= col.end ) { /* enough for word */
        this->append_data( text, word_len );
        col.off = col.end;
        text = &text[ spc_len ];
        len -= spc_len;
        continue;
      }
      if ( col.off == col.start ) { /* if word takes up entire line */
        uint32_t trunc_len = col.end - col.start;
        this->append_data( text, trunc_len );
        col.off = col.end;
        text = &text[ trunc_len ];
        len -= trunc_len;
        continue;
      }
      col.off = col.end; /* too long for this line */
    }
    if ( in_color )
      this->append_data( ansi_normal, ANSI_NORMAL_SIZE );
  }
};

static const size_t MAX_USER_LEN    = 256,
                    MAX_NICK_LEN    = 256,
                    MAX_HOST_LEN    = 256,
                    MAX_CHANNEL_LEN = 256;
struct IRC_String {
  char   * str;
  uint32_t len;

  IRC_String( IRC_String &s ) : str( s.str ), len( s.len ) {}
  IRC_String( char *s = 0,  uint32_t l = 0 ) : str( s ), len( l ) {}
  IRC_String( const char *s ) : str( 0 ), len( 0 ) {
    if ( s != NULL )
      this->set( s, ::strlen( s ) );
  }
  IRC_String & operator=( const char *s ) {
    if ( s != NULL )
      this->set( s, ::strlen( s ) );
    return *this;
  }

  void set( const char *s,  uint32_t z ) {
    if ( s != NULL ) {
      this->str = (char *) ::realloc( this->str, z + 1 );
      this->len = z;
      ::memcpy( this->str, s, z );
      this->str[ z ] = '\0';
    }
    else {
      this->clear();
    }
  }
  void set( const IRC_String &s ) {
    this->set( s.str, s.len );
  }
  void clear( void ) {
    if ( this->str != NULL ) {
      ::free( this->str );
      this->str = NULL;
      this->len = 0;
    }
  }
  bool equal( const char *s,  uint32_t z ) const {
    return z == this->len && ::memcmp( s, this->str, z ) == 0;
  }
  bool equal( const IRC_String &s ) const {
    return this->equal( s.str, s.len );
  }
  int cmp( const char *s,  uint32_t z ) const {
    int i = ::memcmp( this->str, s, z < this->len ? z : this->len );
    if ( i == 0 ) {
      i = ( this->len < z ) ? -1 :
          ( this->len > z ) ? 1 : 0;
    }
    return i;
  }
  int cmp( const IRC_String &s ) const {
    return this->cmp( s.str, s.len );
  }
  uint32_t append( const char *s,  uint32_t z ) {
    uint32_t off = this->len + 1;
    this->str = (char *) ::realloc( this->str, off + z + 1 );
    this->str[ off - 1 ] = ' ';
    ::memcpy( &this->str[ off ], s, z );
    this->len = off + z;
    this->str[ this->len ] = '\0';
    return off;
  }
  bool first_word( uint32_t &i,  uint32_t &z ) const {
    i = z = 0;
    return this->next_word( i, z );
  }
  bool next_word( uint32_t &i,  uint32_t &z ) const {
    i += z;
    for ( ; ; i++ ) {
      if ( i == this->len )
        return false;
      if ( this->str[ i ] != ' ' )
        break;
    }
    z = 1;
    for ( ; i + z < this->len; z++ ) {
      if ( this->str[ i + z ] == ' ' )
        break;
    }
    return true;
  }
};

struct IRC_Table {
  IRC_String db;
  struct Tab {
    uint32_t off, len;
  } * tab;
  uint32_t sz, spc;

  IRC_Table() : tab( 0 ), sz( 0 ), spc( 0 ) {}

  void update( const char *n,  uint32_t nlen ) {
    uint32_t count = 0, off, len, start, idx;

    this->reclaim_space();
    start = this->db.append( n, nlen );
    off = start;
    len = 0;
    while ( this->db.next_word( off, len ) )
      count++;
    if ( this->sz + count == 0 ) {
      this->clear();
      return;
    } 
    idx = this->sz;
    this->sz += count;
    this->tab = (Tab *) ::realloc( this->tab, sizeof( Tab ) * this->sz );
    off = start;
    len = 0;
    while ( this->db.next_word( off, len ) ) {
      this->tab[ idx ].off = off;
      this->tab[ idx++ ].len = len;
    }
    if ( idx == 1 ) /* no need to sort 1 elem */
      return;
#ifndef _MSC_VER
    ::qsort_r( this->tab, idx, sizeof( this->tab[ 0 ] ), tab_cmp, &this->db );
#else
    ::qsort_s( this->tab, idx, sizeof( this->tab[ 0 ] ), tab_cmp, &this->db );
#endif
    for ( uint32_t k = idx - 1; ; ) {
      if ( k == 0 )
        break;
      if ( this->str( k ).equal( this->str( k - 1 ) ) )
        this->del_idx( k );
      k -= 1;
    }
  }
  bool insert( IRC_String &s ) {
    uint32_t k;
    if ( this->find_idx( s, k ) )
      return false;

    if ( k < this->sz ) {
      IRC_String v;
      this->val( k, v );
      if ( v.cmp( s ) < 0 )
        k++;
    }
    uint32_t off = this->db.append( s.str, s.len );
    this->tab = (Tab *) ::realloc( this->tab, sizeof( Tab ) * ( this->sz + 1 ));
    ::memmove( &this->tab[ k + 1 ], &this->tab[ k ],
               ( this->sz - k ) * sizeof( Tab ) );
    this->tab[ k ].off = off;
    this->tab[ k ].len = s.len;
    this->sz++;
    return true;
  }
  static int tab_cmp( const void *p1,  const void *p2,  void *arg ) {
    const Tab  * el1 = (const Tab *) p1,
               * el2 = (const Tab *) p2;
    IRC_String * db  = (IRC_String *) arg;
    const char * s1  = &db->str[ el1->off ],
               * s2  = &db->str[ el2->off ];
    int i = ::memcmp( s1, s2, el1->len < el2->len ? el1->len : el2->len );
    if ( i == 0 ) {
      i = ( el1->len < el2->len ) ? -1 :
          ( el1->len > el2->len ) ? 1 : 0;
    }
    return i;
  } 
  IRC_String str( uint32_t k ) {
    IRC_String s;
    return this->val( k, s );
  }
  IRC_String &val( uint32_t k,  IRC_String &s ) {
    s.str = &this->db.str[ this->tab[ k ].off ];
    s.len = this->tab[ k ].len;
    return s;
  }
  bool find_idx( IRC_String &id,  uint32_t &k ) {
    IRC_String v;
    uint32_t piv, size = this->sz;
    int n = 0;
    for ( k = 0; ; ) {
      if ( size < 4 ) {
        for ( ; size > 0 && (n = id.cmp( this->val( k, v ) )) > 0; size-- )
          k++;
        return size > 0 && n == 0;
      }
      piv = size / 2;
      if ( (n = id.cmp( this->val( k + piv, v ) )) <= 0 ) {
        if ( n == 0 ) {
          k += piv;
          return true;
        }
        size = piv;
      }
      else {
        size -= piv + 1;
        k    += piv + 1;
      }
    }
  }
  bool find( IRC_String &id ) {
    uint32_t k;
    return this->find_idx( id, k );
  }
  bool find_first_prefix( IRC_String &id,  uint32_t &k ) {
    k = 0;
    return this->find_prefix( id, k );
  }
  bool find_next_prefix( IRC_String &id,  uint32_t &k ) {
    k++;
    return this->find_prefix( id, k );
  }
  bool find_prefix( IRC_String &id,  uint32_t &k ) {
    for ( ; k < this->sz; k++ ) {
      IRC_String v;
      bool match = true;
      this->val( k, v );
      for ( uint32_t j = 0; j < id.len; j++ ) {
        if ( j == v.len || toupper( id.str[ j ] ) != toupper( v.str[ j ] ) ) {
          match = false;
          break;
        }
      }
      if ( match )
        return true;
    }
    return false;
  }
  bool del( IRC_String &id ) {
    uint32_t k;
    if ( ! this->find_idx( id, k ) )
      return false;
    this->del_idx( k );
    return true;
  }
  void del_idx( uint32_t k ) {
    this->spc += this->tab[ k ].len + 1;
    ::memset( &this->db.str[ this->tab[ k ].off ], ' ', this->tab[ k ].len );
    ::memmove( &this->tab[ k ], &this->tab[ k + 1 ],
               ( this->sz - ( k + 1 ) ) * sizeof( Tab ) );
    this->sz -= 1;
  }
  void reclaim_space( void ) {
    if ( this->spc * 2 > this->db.len ) {
      uint32_t i = 0, j = 0,
               len = this->db.len;
      char   * text = this->db.str;
      while ( j < len && text[ j ] == ' ' )
        j++;
      while ( j < len ) {
        if ( text[ j ] == ' ' ) {
          while ( j < len && text[ j ] == ' ' )
            j++;
          text[ i++ ] = ' ';
        }
        while ( j < len && text[ j ] != ' ' )
          text[ i++ ] = text[ j++ ];
      }
      if ( i > 0 && text[ i - 1 ] == ' ' )
        i -= 1;
      this->db.len = i;
      text[ i ] = '\0';
      this->spc = 0;
    }
  }
  void clear( void ) {
    this->db.clear();
    if ( this->tab != NULL ) {
      ::free( this->tab );
      this->tab = NULL;
      this->sz = 0;
    }
  }
};

struct IRC_Channel {
  IRC_Channel * next,
              * back;
  IRC_String    chan; /* lc153 */
  IRC_Table     nick_tab;/* nicks in channel */

  IRC_Channel( IRC_String &c ) : next( 0 ), back( 0 ) {
    this->chan.set( c );
  }
  ~IRC_Channel() {
    this->chan.clear();
    this->nick_tab.clear();
  }
  void add_nicks( const char *n,  uint32_t len ) {
    this->nick_tab.update( n, len );
  }
  bool add_nick( const char *n,  uint32_t len ) {
    IRC_String id( (char *) n, len );
    return this->nick_tab.insert( id );
  }
  bool del_nick( const char *n,  uint32_t len ) {
    IRC_String id( (char *) n, len );
    return this->nick_tab.del( id );
  }
  bool is_nick( IRC_String &id ) {
    return this->nick_tab.find( id );
  }
};

struct IRC_State {
  IRC_Channel * hd,       /* channels joined */
              * tl;
  IRC_Table     chan_tab; /* /list reply */
  IRC_String    user,     /* anon153 */
                host,     /* irc.libera.chat */
                port,     /* 6667 */
                real,     /* real name */
                nick,     /* lc153 */
                pass,     /* passwd */
                chan_id,  /* lc153!~anon153@104-.... */
                log_fn;   /* log filename */
  BufQueue      chan_log; /* channel list log */
  int           log_fd;   /* append only log fd */
  bool          quiet;    /* mute QUIT, JOIN, PART msgs */
  IRC_State() : hd( 0 ), tl( 0 ), log_fd( -1 ), quiet( false ) {}

  IRC_Channel *find_chan( IRC_String &id ) {
    for ( IRC_Channel *chan = this->hd; chan != NULL; chan = chan->next ) {
      if ( chan->chan.equal( id ) )
        return chan;
    }
    return NULL;
  }
  void push_chan_hd( IRC_Channel *chan ) {
    chan->next = this->hd;
    chan->back = NULL;
    if ( this->hd == NULL )
      this->tl = chan;
    else
      this->hd->back = chan;
    this->hd = chan;
  }
  void pop_chan( IRC_Channel *chan ) {
    if ( chan->back == NULL )
      this->hd = chan->next;
    else
      chan->back->next = chan->next;
    if ( chan->next == NULL )
      this->tl = chan->back;
    else
      chan->next->back = chan->back;
    chan->next = chan->back = NULL;
  }
  void set_nick( const char *n,  uint32_t len ) {
    this->nick.set( n, len );
  }
  IRC_Channel *join_channel( IRC_String &id, const char *p,  uint32_t plen ) {
    IRC_Channel *chan = this->find_chan( id );
    if ( chan == NULL )
      this->chan_id.set( p, plen );
    if ( chan == NULL ) {
      chan = new IRC_Channel( id );
      this->push_chan_hd( chan );
      this->chan_tab.insert( id );
    }
    else if ( chan != this->hd ) {
      this->pop_chan( chan );
      this->push_chan_hd( chan );
    }
    return chan;
  }
  void part_channel( IRC_Channel *chan ) {
    this->pop_chan( chan );
    delete chan;
  }
  void update_chan_tab( IRC_String &id ) {
    this->chan_tab.insert( id );
  }
};

/* message    = [ ":" prefix SPACE ] command [ params ] crlf
   prefix     = servername / ( nickname [ [ "!" user ] "@" host ])
   command    = 1*letter / 3digit
   params     = *14( SPACE middle ) [ SPACE ":" trailing ]
              =/ 14( SPACE middle ) [ SPACE [ ":" ] trailing ]
   nospcrlfcl = %x01-09 / %x0B-0C / %x0E-1F / %x21-39 / %x3B-FF ; not \0\r\n:
   middle     = nospcrlfcl *( ":" / nospcrlfcl )
   trailing   = *( ":" / " " / nospcrlfcl )
   SPACE      = %x20 ; Whitespace.
   crlf       = %x0D %x0A ; Carriage return/linefeed.  */

enum MsgType {
  /* replies by 3 digit number */
  M_RPL_LISTSTART = 321, /* list start */
  M_RPL_LIST      = 322, /* list reply  -- list of channels */
  M_RPL_LISTEND   = 323, /* list end */
  M_RPL_NAMREPLY  = 353, /* /names reply -- list of nicks */
  /* commands by name */
  M_ADMIN = 1000, M_AWAY, M_CONNECT, M_DIE, M_ERROR, M_INFO, M_INVITE, M_ISON,
  M_JOIN, M_KICK, M_KILL, M_LINKS, M_LIST, M_LUSERS, M_MODE, M_MOTD, M_NAMES,
  M_NICK, M_NJOIN, M_NOTICE, M_OPER, M_PART, M_PASS, M_PING, M_PONG, M_PRIVMSG,
  M_QUIT, M_REHASH, M_RESTART, M_SERVER, M_SERVICE, M_SERVLIST, M_SQUERY,
  M_SQUIRT, M_SQUIT, M_STATS, M_SUMMON, M_TIME, M_TOPIC, M_TRACE, M_USER,
  M_USERHOST, M_USERS, M_VERSION, M_WALLOPS, M_WHO, M_WHOIS, M_WHOWAS
};
static struct MsgString {
  uint16_t     msg_type;
  uint16_t     cmd_len;
  const char   cmd_name[ 8 ];
} msg_string[] = {
{M_ADMIN, 5, "ADMIN"},    {M_AWAY, 4, "AWAY"},       {M_CONNECT, 7, "CONNECT"},
{M_DIE, 3, "DIE"},        {M_ERROR, 5, "ERROR"},     {M_INFO, 4, "INFO"},
{M_INVITE, 6, "INVITE"},  {M_ISON, 4, "ISON"},       {M_JOIN, 4, "JOIN"},
{M_KICK, 4, "KICK"},      {M_KILL, 4, "KILL"},       {M_LINKS, 5, "LINKS"},

{M_LIST, 4, "LIST"},      {M_LUSERS, 6, "LUSERS"},   {M_MODE, 4, "MODE"},
{M_MOTD, 4, "MOTD"},      {M_NAMES, 5, "NAMES"},     {M_NICK, 4, "NICK"},
{M_NJOIN, 5, "NJOIN"},    {M_NOTICE, 6, "NOTICE"},   {M_OPER, 4, "OPER"},
{M_PART, 4, "PART"},      {M_PASS, 4, "PASS"},       {M_PING, 4, "PING"},

{M_PONG, 4, "PONG"},      {M_PRIVMSG, 7, "PRIVMSG"}, {M_QUIT, 4, "QUIT"},
{M_REHASH, 6, "REHASH"},  {M_RESTART, 7, "RESTART"}, {M_SERVER, 6, "SERVER"},
{M_SERVICE, 7, "SERVICE"},{M_SERVLIST, 7, "SERVLIS"},{M_SQUERY, 6, "SQUERY"},
{M_SQUIRT, 6, "SQUIRT"},  {M_SQUIT, 5, "SQUIT"},     {M_STATS, 5, "STATS"},

{M_SUMMON, 6, "SUMMON"},  {M_TIME, 4, "TIME"},       {M_TOPIC, 5, "TOPIC"},
{M_TRACE, 5, "TRACE"},    {M_USER, 4, "USER"},       {M_USERHOST, 7, "USERHOS"},
{M_USERS, 5, "USERS"},    {M_VERSION, 7, "VERSION"}, {M_WALLOPS, 7, "WALLOPS"},
{M_WHO, 3, "WHO"},        {M_WHOIS, 5, "WHOIS"},     {M_WHOWAS, 6, "WHOWAS"}
};
static const uint32_t nmsg_strings =
  sizeof( msg_string ) / sizeof( msg_string[ 0 ] );
static uint8_t msg_string_index[ 256 ]; /* assert nmsg_strings < 256 */

void
init_msg_string_index( void ) noexcept
{
  uint32_t i;
  char     c;
  for ( i = 0; i < nmsg_strings; i++ ) {
    c = msg_string[ i ].cmd_name[ 0 ];
    if ( msg_string_index[ (uint8_t) c ] == 0 )
      msg_string_index[ (uint8_t) c ] = i;
  }
  for ( i = 0; i < nmsg_strings; i++ ) {
    if ( msg_string_index[ i ] == 0 )
      msg_string_index[ i ] = (uint8_t) nmsg_strings;
  }
}

uint32_t
lookup_cmd( const char *name,  uint32_t name_len ) noexcept
{
  if ( name_len == 3 && isdigit( name[ 0 ] ) &&
       isdigit( name[ 1 ] ) && isdigit( name[ 2 ] ) ) {
    return ( name[ 0 ] - '0' ) * 100 +
           ( name[ 1 ] - '0' ) * 10 +
           ( name[ 2 ] - '0' );
  }
  if ( msg_string_index[ 'W' ] == 0 )
    init_msg_string_index();
  if ( name_len > 7 )
    name_len = 7;
  char     c = toupper( name[ 0 ] );
  uint32_t i = msg_string_index[ (uint8_t) c ];
  for ( ; i < nmsg_strings; i++ ) {
    if ( msg_string[ i ].cmd_name[ 0 ] > c )
      return 0;
    if ( name_len == msg_string[ i ].cmd_len &&
         ::strncasecmp( name, msg_string[ i ].cmd_name, name_len ) == 0 )
      return msg_string[ i ].msg_type;
  }
  return 0;
}

static struct CmdDescr {
  uint16_t msg_type;
  const char * args,
             * descr;
} cmd_descr[] = {
{ M_ADMIN, "[<server>]", "Get info about the admin of a server" },
{ M_AWAY, "[<msg>]", "Set an reply string for any PRIVMSG commands" },
{ M_CONNECT, "<target>", "Request a new connection to another server" },
{ M_DIE, "", "Shutdown the server" },
{ M_ERROR, "<msg>", "Report a serious or fatal error to a peer" },
{ M_INFO, "[<server>]", "Get info describing a server" },
{ M_INVITE, "<nick> <chan>", "Invite a user to a channel" },
{ M_ISON, "<nick>", "Determine if a nickname is currently on IRC" },
{ M_JOIN, "<chan>", "Join a channel" },
{ M_KICK, "<chan> <nick> [<why>]", "Removal of a user from a channel" },
{ M_KILL, "<nick> [<why>]", "Close a conn by the server" },
{ M_LINKS, "", "List all snames which are known by the server" },
{ M_LIST, "[<chan>]", "List channels and their topics" },
{ M_LUSERS, "", "Get statistics about the size of the IRC network" },
{ M_MODE, "<chan> {+opsitnbv}", "User mode" },
{ M_MOTD, "", "Get the Message of the Day" },
{ M_NAMES, "[<chan>]", "List all visible nicknames" },
{ M_NICK, "<nick>", "Set a nickname" },
{ M_NJOIN, "", "Exchange list of channel members between servers" },
{ M_NOTICE, "<nick> <text>", "Similar to PRIVMSG, no automatic reply" },
{ M_OPER, "", "Obtain operator privileges" },
{ M_PART, "<chan>", "Leave a channel" },
{ M_PASS, "<text>", "Set a connection password" },
{ M_PING, "<text>", "Test for the presence of an active conn" },
{ M_PONG, "<text>", "Reply to a PING message" },
{ M_PRIVMSG, "<recv> :<text>", "Send messages to users, as well channels" },
{ M_QUIT, "[<msg>]", "Terminate the client session" },
{ M_REHASH, "", "Force the server to process its configuration file" },
{ M_RESTART, "", "Force the server to restart itself" },
{ M_SERVER, "<name> <hop> <info>", "Register a new server" },
{ M_SERVICE, "<nick> <r> <dist> <type> <r> <info>", "Register a new service" },
{ M_SERVLIST, "[<mask> [<type>]]", "List services connected to the network" },
{ M_SQUERY, "<service> <text>", "Message a service" },
{ M_SQUIRT, "<server>", "Disconnect a server link" },
{ M_SQUIT, "<server>", "Break a local or remote server link" },
{ M_STATS, "[chiklmoyu]", "Get server statistics" },
{ M_SUMMON, "<nick>", "Ask a user to join IRC" },
{ M_TIME, "", "Get the local time from the server" },
{ M_TOPIC, "<chan> <topic>", "Change or view the topic of a channel" },
{ M_TRACE, "[<server>]", "Find the route to a server" },
{ M_USER, "<nick> <host> <svr> <real>", "Specify the info of a new user" },
{ M_USERHOST, "<nick> [<nick>]", "Get info about upto 5 nicknames" },
{ M_USERS, "", "Get a list of users logged into the server" },
{ M_VERSION, "", "Get the version of the server" },
{ M_WALLOPS, "", "Send a message to all users who have the 'w' user mode" },
{ M_WHO, "[<nick>|0]", "List a set of users" },
{ M_WHOIS, "<nick>", "Get info about a specific user" },
{ M_WHOWAS, "<nick>", "Get info about a nickname which no longer exists" }
};
static const uint32_t ncmd_descrs =
  sizeof( cmd_descr ) / sizeof( cmd_descr[ 0 ] );

struct Message {
  static const uint32_t MAX_PARAMS = 80;
  const char * msg,    /* msg = [ ":" prefix SPACE ] command [ params ] crlf */
             * prefix, /* prefix = server / ( nick [ [ "!" user ] "@" host ])*/
             * nick,   /* nick = nick ( [ [ ! user ] @ host ] ) */
             * command, /* command = letter+ / 3digit */
             * param[ MAX_PARAMS ], /* ( SPACE middle )*[ SPACE ":" trail ] */
             * text; /* [ : trail ] */
  uint32_t     msg_type,
               msg_len,
               prefix_len,
               nick_len,
               command_len,
               param_len[ MAX_PARAMS ],
               nparams,
               text_len;
  bool         self_msg;

  Message( const char *m,  uint32_t l )
    : msg( m ), prefix( 0 ), nick( 0 ), command( 0 ), text( 0 ), msg_type( 0 ),
      msg_len( l ), prefix_len( 0 ), nick_len( 0 ), command_len( 0 ),
      nparams( 0 ), text_len( 0 ), self_msg( false ) {}
  Message( IRC_State &state,  IRC_String &rcv,  IRC_String &txt,
           const char *cmd = "PRIVMSG",  uint32_t clen = 7)
    : msg( 0 ), prefix( 0 ), nick( state.nick.str ), command( cmd ),
      text( txt.str ), msg_type( M_PRIVMSG ), msg_len( 0 ), prefix_len( 0 ),
      nick_len( state.nick.len ), command_len( clen ), nparams( 1 ),
      text_len( txt.len ), self_msg( true ) {
    this->param[ 0 ]     = rcv.str;
    this->param_len[ 0 ] = rcv.len;
  }
  uint32_t line_len( void ) {
    uint32_t len = this->msg_len;
    if ( len > 1 && this->msg[ len - 1 ] == '\n' ) {
      len -= 1;
      if ( len > 1 && this->msg[ len - 1 ] == '\r' )
        len -= 1;
    }
    return len;
  }

  IRC_String &param_str( uint32_t n,  IRC_String &s ) {
    if ( n < this->nparams ) {
      s.str = (char *) this->param[ n ];
      s.len = this->param_len[ n ];
    }
    else {
      s.str = NULL;
      s.len = 0;
    }
    return s;
  }
  IRC_String &nick_str( IRC_String &s ) {
    s.str = (char *) this->nick;
    s.len = this->nick_len;
    return s;
  }
  bool parse( void ) {
    if ( this->msg_len < 3 ) return false;
    if ( this->msg[ 0 ] != ':' ) return false;
    if ( this->msg[ this->msg_len - 1 ] != '\n' ) return false;
    if ( this->msg[ this->msg_len - 2 ] != '\r' ) return false;
    const char * end = &this->msg[ this->msg_len - 2 ];
    uint32_t i;
    this->prefix = &this->msg[ 1 ];
    for ( i = 0; &this->prefix[ i ] < end; i++ ) {
      if ( this->prefix[ i ] == '!' && this->nick_len == 0 ) {
        this->nick = this->prefix;
        this->nick_len = i;
      }
      if ( this->prefix[ i ] == ' ' )
        break;
    }
    if ( (this->prefix_len = i) == 0 )
      return false;
    this->command = &this->prefix[ i + 1 ];
    if ( this->command >= end )
      return false;
    for ( i = 0; &this->command[ i ] < end; i++ )
      if ( this->command[ i ] <= ' ' )
        break;
    if ( (this->command_len = i) == 0 )
      return false;
    this->msg_type = lookup_cmd( this->command, this->command_len );
    if ( this->msg_type == 0 )
      return false;
    const char *p = &this->command[ i + 1 ];
    while ( p < end ) {
      if ( *p == ':' ) {
        this->text = p + 1;
        this->text_len = (uint32_t) ( end - this->text );
        return true;
      }
      for ( i = 0; &p[ i ] < end; i++ )
        if ( p[ i ] == ' ' )
          break;
      if ( i == 0 )
        return true;
      this->param[ this->nparams ] = p;
      this->param_len[ this->nparams ] = i;
      if ( ++this->nparams == MAX_PARAMS )
        return false;
      p = &p[ i + 1 ];
    }
    return true;
  }
};

uint32_t djb_hash( const char *str,  uint32_t len ) {
  uint32_t key = 5381;
  for ( ; len > 0; len -= 1 ) {
    uint8_t c = (uint8_t) *str++;
    key = (uint32_t) c ^ ( ( key << 5 ) + key );
  }
  return key;
}

uint32_t nick_fmt_color( const char *nick,  uint32_t nick_len,  char *buf ) {
  uint32_t colnum = djb_hash( nick, nick_len ) % 127,
           r, g, b;
  r = 255 - ( colnum * 255 / 126 );
  g = ( colnum * 510 / 126 );
  b = ( colnum * 255 / 126 );
  if ( g > 255 ) g = 510 - g;
  return (uint32_t) ::snprintf( buf, 32, ANSI_BOLD ANSI_24BIT_FG_FMT,
                                255 - r, 255 - g, 255 - b );
}

uint32_t chan_fmt_color( const char *chan,  uint32_t chan_len,  char *buf ) {
  uint32_t colnum = djb_hash( chan, chan_len ) % 127,
           r, g, b;
  g = 255 - ( colnum * 255 / 126 );
  r = ( colnum * 510 / 126 );
  b = ( colnum * 255 / 126 );
  if ( r > 255 ) r = 510 - r;
  return (uint32_t) ::snprintf( buf, 32, ANSI_BOLD ANSI_24BIT_FG_FMT,
                                255 - r, 255 - g, 255 - b );
}

enum {
  SHOW_CHANNELS   = 1,
  PRINT_TIMESTAMP = 2
};

struct MsgPrint {
  IRC_State & state;
  BufQueue  & fmt_buf;
  bool      & update_prompt;
  Column      left,
              right;
  uint32_t    nick_pad;
  bool        print_timestamp,
              show_channels;

  MsgPrint( IRC_State &s, BufQueue &f, bool &p, uint32_t c,
            int what = PRINT_TIMESTAMP ) :
      state( s ), fmt_buf( f ), update_prompt( p ),
      left( 0, 16 ), right( 16, c - 1 ), nick_pad( c / 4 ),
      print_timestamp( what & PRINT_TIMESTAMP ),
      show_channels( what & SHOW_CHANNELS ) {
    if ( c / 4 < 16 ) {
      this->left.end = c / 4;
      this->right.init( c / 4, c - 1 );
    }
    if ( this->nick_pad > 24 )
      this->nick_pad = 24;
  }

  void print_msg( Message &msg ) noexcept;
  void print_channel( IRC_String &chan ) noexcept;
  void print_nick( IRC_String &nick,  char suffix ) noexcept;
  void print_error( Message &msg ) noexcept;
  void print_parse_error( Message &msg ) noexcept;
};

enum CtcpType {
  CTCP_NONE = 0,
  CTCP_ACTION
};

struct Connection;
struct Console : public EventDispatch {
  LineCook   & lc;
  TTYCook    & tty;
  Connection * conn;
  IRC_State  & state;
  BufQueue     fmt_buf;
  bool         update_prompt;

  Console( Poller &p, LineCook &l, TTYCook &t, IRC_State &i ) 
    : EventDispatch( p ), lc( l ), tty( t ), conn( 0 ),
      state( i ), update_prompt( false ) {
    l.complete_cb  = Console::complete_cb;
    l.complete_arg = this;
  }

  static int complete_cb( LineCook *lc,  const char *buf,  size_t off,
                          size_t len,  void *arg ) noexcept;
  virtual bool         dispatch( void ) noexcept;
  virtual poll_event_t get_event( void ) noexcept;
  int                  print( const char *fmt, ... ) noexcept
    __attribute__((format(printf,2,3)));
  int                  write( const void *buf, uint32_t buflen ) noexcept;

  void send_privmsg( CtcpType ctcp,  IRC_String &recv,  IRC_String &text ) noexcept;
  bool parse_recv_text( uint32_t i,  IRC_String &recv,  IRC_String &text ) noexcept;
  void make_prompt( bool show ) noexcept; /* show nick + svr + chan */
  void flush( void ) noexcept; /* flush fmt_buf */
  void complete( const char *buf,  size_t off,  size_t len ) noexcept;
  void show_help( char which ) noexcept;
  void show_channels( char which ) noexcept;
  void bang_connect( void ) noexcept;
  void bang_channels( void ) noexcept;
  void bang_help( void ) noexcept;
  void bang_quiet( void ) noexcept;
  void bang_quit( void ) noexcept;
  void slash_msg( void ) noexcept;
  void slash_me( void ) noexcept;
};

struct LocalCmd {
  const char * cmd,
             * args,
             * descr;
  void (Console::*dispatch_cmd)( void );
};

static LocalCmd bang_cmd[] = {
{ "!connect",  "", "connect or reconnect to host:port", &Console::bang_connect },
{ "!channels", "", "show list of channels", &Console::bang_channels },
{ "!help",     "", "show help", &Console::bang_help },
{ "!quiet",    "", "toggle quiet mode, mute join and quit msgs", &Console::bang_quiet },
{ "!quit",     "", "quit program", &Console::bang_quit }
};
static const uint32_t nbang_cmds = sizeof( bang_cmd ) / sizeof( bang_cmd[ 0 ] );

static LocalCmd slash_cmd[] = {
{ "/msg", "<recv> <text>", "Send a message (PRIVMSG recv :text)", &Console::slash_msg },
{ "/me",  "<recv> <text>", "Send an ACTION (PRIVMSG recv :0x1ACTION text0x1)", &Console::slash_me }
};
static const uint32_t nslash_cmds = sizeof( slash_cmd ) / sizeof( slash_cmd[ 0 ] );

struct Connection : public EventDispatch {
  socket_t    sock;
  Console   & console;
  BufQueue    recv_buf,
              send_buf;
  IRC_State & state;
  int         gai_status,
              sock_errno;

  Connection( Poller &p, Console &c, IRC_State &i )
    : EventDispatch( p ), sock( INVALID_SOCKET ), console( c ), state( i ),
      gai_status( 0 ), sock_errno( 0 ) {
    c.conn = this;
  }
  bool is_connected( void ) {
    return this->sock != INVALID_SOCKET;
  }
  bool                 connect( BufQueue *out = NULL ) noexcept;
  virtual bool         dispatch( void ) noexcept;
  virtual poll_event_t get_event( void ) noexcept;
  virtual void         close( void ) noexcept;
  int                  print( const char *fmt, ... ) noexcept
    __attribute__((format(printf,2,3)));
  void flush( void ) noexcept;
};

struct Poller {
  EventDispatch * events[ 64 ];
  poll_event_t    poll_set[ 64 ];
  int             nevents,
                  dispatch_cnt;
  bool            quit;

  Poller() : nevents( 0 ), dispatch_cnt( 0 ), quit( false ) {}

  void add( EventDispatch *ev ) noexcept;
  void remove( EventDispatch *ev ) noexcept;
  bool next_event( EventDispatch *&ev, int time_ms ) noexcept;
};

