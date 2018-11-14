/* vim: set expandtab sw=2 sts=2 fenc=utf-8 : */
/*
 * Runscript    Run a login-or-something script.
 *    A basic like "programming language".
 *    This program also looks like a basic interpreter :
 *    a bit messy. (But hey, I'm no compiler writer :-))
 *
 * Author:  Yuichi Nakai, xoxyuxu@gmail.com
 *
 *    This file is part of the minicom communications package,
 *    Copyright 2018 Yuichi Nakai
 *
 *    This program is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU General Public License
 *    as published by the Free Software Foundation; either version
 *    2 of the License, or (at your option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/wait.h>
#include <stdarg.h>

#include "port.h"
#include "minicom.h"
#include "intl.h"

#define MAX_INBUF_SIZE  512
#define MAX_NUM_EXPECT  16
#define DFL_GTIMEOUT  (60 * 60)
#define DFL_ETIMEOUT  (60 * 2)

int gtimeout = DFL_GTIMEOUT;          /* Global Timeout */
int etimeout_default = DFL_ETIMEOUT;  /* Default timeout in expect routine */
int etimeout = 0;                     /* Timeout in expect routine */
jmp_buf ejmp;                         /* To jump to if expect times out */
bool inexpect = false;                /* Are we in the expect routine */
const char *s_login = "name";         /* User's login name */
const char *s_pass = "password";      /* User's password */
char homedir[256];                    /* Home directory */
char logfname[PARS_VAL_LEN];          /* Name of logfile */
const char* scriptfn;

int laststatus = 0;                   /* Status of last command */
bool verbose = true;
const char *newline;                  /* What to print for '\n'. */
struct line *thisline;                /* Line to be executed */

/* under construnction */
/* are we need locking operation? No, This program is single thread. */
struct {
  char buf[MAX_INBUF_SIZE];
  int idx_next_write;
  int idx_next_read;
  int len;
  bool is_get_cr;
} inbuf;

static void inbuf_flush(void)
{
  inbuf.idx_next_write = 0;
  inbuf.idx_next_read = 0;
  inbuf.len = 0;
  inbuf.is_get_cr = false;
}

static void inbuf_putc( char c )
{
  inbuf.buf[ inbuf.idx_next_write ] = c;
  if (++inbuf.idx_next_write >= sizeof(inbuf.buf))
    inbuf.idx_next_write = 0;

  if (inbuf.idx_next_write == inbuf.idx_next_read) {
    if (++inbuf.idx_next_read >= sizeof(inbuf.buf))
      inbuf.idx_next_read = 0;
  }
  else
    ++inbuf.len ;
}

static int inbuf_read( char* p, int maxlen )
{
  int len, totlen ;
  if (inbuf.idx_next_read == inbuf.idx_next_write)
    return 0;

  totlen = maxlen < inbuf.len ? maxlen : inbuf.len;
  if (inbuf.idx_next_read + totlen >= sizeof(inbuf.buf)) {
    len = sizeof(inbuf.buf) - inbuf.idx_next_read;
    memcpy(p, &inbuf.buf[inbuf.idx_next_read], len);
    inbuf.idx_next_read = 0;
    memcpy(p+len, &inbuf.buf[0], totlen - len);
  }
  else {
    memcpy(p, &inbuf.buf[inbuf.idx_next_read], totlen);
    inbuf.idx_next_read += totlen;
  }
  inbuf.len -= totlen;
return totlen;
}
/* uinder construnction */


/* Lua related files */
#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

#include <stdlib.h>

/**
 * @note How input buffer (inbuf) works
 *
 *  inbuf is treated as a shift register.
 *  index zero is the oldest received character,
 *  index (MAX_INBUF_SIZE - 1) is the newest received character.
 *  The index MAX_INBUF_SIZE is terminator '\0', it is initialized somewhere.
 */

/**
 * @brief Read from stdin(modem port) and save it to inbuf.
 * When verbose==true, this function outputs the character to
 * minicom via stderr.
 */
