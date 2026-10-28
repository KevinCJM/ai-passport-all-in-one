// Mac side of 语音输入: presses Command-V to paste the recognised text, and its shortcut keys; never captures a microphone.
#include <ApplicationServices/ApplicationServices.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

// One key with its modifiers, down then up, from a private source so keys held on the real keyboard do not mix in.
// Arrow keys carry the flags a real keyboard gives them.
// ponytail: key codes are ANSI positions (V 9, E 14, U 32): right for QWERTY and the Chinese IMEs; Dvorak would need them looked up.
bool press(CGEventSourceRef source,CGKeyCode code,CGEventFlags flags) {
    if(code>=123&&code<=126) flags|=kCGEventFlagMaskNumericPad|kCGEventFlagMaskSecondaryFn;
    for(const bool down:{true,false}) {
        auto event=CGEventCreateKeyboardEvent(source,code,down);
        if(!event) return false;
        CGEventSetFlags(event,down?flags:0);
        CGEventPost(kCGSessionEventTap,event); CFRelease(event);
    }
    return true;
}
// "paste" (Command-V) or "key <code> <cmd|ctrl|none>".
bool passport_command(CGEventSourceRef source,const std::string& line) {
    if(line=="paste") return press(source,9,kCGEventFlagMaskCommand);
    unsigned code=0;char modifier[8]{};
    if(sscanf(line.c_str(),"key %u %7s",&code,modifier)!=2||code>127) return false;
    const CGEventFlags flags=!strcmp(modifier,"cmd")?kCGEventFlagMaskCommand:!strcmp(modifier,"ctrl")?kCGEventFlagMaskControl:0;
    return press(source,static_cast<CGKeyCode>(code),flags);
}
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    if(!strcmp(argv[1],"request_permissions")) {
        const void* keys[]={kAXTrustedCheckOptionPrompt};const void* values[]={kCFBooleanTrue};
        auto options=CFDictionaryCreate(nullptr,keys,values,1,&kCFTypeDictionaryKeyCallBacks,&kCFTypeDictionaryValueCallBacks);
        const bool trusted=AXIsProcessTrustedWithOptions(options);CFRelease(options);
        std::cout<<"{\"accessibility\":"<<(trusted?"true":"false")<<",\"post_event_access\":"<<(CGRequestPostEventAccess()?"true":"false")<<"}\n";
        return 0;
    }
    if(!strcmp(argv[1],"status")) {
        std::cout<<"{\"accessibility\":"<<(AXIsProcessTrusted()?"true":"false")
                 <<",\"post_event_access\":"<<(CGPreflightPostEventAccess()?"true":"false")<<"}\n";return 0;
    }
    // One command per line (command() above), one JSON reply each; the process keeps the granted permission warm.
    if(!strcmp(argv[1],"keyboard_daemon")) {
        if(!AXIsProcessTrusted() || !CGPreflightPostEventAccess()) { std::cout<<"{\"ok\":false,\"reason\":\"post_event_access\"}\n"<<std::flush; return 1; }
        auto source=CGEventSourceCreate(kCGEventSourceStatePrivate);
        if(!source) return 1;
        std::string command;
        while(std::getline(std::cin,command))
            std::cout<<"{\"ok\":"<<(passport_command(source,command)?"true":"false")<<"}\n"<<std::flush;
        CFRelease(source); return 0;
    }
    return 2;
}
