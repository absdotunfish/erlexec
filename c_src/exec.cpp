/*
    exec.cpp

    Author:   Serge Aleynikov
    Created:  2003/07/10

    Description:
    ============

    Erlang port program for spawning and controlling OS tasks.
    It listens for commands sent from Erlang and executes them until
    the pipe connecting it to Erlang VM is closed or the program
    receives SIGINT or SIGTERM. At that point it kills all processes
    it forked by issuing SIGTERM followed by SIGKILL in 6 seconds.

    Marshalling protocol:
        Erlang                                                  C++
          | ---- {TransId::integer(), Instruction::tuple()} ---> |
          | <----------- {TransId::integer(), Reply} ----------- |

    Instruction = {manage, OsPid::integer(), Options} |
                  {run,   Cmd::string(), Options}   |
                  {shell, Cmd::string(), Options}   |
                  {list}                            |
                  {stop, OsPid::integer()}          |
                  {kill, OsPid::integer(), Signal::integer()} |
                  {stdin, OsPid::integer(), Data::binary()}

    Options = [Option]
    Option  = {cd, Dir::string()} |
              {env, [string() | {string(), string()}]} |
              {kill, Cmd::string()} |
              {kill_timeout, Sec::integer()} |
              {group, integer() | string()} |
              {user, User::string()} |
              {nice, Priority::integer()} |
              stdin  | {stdin, null | close | File::string()} |
              stdout | {stdout, Device::string()} |
              stderr | {stderr, Device::string()} |

    Device  = close | null | stderr | stdout | File::string() | {append, File::string()}

    Reply = ok                      |       // For kill/stop commands
            {ok, OsPid}             |       // For run/shell command
            {ok, [OsPid]}           |       // For list command
            {error, Reason}         |
            {exit_status, OsPid, Status}    // OsPid terminated with Status

    Reason = atom() | string()
    OsPid  = integer()
    Status = integer()
*/

#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <signal.h>

#ifdef HAVE_CAP
#include <sys/prctl.h>
#include <sys/capability.h>
#endif

#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <setjmp.h>
#include <limits.h>
#include <grp.h>
#include <pwd.h>
#include <fcntl.h>
#include <map>
#include <list>
#include <deque>
#include <sstream>

#include <ei.h>
#include "ei++.h"

#if defined(__CYGWIN__) || defined(__WIN32)
#  define sigtimedwait(a, b, c) 0
#endif

using namespace ei;

//-------------------------------------------------------------------------
// Defines
//-------------------------------------------------------------------------

#define BUF_SIZE 2048

/* In the event we have tried to kill something, wait this many
 * seconds and then *really* kill it with SIGKILL if needs be.  */
#define KILL_TIMEOUT_SEC 5

//-------------------------------------------------------------------------
// Global variables
//-------------------------------------------------------------------------

extern char **environ; // process environment

ei::Serializer eis(/* packet header size */ 2);

sigjmp_buf  jbuf;
static int  alarm_max_time  = 12;
static int  debug           = 0;
static bool oktojump        = false;
static int  terminated      = 0;    // indicates that we got a SIGINT / SIGTERM event
static bool superuser       = false;
static bool pipe_valid      = true;
static int  max_fds;
static int  dev_null;

//-------------------------------------------------------------------------
// Types & variables
//-------------------------------------------------------------------------

class CmdInfo;

typedef unsigned char byte;
typedef int   exit_status_t;
typedef pid_t kill_cmd_pid_t;
typedef std::pair<pid_t, exit_status_t>     PidStatusT;
typedef std::pair<pid_t, CmdInfo>           PidInfoT;
typedef std::map <pid_t, CmdInfo>           MapChildrenT;
typedef std::pair<kill_cmd_pid_t, pid_t>    KillPidStatusT;
typedef std::map <kill_cmd_pid_t, pid_t>    MapKillPidT;
typedef std::map<std::string, std::string>  MapEnv;
typedef typename MapEnv::iterator           MapEnvIterator;

MapChildrenT children;              // Map containing all managed processes started by this port program.
MapKillPidT  transient_pids;        // Map of pids of custom kill commands.

#define SIGCHLD_MAX_SIZE 4096
std::list< PidStatusT > exited_children;  // deque of processed SIGCHLD events

const char* CS_DEV_NULL = "/dev/null";

enum RedirectType {
    REDIRECT_STDOUT = -1,   // Redirect to stdout
    REDIRECT_STDERR = -2,   // Redirect to stderr
    REDIRECT_NONE   = -3,   // No output redirection
    REDIRECT_CLOSE  = -4,   // Close output file descriptor
    REDIRECT_ERL    = -5,   // Redirect output back to Erlang
    REDIRECT_FILE   = -6,   // Redirect output to file
    REDIRECT_NULL   = -7    // Redirect input/output to /dev/null
};

std::string fd_type(int tp) {
    switch (tp) {
        case REDIRECT_STDOUT:   return "stdout";
        case REDIRECT_STDERR:   return "stderr";
        case REDIRECT_NONE:     return "none";
        case REDIRECT_CLOSE:    return "close";
        case REDIRECT_ERL:      return "erlang";
        case REDIRECT_FILE:     return "file";
        case REDIRECT_NULL:     return "null";
        default: {
            std::stringstream s;
            s << "fd:" << tp;
            return s.str();
        }
    }
    return std::string(); // Keep the compiler happy
}

class CmdOptions;
class CmdInfo;

//-------------------------------------------------------------------------
// Local Functions
//-------------------------------------------------------------------------

int   send_ok(int transId, pid_t pid = -1);
int   send_pid_status_term(const PidStatusT& stat);
int   send_error_str(int transId, bool asAtom, const char* fmt, ...);
int   send_pid_list(int transId, const MapChildrenT& children);
int   send_ospid_output(int pid, const char* type, const char* data, int len);

pid_t start_child(CmdOptions& op, std::string& err);
int   kill_child(pid_t pid, int sig, int transId, bool notify=true);
int   check_children(int& isTerminated, bool notify = true);
bool  process_pid_input(CmdInfo& ci);
void  process_pid_output(CmdInfo& ci, int maxsize = 4096);
void  stop_child(pid_t pid, int transId, const TimeVal& now);
int   stop_child(CmdInfo& ci, int transId, const TimeVal& now, bool notify = true);
void  erase_child(MapChildrenT::iterator& it);

int process_command();
int finalize();
int set_nonblock_flag(pid_t pid, int fd, bool value);
int erl_exec_kill(pid_t pid, int signal);
int open_file(const char* file, bool append, const char* stream,
              const char* cmd, ei::StringBuffer<128>& err);
int open_pipe(int fds[2], const char* stream, ei::StringBuffer<128>& err);

//-------------------------------------------------------------------------
// Types
//-------------------------------------------------------------------------

class CmdOptions {
    ei::StringBuffer<256>   m_tmp;
    std::stringstream       m_err;
    std::string             m_cmd;
    std::string             m_cd;
    std::string             m_kill_cmd;
    int                     m_kill_timeout;
    MapEnv                  m_env;
    const char**            m_cenv;
    long                    m_nice;     // niceness level
    size_t                  m_size;
    size_t                  m_count;
    int                     m_group;    // used in setgid()
    int                     m_user;     // run as
    std::string             m_std_stream[3];
    bool                    m_std_stream_append[3];
    int                     m_std_stream_fd[3];