static int readchar(char*buf)
{
  char c;
  int n;

  while ((n = read(0, &c, 1)) != 1)
    if (errno != EINTR)
      break;

  if (n <= 0)
    return 0;

  if (verbose)
    fputc(c, stderr);
  *buf = c;
  return 1;
}

static int mc_readline(lua_State *L)
{
  luaL_Buffer b;
  char c;
  char *p;
  int len = 0;

  if (sigsetjmp(ejmp, 1) != 0) {
    /* this block execute when longjmp() executed, as timeout */
    inexpect = false;
    lua_pushboolean(L, false);
    return 1;
  }

  etimeout = etimeout_default ;
  inexpect = true;
  while (true) {
    if (readchar(&c) ==0)
      continue;
    inbuf_putc(c);
    if (c=='\n')
      break;
  }
  p = luaL_buffinitsize(L, &b, inbuf.len);
  lua_pushboolean(L, true);
  len = inbuf_read( p, inbuf.len );
  luaL_pushresultsize(&b, len - 1); /* truncate CR */
  return 2;
}

/**
 * @brief Lua function: get environment value
 *
 * @param L lua_State
 *
 * @return 1  bumber of return parameter
 * @note lua function "getenv(varname)".
 *    if varname is LOGIN or PASS, return the value from minicom.
 *    Others, get value from environment named varname.
 */
static int mc_getenv (lua_State *L)
{
const char *p = luaL_checkstring(L,1);

  if (!strcmp(p, "LOGIN"))
      lua_pushstring(L, s_login);
  else if (!strcmp(p, "PASS"))
      lua_pushstring(L, s_pass);
  else lua_pushstring(L, getenv(p));

  return 1;
}

/**
 * find strings from receive data. timeout is set by ...
 * expect( str1,str2,...,str16 )
 * The argument can be 1 to 16.
 */
static int mc_expect (lua_State *L)
{
  int arg_cnt, idx;
  volatile int found = 0;
  struct {
    const char* match;
    const char* p;
  } seq[MAX_NUM_EXPECT + 1] = {{0,}};
  char c;

  arg_cnt = lua_gettop(L);
  if (arg_cnt>MAX_NUM_EXPECT)
    luaL_error(L, _("number of argument of expect() is less than or equal 16."),
        MAX_NUM_EXPECT); /* ithis function is not returned. */
  if (arg_cnt<=0)
    luaL_error(L, _("expect() needs atleast one argument."), MAX_NUM_EXPECT);

  for (idx=1; idx<=arg_cnt; ++idx) {
    seq[idx-1].match = luaL_optstring(L, idx, NULL);
    if (seq[idx-1].match==NULL)
      fprintf(stderr, 
          "expect: arg #%d may be nil. So subsequent arguments are invalid.\r",
          idx);
    seq[idx-1].p = seq[idx-1].match;
  }

  if (sigsetjmp(ejmp, 1) != 0) {
    /* this block execute when longjmp() executed, as timeout */
    inexpect = false;
    lua_pushinteger(L, 0);
    return 1;
  }

  etimeout = etimeout_default ;
  inexpect = true;
  while (!found) {
    if (readchar(&c) ==0)
      continue;

    for (idx = 0; seq[idx].match; ++idx) {
      if (c == *seq[idx].p) {
        ++seq[idx].p;
        if (*seq[idx].p == '\0') {
          found = 1;
          break;
        }
      }
      else
        seq[idx].p = seq[idx].match;
    }
  }
  lua_pushinteger(L, 1 + idx);
  return 1;
}

/**
 * @brief Run a command and send its stdout to stdout ( = modem).
 *
 * @param L[in] lua_State
 *
 * @return 1  Lua-ret ( false )
 * @return 2  Lua-ret ( true, status )
 */
