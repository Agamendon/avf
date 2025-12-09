/*++

Copyright (c) 1989-2002  Microsoft Corporation
Modified for AV Filter functionality

Module Name:

    RegistrationData.c

Abstract:

    This module contains minifilter registration data for the AVF driver.
    The actual FLT_REGISTRATION structure is defined in avf.c.

Environment:

    Kernel mode

--*/

#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>
#include "avf.h"

//
//  This file can contain additional registration-related data if needed.
//  The main FLT_REGISTRATION structure is in avf.c
//
//  This file is kept for project structure compatibility with the
//  original minispy sample.
//