    void init_streams() {
        m_std_stream[STDOUT_FILENO] = CS_DEV_NULL;
        m_std_stream[STDERR_FILENO] = CS_DEV_NULL;

        for (int i=STDIN_FILENO; i <= STDERR_FILENO; i++)
            m_std_stream_append[i] = false;

        m_std_stream_fd[STDIN_FILENO]  = REDIRECT_NULL;
        m_std_stream_fd[STDOUT_FILENO] = REDIRECT_NONE;
        m_std_stream_fd[STDERR_FILENO] = REDIRECT_NONE;
    }

public:

    CmdOptions()
        : m_tmp(0, 256)
        , m_kill_timeout(KILL_TIMEOUT_SEC)
        , m_cenv(NULL), m_nice(INT_MAX), m_size(0), m_count(0)
        , m_group(INT_MAX), m_user(INT_MAX)
    {
        init_streams();
    }
    CmdOptions(const char* cmd, const char* cd = NULL, const char** env = NULL,
               int user = INT_MAX, int nice = INT_MAX, int group = INT_MAX)
        : m_cmd(cmd), m_cd(cd ? cd : "")
        , m_kill_timeout(KILL_TIMEOUT_SEC)
        , m_cenv(NULL), m_nice(INT_MAX), m_size(0), m_count(0)
        , m_group(group), m_user(user)
    {
        init_streams();
    }
    ~CmdOptions() {
        if (m_cenv != environ) delete [] m_cenv;
        m_cenv = NULL;
    }

    const char*  strerror()             const { return m_err.str().c_str(); }
    const char*  cmd()                  const { return m_cmd.c_str(); }
    const char*  cd()                   const { return m_cd.c_str(); }
    char* const* env()                  const { return (char* const*)m_cenv; }
    const char*  kill_cmd()             const { return m_kill_cmd.c_str(); }
    int          kill_timeout()         const { return m_kill_timeout; }
    int          group()                const { return m_group; }
    int          user()                 const { return m_user; }
    int          nice()                 const { return m_nice; }
    const char*  stream_file(int i)     const { return m_std_stream[i].c_str(); }
    bool         stream_append(int i)   const { return m_std_stream_append[i]; }
    int          stream_fd(int i)       const { return m_std_stream_fd[i]; }
    int&         stream_fd(int i)             { return m_std_stream_fd[i]; }
    const char*  stream_fd_type(int i)  const { return fd_type(stream_fd(i)).c_str(); }

    void stream_file(int i, const std::string& file, bool append) {
        m_std_stream_fd[i]      = REDIRECT_FILE;
        m_std_stream_append[i]  = append;
        m_std_stream[i]         = file;
    }

    void stream_null(int i) {
        m_std_stream_fd[i]      = REDIRECT_NULL;
        m_std_stream_append[i]  = false;
        m_std_stream[i]         = CS_DEV_NULL;
    }

    void stream_redirect(int i, RedirectType type) {
        m_std_stream_fd[i]      = type;
        m_std_stream_append[i]  = false;
        m_std_stream[i].clear();
    }

    int ei_decode(ei::Serializer& ei, bool getCmd = false);
    int init_cenv();
};

/// Contains run-time info of a child OS process.
/// When a user provides a custom command to kill a process this
/// structure will contain its run-time information.
struct CmdInfo {
    std::string     cmd;            // Executed command
    pid_t           cmd_pid;        // Pid of the custom kill command
    std::string     kill_cmd;       // Kill command to use (if provided - otherwise use SIGTERM)
    kill_cmd_pid_t  kill_cmd_pid;   // Pid of the command that <pid> is supposed to kill
    ei::TimeVal     deadline;       // Time when the <cmd_pid> is supposed to be killed using SIGTERM.
    bool            sigterm;        // <true> if sigterm was issued.
    bool            sigkill;        // <true> if sigkill was issued.
    int             kill_timeout;   // Pid shutdown interval in msec before it's killed with SIGKILL
    bool            managed;        // <true> if this pid is started externally, but managed by erlexec
    int             stream_fd[3];   // Pipe fd getting   process's stdin/stdout/stderr
    int             stdin_wr_pos;   // Offset of the unwritten portion of the head item of stdin_queue 
    std::list<std::string> stdin_queue;

    CmdInfo() {
        new (this) CmdInfo("", "", 0);
    }
    CmdInfo(const CmdInfo& ci) {
        new (this) CmdInfo(ci.cmd.c_str(), ci.kill_cmd.c_str(), ci.cmd_pid, ci.managed,
                           ci.stream_fd[STDIN_FILENO], ci.stream_fd[STDOUT_FILENO],
                           ci.stream_fd[STDERR_FILENO]);
    }
    CmdInfo(const char* _cmd, const char* _kill_cmd, pid_t _cmd_pid, bool _managed = false,
            int _stdin_fd = REDIRECT_NULL, int _stdout_fd = REDIRECT_NONE, int _stderr_fd = REDIRECT_NONE,
            int _kill_timeout = KILL_TIMEOUT_SEC)
        : cmd(_cmd), cmd_pid(_cmd_pid), kill_cmd(_kill_cmd), kill_cmd_pid(-1)
        , sigterm(false), sigkill(false)
        , kill_timeout(_kill_timeout), managed(_managed)
        , stdin_wr_pos(0)
    {
        stream_fd[STDIN_FILENO]  = _stdin_fd;
        stream_fd[STDOUT_FILENO] = _stdout_fd;
        stream_fd[STDERR_FILENO] = _stderr_fd;
    }

    const char* stream_name(int i) const {
        switch (i) {
            case STDIN_FILENO:  return "stdin";
            case STDOUT_FILENO: return "stdout";
            case STDERR_FILENO: return "stderr";
            default:            return "<unknown>";
        }
    }

    void include_stream_fd(int i, int& maxfd, fd_set* readfds, fd_set* writefds) {
        bool ok;
        fd_set* fds;
       
        if (i == STDIN_FILENO) {
            ok = stream_fd[i] >= 0 && stdin_wr_pos > 0;
            if (debug > 2)
                fprintf(stderr, "Pid %d adding stdin available notification (fd=%d, pos=%d)\r\n",
                    cmd_pid, stream_fd[i], stdin_wr_pos);
            fds = writefds;
        } else {
            ok = stream_fd[i] >= 0;
            if (debug > 2)
                fprintf(stderr, "Pid %d adding stdout checking (fd=%d)\r\n", cmd_pid, stream_fd[i]);
            fds = readfds;
        }

        if (ok) {
            FD_SET(stream_fd[i], fds);
            if (stream_fd[i] > maxfd) maxfd = stream_fd[i];
        }
    }

    void process_stream_data(int i, fd_set* readfds, fd_set* writefds) {
        int     fd  = stream_fd[i];
        fd_set* fds = i == STDIN_FILENO ? writefds : readfds;

        if (fd < 0 || !FD_ISSET(fd, fds)) return;

        if (i == STDIN_FILENO)
            process_pid_input(*this);
        else
            process_pid_output(*this);
    }
};

//-------------------------------------------------------------------------
// Local Functions
//-------------------------------------------------------------------------

void gotsignal(int signal)
{
    if (signal == SIGTERM || signal == SIGINT || signal == SIGPIPE)
        terminated = 1;
    if (signal == SIGPIPE)
        pipe_valid = false;
    if (debug)
        fprintf(stderr, "Got signal: %d (oktojump=%d)\r\n", signal, oktojump);
    if (oktojump) siglongjmp(jbuf, 1);
}

