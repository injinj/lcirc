#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <linecook/linecook.h>
#include <linecook/ttycook.h>
#include <lcirc/lc_irc.h>

void
Poller::add( EventDispatch *ev ) noexcept
{
  ev->index                       = this->nevents;
  this->events[ this->nevents ]   = ev;
  this->poll_set[ this->nevents ] = ev->get_event();
  this->nevents++;
}

void
Poller::remove( EventDispatch *ev ) noexcept
{
  if ( ev->index < this->nevents - 1 ) {
    ::memmove( &this->events[ ev->index ], &this->events[ ev->index + 1 ],
               sizeof( this->events[ 0 ] ) *
                 ( this->nevents - ( ev->index + 1 ) ) );
    ::memmove( &this->poll_set[ ev->index ], &this->poll_set[ ev->index + 1 ],
               sizeof( this->poll_set[ 0 ] ) *
                 ( this->nevents - ( ev->index + 1 ) ) );
  }
  this->nevents--;
}

bool
Poller::next_event( EventDispatch *&ev, int time_ms ) noexcept
{
#ifdef _MSC_VER
  DWORD cnt;
  cnt = WaitForMultipleObjects( this->nevents, this->poll_set, FALSE, time_ms );
  if ( cnt >= WAIT_OBJECT_0 && cnt < WAIT_OBJECT_0 + this->nevents ) {
    cnt -= WAIT_OBJECT_0;
    ev = this->events[ cnt ];
    this->dispatch_cnt++;
    return true;
  }
  return false;
#else
  for (;;) {
    int n;
    for ( n = 0; n < this->nevents; n++ ) {
      if ( this->poll_set[ n ].revents != 0 ) {
        ev = this->events[ n ];
        this->dispatch_cnt++;
        return true;
      }
    }
    n = ::poll( this->poll_set, this->nevents, time_ms );
    if ( n == 0 )
      return false;
  }
#endif
}

poll_event_t
Console::get_event( void ) noexcept
{
#ifdef _MSC_VER
  return GetStdHandle( STD_INPUT_HANDLE );
#else
  poll_event_t ev;
  ev.fd      = STDIN_FILENO;
  ev.events  = POLLIN | POLLRDHUP;
  ev.revents = 0;
  return ev;
#endif
}

bool
Console::dispatch( void ) noexcept
{
  enum {
    DO_HELP    = 1,
    NO_SERVER  = 2,
    NO_BANG    = 3,
    NO_CHANNEL = 4,
    NO_MATCH   = 5
  };
  static const char *status_string[ 6 ] = {
    0, 0, "No server connection", "No bang command matchss",
    "No channel to send message", "Not joined that channel"
  };
  uint32_t i, len;
  int n      = lc_tty_get_line( &this->tty ),
      status = 0;
  if ( n < 0 )
    return false;
  if ( this->tty.lc_status == LINE_STATUS_EXEC ) {
    lc_tty_normal_mode( &this->tty );

    if ( this->tty.line_len == 1 ) {
      if ( this->tty.line[ 0 ] == 'q' ) {
        this->poll.quit = true;
        return false;
      }
      bool q = ( this->tty.line[ 0 ] == '?' ),
           s = ( this->tty.line[ 0 ] == '/' ),
           b = ( this->tty.line[ 0 ] == '!' );
      if ( q || s || b )
        status = DO_HELP;
    }
    if ( status == 0 && this->tty.line_len > 0 ) {
      if ( this->tty.line[ 0 ] == '/' ) { /* irc command */
        if ( ! this->conn->is_connected() )
          status = NO_SERVER;
        else {
          for ( i = 0; i < nslash_cmds; i++ ) {
            size_t len = ::strlen( slash_cmd[ i ].cmd );
            if ( ::strncasecmp( slash_cmd[ i ].cmd, this->tty.line, len ) == 0 &&
                 this->tty.line[ len ] == ' ' ) {
              (this->*slash_cmd[ i ].dispatch_cmd)();
              break;
            }
          }
          if ( i == nslash_cmds )
            this->conn->send_buf.print( "%s\r\n", &this->tty.line[ 1 ] );
        }
      }
      else if ( this->tty.line[ 0 ] == '!' ) { /* local command */
        for ( len = 0; ; ) {
          if ( ++len == this->tty.line_len || this->tty.line[ len ] == ' ' )
            break;
        }
        for ( i = 0; i < nbang_cmds; i++ ) {
          if ( len == ::strlen( bang_cmd[ i ].cmd ) &&
               ::strncasecmp( this->tty.line, bang_cmd[ i ].cmd, len ) == 0 ) {
            (this->*bang_cmd[ i ].dispatch_cmd)();
            if ( this->poll.quit )
              return false;
            break;
          }
        }
        if ( i == nbang_cmds )
          status = NO_BANG;
      }
      else if ( this->tty.line[ 0 ] == '#' ) { /* switch chan */
        IRC_Channel *chan;
        for ( chan = this->state.hd; chan != NULL; chan = chan->next ) {
          if ( ::strncasecmp( this->tty.line, chan->chan.str,
                              this->tty.line_len ) == 0 ) {
            this->state.pop_chan( chan );
            this->state.push_chan_hd( chan );
            this->update_prompt = true;
            break;
          }
        }
        if ( chan == NULL )
          status = NO_MATCH;
      }
      else if ( this->state.hd != NULL ) {
        if ( ! this->conn->is_connected() )
          status = NO_SERVER;
        else {
          IRC_String text( this->tty.line, this->tty.line_len );
          this->send_privmsg( CTCP_NONE, this->state.hd->chan, text );
        }
      }
      else {
        status = NO_CHANNEL;
      }
      lc_tty_log_history( &this->tty );
      if ( this->conn->is_connected() )
        this->conn->flush();
    }
    if ( status != 0 ) {
      if ( status != DO_HELP )
        this->print( "%s\n", status_string[ status ] );
      else
        this->show_help( this->tty.line[ 0 ] );
    }
    this->flush();
  }
  else if ( this->tty.lc_status == LINE_STATUS_COMPLETE ) {
    if ( lc_get_complete_type( &this->lc ) == COMPLETE_HELP )
      this->show_help( 1 );
    else if ( lc_get_complete_type( &this->lc ) == COMPLETE_MAN )
      this->show_channels( 1 );
  }
  if ( this->tty.lc_status > LINE_STATUS_RD_EAGAIN )
    return true;
  reset_console_event( this->poll.poll_set[ this->index ] );
  return false;
}