int mc_pipedshell(lua_State *L)
{
  FILE *fp;
  char received[4096];
  size_t read = 0;
  const char* cmd;

  cmd = luaL_optstring(L, 1, NULL);
  if (cmd==NULL || (fp = popen(cmd, "r"))==NULL) {
    lua_pushboolean(L, false);
    return 1;
  }

  while ((read = fread(received, sizeof(char), sizeof(received), fp))) {
    char *sent = received;
#ifdef HAVE_USLEEP
    /* 20 ms delay. */
    usleep(20000);
#endif

    while (read-- > 0)
      fputc(*sent++, stdout);

    fflush(stdout);
  }

  int status = pclose(fp);
  if (WIFEXITED(status))
    laststatus = WEXITSTATUS(status);
  else if (WIFSIGNALED(status))
    laststatus = WTERMSIG(status);
  else
    laststatus = status;
  m_flush(0);

  inbuf_flush();
  lua_pushboolean(L, true);
  lua_pushinteger(L, status);

  return 2;
}


/**
 * @brief write NIL terminated string to fp. Treat '\n' is newline.
 * The global variable 'newline' is set by caller.
 *
 * @param p   NIL terminated string
 * @param fp  FILE pointer (stdout (to modem) or stderr (to minicom))
 *
 * @return 0  fixed.
 */
static int output(const char *p, FILE *fp)
{
#ifdef HAVE_USLEEP
  /* 200 ms delay. */
  usleep(200000);
#endif

  for (; *p; ++p) {
    if (*p == '\n')
      fputs(newline, fp);
    else
      fputc(*p, fp);
  }
  fflush(fp);
  return 0;
}

/**
 * @brief fflush stdin and flush inbuf.
 *
 * @param L
 *
 * @return 0
 */
static int mc_flush(lua_State *L)
{
  (void)L;
  /* Before we send anything, flush input buffer. */
  m_flush(0);
  inbuf_flush();
  return 0;
}

/**
 * @brief Lua function: send( string )
 *          send string to stdout (modem).
 *
 * @param L lua_State
 *
 * @return 1  Lua_ret ( true )
 */
static int mc_send(lua_State *L)
{
const char *p = luaL_checkstring(L,1);

  newline = "\n";

  output(p, stdout);
  lua_pushboolean(L, true);

  return 1;
}

/**
 * @brief Lua function: print( string )
 *          send string to stderr (minicom).
 * @param L lua_State
 *
 * @return 1  Lua_ret ( true )
 */
static int mc_print(lua_State *L)
{
  const char* p;

  p = luaL_checkstring(L,1);

  newline = "\r\n";

  output(p, stderr);
  lua_pushboolean(L, true);

  return 1;
}

/**
 * @brief Lua function: timeout( varname )
 *          Set timeout-time to global time or expect time.
 *
 * @param L lua_State
 *
 * @return  1 Lua_ret ( boolean ).
 *            false if varname is neither "gtime" nor "etime".
 */
int mc_timeout(lua_State *L)
{
  int val;
  const char *p;
  p = luaL_checkstring(L,1);
  val = luaL_optinteger(L,2, DFL_GTIMEOUT);
  if (strcmp(p,"gtime")==0) {
    gtimeout = val;
    lua_pushboolean(L, true);
  }
  else if (strcmp(p,"etime")==0) {
    etimeout_default = val;
    lua_pushboolean(L, true);
  }
  else
    lua_pushboolean(L, false);
  return 1;
}

/**
 * @brief Lua function: verbose( varbool )
 *          Turn verbose on/off (= echo stdin to stderr)
 *
 * @param L Lua_state
 *
 * @return 1  Lua_ret ( varbool )
 *            false if varbool is neither "on" nor "off".
 */
static int mc_verbose(lua_State *L)
{
  const char *p;
  p = luaL_checkstring(L,1);

  if (strcmp(p, "on")==0) {
    lua_pushboolean(L, true);
    return 1;
  }
  else if (strcmp(p, "off")==0) {
    verbose = 0;
    lua_pushboolean(L, true);
  return 1;
  }
  else
    lua_pushboolean(L, false);

//  syntaxerr(_("(unexpected argument)"));
  return 1;
}


