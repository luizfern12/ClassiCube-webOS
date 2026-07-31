/* Shared declarations for the webOS port. Only compiled into webOS builds.
   Kept free of SDL types so the header can be pulled in by upstream source
   files (Platform_Posix.c, Window_SDL2.c) under #ifdef CC_BUILD_WEBOS. */
#ifdef CC_BUILD_WEBOS
#ifndef CC_WEBOS_H
#define CC_WEBOS_H

/* Platform_WebOS.c */
void WebOS_Bootstrap(int argc, char** argv);

/* Window_WebOS.c */
void WebOS_PreSDLInit(void);
void WebOS_ApplyWindowSize(int* width, int* height, int* flags);
void WebOS_LoadGamepadMappings(void);
void WebOS_ReopenGamepads(void);

#endif
#endif
