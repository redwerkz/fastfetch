#include "fastfetch.h"
#include "detection/host/host.h"
#include "common/io.h"
#include "common/parsing.h"
#include "common/processing.h"
#include "common/thread.h"
#include "terminalshell.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __APPLE__
    #include <libproc.h>
#endif

static void setExeName(FFstrbuf* exe, const char** exeName)
{
    assert(exe->length > 0);
    uint32_t lastSlashIndex = ffStrbufLastIndexC(exe, '/');
    if(lastSlashIndex < exe->length)
        *exeName = exe->chars + lastSlashIndex + 1;
}

static void getProcessInformation(pid_t pid, FFstrbuf* processName, FFstrbuf* exe, const char** exeName)
{
    assert(processName->length > 0);
    ffStrbufClear(exe);

    #ifdef __linux__

    char cmdlineFilePath[64];
    snprintf(cmdlineFilePath, sizeof(cmdlineFilePath), "/proc/%d/cmdline", (int)pid);

    if(ffAppendFileBuffer(cmdlineFilePath, exe))
    {
        ffStrbufSubstrBeforeFirstC(exe, '\0'); //Trim the arguments
        ffStrbufTrimLeft(exe, '-'); //Happens in TTY
    }

    #elif defined(__APPLE__)

    int length = proc_pidpath((int)pid, exe->chars, exe->allocated);
    if(length > 0)
        exe->length = (uint32_t)length;

    #else

    //TODO: support bsd (https://www.freebsd.org/cgi/man.cgi?query=kinfo_getproc)

    #endif

    if(exe->length == 0)
        ffStrbufSet(exe, processName);

    setExeName(exe, exeName);
}

static const char* getProcessNameAndPpid(pid_t pid, char* name, pid_t* ppid)
{
    const char* error = NULL;

    #ifdef __linux__

    char statFilePath[64];
    snprintf(statFilePath, sizeof(statFilePath), "/proc/%d/stat", (int)pid);
    FILE* stat = fopen(statFilePath, "r");
    if(stat == NULL)
        return "fopen(statFilePath, \"r\") failed";

    *ppid = 0;
    if(
        fscanf(stat, "%*s (%255[^)]) %*c %d", name, ppid) != 2 || //stat (comm) state ppid
        !ffStrSet(name) ||
        *ppid == 0
    )
        error = "fscanf(stat) failed";

    fclose(stat);

    #elif defined(__APPLE__)

    struct proc_bsdshortinfo proc;
    if(proc_pidinfo(pid, PROC_PIDT_SHORTBSDINFO, 0, &proc, PROC_PIDT_SHORTBSDINFO_SIZE) <= 0)
        error = "proc_pidinfo(pid) failed";
    else
    {
        *ppid = (pid_t)proc.pbsi_ppid;
        strncpy(name, proc.pbsi_comm, 16); //trancated to 16 chars
    }

    #else

    //TODO: support bsd (https://www.freebsd.org/cgi/man.cgi?query=kinfo_getproc)
    error = "unimplemented";

    #endif

    return error;
}

static void getTerminalShell(FFTerminalShellResult* result, pid_t pid)
{
    char name[256];
    name[0] = '\0';

    pid_t ppid = 0;

    if(getProcessNameAndPpid(pid, name, &ppid))
        return;

    //Common programs that are between terminal and own process, but are not the shell
    if(
        strcasecmp(name, "sudo")          == 0 ||
        strcasecmp(name, "su")            == 0 ||
        strcasecmp(name, "doas")          == 0 ||
        strcasecmp(name, "strace")        == 0 ||
        strcasecmp(name, "sshd")          == 0 ||
        strcasecmp(name, "gdb")           == 0 ||
        strcasecmp(name, "lldb")          == 0 ||
        strcasecmp(name, "guake-wrapped") == 0 ||
        strcasestr(name, "debug")         != NULL
    ) {
        getTerminalShell(result, ppid);
        return;
    }

    //Known shells
    if(
        strcasecmp(name, "bash")      == 0 ||
        strcasecmp(name, "sh")        == 0 ||
        strcasecmp(name, "zsh")       == 0 ||
        strcasecmp(name, "ksh")       == 0 ||
        strcasecmp(name, "fish")      == 0 ||
        strcasecmp(name, "dash")      == 0 ||
        strcasecmp(name, "pwsh")      == 0 ||
        strcasecmp(name, "nu")        == 0 ||
        strcasecmp(name, "git-shell") == 0
    ) {
        if (result->shellProcessName.length == 0)
        {
            ffStrbufSetS(&result->shellProcessName, name);
            getProcessInformation(pid, &result->shellProcessName, &result->shellExe, &result->shellExeName);
        }

        getTerminalShell(result, ppid);
        return;
    }

    ffStrbufSetS(&result->terminalProcessName, name);
    getProcessInformation(pid, &result->terminalProcessName, &result->terminalExe, &result->terminalExeName);
}

