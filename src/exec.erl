%%%------------------------------------------------------------------------
%%% File: $Id$
%%%------------------------------------------------------------------------
%%% @doc OS shell command starter.
%%%      It communicates with a separate C++ port process `exec-port'
%%%      spawned by this module, which is responsible
%%%      for starting, killing, listing, terminating, and notifying of
%%%      state changes.
%%%
%%%      The port program serves as a middle-man between
%%%      the OS and the virtual machine to carry out OS-specific low-level
%%%      process control.  The Erlang/C++ protocol is described in the
%%%      `exec.cpp' file.  On platforms/environments which permit
%%%      setting the suid bit on the `exec-port' executable, it can
%%%      run external tasks by impersonating a different user. When
%%%      suid bit is on, the application requires `exec:start_link/2'
%%%      to be given the `{user, User}' option so that `exec-port'
%%%      will not run as root.  Before changing the effective `User',
%%%      it sets the kernel capabilities so that it's able to start
%%%      processes as other users and adjust process priorities.
%%%
%%%      At exit the port program makes its best effort to perform
%%%      clean shutdown of all child OS processes.
%%%      Every started OS process is linked to a spawned light-weight
%%%      Erlang process returned by the run/2, run_link/2 command.
%%%      The application ensures that termination of spawned OsPid
%%%      leads to termination of the associated Erlang Pid, and vice
%%%      versa.
%%%
%%% @author Serge Aleynikov <saleyn@gmail.com>
%%% @version {@vsn}
%%%
%%% @type exec_options() = [Option]
%%%         Option = debug | {debug, Level::integer()} |
%%%                  verbose | {args, Args} | {alarm, Secs} |
%%%                  {user, User} | {limit_users, Users} |
%%%                  {portexe, Exe::string()} | {env, Env::list()}
%%%         Users  = [User]
%%%         User   = Acount::string().
%%%     Options passed to the exec process at startup.
%%%     <dl>
%%%     <dt>debug</dt><dd>Same as {debug, 1}</dd>
%%%     <dt>{debug, Level}</dt><dd>Enable port-programs debug trace at `Level'.</dd>
%%%     <dt>verbose</dt><dd>Enable verbose prints of the Erlang process.</dd>
%%%     <dt>{args, Args}</dt><dd>Append `Args' to the port command.</dd>
%%%     <dt>{alarm, Secs}</dt>
%%%         <dd>Give `Secs' deadline for the port program to clean up
%%%             child pids before exiting</dd>
%%%     <dt>{user, User}</dt>
%%%         <dd>When the port program was compiled with capability (Linux)
%%%             support enabled, and is owned by root with a a suid bit set,
%%%             this option must be specified so that upon startup the port
%%%             program is running under the effective user different from root.
%%%             This is a security measure that will also prevent the port program
%%%             to execute root commands.</dd>
%%%     <dt>{limit_users, LimitUsers}</dt>
%%%         <dd>Limit execution of external commands to these set of users.
%%%             This option is only valid when the port program is owned
%%%             by root.</dd>
%%%     <dt>{portexe, Exe}</dt>
%%%         <dd>Provide an alternative location of the port program.
%%%             This option is useful when this application is stored
%%%             on NFS and the port program needs to be copied locally
%%%             so that root suid bit can be set.</dd>
%%%     <dt>{env, Env}</dt>
%%%         <dd>Extend environment of the port program by using `Env' specification.
%%%             `Env' should be a list of tuples `{Name, Val}', where Name is the
%%%             name of an environment variable, and Val is the value it is to have
%%%             in the spawned port process.</dd>
%%%     </dl>.
%%% @type cmd_options() = [Option]
%%%         Option      = {cd, WorkDir::string()} | {env, Env} |
%%%                       {kill, Cmd::string()} |
%%%                       {kill_timeout, Sec::integer()} |
%%%                       {user, RunAsUser::string()} |
%%%                       {nice, Priority::integer()} |
%%%                       stdin | stdout | stderr |
%%%                       {stdout, Device} | {stderr, Device} |
%%%                       monitor
%%%         Env         = [VarEqVal]
%%%         VarEqVal    = string() | {Var::string(), Value::string()}
%%%         Device      = null | stdout | stderr | File | {append, File} | true
%%%         File        = string().
%%%     Command options:
%%%     <dl>
%%%     <dt>monitor</dt><dd>Set up a monitor for the spawned process</dd>
%%%     <dt>{cd, WorkDir}</dt><dd>Working directory</dd>
%%%     <dt>{env, Env}</dt>
%%%         <dd>List of "VAR=VALUE" environment variables or
%%%             list of {Var, Value} tuples. Both representations are
%%%             used in other parts of Erlang/OTP
%%%             (e.g. os:getenv/0, erlang:open_port/2)</dd>
%%%     <dt>{kill, Cmd}</dt>
%%%         <dd>This command will be used for killing the process. After
%%%             a 5-sec timeout if the process is still alive, it'll be
%%%             killed with SIGTERM followed by SIGKILL.  By default
%%%             SIGTERM/SIGKILL combination is used for process
%%%             termination.</dd>
%%%     <dt>{kill_timeout, Sec::integer()}</dt>
%%%         <dd>Number of seconds to wait after issueing a SIGTERM or
%%%             executing the custom `kill' command (if specified) before
%%%             killing the process with the `SIGKILL' signal</dd>
%%%     <dt>{group, GID}</dt>
%%%         <dd>Sets the effective group ID of the spawned process</dd>
%%%     <dt>{user, RunAsUser}</dt>
%%%         <dd>When exec-port was compiled with capability (Linux) support
%%%             enabled and has a suid bit set, it's capable of running
%%%             commands with a different RunAsUser effective user. Passing
%%%             "root" value of `RunAsUser' is prohibited.</dd>
%%%     <dt>{nice, Priority}</dt>
%%%         <dd>Set process priority between -20 and 20. Note that
%%%             negative values can be specified only when `exec-port'
%%%             is started with a root suid bit set.</dd>
%%%     <dt>stdin</dt>
%%%         <dd>Enable communication with an OS process via its `stdin'. The
%%%             input to the process is sent by `exec:send(OsPid, Data)'.</dd>
%%%     <dt>stdout</dt>
%%%         <dd>Same as `{stdout, self()}'.</dd>
%%%     <dt>stderr</dt>
%%%         <dd>Same as `{stderr, self()}'.</dd>
%%%     <dt>{stdout, output_device()}</dt>
%%%         <dd>Option for redirecting process's standard output stream</dd>
%%%     <dt>{stderr, output_device()}</dt>
%%%         <dd>Option for redirecting process's standard error stream</dd>
%%%     </dl>
%%% @type output_device() = null | close | stdout | stderr | print | pid() |
%%%         OutputFun | Filename | {append, Filename}
%%%         OutputFun = fun((stdout | stderr, integer(), binary()) -> none())
%%%         Filename  = string().
%%%     Output device option:
%%%     <dl>
%%%     <dt>null</dt><dd>Suppress output.</dd>
%%%     <dt>close</dt><dd>Close file descriptor for writing.</dd>
%%%     <dt>stdout</dt><dd>Redirect output to stdout.</dd>
%%%     <dt>stderr</dt><dd>Redirect output to stderr.</dd>
%%%     <dt>pid()</dt><dd>Redirect output to this pid.</dd>
%%%     <dt>fun((Stream, OsPid, Data) -> none())</dt>
%%%         <dd>Execute this callback on receiving output data</dd>
%%%     <dt>print</dt>
%%%         <dd>A debugging convenience device that prints the output to the
%%%             console shell</dd>
%%%     <dt>Filename</dt><dd>Save output to file by overwriting it.</dd>
%%%     <dt>{append, Filename}</dt><dd>Append output to file.</dd>
%%%     </dl>
%%% @end
%%%------------------------------------------------------------------------
%%% Created: 2003-06-10 by Serge Aleynikov <saleyn@gmail.com>
%%% $Header$
%%%------------------------------------------------------------------------
-module(exec).
-author('saleyn@gmail.com').

