/*
 * WIN32S16
 * DLL for Win32s
 *
 * Copyright (c) 1997 Andreas Mohr
 */

#include "windows.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void BootTask()
{
	fprintf(stderr, "BootTask(): should only be used by WIN32S.EXE.\n");
}