static void getTerminalFromEnv(FFTerminalShellResult* result)
{
    if(
        result->terminalProcessName.length > 0 &&
        !ffStrbufStartsWithIgnCaseS(&result->terminalProcessName, "login") &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "(login)") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "systemd") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "init") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "(init)") != 0 &&
        ffStrbufIgnCaseCompS(&result->terminalProcessName, "0") != 0
    ) return;

    const char* term = NULL;

    //SSH
    if(getenv("SSH_CONNECTION") != NULL)
        term = getenv("SSH_TTY");

    //Windows Terminal
    if(!ffStrSet(term) && (
        getenv("WT_SESSION") != NULL ||
        getenv("WT_PROFILE_ID") != NULL
    )) term = "Windows Terminal";

    //Alacritty
    if(!ffStrSet(term) && (
        getenv("ALACRITTY_SOCKET") != NULL ||
        getenv("ALACRITTY_LOG") != NULL ||
        getenv("ALACRITTY_WINDOW_ID") != NULL
    )) term = "Alacritty";

    #ifdef __ANDROID__
    //Termux
    if(!ffStrSet(term) && (
        getenv("TERMUX_VERSION") != NULL ||
        getenv("TERMUX_MAIN_PACKAGE_FORMAT") != NULL ||
        getenv("TMUX_TMPDIR") != NULL
    )) term = "Termux";
    #endif

    #ifdef __linux__
    //Konsole
    if(!ffStrSet(term) && (
        getenv("KONSOLE_VERSION") != NULL
    )) term = "konsole";
    #endif

    //MacOS, mintty
    if(!ffStrSet(term))
        term = getenv("TERM_PROGRAM");

    #ifdef __linux__
    if(!ffStrSet(term))
    {
        //We are in WSL but not in Windows Terminal
        const FFHostResult* host = ffDetectHost();
        if(ffStrbufCompS(&host->productName, FF_HOST_PRODUCT_NAME_WSL) == 0 ||
            ffStrbufCompS(&host->productName, FF_HOST_PRODUCT_NAME_MSYS) == 0) //TODO better WSL or MSYS detection
        term = "conhost";
    }
    #endif

    //Normal Terminal
    if(!ffStrSet(term))
        term = getenv("TERM");

    //TTY
    if(!ffStrSet(term) || strcasecmp(term, "linux") == 0)
        term = ttyname(STDIN_FILENO);

    if(ffStrSet(term))
    {
        ffStrbufSetS(&result->terminalProcessName, term);
        ffStrbufSetS(&result->terminalExe, term);
        setExeName(&result->terminalExe, &result->terminalExeName);
    }
}

static void getUserShellFromEnv(FFTerminalShellResult* result)
{
    ffStrbufAppendS(&result->userShellExe, getenv("SHELL"));
    if(result->userShellExe.length == 0)
        return;
    setExeName(&result->userShellExe, &result->userShellExeName);

    //If shell detection via processes failed
    if(result->shellProcessName.length == 0 && result->userShellExe.length > 0)
    {
        ffStrbufAppendS(&result->shellProcessName, result->userShellExeName);
        ffStrbufSet(&result->shellExe, &result->userShellExe);
        setExeName(&result->shellExe, &result->shellExeName);
    }
}