void gotsigchild(int signal, siginfo_t* si, void* context)
{
    // If someone used kill() to send SIGCHLD ignore the event
    if (si->si_code == SI_USER || signal != SIGCHLD)
        return;

    pid_t pid = si->si_pid;

    int status;
    pid_t ret;

    while ((ret = waitpid(pid, &status, WNOHANG)) < 0 && errno == EINTR);

    if (debug)
        fprintf(stderr, "Process %d exited (status=%d, oktojump=%d)\r\n", si->si_pid, status, oktojump);

    if (ret < 0 && errno == ECHILD) {
        int status = ECHILD;
        if (erl_exec_kill(pid, 0) == 0) // process likely forked and is alive
            status = 0;
        if (status != 0)
            exited_children.push_back(std::make_pair(pid <= 0 ? ret : pid, status));
    } else if (pid <= 0)
        exited_children.push_back(std::make_pair(ret, status));
    else if (ret == pid)
        exited_children.push_back(std::make_pair(pid, status));

    if (oktojump) siglongjmp(jbuf, 1);
}

void check_pending()
{
    static const struct timespec timeout = {0, 0};

    sigset_t  set;
    siginfo_t info;
    int sig;
    sigemptyset(&set);
    if (sigpending(&set) == 0) {
        while ((sig = sigtimedwait(&set, &info, &timeout)) > 0 || errno == EINTR)
            switch (sig) {
                case SIGCHLD:   gotsigchild(sig, &info, NULL); break;
                case SIGPIPE:   pipe_valid = false; /* intentionally follow through */
                case SIGTERM:
                case SIGINT:
                case SIGHUP:    gotsignal(sig); break;
                default:        break;
            }
    }
}

void usage(char* progname) {
    fprintf(stderr,
        "Usage:\n"
        "   %s [-n] [-alarm N] [-debug [Level]] [-user User]\n"
        "Options:\n"
        "   -n              - Use marshaling file descriptors 3&4 instead of default 0&1.\n"
        "   -alarm N        - Allow up to <N> seconds to live after receiving SIGTERM/SIGINT (default %d)\n"
        "   -debug [Level]  - Turn on debug mode (default Level: 1)\n"
        "   -user User      - If started by root, run as User\n"
        "Description:\n"
        "   This is a port program intended to be started by an Erlang\n"
        "   virtual machine.  It can start/kill/list OS processes\n"
        "   as requested by the virtual machine.\n",
        progname, alarm_max_time);
    exit(1);
}

//-------------------------------------------------------------------------
// MAIN
//-------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    fd_set readfds, writefds;
    struct sigaction sact, sterm;
    int userid = 0;

    sterm.sa_handler = gotsignal;
    sigemptyset(&sterm.sa_mask);
    sigaddset(&sterm.sa_mask, SIGCHLD);
    sterm.sa_flags = 0;
    sigaction(SIGINT,  &sterm, NULL);
    sigaction(SIGTERM, &sterm, NULL);
    sigaction(SIGHUP,  &sterm, NULL);
    sigaction(SIGPIPE, &sterm, NULL);

    sact.sa_handler = NULL;
    sact.sa_sigaction = gotsigchild;
    sigemptyset(&sact.sa_mask);
    sact.sa_flags = SA_SIGINFO | SA_RESTART | SA_NOCLDSTOP | SA_NODEFER;
    sigaction(SIGCHLD, &sact, NULL);

    if (argc > 1) {
        int res;
        for(res = 1; res < argc; res++) {
            if (strcmp(argv[res], "-h") == 0 || strcmp(argv[res], "--help") == 0) {
                usage(argv[0]);
            } else if (strcmp(argv[res], "-debug") == 0) {
                debug = (res+1 < argc && argv[res+1][0] != '-') ? atoi(argv[++res]) : 1;
                if (debug > 3)
                    eis.debug(true);
            } else if (strcmp(argv[res], "-alarm") == 0 && res+1 < argc) {
                if (argv[res+1][0] != '-')
                    alarm_max_time = atoi(argv[++res]);
                else
                    usage(argv[0]);
            } else if (strcmp(argv[res], "-n") == 0) {
                eis.set_handles(3, 4);
            } else if (strcmp(argv[res], "-user") == 0 && res+1 < argc && argv[res+1][0] != '-') {
                char* run_as_user = argv[++res];
                struct passwd *pw = NULL;
                if ((pw = getpwnam(run_as_user)) == NULL) {
                    fprintf(stderr, "User %s not found!\r\n", run_as_user);
                    exit(3);
                }
                userid = pw->pw_uid;
            }
        }
    }

    // If we are root, switch to non-root user and set capabilities
    // to be able to adjust niceness and run commands as other users.
    if (getuid() == 0) {
        superuser = true;
        if (userid == 0) {
            fprintf(stderr, "When running as root, \"-user User\" option must be provided!\r\n");
            exit(4);
        }

        #ifdef HAVE_CAP
        if (prctl(PR_SET_KEEPCAPS, 1) < 0) {
            perror("Failed to call prctl to keep capabilities");
            exit(5);
        }
        #endif

        if (
            #ifdef HAVE_SETRESUID
            setresuid(-1, userid, geteuid()) // glibc, FreeBSD, OpenBSD, HP-UX
            #elif HAVE_SETREUID
            setreuid(-1, userid)             // MacOSX, NetBSD, AIX, IRIX, Solaris>=2.5, OSF/1, Cygwin
            #else
            #error setresuid(3) not supported!
            #endif
        < 0) {
            perror("Failed to set userid");
            exit(6);
        }

        struct passwd* pw;
        if (debug && (pw = getpwuid(geteuid())) != NULL)
            fprintf(stderr, "exec: running as: %s (euid=%d)\r\n", pw->pw_name, geteuid());

        if (geteuid() == 0) {
            fprintf(stderr, "Failed to set effective userid to a non-root user %s (uid=%d)\r\n",
                pw ? pw->pw_name : "", geteuid());
            exit(7);
        }

        #ifdef HAVE_CAP
        cap_t cur;
        if ((cur = cap_from_text("cap_setuid=eip cap_kill=eip cap_sys_nice=eip")) == 0) {
            perror("Failed to convert cap_setuid & cap_sys_nice from text");
            exit(8);
        }
        if (cap_set_proc(cur) < 0) {
            perror("Failed to set cap_setuid & cap_sys_nice");
            exit(9);
        }
        cap_free(cur);

        if (debug && (cur = cap_get_proc()) != NULL) {
            fprintf(stderr, "exec: current capabilities: %s\r\n",  cap_to_text(cur, NULL));
            cap_free(cur);
        }
        #else
        if (debug)
            fprintf(stderr, "capability feature is not implemented for this plaform!\r\n");
        //exit(10);
        #endif
    }

    #if !defined(NO_SYSCONF)
    max_fds = sysconf(_SC_OPEN_MAX);
    #else
    max_fds = OPEN_MAX;
    #endif
    if (max_fds < 1024) max_fds = 1024;

    dev_null = open(CS_DEV_NULL, O_RDWR);

    if (dev_null < 0) {
        fprintf(stderr, "cannot open %s: %s\r\n", CS_DEV_NULL, strerror(errno));
        exit(10);
    }

    while (!terminated) {

        sigsetjmp(jbuf, 1); oktojump = 0;

        FD_ZERO (&writefds);
        FD_ZERO (&readfds);

        FD_SET (eis.read_handle(), &readfds);

        int maxfd = eis.read_handle();

        while (!terminated && !exited_children.empty())
            check_children(terminated);

        // Set up all stdout/stderr input streams that we need to monitor and redirect to Erlang
        for(MapChildrenT::iterator it=children.begin(), end=children.end(); it != end; ++it)
            for (int i=STDIN_FILENO; i <= STDERR_FILENO; i++)
                it->second.include_stream_fd(i, maxfd, &readfds, &writefds);

        check_pending(); // Check for pending signals arrived while we were in the signal handler

        if (terminated) break;

        oktojump = 1;
        ei::TimeVal timeout(KILL_TIMEOUT_SEC, 0);

        if (debug > 2)
            fprintf(stderr, "Selecting maxfd=%d\r\n", maxfd);

        int cnt = select (maxfd+1, &readfds, &writefds, (fd_set *) 0, &timeout.timeval());
        int interrupted = (cnt < 0 && errno == EINTR);
        oktojump = 0;

        if (debug > 2)
            fprintf(stderr, "Select got %d events (maxfd=%d)\r\n", cnt, maxfd);

        if (interrupted || cnt == 0) {
            if (check_children(terminated) < 0)
                break;
        } else if (cnt < 0) {
            fprintf(stderr, "Error in select: %s\r\n", strerror(errno));
            terminated = 11;
            break;
        } else if ( FD_ISSET (eis.read_handle(), &readfds) ) {
            /* Read from input stream a command sent by Erlang */
            if (process_command() < 0)
                break;
        } else {
            // Check if any stdout/stderr streams have data
            for(MapChildrenT::iterator it=children.begin(), end=children.end(); it != end; ++it)
                for (int i=STDIN_FILENO; i <= STDERR_FILENO; i++)
                    it->second.process_stream_data(i, &readfds, &writefds);
        }
    }

    sigsetjmp(jbuf, 1); oktojump = 0;

    return finalize();

}