-behaviour(gen_server).

%% External exports
-export([
    start/1, start_link/1, run/2, run_link/2, manage/2, send/2,
    which_children/0, kill/2, stop/1, ospid/1, pid/1, status/1, signal/1
]).

%% Internal exports
-export([default/0, default/1]).

%% gen_server callbacks
-export([init/1, handle_call/3, handle_cast/2, handle_info/2,
         code_change/3, terminate/2]).

-include("exec.hrl").

-record(state, {
    port,
    last_trans  = 0,            % Last transaction number sent to port
    trans       = queue:new(),  % Queue of outstanding transactions sent to port
    limit_users = [],           % Restricted list of users allowed to run commands
    registry,                   % Pids to notify when an OsPid exits
    debug       = false
}).

-type exec_options() :: [exec_option()].
-type exec_option()  ::
      debug
    | {debug, integer()}
    | verbose
    | {args, [string(), ...]}
    | {alarm, non_neg_integer()}
    | {user, string()}
    | {limit_users, [string(), ...]}
    | {portexe, string()}
    | {env, [{string(), string()}, ...]}.

-type cmd_options() :: [cmd_option()].
-type cmd_option()  ::
      {cd, string()}
    | {env, [string() | {Name :: string(), Value :: string()}, ...]}
    | {kill, string()}
    | {kill, non_neg_integer()}
    | {user, string()}
    | {nice, integer()}
    | stdin  | {stdin,  null | close | string() | true}
    | stdout
    | {stdout, null | close | stdout | stderr | print |
               fun((stdout, integer(), binary()) -> none()) | pid() |
               string() | {append, string()}}
    | stderr
    | {stderr, null | close | stdout | stderr | print |
               fun((stderr, integer(), binary()) -> none()) | pid() |
               string() | {append, string()}}.