void
Console::send_privmsg( CtcpType ctcp,  IRC_String &recv,
                       IRC_String &text ) noexcept
{
  const char *pre = "", *suf = "";
  if ( ctcp == CTCP_ACTION ) {
    pre = "\001ACTION";
    suf = "\001";
  }
  this->conn->send_buf.print( "PRIVMSG %.*s :%s%.*s%s\r\n",
                             recv.len, recv.str, pre, text.len, text.str, suf );
  bool prompt = false;
  MsgPrint printer( this->state, this->fmt_buf, prompt, this->tty.cols );
  Message msg( this->state, recv, text );
  printer.print_msg( msg );

  if ( this->state.log_fd != -1 ) {
    BufQueue tmp;
    tmp.print( ":%s PRIVMSG %.*s :%s%.*s%s\r\n", this->state.chan_id.str,
               recv.len, recv.str, pre, text.len, text.str, suf );
    uint32_t nbytes;
    char *ptr = tmp.consume_buf( nbytes );
    os_write( this->state.log_fd, ptr, nbytes );
    tmp.reset();
  }
}

void
Console::bang_connect( void ) noexcept
{
  this->conn->connect( &this->fmt_buf );
}

void
Console::bang_help( void ) noexcept
{
  this->show_help( 0 );
}

void
Console::bang_channels( void ) noexcept
{
  this->show_channels( 0 );
}

void
Console::bang_quiet( void ) noexcept
{
  this->state.quiet = ! this->state.quiet;
  this->print( "quiet = %s\n", this->state.quiet ? "true" : "false" );
}

void
Console::bang_quit( void ) noexcept
{
  this->poll.quit = true;
}

bool
Console::parse_recv_text( uint32_t i,  IRC_String &recv,
                          IRC_String &text ) noexcept
{
  uint32_t len  = this->tty.line_len;
  char   * line = this->tty.line;
  while ( i < len && isspace( line[ i ] ) )
    i++;
  char *target = &line[ i ];
  while ( i < len && ! isspace( line[ i ] ) )
    i++;
  uint32_t target_len = (uint32_t) ( &line[ i ] - target );
  while ( i < len && isspace( line[ i ] ) )
    i++;
  if ( i == len || target_len == 0 ) {
    this->fmt_buf.print( "Not enough args: <recv> <text>\n" );
    return false;
  }
  recv.str = target;
  recv.len = target_len;
  text.str = &line[ i ];
  text.len = len - i;
  return true;
}

void
Console::slash_msg( void ) noexcept
{
  IRC_String recv, text;
  if ( ! this->parse_recv_text( sizeof( "/msg" ), recv, text ) )
    return;
  this->send_privmsg( CTCP_NONE, recv, text );
}

void
Console::slash_me( void ) noexcept
{
  IRC_String recv, text;
  if ( ! this->parse_recv_text( sizeof( "/me" ), recv, text ) )
    return;
  this->send_privmsg( CTCP_ACTION, recv, text );
}