int process_command()
{
    int  err, arity;
    long transId;
    std::string command;

    // Note that if we were using non-blocking reads, we'd also need to check
    // for errno EWOULDBLOCK.
    if ((err = eis.read()) < 0) {
        terminated = 90-err;
        return -1;
    }

    /* Our marshalling spec is that we are expecting a tuple
     * TransId, {Cmd::atom(), Arg1, Arg2, ...}} */
    if (eis.decodeTupleSize() != 2 ||
        (eis.decodeInt(transId)) < 0 ||
        (arity = eis.decodeTupleSize()) < 1)
    {
        terminated = 12;
        return -1;
    }

    enum CmdTypeT        {  MANAGE,  RUN,  SHELL,  STOP,  KILL,  LIST,  SHUTDOWN,  STDIN  } cmd;
    const char* cmds[] = { "manage","run","shell","stop","kill","list","shutdown","stdin" };

    /* Determine the command */
    if ((int)(cmd = (CmdTypeT) eis.decodeAtomIndex(cmds, command)) < 0) {
        if (send_error_str(transId, false, "Unknown command: %s", command.c_str()) < 0) {
            terminated = 13;
            return -1;
        }
        return 0;
    }

    switch (cmd) {
        case SHUTDOWN: {
            terminated = 0;
            return -1;
        }
        case MANAGE: {
            // {manage, Cmd::string(), Options::list()}
            CmdOptions po;
            long pid;
            pid_t realpid;

            if (arity != 3 || (eis.decodeInt(pid)) < 0 || po.ei_decode(eis) < 0) {
                send_error_str(transId, true, "badarg");
                return 0;
            }
            realpid = pid;

            CmdInfo ci("managed pid", po.kill_cmd(), realpid, true);
            ci.kill_timeout = po.kill_timeout();
            children[realpid] = ci;

            send_ok(transId, pid);
            break;
        }
        case RUN:
        case SHELL: {
            // {shell, Cmd::string(), Options::list()}
            CmdOptions po;

            if (arity != 3 || po.ei_decode(eis, true) < 0) {
                send_error_str(transId, false, po.strerror());
                break;
            }

            pid_t pid;
            std::string err;
            if ((pid = start_child(po, err)) < 0)
                send_error_str(transId, false, "Couldn't start pid: %s", err.c_str());
            else {
                CmdInfo ci(po.cmd(), po.kill_cmd(), pid, false,
                           po.stream_fd(STDIN_FILENO),
                           po.stream_fd(STDOUT_FILENO),
                           po.stream_fd(STDERR_FILENO),
                           po.kill_timeout());
                children[pid] = ci;
                send_ok(transId, pid);
            }
            break;
        }
        case STOP: {
            // {stop, OsPid::integer()}
            long pid;
            if (arity != 2 || eis.decodeInt(pid) < 0) {
                send_error_str(transId, true, "badarg");
                break;
            }
            stop_child(pid, transId, TimeVal(TimeVal::NOW));
            break;
        }
        case KILL: {
            // {kill, OsPid::integer(), Signal::integer()}
            long pid, sig;
            if (arity != 3 || eis.decodeInt(pid) < 0 || (eis.decodeInt(sig)) < 0) {
                send_error_str(transId, true, "badarg");
                break;
            } if (superuser && children.find(pid) == children.end()) {
                send_error_str(transId, false, "Cannot kill a pid not managed by this application");
                break;
            }
            kill_child(pid, sig, transId);
            break;
        }
        case LIST: {
            // {list}
            if (arity != 1) {
                send_error_str(transId, true, "badarg");
                break;
            }
            send_pid_list(transId, children);
            break;
        }
        case STDIN: {
            long pid;
            std::string data;
            if (arity != 3 || eis.decodeInt(pid) < 0 || eis.decodeBinary(data) < 0) {
                send_error_str(transId, true, "badarg");
                break;
            }

            MapChildrenT::iterator it = children.find(pid);
            if (it == children.end()) {
                if (debug)
                    fprintf(stderr, "Stdin (%ld bytes) cannot be sent to non-existing pid %ld\r\n",
                        data.size(), pid);
                break;
            }
            it->second.stdin_queue.push_front(data);
            process_pid_input(it->second);
            break;
        }
    }
    return 0;
}

int finalize()
{
    if (debug) fprintf(stderr, "Setting alarm to %d seconds\r\n", alarm_max_time);
    alarm(alarm_max_time);  // Die in <alarm_max_time> seconds if not done

    int old_terminated = terminated;
    terminated = 0;

    erl_exec_kill(0, SIGTERM); // Kill all children in our process group

    TimeVal now(TimeVal::NOW);
    TimeVal deadline(now, 6, 0);

    while (children.size() > 0) {
        sigsetjmp(jbuf, 1);

        if (children.size() > 0 || !exited_children.empty()) {
            int term = 0;
            check_children(term, pipe_valid);
        }

        for(MapChildrenT::iterator it=children.begin(), end=children.end(); it != end; ++it)
            stop_child(it->second, 0, now, false);

        for(MapKillPidT::iterator it=transient_pids.begin(), end=transient_pids.end(); it != end; ++it) {
            erl_exec_kill(it->first, SIGKILL);
            transient_pids.erase(it);
        }

        if (children.size() == 0)
            break;

        TimeVal timeout(TimeVal::NOW);
        if (timeout < deadline) {
            timeout = deadline - timeout;

            oktojump = 1;
            select (0, (fd_set *)0, (fd_set *)0, (fd_set *) 0, &timeout);
            oktojump = 0;
        }
    }

    if (debug)
        fprintf(stderr, "Exiting (%d)\r\n", old_terminated);

    return old_terminated;
}