-type ospid() :: integer().
%% Representation of OS process ID.

%%-------------------------------------------------------------------------
%% @doc Supervised start an external program manager.
%% @end
%%-------------------------------------------------------------------------
-spec start_link(exec_options()) -> {ok, pid()} | {error, any()}.
start_link(Options) when is_list(Options) ->
    gen_server:start_link({local, ?MODULE}, ?MODULE, [Options], []).

%%-------------------------------------------------------------------------
%% @equiv start_link/1
%% @doc Start of an external program manager without supervision.
%% @end
%%-------------------------------------------------------------------------
-spec start(exec_options()) -> {ok, pid()} | {error, any()}.
start(Options) when is_list(Options) ->
    gen_server:start({local, ?MODULE}, ?MODULE, [Options], []).

%%-------------------------------------------------------------------------
%% @doc Run an external program. `OsPid' is the OS process identifier of
%%      the new process.
%% @end
%%-------------------------------------------------------------------------
-spec run(Exe::string(), Options::cmd_options()) ->
    {ok, pid(), ospid()} | {error, any()}.
run(Exe, Options) when is_list(Exe), is_list(Options) ->
    do_run({run, Exe, Options}, Options).

%%-------------------------------------------------------------------------
%% @equiv run/2
%% @doc Run an external program and link to the OsPid. If OsPid exits,
%%      the calling process will be killed or if it's trapping exits,
%%      it'll get {'EXIT', OsPid, Status} message.  If the calling process
%%      dies the OsPid will be killed.
%% @end
%%-------------------------------------------------------------------------
-spec run_link(string(), cmd_options()) -> {ok, pid(), integer()} | {error, any()}.
run_link(Exe, Options) when is_list(Exe), is_list(Options) ->
    do_run({run, Exe, Options}, [link | Options]).

-spec do_run(Cmd::any(), Options::cmd_options()) ->
    {ok, pid(), ospid()} | {error, any()}.
do_run(Cmd, Options) ->
    Mon = proplists:get_value(monitor, Options),
    Link= case proplists:get_value(link, Options) of
          true -> link;
          _    -> nolink
          end,
    Cmd2 = {port, {Cmd, Link}},
    case {Mon, gen_server:call(?MODULE, Cmd2, 30000)} of
    {true, {ok, Pid, _} = R} ->
        monitor(process, Pid),
        R;
    {_, R} ->
        R
    end.

%%-------------------------------------------------------------------------
%% @doc Manage an existing external process. `OsPid' is the OS process
%%      identifier of the external OS process.
%% @end
%%-------------------------------------------------------------------------
-spec manage(ospid(), Options::cmd_options()) ->
    {ok, pid(), ospid()} | {error, any()}.
manage(Pid, Options) ->
    do_run({manage, Pid, Options}, Options).

%%-------------------------------------------------------------------------
%% @doc Get a list of children managed by port program.
%% @end
%%-------------------------------------------------------------------------
-spec which_children() -> [ospid(), ...].
which_children() ->
    gen_server:call(?MODULE, {port, {list}}).

%%-------------------------------------------------------------------------
%% @doc Send a `Signal' to a child `Pid' or `OsPid'.
%% @end
%%-------------------------------------------------------------------------
-spec kill(pid() | ospid(), integer()) -> ok | {error, any()}.
kill(Pid, Signal) when is_pid(Pid); is_integer(Pid) ->
    gen_server:call(?MODULE, {port, {kill, Pid, Signal}}).

