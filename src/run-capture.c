/* Native Windows child-process runner. MIT; see licenses/Launcher-MIT.txt.
 * Retains the real process handle and calls GetExitCodeProcess. Captures raw
 * stdout/stderr to files. No registry or permanent settings changes.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdint.h>

static void close_valid(HANDLE h){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);}
static wchar_t *read_arguments(const wchar_t *path){
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE)return NULL;
    LARGE_INTEGER size;
    if(!GetFileSizeEx(file,&size)||size.QuadPart<2||size.QuadPart>65534||(size.QuadPart&1)){CloseHandle(file);return NULL;}
    DWORD bytes=(DWORD)size.QuadPart,got=0;wchar_t *text=calloc((size_t)bytes+4,1);
    if(!text){CloseHandle(file);return NULL;}
    BOOL ok=ReadFile(file,text,bytes,&got,NULL);CloseHandle(file);
    if(!ok||got!=bytes||text[0]!=0xfeff){free(text);return NULL;}
    memmove(text,text+1,bytes-2);text[(bytes-2)/2]=0;return text;
}
int wmain(int argc,wchar_t **argv){
    const wchar_t *exe=NULL,*cwd=NULL,*out=NULL,*err=NULL,*args=L"",*args_file=NULL;
    DWORD seconds=0;
    for(int i=1;i<argc;i++){
        if(i+1>=argc){fprintf(stderr,"FAIL: missing option value\n");return 2;}
        if(!wcscmp(argv[i],L"--exe"))exe=argv[++i];
        else if(!wcscmp(argv[i],L"--cwd"))cwd=argv[++i];
        else if(!wcscmp(argv[i],L"--out"))out=argv[++i];
        else if(!wcscmp(argv[i],L"--err"))err=argv[++i];
        else if(!wcscmp(argv[i],L"--args"))args=argv[++i];
        else if(!wcscmp(argv[i],L"--args-file"))args_file=argv[++i];
        else if(!wcscmp(argv[i],L"--timeout"))seconds=wcstoul(argv[++i],NULL,10);
        else{fprintf(stderr,"FAIL: unknown runner option\n");return 2;}
    }
    if(!exe||!cwd||!out||!err){fprintf(stderr,"Usage: RunCapture.exe --exe path --cwd directory --out file --err file [--args-file UTF16LE-file] [--timeout seconds]\n");return 2;}
    wchar_t *owned_args=NULL;
    if(args_file){owned_args=read_arguments(args_file);if(!owned_args){fprintf(stderr,"FAIL: cannot read UTF-16LE argument file\n");return 2;}args=owned_args;}
    if(wcschr(exe,L'"')){fprintf(stderr,"FAIL: invalid executable path\n");free(owned_args);return 2;}
    size_t chars=wcslen(exe)+wcslen(args)+5;
    if(chars>32767){fprintf(stderr,"FAIL: command line too long\n");free(owned_args);return 2;}
    wchar_t *command=calloc(chars,sizeof(*command));
    if(!command){fprintf(stderr,"FAIL: runner allocation\n");free(owned_args);return 2;}
    swprintf(command,chars,L"\"%ls\" %ls",exe,args);
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE hout=CreateFileW(out,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    HANDLE herr=CreateFileW(err,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    HANDLE hin=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(hout==INVALID_HANDLE_VALUE||herr==INVALID_HANDLE_VALUE||hin==INVALID_HANDLE_VALUE){
        DWORD error=GetLastError();fprintf(stderr,"FAIL: log/input handle Windows error=%lu\n",error);
        close_valid(hout);close_valid(herr);close_valid(hin);free(command);free(owned_args);return 3;
    }
    STARTUPINFOEXW start;ZeroMemory(&start,sizeof(start));start.StartupInfo.cb=sizeof(start);
    start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    start.StartupInfo.hStdOutput=hout;start.StartupInfo.hStdError=herr;start.StartupInfo.hStdInput=hin;
    SIZE_T bytes=0;InitializeProcThreadAttributeList(NULL,1,0,&bytes);
    start.lpAttributeList=HeapAlloc(GetProcessHeap(),0,bytes);
    BOOL list_ready=FALSE;
    if(start.lpAttributeList)list_ready=InitializeProcThreadAttributeList(start.lpAttributeList,1,0,&bytes);
    HANDLE allowed[]={hout,herr,hin};
    if(!list_ready || !UpdateProcThreadAttribute(start.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowed,sizeof(allowed),NULL,NULL)){
        fprintf(stderr,"FAIL: restricted handle inheritance Windows error=%lu\n",GetLastError());
        if(list_ready)DeleteProcThreadAttributeList(start.lpAttributeList);
        if(start.lpAttributeList)HeapFree(GetProcessHeap(),0,start.lpAttributeList);
        close_valid(hout);close_valid(herr);close_valid(hin);free(command);free(owned_args);return 3;
    }
    PROCESS_INFORMATION process;ZeroMemory(&process,sizeof(process));
    BOOL ok=CreateProcessW(exe,command,NULL,NULL,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW,NULL,cwd,&start.StartupInfo,&process);
    DWORD error=ok?0:GetLastError();
    DeleteProcThreadAttributeList(start.lpAttributeList);HeapFree(GetProcessHeap(),0,start.lpAttributeList);
    close_valid(hout);close_valid(herr);close_valid(hin);free(command);free(owned_args);
    if(!ok){
        fprintf(stderr,"FAIL: CreateProcessW Windows error=%lu\n",error);
        if(error==ERROR_ELEVATION_REQUIRED)fprintf(stderr,"The executable requests elevation. Check its compatibility setting; do not launch this Vulkan workaround as Administrator.\n");
        return 4;
    }
    CloseHandle(process.hThread);printf("Started child PID=%lu\n",process.dwProcessId);fflush(stdout);
    DWORD wait_ms=seconds ? (seconds>4294966u?4294966000u:seconds*1000u) : INFINITE;
    DWORD waited=WaitForSingleObject(process.hProcess,wait_ms);
    if(waited==WAIT_TIMEOUT){
        /* The launcher sets this option ONLY for test probes, never RimWorld. */
        fprintf(stderr,"FAIL: test probe timeout after %lu seconds\n",seconds);
        TerminateProcess(process.hProcess,124);WaitForSingleObject(process.hProcess,INFINITE);CloseHandle(process.hProcess);return 124;
    }
    if(waited!=WAIT_OBJECT_0){fprintf(stderr,"FAIL: process wait Windows error=%lu\n",GetLastError());CloseHandle(process.hProcess);return 5;}
    DWORD code=0;
    if(!GetExitCodeProcess(process.hProcess,&code)){fprintf(stderr,"FAIL: GetExitCodeProcess Windows error=%lu\n",GetLastError());CloseHandle(process.hProcess);return 5;}
    CloseHandle(process.hProcess);printf("Child exit code=%lu (0x%08lX)\n",code,code);fflush(stdout);
    return (int)code;
}