pid_t start_child(CmdOptions& op, std::string& error)
{
    enum { RD = 0, WR = 1 };

    int stream_fd[][2] = {
        { REDIRECT_NULL, REDIRECT_NONE },
        { REDIRECT_NONE, REDIRECT_NONE },
        { REDIRECT_NONE, REDIRECT_NONE }
    };

    ei::StringBuffer<128> err;

    const char* stream[] = { "stdin", "stdout", "stderr" };

    // Optionally setup stdin/stdout/stderr redirect
    for (int i=0; i < 3; i++) {
        int  crw        = i==0 ? RD : WR;
        int  cfd        = op.stream_fd(i);
        int* sfd        = stream_fd[i];
        const char* file= op.stream_file(i);
        bool append     = op.stream_append(i);

        // Optionally setup stdout redirect
        switch (cfd) {
            case REDIRECT_CLOSE:
                sfd[RD] = cfd;
                sfd[WR] = cfd;
                if (debug)
                    fprintf(stderr, "  Closing %s\r\n", stream[i]);
                break;
            case REDIRECT_STDOUT:
            case REDIRECT_STDERR:
                sfd[crw] = cfd;
                if (debug)
                    fprintf(stderr, "  Redirecting [%s -> %s]\r\n", stream[i], fd_type(cfd).c_str());
                break;
            case REDIRECT_ERL:
                if (open_pipe(sfd, stream[i], err) < 0) {
                    error = err.c_str();
                    return -1;
                }
                break;
            case REDIRECT_NULL:
                sfd[crw] = dev_null;
                if (debug)
                    fprintf(stderr, "  Redirecting [%s -> null]\r\n", stream[i]);
                break;
            case REDIRECT_FILE: {
                sfd[crw] = open_file(file, append, stream[i], op.cmd(), err);
                if (sfd[crw] < 0) {
                    error = err.c_str();
                    return -1;
                }
                break;
            }
        }
    }

    if (debug)
        fprintf(stderr, "Starting child: '%s'\r\n"
                        "  child  = (stdin=%s, stdout=%s, stderr=%s)\r\n"
                        "  parent = (stdin=%s, stdout=%s, stderr=%s)\r\n",
            op.cmd(),
            fd_type(stream_fd[STDIN_FILENO ][RD]).c_str(),
            fd_type(stream_fd[STDOUT_FILENO][WR]).c_str(),
            fd_type(stream_fd[STDERR_FILENO][WR]).c_str(),
            fd_type(stream_fd[STDIN_FILENO ][WR]).c_str(),
            fd_type(stream_fd[STDOUT_FILENO][RD]).c_str(),
            fd_type(stream_fd[STDERR_FILENO][RD]).c_str()
        );

    pid_t pid = fork();

    if (pid < 0) {
        error = strerror(errno);
        return pid;
    } else if (pid == 0) {
        // I am the child

        // Setup stdin/stdout/stderr redirect
        for (int fd=STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
            int  crw = fd==STDIN_FILENO ? RD : WR;
            int* sfd = stream_fd[fd];

            // Set up stdin/stdout/stderr redirect
            close(sfd[fd==STDIN_FILENO ? WR : RD]);         // Close parent end of child pipes

            if (sfd[crw] == REDIRECT_CLOSE)
                close(fd);
            else if (sfd[crw] == REDIRECT_STDOUT && fd == STDERR_FILENO) {
                dup2(STDOUT_FILENO, fd);
            } else if (sfd[crw] == REDIRECT_STDERR && fd == STDOUT_FILENO) {
                dup2(STDERR_FILENO, fd);
            } else if (sfd[crw] >= 0) {                     // Child end of the parent pipe
                dup2(sfd[crw], fd);
                // Don't close sfd[rw] here, since if the same fd is used for redirecting
                // stdout and stdin (e.g. /dev/null) if won't work correctly. Instead the loop
                // following this one will close all extra fds.

                //setlinebuf(stdout);                       // Set line buffering
            }
        }

        for(int i=STDERR_FILENO+1; i < max_fds; i++)
            close(i);

        #if !defined(__CYGWIN__) && !defined(__WIN32)
        if (op.user() != INT_MAX && setresuid(op.user(), op.user(), op.user()) < 0) {
            err.write("Cannot set effective user to %d", op.user());
            perror(err.c_str());
            return EXIT_FAILURE;
        }
        #endif

        if (op.group() != INT_MAX && setgid(op.group()) < 0) {
            err.write("Cannot set effective group to %d", op.group());
            perror(err.c_str());
            return EXIT_FAILURE;
        }

        const char* const argv[] = { getenv("SHELL"), "-c", op.cmd(), (char*)NULL };
        if (op.cd() != NULL && op.cd()[0] != '\0' && chdir(op.cd()) < 0) {
            err.write("Cannot chdir to '%s'", op.cd());
            perror(err.c_str());
            return EXIT_FAILURE;
        }

        // Setup process environment
        if (op.init_cenv() < 0) {
            perror(err.c_str());
            return EXIT_FAILURE;
        }

        // Execute the process
        if (execve((const char*)argv[0], (char* const*)argv, op.env()) < 0) {
            err.write("Cannot execute '%s'", op.cmd());
            perror(err.c_str());
            return EXIT_FAILURE;
        }
        // On success execve never returns
        return EXIT_FAILURE;
    }

    // I am the parent
    for (int i=0; i < 3; i++) {
        int  wr  = i==0 ? WR : RD;
        int& cfd = op.stream_fd(i);
        int* sfd = stream_fd[i];

        int fd = sfd[i==0 ? RD : WR];
        if (fd >= 0 && fd != dev_null) {
            if (debug)
                fprintf(stderr, "  Parent closing pid %d pipe %s end (fd=%d)\r\n",
                    pid, i==0 ? "reading" : "writing", fd);
            close(fd); // Close stdin/reading or stdout(err)/writing end of the child pipe
        }

        if (sfd[wr] >= 0 && sfd[wr] != dev_null) {
            cfd = sfd[wr];
            // Make sure the writing end is non-blocking
            set_nonblock_flag(pid, cfd, true);

            if (debug)
                fprintf(stderr, "  Setup %s end of pid %d %s redirection (fd=%d%s)\r\n",
                    i==0 ? "writing" : "reading", pid, stream[i], cfd,
                    (fcntl(cfd, F_GETFL, 0) & O_NONBLOCK) == O_NONBLOCK ? " [non-block]" : "");
        }
    }

    if (op.nice() != INT_MAX && setpriority(PRIO_PROCESS, pid, op.nice()) < 0) {
        err.write("Cannot set priority of pid %d to %d", pid, op.nice());
        error = err.c_str();
        if (debug)
            fprintf(stderr, "%s\r\n", error.c_str());
    }
    return pid;
}

