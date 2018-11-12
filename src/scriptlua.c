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

static char inbuf[MAX_INBUF_SIZE + 1];/* Input buffer. */


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
static void readchar(void)
{
  char c;
  int n;

  while ((n = read(0, &c, 1)) != 1)
    if (errno != EINTR)
      break;

  if (n <= 0)
    return;

  /* Shift character into the buffer. */
#ifdef _SYSV
  memcpy(inbuf, inbuf + 1, MAX_INBUF_SIZE - 1);
#else
#  ifdef _BSD43
  bcopy(inbuf + 1, inbuf, MAX_INBUF_SIZE - 1);
#  else
  /* This is Posix, I believe. */
  memmove(inbuf, inbuf + 1, MAX_INBUF_SIZE - 1);
#  endif
#endif
  if (verbose)
    fputc(c, stderr);
  inbuf[MAX_INBUF_SIZE - 1] = c;
}

/**
 * @brief See if a string just came in.
 *
 * @param[in] word  checked string
 *
 * @return  true  found it in the buffer.
 * @return  false not found it in the buffer.
 */
static int expfound(const char *word)
{
  int len;

  if (word == NULL) {
    fprintf(stderr, _("NULL paramenter to %s!"), __func__);
    exit(1);
  }

  len = strlen(word);
  if (len > MAX_INBUF_SIZE)
    len = MAX_INBUF_SIZE;

  return !strcmp(inbuf + MAX_INBUF_SIZE - len, word);
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
  const char* seq[MAX_NUM_EXPECT + 1] = {0};
  volatile int found = 0;

  arg_cnt = lua_gettop(L);
  if (arg_cnt>MAX_NUM_EXPECT)
    luaL_error(L, _("number of argument of expect() is less than or equal 16."),
        MAX_NUM_EXPECT); /* ithis function is not returned. */

  for (idx=1; idx<=arg_cnt; ++idx) {
    seq[idx-1] = luaL_optstring(L, idx, NULL);
    if (seq[idx-1]==NULL)
      fprintf(stderr, 
          "expect: arg #%d may be nil. So subsequent arguments are invalid.\r",
          idx);
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
    readchar();
    for (idx = 0; seq[idx]; ++idx) {
      if (expfound(seq[idx])) {
        found = 1;
        break;
      }
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
  memset(inbuf, 0, sizeof(inbuf));

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
  memset(inbuf, 0, sizeof(inbuf));
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

  memset(inbuf, 0, sizeof(inbuf));

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