void
Console::complete( const char *, size_t , size_t ) noexcept
{
  int    arg_num,   /* which arg is completed, 0 = first */
         arg_count, /* how many args */
         arg_off[ 32 ],  /* offset of args */
         arg_len[ 32 ];  /* length of args */
  char   buf[ 1024 ];
  bool   add_slash    = false,
         add_bang     = false,
         add_nicks    = false,
         add_channels = false;

  int n = lc_tty_get_completion_cmd( &this->tty, buf, sizeof( buf ),
                                     &arg_num, &arg_count, arg_off,
                                     arg_len, 32 );
  if ( n < 0 )
    return;

  if ( arg_count > 0 && arg_len[ arg_num ] > 0 ) {
    if ( arg_num == 0 && buf[ arg_off[ 0 ] ] == '/' )
      add_slash = true;
    else if ( arg_num == 0 && buf[ arg_off[ 0 ] ] == '!' )
      add_bang = true;
    else if ( buf[ arg_off[ arg_num ] ] == '#' )
      add_channels = true;
  }
  if ( ! add_slash && ! add_bang && ! add_channels )
    add_nicks = true;

  if ( add_slash ) {
    for ( uint32_t i = 0; i < nmsg_strings; i++ ) {
      uint32_t j, k = 0, cmd_len = msg_string[ i ].cmd_len;
      char buf[ 16 ];
      buf[ k++ ] = '/';
      for ( j = 0; j < cmd_len; j++ )
        buf[ k++ ] = tolower( msg_string[ i ].cmd_name[ j ] );
      if ( cmd_len == 7 && ( msg_string[ i ].msg_type == M_USERHOST ||
                             msg_string[ i ].msg_type == M_SERVLIST ) )
        buf[ k++ ] = 't';

      buf[ k ] = '\0';
      lc_add_completion( &this->lc, buf, k );
    }
  }
  if ( add_bang ) {
    for ( uint32_t i = 0; i < nbang_cmds; i++ ) {
      lc_add_completion( &this->lc, bang_cmd[ i ].cmd,
                         ::strlen( bang_cmd[ i ].cmd ) );
    }
  }
  if ( add_channels ) {
    IRC_String id( &buf[ arg_off[ arg_num ] ], arg_len[ arg_num ] );
    if ( arg_num == 0 ) { /* switch to active channel */
      for ( IRC_Channel *chan = this->state.hd; chan != NULL;
            chan = chan->next ) {
        lc_add_completion( &this->lc, chan->chan.str, chan->chan.len );
      }
    }
    else { /* /join a channel */
      uint32_t k;
      if ( this->state.chan_tab.find_first_prefix( id, k ) ) {
        do {
          IRC_String name;
          this->state.chan_tab.val( k, name );
          lc_add_completion( &this->lc, name.str, name.len );
        } while ( this->state.chan_tab.find_next_prefix( id, k ) );
      }
    }
  }
  if ( add_nicks && this->state.hd != NULL ) {
    IRC_String id( &buf[ arg_off[ arg_num ] ], arg_len[ arg_num ] );
    IRC_Channel & chan = *this->state.hd;
    uint32_t k;
    if ( chan.nick_tab.find_first_prefix( id, k ) ) {
      do {
        IRC_String nick;
        chan.nick_tab.val( k, nick );
        lc_add_completion( &this->lc, nick.str, nick.len );
      } while ( chan.nick_tab.find_next_prefix( id, k ) );
    }
  }
}

void
Console::show_help( char which ) noexcept
{
  char buf[ 120 ];
  uint32_t i;
  if ( which <= 1 || which == '/' || which == '?' ) {
    for ( i = 0; i < nmsg_strings; i++ ) {
      uint32_t j, k = 0, cmd_len = msg_string[ i ].cmd_len;
      buf[ k++ ] = '/';
      for ( j = 0; j < cmd_len; j++ )
        buf[ k++ ] = tolower( msg_string[ i ].cmd_name[ j ] );
      if ( cmd_len == 7 && ( msg_string[ i ].msg_type == M_USERHOST ||
                             msg_string[ i ].msg_type == M_SERVLIST ) )
        buf[ k++ ] = 't';

      for ( j = 0; j < ncmd_descrs; j++ ) {
        if ( cmd_descr[ j ].msg_type == msg_string[ i ].msg_type ) {
          int n = ( k < 20 ? 20 - k : 0 );
          k += ::snprintf( &buf[ k ], sizeof( buf ) - k, " %-*s -- %s", n,
                           cmd_descr[ j ].args, cmd_descr[ j ].descr );
          break;
        }
      }
      buf[ k ] = '\0';
      if ( which == 1 )
        lc_add_completion( &this->lc, buf, k );
      else
        this->fmt_buf.print( "%.*s\n", k, buf );
    }
    for ( i = 0; i < nslash_cmds; i++ ) {
      uint32_t k;
      k = ::snprintf( buf, sizeof( buf ), "%s %-*s -- %s",
                      slash_cmd[ i ].cmd,
                      20 - (int) ::strlen( slash_cmd[ i ].cmd ),
                      slash_cmd[ i ].args, slash_cmd[ i ].descr );
      if ( which == 1 )
        lc_add_completion( &this->lc, buf, k );
      else
        this->fmt_buf.print( "%.*s\n", k, buf );
    }
  }
  if ( which <= 1 || which == '!' || which == '?' ) {
    for ( i = 0; i < nbang_cmds; i++ ) {
      int n = ::snprintf( buf, sizeof( buf ), "%-21s -- %s",
                          bang_cmd[ i ].cmd, bang_cmd[ i ].descr );
      if ( which == 1 )
        lc_add_completion( &this->lc, buf, n );
      else
        this->fmt_buf.print( "%.*s\n", n, buf );
    }
  }
}