%%-------------------------------------------------------------------------
%% @doc Terminate a managed `Pid' or `OsPid' process. The OS process is
%%      terminated gracefully.  If it was given a `{kill, Cmd}' option at
%%      startup, that command is executed and a timer is started.  If
%%      the program doesn't exit, then the default termination is
%%      performed.  Default termination implies sending a `SIGTERM' command
%%      followed by `SIGKILL' in 5 seconds, if the program doesn't get
%%      killed.
%% @end
%%-------------------------------------------------------------------------
-spec stop(pid() | ospid()) -> ok | {error, any()}.
stop(Pid) when is_pid(Pid); is_integer(Pid) ->
    gen_server:call(?MODULE, {port, {stop, Pid}}, 30000).

%%-------------------------------------------------------------------------
%% @doc Get `OsPid' of the given Erlang `Pid'.  The `Pid' must be created
%%      previously by running the run/2 or run_link/2 commands.
%% @end
%%-------------------------------------------------------------------------
-spec ospid(pid()) -> ospid() | {error, Reason::any()}.
ospid(Pid) when is_pid(Pid) ->
    Ref = make_ref(),
    Pid ! {{self(), Ref}, ospid},
    receive
    {Ref, Reply} -> Reply;
    Other        -> Other
    after 5000   -> {error, timeout}
    end.

%%-------------------------------------------------------------------------
%% @doc Get `Pid' of the given `OsPid'.  The `OsPid' must be created
%%      previously by running the run/2 or run_link/2 commands.
%% @end
%%-------------------------------------------------------------------------
-spec pid(OsPid::ospid()) -> pid() | undefined | {error, timeout}.
pid(OsPid) when is_integer(OsPid) ->
    gen_server:call(?MODULE, {pid, OsPid}).

%%-------------------------------------------------------------------------
%% @doc Send `Data' to stdin of the OS process identified by `OsPid'.
%% @end
%%-------------------------------------------------------------------------
-spec send(OsPid :: ospid() | pid(), binary()) -> ok.
send(OsPid, Data) when (is_integer(OsPid) orelse is_pid(OsPid)) andalso is_binary(Data) ->
    gen_server:call(?MODULE, {port, {send, OsPid, Data}}).