int stop_child(CmdInfo& ci, int transId, const TimeVal& now, bool notify)
{
    bool use_kill = false;

    if (ci.sigkill)     // Kill signal already sent
        return 0;
    else if (ci.kill_cmd_pid > 0 || ci.sigterm) {
        // There was already an attempt to kill it.
        if (ci.sigterm && now.diff(ci.deadline) > 0) {
            // More than KILL_TIMEOUT_SEC secs elapsed since the last kill attempt
            erl_exec_kill(ci.cmd_pid, SIGKILL);
            if (ci.kill_cmd_pid > 0)
                erl_exec_kill(ci.kill_cmd_pid, SIGKILL);

            ci.sigkill = true;
        }
        if (notify) send_ok(transId);
        return 0;
    } else if (!ci.kill_cmd.empty()) {
        // This is the first attempt to kill this pid and kill command is provided.
        CmdOptions co(ci.kill_cmd.c_str());
        std::string err;
        ci.kill_cmd_pid = start_child(co, err);
        if (!err.empty() && debug)
            fprintf(stderr, "Error executing kill command '%s': %s\r\r",
                ci.kill_cmd.c_str(), err.c_str());

        if (ci.kill_cmd_pid > 0) {
            transient_pids[ci.kill_cmd_pid] = ci.cmd_pid;
            ci.deadline.set(now, ci.kill_timeout);
            if (notify) send_ok(transId);
            return 0;
        } else {
            if (notify) send_error_str(transId, false, "bad kill command - using SIGTERM");
            use_kill = true;
            notify = false;
        }
    } else {
        // This is the first attempt to kill this pid and no kill command is provided.
        use_kill = true;
    }

    if (use_kill) {
        // Use SIGTERM / SIGKILL to nuke the pid
        int n;
        if (!ci.sigterm && (n = kill_child(ci.cmd_pid, SIGTERM, transId, notify)) == 0) {
            if (debug)
                fprintf(stderr, "Sent SIGTERM to pid %d (timeout=%dms)\r\n", ci.cmd_pid, ci.kill_timeout);
            ci.deadline.set(now, ci.kill_timeout);
        } else if (!ci.sigkill && (n = kill_child(ci.cmd_pid, SIGKILL, 0, false)) == 0) {
            if (debug)
                fprintf(stderr, "Sent SIGKILL to pid %d\r\n", ci.cmd_pid);
            ci.deadline = now;
            ci.sigkill  = true;
        } else {
            n = 0; // FIXME
            // Failed to send SIGTERM & SIGKILL to the process - give up
            ci.sigkill = true;
            if (debug)
                fprintf(stderr, "Failed to kill process %d - leaving a zombie\r\n", ci.cmd_pid);
            MapChildrenT::iterator it = children.find(ci.cmd_pid);
            if (it != children.end())
                erase_child(it);
        }
        ci.sigterm = true;
        return n;
    }
    return 0;
}

void stop_child(pid_t pid, int transId, const TimeVal& now)
{
    int n = 0;

    MapChildrenT::iterator it = children.find(pid);
    if (it == children.end()) {
        send_error_str(transId, false, "pid not alive");
        return;
    } else if ((n = erl_exec_kill(pid, 0)) < 0) {
        send_error_str(transId, false, "pid not alive (err: %d)", n);
        return;
    }
    stop_child(it->second, transId, now);
}

int kill_child(pid_t pid, int signal, int transId, bool notify)
{
    // We can't use -pid here to kill the whole process group, because our process is
    // the group leader.
    int err = erl_exec_kill(pid, signal);
    switch (err) {
        case 0:
            if (notify) send_ok(transId);
            break;
        case EINVAL:
            if (notify) send_error_str(transId, false, "Invalid signal: %d", signal);
            break;
        case ESRCH:
            if (notify) send_error_str(transId, true, "esrch");
            break;
        case EPERM:
            if (notify) send_error_str(transId, true, "eperm");
            break;
        default:
            if (notify) send_error_str(transId, true, strerror(err));
            break;
    }
    return err;
}

bool process_pid_input(CmdInfo& ci)
{
    int& fd = ci.stream_fd[STDIN_FILENO];

    if (fd < 0) return true;

    while (!ci.stdin_queue.empty()) {
        std::string& s = ci.stdin_queue.back();

        const void* p = s.c_str() + ci.stdin_wr_pos;
        int n, len = s.size() - ci.stdin_wr_pos;

        while ((n = write(fd, p, len)) < 0 && errno == EINTR);

        if (debug) {
            if (n < 0)
                fprintf(stderr, "Error writing %d bytes to stdin (fd=%d) of pid %d: %s\r\n",
                    len, fd, ci.cmd_pid, strerror(errno));
            else
                fprintf(stderr, "Wrote %d/%d bytes to stdin (fd=%d) of pid %d\r\n",
                    n, len, fd, ci.cmd_pid);
        }

        if (n > 0 && n < len) {
            ci.stdin_wr_pos += n;
            return false;
        } else if (n < 0 && errno == EAGAIN) {
            break;
        } else if (n <= 0) {
            if (debug)
                fprintf(stderr, "Eof writing pid %d's stdin, closing fd=%d: %s\r\n",
                    ci.cmd_pid, fd, strerror(errno));
            ci.stdin_wr_pos = 0;
            close(fd);
            fd = REDIRECT_CLOSE;
            ci.stdin_queue.clear();
            return true;
        }

        ci.stdin_queue.pop_back();
        ci.stdin_wr_pos = 0;
    }

    return true;
}

void process_pid_output(CmdInfo& ci, int maxsize)
{
    char buf[4096];

    for (int i=STDOUT_FILENO; i <= STDERR_FILENO; i++) {
        int& fd = ci.stream_fd[i];

        if (fd >= 0) {
            for(int got = 0, n = sizeof(buf); got < maxsize && n == sizeof(buf); got += n) {
                while ((n = read(fd, buf, sizeof(buf))) < 0 && errno == EINTR);
                if (debug > 1)
                    fprintf(stderr, "Read %d bytes from pid %d's %s (fd=%d): %s\r\n",
                        n, ci.cmd_pid, ci.stream_name(i), fd, n > 0 ? "ok" : strerror(errno));
                if (n > 0) {
                    send_ospid_output(ci.cmd_pid, ci.stream_name(i), buf, n);
                    if (n < (int)sizeof(buf))
                        break;
                } else if (n < 0 && errno == EAGAIN)
                    break;
                else if (n <= 0) {
                    if (debug)
                        fprintf(stderr, "Eof reading pid %d's %s, closing fd=%d: %s\r\n",
                            ci.cmd_pid, ci.stream_name(i), fd, strerror(errno));
                    close(fd);
                    fd = REDIRECT_CLOSE;
                    break;
                }
            }
        }
    }
}

void erase_child(MapChildrenT::iterator& it)
{
    for (int i=STDIN_FILENO; i<=STDERR_FILENO; i++)
        if (it->second.stream_fd[i] >= 0) {
            if (debug)
                fprintf(stderr, "Closing pid %d's %s\r\n", it->first, it->second.stream_name(i));
            close(it->second.stream_fd[i]);
        }

    children.erase(it);
}