void
Console::show_channels( char which ) noexcept
{
  const char not_found[] = "Use /list to populate";
  uint32_t buflen;
  char *log = this->state.chan_log.consume_buf( buflen );
  if ( buflen == 0 ) {
    if ( which == 1 )
      lc_add_completion( &this->lc, not_found, sizeof( not_found ) - 1 );
    else
      this->fmt_buf.print( "%s\n", not_found );
    return;
  }
  BufQueue tmp_buf,
         & out_buf = ( which == 1 ? tmp_buf : this->fmt_buf );
  uint32_t cols    = this->tty.cols;
  if ( which == 1 )
    cols -= 1;
  for (;;) {
    char * eol = (char *) ::memchr( log, '\n', buflen );
    if ( eol == NULL )
      break;

    uint32_t linelen = (uint32_t) ( &eol[ 1 ] - log );

    Message  msg( log, linelen );
    MsgPrint printer( this->state, out_buf, this->update_prompt, cols,
                      SHOW_CHANNELS );
    if ( msg.parse() ) {
      printer.print_msg( msg );
      if ( which == 1 ) {
        uint32_t avail;
        char * ptr = out_buf.consume_buf( avail ),
             * end = &ptr[ avail ];
        while ( ptr < end ) {
          char * eol = (char *) ::memchr( ptr, '\n', end - ptr );
          if ( eol == NULL )
            break;
          lc_add_completion( &this->lc, ptr, eol - ptr );
          ptr = &eol[ 1 ];
        }
        out_buf.consume_incr( avail );
      }
    }
    log = &log[ linelen ];
    buflen -= linelen;
  }
  tmp_buf.reset();
}

int
Console::print( const char *fmt, ... ) noexcept
{
  va_list args;
  va_start( args, fmt );
  int n = lc_tty_printv( &this->tty, fmt, args );
  va_end( args );
  return n;
}

int
Console::write( const void *buf, uint32_t buflen ) noexcept
{
  return lc_tty_write( &this->tty, buf, buflen );
}

poll_event_t
Connection::get_event( void ) noexcept
{
  return open_connection_event( this->sock );
}

void
Connection::close( void ) noexcept
{
  close_connection_event( this->poll.poll_set[ this->index ], this->sock );
  this->poll.remove( this );
  this->send_buf.reset();
  this->recv_buf.reset();
}

bool
Connection::dispatch( void ) noexcept
{
  char   * ptr;
  uint32_t buflen;
  int      n;

  ptr = this->recv_buf.append_buf( buflen );
  for (;;) {
    n = ::recv( this->sock, ptr, buflen, 0 );

    if ( n <= 0 ) {
      if ( n == 0 ) {
        this->console.print( "Close %u\n", this->index );
        this->close();
      }
      else {
        reset_connection_event( this->poll.poll_set[ this->index ] );
      }
      break;
    }
    ptr = this->recv_buf.append_incr( (uint32_t) n, buflen );
  }

  ptr = this->recv_buf.consume_buf( buflen );
  while ( buflen > 0 ) {
    char * eol = (char *) ::memchr( ptr, '\n', buflen );
    if ( eol == NULL )
      break;

    uint32_t linelen = (uint32_t) ( &eol[ 1 ] - ptr );
    if ( this->state.log_fd != -1 )
      os_write( this->state.log_fd, ptr, linelen );

    Message  msg( ptr, linelen );
    MsgPrint printer( this->state, this->console.fmt_buf,
                      this->console.update_prompt, this->console.tty.cols );
    if ( msg.parse() )
      printer.print_msg( msg );
    else if ( msg.msg_len > 5 && ::memcmp( msg.msg, "PING ", 5 ) == 0 )
      this->send_buf.print( "PONG %.*s\r\n", msg.msg_len - 5, &msg.msg[ 5 ] );
    else if ( msg.msg_len > 6 && ::memcmp( msg.msg, "ERROR ", 6 ) == 0 )
      printer.print_error( msg );
    else
      printer.print_parse_error( msg );
    ptr = this->recv_buf.consume_incr( msg.msg_len, buflen );
  }
  this->flush();
  this->console.flush();
  return false;
}

void
Console::flush( void ) noexcept
{
  uint32_t buflen;
  char * ptr = this->fmt_buf.consume_buf( buflen );
  this->write( ptr, buflen );
  if ( this->update_prompt )
    this->make_prompt( true );
  this->fmt_buf.consume_incr( buflen );
}

void
MsgPrint::print_error( Message &msg ) noexcept
{
  this->fmt_buf.print( ANSI_RED "%.*s" ANSI_NORMAL "\n", msg.line_len(),
                       msg.msg );
}

void
MsgPrint::print_parse_error( Message &msg ) noexcept
{
  this->fmt_buf.print( ANSI_RED "unable to parse " ANSI_NORMAL
                       "[%.*s]\n", msg.line_len(), msg.msg );
}