%%-------------------------------------------------------------------------
%% @doc Decode the program's exit_status.  If the program exited by signal
%%      the function returns `{signal, Signal, Core}' where the `Signal'
%%      is the signal number or atom, and `Core' indicates if the core file
%%      was generated.
%% @end
%%-------------------------------------------------------------------------
-spec status(integer()) ->
        {status, ExitStatus :: integer()} |
        {signal, Singnal :: integer() | atom(), Core :: boolean()}.
status(Status) when is_integer(Status) ->
    TermSignal = Status band 16#7F,
    IfSignaled = ((TermSignal + 1) bsr 1) > 0,
    ExitStatus = (Status band 16#FF00) bsr 8,
    case IfSignaled of
    true ->
        CoreDump = (Status band 16#80) =:= 16#80,
        {signal, signal(TermSignal), CoreDump};
    false ->
        {status, ExitStatus}
    end.

%%-------------------------------------------------------------------------
%% @doc Convert a signal number to atom
%% @end
%%-------------------------------------------------------------------------
-spec signal(integer()) -> atom() | integer().
signal( 1) -> sighup;
signal( 2) -> sigint;
signal( 3) -> sigquit;
signal( 4) -> sigill;
signal( 5) -> sigtrap;
signal( 6) -> sigabrt;
signal( 7) -> sigbus;
signal( 8) -> sigfpe;
signal( 9) -> sigkill;
signal(11) -> sigsegv;
signal(13) -> sigpipe;
signal(14) -> sigalrm;
signal(15) -> sigterm;
signal(16) -> sigstkflt;
signal(17) -> sigchld;
signal(18) -> sigcont;
signal(19) -> sigstop;
signal(20) -> sigtstp;
signal(21) -> sigttin;
signal(22) -> sigttou;
signal(23) -> sigurg;
signal(24) -> sigxcpu;
signal(25) -> sigxfsz;
signal(26) -> sigvtalrm;
signal(27) -> sigprof;
signal(28) -> sigwinch;
signal(29) -> sigio;
signal(30) -> sigpwr;
signal(31) -> sigsys;
signal(34) -> sigrtmin;
signal(64) -> sigrtmax;
signal(XX) -> XX.

%%-------------------------------------------------------------------------
%% @private
%% @spec () -> Default::exec_options()
%% @doc Provide default value of a given option.
%% @end
%%-------------------------------------------------------------------------
default() -> 
    [{debug, 0},        % Debug mode of the port program. 
     {verbose, false},  % Verbose print of events on the Erlang side.
     {args, ""},        % Extra arguments that can be passed to port program
     {alarm, 12},
     {user, ""},        % Run port program as this user
     {limit_users, []}, % Restricted list of users allowed to run commands
     {portexe, default(portexe)}].

%% @private
default(portexe) -> 
    % Get architecture (e.g. i386-linux)
    Dir = filename:dirname(filename:dirname(code:which(?MODULE))),
    filename:join([Dir, "priv", erlang:system_info(system_architecture), "exec-port"]);
default(Option) ->
    proplists:get_value(Option, default()).

%%%----------------------------------------------------------------------
%%% Callback functions from gen_server
%%%----------------------------------------------------------------------

%%-----------------------------------------------------------------------
%% Func: init/1
%% Returns: {ok, State}          |
%%          {ok, State, Timeout} |
%%          ignore               |
%%          {stop, Reason}
%% @private
%%-----------------------------------------------------------------------
init([Options]) ->
    process_flag(trap_exit, true),
    Opts0 = proplists:normalize(Options,
                    [{expand, [{debug,   {debug, 1}},
                               {verbose, {verbose, true}}]}]),
    Opts1 = [T || T = {O,_} <- Opts0, 
                lists:member(O, [debug, verbose, args, alarm, user])],
    Opts  = proplists:normalize(Opts1, [{aliases, [{args, ''}]}]),
    Args  = lists:foldl(
        fun({Opt, I}, Acc) when is_list(I), I =/= ""   ->
                [" -"++atom_to_list(Opt)++" "++I | Acc];
           ({Opt, I}, Acc) when is_integer(I) ->
                [" -"++atom_to_list(Opt)++" "++integer_to_list(I) | Acc];
           (_, Acc) -> Acc
        end, [], Opts),
    Exe   = proplists:get_value(portexe,     Options, default(portexe)) ++ lists:flatten([" -n"|Args]),
    Users = proplists:get_value(limit_users, Options, default(limit_users)),
    Debug = proplists:get_value(verbose,     Options, default(verbose)),
    Env   = case proplists:get_value(env, Options) of
            undefined -> [];
            Other     -> [{env, Other}]
            end,
    try
        debug(Debug, "exec: port program: ~s\n env: ~p\n", [Exe, Env]),
        PortOpts = Env ++ [binary, exit_status, {packet, 2}, nouse_stdio, hide],
        Port = erlang:open_port({spawn, Exe}, PortOpts),
        Tab  = ets:new(exec_mon, [protected,named_table]),
        {ok, #state{port=Port, limit_users=Users, debug=Debug, registry=Tab}}
    catch _:Reason ->
        {stop, ?FMT("Error starting port '~s': ~200p", [Exe, Reason])}
    end.

%%----------------------------------------------------------------------
%% Func: handle_call/3
%% Returns: {reply, Reply, State}          |
%%          {reply, Reply, State, Timeout} |
%%          {noreply, State}               |
%%          {noreply, State, Timeout}      |
%%          {stop, Reason, Reply, State}   | (terminate/2 is called)
%%          {stop, Reason, State}            (terminate/2 is called)
%% @private
%%----------------------------------------------------------------------
handle_call({port, Instruction}, From, #state{last_trans=Last} = State) ->
    try is_port_command(Instruction, element(1, From), State) of
    {ok, Term} ->
        erlang:port_command(State#state.port, term_to_binary({0, Term})),
        {reply, ok, State};
    {ok, Term, Link, PidOpts} ->
        Next = next_trans(Last),
        erlang:port_command(State#state.port, term_to_binary({Next, Term})),
        {noreply, State#state{trans = queue:in({Next, From, Link, PidOpts}, State#state.trans)}}
    catch _:{error, Why} ->
        {reply, {error, Why}, State}
    end;

handle_call({pid, OsPid}, _From, State) ->
    case ets:lookup(exec_mon, OsPid) of
    [{_, Pid}] -> {reply, Pid, State};
    _          -> {reply, undefined, State}
    end;

handle_call(Request, _From, _State) ->
    {stop, {not_implemented, Request}}.

%%----------------------------------------------------------------------
%% Func: handle_cast/2
%% Returns: {noreply, State}          |
%%          {noreply, State, Timeout} |
%%          {stop, Reason, State}            (terminate/2 is called)
%% @private
%%----------------------------------------------------------------------
handle_cast(_Msg, State) ->
    {noreply, State}.

%%----------------------------------------------------------------------
%% Func: handle_info/2
%% Returns: {noreply, State}          |
%%          {noreply, State, Timeout} |
%%          {stop, Reason, State}            (terminate/2 is called)
%% @private
%%----------------------------------------------------------------------
handle_info({Port, {data, Bin}}, #state{port=Port, debug=Debug} = State) ->
    Msg = binary_to_term(Bin),
    debug(Debug, "~w got msg from port: ~p\n", [?MODULE, Msg]),
    case Msg of
    {N, Reply} when N =/= 0 ->
        case get_transaction(State#state.trans, N) of
        {true, {Pid,_} = From, MonType, PidOpts, Q} ->
            NewReply = maybe_add_monitor(Reply, Pid, MonType, PidOpts, Debug),
            gen_server:reply(From, NewReply);
        {false, Q} ->
            ok
        end,
        {noreply, State#state{trans=Q}};
    {0, {Stream, OsPid, Data}} when Stream =:= stdout; Stream =:= stderr ->
        send_to_ospid_owner(OsPid, {Stream, Data}),
        {noreply, State};
    {0, {exit_status, OsPid, Status}} ->
        debug(Debug, "Pid ~w exited with status: ~s{~w,~w}\n",
            [OsPid, if (((Status band 16#7F)+1) bsr 1) > 0 -> "signaled "; true -> "" end,
             (Status band 16#FF00 bsr 8), Status band 127]),
        notify_ospid_owner(OsPid, Status),
        {noreply, State};
    {0, Ignore} ->
        error_logger:warning_msg("~w [~w] unknown msg: ~p\n", [self(), ?MODULE, Ignore]),
        {noreply, State}
    end;

handle_info({Port, {exit_status, 0}}, #state{port=Port} = State) ->
    {stop, normal, State};
handle_info({Port, {exit_status, Status}}, #state{port=Port} = State) ->
    {stop, {exit_status, Status}, State};
handle_info({'EXIT', Port, Reason}, #state{port=Port} = State) ->
    {stop, Reason, State};
handle_info({'EXIT', Pid, Reason}, State) ->
    % OsPid's Pid owner died. Kill linked OsPid.
    do_unlink_ospid(Pid, Reason, State),
    {noreply, State};
handle_info(_Info, State) ->
    error_logger:info_msg("~w - unhandled message: ~p\n", [?MODULE, _Info]),
    {noreply, State}.

%%----------------------------------------------------------------------
%% Func: code_change/3
%% Purpose: Convert process state when code is changed
%% Returns: {ok, NewState}
%% @private
%%----------------------------------------------------------------------
code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

%%----------------------------------------------------------------------
%% Func: terminate/2
%% Purpose: Shutdown the server
%% Returns: any (ignored by gen_server)
%% @private
%%----------------------------------------------------------------------
terminate(_Reason, State) ->
    try
        erlang:port_command(State#state.port, term_to_binary({0, {shutdown}})),
        Status = wait_port_exit(State#state.port),
        error_logger:warning_msg("~w - exec process terminated (status: ~w)\n",
            [self(), Status])
    catch _:_ ->
        ok
    end.

wait_port_exit(Port) ->
    receive
    {Port,{exit_status,Status}} ->
        Status;
    _ ->
        wait_port_exit(Port)
    end.

%%%---------------------------------------------------------------------
%%% Internal functions
%%%---------------------------------------------------------------------

%% Add a link for Pid to OsPid if requested.
maybe_add_monitor({ok, OsPid}, Pid, MonType, PidOpts, Debug) when is_integer(OsPid) ->
    % This is a reply to a run/run_link command. The port program indicates
    % of creating a new OsPid process.
    % Spawn a light-weight process responsible for monitoring this OsPid
    Self = self(),
    LWP  = spawn_link(fun() -> ospid_init(Pid, OsPid, MonType, Self, PidOpts, Debug) end),
    ets:insert(exec_mon, [{OsPid, LWP}, {LWP, OsPid}]),
    {ok, LWP, OsPid};
maybe_add_monitor(Reply, _Pid, _MonType, _PidOpts, _Debug) ->
    Reply.

%%----------------------------------------------------------------------
%% @spec (Pid, OsPid::integer(), LinkType, Parent, PidOpts::list(), Debug::boolean()) ->
%%          void()
%% @doc Every OsPid is associated with an Erlang process started with
%%      this function. The `Parent' is the ?MODULE port manager that
%%      spawned this process and linked to it. `Pid' is the process
%%      that ran an OS command associated with OsPid. If that process
%%      requested a link (LinkType = 'link') we'll link to it.
%% @end
%% @private
%%----------------------------------------------------------------------
ospid_init(Pid, OsPid, LinkType, Parent, PidOpts, Debug) ->
    process_flag(trap_exit, true),
    StdOut = proplists:get_value(stdout, PidOpts),
    StdErr = proplists:get_value(stderr, PidOpts),
    case LinkType of
    link -> link(Pid); % The caller pid that requested to run the OsPid command & link to it. 
    _    -> ok
    end,
    ospid_loop({Pid, OsPid, Parent, StdOut, StdErr, Debug}).

ospid_loop({Pid, OsPid, Parent, StdOut, StdErr, Debug} = State) ->
    receive
    {{From, Ref}, ospid} ->
        From ! {Ref, OsPid},
        ospid_loop(State);
    {stdout, Data} when is_binary(Data) ->
        ospid_deliver_output(StdOut, {stdout, OsPid, Data}),
        ospid_loop(State);
    {stderr, Data} when is_binary(Data) ->
        ospid_deliver_output(StdErr, {stderr, OsPid, Data}),
        ospid_loop(State);
    {'DOWN', OsPid, {exit_status, Status}} ->
        debug(Debug, "~w ~w got down message (~w)\n", [self(), OsPid, status(Status)]),
        % OS process died
        case Status of
        0 -> exit(normal);
        _ -> exit({exit_status, Status})
        end;
    {'EXIT', Pid, Reason} ->
        % Pid died
        debug(Debug, "~w ~w got exit from linked ~w: ~p\n", [self(), OsPid, Pid, Reason]),
        exit({owner_died, Reason});
    {'EXIT', Parent, Reason} ->
        % Port program died
        debug(Debug, "~w ~w got exit from parent ~w: ~p\n", [self(), OsPid, Parent, Reason]),
        exit({port_closed, Reason});
    Other ->
        error_logger:warning_msg("~w - unknown msg: ~p\n", [self(), Other]),
        ospid_loop(State)
    end.

ospid_deliver_output(DestPid, Msg) when is_pid(DestPid) ->
    DestPid ! Msg;
ospid_deliver_output(DestFun, {Stream, OsPid, Data}) when is_function(DestFun) ->
    DestFun(Stream, OsPid, Data).

notify_ospid_owner(OsPid, Status) ->
    % See if there is a Pid owner of this OsPid. If so, sent the 'DOWN' message.
    case ets:lookup(exec_mon, OsPid) of
    [{_OsPid, Pid}] ->
        unlink(Pid),
        Pid ! {'DOWN', OsPid, {exit_status, Status}},
        ets:delete(exec_mon, {Pid, OsPid}),
        ets:delete(exec_mon, {OsPid, Pid});
    [] ->
        %error_logger:warning_msg("Owner ~w not found\n", [OsPid]),
        ok
    end.

send_to_ospid_owner(OsPid, Msg) ->
    case ets:lookup(exec_mon, OsPid) of
    [{_, Pid}] -> Pid ! Msg;
    _ -> ok
    end.

debug(false, _, _) ->
    ok;
debug(true, Fmt, Args) ->        
    io:format(Fmt, Args).

%%----------------------------------------------------------------------
%% @spec (Pid::pid(), Action, State::#state{}) -> 
%%          {ok, LastTok::integer(), LeftLinks::integer()}
%% @doc Pid died or requested to unlink - remove linked Pid records and 
%% optionally kill all OsPids linked to the Pid.
%% @end
%%----------------------------------------------------------------------
do_unlink_ospid(Pid, _Reason, State) ->
    case ets:lookup(exec_mon, Pid) of
    [{_Pid, OsPid}] when is_integer(OsPid) ->
        debug(State#state.debug, "Pid ~p died. Killing linked OsPid ~w\n", [Pid, OsPid]),
        ets:delete(exec_mon, {Pid, OsPid}),
        ets:delete(exec_mon, {OsPid, Pid}),
        erlang:port_command(State#state.port, term_to_binary({0, {stop, OsPid}}));
    _ ->
        ok 
    end.

get_transaction(Q, I) -> 
    get_transaction(Q, I, Q).
get_transaction(Q, I, OldQ) ->
    case queue:out(Q) of
    {{value, {I, From, LinkType, PidOpts}}, Q2} ->
        {true, From, LinkType, PidOpts, Q2};
    {empty, _} ->
        {false, OldQ};
    {_, Q2} ->
        get_transaction(Q2, I, OldQ)
    end.
    
is_port_command({{run, Cmd, Options}, Link}, Pid, State) ->
    {PortOpts, Other} = check_cmd_options(Options, Pid, State, [], []),
    {ok, {run, Cmd, PortOpts}, Link, Other};
is_port_command({list} = T, _Pid, _State) -> 
    {ok, T, undefined, []};
is_port_command({stop, OsPid}=T, _Pid, _State) when is_integer(OsPid) -> 
    {ok, T, undefined, []};
is_port_command({stop, Pid}, _Pid, _State) when is_pid(Pid) ->
    case ets:lookup(exec_mon, Pid) of
    [{_Pid, OsPid}] -> {ok, {stop, OsPid}, undefined, []};
    []              -> throw({error, no_process})
    end;
is_port_command({{manage, OsPid, Options}, Link}, Pid, State) when is_integer(OsPid) ->
    {PortOpts, _Other} = check_cmd_options(Options, Pid, State, [], []),
    {ok, {manage, OsPid, PortOpts}, Link, []};
is_port_command({send, Pid, Data}, _Pid, _State) when is_pid(Pid), is_binary(Data) ->
    case ets:lookup(exec_mon, Pid) of
    [{Pid, OsPid}]  -> {ok, {stdin, OsPid, Data}, undefined, []};
    []              -> throw({error, no_process})
    end;
is_port_command({send, OsPid, Data}, _Pid, _State) when is_integer(OsPid), is_binary(Data) ->
    {ok, {stdin, OsPid, Data}};
is_port_command({kill, OsPid, Sig}=T, _Pid, _State) when is_integer(OsPid),is_integer(Sig) -> 
    {ok, T, undefined, []};
is_port_command({kill, Pid, Sig}, _Pid, _State) when is_pid(Pid),is_integer(Sig) -> 
    case ets:lookup(exec_mon, Pid) of
    [{Pid, OsPid}]  -> {ok, {kill, OsPid, Sig}, undefined, []};
    []              -> throw({error, no_process})
    end.

check_cmd_options([monitor|T], Pid, State, PortOpts, OtherOpts) ->
    check_cmd_options(T, Pid, State, PortOpts, OtherOpts);
check_cmd_options([link|T], Pid, State, PortOpts, OtherOpts) ->
    check_cmd_options(T, Pid, State, PortOpts, OtherOpts);
check_cmd_options([{cd, Dir}=H|T], Pid, State, PortOpts, OtherOpts) when is_list(Dir) ->
    check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
check_cmd_options([{env, Env}=H|T], Pid, State, PortOpts, OtherOpts) when is_list(Env) ->
    case lists:filter(fun(S) when is_list(S) -> false;
                         ({S1,S2}) when is_list(S1), is_list(S2) -> false;
                         (_) -> true
                      end, Env) of
    [] -> check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
    L  -> throw({error, {invalid_env_value, L}})
    end;
check_cmd_options([{kill, Cmd}=H|T], Pid, State, PortOpts, OtherOpts) when is_list(Cmd) ->
    check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
check_cmd_options([{kill_timeout, I}=H|T], Pid, State, PortOpts, OtherOpts) when is_integer(I), I >= 0 ->
    check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
check_cmd_options([{nice, I}=H|T], Pid, State, PortOpts, OtherOpts) when is_integer(I), I >= -20, I =< 20 ->
    check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
check_cmd_options([H|T], Pid, State, PortOpts, OtherOpts) when H=:=stdin; H=:=stdout; H=:=stderr ->
    check_cmd_options(T, Pid, State, [H|PortOpts], [{H, Pid}|OtherOpts]);
check_cmd_options([{stdin, I}=H|T], Pid, State, PortOpts, OtherOpts)
        when I=:=null; I=:=close; is_list(I) ->
    check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
check_cmd_options([{Std, I}=H|T], Pid, State, PortOpts, OtherOpts)
        when Std=:=stderr, I=/=Std; Std=:=stdout, I=/=Std ->
    if
        I=:=null; I=:=close; I=:=stderr; I=:=stdout; is_list(I); 
        is_tuple(I), size(I)=:=2, element(1,I)=:=append, is_list(element(2,I)) ->
            check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
        I=:=print ->
            check_cmd_options(T, Pid, State, [Std | PortOpts], [{Std, fun print/3} | OtherOpts]);
        is_pid(I) ->
            check_cmd_options(T, Pid, State, [Std | PortOpts], [H|OtherOpts]);
        is_function(I) ->
            {arity, 3} =:= erlang:fun_info(I, arity)
                orelse throw({error, ?FMT("Invalid ~w option ~p: expected Fun/3", [Std, I])}),
            check_cmd_options(T, Pid, State, [Std | PortOpts], [H|OtherOpts]);
        true -> 
            throw({error, ?FMT("Invalid ~w option ~p", [Std, I])})
    end;
check_cmd_options([{group, I}=H|T], Pid, State, PortOpts, OtherOpts) when is_integer(I), I >= 0; is_list(I) ->
    check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
check_cmd_options([{user, U}=H|T], Pid, State, PortOpts, OtherOpts) when is_list(U), U =/= "" ->
    case lists:member(U, State#state.limit_users) of
    true  -> check_cmd_options(T, Pid, State, [H|PortOpts], OtherOpts);
    false -> throw({error, ?FMT("User ~s is not allowed to run commands!", [U])})
    end;
check_cmd_options([Other|_], _Pid, _State, _PortOpts, _OtherOpts) ->
    throw({error, {invalid_option, Other}});
check_cmd_options([], _Pid, _State, PortOpts, OtherOpts) ->
    {PortOpts, OtherOpts}.
    
next_trans(I) when I =< 134217727 ->
    I+1;
next_trans(_) ->
    1.

print(Stream, OsPid, Data) ->
    io:format("Got ~w from ~w: ~p\n", [Stream, OsPid, Data]).