int check_children(int& isTerminated, bool notify)
{
    if (debug > 2)
        fprintf(stderr, "Checking %ld exited children\r\n", exited_children.size());

    for (MapChildrenT::iterator it=children.begin(), end=children.end(); it != end; ++it) {
        TimeVal now(TimeVal::NOW);

        int   status = ECHILD;
        pid_t pid = it->first;
        int n = erl_exec_kill(pid, 0);

        if (n == 0) { // process is alive
            /* If a deadline has been set, and we're over it, wack it. */
            if (!it->second.deadline.zero() && now.diff(it->second.deadline) > 0)
                stop_child(it->second, 0, now, false);

            while ((n = waitpid(pid, &status, WNOHANG)) < 0 && errno == EINTR);

            if (n > 0) {
                if (WIFEXITED(status) || WIFSIGNALED(status)) {
                    exited_children.push_back(std::make_pair(pid <= 0 ? n : pid, status));
                } else if (WIFSTOPPED(status)) {
                    if (debug)
                        fprintf(stderr, "Pid %d %swas stopped by delivery of a signal %d\r\n",
                            pid, it->second.managed ? "(managed) " : "", WSTOPSIG(status));
                } else if (WIFCONTINUED(status)) {
                    if (debug)
                        fprintf(stderr, "Pid %d %swas resumed by delivery of SIGCONT\r\n",
                            pid, it->second.managed ? "(managed) " : "");
                }
            }
        } else if (n < 0 && errno == ESRCH) {
            exited_children.push_back(std::make_pair(pid, -1));
        }
    }

    // For each process info in the <exited_children> queue deliver it to the Erlang VM
    // and remove it from the managed <children> map.
    while (!isTerminated && !exited_children.empty()) {
        PidStatusT& item = exited_children.front();

        MapChildrenT::iterator i = children.find(item.first);
        MapKillPidT::iterator j;
        if (i != children.end()) {
            process_pid_output(i->second, INT_MAX);
            // Override status code if termination was requested by Erlang
            PidStatusT ps(item.first, i->second.sigterm ? 0 : item.second);
            if (notify && send_pid_status_term(ps) < 0) {
                isTerminated = 1;
                return -1;
            }
            erase_child(i);
        } else if ((j = transient_pids.find(item.first)) != transient_pids.end()) {
            // the pid is one of the custom 'kill' commands started by us.
            transient_pids.erase(j);
        }

        exited_children.pop_front();
    }

    return 0;
}

int send_pid_list(int transId, const MapChildrenT& children)
{
    // Reply: {TransId, [OsPid::integer()]}
    eis.reset();
    eis.encodeTupleSize(2);
    eis.encode(transId);
    eis.encodeListSize(children.size());
    for(MapChildrenT::const_iterator it=children.begin(), end=children.end(); it != end; ++it)
        eis.encode(it->first);
    eis.encodeListEnd();
    return eis.write();
}

int send_error_str(int transId, bool asAtom, const char* fmt, ...)
{
    char str[MAXATOMLEN];
    va_list vargs;
    va_start (vargs, fmt);
    vsnprintf(str, sizeof(str), fmt, vargs);
    va_end   (vargs);

    eis.reset();
    eis.encodeTupleSize(2);
    eis.encode(transId);
    eis.encodeTupleSize(2);
    eis.encode(atom_t("error"));
    (asAtom) ? eis.encode(atom_t(str)) : eis.encode(str);
    return eis.write();
}

int send_ok(int transId, pid_t pid)
{
    eis.reset();
    eis.encodeTupleSize(2);
    eis.encode(transId);
    if (pid < 0)
        eis.encode(atom_t("ok"));
    else {
        eis.encodeTupleSize(2);
        eis.encode(atom_t("ok"));
        eis.encode(pid);
    }
    return eis.write();
}

int send_pid_status_term(const PidStatusT& stat)
{
    eis.reset();
    eis.encodeTupleSize(2);
    eis.encode(0);
    eis.encodeTupleSize(3);
    eis.encode(atom_t("exit_status"));
    eis.encode(stat.first);
    eis.encode(stat.second);
    return eis.write();
}

int send_ospid_output(int pid, const char* type, const char* data, int len)
{
    eis.reset();
    eis.encodeTupleSize(2);
    eis.encode(0);
    eis.encodeTupleSize(3);
    eis.encode(atom_t(type));
    eis.encode(pid);
    eis.encode(data, len);
    return eis.write();
}

int open_file(const char* file, bool append, const char* stream,
              const char* cmd, ei::StringBuffer<128>& err)
{
    int flags = O_RDWR | O_CREAT | (append ? O_APPEND : O_TRUNC);
    int mode  = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    int fd    = open(file, flags, mode);
    if (fd < 0) {
        err.write("Failed to redirect %s to file: %s", stream, strerror(errno));
        return -1;
    }
    if (debug)
        fprintf(stderr, "  Redirecting %s of cmd '%s' to file: '%s' (fd=%d)\r\n",
            stream, cmd, file, fd);

    return fd;
}

int open_pipe(int fds[2], const char* stream, ei::StringBuffer<128>& err)
{
    if (pipe(fds) < 0) {
        err.write("Failed to create a pipe for %s: %s", stream, strerror(errno));
        return -1;
    }
    if (fds[1] > max_fds) {
        close(fds[0]);
        close(fds[1]);
        err.write("Exceeded number of available file descriptors (fd=%d)", fds[1]);
        return -1;
    }
    if (debug)
        fprintf(stderr, "  Redirecting [%s -> pipe(rd=%d, wr=%d)]\r\n", stream, fds[0], fds[1]);

    return 0;
}