static bool
is_nick_char( char c ) noexcept /* may need unicode update */
{
  if ( isalpha( c ) ) return true;
  if ( isdigit( c ) ) return true;
  if ( c == '_' || c == '-' || c == '^' || c == '{' || c == '}' ||
       c == '[' || c == ']' || c == '\\' || c == '|' || c == '`' )
    return true;
  return false;
}

void
MsgPrint::print_channel( IRC_String &chan ) noexcept
{
  char     buf[ 128 ];
  uint32_t len = 0, pad = 2;

  if ( this->print_timestamp ) {
    time_t t = time( NULL );
    struct tm tmbuf, * lt = localtime_r( &t, &tmbuf );
    len = ::snprintf( buf, sizeof( buf ),
                      ANSI_BLUE "%02d%02d%02d" ANSI_NORMAL " ",
                      lt->tm_hour, lt->tm_min, lt->tm_sec );
    pad += 2 + 2 + 2 + 1;
  }
  buf[ len++ ] = '[';
  len += chan_fmt_color( chan.str, chan.len, &buf[ len ] );
  const char fmt2[] = "%.*s" ANSI_NORMAL "]";
  ::memcpy( &buf[ len ], fmt2, sizeof( fmt2 ) );
  len += sizeof( fmt2 ) - 1;
  this->fmt_buf.print_col( this->left, buf, pad, chan.len, chan.str );
  this->right.off = this->left.off;
}

void
MsgPrint::print_nick( IRC_String &nick,  char suffix ) noexcept
{
  char     buf[ 128 ];
  uint32_t len;
  if ( nick.equal( this->state.nick ) ) { /* I sent it */
    const char fmt[] = ANSI_GREEN "%.*s" ANSI_NORMAL ": ";
    len   = sizeof( fmt ) - 1;
    ::memcpy( buf, fmt, sizeof( fmt ) );
  }
  else {
    len = nick_fmt_color( nick.str, nick.len, buf );
    const char fmt2[] = "%.*s" ANSI_NORMAL ": ";
    ::memcpy( &buf[ len ], fmt2, sizeof( fmt2 ) );
    len += sizeof( fmt2 ) - 1;
  }
  uint32_t pad = 2;
  if ( suffix == ' ' ) /* no text following, remove ':' */
    buf[ len - 2 ] = ' ';
  else if ( suffix == 0 ) {
    buf[ len - 2 ] = '\0';
    pad = 0;
  }
  this->fmt_buf.print_col( this->right, buf, pad, nick.len, nick.str );
}