static void getShellVersionGeneric(FFstrbuf* exe, const char* exeName, FFstrbuf* version)
{
    FFstrbuf command;
    ffStrbufInit(&command);
    ffStrbufAppendS(&command, "printf \"%s\" \"$");
    ffStrbufAppendTransformS(&command, exeName, toupper);
    ffStrbufAppendS(&command, "_VERSION\"");

    ffProcessAppendStdOut(version, (char* const[]) {
        "env",
        "-i",
        exe->chars,
        "-c",
        command.chars,
        NULL
    });
    ffStrbufSubstrBeforeFirstC(version, '(');
    ffStrbufRemoveStrings(version, 2, "-release", "release");

    ffStrbufDestroy(&command);
}

bool fftsGetShellVersion(FFstrbuf* exe, const char* exeName, FFstrbuf* version);

static void getShellVersion(FFstrbuf* exe, const char* exeName, FFstrbuf* version)
{
    ffStrbufClear(version);
    if(!fftsGetShellVersion(exe, exeName, version))
        getShellVersionGeneric(exe, exeName, version);
}

const FFTerminalShellResult* ffDetectTerminalShell(const FFinstance* instance)
{
    FF_UNUSED(instance);

    static FFThreadMutex mutex = FF_THREAD_MUTEX_INITIALIZER;
    static FFTerminalShellResult result;
    static bool init = false;
    ffThreadMutexLock(&mutex);
    if(init)
    {
        ffThreadMutexUnlock(&mutex);
        return &result;
    }
    init = true;

    ffStrbufInit(&result.shellProcessName);
    ffStrbufInitA(&result.shellExe, 128);
    result.shellExeName = result.shellExe.chars;
    ffStrbufInit(&result.shellVersion);

    ffStrbufInit(&result.terminalProcessName);
    ffStrbufInitA(&result.terminalExe, 128);
    result.terminalExeName = result.terminalExe.chars;

    ffStrbufInit(&result.userShellExe);
    result.userShellExeName = result.userShellExe.chars;
    ffStrbufInit(&result.userShellVersion);

    getTerminalShell(&result, getppid());

    getTerminalFromEnv(&result);
    getUserShellFromEnv(&result);
    getShellVersion(&result.shellExe, result.shellExeName, &result.shellVersion);

    if(strcasecmp(result.shellExeName, result.userShellExeName) != 0)
        getShellVersion(&result.userShellExe, result.userShellExeName, &result.userShellVersion);
    else
        ffStrbufSet(&result.userShellVersion, &result.shellVersion);

    if(ffStrbufEqualS(&result.shellProcessName, "pwsh"))
        ffStrbufInitS(&result.shellPrettyName, "PowerShell");
    else if(ffStrbufEqualS(&result.shellProcessName, "nu"))
        ffStrbufInitS(&result.shellPrettyName, "nushell");
    else
    {
        // https://github.com/LinusDierheimer/fastfetch/discussions/280#discussioncomment-3831734
        ffStrbufInitS(&result.shellPrettyName, result.shellExeName);
    }

    if(ffStrbufEqualS(&result.terminalProcessName, "iTerm.app"))
        ffStrbufInitS(&result.terminalPrettyName, "iTerm");
    else if(ffStrbufEqualS(&result.terminalProcessName, "Apple_Terminal"))
        ffStrbufInitS(&result.terminalPrettyName, "Apple Terminal");
    else if(strncmp(result.terminalExeName, result.terminalProcessName.chars, result.terminalProcessName.length) == 0) // if exeName starts with processName, print it. Otherwise print processName
        ffStrbufInitS(&result.terminalPrettyName, result.terminalExeName);
    else
        ffStrbufInitCopy(&result.terminalPrettyName, &result.terminalProcessName);

    ffThreadMutexUnlock(&mutex);
    return &result;
}