int CmdOptions::ei_decode(ei::Serializer& ei, bool getCmd)
{
    // {Cmd::string(), [Option]}
    //      Option = {env, Strings} | {cd, Dir} | {kill, Cmd}
    int sz;
    std::string op, val;

    m_err.str("");
    m_cmd.clear();
    m_kill_cmd.clear();
    m_env.clear();

    m_nice = INT_MAX;

    if (getCmd && eis.decodeString(m_cmd) < 0) {
        m_err << "badarg: cmd string expected or string size too large";
        return -1;
    } else if ((sz = eis.decodeListSize()) < 0) {
        m_err << "option list expected";
        return -1;
    }

    // Note: The STDIN, STDOUT, STDERR enums must occupy positions 0, 1, 2!!!
    enum OptionT       { STDIN,  STDOUT,  STDERR,  CD,  ENV,  KILL,  KILL_TIMEOUT,  NICE,  USER,  GROUP} opt;
    const char* opts[]={"stdin","stdout","stderr","cd","env","kill","kill_timeout","nice","user","group"};

    bool seen_opt[STDERR+1] = {false};

    for(int i=0; i < sz; i++) {
        int arity, type = eis.decodeType(arity);

        if (type == ERL_ATOM_EXT && (int)(opt = (OptionT)eis.decodeAtomIndex(opts, op)) >= 0)
            arity = 1;
        else if (type != ERL_SMALL_TUPLE_EXT ||
                   eis.decodeTupleSize() != 2  ||
                   (int)(opt = (OptionT)eis.decodeAtomIndex(opts, op)) < 0) {
            m_err << "badarg: cmd option must be {Cmd, Opt} or atom";
            return -1;
        }

        if (seen_opt[opt]) {
            m_err << "duplicate " << op << " option specified";
            return -1;
        }
        seen_opt[opt] = true;

        switch (opt) {
            case CD:
                // {cd, Dir::string()}
                if (eis.decodeString(val) < 0) { m_err << op << " bad option value"; return -1; }
                m_cd = val;
                break;

            case KILL:
                // {kill, Cmd::string()}
                if (eis.decodeString(val) < 0) { m_err << op << " bad option value"; return -1; }
                m_kill_cmd = val;
                break;

            case GROUP: {
                // {group, integer() | string()}
                type = eis.decodeType(arity);
                if (type == etString) {
                    if (eis.decodeString(val) < 0) { m_err << op << " bad group value"; return -1; }
                    struct group g;
                    char buf[1024];
                    struct group* res;
                    if (getgrnam_r(val.c_str(), &g, buf, sizeof(buf), &res) < 0) {
                        m_err << op << " invalid group name: " << val;
                        return -1;
                    }
                    m_group = g.gr_gid;
                } else if (eis.decodeInt(m_group) < 0) {
                    m_err << op << " bad group value type (expected int or string)";
                    return -1;
                }
                break;
            }
            case USER:
                // {user, Dir::string()} | {kill, Cmd::string()}
                if (eis.decodeString(val) < 0) {
                    m_err << op << " bad option value"; return -1;
                }
                if      (opt == CD)     m_cd        = val;
                else if (opt == KILL)   m_kill_cmd  = val;
                else if (opt == USER) {
                    struct passwd *pw = getpwnam(val.c_str());
                    if (pw == NULL) {
                        m_err << "Invalid user " << val << ": " << ::strerror(errno);
                        return -1;
                    }
                    m_user = pw->pw_uid;
                }
                break;

            case KILL_TIMEOUT:
                if (eis.decodeInt(m_kill_timeout) < 0) {
                    m_err << "invalid value of kill_timeout";
                    return -1;
                }
                break;

            case NICE:
                if (eis.decodeInt(m_nice) < 0 || m_nice < -20 || m_nice > 20) {
                    m_err << "nice option must be an integer between -20 and 20";
                    return -1;
                }
                break;

            case ENV: {
                // {env, [NameEqualsValue::string()]}
                // passed in env variables are appended to the existing ones
                // obtained from environ global var
                int opt_env_sz = eis.decodeListSize();
                if (opt_env_sz < 0) {
                    m_err << "env list expected";
                    return -1;
                }

                for (int i=0; i < opt_env_sz; i++) {
                    int sz, type = eis.decodeType(sz);
                    bool res = false;
                    std::string s, key;

                    if (type == ERL_STRING_EXT) {
                        res = !eis.decodeString(s);
                        if (res) {
                            size_t pos = s.find_first_of('=');
                            if (pos == std::string::npos)
                                res = false;
                            else
                                key = s.substr(0, pos);
                        }
                    } else if (type == ERL_SMALL_TUPLE_EXT && sz == 2) {
                        eis.decodeTupleSize();
                        std::string s2;
                        if (eis.decodeString(key) == 0 && eis.decodeString(s2) == 0) {
                            res = true;
                            s = key + "=" + s2;
                        }
                    }

                    if (!res) {
                        m_err << "invalid env argument #" << i;
                        return -1;
                    }
                    m_env[key] = s;
                }
                break;
            }

            case STDIN:
            case STDOUT:
            case STDERR: {
                int& fdr = stream_fd(opt);

                if (arity == 1)
                    stream_redirect(opt, REDIRECT_ERL);
                else {
                    int type = 0, sz;
                    std::string s, fop;
                    type = eis.decodeType(sz);

                    if (type == ERL_ATOM_EXT)
                        eis.decodeAtom(s);
                    else if (type == ERL_STRING_EXT)
                        eis.decodeString(s);
                    else if (! (type == ERL_SMALL_TUPLE_EXT && sz == 2 &&
                        eis.decodeTupleSize() == 2 &&
                        eis.decodeAtom(fop) == 0 &&
                        eis.decodeString(s) == 0 && fop == "append"))
                    {
                        m_err << "atom, string or {append, Name} tuple required for option " << op;
                        return -1;
                    }

                    if (s == "null") {
                        stream_null(opt);
                        fdr = REDIRECT_NULL;
                    } else if (s == "close") {
                        stream_redirect(opt, REDIRECT_CLOSE);
                    } else if (s == "stderr" && opt == STDOUT)
                        stream_redirect(opt, REDIRECT_STDERR);
                    else if (s == "stdout" && opt == STDERR)
                        stream_redirect(opt, REDIRECT_STDOUT);
                    else if (!s.empty()) {
                        stream_file(opt, s, fop == "append");
                    }
                }

                if (opt == STDIN &&
                    !(fdr == REDIRECT_NONE  || fdr == REDIRECT_ERL ||
                      fdr == REDIRECT_CLOSE || fdr == REDIRECT_NULL || fdr == REDIRECT_FILE)) {
                    m_err << "invalid " << op << " redirection option: '" << op << "'";
                    return -1;
                }
                break;
            }
            default:
                m_err << "bad option: " << op; return -1;
        }
    }

    for (int i=STDOUT_FILENO; i <= STDERR_FILENO; i++)
        if (stream_fd(i) == (i == STDOUT_FILENO ? REDIRECT_STDOUT : REDIRECT_STDERR)) {
            m_err << "self-reference of " << stream_fd_type(i);
            return -1;
        }

    if (stream_fd(STDOUT_FILENO) == REDIRECT_STDERR &&
        stream_fd(STDERR_FILENO) == REDIRECT_STDOUT)
    {
        m_err << "circular reference of stdout and stderr";
        return -1;
    }

    if (debug > 1)
        fprintf(stderr, "Parsed cmd '%s' options\r\n  (stdin=%s, stdout=%s, stderr=%s)\r\n",
            m_cmd.c_str(), stream_fd_type(0), stream_fd_type(1), stream_fd_type(2));

    return 0;
}

/* This exists just to make sure that we don't inadvertently do a
 * kill(-1, SIGKILL), which will cause all kinds of bad things to
 * happen. */

int erl_exec_kill(pid_t pid, int signal) {
    if (pid < 0) {
        if (debug)
            fprintf(stderr, "kill(-1, %d) attempt prohibited!\r\n", signal);

        return -1;
    }

    if (debug && signal > 0)
        fprintf(stderr, "Calling kill(pid=%d, sig=%d)\r\n", pid, signal);

    return kill(pid, signal);
}

int set_nonblock_flag(pid_t pid, int fd, bool value)
{
    int oldflags = fcntl(fd, F_GETFL, 0);
    if (oldflags < 0)
        return oldflags;
    if (value != 0)
        oldflags |= O_NONBLOCK;
    else
        oldflags &= ~O_NONBLOCK;

    int ret = fcntl(fd, F_SETFL, oldflags);
    if (debug > 3) {
        oldflags = fcntl(fd, F_GETFL, 0);
        fprintf(stderr, "  Set pid %d's fd=%d to non-blocking mode (flags=%x)\r\n",
            pid, fd, oldflags);
    }

    return ret;
}

int CmdOptions::init_cenv()
{
    if (m_env.empty()) {
        m_cenv = (const char**)environ;
        return 0;
    }

    // Copy environment of the caller process
    for (char **env_ptr = environ; *env_ptr; env_ptr++) {
        std::string s(*env_ptr), key(s.substr(0, s.find_first_of('=')));
        MapEnvIterator it = m_env.find(key);
        if (it == m_env.end())
            m_env[key] = s;
    }

    if ((m_cenv = (const char**) new char* [m_env.size()+1]) == NULL) {
        m_err << "Cannot allocate memory for " << m_env.size()+1 << " environment entries";
        return -1;
    }

    int i = 0;
    for (MapEnvIterator it = m_env.begin(), end = m_env.end(); it != end; ++it, ++i)
        m_cenv[i] = it->second.c_str();
    m_cenv[i] = NULL;

    return 0;
}