void
MsgPrint::print_msg( Message &msg ) noexcept
{
  IRC_Channel * chan = NULL;
  IRC_String    id;
  uint32_t      msg_type = msg.msg_type,
                param_off;
  bool          show_msg = true;

  switch ( msg_type ) {
    case M_RPL_LISTSTART:
      this->state.chan_log.reset();
      break;

    case M_RPL_LISTEND:
      break;

    case M_RPL_LIST:
      if ( ! this->show_channels ) { /* always print and discard */
        if ( this->state.nick.equal( msg.param_str( 0, id ) ) ) {
          msg.param_str( 1, id );
          if ( id.len > 0 ) {
            this->state.update_chan_tab( id );
            this->state.chan_log.append_msg( msg.msg, msg.msg_len );
          }
          if ( this->state.quiet )
            show_msg = false;
        }
      }
      break;
      
    case M_RPL_NAMREPLY:
      /* look for 353 [nick] [*] [chan ] :name1 name2 name3 */
      chan = this->state.find_chan( msg.param_str( 2, id ) );
      if ( chan != NULL && this->state.nick.equal( msg.param_str( 0, id ) ) ) {
        if ( msg.text_len > 0 ) {
          char   tmp_buf[ 8 * 1024 ];
          char * tmp = ( msg.text_len >= 8 * 1024 ?
                         (char *) ::malloc( msg.text_len + 1 ) : tmp_buf ),
               * p   = tmp;
          ::memcpy( tmp, msg.text, msg.text_len );
          tmp[ msg.text_len ] = '\0';
          /* remove '+' prefixes */
          for (;;) {
            p = (char *) ::memchr( p, '+', &tmp[ msg.text_len ] - p );
            if ( p == NULL )
              break;
            if ( p == tmp || *(p - 1) == ' ' )
              *p = ' ';
          }
          chan->add_nicks( tmp, msg.text_len );
          if ( tmp != tmp_buf )
            ::free( tmp );
        }
        if ( this->state.quiet )
          show_msg = false;
      }
      break;

    case M_JOIN:
      if ( this->state.nick.equal( msg.nick, msg.nick_len ) ) {
        chan = this->state.join_channel( msg.param_str( 0, id ),
                                         msg.prefix, msg.prefix_len );
        this->update_prompt = true;
      }
      else {
        chan = this->state.find_chan( msg.param_str( 0, id ) );
        if ( this->state.quiet )
          show_msg = false;
      }
      if ( chan != NULL ) {
        if ( msg.nick_len > 0 )
          if ( ! chan->add_nick( msg.nick, msg.nick_len ) )
            this->fmt_buf.print( "\"%.*s\" already joined\n", msg.nick_len,
                                 msg.nick );
      }
      break;

    case M_PART:
      chan = this->state.find_chan( msg.param_str( 0, id ) );
      if ( chan != NULL ) {
        if ( this->state.nick.equal( msg.nick, msg.nick_len ) ) {
          this->state.part_channel( chan );
          this->update_prompt = true;
          chan = NULL;
        }
        else {
          if ( msg.nick_len > 0 ) {
            if ( ! chan->del_nick( msg.nick, msg.nick_len ) )
              this->fmt_buf.print( "\"%.*s\" not found\n", msg.nick_len,
                                   msg.nick );
          }
          if ( this->state.quiet )
            show_msg = false;
        }
      }
      break;

    case M_NICK:
      if ( this->state.nick.equal( msg.nick, msg.nick_len ) ) {
        this->state.nick.set( msg.text, msg.text_len );
        this->update_prompt = true;
      }
      else if ( this->state.quiet )
        show_msg = false;
      for ( chan = this->state.hd; chan != NULL; chan = chan->next ) {
        if ( chan->del_nick( msg.nick, msg.nick_len ) )
          chan->add_nick( msg.text, msg.text_len );
      }
      break;

    case M_QUIT:
      for ( chan = this->state.hd; chan != NULL; chan = chan->next )
        chan->del_nick( msg.nick, msg.nick_len );
      if ( this->state.quiet )
        show_msg = false;
      break;

    case M_PRIVMSG:
      chan = this->state.find_chan( msg.param_str( 0, id ) );
      break;
  }
  if ( ! show_msg )
    return;

  if ( msg_type == M_PRIVMSG ) {
    this->print_channel( msg.param_str( 0, id ) );
    param_off = 1;
  }
  else if ( msg_type == M_RPL_LIST ) {
    this->print_timestamp = false;
    this->print_channel( msg.param_str( 1, id ) );
    this->right.off = this->left.off;
    param_off = 2;
  }
  else {
    this->fmt_buf.print_col( this->left, ANSI_GREEN "%.*s" ANSI_NORMAL,
                             0, msg.command_len, msg.command );
    this->right.off = this->left.off;
    param_off = 0;
  }
  for ( uint32_t i = param_off; i < msg.nparams; i++ ) {
    this->fmt_buf.print_col(
      this->right, "[" ANSI_BLUE "%.*s" ANSI_NORMAL "] ", 3,
      msg.param_len[ i ], msg.param[ i ] );
  }

  if ( msg.nick_len > 0 ) {
    if ( msg_type == M_PRIVMSG &&
         this->right.off + msg.nick_len < this->nick_pad &&
         this->right.off + this->nick_pad < this->right.end )
      this->fmt_buf.pad_to( this->right, this->nick_pad - msg.nick_len );
    this->print_nick( msg.nick_str( id ), msg.text_len > 0 ? ':' : ' ' );
  }
  /* scan for nicks in the prefix of text */
  uint32_t id_off = 0, id_end = 0;
  if ( msg_type == M_PRIVMSG ) {
    for (;;) {
      IRC_String id( (char *) &msg.text[ id_off ], 0 );
      for ( ; id_off + id.len < msg.text_len; id.len++ )
        if ( ! is_nick_char( id.str[ id.len ] ) )
          break;
      if ( id.len == 0 || chan == NULL || ! chan->is_nick( id ) )
        break;

      this->print_nick( id, 0 );
      id_end += id.len;
      id_off += id.len;

      for ( ; id_off < msg.text_len; id_off++ )
        if ( is_nick_char( id.str[ id_off ] ) )
          break;
      if ( id_off == id_end )
        break;
      this->fmt_buf.print_col( this->right, "%.*s", 0, id_off - id_end,
                               &msg.text[ id_end ] );
      id_end = id_off;
    }
  }
  if ( msg.text_len > id_off )
    this->fmt_buf.print_text( this->right, msg.text_len - id_off,
                              &msg.text[ id_off ] );
  this->fmt_buf.print( "\n" );
}

void
Connection::flush( void ) noexcept
{
  uint32_t buflen;
  char * ptr = this->send_buf.consume_buf( buflen );
  if ( buflen > 0 ) {
    int n = ::send( this->sock, ptr, buflen, 0 );
    if ( n > 0 )
      this->send_buf.consume_incr( (uint32_t) n );
  }
}

int
Connection::print( const char *fmt, ... ) noexcept
{
  char buf[ 8 * 1024 ];

  va_list args;
  va_start( args, fmt );
  int n = ::vsnprintf( buf, sizeof( buf ), fmt, args );
  va_end( args );
  ::send( this->sock, buf, n, 0 );
  return n;
}