/**
 * @brief Array for register functions.
 * "Lua function name", "C function"
 */
static const luaL_Reg minicomlib[] = {
  {"getenv",      mc_getenv},
  {"expect",      mc_expect},
  {"pipedshell",  mc_pipedshell},
  {"send",        mc_send},
  {"flush",       mc_flush},
  {"print",   mc_print},
  {"timeout",   mc_timeout},
  {"verbose",   mc_verbose},
  {"readline",  mc_readline},
  {NULL, NULL}
};

/*
 * Walk through the environment, see if LOGIN and/or PASS are present.
 * If so, delete them. (Someone using "ps" might see them!)
 */
void init_env(void)
{
  extern char **environ;
  char **e;

  for (e = environ; *e; e++) {
    if (!strncmp(*e, "LOGIN=", 6)) {
      s_login = *e + 6;
      *e = "LOGIN=";
    }
    if (!strncmp(*e, "PASS=", 5)) {
      s_pass = *e + 5;
      *e = "PASS=";
    }
  }
}


/**
 * @brief alarm signal handler.
 *
 * @param dummy   No use.
 */
void myclock(int dummy)
{
  (void)dummy;
  signal(SIGALRM, myclock);
  alarm(1);

  if (--gtimeout == 0) {
    fprintf(stderr, "script \"%s\": global timeout\r\n",
            scriptfn);
    exit(1);
  }
  if (inexpect && etimeout && --etimeout == 0)
    siglongjmp(ejmp, 1);
}


/**
 * @brief check command line argument.
 *
 * @param argc  number of arguments.
 * @param argv  array of pointer to string
 */
void do_args(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "--version")) {
    printf(_("runluascript, part of minicom version %s\n"), VERSION);
    exit(0);
  }

  if (argc < 2) {
    fprintf(stderr, _("Usage: runluascript <scriptfile> [logfile [homedir]]%s\n"),"\r");
    exit(1);
  }
}

/**
 * @brief Entry function.
 *
 * @param argc  number of arguments.
 * @param argv  array of pointer to string
 *
 * @return return code..
 */
int main(int argc, char **argv)
{
  char *s;
  int rc;
  lua_State *L;

#if 0 /* Shouldn't need this.. */
  signal(SIGHUP, SIG_IGN);
#endif
  /* On some Linux systems SIGALRM is masked by default. Unmask it */
  sigrelse(SIGALRM);

  /* initialize locale support */
  setlocale(LC_ALL, "");
  bindtextdomain(PACKAGE, LOCALEDIR);
  textdomain(PACKAGE);

  init_env();

  do_args(argc, argv);

  inbuf_flush();

  if (argc > 2) {
    strncpy(logfname, argv[2], sizeof(logfname));
    logfname[sizeof(logfname) - 1] = '\0';
    if (argc > 3)
      strncpy(homedir, argv[3], sizeof(homedir));
    else if ((s = getenv("HOME")) != NULL)
      strncpy(homedir, s, sizeof(homedir));
    else
      homedir[0] = 0;
    homedir[sizeof(homedir) - 1] = '\0';
  }
  else
    logfname[0] = 0;

  scriptfn = argv[1];

  L = luaL_newstate();
  luaL_openlibs(L);

  /* entry minicom library */
  luaL_newlib(L, minicomlib);
  lua_setglobal(L, "minicom");
#if 0
  for (i=0; minicomlib[i].name!=NULL; ++i) {
    lua_pushcfunction(L, minicomlib[i].func);
    lua_setglobal( L, minicomlib[i].name );
  }
#endif

  luaL_loadfile(L, scriptfn);

  signal(SIGALRM, myclock);
  alarm(1);

  rc = lua_pcall(L, 0, 0, 0);

  if (rc!=0) {
    fprintf(stderr, "%s", lua_tostring(L, -1));
    lua_close(L);
    exit(EXIT_FAILURE);
  }

  return rc;
}