void
Console::make_prompt( bool show ) noexcept
{
  static const char fmt_init[] = ANSI_GREEN   "%s"    ANSI_NORMAL "@" \
                                 ANSI_MAGENTA "%s:%s" ANSI_NORMAL "> ";
  char prompt_buf[ sizeof( fmt_init ) + 8 +
                   MAX_USER_LEN + MAX_HOST_LEN + MAX_CHANNEL_LEN ];
  const char * host = ( this->state.host.len ? this->state.host.str : "local" );
  const char * port = ( this->state.port.len ? this->state.port.str : "6667" );
  if ( this->state.hd != NULL ) {
    char     buf[ 80 ];
    uint32_t len = 0;
    len = sizeof( fmt_init ) - 3; /* "\\$ \0" */
    ::memcpy( buf, fmt_init, len );
    buf[ len++ ] = '[';
    len += chan_fmt_color( this->state.hd->chan.str, this->state.hd->chan.len,
                           &buf[ len ] );
    const char fmt2[] = "%s" ANSI_NORMAL "]> ";
    ::memcpy( &buf[ len ], fmt2, sizeof( fmt2 ) );
    ::snprintf( prompt_buf, sizeof( prompt_buf ), buf,
        this->state.nick.str, host, port, this->state.hd->chan.str );
  }
  else {
    ::snprintf( prompt_buf, sizeof( prompt_buf ), fmt_init,
                this->state.nick.str, host, port );
  }
  lc_tty_set_prompt( &this->tty, TTYP_PROMPT1, prompt_buf );
  if ( show )
    lc_tty_show_prompt( &this->tty );
  this->update_prompt = false;
}

int
Console::complete_cb( LineCook *,  const char *buf,  size_t off,
                      size_t len,  void *arg ) noexcept
{
  ((Console *) arg)->complete( buf, off, len );
  return 0;
}

bool
Connection::connect( BufQueue *out ) noexcept
{
  struct addrinfo * res, * iter, hints;
  const char * port = ( this->state.port.len ? this->state.port.str : "6667" );

  ::memset( &hints, 0, sizeof( hints ) );
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  this->gai_status = ::getaddrinfo( this->state.host.str, port, &hints, &res );
  if ( this->gai_status != 0 )
    goto on_error;
  for ( iter = res; iter != NULL; iter = iter->ai_next ) {
    this->sock = ::socket( iter->ai_family, iter->ai_socktype,
                           iter->ai_protocol );
    if ( is_invalid( this->sock ) ) {
      continue;
    }
    if ( ::connect( this->sock, iter->ai_addr, iter->ai_addrlen ) != 0 ) {
      this->sock_errno = errno;
      ::closesocket( this->sock );
      this->sock = INVALID_SOCKET;
      continue;
    }
    break;
  }
  ::freeaddrinfo( res );
  if ( is_invalid( this->sock ) )
    goto on_error;

  this->poll.add( this );
  if ( this->state.pass.len > 0 )
    this->send_buf.print( "PASS %s\r\n", this->state.pass.str );
  this->send_buf.print( "NICK %s\r\n", this->state.nick.str );
  this->send_buf.print( "USER %s - - :%s\r\n", this->state.user.str,
                        this->state.real.str );
  this->flush();
  return true; 

on_error:;
  const char * host = this->state.host.len ? this->state.host.str : "(null)";
  int          status;

  if ( (status = this->gai_status) != 0 )  {
    const char *fmt =
      ANSI_RED "getaddrinfo" ANSI_NORMAL ": %d/%s, host \"%s\" port %s\n";
    if ( out != NULL )
      out->print( fmt, status, gai_strerror( status ), host, port );
    else
      fprintf( stderr, fmt, status, gai_strerror( status ), host, port );
  }
  else {
    const char *fmt =
      ANSI_RED "sock err" ANSI_NORMAL ": %d/%s host \"%s\" port %s\n";
    status = this->sock_errno;
    if ( out != NULL )
      out->print( fmt, status, strerror( status ), host, port );
    else
      fprintf( stderr, fmt, status, strerror( status ), host, port );
  }
  return false;
}

static const char *
get_arg( int argc, const char *argv[], int b, const char *f, const char *g,
         const char *def ) noexcept
{
  for ( int i = 1; i < argc - b; i++ )
    if ( ::strcmp( f, argv[ i ] ) == 0 || ::strcmp( g, argv[ i ] ) == 0 )
      return argv[ i + b ];
  return def; /* default value */
}

static int
replay_log( IRC_State &state,  const char *fn,  int fd,
            bool null_output ) noexcept
{
  int          rfd = os_open( fn, O_RDONLY, 0 );
  const char * map;
  os_stat      st;
  if ( rfd < 0 || os_fstat( rfd, &st ) != 0 ||
       (map = os_map_read( st.st_size, rfd )) == NULL ) {
    perror( fn );
    return 1;
  }
  size_t off = 0, size = st.st_size;
  const char * p;
  bool update_prompt = false;
  uint32_t avail;
  int lines = 25, cols = 80;
  char * fmt;
  BufQueue fmt_buf;
  lc_tty_get_terminal_geom( fd, &cols, &lines );
  for (;;) {
    p = (const char *) ::memchr( &map[ off ], '\n', size - off );
    if ( p == NULL )
      break;
    size_t linelen = ( &p[ 1 ] - &map[ off ] );
    Message msg( &map[ off ], (uint32_t) linelen );
    off += linelen;

    MsgPrint printer( state, fmt_buf, update_prompt, cols, 0 );
    if ( msg.parse() )
      printer.print_msg( msg );
    else if ( msg.msg_len > 5 && ::memcmp( msg.msg, "PING ", 5 ) == 0 )
      ;
    else if ( msg.msg_len > 6 && ::memcmp( msg.msg, "ERROR ", 6 ) == 0 )
      printer.print_error( msg );
    else
      printer.print_parse_error( msg );
    fmt = fmt_buf.consume_buf( avail );
    if ( avail > 1024 ) {
      if ( ! null_output )
        os_write( fd, fmt, avail );
      fmt_buf.consume_incr( avail );
    }
  }
  fmt = fmt_buf.consume_buf( avail );
  if ( avail > 0 ) {
    if ( ! null_output )
      os_write( fd, fmt, avail );
    fmt_buf.consume_incr( avail );
  }
  os_unmap_read( map, st.st_size );
  os_close( rfd );
  fmt_buf.reset();
  return 0;
}

int
main( int argc, const char *argv[] )
{
  IRC_State    state;
  const char * log_fn,
             * read_fn,
             * hist_fn;
  bool         preload = false;

  state.user  = get_arg( argc, argv, 1, "-u", "-user", /*"anon153"*/ NULL );
  state.host  = get_arg( argc, argv, 1, "-s", "-host", /*"irc.libera.chat" */NULL);
  state.port  = get_arg( argc, argv, 1, "-p", "-port", "6667" );
  state.nick  = get_arg( argc, argv, 1, "-n", "-nick", /*"lc153"*/ NULL );
  state.real  = get_arg( argc, argv, 1, "-r", "-real", NULL );
  state.pass  = get_arg( argc, argv, 1, "-k", "-pass", NULL );
  state.quiet = get_arg( argc, argv, 0, "-q", "-quiet", NULL ) != NULL;
  log_fn      = get_arg( argc, argv, 1, "-l", "-log", NULL );
  read_fn     = get_arg( argc, argv, 1, "-a", "-play", NULL );
  preload     = get_arg( argc, argv, 0, "-e", "-preload", NULL ) != NULL;
  hist_fn     = get_arg( argc, argv, 1, "-x", "-hist", "irc_hist.txt" );

  if ( state.user.len == 0 ) state.user.set( state.nick );
  if ( state.real.len == 0 ) state.real.set( state.nick );

  if ( get_arg( argc, argv, 0, "-h", "-help", NULL ) != NULL ||
       state.nick.len == 0 ) {
    if ( state.nick.len == 0 )
      fprintf( stderr, "must provide a -n nick\n" );
    fprintf( stderr, "Connect and load a terminal:\n" );
    fprintf( stderr, "usage: %s [-n nick] [-u user] [-h host] [-p port] "
                     "[-r real] [-k pass] [-l log] [-x hist] [-q]\n", argv[ 0 ] );
    fprintf( stderr, "Print contents of a log file:\n" );
    fprintf( stderr, "usage: %s [-a log] [-n nick]\n", argv[ 0 ] );
    return 1;
  }
  if ( read_fn != NULL ) {
    int status = replay_log( state, read_fn, STDOUT_FILENO, preload );
    if ( status != 0 )
      return status;
    if ( ! preload )
      return 0;
  }
  if ( log_fn != NULL ) {
    state.log_fn.set( log_fn );
    state.log_fd = os_open( log_fn, O_WRONLY | O_APPEND | O_CREAT, 0666 );
    if ( state.log_fd < 0 ) {
      perror( log_fn );
      return 1;
    }
  }
  Poller       poll;
  LineCook   * lc  = lc_create_state( 80, 25 );
  TTYCook    * tty = lc_tty_create( lc );
  Console      console( poll, *lc, *tty, state );
  Connection   conn( poll, console, state );
#ifdef _MSC_VER
  WORD         ver;
  WSADATA      wdata;
  ver = MAKEWORD( 2, 2 );
  WSAStartup( ver, &wdata );
#endif

  if ( state.host.len != 0 && ! conn.connect() )
    return 1;

  lc_tty_set_locale();
  lc_set_completion_break( lc, " ", 1 );
  lc_set_quotables( lc, " ", 1, '\"' );
  lc_tty_open_history( tty, hist_fn );
  lc_tty_init_fd( tty, STDIN_FILENO, STDOUT_FILENO );
  lc_tty_set_color_prompts( tty );
  console.make_prompt( false );
  lc_tty_init_geom( tty );
  lc_tty_init_sigwinch( tty );
  lc_tty_show_prompt( tty );

  poll.add( &console );
  while ( ! poll.quit ) {
    EventDispatch *ev;
    if ( poll.next_event( ev, 1000 ) ) {
      while ( ev->dispatch() )
        ;
    }
  }
  lc_tty_normal_mode( tty );
  printf( "\nbye\n" );
  fflush( stdout );
  return 0;
}
